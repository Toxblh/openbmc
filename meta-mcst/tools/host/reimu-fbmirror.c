// reimu-fbmirror: the X desktop of the main video card copied into the BMC's
// ASPEED framebuffer, so that the REIMU KVM (the BMC's video engine) shows it.
//
// Instead of grabbing whole frames (ffmpeg x11grab + colour conversion: most
// of a CPU at 8 frames/s), only what X reports as changed (XDamage) is read
// with MIT-SHM and written to /dev/fbN as whole rows: the kernel copies just
// those rows to the card. X's 32-bit pixels are the framebuffer's XRGB8888 as
// they are, so nothing is converted. The pointer is drawn in (X leaves it out).
//
//   reimu-fbmirror [-f fps] [-d /dev/fbN] [-v]
//   reimu-fbmirror -t      only check that the X display opens (exit status)
//
// The desktop is scaled (nearest pixel) when its size differs from the
// framebuffer's. It exits when X goes away or the desktop changes size (the
// service restarts it). Build: gcc -O2 -o reimu-fbmirror reimu-fbmirror.c
//                        $(pkg-config --cflags --libs xcb xcb-shm xcb-damage xcb-xfixes)
#include <errno.h>
#include <fcntl.h>
#include <linux/fb.h>
#include <poll.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/ipc.h>
#include <sys/shm.h>
#include <time.h>
#include <unistd.h>
#include <xcb/damage.h>
#include <xcb/shm.h>
#include <xcb/xcb.h>
#include <xcb/xfixes.h>

static int verbose;
static double now(void)
{
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return t.tv_sec + t.tv_nsec / 1e9;
}

static const char *find_fb(void)
{
    static char path[32];
    for (int i = 0; i < 8; i++) {
        char n[64], name[64] = "";
        snprintf(n, sizeof n, "/sys/class/graphics/fb%d/name", i);
        FILE *f = fopen(n, "r");
        if (!f) continue;
        if (fgets(name, sizeof name, f) && !strncmp(name, "astdrmfb", 8)) {
            fclose(f);
            snprintf(path, sizeof path, "/dev/fb%d", i);
            return path;
        }
        fclose(f);
    }
    return NULL;
}

// ------------------------------------------------------------------ state
static xcb_connection_t *xc;
static xcb_window_t root;
static int W, H;            // desktop
static int FW, FH, stride;  // framebuffer
static int fb = -1;
static uint32_t *clean;     // the desktop as last read (W x H)
static uint32_t *row;       // one output band (FW wide, up to FH rows)
static int *xmap;           // framebuffer column -> desktop column
static xcb_shm_seg_t seg;
static uint32_t *shm;
static int shmid = -1;

// pointer
static int cur_x = -1, cur_y = -1, cur_w, cur_h, cur_hx, cur_hy, cur_serial = -1;
static uint32_t *cur_img;

// dirty framebuffer rows [y0, y1)
static int dirty0 = 1 << 30, dirty1 = -1;
static void dirty_fb_rows(int y0, int y1)
{
    if (y0 < 0) y0 = 0;
    if (y1 > FH) y1 = FH;
    if (y0 >= y1) return;
    if (y0 < dirty0) dirty0 = y0;
    if (y1 > dirty1) dirty1 = y1;
}
// desktop rows -> framebuffer rows
static int fb_row_of(int y) { return (int)((int64_t)y * FH / H); }
static void dirty_desk_rows(int y0, int y1) { dirty_fb_rows(fb_row_of(y0), fb_row_of(y1 - 1) + 1 + (FH > H)); }

// ------------------------------------------------------------------ X
static int read_rect(int x, int y, int w, int h)
{
    if (x < 0) w += x, x = 0;
    if (y < 0) h += y, y = 0;
    if (x + w > W) w = W - x;
    if (y + h > H) h = H - y;
    if (w <= 0 || h <= 0) return 0;
    xcb_shm_get_image_reply_t *r = xcb_shm_get_image_reply(xc,
        xcb_shm_get_image(xc, root, x, y, w, h, ~0u, XCB_IMAGE_FORMAT_Z_PIXMAP, seg, 0), NULL);
    if (!r) return -1;
    free(r);
    for (int j = 0; j < h; j++) memcpy(clean + (size_t)(y + j) * W + x, shm + (size_t)j * w, (size_t)w * 4);
    dirty_desk_rows(y, y + h);
    return 0;
}

static void read_cursor(void)
{
    xcb_xfixes_get_cursor_image_reply_t *r = xcb_xfixes_get_cursor_image_reply(xc, xcb_xfixes_get_cursor_image(xc), NULL);
    if (!r) return;
    int shape = (int)r->cursor_serial != cur_serial, moved = r->x != cur_x || r->y != cur_y;
    if (!shape && !moved) {
        free(r);
        return;
    }
    // the rows under the old pointer and under the new one
    if (cur_x >= 0) dirty_desk_rows(cur_y - cur_hy, cur_y - cur_hy + cur_h);
    if (shape) {
        free(cur_img);
        cur_w = r->width, cur_h = r->height, cur_hx = r->xhot, cur_hy = r->yhot;
        cur_img = malloc((size_t)cur_w * cur_h * 4);
        memcpy(cur_img, xcb_xfixes_get_cursor_image_cursor_image(r), (size_t)cur_w * cur_h * 4);
        cur_serial = r->cursor_serial;
    }
    cur_x = r->x, cur_y = r->y;
    dirty_desk_rows(cur_y - cur_hy, cur_y - cur_hy + cur_h);
    free(r);
}

// ------------------------------------------------------------------ output
static inline uint32_t blend(uint32_t d, uint32_t s)
{
    uint32_t a = s >> 24;
    if (a == 255) return s;
    if (!a) return d;
    // XFixes cursor pixels are premultiplied ARGB
    uint32_t na = 255 - a;
    uint32_t rb = ((d & 0xff00ff) * na >> 8 & 0xff00ff) + (s & 0xff00ff);
    uint32_t g = ((d & 0x00ff00) * na >> 8 & 0x00ff00) + (s & 0x00ff00);
    return rb | g;
}

static void write_rows(void)
{
    if (dirty1 <= dirty0) return;
    int y0 = dirty0, y1 = dirty1;
    dirty0 = 1 << 30, dirty1 = -1;
    for (int fy = y0; fy < y1; fy++) {
        int sy = (int)((int64_t)fy * H / FH);
        const uint32_t *src = clean + (size_t)sy * W;
        uint32_t *dst = row + (size_t)(fy - y0) * FW;
        if (FW == W) memcpy(dst, src, (size_t)W * 4);
        else for (int x = 0; x < FW; x++) dst[x] = src[xmap[x]];
        // the pointer, in desktop coordinates
        int cy = sy - (cur_y - cur_hy);
        if (cur_img && cy >= 0 && cy < cur_h) {
            for (int cx = 0; cx < cur_w; cx++) {
                int dx = cur_x - cur_hx + cx;
                if (dx < 0 || dx >= W) continue;
                int fx = FW == W ? dx : (int)((int64_t)dx * FW / W);
                dst[fx] = blend(dst[fx], cur_img[cy * cur_w + cx]);
            }
        }
    }
    if (stride == FW * 4) {
        if (pwrite(fb, row, (size_t)(y1 - y0) * stride, (off_t)y0 * stride) < 0 && verbose) perror("fb");
    } else {
        for (int fy = y0; fy < y1; fy++) pwrite(fb, row + (size_t)(fy - y0) * FW, (size_t)FW * 4, (off_t)fy * stride);
    }
}

int main(int argc, char **argv)
{
    int fps = 60, o, test = 0;
    const char *fbpath = NULL;
    while ((o = getopt(argc, argv, "f:d:vt")) != -1)
        switch (o) {
        case 'f': fps = atoi(optarg); break;
        case 'd': fbpath = optarg; break;
        case 'v': verbose = 1; break;
        case 't': test = 1; break;
        default: fprintf(stderr, "usage: reimu-fbmirror [-f fps] [-d /dev/fbN] [-v] | -t\n"); return 2;
        }
    if (test) {
        xc = xcb_connect(NULL, NULL);
        return xcb_connection_has_error(xc) ? 1 : 0;
    }
    if (fps < 1 || fps > 120) fps = 60;
    if (!fbpath && !(fbpath = find_fb())) { fprintf(stderr, "ASPEED framebuffer (astdrmfb) not found\n"); return 1; }
    fb = open(fbpath, O_RDWR | O_CLOEXEC);
    if (fb < 0) { perror(fbpath); return 1; }
    struct fb_var_screeninfo vi;
    struct fb_fix_screeninfo fi;
    if (ioctl(fb, FBIOGET_VSCREENINFO, &vi) || ioctl(fb, FBIOGET_FSCREENINFO, &fi) || vi.bits_per_pixel != 32) {
        fprintf(stderr, "%s: not a 32-bit framebuffer\n", fbpath);
        return 1;
    }
    FW = vi.xres, FH = vi.yres, stride = fi.line_length;

    int scr;
    xc = xcb_connect(NULL, &scr);
    if (xcb_connection_has_error(xc)) { fprintf(stderr, "cannot open the X display\n"); return 1; }
    const xcb_setup_t *setup = xcb_get_setup(xc);
    xcb_screen_iterator_t it = xcb_setup_roots_iterator(setup);
    for (int i = 0; i < scr; i++) xcb_screen_next(&it);
    root = it.data->root;
    W = it.data->width_in_pixels, H = it.data->height_in_pixels;
    if (it.data->root_depth != 24) { fprintf(stderr, "X depth %d: only 24-bit desktops\n", it.data->root_depth); return 1; }

    xcb_damage_query_version_reply_t *dv = xcb_damage_query_version_reply(xc, xcb_damage_query_version(xc, 1, 1), NULL);
    xcb_xfixes_query_version_reply_t *xv = xcb_xfixes_query_version_reply(xc, xcb_xfixes_query_version(xc, 4, 0), NULL);
    if (!dv || !xv) { fprintf(stderr, "X has no DAMAGE/XFIXES\n"); return 1; }
    free(dv), free(xv);
    const xcb_query_extension_reply_t *dext = xcb_get_extension_data(xc, &xcb_damage_id);
    const xcb_query_extension_reply_t *xext = xcb_get_extension_data(xc, &xcb_xfixes_id);

    shmid = shmget(IPC_PRIVATE, (size_t)W * H * 4, IPC_CREAT | 0600);
    shm = shmat(shmid, NULL, 0);
    shmctl(shmid, IPC_RMID, NULL);
    seg = xcb_generate_id(xc);
    xcb_shm_attach(xc, seg, shmid, 0);

    clean = malloc((size_t)W * H * 4);
    row = malloc((size_t)FW * FH * 4);
    xmap = malloc(sizeof(int) * FW);
    for (int x = 0; x < FW; x++) xmap[x] = (int)((int64_t)x * W / FW);

    xcb_damage_damage_t dmg = xcb_generate_id(xc);
    xcb_damage_create(xc, dmg, root, XCB_DAMAGE_REPORT_LEVEL_NON_EMPTY);
    xcb_xfixes_region_t region = xcb_generate_id(xc);
    xcb_xfixes_create_region(xc, region, 0, NULL);
    xcb_xfixes_select_cursor_input(xc, root, XCB_XFIXES_CURSOR_NOTIFY_MASK_DISPLAY_CURSOR);
    uint32_t mask = XCB_EVENT_MASK_STRUCTURE_NOTIFY;
    xcb_change_window_attributes(xc, root, XCB_CW_EVENT_MASK, &mask);
    xcb_flush(xc);

    fprintf(stderr, "reimu-fbmirror: desktop %dx%d -> %s %dx%d, up to %d frames/s\n", W, H, fbpath, FW, FH, fps);
    read_rect(0, 0, W, H);
    read_cursor();
    write_rows();

    double frame = 1.0 / fps, next = now(), stat_t = now();
    long n_frames = 0, n_rows = 0;
    int damaged = 0;
    struct pollfd pfd = { .fd = xcb_get_file_descriptor(xc), .events = POLLIN };
    for (;;) {
        xcb_generic_event_t *ev;
        while ((ev = xcb_poll_for_event(xc))) {
            int t = ev->response_type & 0x7f;
            if (t == dext->first_event + XCB_DAMAGE_NOTIFY) damaged = 1;
            else if (t == xext->first_event + XCB_XFIXES_CURSOR_NOTIFY) cur_serial = -1;
            else if (t == XCB_CONFIGURE_NOTIFY) {
                xcb_configure_notify_event_t *c = (xcb_configure_notify_event_t *)ev;
                if (c->window == root && (c->width != W || c->height != H)) {
                    fprintf(stderr, "desktop is now %dx%d\n", c->width, c->height);
                    return 0;
                }
            }
            free(ev);
        }
        if (xcb_connection_has_error(xc)) { fprintf(stderr, "X is gone\n"); return 0; }
        double t = now();
        if (t < next) {
            poll(&pfd, 1, (int)((next - t) * 1000) + 1);
            continue;
        }
        next = t + frame > next + frame ? t + frame : next + frame;
        if (damaged) {
            damaged = 0;
            xcb_damage_subtract(xc, dmg, XCB_NONE, region);
            xcb_xfixes_fetch_region_reply_t *r = xcb_xfixes_fetch_region_reply(xc, xcb_xfixes_fetch_region(xc, region), NULL);
            if (r) {
                xcb_rectangle_t *rs = xcb_xfixes_fetch_region_rectangles(r);
                int n = xcb_xfixes_fetch_region_rectangles_length(r);
                // many small rectangles: read their bounding box at once
                if (n > 16) {
                    int x0 = W, y0 = H, x1 = 0, y1 = 0;
                    for (int i = 0; i < n; i++) {
                        if (rs[i].x < x0) x0 = rs[i].x;
                        if (rs[i].y < y0) y0 = rs[i].y;
                        if (rs[i].x + rs[i].width > x1) x1 = rs[i].x + rs[i].width;
                        if (rs[i].y + rs[i].height > y1) y1 = rs[i].y + rs[i].height;
                    }
                    read_rect(x0, y0, x1 - x0, y1 - y0);
                } else
                    for (int i = 0; i < n; i++) read_rect(rs[i].x, rs[i].y, rs[i].width, rs[i].height);
                free(r);
            }
        }
        read_cursor();
        if (dirty1 > dirty0) {
            n_rows += dirty1 - dirty0;
            n_frames++;
            write_rows();
        }
        if (verbose && t - stat_t >= 2) {
            fprintf(stderr, "%.1f updates/s, %.0f rows/update\n", n_frames / (t - stat_t), n_frames ? (double)n_rows / n_frames : 0);
            n_frames = n_rows = 0;
            stat_t = t;
        }
    }
}
