#include <stdio.h>
#include <stdlib.h>
#include <signal.h>
#include <unistd.h>
#include <stdint.h>
#include <string.h>
#include <time.h>
#include <errno.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <stddef.h>
#include <linux/i2c-dev.h>
#include <libfdt.h>
#include <reimu.h>

/* This daemon runs for years: nothing in the main loop may allocate memory
 * it doesn't free, and nothing but OOM may make it exit. A failure is
 * reported once (when it starts and when it's gone) and retried next poll. */

/* Defaults, used until the config is read, and kept while it can't be */
int overheatd_enabled = 1;
int debug = 0;
long cpu_temp_limit = 85;
long wait_msec = 5000;
long blink_msec = 500;
long pcs_bus = 5;        /* BMC I2C bus of the E8C2 PCS (CPU temperature sensors) */
long pcs_settle_sec = 30; /* Don't touch the platform I2C while it powers up */
int overheat_poweroff = 1;              /* OVERHEAT_POWEROFF: the power actions; off: the alarm only */
long cpu_shutdown_temp = 90;            /* A graceful shutdown of the host */
long cpu_poweroff_temp = 95;            /* The forced power-off */
long graceful_timeout_sec = 60;         /* Still hot this long after the graceful request: forced */
long blind_alarm_min = 10;              /* Blind this long: said again, every so long */

#define dbg_printf if(debug) reimu_message

volatile int s_exit_req = 0;
volatile int s_exit_sig;

static void exit_handler(int sig)
{
    signal(SIGTERM, SIG_IGN);
    signal(SIGHUP, SIG_IGN);
    signal(SIGINT, SIG_IGN);
    s_exit_req = 1;
    s_exit_sig = sig;
}

static void rm_pidfile(void)
{
    unlink("/run/overheatd.pid");
}

/* Read a small (sysfs) file as a string, without malloc. Returns its length, or -1 */
static int read_small_file(const char *name, char *buf, size_t size)
{
    int fd = open(name, O_RDONLY | O_CLOEXEC);
    if (fd < 0) return -1;
    size_t len = 0;
    while (len < size - 1)
    {
        ssize_t n = read(fd, buf + len, size - 1 - len);
        if (n < 0)
        {
            if (errno == EINTR) continue;
            close(fd);
            return -1;
        }
        if (n == 0) break;
        len += n;
    }
    close(fd);
    buf[len] = '\0';
    return len;
}

/* Write a string to a (sysfs) file. Only async-signal-safe calls: used by the SIGALRM handler too */
static int write_small_file(const char *name, const char *str)
{
    int fd = open(name, O_WRONLY | O_CLOEXEC);
    if (fd < 0) return -1;
    size_t len = strlen(str);
    int rv = (write(fd, str, len) == (ssize_t)len) ? 0 : -1;
    if (close(fd)) rv = -1;
    return rv;
}

static long monotonic_sec(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec;
}

/* sd_notify(3) without libsystemd: a datagram to $NOTIFY_SOCKET (none: not run by systemd) */
static void notify_systemd(const char *msg)
{
    const char *path = getenv("NOTIFY_SOCKET");
    if ((path == NULL) || ((path[0] != '/') && (path[0] != '@'))) return;
    struct sockaddr_un sa;
    size_t len = strlen(path);
    if (len >= sizeof(sa.sun_path)) return;
    memset(&sa, 0, sizeof(sa));
    sa.sun_family = AF_UNIX;
    memcpy(sa.sun_path, path, len);
    if (sa.sun_path[0] == '@') sa.sun_path[0] = '\0'; /* abstract socket */
    int fd = socket(AF_UNIX, SOCK_DGRAM | SOCK_CLOEXEC, 0);
    if (fd < 0) return;
    sendto(fd, msg, strlen(msg), MSG_NOSIGNAL, (struct sockaddr *)&sa, offsetof(struct sockaddr_un, sun_path) + len);
    close(fd);
}

/* Tell the systemd watchdog we're alive (WatchdogSec= in overheatd.service):
 * from the main loop only, so a hang anywhere (e.g. a read of a hung I2C
 * bus) gets the daemon restarted. At most once a second. */
static void watchdog_ping(void)
{
    static long last = -1;
    long now = monotonic_sec();
    if (now == last) return;
    last = now;
    notify_systemd("WATCHDOG=1");
}

/* CPU temperature sensors.
 *
 * The motherboard device tree (put by mcst-fruid) describes them either way:
 *   - old: a node with compatible "l_pcs_i2c" under a bus of /bmc (i2cN =
 *     "/path" is BMC bus N-1), read through the l_pcs_i2c hwmon driver;
 *   - current (ПНС devtree 1.0, 2.0): a top level node pcs@N per CPU N, with
 *     its sensors at 0x20+0x10*N+k, k = 0..7, on BMC bus PCS_BUS. These are
 *     read directly (2 bytes, big endian, 1/8 °C, 0 = no sensor), the same
 *     way reimu-pnsd does: a single read message, so the two don't disturb
 *     each other.
 * Without a device tree (mcst-fruid failed: FRU EEPROM unreadable while the
 * host was off at BMC boot), CPU 0 is probed this way: the protection must
 * not depend on the FRU. */

#define MAX_CPUS 4
#define MAX_SENSORS (MAX_CPUS * 8)
#define DEVTREE_FILE "/var/volatile/motherboard_devtree.dtb"
#define DEVTREE_MAX (4 * 1024 * 1024)

struct cpu_sensor
{
    int cpu, sensor, bus, reg;
    int hwmon_driver; /* 1: old format, through l_pcs_i2c hwmon; 0: direct I2C read */
    long hwmon;       /* hwmon number, -1 = not found (yet) */
    int seen;         /* direct: has given a non-zero reading since the host powered on */
    int failed;       /* the last read failed (reported) */
    /* What the power actions believe (temp_accept()) */
    int implausible;  /* the last reading was out of range (reported) */
    int has_last;     /* last: the last accepted reading, °C */
    long last;
    int pending;      /* polls of a jump from it not confirmed yet, at about pending_val */
    long pending_val;
    int over_shutdown, over_poweroff; /* polls in a row at or over the temperature */
    int answered, accepted; /* this poll */
};

struct cpu_sensor s_sensors[MAX_SENSORS];
int s_nsensors = 0;
long s_cpu_limit[MAX_CPUS]; /* from "hot"/"critical" trips of the device tree, -1 = none */
char *s_dtb = NULL;
struct stat s_dtb_stat;
int s_dtb_state = -1; /* 0 = sensors from device tree, else why not; -1 = read it again */

static struct cpu_sensor *find_sensor(int bus, int reg)
{
    for (int i = 0; i < s_nsensors; ++i)
    {
        if ((s_sensors[i].bus == bus) && (s_sensors[i].reg == reg)) return &s_sensors[i];
    }
    return NULL;
}

static void add_sensor(int bus, int reg, int hwmon_driver)
{
    /* Only 0x20..0x27 (CPU 0) ... 0x50..0x57 (CPU 3) */
    if ((reg < 0x20) || (reg > 0x57) || ((reg & 0x0f) > 7) || (s_nsensors >= MAX_SENSORS) || find_sensor(bus, reg)) return;
    struct cpu_sensor *s = &s_sensors[s_nsensors++];
    memset(s, 0, sizeof(*s));
    s->cpu = (reg >> 4) - 2;
    s->sensor = reg & 0x0f;
    s->bus = bus;
    s->reg = reg;
    s->hwmon_driver = hwmon_driver;
    s->hwmon = -1;
    dbg_printf(stdout, "CPU %d sensor %d: bus %d, 0x%02x, %s\n", s->cpu, s->sensor, bus, reg, hwmon_driver ? "l_pcs_i2c hwmon" : "direct");
}

static void add_pcs_node(int cpu)
{
    if ((cpu < 0) || (cpu >= MAX_CPUS) || (pcs_bus < 0)) return;
    for (int k = 0; k <= 7; ++k) add_sensor(pcs_bus, 0x20 + 0x10 * cpu + k, 0);
}

/* The lowest "hot" or "critical" trip of the thermal zones fed by phandle, in °C, or -1.
 * ("active" trips are the fan steps of the host, not limits.) */
static long trip_limit(const void *dtb, uint32_t phandle)
{
    long limit = -1;
    int zones = fdt_path_offset(dtb, "/thermal-zones");
    if (zones < 0) return -1;
    int zone;
    fdt_for_each_subnode(zone, dtb, zones)
    {
        int len;
        const fdt32_t *ts = fdt_getprop(dtb, zone, "thermal-sensors", &len);
        if ((ts == NULL) || (len < 4) || (fdt32_to_cpu(ts[0]) != phandle)) continue;
        int trips = fdt_subnode_offset(dtb, zone, "trips");
        if (trips < 0) continue;
        int trip;
        fdt_for_each_subnode(trip, dtb, trips)
        {
            int tlen;
            const char *type = fdt_getprop(dtb, trip, "type", &tlen);
            const fdt32_t *temp = fdt_getprop(dtb, trip, "temperature", &len);
            if ((type == NULL) || (tlen < 1) || type[tlen - 1] || (temp == NULL) || (len < 4)) continue;
            if (strcmp(type, "hot") && strcmp(type, "critical")) continue;
            long t = (int32_t)fdt32_to_cpu(*temp) / 1000;
            if ((limit < 0) || (t < limit)) limit = t;
        }
    }
    return limit;
}

static void parse_devtree(const void *dtb)
{
    /* Old format: l_pcs_i2c nodes on the buses of /bmc */
    int bmc = fdt_path_offset(dtb, "/bmc");
    for (int i2c = 1; (bmc >= 0) && (i2c <= 15); ++i2c)
    {
        char name[8];
        snprintf(name, sizeof(name), "i2c%d", i2c);
        int len;
        const char *path = fdt_getprop(dtb, bmc, name, &len);
        if ((path == NULL) || (len < 1) || path[len - 1]) continue;
        int bus = fdt_path_offset(dtb, path);
        if (bus < 0) continue;
        int node;
        fdt_for_each_subnode(node, dtb, bus)
        {
            int len;
            const char *compatible = fdt_getprop(dtb, node, "compatible", &len);
            const fdt32_t *reg = fdt_getprop(dtb, node, "reg", NULL);
            if ((compatible == NULL) || (len < 1) || compatible[len - 1] || (reg == NULL)) continue;
            const char *comma = strrchr(compatible, ',');
            if (strcmp(comma ? comma + 1 : compatible, "l_pcs_i2c")) continue;
            add_sensor(i2c - 1, fdt32_to_cpu(*reg), 1);
        }
    }

    /* Current format: pcs@N (CPU N, or its numa-node-id), thresholds from its thermal zone */
    int node;
    fdt_for_each_subnode(node, dtb, 0)
    {
        const char *name = fdt_get_name(dtb, node, NULL);
        if ((name == NULL) || strncmp(name, "pcs@", 4)) continue;
        int len;
        const fdt32_t *numa = fdt_getprop(dtb, node, "numa-node-id", &len);
        int cpu = (numa && (len >= 4)) ? (int)fdt32_to_cpu(*numa) : atoi(name + 4);
        add_pcs_node(cpu);
        uint32_t phandle = fdt_get_phandle(dtb, node);
        if ((cpu >= 0) && (cpu < MAX_CPUS) && phandle) s_cpu_limit[cpu] = trip_limit(dtb, phandle);
    }
}

static int same_file(const struct stat *a, const struct stat *b)
{
    return (a->st_ino == b->st_ino) && (a->st_size == b->st_size) && (a->st_mtime == b->st_mtime);
}

/* Re-read the sensor list when the device tree file appears, changes or goes */
static void update_sensors(void)
{
    struct stat st;
    int state = stat(DEVTREE_FILE, &st) ? 1 : 0;

    /* Unchanged since the last time (good or bad) */
    if ((s_dtb_state == 1) && (state == 1)) return;
    if ((s_dtb_state >= 0) && (s_dtb_state != 1) && !state && same_file(&st, &s_dtb_stat)) return;

    free(s_dtb);
    s_dtb = NULL;
    if (!state)
    {
        s_dtb_stat = st;
        if ((st.st_size <= 0) || (st.st_size > DEVTREE_MAX)) state = 2;
    }
    if (!state)
    {
        if ((s_dtb = malloc(st.st_size)) == NULL) reimu_cancel(-250, "Out of memory\n");
        int fd = open(DEVTREE_FILE, O_RDONLY | O_CLOEXEC);
        ssize_t len = 0;
        if (fd >= 0)
        {
            while (len < st.st_size)
            {
                ssize_t n = read(fd, s_dtb + len, st.st_size - len);
                if ((n < 0) && (errno == EINTR)) continue;
                if (n <= 0) break;
                len += n;
            }
            close(fd);
        }
        if ((fd < 0) || (len != st.st_size)) state = 3;
        else if (fdt_check_header(s_dtb) || (fdt_totalsize(s_dtb) > (uint32_t)st.st_size)) state = 4;
        if (state) { free(s_dtb); s_dtb = NULL; }
    }

    s_nsensors = 0;
    for (int cpu = 0; cpu < MAX_CPUS; ++cpu) s_cpu_limit[cpu] = -1;
    if (s_dtb) parse_devtree(s_dtb);
    if (s_nsensors == 0)
    {
        if (!state) state = 5;
        add_pcs_node(0);
    }

    const char *why[] = { "", "no device tree", "empty or too big device tree", "unreadable device tree", "malformed device tree", "no CPU sensors in device tree" };
    if (state) reimu_message(stderr, "(%s) Warning: %s %s, probing CPU 0 sensors on bus %ld\n", reimu_gettime(), why[state], DEVTREE_FILE, pcs_bus);
    else reimu_message(stderr, "(%s) %d CPU sensors from device tree %s\n", reimu_gettime(), s_nsensors, DEVTREE_FILE);
    s_dtb_state = state;
}

/* Find the hwmon of an l_pcs_i2c sensor. Its number changes each time the
 * device is bound again (e.g. after a host power cycle), so a known one is
 * checked to be still there. */
static void detect_hwmon(struct cpu_sensor *s)
{
    char path[1024];
    if (s->hwmon >= 0)
    {
        snprintf(path, 1023, "/sys/bus/i2c/devices/i2c-%d/%d-00%02x/hwmon/hwmon%ld/temp1_input", s->bus, s->bus, s->reg, s->hwmon);
        if (!reimu_chkfile(path)) return;
        dbg_printf(stdout, "Sensor %d:%02x is no longer hwmon%ld\n", s->bus, s->reg, s->hwmon);
        s->hwmon = -1;
    }

    snprintf(path, 1023, "/sys/bus/i2c/devices/i2c-%d/%d-00%02x/hwmon", s->bus, s->bus, s->reg);
    dbg_printf(stdout, "Trying to check %s\n", path);

    char *file = NULL;
    if (reimu_find_filename(0, path, &file)) return;
    dbg_printf(stdout, "Found file %s\n", file);
    if(memcmp(file, "hwmon", 5)) { free(file); return; }

    char *chwmon = file + 5;
    if(!*chwmon) { free(file); return; }

    char *chwmonend;
    long hwmon = strtol(chwmon, &chwmonend, 10);
    if(*chwmonend) { free(file); return; }
    free(file);
    dbg_printf(stdout, "Sensor %d:%02x is hwmon%ld\n", s->bus, s->reg, hwmon);

    s->hwmon = hwmon;
}

/* What a power-off may be decided on. A reading glitch must never power off a
 * working machine (the board has seen register corruption and -47 °C style
 * garbage): out of range is ignored, a jump counts only once it has lasted,
 * and an action takes LADDER_CONFIRM_POLLS polls in a row of LADDER_MIN_SENSORS
 * distinct sensors (one if the CPU has only one answering). */
#define TEMP_MIN 5
#define TEMP_MAX 150
#define TEMP_JUMP 30
#define LADDER_CONFIRM_POLLS 3
#define LADDER_MIN_SENSORS 2
#define TTRIP_CONFIRM_POLLS 3
#define TTRIP_HOT_TEMP 75
#define TTRIP_HOLD_SEC 30

static int temp_plausible(long temp)
{
    return (temp >= TEMP_MIN) && (temp <= TEMP_MAX);
}

/* A plausible reading: does it count yet? */
static int temp_accept(struct cpu_sensor *s, long temp)
{
    if (s->has_last && (labs(temp - s->last) > TEMP_JUMP))
    {
        s->pending = (s->pending && (labs(temp - s->pending_val) <= TEMP_JUMP)) ? s->pending + 1 : 1;
        s->pending_val = temp;
        if (s->pending < LADDER_CONFIRM_POLLS) return 0;
    }
    s->pending = 0;
    s->last = temp;
    s->has_last = 1;
    return 1;
}

/* Temperature in millidegrees: 0 = done, 1 = no sensor there, <0 = failure */
static int get_temperature(struct cpu_sensor *s, int *i2c_fd, int *i2c_fd_bus, long *result)
{
    if (s->hwmon_driver)
    {
        detect_hwmon(s);
        if (s->hwmon < 0) return 1;

        char path[1024];
        snprintf(path, 1023, "/sys/bus/i2c/devices/i2c-%d/%d-00%02x/hwmon/hwmon%ld/temp1_input", s->bus, s->bus, s->reg, s->hwmon);

        char ctemp[32], *ctempend;
        if (read_small_file(path, ctemp, sizeof(ctemp)) <= 0) { s->hwmon = -1; return -258; }

        long temp = strtol(ctemp, &ctempend, 10);
        if ((ctempend == ctemp) || (*ctempend && (*ctempend != '\n'))) return -259;
        /* The driver gives 0 when the sensor doesn't answer */
        if (!temp) return 1;
        *result = temp;
        return 0;
    }

    /* One /dev/i2c-N open per poll and bus */
    if (*i2c_fd_bus != s->bus)
    {
        if (*i2c_fd >= 0) close(*i2c_fd);
        char dev[32];
        snprintf(dev, sizeof(dev), "/dev/i2c-%d", s->bus);
        *i2c_fd = open(dev, O_RDWR | O_CLOEXEC);
        *i2c_fd_bus = s->bus;
    }
    if (*i2c_fd < 0) return -260;

    /* FORCE: the address may be taken by the l_pcs_i2c driver */
    unsigned char b[2];
    if (ioctl(*i2c_fd, I2C_SLAVE_FORCE, s->reg) < 0) return -261;
    if (read(*i2c_fd, b, 2) != 2) return s->seen ? -262 : 1;
    long raw = (b[0] << 8) | b[1];
    if (!raw) return 1;
    s->seen = 1;
    *result = raw * 125;
    return 0;
}

/* The power actions, the last resort when the CPU's own protection fails.
 *
 * Over CPU_SHUTDOWN_TEMP: a graceful shutdown, by the host agent (reimu-hostlink,
 * OsPower) if it's there, else a short press of the power button. Over
 * CPU_POWEROFF_TEMP, TTRIP# confirmed, or still over CPU_SHUTDOWN_TEMP after
 * GRACEFUL_TIMEOUT_SEC: forced off, a long press, until power good goes.
 *
 * A forced power-off leaves a marker in the rwfs, while it is there nothing
 * powers the host on automatically (allow_host_boot, phosphor-discover-system-
 * state); power on by hand clears it: host-poweron.service (web, Redfish, IPMI),
 * power good coming up without us (the front panel button, seen here), and
 * ClearEmergency (reimu-pnsd) without a power-on.
 *
 * Children do the work (server_ctl may wait for the PNS lock and holds the
 * button for 5 s: the watchdog must go on being fed), the state goes to /run so
 * that a restart goes on where it was: GRACEFUL_TIMEOUT_SEC counts from the
 * request, the button isn't pressed twice. It uses only what's on the rootfs:
 * neither bmcweb nor reimu-hostlink need to be up.
 * Without a CPLD (this board) TTRIP# doesn't exist: the temperatures stand alone. */
#define LADDER_STATE_DIR "/run/overheatd"
#define LADDER_STATE LADDER_STATE_DIR "/ladder"
#define EMERGENCY_DIR "/var/lib/overheatd"
#define EMERGENCY_FILE EMERGENCY_DIR "/emergency"
#define ACTION_TIMEOUT_SEC 45  /* server_ctl: the PNS lock 15 s, the press 5 s */
#define FORCE_VERIFY_SEC 3     /* power good after the press */
#define SELF_POWER_ON_SEC 30   /* power good this soon after a forced off, the marker still there: the
                                * platform by itself (seen once on the board after a forced off, 1 s
                                * later, cause unknown), a CPU that hasn't cooled: off again */
#define ACTION_SENT 100        /* exit code of a child: told the agent */
/* Critical in the journal (SyslogLevelPrefix) */
#define crit_printf(fmt, ...) reimu_message(stderr, "<2>(%s) " fmt "\n", reimu_gettime(), ##__VA_ARGS__)

enum { LADDER_IDLE, LADDER_GRACEFUL, LADDER_FORCING };
const char *const s_ladder_names[] = { "idle", "graceful", "forcing" };
int s_ladder = LADDER_IDLE;
long s_graceful_since = 0;  /* monotonic, as the state in /run: it survives a restart of the daemon */
pid_t s_action_pid = 0;
int s_action_force = 0;
long s_action_since = 0;
int s_action_rv = 0;        /* of the last child, -1: killed */
long s_action_done = 0;
long s_forced_off_at = 0;   /* the forced power-off seen */
int s_skipped_graceful = 0, s_skipped_force = 0;
int s_graceful_cooled = 0;
int s_polls_since_start = 0;
char s_emergency[512] = ""; /* the marker's content, one line */

static void ladder_save(void)
{
    char buf[48];
    mkdir(LADDER_STATE_DIR, 0755);
    snprintf(buf, sizeof(buf), "%d %ld %ld\n", s_ladder, s_graceful_since, s_forced_off_at);
    reimu_writefile(LADDER_STATE, buf, strlen(buf));
}

static void ladder_load(void)
{
    char buf[48];
    int state;
    long since, off_at;
    if ((read_small_file(LADDER_STATE, buf, sizeof(buf)) > 0) && (sscanf(buf, "%d %ld %ld", &state, &since, &off_at) == 3) && (state >= LADDER_IDLE) && (state <= LADDER_FORCING))
    {
        s_ladder = state;
        s_graceful_since = since;
        s_forced_off_at = off_at;
        if (state != LADDER_IDLE) crit_printf("Overheat: going on with the power actions after a restart (%s)", s_ladder_names[state]);
    }
}

/* The child of an action: its own process group, for the kill */
static void action_child(int force)
{
    setsid();
    if (!force)
    {
        pid_t p = fork();
        if (p == 0)
        {
            execl("/usr/bin/busctl", "busctl", "--timeout=10", "call", "xyz.openbmc_project.Reimu.HostLink", "/xyz/openbmc_project/reimu/hostlink",
                  "xyz.openbmc_project.Reimu.HostLink", "OsPower", "s", "poweroff", (char *)NULL);
            _exit(127);
        }
        int status;
        if ((p > 0) && (waitpid(p, &status, 0) == p) && WIFEXITED(status) && !WEXITSTATUS(status)) _exit(ACTION_SENT);
    }
    /* These look at power good first: the button of a host that is off would power it on */
    const char *cmd = force ? "/usr/bin/server_pwr_off_hard" : "/usr/bin/server_pwr_off";
    execl(cmd, cmd, (char *)NULL);
    _exit(127);
}

static void action_start(int force)
{
    pid_t p = fork();
    if (p < 0)
    {
        crit_printf("Overheat: can't start the power action: %s", strerror(errno));
        s_action_rv = -1;
        s_action_done = monotonic_sec();
        return;
    }
    if (p == 0) action_child(force);
    s_action_pid = p;
    s_action_force = force;
    s_action_since = monotonic_sec();
}

/* A press killed half way leaves the button driven low, held down (a line given
 * up keeps its direction; server_ctl floats it only at the end of the press):
 * reading it makes it an input again, nothing is pressed any more. Busy: someone
 * is pressing it, and that one ends it. */
static void button_release(void)
{
    reimu_get_gpio_by_name("GPIO_PWR_BTN");
}

static void action_reap(int kill_it)
{
    if (!s_action_pid) return;
    int status;
    if (kill_it || (monotonic_sec() - s_action_since > ACTION_TIMEOUT_SEC))
    {
        kill(-s_action_pid, SIGKILL);
        if (!kill_it) crit_printf("Overheat: the power action didn't end in %d s, killed", ACTION_TIMEOUT_SEC);
        waitpid(s_action_pid, &status, 0);
        s_action_rv = -1;
    }
    else
    {
        if (waitpid(s_action_pid, &status, WNOHANG) != s_action_pid) return;
        s_action_rv = WIFEXITED(status) ? WEXITSTATUS(status) : -1;
    }
    s_action_pid = 0;
    s_action_done = monotonic_sec();
    if (s_action_rv == -1) button_release();
    if (s_action_rv == ACTION_SENT) crit_printf("Overheat: graceful shutdown requested from the host agent (OsPower)");
    else if (!s_action_force && !s_action_rv) crit_printf("Overheat: graceful shutdown requested by the power button");
    else if (s_action_rv) crit_printf("Overheat: the %s failed (%d)", s_action_force ? "forced power-off" : "graceful shutdown", s_action_rv);
}

/* The other alert lines can't drive a power-off, they are only said (active low):
 * I2CM_ALERT#/ALERT_CPU# (14), I2C1_TCRIT#/TCRIT_SMBUS# (9), I2C1_ALERT#/ALERT_SMBUS# (8) */
static void log_alert_lines(uint32_t alerts)
{
    static uint32_t s_old = 0xffffffff;
    uint32_t now = alerts & 0x00004300;
    if (now == s_old) return;
    reimu_message(stderr, "(%s) Alert lines: ALERT_CPU# %s, TCRIT_SMBUS# %s, ALERT_SMBUS# %s (no action on these)\n", reimu_gettime(),
                  (now & 0x4000) ? "inactive" : "ASSERTED", (now & 0x0200) ? "inactive" : "ASSERTED", (now & 0x0100) ? "inactive" : "ASSERTED");
    s_old = now;
}

/* What we can't wait for and don't need to: a phosphor-logging entry (only the error log of the web) */
static void log_entry(const char *msg)
{
    pid_t p = fork();
    if (p == 0)
    {
        /* A grandchild, not ours to wait for */
        setsid();
        if (fork() != 0) _exit(0);
        execl("/usr/bin/busctl", "busctl", "--timeout=10", "call", "xyz.openbmc_project.Logging", "/xyz/openbmc_project/logging", "xyz.openbmc_project.Logging.Create", "Create",
              "ssa{ss}", msg, "xyz.openbmc_project.Logging.Entry.Level.Critical", "0", (char *)NULL);
        _exit(127);
    }
    if (p > 0) waitpid(p, NULL, 0);
}

static void emergency_write(const char *reason, const char *temps)
{
    char buf[512];
    snprintf(buf, sizeof(buf), "time=%s\nreason=%s\ntemps=%s\n", reimu_gettime(), reason, temps);
    mkdir(EMERGENCY_DIR, 0755);
    int fd = open(EMERGENCY_FILE ".new", O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
    int ok = (fd >= 0) && (write(fd, buf, strlen(buf)) == (ssize_t)strlen(buf)) && !fsync(fd);
    if (fd >= 0) close(fd);
    ok = ok && !rename(EMERGENCY_FILE ".new", EMERGENCY_FILE);
    fd = open(EMERGENCY_DIR, O_RDONLY | O_CLOEXEC);
    if (fd >= 0) { fsync(fd); close(fd); }
    /* It goes off all the same */
    if (!ok) crit_printf("Overheat: can't write the marker %s: the host may be powered on automatically", EMERGENCY_FILE);
}

/* The marker's content, as a line of the state file; returns whether it's there */
static int emergency_read(void)
{
    int n = read_small_file(EMERGENCY_FILE, s_emergency, sizeof(s_emergency));
    if (n < 0) { s_emergency[0] = '\0'; return 0; }
    for (char *c = s_emergency; *c; ++c)
    {
        if (*c == '\n') *c = ';';
        if ((*c == '<') || (*c == '>') || (*c == '&')) *c = ' ';
    }
    return 1;
}

static void ladder_force(const char *reason, const char *temps)
{
    action_reap(1);
    crit_printf("Overheat: FORCED POWER-OFF: %s (%s)", reason, temps);
    emergency_write(reason, temps);
    char msg[320];
    snprintf(msg, sizeof(msg), "Overheat emergency power-off: %s (%s)", reason, temps);
    log_entry(msg);
    s_ladder = LADDER_FORCING;
    ladder_save();
    action_start(1);
}

/* Called on every poll; power: GPIO_POWER_IN as read, <0: unknown (no action then).
 * shutdown, poweroff: confirmed over these temperatures; ttrip: confirmed. */
static void ladder_step(int power, int shutdown, int poweroff, int ttrip, const char *temps)
{
    static int s_old_power = -1;
    long now = monotonic_sec();
    action_reap(0);
    ++s_polls_since_start;

    if (power < 0) return;

    /* Power good came up. Anything automatic is held back while the marker is
     * there (see above), and a power-on through host-poweron.service has removed
     * it already: someone did it by hand (the button), unless the platform came
     * back by itself right after the forced off: that one is pressed again */
    if ((power == 1) && (s_old_power != 1) && s_emergency[0] && overheat_poweroff)
    {
        if (s_forced_off_at && (now - s_forced_off_at < SELF_POWER_ON_SEC))
        {
            crit_printf("Overheat: the host came back on by itself %ld s after the forced power-off: pressing again (%s)", now - s_forced_off_at, s_emergency);
            action_reap(1);
            s_ladder = LADDER_FORCING;
            s_action_rv = 0;
            s_action_done = now; /* the backoff of the FORCING state below */
            ladder_save();
        }
        else if (s_old_power == 0)
        {
            crit_printf("Overheat: the host was powered on by hand: the emergency marker is cleared (%s)", s_emergency);
            unlink(EMERGENCY_FILE);
        }
    }
    s_old_power = power;

    if (power == 0)
    {
        if (s_ladder == LADDER_FORCING)
        {
            crit_printf("Overheat: the host is off (power good is gone) after the forced power-off");
            s_forced_off_at = now;
        }
        /* (the press itself goes on: not again before it ends) */
        else if (s_ladder == LADDER_GRACEFUL) crit_printf("Overheat: the host is off after the graceful shutdown");
        if (s_ladder != LADDER_IDLE)
        {
            s_ladder = LADDER_IDLE;
            ladder_save();
        }
        s_skipped_graceful = s_skipped_force = s_graceful_cooled = 0;
        return;
    }

    const char *force_reason = NULL;
    char reason[96];
    if (ttrip) force_reason = "I2CM_TTRIP# asserted";
    else if (poweroff)
    {
        snprintf(reason, sizeof(reason), "CPU temperature at or over CPU_POWEROFF_TEMP %ld C", cpu_poweroff_temp);
        force_reason = reason;
    }

    if (!overheat_poweroff)
    {
        /* Turned off on purpose: said at each action it would take */
        if (force_reason && !s_skipped_force) crit_printf("Overheat: forced power-off SKIPPED (OVERHEAT_POWEROFF=no): %s (%s)", force_reason, temps);
        else if (shutdown && !force_reason && !s_skipped_graceful) crit_printf("Overheat: graceful shutdown SKIPPED (OVERHEAT_POWEROFF=no): CPU temperature at or over CPU_SHUTDOWN_TEMP %ld C (%s)", cpu_shutdown_temp, temps);
        s_skipped_force = (force_reason != NULL);
        s_skipped_graceful = shutdown;
        if (s_ladder != LADDER_IDLE)
        {
            action_reap(1);
            s_ladder = LADDER_IDLE;
            ladder_save();
        }
        return;
    }
    s_skipped_graceful = s_skipped_force = 0;

    switch (s_ladder)
    {
        case LADDER_IDLE:
            if (force_reason) ladder_force(force_reason, temps);
            else if (shutdown)
            {
                crit_printf("Overheat: graceful shutdown: CPU temperature at or over CPU_SHUTDOWN_TEMP %ld C (%s)", cpu_shutdown_temp, temps);
                s_ladder = LADDER_GRACEFUL;
                s_graceful_since = now;
                s_graceful_cooled = 0;
                ladder_save();
                action_start(0);
            }
            break;
        case LADDER_GRACEFUL:
        {
            long elapsed = now - s_graceful_since;
            if (force_reason) ladder_force(force_reason, temps);
            else if (shutdown)
            {
                s_graceful_cooled = 0;
                if (elapsed >= graceful_timeout_sec)
                {
                    snprintf(reason, sizeof(reason), "still over CPU_SHUTDOWN_TEMP %ld C after %ld s of graceful shutdown", cpu_shutdown_temp, elapsed);
                    ladder_force(reason, temps);
                }
                /* Not even asked (the button's GPIO busy...): again */
                else if (!s_action_pid && (s_action_rv != ACTION_SENT) && s_action_rv && !s_action_force && (now - s_action_done >= 5)) action_start(0);
            }
            else if (s_polls_since_start > LADDER_CONFIRM_POLLS * 2)
            {
                if (!s_graceful_cooled) crit_printf("Overheat: CPU temperature under CPU_SHUTDOWN_TEMP again (%s): no escalation (an OS shutdown already started goes on)", temps);
                s_graceful_cooled = 1;
                if (elapsed >= graceful_timeout_sec)
                {
                    s_ladder = LADDER_IDLE;
                    ladder_save();
                }
            }
            break;
        }
        case LADDER_FORCING:
            /* Until it's off: verified after each press */
            if (!s_action_pid && (now - s_action_done >= FORCE_VERIFY_SEC))
            {
                crit_printf("Overheat: power good still on after the forced power-off (%d): pressing again", s_action_rv);
                action_start(1);
            }
            break;
    }
}

/* What each sensor gave on this poll, for the journal of a blind state */
static const char *sensors_blind_summary(void)
{
    static char buf[320];
    int len = 0;
    buf[0] = '\0';
    for (int i = 0; (i < s_nsensors) && (len < (int)sizeof(buf) - 32); ++i)
    {
        const struct cpu_sensor *s = &s_sensors[i];
        const char *what = s->implausible ? "implausible" : s->failed ? "read failed" : "no answer";
        len += snprintf(buf + len, sizeof(buf) - len, "%scpu%d/%d (bus %d 0x%02x): %s", len ? ", " : "", s->cpu, s->sensor, s->bus, s->reg, what);
    }
    return len ? buf : "no sensors known";
}

/* The last accepted readings, for the journal and the marker */
static const char *sensors_summary(void)
{
    static char buf[256];
    int len = 0;
    buf[0] = '\0';
    for (int i = 0; (i < s_nsensors) && (len < (int)sizeof(buf) - 24); ++i)
    {
        if (s_sensors[i].accepted) len += snprintf(buf + len, sizeof(buf) - len, "%scpu%d/%d=%ldC", len ? " " : "", s_sensors[i].cpu, s_sensors[i].sensor, s_sensors[i].last);
    }
    return len ? buf : "no reading";
}

static const char *overheat_set_led_gpio(const char *trigger)
{
    /* "blind" (no CPU temperature known): blinks too, but a short flash every 2 s, not BLINK_MSEC on/off */
    int blind = !strcmp(trigger, "blind");
    char triggerlf[32];
    snprintf(triggerlf, sizeof(triggerlf), "%s\n", blind ? "timer" : trigger);

    const char *triggerfiles[] =
    {
        "/sys/class/leds/platform:red:overheat/trigger",
        "/sys/class/leds/platform:red:ohfanfail/trigger",
        "/sys/class/leds/platform:red:ohfanfail2/trigger",
        NULL
    };

    const char *delayfiles[] =
    {
        "/sys/class/leds/platform:red:overheat/delay_on",
        "/sys/class/leds/platform:red:ohfanfail/delay_on",
        "/sys/class/leds/platform:red:ohfanfail2/delay_on",
        "/sys/class/leds/platform:red:overheat/delay_off",
        "/sys/class/leds/platform:red:ohfanfail/delay_off",
        "/sys/class/leds/platform:red:ohfanfail2/delay_off",
        NULL
    };

    for(int trig = 0; triggerfiles[trig]; ++trig)
    {
        if(reimu_writefile(triggerfiles[trig], triggerlf, strlen(triggerlf))) return triggerfiles[trig];
    }

    if (!strcmp(trigger,"timer") || blind)
    {
        char msec[2][16];
        sprintf(msec[0], "%ld\n", blind ? 100L : blink_msec);  /* delay_on */
        sprintf(msec[1], "%ld\n", blind ? 1900L : blink_msec); /* delay_off */
        for(int trig = 0; delayfiles[trig]; ++trig)
        {
            const char *m = msec[trig >= 3];
            if(reimu_writefile(delayfiles[trig], m, strlen(m))) return delayfiles[trig];
        }
    }

    dbg_printf(stdout, "Changed LED state trigger to %s through sysfs\n", trigger);
    return NULL;
}

int is_atexit_tspi_led_fini = 0;
volatile int s_timer_enable = 0;
volatile int s_tspi_led = 0;
timer_t s_tspi_timer;
int s_tspi_timer_ready = 0;
int s_old_blink_msec = -1;
struct itimerspec s_blink_its = {{0, 0}, {0, 0}};

/* Called from the SIGALRM handler (nodbg = 1): no stdio, no malloc there */
static const char *overheat_control_led_tspi(int status, int nodbg)
{
    const char *ledfile = status ? "/sys/kernel/tinyspi/command_bits_reset" : "/sys/kernel/tinyspi/command_bits_set";
    if(write_small_file(ledfile, "0x00000040\n")) return ledfile;
    if (!nodbg) dbg_printf(stdout, "Changed LED state through %s\n", ledfile);
    return NULL;
}

static void tspi_led_handler(int sig __attribute__((unused)))
{
    int saved_errno = errno;
    if (s_timer_enable)
    {
        /* A failure is retried on the next tick */
        s_tspi_led = 1 - s_tspi_led;
        overheat_control_led_tspi(s_tspi_led, 1);
    }
    signal(SIGALRM, tspi_led_handler);
    errno = saved_errno;
}

static void tspi_led_fini(void)
{
    if (s_tspi_timer_ready) timer_delete(s_tspi_timer);
}

static int tspi_led_init(void)
{
    if (!s_tspi_timer_ready)
    {
        if (timer_create(CLOCK_MONOTONIC, NULL, &s_tspi_timer)) return -1;
        signal(SIGALRM, tspi_led_handler);
        reimu_set_atexit(&is_atexit_tspi_led_fini, tspi_led_fini);
        dbg_printf(stdout, "Created TinySPI LED timer\n");
        s_tspi_timer_ready = 1;
    }
    return 0;
}

static const char *overheat_set_led_tspi(const char *trigger)
{
    if (tspi_led_init()) return "TinySPI LED timer";
    if(!strcmp(trigger, "timer") || !strcmp(trigger, "blind"))
    {
        s_timer_enable = 1;
        /* Arm it when blinking starts for the first time or BLINK_MSEC changed */
        if (s_old_blink_msec != blink_msec)
        {
            if(timer_settime(s_tspi_timer, 0, &s_blink_its, NULL)) return "TinySPI LED timer";
            s_old_blink_msec = blink_msec;
        }
        dbg_printf(stdout, "Enabled TinySPI LED timer\n");
        return NULL;
    }
    else
    {
        dbg_printf(stdout, "Disabled TinySPI LED timer\n");
        s_timer_enable = 0;
        s_tspi_led = (!strcmp(trigger, "default-on")) ? 1 : 0;
        return overheat_control_led_tspi(s_tspi_led, 0);
    }
}

static int get_alerts_tinyspi(uint32_t *alerts)
{
    /* Reset TinySPI alert */
    if(reimu_writefile("/sys/kernel/tinyspi/command_bits_set", "00000010\n", 9)) return 3;
    reimu_msleep(10, &s_exit_req);
    if(reimu_writefile("/sys/kernel/tinyspi/command_bits_reset", "00000010\n", 9)) return 5;

    /* Read status register */
    char tinyspi_state[32], *endptr;
    if (read_small_file("/sys/kernel/tinyspi/state_reg", tinyspi_state, sizeof(tinyspi_state)) <= 0) return 64;

    uint32_t res = strtoul(tinyspi_state, &endptr, 16);
    if ((endptr == tinyspi_state) || (*endptr != '\n')) return 65;
    *alerts = res;
    return 0;
}

static uint32_t get_alerts_gpio(void)
{
    /* TinySPI has the following assignment, so we try to adopt it:
     * INTRUSION_SW# = bit 4  = INTRUSION_SW#
     * I2C0_ALERT#   = bit 6  = ALERT_MEM#
     * I2C1_ALERT#   = bit 8  = ALERT_SMBUS#
     * I2C1_TCRIT#   = bit 9  = TCRIT_SMBUS#
     * I2C1_FAULT#   = bit 10 = ALERT_FRU#
     * I2C2_ALERT#   = bit 11 = ALERT_PCIE#
     * I2C3_ALERT#   = bit 12 = ALERT_CPU2#
     * I2CM_ALERT#   = bit 14 = ALERT_CPU#
     * PWROK_MAIN    = bit 21 = PWROK_ATX
     * APMDZ_LED#    = bit 23 = APMDZ_LED#
     * GPI[7] (RFU)  = bit 31 = PLT_RST#
     */

    static const char *names[32] =
    {
        [31] = "GPIO_RESET_IN", [23] = "GPIO_APMDZ_LED", [21] = "GPIO_POWER_IN", [14] = "GPIO_ALERT_1",
        [12] = "GPIO_ALERT_2",  [6]  = "GPIO_ALERT_4",   [11] = "GPIO_ALERT_3",  [10] = "GPIO_ALERT_7",
        [8]  = "GPIO_ALERT_5",  [9]  = "GPIO_ALERT_6",   [4]  = "GPIO_INTRUSION",
        [15] = "GPIO_TTRIP",
    };
    static uint32_t s_failed_gpios = 0;

    uint32_t alerts = 0, failed = 0;
    for (int i = 0; i < 32; ++i)
    {
        if (names[i] == NULL) continue;
        int b = reimu_get_gpio_by_name(names[i]);
        /* I2CM_TTRIP#: boards without CPLD don't have it ("always 1" in the REIMU docs; with
         * a CPLD it's in the TinySPI status register), read only if the gpiotab names a line
         * for it (the QEMU test does); -2: not in the gpiotab */
        if ((i == 15) && (b == -2)) b = 1;
        /* Unreadable (e.g. busy: exported to sysfs by checkalerts): taken as high = inactive */
        if (b < 0) { failed |= (1u << i); b = 1; }
        alerts |= ((uint32_t)(b & 1) << i);
    }
    if (failed != s_failed_gpios)
    {
        if (failed) reimu_message(stderr, "(%s) Warning: can't acquire alert GPIOs 0x%08x, taken as inactive\n", reimu_gettime(), failed);
        else reimu_message(stderr, "(%s) Alert GPIOs are readable again\n", reimu_gettime());
        s_failed_gpios = failed;
    }

    return alerts | 0x001020a5; /* (bit 15, I2CM_TTRIP#, is read: GPIO_TTRIP above) */
}

volatile int s_dbg_state = 0; /* 0 = normal state; 1 = slight overheat; 2 = critical overheat */

static void set_dbg_state(int sig __attribute__((unused)))
{
    s_dbg_state = (s_dbg_state + 1) % 3;
    signal(SIGUSR1, set_dbg_state);
}

/* An optional value: not there = default */
static long get_conf_long_opt(const char *cfg, int len, const char *name, long def)
{
    long v = reimu_get_conf_long(cfg, len, name);
    return (v == -1) ? def : v;
}

static void reload_config(const char *configfile)
{
    static int s_config_bad = 0;
    if (reimu_chkfile(configfile))
    {
        /* If no configfile exist, create it there */

        const char *defaultconfig =
            "OVERHEATD_ENABLED=yes\n"
            "DEBUG=off\n"
            "CPU_TEMP_LIMIT=85\n"
            "WAIT_MSEC=5000\n"
            "BLINK_MSEC=500\n"
            "CPU_SHUTDOWN_TEMP=90\n"
            "CPU_POWEROFF_TEMP=95\n"
            "GRACEFUL_TIMEOUT_SEC=60\n"
            "OVERHEAT_POWEROFF=yes\n"
            "BLIND_ALARM_MIN=10\n";

        if(reimu_writefile(configfile, defaultconfig, strlen(defaultconfig)) && !s_config_bad) reimu_message(stderr, "(%s) Can't create new config %s\n", reimu_gettime(), configfile);
    }

    /* No malloc, and NUL-terminated (the parsers strtol() past a last line without LF otherwise) */
    char daemoncfg[4096];
    int daemoncfg_len = read_small_file(configfile, daemoncfg, sizeof(daemoncfg));
    const char *err = NULL;
    int new_enabled = 0, new_debug = 0;
    long new_limit = 0, new_wait = 0, new_blink = 0, new_pcs_bus = 0, new_settle = 0;

    if ((daemoncfg_len <= 0) || (daemoncfg_len >= (int)sizeof(daemoncfg) - 1))
    {
        err = "Unable to read config file";
    }
    else
    {
        new_enabled = reimu_get_conf_bool(daemoncfg, daemoncfg_len, "OVERHEATD_ENABLED");
        new_debug   = reimu_get_conf_bool(daemoncfg, daemoncfg_len, "DEBUG");
        new_limit   = reimu_get_conf_long(daemoncfg, daemoncfg_len, "CPU_TEMP_LIMIT");
        new_wait    = reimu_get_conf_long(daemoncfg, daemoncfg_len, "WAIT_MSEC");
        new_blink   = reimu_get_conf_long(daemoncfg, daemoncfg_len, "BLINK_MSEC");
        new_pcs_bus = get_conf_long_opt(daemoncfg, daemoncfg_len, "PCS_BUS", 5);
        new_settle  = get_conf_long_opt(daemoncfg, daemoncfg_len, "PCS_SETTLE_SEC", 30);
        if((new_enabled < 0) || (new_debug < 0) || (new_limit < 0) || (new_wait < 0) || (new_blink < 0) || (new_pcs_bus < 0) || (new_settle < 0))
        {
            err = "Malformed values in";
        }
    }

    /* Keep protecting with the last good (or default) settings */
    if (err)
    {
        if (!s_config_bad) reimu_message(stderr, "(%s) %s %s, keeping the previous settings\n", reimu_gettime(), err, configfile);
        s_config_bad = 1;
        return;
    }
    if (s_config_bad) reimu_message(stderr, "(%s) Config %s is read again\n", reimu_gettime(), configfile);
    s_config_bad = 0;

    overheatd_enabled = new_enabled;
    debug             = new_debug;
    cpu_temp_limit    = new_limit;
    wait_msec         = new_wait;
    blink_msec        = new_blink;
    pcs_settle_sec    = new_settle;
    if (pcs_bus != new_pcs_bus)
    {
        pcs_bus = new_pcs_bus;
        s_dtb_state = -1; /* re-read the sensor list */
        free(s_dtb);
        s_dtb = NULL;
    }

    /* The power actions: POWEROFF > SHUTDOWN >= LIMIT, or the defaults for all
     * three; a malformed OVERHEAT_POWEROFF: they stay on */
    static int s_ladder_bad = 0, s_old_poweroff = 1;
    long new_shutdown = get_conf_long_opt(daemoncfg, daemoncfg_len, "CPU_SHUTDOWN_TEMP", 90);
    long new_off      = get_conf_long_opt(daemoncfg, daemoncfg_len, "CPU_POWEROFF_TEMP", 95);
    long new_timeout  = get_conf_long_opt(daemoncfg, daemoncfg_len, "GRACEFUL_TIMEOUT_SEC", 60);
    int new_poweroff  = reimu_get_conf_bool(daemoncfg, daemoncfg_len, "OVERHEAT_POWEROFF");
    long new_blind    = get_conf_long_opt(daemoncfg, daemoncfg_len, "BLIND_ALARM_MIN", 10);
    blind_alarm_min   = ((new_blind >= 1) && (new_blind <= 1440)) ? new_blind : 10;
    int bad = (new_shutdown < new_limit) || (new_off <= new_shutdown) || !temp_plausible(new_shutdown) || !temp_plausible(new_off) || (new_timeout < 1) || (new_timeout > 3600) || (new_poweroff == -2);
    if (bad)
    {
        if (!s_ladder_bad) reimu_message(stderr, "(%s) Bad CPU_SHUTDOWN_TEMP (%ld), CPU_POWEROFF_TEMP (%ld), GRACEFUL_TIMEOUT_SEC (%ld) or OVERHEAT_POWEROFF in %s: the defaults 90, 95, 60, yes\n", reimu_gettime(), new_shutdown, new_off, new_timeout, configfile);
        new_shutdown = 90;
        new_off = 95;
        new_timeout = 60;
        if (new_poweroff == -2) new_poweroff = 1;
    }
    s_ladder_bad = bad;
    cpu_shutdown_temp = new_shutdown;
    cpu_poweroff_temp = new_off;
    graceful_timeout_sec = new_timeout;
    overheat_poweroff = (new_poweroff != 0);
    if (overheat_poweroff != s_old_poweroff)
    {
        if (overheat_poweroff) crit_printf("Overheat: the power actions are ON again (OVERHEAT_POWEROFF)");
        else crit_printf("Overheat: the power actions are OFF (OVERHEAT_POWEROFF=no): the BMC won't power the host off on overheat, the alarm only");
        s_old_poweroff = overheat_poweroff;
    }

    s_blink_its.it_value.tv_sec  = blink_msec / 1000L;
    s_blink_its.it_value.tv_nsec = (blink_msec % 1000L) * 1000000L;
    s_blink_its.it_interval = s_blink_its.it_value;
}

static void set_signal_handlers(void)
{
    signal(SIGTERM, exit_handler);
    signal(SIGHUP, exit_handler);
    signal(SIGINT, exit_handler);
    signal(SIGUSR1, set_dbg_state);
}

static void create_pidfile(void)
{
    char pid[16];
    sprintf(pid, "%d\n", getpid());
    reimu_set_atexit(NULL, rm_pidfile);
    if(reimu_writefile("/run/overheatd.pid", pid, strlen(pid))) reimu_message(stderr, "Can't create pid file\n");
}

static int detect_tinyspi(void)
{
    if (!reimu_find_in_file("/etc/reimu.conf", "TINYSPI=yes") || !reimu_find_in_file("/var/volatile/reimu.conf", "TINYSPI=yes") || !reimu_find_in_file("/var/volatile/boardid", "1111"))
    {
        /* Not an exit: systemd would start it again and again, unprotected */
        if(reimu_chkdir("/sys/kernel/tinyspi/"))
        {
            reimu_message(stderr, "(%s) Error: TinySPI is enabled, but no API files exported by kernel: using the GPIOs\n", reimu_gettime());
            return 0;
        }
        return 1;
    }
    return 0;
}

int main(int argc, char *argv[])
{
    const char *led_trigger = "";
    uint32_t alerts = 0;
    int tinyspi_alert = 2; /* Unconditionally acquire TinySPI data */
    int overheat_cond = 0;
    int last_dbg_state = 0;
    int led_failed = 0, state_failed = 0, no_sensors = 0, was_powered_on = 0, over_limit = 0;
    int ttrip_polls = 0, fast_poll = 0, one_sensor_said = 0;
    long ttrip_since = 0, blind_since = 0, blind_said = 0;
    long powered_on_since = 0;
    unsigned long poll = 0;
    set_signal_handlers();
    create_pidfile();
    if (reimu_textfile_buf_alloc()) reimu_cancel(-250, "Out of memory\n");
    int tinyspi = detect_tinyspi();
    /* (a press of the last run goes on: KillMode=process, then it's busy here) */
    button_release();
    ladder_load();
    if (emergency_read()) crit_printf("Overheat: emergency marker %s: no automatic power-on of the host (%s)", EMERGENCY_FILE, s_emergency);
    notify_systemd("READY=1");

    for(;;)
    {
        fast_poll = 0;
        watchdog_ping();
        reload_config("/etc/overheatd.conf");
        dbg_printf(stdout, "Reloaded config: (OVERHEATD_ENABLED = %d, DEBUG = %d; CPU_TEMP_LIMIT = %ld; WAIT_MSEC = %ld; BLINK_MSEC = %ld; PCS_BUS = %ld; PCS_SETTLE_SEC = %ld)\n", overheatd_enabled, debug, cpu_temp_limit, wait_msec, blink_msec, pcs_bus, pcs_settle_sec);

        if(overheatd_enabled)
        {
            /* poll: counts up while the daemon works, for whoever watches the file */
            if(reimu_textfile_buf_append("<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n<state date=\"%s\" poll=\"%lu\">\n", reimu_gettime(), ++poll)) reimu_cancel(-250, "Out of memory\n");

            if (last_dbg_state != s_dbg_state)
            {
                tinyspi_alert = 2;
                last_dbg_state = s_dbg_state;
            }

            int powered_on;
            const char *oldledtrigger = led_trigger;
            led_trigger = "none";

            /* Check current power state */
            for(int i = 4; i >= 0; --i)
            {
                if(((powered_on = reimu_get_gpio_by_name("GPIO_POWER_IN")) < 0) && (i > 0))
                {
                    reimu_message(stderr, "(%s) Warning: GPIO_POWER_IN is unavailable, retrying in 100 ms, %d attempts remaining\n", reimu_gettime(), i);
                    reimu_msleep(100, &s_exit_req);
                    continue;
                }
                break;
            }
            int alerts_valid = 1;
            int real_power = powered_on; /* <0: no power action, the button could power it on */
            if (powered_on < 0)
            {
                /* Better check the sensors in vain than not at all */
                reimu_message(stderr, "(%s) Can't acquire GPIO_POWER_IN, taking the host as powered on\n", reimu_gettime());
                powered_on = 1;
            }
            if (powered_on && !was_powered_on) powered_on_since = monotonic_sec();
            const char *status = powered_on ? "settling" : "host-off";
            was_powered_on = powered_on;

            /* Re-read alerts, if needed */
            if (tinyspi)
            {
                int check = (tinyspi_alert == 2);
                if ((tinyspi_alert = reimu_get_gpio_by_name("GPIO_TINYSPI_ALERT")) < 0)
                {
                    reimu_message(stderr, "(%s) Can't acquire TinySPI alert, re-reading alerts\n", reimu_gettime());
                    check = 1;
                    tinyspi_alert = 2;
                }

                if(check || !tinyspi_alert)
                {
                    dbg_printf(stdout, "Re-reading alerts (tinyspi_alert = %d, old alerts = 0x%08x)\n", tinyspi_alert, alerts);
                    int rv = get_alerts_tinyspi(&alerts);
                    if (rv)
                    {
                        reimu_message(stderr, "(%s) Can't read TinySPI alerts (error %d), keeping 0x%08x\n", reimu_gettime(), rv, alerts);
                        tinyspi_alert = 2;
                        alerts_valid = 0;
                    }
                }
            }
            else
            {
                dbg_printf(stdout, "Re-reading alerts (old alerts = 0x%08x)\n", alerts);
                alerts = get_alerts_gpio();
            }

            dbg_printf(stdout, "Alerts: 0x%08x\n", alerts);
            if(reimu_textfile_buf_append("\t<alerts>0x%08x</alerts>\n", alerts)) reimu_cancel(-250, "Out of memory\n");

            /* (before the simulated states of SIGUSR1: testing the LED must never cost the host.
             * They only reach the LED and the status; the power actions look at real readings) */
            int ttrip_low = alerts_valid && !(alerts & 0x00008000) && (real_power == 1);
            if (alerts_valid) log_alert_lines(alerts);
            if (s_dbg_state == 2) { alerts &= 0xffff7fff; dbg_printf(stdout, "DEBUG MODE: alerts altered to 0x%08x\n", alerts); }

            /* THERMAL_SHDN# (bit 0) == 0 : overheat on */
            if ((alerts & 0x00000001) == 0)
            {
                led_trigger = "default-on";
                overheat_cond = 1;
            }

            /* I2CM_TTRIP# (bit 15) == 0 & PWROK_MAIN (bit 21) == 1 : overheat on */
            if ((alerts & 0x00200000) != 0)
            {
                if ((alerts & 0x00008000) == 0)
                {
                    led_trigger = "default-on";
                    overheat_cond = 1;
                }
                else
                {
                    overheat_cond = 0;
                }
            }
            dbg_printf(stdout, "After checking TTRIP: led_trigger: %s, overheat_cond: %d\n", led_trigger, overheat_cond);

            /* I2CM_TTRIP# (bit 15) == 0 & PWROK_MAIN (bit 21) == 0, but overheat condition is reached : overheat on */
            if ((alerts & 0x00008000) == 0)
            {
                if ((alerts & 0x00200000) == 0)
                {
                    led_trigger = overheat_cond ? "default-on" : "none";
                }
            }
            dbg_printf(stdout, "led_trigger after checking overheat_cond: %s\n", led_trigger);

            /* Host powered off: its sensors are unpowered; read them again from scratch next time */
            if (!powered_on)
            {
                for (int i = 0; i < s_nsensors; ++i)
                {
                    s_sensors[i].seen = s_sensors[i].has_last = s_sensors[i].pending = s_sensors[i].accepted = 0;
                    s_sensors[i].over_shutdown = s_sensors[i].over_poweroff = 0;
                }
            }
            int confirmed_shutdown = 0, confirmed_poweroff = 0;
            long hot_temp = 0;

            /* Powered on (for a while: its power-up must not see BMC traffic on the bus), so check I2C */
            if(powered_on && (monotonic_sec() - powered_on_since >= pcs_settle_sec))
            {
                update_sensors();

                int cpus[MAX_CPUS] = {0, 0, 0, 0};
                int monitored = 0;
                int i2c_fd = -1, i2c_fd_bus = -1;
                long maxover = -1000; /* the highest temperature above its limit, °C */
                long maxtemp = 0;

                /* Check temperature of each cpu sensor */
                for (int i = 0; i < s_nsensors; ++i)
                {
                    struct cpu_sensor *s = &s_sensors[i];
                    long temp;
                    int rv = get_temperature(s, &i2c_fd, &i2c_fd_bus, &temp);
                    watchdog_ping();
                    s->answered = s->accepted = 0;
                    if (rv != 0) s->over_shutdown = s->over_poweroff = 0;
                    if (rv < 0)
                    {
                        if (!s->failed) reimu_message(stderr, "(%s) Failure reading CPU sensor (cpu = %d, sensor = %d, error %d)\n", reimu_gettime(), s->cpu, s->sensor, rv);
                        s->failed = 1;
                        continue;
                    }
                    if (s->failed) reimu_message(stderr, "(%s) CPU sensor (cpu = %d, sensor = %d) is readable again\n", reimu_gettime(), s->cpu, s->sensor);
                    s->failed = 0;
                    if (rv > 0) continue;

                    double tempdbl = temp / 1000.0;
                    temp /= 1000;
                    dbg_printf(stdout, "Temp (%d:%d): %ld (%f)\n", s->cpu, s->sensor, temp, tempdbl);
                    s->answered = 1;

                    /* Garbage: not a temperature at all */
                    if (!temp_plausible(temp))
                    {
                        if (!s->implausible) reimu_message(stderr, "(%s) Warning: implausible reading of CPU sensor (cpu = %d, sensor = %d): %ld C, ignored\n", reimu_gettime(), s->cpu, s->sensor, temp);
                        s->implausible = 1;
                        s->pending = s->over_shutdown = s->over_poweroff = 0;
                        if(reimu_textfile_buf_append("\t<implausible cpu=\"%d\" sensor=\"%d\">%f</implausible>\n", s->cpu, s->sensor, tempdbl)) reimu_cancel(-250, "Out of memory\n");
                        continue;
                    }
                    s->implausible = 0;

                    if(reimu_textfile_buf_append("\t<temperature cpu=\"%d\" sensor=\"%d\">%f</temperature>\n", s->cpu, s->sensor, tempdbl)) reimu_cancel(-250, "Out of memory\n");
                    cpus[s->cpu] = 1;
                    ++monitored;

                    long limit = cpu_temp_limit;
                    if ((s_cpu_limit[s->cpu] >= 0) && (s_cpu_limit[s->cpu] < limit)) limit = s_cpu_limit[s->cpu];
                    if (temp > limit) fast_poll = 1; /* Confirm it soon, whether it's real or not */

                    /* A jump from the last reading counts once it has lasted */
                    s->accepted = temp_accept(s, temp);
                    if (!s->accepted)
                    {
                        s->over_shutdown = s->over_poweroff = 0;
                        continue;
                    }
                    if (temp > maxtemp) maxtemp = temp;
                    if (temp - limit > maxover) maxover = temp - limit;
                    s->over_shutdown = (temp >= cpu_shutdown_temp) ? s->over_shutdown + 1 : 0;
                    s->over_poweroff = (temp >= cpu_poweroff_temp) ? s->over_poweroff + 1 : 0;
                }
                if (i2c_fd >= 0) close(i2c_fd);

                /* A CPU is over a temperature when 2 of its sensors say so, polls in a row
                 * (or its only one that answers; an answer of garbage is an answer) */
                int any_over = 0;
                for (int cpu = 0; cpu < MAX_CPUS; ++cpu)
                {
                    int answering = 0, over_s = 0, over_p = 0;
                    for (int i = 0; i < s_nsensors; ++i)
                    {
                        struct cpu_sensor *s = &s_sensors[i];
                        if (s->cpu != cpu) continue;
                        answering += s->answered;
                        over_s += (s->over_shutdown >= LADDER_CONFIRM_POLLS);
                        over_p += (s->over_poweroff >= LADDER_CONFIRM_POLLS);
                    }
                    int need = (answering >= LADDER_MIN_SENSORS) ? LADDER_MIN_SENSORS : 1;
                    if (over_s >= need) confirmed_shutdown = 1;
                    if (over_p >= need) confirmed_poweroff = 1;
                    if (over_s) any_over = 1;
                    if (over_s && !one_sensor_said)
                    {
                        if ((over_s >= need) && (answering == 1)) reimu_message(stderr, "(%s) Warning: CPU %d has one sensor answering: acting on it alone\n", reimu_gettime(), cpu);
                        if (over_s < need) reimu_message(stderr, "(%s) Warning: one sensor of CPU %d over CPU_SHUTDOWN_TEMP, the others aren't: unconfirmed, no action (%s)\n", reimu_gettime(), cpu, sensors_summary());
                        one_sensor_said = (answering == 1) || (over_s < need);
                    }
                }
                if (!any_over) one_sensor_said = 0;
                hot_temp = maxtemp;

                dbg_printf(stdout, "CPUs detected: %d, %d, %d, %d\n", cpus[0], cpus[1], cpus[2], cpus[3]);
                if(reimu_textfile_buf_append("\t<cpus>")) reimu_cancel(-250, "Out of memory\n");
                int first = 1;
                for (int cpu = 0; cpu < MAX_CPUS; ++cpu)
                {
                    if (cpus[cpu])
                    {
                        if(reimu_textfile_buf_append(first ? "%d" : " %d", cpu)) reimu_cancel(-250, "Out of memory\n");
                        first = 0;
                    }
                }
                if(reimu_textfile_buf_append("</cpus>\n")) reimu_cancel(-250, "Out of memory\n");

                dbg_printf(stdout, "maxtemp: %ld\n", maxtemp);
                if (s_dbg_state == 1) { maxover = 1; dbg_printf(stdout, "DEBUG MODE: maxtemp altered to over the limit\n"); }

                /* Any of temperatures is greater than limit */
                status = "ok";
                if (maxover > 0)
                {
                    if (!over_limit) reimu_message(stderr, "(%s) Warning: CPU temperature %ld °C is %ld °C over the limit\n", reimu_gettime(), maxtemp, maxover);
                    if (!strcmp(led_trigger, "none")) led_trigger = "timer";
                    status = "over-limit";
                }
                else if (over_limit) reimu_message(stderr, "(%s) CPU temperatures are under the limit again (max %ld °C)\n", reimu_gettime(), maxtemp);
                over_limit = (maxover > 0);

                /* The host runs, but no CPU temperature is known: say so loudly, and blink otherwise than for an overheat */
                if (!monitored)
                {
                    long now = monotonic_sec();
                    if (!no_sensors)
                    {
                        crit_printf("Overheat: the host is powered on, but no CPU temperature sensor is readable: overheat protection is blind, no power action on temperature (%s)", sensors_blind_summary());
                        blind_since = blind_said = now;
                    }
                    else if (now - blind_said >= blind_alarm_min * 60)
                    {
                        crit_printf("Overheat: still blind after %ld min: no CPU temperature, no power action on temperature (%s)", (now - blind_since) / 60, sensors_blind_summary());
                        blind_said = now;
                    }
                    if(reimu_textfile_buf_append("\t<error>no CPU temperature sensor readable</error>\n")) reimu_cancel(-250, "Out of memory\n");
                    if (!strcmp(led_trigger, "none")) led_trigger = "blind";
                    status = "blind";
                }
                else if (no_sensors) reimu_message(stderr, "(%s) CPU temperature sensors are readable again (%d)\n", reimu_gettime(), monitored);
                no_sensors = !monitored;
                if (!monitored && reimu_textfile_buf_append("\t<blind sec=\"%ld\" alarm=\"%d\"/>\n", monotonic_sec() - blind_since, monotonic_sec() - blind_since >= blind_alarm_min * 60)) reimu_cancel(-250, "Out of memory\n");
            }
            if (!strcmp(led_trigger, "default-on")) status = "critical";
            /* ok, over-limit, blind (no CPU temperature), critical (alert lines), host-off, settling (just powered on) */
            if(reimu_textfile_buf_append("\t<status>%s</status>\n", status)) reimu_cancel(-250, "Out of memory\n");

            /* TTRIP#: some polls with a hot sensor, or held on its own */
            int ttrip_confirmed = 0;
            if (ttrip_low)
            {
                if (!ttrip_polls++)
                {
                    ttrip_since = monotonic_sec();
                    crit_printf("Overheat: I2CM_TTRIP# asserted, to be confirmed (%s)", sensors_summary());
                }
                ttrip_confirmed = ((ttrip_polls >= TTRIP_CONFIRM_POLLS) && (hot_temp >= TTRIP_HOT_TEMP)) || (monotonic_sec() - ttrip_since >= TTRIP_HOLD_SEC);
                fast_poll = 1;
            }
            else ttrip_polls = 0;

            emergency_read();
            ladder_step(real_power, confirmed_shutdown, confirmed_poweroff, ttrip_confirmed, sensors_summary());
            emergency_read();
            if (s_ladder != LADDER_IDLE) fast_poll = 1;
            if(reimu_textfile_buf_append("\t<protection enabled=\"%s\" state=\"%s\" limit=\"%ld\" shutdown=\"%ld\" poweroff=\"%ld\" timeout=\"%ld\"/>\n", overheat_poweroff ? "yes" : "no", s_ladder_names[s_ladder], cpu_temp_limit, cpu_shutdown_temp, cpu_poweroff_temp, graceful_timeout_sec)) reimu_cancel(-250, "Out of memory\n");
            if (s_emergency[0] && reimu_textfile_buf_append("\t<emergency>%s</emergency>\n", s_emergency)) reimu_cancel(-250, "Out of memory\n");

            /* Update LEDs, if needed */
            dbg_printf(stdout, "LED trigger: %s -> %s\n", oldledtrigger, led_trigger);
            if (strcmp(oldledtrigger, led_trigger))
            {
                const char *failed = tinyspi ? overheat_set_led_tspi(led_trigger) : overheat_set_led_gpio(led_trigger);
                if (failed)
                {
                    if (!led_failed) reimu_message(stderr, "(%s) Can't set LED to %s (%s), retrying\n", reimu_gettime(), led_trigger, failed);
                    led_trigger = ""; /* Not set: try again on the next poll */
                }
                else if (led_failed) reimu_message(stderr, "(%s) LED is set again\n", reimu_gettime());
                led_failed = (failed != NULL);
            }

            /* Finalize statefile */
            if(reimu_textfile_buf_append("</state>\n")) reimu_cancel(-250, "Out of memory\n");
            /* Written aside and renamed: a reader never sees it half written */
            if(reimu_textfile_buf_commit("/var/volatile/systemstate.xml.new") || rename("/var/volatile/systemstate.xml.new", "/var/volatile/systemstate.xml"))
            {
                if (!state_failed) reimu_message(stderr, "(%s) Can't update statefile\n", reimu_gettime());
                state_failed = 1;
                /* Not committed, so not emptied either: empty it, or it grows by a state every poll */
                if (reimu_textfile_buf_alloc()) reimu_cancel(-250, "Out of memory\n");
            }
            else state_failed = 0;
        }

        /* Sleep till next cycle, in slices to keep the watchdog fed whatever WAIT_MSEC is
         * (and a SIGTERM that came while working doesn't interrupt a sleep) */
        for (long left = (fast_poll && (wait_msec > 1000)) ? 1000 : wait_msec; left > 0; left -= 1000)
        {
            if (s_exit_req) exit(0);
            reimu_msleep((left > 1000) ? 1000 : left, &s_exit_req);
            watchdog_ping();
        }
        if (s_exit_req) exit(0);
    }
}
