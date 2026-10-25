// reimu-kvm: the platform's screen straight from the ASPEED video engine to the
// browser, and its keyboard/mouse back into the USB HID gadget.
//
// obmc-ikvm + bmcweb (VNC pulled frame by frame through a websocket proxy) gets
// 6-8 frames/s to a browser on the AST2500; the engine itself gives ~44 at
// 1920x1080. Here each JPEG frame the engine makes is pushed as one websocket
// message over our own TLS (ChaCha20: the ARM11 has no AES instructions), and a
// client that is still busy with the previous frame skips frames instead of
// queueing them, so the picture stays current.
//
// A browser connects to wss://BMC:8443/kvm?t=TICKET; the ticket comes from the
// D-Bus method Ticket() of /xyz/openbmc_project/reimu/kvm, which a logged-in
// web page calls through bmcweb (so bmcweb does the authentication). A ticket
// is good for one connection within 30 s.
//
// Messages, server -> browser (binary):
//   1  u8 type, u16 width, u16 height, u32 sequence, JPEG bytes   a frame
// server -> browser (text): {"t":"stat",...} once a second (with "leds": the
// host's keyboard LEDs, bit 0 Num, 1 Caps, 2 Scroll Lock), {"t":"nosignal"}
// browser -> server (binary):
//   2  u8 type, u8 HID usage, u8 down        a key (usages 0xe0..0xe7: modifiers)
//   3  u8 type, u8 buttons, u16 x, u16 y     the pointer, x/y 0..32767 (LE)
//   4  u8 type                               release all keys and buttons
//   5  u8 type, u8 fps, u8 quality, u8 420   capture settings (0 keeps one)
//   7  u8 type, "host:port"                  take H.264 from reimu-h264 on the host
//                                            (empty: the host that announced itself)
//   8  u8 type                               back to the engine's JPEG
//  11  u8 type, u8 on                         the host's sound while on the engine's JPEG
//                                            (an audio-only stream from reimu-h264)
//   9  u8 type, i8 wheel, i8 pan             scroll (the mouse report has a wheel and pan
//                                            byte when the gadget was made with them;
//                                            a 5-byte gadget just drops them)
// reimu-h264 announces itself every few seconds with a UDP datagram
// "REIMU-H264 <port>" to this port: the BMC learns the host's address from it
// (DHCP or not) and the stats carry it as "host" while it keeps coming.
// And for H.264 (from the host's GPU encoder, while its OS runs), server -> browser:
//   {"t":"h264","codec":...,"w":..,"h":..}  then  6  u8 type, u8 key frame, access unit
//   (u8 2 instead of the key flag: an Opus packet of the host's sound, 48 kHz stereo)
//   {"t":"h264off","reason":...}            the host stream ended: JPEG again
#define _GNU_SOURCE
#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <getopt.h>
#include <linux/videodev2.h>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <openssl/err.h>
#include <openssl/evp.h>
#include <openssl/rand.h>
#include <openssl/sha.h>
#include <openssl/ssl.h>
#include <signal.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/epoll.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <systemd/sd-bus.h>
#include <time.h>
#include <unistd.h>

#define MAX_CLIENTS 4
#define NBUF 3
#define TICKETS 8
#define TICKET_TTL 30.0

static const char *video_path = "/dev/video0", *kbd_path = "/dev/hidg0", *ptr_path = "/dev/hidg1";
static const char *cert_path = "/etc/ssl/certs/https/server.pem";
static int port = 8443, want_fps = 60, quality = 0, sub420 = 1, verbose;

static int ep;
static void logf_(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
    fputc('\n', stderr);
}
#define LOG(...) logf_(__VA_ARGS__)
#define DBG(...) do { if (verbose) logf_(__VA_ARGS__); } while (0)

static double now(void)
{
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return t.tv_sec + t.tv_nsec / 1e9;
}

// ------------------------------------------------------------------ tickets
static struct { char id[33]; double until; } tickets[TICKETS];

static const char *ticket_new(void)
{
    int slot = 0;
    for (int i = 0; i < TICKETS; i++)
        if (tickets[i].until < tickets[slot].until) slot = i;
    unsigned char r[16];
    RAND_bytes(r, sizeof r);
    for (int i = 0; i < 16; i++) sprintf(tickets[slot].id + 2 * i, "%02x", r[i]);
    tickets[slot].until = now() + TICKET_TTL;
    return tickets[slot].id;
}

static int ticket_take(const char *id)
{
    double t = now();
    for (int i = 0; i < TICKETS; i++)
        if (tickets[i].until > t && strlen(id) == 32 && !CRYPTO_memcmp(tickets[i].id, id, 32)) {
            tickets[i].until = 0;
            return 1;
        }
    return 0;
}

// ------------------------------------------------------------------ HID
// f_hid has one report in flight per device: a write() while the host hasn't
// fetched the previous report fails with EAGAIN (the fds are non-blocking, the
// loop never waits on the host). So every report goes into its device's queue
// and is written when the device takes it (EPOLLOUT): no press, release or
// click is lost. Pointer moves with the same buttons are merged into the
// queued move (the newest position), wheel steps added up. A device that takes
// nothing for HID_STALL seconds (host off, suspended, gone): its queue is
// dropped and everything released; that report goes out once it reads again.
// A write error other than EAGAIN (ESHUTDOWN: unbound/disconnected, ENODEV,
// ENXIO...) closes the fd: the next report opens the device again (a UDC
// rebind makes a new function instance behind the same /dev/hidgN).
#define HID_QLEN 512
#define HID_STALL 2.0
#define TAG_KBD 0xfffffff4u
#define TAG_PTR 0xfffffff5u
// GADGET_HID_WRITE_GET_REPORT (6.18 f_hid, linux/usb/g_hid.h; older headers
// lack it): the report a host's GET_REPORT gets at once; without one f_hid
// waits 2.5 s for userspace and then answers zeros
struct hidg_get_report { uint8_t report_id, userspace_req; uint16_t length; uint8_t data[64], padding[4]; };
#define HIDG_WRITE_GET_REPORT _IOW('g', 0x42, struct hidg_get_report)

struct hiddev {
    const char **path;
    int fd;
    uint32_t tag;
    size_t len;              // report bytes (f_hid cuts a write to report_length)
    uint8_t q[HID_QLEN][8];  // queued reports, oldest at head
    uint8_t mergeable[HID_QLEN]; // a move/scroll a later one may merge into
    int head, n;
    uint8_t last[8];         // the last report the host took
    double stuck;            // since when the head has waited (0: not waiting)
    int polling, gaveup;
    long written, merged, dropped;
};
static struct hiddev kbd = { .path = &kbd_path, .fd = -1, .tag = TAG_KBD, .len = 8 };
// buttons, x (LE), y (LE), wheel, pan: 7 bytes (a 5-byte stock mouse drops the last two)
static struct hiddev ptr = { .path = &ptr_path, .fd = -1, .tag = TAG_PTR, .len = 7 };
static uint8_t mods, down[16], ptr_rep[7];
static int ndown;
static uint8_t leds; // the host's keyboard LEDs (bit 0 Num, 1 Caps, 2 Scroll Lock)
// whether the host has the keyboard function enabled (configured it since the
// gadget was bound): f_hid answers a read with ESHUTDOWN once the host's
// configuration went (unbind, disconnect) until the host sets it again; the
// UDC's sysfs "state" can't tell (it stays "configured" across a rebind).
// Since when it has been off (0: on or unknown): Status "usboff" for
// reimu-hid-watch
static double usb_off_since;

static void hid_epoll(struct hiddev *d, int out)
{
    struct epoll_event ev = { .events = EPOLLIN | (out ? EPOLLOUT : 0), .data.u32 = d->tag };
    epoll_ctl(ep, EPOLL_CTL_MOD, d->fd, &ev);
    d->polling = out;
}

static void hid_get_report(struct hiddev *d, const uint8_t *rep)
{
    struct hidg_get_report r = { .length = d->len };
    memcpy(r.data, rep, d->len);
    ioctl(d->fd, HIDG_WRITE_GET_REPORT, &r); // ENOTTY before 6.x: harmless
}

static void hid_read(struct hiddev *d);

static int hid_open(struct hiddev *d)
{
    if (d->fd >= 0) return 0;
    d->fd = open(*d->path, O_RDWR | O_CLOEXEC | O_NONBLOCK);
    if (d->fd < 0) return -1;
    struct epoll_event ev = { .events = EPOLLIN, .data.u32 = d->tag };
    epoll_ctl(ep, EPOLL_CTL_ADD, d->fd, &ev);
    d->polling = 0;
    // what the host holds is unknown here (reimu-kvm started again after a
    // crash, the device opened again after an error): a release goes first,
    // whatever `last` says, so a key or button held before can't stay held.
    // The pointer is released where it last was (0,0 at start-up)
    uint8_t r[8] = { 0 };
    if (d == &ptr) memcpy(r + 1, ptr_rep + 1, 4);
    if (d->n == HID_QLEN) d->n--;
    d->head = (d->head - 1 + HID_QLEN) % HID_QLEN;
    memcpy(d->q[d->head], r, d->len);
    d->mergeable[d->head] = 0;
    d->n++;
    memset(d->last, 0xff, sizeof d->last); // matches no report: nothing deduplicated against it
    hid_get_report(d, r);
    if (d == &kbd) hid_read(d); // enabled by the host or not (see usb_off_since)
    return d->fd >= 0 ? 0 : -1;
}

static void hid_close(struct hiddev *d)
{
    if (d->fd < 0) return;
    epoll_ctl(ep, EPOLL_CTL_DEL, d->fd, NULL);
    close(d->fd);
    d->fd = -1;
    d->polling = 0;
}

// write the queue while the device takes it
static void hid_flush(struct hiddev *d)
{
    while (d->n) {
        if (hid_open(d) < 0) { d->dropped += d->n; d->n = 0; break; } // no device to deliver to
        uint8_t *r = d->q[d->head];
        ssize_t w = write(d->fd, r, d->len);
        if (w == (ssize_t)d->len) {
            memcpy(d->last, r, d->len);
            hid_get_report(d, r);
            d->head = (d->head + 1) % HID_QLEN;
            d->n--;
            d->written++;
            d->stuck = 0;
            d->gaveup = 0;
            if (d == &kbd) usb_off_since = 0;
            continue;
        }
        if (w < 0 && (errno == EAGAIN || errno == EINTR)) {
            if (!d->stuck) d->stuck = now();
            if (!d->polling) hid_epoll(d, 1);
            return;
        }
        DBG("%s: %s, reopened at the next report", *d->path, w < 0 ? strerror(errno) : "short write");
        hid_close(d);
        d->dropped += d->n;
        d->n = 0;
        break;
    }
    d->stuck = 0;
    if (d->fd >= 0 && d->polling) hid_epoll(d, 0);
}

// queue a report (mergeable: a pointer move or scroll with unchanged buttons)
static void hid_push(struct hiddev *d, const uint8_t *rep, int mergeable)
{
    int t = (d->head + d->n - 1 + HID_QLEN) % HID_QLEN;
    uint8_t *tail = d->n ? d->q[t] : d->last;
    int rel = d == &ptr && (rep[5] || rep[6]);
    if (!rel && !memcmp(tail, rep, d->len)) return; // nothing new for the host
    if (d->n && mergeable && d->mergeable[t] && tail[0] == rep[0]) {
        int w = (int8_t)tail[5] + (int8_t)rep[5], p = (int8_t)tail[6] + (int8_t)rep[6];
        if (w >= -127 && w <= 127 && p >= -127 && p <= 127) {
            memcpy(tail + 1, rep + 1, 4);
            tail[5] = (uint8_t)w, tail[6] = (uint8_t)p;
            d->merged++;
            hid_flush(d);
            return;
        }
    }
    if (d->n == HID_QLEN) {
        // full (a host that reads slower than the browser sends for long):
        // the newest state in place of the last queued one
        memcpy(d->q[t], rep, d->len);
        d->mergeable[t] = 0;
        d->dropped++;
    } else {
        int i = (d->head + d->n) % HID_QLEN;
        memcpy(d->q[i], rep, d->len);
        d->mergeable[i] = mergeable;
        d->n++;
    }
    hid_flush(d);
}

// the device's OUT reports: the keyboard's LEDs (SET_REPORT, 1 byte)
static void hid_read(struct hiddev *d)
{
    uint8_t b[64];
    for (;;) {
        ssize_t n = read(d->fd, b, sizeof b);
        if (n > 0) {
            if (d == &kbd) leds = b[0], usb_off_since = 0;
            continue;
        }
        if (n < 0 && (errno == EAGAIN || errno == EINTR)) {
            if (d == &kbd) usb_off_since = 0;
            return;
        }
        // ESHUTDOWN: the host's configuration is gone (unbound, disconnected)
        if (d == &kbd && !usb_off_since) usb_off_since = now();
        hid_close(d);
        return;
    }
}

static void hid_io(struct hiddev *d, uint32_t events)
{
    if (d->fd < 0) return;
    if (events & EPOLLIN) hid_read(d);
    if (d->fd >= 0 && (events & EPOLLOUT)) hid_flush(d);
}

// more keys down than the report's 6 slots: ErrorRollOver (usage 1) in every
// slot, as a real keyboard does; the host keeps its state until fewer are down
static void kbd_report(uint8_t *r)
{
    memset(r, 0, 8);
    r[0] = mods;
    if (ndown > 6) memset(r + 2, 0x01, 6);
    else memcpy(r + 2, down, ndown);
}

static void key_event(uint8_t usage, int dn)
{
    if (usage >= 0xe0 && usage <= 0xe7) {
        uint8_t bit = 1 << (usage - 0xe0);
        mods = dn ? mods | bit : mods & ~bit;
    } else if (usage) {
        int at = -1;
        for (int i = 0; i < ndown; i++)
            if (down[i] == usage) at = i;
        if (dn && at < 0 && ndown < (int)sizeof down) down[ndown++] = usage;
        if (!dn && at >= 0) {
            memmove(down + at, down + at + 1, ndown - at - 1);
            ndown--;
        }
    }
    uint8_t r[8];
    kbd_report(r);
    hid_push(&kbd, r, 0);
}

static void ptr_event(uint8_t buttons, uint16_t x, uint16_t y)
{
    // browser: bit0 left, bit1 middle, bit2 right; HID: bit0 left, bit1 right, bit2 middle
    uint8_t b = (buttons & 1) | ((buttons & 4) >> 1) | ((buttons & 2) << 1);
    int moved_only = b == ptr_rep[0];
    ptr_rep[0] = b;
    if (x > 32767) x = 32767;
    if (y > 32767) y = 32767;
    ptr_rep[1] = x & 0xff, ptr_rep[2] = x >> 8, ptr_rep[3] = y & 0xff, ptr_rep[4] = y >> 8;
    ptr_rep[5] = ptr_rep[6] = 0;
    hid_push(&ptr, ptr_rep, moved_only);
}

static void wheel_event(int8_t wheel, int8_t pan)
{
    // relative: in this report only, the next ones carry 0 again
    uint8_t r[7];
    memcpy(r, ptr_rep, 5);
    r[5] = (uint8_t)wheel, r[6] = (uint8_t)pan;
    hid_push(&ptr, r, 1);
}

static void release_all(void)
{
    mods = 0;
    ndown = 0;
    uint8_t r[8];
    kbd_report(r);
    hid_push(&kbd, r, 0);
    ptr_rep[0] = ptr_rep[5] = ptr_rep[6] = 0;
    hid_push(&ptr, ptr_rep, 0);
}

// a device that took nothing for HID_STALL: its queue dropped, all released
static void hid_check_stall(struct hiddev *d, double t)
{
    if (!d->n || !d->stuck || t - d->stuck < HID_STALL) return;
    if (!d->gaveup) LOG("%s: the host took no report for %.0f s: %d dropped, all released", *d->path, HID_STALL, d->n);
    d->gaveup = 1;
    d->dropped += d->n;
    d->n = 0;
    d->stuck = 0;
    if (d == &kbd) mods = 0, ndown = 0;
    else ptr_rep[0] = ptr_rep[5] = ptr_rep[6] = 0;
    uint8_t r[8] = { 0 };
    if (d == &ptr) memcpy(r, ptr_rep, 7);
    if (memcmp(r, d->last, d->len)) {
        memcpy(d->q[d->head], r, d->len);
        d->mergeable[d->head] = 0;
        d->n = 1;
    }
    hid_flush(d);
}

// SIGTERM etc.: release everything before going (a stopped reimu-kvm must not
// leave a key held on the host), up to 0.3 s for the host to take it
static volatile sig_atomic_t quit;
static void on_signal(int s) { quit = s; }
static void hid_goodbye(void)
{
    release_all();
    double end = now() + 0.3;
    while ((kbd.n || ptr.n) && now() < end) {
        struct epoll_event evs[4];
        int n = epoll_wait(ep, evs, 4, 20);
        for (int i = 0; i < n; i++)
            if (evs[i].data.u32 == TAG_KBD) hid_io(&kbd, evs[i].events);
            else if (evs[i].data.u32 == TAG_PTR) hid_io(&ptr, evs[i].events);
        hid_flush(&kbd), hid_flush(&ptr);
    }
}

// ------------------------------------------------------------------ video
static int vfd = -1, streaming, signal_ok = 1, fresh_viewer;
// a still picture is captured at IDLE_FPS (the engine then costs a third of
// the CPU); a changed frame or any key/pointer input goes back to want_fps
#define IDLE_FPS 15
#define IDLE_AFTER 1.0
static int cur_fps;
static double last_change;
static unsigned vw, vh;
static struct { void *p; size_t len; } vbuf[NBUF];
static uint32_t seq;
static double last_frame, last_try, last_sent;
static long n_frames, n_bytes, n_same;
// the engine compresses every frame, changed or not: a frame equal to the
// previous one is not sent (one in a second still is, for a client that
// connected in between or lost one)
// The engine's buffers are mapped uncached: each frame is read from them once,
// with one burst copy into cur, and compared and sent from there.
static unsigned char *prev, *cur;
static size_t prev_len, buf_cap;

static int same_bytes(const unsigned char *a, const unsigned char *b, size_t n)
{
    // both buffers come from malloc (8-byte aligned) and start at the same offset
    while (n && ((uintptr_t)a & 3)) { if (*a++ != *b++) return 0; n--; }
    const uint32_t *x = (const uint32_t *)a, *y = (const uint32_t *)b;
    for (; n >= 16; n -= 16, x += 4, y += 4)
        if ((x[0] ^ y[0]) | (x[1] ^ y[1]) | (x[2] ^ y[2]) | (x[3] ^ y[3])) return 0;
    a = (const unsigned char *)x, b = (const unsigned char *)y;
    while (n--) if (*a++ != *b++) return 0;
    return 1;
}

// where the picture data starts: past the comment segment (FF FE) the
// engine puts near the start of each frame, which holds a frame counter
static size_t jpeg_skip(const unsigned char *p, size_t len)
{
    for (size_t i = 2; i + 4 < len && i < 128;) {
        if (p[i] != 0xff) break;
        size_t seg = 2 + (p[i + 2] << 8 | p[i + 3]);
        if (p[i + 1] == 0xfe) return i + seg;
        if (p[i + 1] == 0xda) break;
        i += seg;
    }
    return 0;
}

static void video_ctl(unsigned id, int v)
{
    struct v4l2_control c = { .id = id, .value = v };
    if (ioctl(vfd, VIDIOC_S_CTRL, &c)) DBG("ctrl %x=%d: %s", id, v, strerror(errno));
}

static void set_rate(int fps)
{
    if (vfd < 0 || fps == cur_fps) return;
    struct v4l2_streamparm sp = { .type = V4L2_BUF_TYPE_VIDEO_CAPTURE };
    sp.parm.capture.timeperframe.numerator = 1;
    sp.parm.capture.timeperframe.denominator = fps;
    if (!ioctl(vfd, VIDIOC_S_PARM, &sp)) cur_fps = fps;
    DBG("capture rate %d", fps);
}

static void activity(void)
{
    last_change = now();
    set_rate(want_fps);
}

static void video_stop(void)
{
    if (vfd < 0) return;
    if (streaming) {
        int type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        ioctl(vfd, VIDIOC_STREAMOFF, &type);
    }
    for (int i = 0; i < NBUF; i++)
        if (vbuf[i].p) munmap(vbuf[i].p, vbuf[i].len), vbuf[i].p = NULL;
    epoll_ctl(ep, EPOLL_CTL_DEL, vfd, NULL);
    close(vfd);
    vfd = -1;
    streaming = 0;
}

// 0 streaming, -1 failed (no signal, busy...); err gets a reason
static int video_start(const char **err)
{
    video_stop();
    last_try = now();
    vfd = open(video_path, O_RDWR | O_CLOEXEC | O_NONBLOCK);
    if (vfd < 0) { *err = "no video device"; return -1; }
    struct v4l2_dv_timings tm = { 0 };
    if (ioctl(vfd, VIDIOC_QUERY_DV_TIMINGS, &tm) || !tm.bt.width) {
        *err = "nosignal";
        signal_ok = 0;
        video_stop();
        return -1;
    }
    ioctl(vfd, VIDIOC_S_DV_TIMINGS, &tm);
    video_ctl(V4L2_CID_JPEG_COMPRESSION_QUALITY, quality);
    video_ctl(V4L2_CID_JPEG_CHROMA_SUBSAMPLING, sub420 ? V4L2_JPEG_CHROMA_SUBSAMPLING_420 : V4L2_JPEG_CHROMA_SUBSAMPLING_444);
    struct v4l2_format f = { .type = V4L2_BUF_TYPE_VIDEO_CAPTURE };
    ioctl(vfd, VIDIOC_G_FMT, &f);
    vw = f.fmt.pix.width, vh = f.fmt.pix.height;
    struct v4l2_streamparm sp = { .type = V4L2_BUF_TYPE_VIDEO_CAPTURE };
    sp.parm.capture.timeperframe.numerator = 1;
    sp.parm.capture.timeperframe.denominator = want_fps;
    ioctl(vfd, VIDIOC_S_PARM, &sp);
    cur_fps = want_fps;
    last_change = now();
    struct v4l2_requestbuffers rb = { .count = NBUF, .type = V4L2_BUF_TYPE_VIDEO_CAPTURE, .memory = V4L2_MEMORY_MMAP };
    if (ioctl(vfd, VIDIOC_REQBUFS, &rb)) {
        *err = errno == EBUSY ? "video busy (a VNC client is connected)" : "video REQBUFS failed";
        video_stop();
        return -1;
    }
    for (unsigned i = 0; i < rb.count && i < NBUF; i++) {
        struct v4l2_buffer b = { .type = rb.type, .memory = rb.memory, .index = i };
        ioctl(vfd, VIDIOC_QUERYBUF, &b);
        vbuf[i].len = b.length;
        vbuf[i].p = mmap(NULL, b.length, PROT_READ, MAP_SHARED, vfd, b.m.offset);
        if (vbuf[i].p == MAP_FAILED) vbuf[i].p = NULL;
        ioctl(vfd, VIDIOC_QBUF, &b);
    }
    struct v4l2_event_subscription sub = { .type = V4L2_EVENT_SOURCE_CHANGE };
    ioctl(vfd, VIDIOC_SUBSCRIBE_EVENT, &sub);
    int type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    if (ioctl(vfd, VIDIOC_STREAMON, &type)) { *err = "STREAMON failed"; video_stop(); return -1; }
    streaming = 1;
    signal_ok = 1;
    last_frame = now();
    struct epoll_event ev = { .events = EPOLLIN | EPOLLPRI, .data.u32 = 0xfffffff0 };
    epoll_ctl(ep, EPOLL_CTL_ADD, vfd, &ev);
    LOG("video %ux%u, %d fps asked, quality %d, %s", vw, vh, want_fps, quality, sub420 ? "4:2:0" : "4:4:4");
    return 0;
}

// ------------------------------------------------------------------ clients
enum { C_TLS, C_HTTP, C_WS };
struct upstream;
struct client {
    int kind; // K_CLIENT; epoll data points at a client or an upstream
    int fd;
    SSL *ssl;
    int state;
    unsigned char in[4096];
    size_t inlen;
    unsigned char *out;
    size_t outcap, outlen, outoff;
    int want_out;
    long frames, skipped;
    struct upstream *up; // H.264 from the host instead of JPEG, or its sound only
    int want_audio;      // the viewer wants the host's sound
    double audio_retry;  // no new audio-only attempt before this
};
enum { K_CLIENT = 1, K_UP };
struct upstream {
    int kind; // K_UP
    int fd, connected, need_key, have_hdr;
    int audio_only; // the host's sound next to the engine's JPEG
    struct client *c;
    unsigned char *buf;
    size_t len, cap;
    long frames, dropped;
};
static struct client *clients[MAX_CLIENTS];
static SSL_CTX *ctx;

// objects closed while handling a batch of epoll events are freed after it:
// a later event of the batch may still point at them (kind is 0 by then)
static void *grave[4 * MAX_CLIENTS + 4];
static int ngrave;
static void bury(void *p, int *kind)
{
    *kind = 0;
    if (ngrave < (int)(sizeof grave / sizeof grave[0])) grave[ngrave++] = p;
}

static int ws_clients(void)
{
    int n = 0;
    for (int i = 0; i < MAX_CLIENTS; i++)
        if (clients[i] && clients[i]->state == C_WS) n++;
    return n;
}

// a viewer of the engine's JPEG (perhaps with the host's sound)
static int on_jpeg(const struct client *c)
{
    return c && c->state == C_WS && (!c->up || c->up->audio_only);
}

static int jpeg_clients(void)
{
    int n = 0;
    for (int i = 0; i < MAX_CLIENTS; i++)
        if (on_jpeg(clients[i])) n++;
    return n;
}

static void up_close(struct client *c, const char *why);

static void client_close(struct client *c)
{
    int was_ws = c->state == C_WS;
    c->state = -2; // closing: up_close must not restart the capture for it
    if (c->up) up_close(c, NULL);
    for (int i = 0; i < MAX_CLIENTS; i++)
        if (clients[i] == c) clients[i] = NULL;
    epoll_ctl(ep, EPOLL_CTL_DEL, c->fd, NULL);
    if (c->ssl) SSL_free(c->ssl);
    close(c->fd);
    free(c->out);
    c->out = NULL;
    bury(c, &c->kind);
    // nothing a viewer held stays held: every viewer that goes (not only the
    // last) lets go of all keys and buttons
    if (was_ws) release_all();
    if (was_ws && !jpeg_clients() && streaming) {
        video_stop();
        LOG("no JPEG viewer left, capture stopped");
    }
}

static void client_poll(struct client *c, int out)
{
    struct epoll_event ev = { .events = EPOLLIN | (out ? EPOLLOUT : 0), .data.ptr = c };
    (void)0;
    if (c->want_out != out) epoll_ctl(ep, EPOLL_CTL_MOD, c->fd, &ev);
    c->want_out = out;
}

// append raw bytes to the client's output; 0 if it does not fit
static int out_put(struct client *c, const void *p, size_t n)
{
    if (c->outoff && c->outoff == c->outlen) c->outoff = c->outlen = 0;
    if (c->outlen + n > c->outcap) {
        size_t cap = c->outcap ? c->outcap : 65536;
        while (cap < c->outlen + n) cap *= 2;
        if (cap > (4u << 20)) return 0;
        unsigned char *o = realloc(c->out, cap);
        if (!o) return 0;
        c->out = o, c->outcap = cap;
    }
    memcpy(c->out + c->outlen, p, n);
    c->outlen += n;
    return 1;
}

static int flush(struct client *c)
{
    while (c->outoff < c->outlen) {
        int n = SSL_write(c->ssl, c->out + c->outoff, (int)(c->outlen - c->outoff));
        if (n > 0) { c->outoff += n; continue; }
        int e = SSL_get_error(c->ssl, n);
        if (e == SSL_ERROR_WANT_WRITE || e == SSL_ERROR_WANT_READ) { client_poll(c, 1); return 0; }
        return -1;
    }
    c->outoff = c->outlen = 0;
    client_poll(c, 0);
    return 0;
}

static void ws_header(struct client *c, int op, size_t len)
{
    unsigned char h[10];
    size_t n = 2;
    h[0] = 0x80 | op;
    if (len < 126) h[1] = len;
    else if (len < 65536) h[1] = 126, h[2] = len >> 8, h[3] = len, n = 4;
    else {
        h[1] = 127;
        memset(h + 2, 0, 4);
        h[6] = len >> 24, h[7] = len >> 16, h[8] = len >> 8, h[9] = len, n = 10;
    }
    out_put(c, h, n);
}

static int ws_text(struct client *c, const char *fmt, ...)
{
    char s[512];
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(s, sizeof s, fmt, ap);
    va_end(ap);
    ws_header(c, 1, n);
    out_put(c, s, n);
    if (flush(c) < 0) {
        client_close(c);
        return -1;
    }
    return 0;
}

static void broadcast_text(const char *fmt, const char *a)
{
    for (int i = 0; i < MAX_CLIENTS; i++)
        if (clients[i] && clients[i]->state == C_WS) ws_text(clients[i], fmt, a);
}

static void http_reply(struct client *c, const char *status, const char *body)
{
    char h[256];
    int n = snprintf(h, sizeof h, "HTTP/1.1 %s\r\nContent-Type: text/html; charset=utf-8\r\nContent-Length: %zu\r\nConnection: close\r\nCache-Control: no-store\r\n\r\n", status, strlen(body));
    out_put(c, h, n);
    out_put(c, body, strlen(body));
    flush(c);
}

static const char *hdr(const char *req, const char *name, char *val, size_t cap)
{
    size_t nl = strlen(name);
    for (const char *p = strstr(req, "\r\n"); p && p[2]; p = strstr(p + 2, "\r\n")) {
        const char *l = p + 2;
        if (!strncasecmp(l, name, nl) && l[nl] == ':') {
            l += nl + 1;
            while (*l == ' ') l++;
            size_t n = strcspn(l, "\r\n");
            if (n >= cap) n = cap - 1;
            memcpy(val, l, n);
            val[n] = 0;
            return val;
        }
    }
    return NULL;
}

static void handle_http(struct client *c)
{
    char *req = (char *)c->in;
    char *end = strstr(req, "\r\n\r\n");
    if (!end) {
        if (c->inlen >= sizeof c->in - 1) client_close(c);
        return;
    }
    char key[128], up[64], path[256] = "";
    sscanf(req, "GET %255s", path);
    if (!hdr(req, "Upgrade", up, sizeof up) || strcasecmp(up, "websocket")) {
        // a plain visit: the page that lets the browser accept this port's certificate
        http_reply(c, "200 OK", "<!doctype html><meta charset=utf-8><title>REIMU KVM</title>"
                                "<p>Сертификат для прямого видеопотока KVM принят. Эту вкладку можно закрыть.</p>"
                                "<script>try{window.opener&&window.opener.postMessage('reimu-kvm-cert','*')}catch(e){}setTimeout(()=>window.close(),800)</script>");
        c->state = -1;
        return;
    }
    const char *t = strstr(path, "t=");
    char tk[40] = "";
    if (t) sscanf(t + 2, "%39[0-9a-f]", tk);
    if (strncmp(path, "/kvm", 4) || !ticket_take(tk) || !hdr(req, "Sec-WebSocket-Key", key, sizeof key)) {
        http_reply(c, "403 Forbidden", "forbidden");
        c->state = -1;
        return;
    }
    if (ws_clients() >= MAX_CLIENTS - 1) {
        http_reply(c, "503 Service Unavailable", "too many viewers");
        c->state = -1;
        return;
    }
    char cat[200];
    unsigned char sha[20];
    char acc[64];
    snprintf(cat, sizeof cat, "%s258EAFA5-E914-47DA-95CA-C5AB0DC85B11", key);
    SHA1((unsigned char *)cat, strlen(cat), sha);
    EVP_EncodeBlock((unsigned char *)acc, sha, 20);
    char resp[256];
    int n = snprintf(resp, sizeof resp, "HTTP/1.1 101 Switching Protocols\r\nUpgrade: websocket\r\nConnection: Upgrade\r\nSec-WebSocket-Accept: %s\r\n\r\n", acc);
    out_put(c, resp, n);
    size_t used = end + 4 - req;
    memmove(c->in, c->in + used, c->inlen - used);
    c->inlen -= used;
    c->state = C_WS;
    if (flush(c) < 0) { client_close(c); return; }
    LOG("viewer connected (%d)", ws_clients());
    // a new viewer starts from nothing held (another one's keys included)
    release_all();
    fresh_viewer = 1;
    if (!streaming) {
        const char *err = NULL;
        if (video_start(&err)) ws_text(c, "{\"t\":\"%s\"}", !strcmp(err, "nosignal") ? "nosignal" : err);
    }
}

static void apply_settings(int fps, int q, int s)
{
    int changed = 0;
    if (fps > 0 && fps <= 60 && fps != want_fps) want_fps = fps, changed = 1;
    if (q > 0 && q - 1 <= 11 && q - 1 != quality) quality = q - 1, changed = 1;
    if (s > 0 && (s - 1) != sub420) sub420 = s - 1, changed = 1;
    if (changed && streaming) {
        const char *err;
        video_start(&err);
    }
}

// ------------------------------------------------------------------ H.264 from the host
static void up_close(struct client *c, const char *why)
{
    struct upstream *u = c->up;
    if (!u) return;
    epoll_ctl(ep, EPOLL_CTL_DEL, u->fd, NULL);
    close(u->fd);
    LOG("host %s closed (%ld frames, %ld dropped)%s%s", u->audio_only ? "sound" : "stream", u->frames, u->dropped,
        why ? ": " : "", why ? why : "");
    int audio_only = u->audio_only;
    free(u->buf);
    bury(u, &u->kind);
    c->up = NULL;
    if (why && c->kind == K_CLIENT)
        ws_text(c, "{\"t\":\"%s\",\"reason\":\"%s\"}", audio_only ? "audiooff" : "h264off", why);
    if (audio_only) { c->audio_retry = now() + 10; return; }
    // this viewer is back on JPEG: capture right away, not at the next retry
    if (c->kind == K_CLIENT && c->state == C_WS && !streaming) {
        const char *err = NULL;
        video_start(&err);
    }
}

// the host's reimu-h264 as announced over UDP
static char announced[64];
static double announced_at;
#define ANNOUNCE_TTL 15.0

static const char *announced_host(void)
{
    return announced[0] && now() - announced_at < ANNOUNCE_TTL ? announced : "";
}

static void announce_read(int ufd)
{
    char buf[64];
    struct sockaddr_storage sa;
    socklen_t sl = sizeof sa;
    ssize_t n;
    while ((n = recvfrom(ufd, buf, sizeof buf - 1, 0, (struct sockaddr *)&sa, &sl)) > 0) {
        buf[n] = 0;
        int hport;
        char ip[INET6_ADDRSTRLEN];
        if (sscanf(buf, "REIMU-H264 %d", &hport) != 1 || hport < 1 || hport > 65535) continue;
        if (sa.ss_family == AF_INET6) {
            struct sockaddr_in6 *a6 = (struct sockaddr_in6 *)&sa;
            if (IN6_IS_ADDR_V4MAPPED(&a6->sin6_addr)) inet_ntop(AF_INET, &a6->sin6_addr.s6_addr[12], ip, sizeof ip);
            else inet_ntop(AF_INET6, &a6->sin6_addr, ip, sizeof ip);
        } else inet_ntop(AF_INET, &((struct sockaddr_in *)&sa)->sin_addr, ip, sizeof ip);
        char was[64];
        snprintf(was, sizeof was, "%s", announced_host());
        snprintf(announced, sizeof announced, strchr(ip, ':') ? "[%s]:%d" : "%s:%d", ip, hport);
        announced_at = now();
        if (strcmp(was, announced)) LOG("host announced: %s", announced);
        sl = sizeof sa;
    }
}

static void up_open(struct client *c, const char *addr, int audio_only)
{
    char host[128], port[16] = "8555";
    const char *off = audio_only ? "audiooff" : "h264off";
    if (!*addr) addr = announced_host();
    if (!*addr) { ws_text(c, "{\"t\":\"%s\",\"reason\":\"%s\"}", off, "no host announced"); return; }
    const char *colon = strrchr(addr, ':');
    if (colon && !strchr(colon, ']')) {
        snprintf(host, sizeof host, "%.*s", (int)(colon - addr), addr);
        snprintf(port, sizeof port, "%s", colon + 1);
    } else snprintf(host, sizeof host, "%s", addr);
    if (host[0] == '[') { memmove(host, host + 1, strlen(host)); host[strcspn(host, "]")] = 0; }
    if (c->up) up_close(c, NULL);
    struct addrinfo hints = { .ai_socktype = SOCK_STREAM }, *ai = NULL;
    if (getaddrinfo(host, port, &hints, &ai) || !ai) { ws_text(c, "{\"t\":\"%s\",\"reason\":\"%s\"}", off, "address not found"); return; }
    int fd = socket(ai->ai_family, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
    int r = fd < 0 ? -1 : connect(fd, ai->ai_addr, ai->ai_addrlen);
    freeaddrinfo(ai);
    if (fd < 0 || (r && errno != EINPROGRESS)) {
        if (fd >= 0) close(fd);
        ws_text(c, "{\"t\":\"%s\",\"reason\":\"%s\"}", off, "connect failed");
        return;
    }
    int one = 1, rb = 1 << 20;
    setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof one);
    setsockopt(fd, SOL_SOCKET, SO_RCVBUF, &rb, sizeof rb);
    struct upstream *u = calloc(1, sizeof *u);
    u->kind = K_UP, u->fd = fd, u->c = c, u->need_key = 1, u->audio_only = audio_only;
    c->up = u;
    struct epoll_event ev = { .events = EPOLLIN | EPOLLOUT, .data.ptr = u };
    epoll_ctl(ep, EPOLL_CTL_ADD, fd, &ev);
    LOG("host %s: connecting to %s:%s", audio_only ? "sound" : "stream", host, port);
    if (audio_only) return;
    // the JPEG capture may not be needed any more
    if (!jpeg_clients() && streaming) video_stop();
}

static void up_io(struct upstream *u, uint32_t events)
{
    struct client *c = u->c;
    if (!u->connected) {
        int err = 0;
        socklen_t l = sizeof err;
        getsockopt(u->fd, SOL_SOCKET, SO_ERROR, &err, &l);
        if (err) { up_close(c, strerror(err)); return; }
        u->connected = 1;
        // the request: "AUDIO" for the sound alone (anything else, or nothing
        // within a moment, is the picture with its sound)
        const char *req = u->audio_only ? "AUDIO\n" : "VIDEO\n";
        if (write(u->fd, req, strlen(req)) < 0) { up_close(c, strerror(errno)); return; }
        struct epoll_event ev = { .events = EPOLLIN, .data.ptr = u };
        epoll_ctl(ep, EPOLL_CTL_MOD, u->fd, &ev);
        if (!(events & EPOLLIN)) return;
    }
    for (;;) {
        if (u->cap - u->len < 65536) {
            size_t cap = u->cap ? u->cap * 2 : 262144;
            if (cap > (8u << 20)) { up_close(c, "frame too big"); return; }
            unsigned char *b = realloc(u->buf, cap);
            if (!b) { up_close(c, "out of memory"); return; }
            u->buf = b, u->cap = cap;
        }
        ssize_t n = read(u->fd, u->buf + u->len, u->cap - u->len);
        if (n == 0) { up_close(c, "the host closed the stream"); return; }
        if (n < 0) {
            if (errno == EAGAIN) break;
            up_close(c, strerror(errno));
            return;
        }
        u->len += n;
        size_t off = 0;
        if (!u->have_hdr) {
            unsigned char *nl = memchr(u->buf, '\n', u->len);
            if (!nl) { if (u->len > 1024) { up_close(c, "bad header"); return; } continue; }
            *nl = 0;
            // forward the host's JSON header with a type tag
            char msg[600];
            snprintf(msg, sizeof msg, "{\"t\":\"%s\",%s", u->audio_only ? "audio" : "h264", (char *)u->buf + 1);
            if (ws_text(c, "%s", msg) < 0) return;
            u->have_hdr = 1;
            off = nl + 1 - u->buf;
        }
        while (u->len - off >= 5) {
            uint32_t len = (uint32_t)u->buf[off] << 24 | u->buf[off + 1] << 16 | u->buf[off + 2] << 8 | u->buf[off + 3];
            int key = u->buf[off + 4];
            if (len > (4u << 20)) { up_close(c, "bad frame"); return; }
            if (u->len - off < 5 + len) break;
            const unsigned char *au = u->buf + off + 5;
            // H.264 frames depend on each other: when the viewer falls behind,
            // drop until the next key frame (one a second). Sound packets
            // (key 2) stand alone: dropped only while the backlog is large.
            int audio = key == 2, behind = c->outlen - c->outoff > (1u << 20);
            if (!audio && behind) u->need_key = 1;
            if (audio ? behind : u->need_key && !key) u->dropped++;
            else {
                if (!audio) u->need_key = 0;
                unsigned char h[2] = { 6, (unsigned char)key };
                ws_header(c, 2, sizeof h + len);
                out_put(c, h, sizeof h);
                out_put(c, au, len);
                if (!audio) u->frames++, c->frames++;
                if (flush(c) < 0) { client_close(c); return; }
            }
            off += 5 + len;
        }
        memmove(u->buf, u->buf + off, u->len - off);
        u->len -= off;
    }
}

static void handle_ws(struct client *c)
{
    for (;;) {
        if (c->inlen < 2) return;
        unsigned char *p = c->in;
        int op = p[0] & 15, masked = p[1] & 0x80;
        size_t len = p[1] & 127, h = 2;
        if (len == 126) { if (c->inlen < 4) return; len = p[2] << 8 | p[3]; h = 4; }
        else if (len == 127) { client_close(c); return; }
        if (!masked || len > sizeof c->in - 14) { client_close(c); return; }
        if (c->inlen < h + 4 + len) return;
        unsigned char *m = p + h, *d = p + h + 4;
        for (size_t i = 0; i < len; i++) d[i] ^= m[i & 3];
        if (op == 8) { client_close(c); return; }
        if (op == 9) { ws_header(c, 10, len); out_put(c, d, len); if (flush(c) < 0) { client_close(c); return; } }
        if (op == 2 && len >= 1) {
            switch (d[0]) {
            case 2: if (len >= 3) key_event(d[1], d[2]); activity(); break;
            case 3: if (len >= 6) ptr_event(d[1], d[2] | d[3] << 8, d[4] | d[5] << 8); activity(); break;
            case 4: release_all(); break;
            case 9: if (len >= 3) wheel_event(d[1], d[2]); activity(); break;
            case 5: if (len >= 4) apply_settings(d[1], d[2], d[3]); break;
            case 7: {
                char addr[160];
                size_t n = len - 1 < sizeof addr - 1 ? len - 1 : sizeof addr - 1;
                memcpy(addr, d + 1, n);
                addr[n] = 0;
                up_open(c, addr, 0);
                break;
            }
            case 8: up_close(c, NULL); break;
            case 11:
                if (len < 2) break;
                c->want_audio = d[1];
                c->audio_retry = 0;
                if (!c->want_audio && c->up && c->up->audio_only) up_close(c, NULL);
                break;
            }
        }
        size_t used = h + 4 + len;
        memmove(c->in, c->in + used, c->inlen - used);
        c->inlen -= used;
    }
}

static void client_io(struct client *c, uint32_t events)
{
    if (c->state == C_TLS) {
        int r = SSL_accept(c->ssl);
        if (r <= 0) {
            int e = SSL_get_error(c->ssl, r);
            if (e == SSL_ERROR_WANT_READ) { client_poll(c, 0); return; }
            if (e == SSL_ERROR_WANT_WRITE) { client_poll(c, 1); return; }
            client_close(c);
            return;
        }
        c->state = C_HTTP;
    }
    if ((events & EPOLLOUT) && c->outlen) {
        if (flush(c) < 0) { client_close(c); return; }
        if (c->state == -1 && !c->outlen) { client_close(c); return; }
    }
    for (;;) {
        if (c->inlen >= sizeof c->in - 1) { client_close(c); return; }
        int n = SSL_read(c->ssl, c->in + c->inlen, (int)(sizeof c->in - 1 - c->inlen));
        if (n <= 0) {
            int e = SSL_get_error(c->ssl, n);
            if (e == SSL_ERROR_WANT_READ || e == SSL_ERROR_WANT_WRITE) break;
            client_close(c);
            return;
        }
        c->inlen += n;
        c->in[c->inlen] = 0;
        if (c->state == C_HTTP) {
            handle_http(c);
            if (c->state == -1) {
                if (!c->outlen) client_close(c);
                return;
            }
        } else if (c->state == C_WS) {
            handle_ws(c);
        }
        // the client may be gone now
        int alive = 0;
        for (int i = 0; i < MAX_CLIENTS; i++) alive |= clients[i] == c;
        if (!alive) return;
    }
}

static void accept_client(int lfd)
{
    for (;;) {
        int fd = accept4(lfd, NULL, NULL, SOCK_NONBLOCK | SOCK_CLOEXEC);
        if (fd < 0) return;
        int slot = -1;
        for (int i = 0; i < MAX_CLIENTS; i++)
            if (!clients[i]) { slot = i; break; }
        if (slot < 0) { close(fd); continue; }
        int one = 1;
        setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof one);
        int sb = 1 << 20;
        setsockopt(fd, SOL_SOCKET, SO_SNDBUF, &sb, sizeof sb);
        struct client *c = calloc(1, sizeof *c);
        c->kind = K_CLIENT;
        c->fd = fd;
        c->ssl = SSL_new(ctx);
        SSL_set_fd(c->ssl, fd);
        SSL_set_mode(c->ssl, SSL_MODE_ENABLE_PARTIAL_WRITE | SSL_MODE_ACCEPT_MOVING_WRITE_BUFFER);
        c->state = C_TLS;
        clients[slot] = c;
        struct epoll_event ev = { .events = EPOLLIN, .data.ptr = c };
        epoll_ctl(ep, EPOLL_CTL_ADD, fd, &ev);
        client_io(c, EPOLLIN);
    }
}

// ------------------------------------------------------------------ frames
static void video_ready(uint32_t events)
{
    if (events & EPOLLPRI) {
        struct v4l2_event e;
        while (!ioctl(vfd, VIDIOC_DQEVENT, &e))
            if (e.type == V4L2_EVENT_SOURCE_CHANGE) {
                LOG("source changed");
                const char *err;
                if (video_start(&err)) broadcast_text("{\"t\":\"%s\"}", "nosignal");
                return;
            }
    }
    for (;;) {
        struct v4l2_buffer b = { .type = V4L2_BUF_TYPE_VIDEO_CAPTURE, .memory = V4L2_MEMORY_MMAP };
        if (ioctl(vfd, VIDIOC_DQBUF, &b)) return;
        size_t len = b.bytesused;
        int ok = b.index < NBUF && vbuf[b.index].p && len && !(b.flags & V4L2_BUF_FLAG_ERROR);
        if (ok && len > buf_cap) {
            free(prev), free(cur);
            buf_cap = len + 65536;
            prev = malloc(buf_cap), cur = malloc(buf_cap);
            prev_len = 0;
            if (!prev || !cur) { free(prev), free(cur); prev = cur = NULL; buf_cap = 0; ok = 0; }
        }
        if (ok) memcpy(cur, vbuf[b.index].p, len);
        ioctl(vfd, VIDIOC_QBUF, &b);
        if (!ok) continue;
        n_frames++;
        last_frame = now();
        size_t skip = jpeg_skip(cur, len);
        int same = len == prev_len && same_bytes(cur + skip, prev + skip, len - skip);
        if (same) {
            if (last_frame - last_change > IDLE_AFTER) set_rate(IDLE_FPS);
            if (last_frame - last_sent < 1.0 && !fresh_viewer) {
                n_same++;
                continue;
            }
        } else {
            last_change = last_frame;
            set_rate(want_fps);
        }
        unsigned char *t = prev;
        prev = cur, cur = t, prev_len = len;
        last_sent = last_frame;
        fresh_viewer = 0;
        seq++;
        n_bytes += len;
        unsigned char h[9] = { 1, vw & 0xff, vw >> 8, vh & 0xff, vh >> 8, seq & 0xff, seq >> 8 & 0xff, seq >> 16 & 0xff, seq >> 24 };
        for (int i = 0; i < MAX_CLIENTS; i++) {
            struct client *c = clients[i];
            if (!on_jpeg(c)) continue;
            if (c->outlen - c->outoff > 0) { c->skipped++; continue; }
            ws_header(c, 2, sizeof h + len);
            out_put(c, h, sizeof h);
            out_put(c, prev, len);
            c->frames++;
            if (flush(c) < 0) client_close(c);
        }
        if (vfd < 0) return;
    }
}

// ------------------------------------------------------------------ D-Bus
static int method_ticket(sd_bus_message *m, void *ud, sd_bus_error *err)
{
    (void)ud, (void)err;
    char json[200];
    snprintf(json, sizeof json, "{\"ticket\":\"%s\",\"port\":%d}", ticket_new(), port);
    return sd_bus_reply_method_return(m, "s", json);
}

static int method_status(sd_bus_message *m, void *ud, sd_bus_error *err)
{
    (void)ud, (void)err;
    char json[512];
    snprintf(json, sizeof json, "{\"port\":%d,\"viewers\":%d,\"streaming\":%d,\"signal\":%d,\"width\":%u,\"height\":%u,\"fps\":%d,\"quality\":%d,\"sub420\":%d,\"leds\":%d,\"usboff\":%.0f,\"hid\":[%ld,%ld,%ld,%ld,%ld,%ld]}",
             port, ws_clients(), streaming, signal_ok, vw, vh, want_fps, quality, sub420, leds,
             usb_off_since ? now() - usb_off_since : 0.0,
             kbd.written, kbd.merged, kbd.dropped, ptr.written, ptr.merged, ptr.dropped);
    return sd_bus_reply_method_return(m, "s", json);
}

static const sd_bus_vtable kvm_vtable[] = {
    SD_BUS_VTABLE_START(0),
    SD_BUS_METHOD("Ticket", "", "s", method_ticket, SD_BUS_VTABLE_UNPRIVILEGED),
    SD_BUS_METHOD("Status", "", "s", method_status, SD_BUS_VTABLE_UNPRIVILEGED),
    SD_BUS_VTABLE_END,
};

// ------------------------------------------------------------------ main
static void usage(void)
{
    fprintf(stderr, "usage: reimu-kvm [-p port] [-c cert.pem] [-f fps] [-q 0..11] [-s 420|444] [-v video] [-k hidg] [-m hidg] [-d]\n");
    exit(2);
}

int main(int argc, char **argv)
{
    int o;
    while ((o = getopt(argc, argv, "p:c:f:q:s:v:k:m:d")) != -1)
        switch (o) {
        case 'p': port = atoi(optarg); break;
        case 'c': cert_path = optarg; break;
        case 'f': want_fps = atoi(optarg); break;
        case 'q': quality = atoi(optarg); break;
        case 's': sub420 = !strcmp(optarg, "420"); break;
        case 'v': video_path = optarg; break;
        case 'k': kbd_path = optarg; break;
        case 'm': ptr_path = optarg; break;
        case 'd': verbose = 1; break;
        default: usage();
        }
    if (want_fps < 1 || want_fps > 60 || quality < 0 || quality > 11) usage();
    signal(SIGPIPE, SIG_IGN);
    struct sigaction sa = { .sa_handler = on_signal };
    sigaction(SIGTERM, &sa, NULL);
    sigaction(SIGINT, &sa, NULL);
    sigaction(SIGHUP, &sa, NULL);

    ctx = SSL_CTX_new(TLS_server_method());
    SSL_CTX_set_min_proto_version(ctx, TLS1_2_VERSION);
    SSL_CTX_set_options(ctx, SSL_OP_CIPHER_SERVER_PREFERENCE | SSL_OP_NO_RENEGOTIATION);
    SSL_CTX_set_ciphersuites(ctx, "TLS_CHACHA20_POLY1305_SHA256:TLS_AES_128_GCM_SHA256:TLS_AES_256_GCM_SHA384");
    SSL_CTX_set_cipher_list(ctx, "ECDHE-ECDSA-CHACHA20-POLY1305:ECDHE-RSA-CHACHA20-POLY1305:ECDHE-ECDSA-AES128-GCM-SHA256:ECDHE-RSA-AES128-GCM-SHA256");
    SSL_CTX_set_ecdh_auto(ctx, 1);
    if (SSL_CTX_use_certificate_chain_file(ctx, cert_path) != 1 || SSL_CTX_use_PrivateKey_file(ctx, cert_path, SSL_FILETYPE_PEM) != 1) {
        LOG("cannot load %s", cert_path);
        return 1;
    }

    int lfd = socket(AF_INET6, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
    int one = 1, zero = 0;
    setsockopt(lfd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
    setsockopt(lfd, IPPROTO_IPV6, IPV6_V6ONLY, &zero, sizeof zero);
    struct sockaddr_in6 a = { .sin6_family = AF_INET6, .sin6_port = htons(port), .sin6_addr = in6addr_any };
    if (bind(lfd, (struct sockaddr *)&a, sizeof a) || listen(lfd, 8)) {
        LOG("port %d: %s", port, strerror(errno));
        return 1;
    }

    sd_bus *bus = NULL;
    if (sd_bus_open_system(&bus) < 0 ||
        sd_bus_add_object_vtable(bus, NULL, "/xyz/openbmc_project/reimu/kvm", "xyz.openbmc_project.Reimu.Kvm", kvm_vtable, NULL) < 0 ||
        sd_bus_request_name(bus, "xyz.openbmc_project.Reimu.Kvm", 0) < 0) {
        LOG("D-Bus registration failed");
        return 1;
    }

    ep = epoll_create1(EPOLL_CLOEXEC);
    struct epoll_event ev = { .events = EPOLLIN, .data.u32 = 0xfffffff1 };
    epoll_ctl(ep, EPOLL_CTL_ADD, lfd, &ev);
    // the listening socket and the bus are told apart from clients by data.u32
    // tags; clients carry a pointer (heap pointers on the BMC are never these)
    struct epoll_event bev = { .events = EPOLLIN, .data.u32 = 0xfffffff2 };
    epoll_ctl(ep, EPOLL_CTL_ADD, sd_bus_get_fd(bus), &bev);
    // the same port number over UDP: reimu-h264's announcements
    int ufd = socket(AF_INET6, SOCK_DGRAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
    setsockopt(ufd, IPPROTO_IPV6, IPV6_V6ONLY, &zero, sizeof zero);
    if (ufd >= 0 && !bind(ufd, (struct sockaddr *)&a, sizeof a)) {
        struct epoll_event uev = { .events = EPOLLIN, .data.u32 = 0xfffffff3 };
        epoll_ctl(ep, EPOLL_CTL_ADD, ufd, &uev);
    } else LOG("UDP %d: %s (no host announcements)", port, strerror(errno));
    hid_open(&kbd); // the keyboard's LED reports are read from the start
    hid_open(&ptr);
    hid_flush(&kbd), hid_flush(&ptr); // the release hid_open queued
    LOG("reimu-kvm on :%d", port);

    double stat_t = now();
    for (;;) {
        struct epoll_event evs[16];
        int n = epoll_wait(ep, evs, 16, 250);
        if (quit) {
            LOG("signal %d: keys and buttons released, exiting", (int)quit);
            hid_goodbye();
            return 0;
        }
        for (int i = 0; i < n; i++) {
            uint32_t tag = evs[i].data.u32;
            if (tag == TAG_KBD) hid_io(&kbd, evs[i].events);
            else if (tag == TAG_PTR) hid_io(&ptr, evs[i].events);
            else if (tag == 0xfffffff1) accept_client(lfd);
            else if (tag == 0xfffffff2) while (sd_bus_process(bus, NULL) > 0) ;
            else if (tag == 0xfffffff3) announce_read(ufd);
            else if (tag == 0xfffffff0) { if (vfd >= 0) video_ready(evs[i].events); }
            else if (*(int *)evs[i].data.ptr == K_UP) up_io(evs[i].data.ptr, evs[i].events);
            else if (*(int *)evs[i].data.ptr == K_CLIENT) client_io(evs[i].data.ptr, evs[i].events);
        }
        for (int i = 0; i < ngrave; i++) free(grave[i]);
        ngrave = 0;
        while (sd_bus_process(bus, NULL) > 0) ;
        double t = now();
        hid_check_stall(&kbd, t);
        hid_check_stall(&ptr, t);
        // the LEDs come in on the keyboard fd: keep one open (closed by an unbind)
        static double hid_retry;
        if (kbd.fd < 0 && t > hid_retry) { hid_retry = t + 1; if (!hid_open(&kbd)) hid_flush(&kbd); }
        // the host's sound for JPEG viewers who want it
        for (int i = 0; i < MAX_CLIENTS; i++) {
            struct client *c = clients[i];
            if (on_jpeg(c) && c->want_audio && !c->up && t > c->audio_retry && *announced_host()) {
                c->audio_retry = t + 10;
                up_open(c, "", 1);
            }
        }
        if (jpeg_clients()) {
            if (!streaming && t - last_try > 2) {
                const char *err = NULL;
                video_start(&err);
            } else if (streaming && t - last_frame > 1.5) {
                // no frames: signal lost or mode change without an event
                const char *err = NULL;
                if (video_start(&err)) broadcast_text("{\"t\":\"%s\"}", "nosignal");
            }
        }
        if (t - stat_t >= 1) {
            for (int i = 0; i < MAX_CLIENTS; i++) {
                struct client *c = clients[i];
                if (!c || c->state != C_WS) continue;
                char s[400];
                snprintf(s, sizeof s, "{\"t\":\"stat\",\"cap\":%.1f,\"same\":%.1f,\"kbps\":%.0f,\"sent\":%ld,\"skipped\":%ld,\"w\":%u,\"h\":%u,\"fps\":%d,\"q\":%d,\"sub420\":%d,\"viewers\":%d,\"signal\":%d,\"host\":\"%s\",\"leds\":%d}",
                         n_frames / (t - stat_t), n_same / (t - stat_t), n_bytes * 8 / (t - stat_t) / 1000, c->frames, c->skipped, vw, vh, want_fps, quality, sub420, ws_clients(), streaming, announced_host(), leds);
                c->frames = c->skipped = 0;
                // stats go out only between frames, never into the middle of one
                if (c->outlen == c->outoff) ws_text(c, "%s", s);
            }
            n_frames = n_bytes = n_same = 0;
            stat_t = t;
        }
    }
}
