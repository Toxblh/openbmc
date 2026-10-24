// Unit test of reimu-kvm's HID side: the keyboard report builder (more than
// six keys: ErrorRollOver, never a key silently left out) and the report
// queue (nothing lost while the device is busy, pointer moves merged, wheel
// steps added up, a stalled device released).
//
//   cc -o t test_reimu_kvm_hid.c -lssl -lcrypto -lsystemd && ./t   (REIMU_KVM_C=path)
//
// reimu-kvm.c is included with its main() renamed; /dev/hidg0 and /dev/hidg1
// are stood in for by SOCK_SEQPACKET socket pairs (one report per message,
// a small send buffer makes write() fail with EAGAIN like a busy f_hid).
#define main reimu_kvm_main
#include REIMU_KVM_C
#undef main
#include <sys/socket.h>

static int fails;
#define CHECK(c, ...) do { printf((c) ? "  PASS " : "  FAIL "); printf(__VA_ARGS__); printf("\n"); if (!(c)) fails++; } while (0)

#ifdef TAG_KBD
#define KFD kbd.fd
#define PFD ptr.fd
#else
#define KFD kbd_fd
#define PFD ptr_fd
#endif

static int kr, pr; // the reading ends

static void setup(int sndbuf)
{
    int k[2], p[2];
    socketpair(AF_UNIX, SOCK_SEQPACKET | SOCK_NONBLOCK, 0, k);
    socketpair(AF_UNIX, SOCK_SEQPACKET | SOCK_NONBLOCK, 0, p);
    if (sndbuf) {
        setsockopt(k[0], SOL_SOCKET, SO_SNDBUF, &sndbuf, sizeof sndbuf);
        setsockopt(p[0], SOL_SOCKET, SO_SNDBUF, &sndbuf, sizeof sndbuf);
    }
    if (KFD >= 0) close(KFD);
    if (PFD >= 0) close(PFD);
    KFD = k[0], PFD = p[0], kr = k[1], pr = p[1];
    if (!ep) ep = epoll_create1(0);
#ifdef TAG_KBD
    struct epoll_event e1 = { .events = EPOLLIN, .data.u32 = TAG_KBD }, e2 = { .events = EPOLLIN, .data.u32 = TAG_PTR };
    epoll_ctl(ep, EPOLL_CTL_ADD, KFD, &e1);
    epoll_ctl(ep, EPOLL_CTL_ADD, PFD, &e2);
    kbd.n = ptr.n = 0;
    memset(kbd.last, 0, 8), memset(ptr.last, 0, 8);
#endif
}

// every report waiting on the reading end; while reading, the writer's
// queue is flushed as a host taking reports would let it
static int drain(int fd, uint8_t (*out)[8], int max, int len)
{
    int n = 0;
    for (int idle = 0; idle < 3 && n < max;) {
        uint8_t b[16];
        ssize_t r = read(fd, b, sizeof b);
        if (r == len) { memcpy(out[n++], b, len); idle = 0; continue; }
#ifdef TAG_KBD
        hid_flush(&kbd), hid_flush(&ptr);
#endif
        idle++;
    }
    return n;
}

static void t_rollover(void)
{
    printf("== rollover: 7 keys down\n");
    setup(0);
    for (int u = 0x04; u <= 0x0a; u++) key_event(u, 1);
    uint8_t r[16][8];
    int n = drain(kr, r, 16, 8);
    CHECK(n == 7, "7 reports for 7 presses (%d)", n);
    const uint8_t six[8] = { 0, 0, 4, 5, 6, 7, 8, 9 }, roll[8] = { 0, 0, 1, 1, 1, 1, 1, 1 };
    CHECK(n >= 6 && !memcmp(r[5], six, 8), "6 keys: all six listed");
    CHECK(n >= 7 && !memcmp(r[6], roll, 8), "7 keys: ErrorRollOver in every slot (%02x %02x %02x ...)", n >= 7 ? r[6][2] : 0, n >= 7 ? r[6][3] : 0, n >= 7 ? r[6][4] : 0);
    key_event(0x04, 0);
    n = drain(kr, r, 16, 8);
    const uint8_t back[8] = { 0, 0, 5, 6, 7, 8, 9, 10 };
    CHECK(n == 1 && !memcmp(r[0], back, 8), "one released: the other six, the 7th among them (%02x..%02x)", r[0][2], r[0][7]);
    key_event(0xe1, 1); // left shift with them
    n = drain(kr, r, 16, 8);
    CHECK(n == 1 && r[0][0] == 0x02, "a modifier: its bit, the keys unchanged");
    for (int u = 0x05; u <= 0x0a; u++) key_event(u, 0);
    key_event(0xe1, 0);
    n = drain(kr, r, 16, 8);
    const uint8_t none[8] = { 0 };
    CHECK(n == 7 && !memcmp(r[n - 1], none, 8), "all released: an empty report last (%d reports)", n);
}

static void t_queue(void)
{
    printf("== queue: 300 presses while the device is busy\n");
    setup(1); // the smallest send buffer: a few reports fit, then EAGAIN
    for (int i = 0; i < 150; i++) key_event(0x04, 1), key_event(0x04, 0);
    static uint8_t r[400][8];
    int n = drain(kr, r, 400, 8);
    int ok = n == 300;
    for (int i = 0; ok && i < n; i++) ok = r[i][2] == (i % 2 ? 0 : 0x04);
    CHECK(ok, "the host gets all 300 reports in order (%d)", n);
}

static void t_pointer(void)
{
    printf("== pointer: moves merged, the click kept where it was, wheel steps added\n");
    setup(1);
    // fill the socket so the following ones wait in the queue
    for (int i = 0; i < 40; i++) ptr_event(0, i, i);
    for (int i = 0; i < 50; i++) ptr_event(0, 100 + i, 100);  // moves
    ptr_event(1, 200, 200);                                   // left down at 200,200
    ptr_event(0, 200, 200);                                   // up
    for (int i = 0; i < 5; i++) wheel_event(1, 0);            // 5 wheel steps
    static uint8_t r[200][8];
    int n = drain(pr, r, 200, 7);
    int down = -1, up = -1, wheel = 0;
    for (int i = 0; i < n; i++) {
        if (r[i][0] == 1 && down < 0) down = i;
        if (down >= 0 && r[i][0] == 0 && up < 0 && i > down) up = i;
        wheel += (int8_t)r[i][5];
    }
    CHECK(down >= 0 && r[down][1] == 200 && r[down][3] == 200, "the button went down at 200,200");
    CHECK(up > down, "and came up after it");
    CHECK(wheel == 5, "5 wheel steps arrive in all (%d)", wheel);
    CHECK(n < 40 + 50 + 7, "queued moves merged (%d reports for %d events)", n, 40 + 50 + 7);
    CHECK(n && r[n - 1][1] == 200, "the last position is the newest");
}

static void t_stall(void)
{
#ifdef TAG_KBD
    printf("== stall: a device that takes nothing for %.0f s\n", HID_STALL);
    setup(1);
    for (int i = 0; i < 100; i++) key_event(0x04, 1), key_event(0x04, 0);
    key_event(0x07, 1); // held
    int queued = kbd.n;
    hid_check_stall(&kbd, now() + HID_STALL + 1);
    CHECK(queued > 1 && kbd.n <= 1, "the queue (%d) dropped (%d left)", queued, kbd.n);
    uint8_t q0[8];
    memcpy(q0, kbd.n ? kbd.q[kbd.head] : kbd.last, 8); // what the host ends with
    const uint8_t none[8] = { 0 };
    CHECK(!memcmp(q0, none, 8), "the host is left with nothing held");
    CHECK(ndown == 0 && mods == 0, "the state says nothing is held");
#else
    printf("== stall: (no report queue in this reimu-kvm)\n");
    CHECK(0, "a stalled device is released");
#endif
}

int main(void)
{
    t_rollover();
    t_queue();
    t_pointer();
    t_stall();
    printf("%d failed\n", fails);
    return fails != 0;
}
