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
    };
    static uint32_t s_failed_gpios = 0;

    uint32_t alerts = 0, failed = 0;
    for (int i = 0; i < 32; ++i)
    {
        if (names[i] == NULL) continue;
        int b = reimu_get_gpio_by_name(names[i]);
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

    return alerts | 0x0010a0a5;
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
            "BLINK_MSEC=500\n";

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
    long powered_on_since = 0;
    unsigned long poll = 0;
    set_signal_handlers();
    create_pidfile();
    if (reimu_textfile_buf_alloc()) reimu_cancel(-250, "Out of memory\n");
    int tinyspi = detect_tinyspi();
    notify_systemd("READY=1");

    for(;;)
    {
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
                for (int i = 0; i < s_nsensors; ++i) s_sensors[i].seen = 0;
            }

            /* We are not in critical overheat, and powered on (for a while: its power-up must not see BMC traffic on the bus), so check I2C */
            if(!strcmp(led_trigger, "none") && powered_on && (monotonic_sec() - powered_on_since >= pcs_settle_sec))
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
                    if(reimu_textfile_buf_append("\t<temperature cpu=\"%d\" sensor=\"%d\">%f</temperature>\n", s->cpu, s->sensor, tempdbl)) reimu_cancel(-250, "Out of memory\n");
                    cpus[s->cpu] = 1;
                    ++monitored;
                    if (temp > maxtemp) maxtemp = temp;

                    long limit = cpu_temp_limit;
                    if ((s_cpu_limit[s->cpu] >= 0) && (s_cpu_limit[s->cpu] < limit)) limit = s_cpu_limit[s->cpu];
                    if (temp - limit > maxover) maxover = temp - limit;
                }
                if (i2c_fd >= 0) close(i2c_fd);

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
                    led_trigger = "timer";
                    status = "over-limit";
                }
                else if (over_limit) reimu_message(stderr, "(%s) CPU temperatures are under the limit again (max %ld °C)\n", reimu_gettime(), maxtemp);
                over_limit = (maxover > 0);

                /* The host runs, but no CPU temperature is known: say so loudly, and blink otherwise than for an overheat */
                if (!monitored)
                {
                    if (!no_sensors) reimu_message(stderr, "(%s) Error: the host is powered on, but no CPU temperature sensor is readable: overheat protection is blind\n", reimu_gettime());
                    if(reimu_textfile_buf_append("\t<error>no CPU temperature sensor readable</error>\n")) reimu_cancel(-250, "Out of memory\n");
                    led_trigger = "blind";
                    status = "blind";
                }
                else if (no_sensors) reimu_message(stderr, "(%s) CPU temperature sensors are readable again (%d)\n", reimu_gettime(), monitored);
                no_sensors = !monitored;
            }
            if (!strcmp(led_trigger, "default-on")) status = "critical";
            /* ok, over-limit, blind (no CPU temperature), critical (alert lines), host-off, settling (just powered on) */
            if(reimu_textfile_buf_append("\t<status>%s</status>\n", status)) reimu_cancel(-250, "Out of memory\n");

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
        for (long left = wait_msec; left > 0; left -= 1000)
        {
            if (s_exit_req) exit(0);
            reimu_msleep((left > 1000) ? 1000 : left, &s_exit_req);
            watchdog_ping();
        }
        if (s_exit_req) exit(0);
    }
}
