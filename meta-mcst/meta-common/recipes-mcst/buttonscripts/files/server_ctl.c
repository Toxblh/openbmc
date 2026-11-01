#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <libgen.h>
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <time.h>
#include <gpiod.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <syslog.h>
#include <unistd.h>
#include "reimu.h"

/* The PNS flash is switched to the BMC (reimu-pns-flash, under this lock)
 * only while the platform is off. Nothing that can power the platform on
 * runs while a flash job holds the lock; once it is ours (held until we
 * exit, so no job starts meanwhile) the flash goes back to the host, in
 * case a job was killed with it switched to the BMC. A job that is just
 * ending (or reimu-pns-flash disconnect) holds it for a moment: wait for it
 * up to PNS_WAIT_MS; a real read or write takes minutes: refused then. */
#define PNS_LOCK_DIR "/run/reimu-pns"
#define PNS_LOCK PNS_LOCK_DIR "/lock"
#define PNS_WAIT_MS 15000
/* power good after the power button: the lock is kept until then (a job
 * started meanwhile would see the platform off and take the flash) */
#define POWER_ON_WAIT_S 10
#define SHORT_PRESS_MS 200
#define LONG_PRESS_MS 5000
#define PRESS_POLL_MS 200 /* power good during the long press */

static int pns_fd = -1;

static int pns_lock(int wait_ms)
{
    if (pns_fd >= 0) return 0;
    mkdir(PNS_LOCK_DIR, 0755);
    int fd = open(PNS_LOCK, O_RDWR | O_CREAT | O_CLOEXEC, 0644);
    if (fd < 0) return 0;
    for (int ms = 0; flock(fd, LOCK_EX | LOCK_NB); ms += 200)
    {
        if (ms >= wait_ms)
        {
            close(fd);
            return 53;
        }
        if (ms == 0)
        {
            fprintf(stderr, "The PNS flash is busy (reimu-pns-flash): waiting for it up to %d s\n", wait_ms / 1000);
            syslog(LOG_WARNING, "PNS flash busy (reimu-pns-flash): waiting for it up to %d s", wait_ms / 1000);
        }
        reimu_msleep(200, NULL);
    }
    pns_fd = fd;
    return 0;
}

static int pns_guard(void)
{
    if (pns_fd >= 0) return 0;
    if (pns_lock(PNS_WAIT_MS))
    {
        fprintf(stderr, "The PNS flash is being read or written (reimu-pns-flash): the platform can't be powered on or reset now\n");
        syslog(LOG_ERR, "REFUSED: the PNS flash is being read or written (reimu-pns-flash): the platform can't be powered on or reset now");
        return 53;
    }
    if (system(". /libexec/gpio-funcs && gpiowrite `gpio2num $GPIO_SPI_CONNECT` 0") != 0)
        fprintf(stderr, "Warning: can't switch the PNS flash to the host (GPIO_SPI_CONNECT)\n");
    return 0;
}

static int get_power_state(void)
{
    return reimu_get_gpio_by_name("GPIO_POWER_IN");
}

/* in libreimu, not in its header */
int reimu_gpio_to_num(const char *name);

static long long monotonic_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec * 1000LL + ts.tv_nsec / 1000000;
}

/* The long press of a host that is on (libreimu's reimu_set_gpio can't end
 * early): held until power good drops, delay at the most. Holding on after the
 * drop is pressing the button of a host that is off: on the board power good
 * drops at ~4 s of the 5 s, and once the host came back on a second later.
 * Power good is read once per poll, busy or failing just isn't a drop: nothing
 * here may make the press longer than delay. */
static int press_until_power_drops(const char *sym, int delay)
{
    int num = reimu_gpio_to_num(sym), power_num = reimu_gpio_to_num("GPIO_POWER_IN");
    if (num < 0) return num;
    if (power_num < 0) return power_num;
    struct gpiod_chip *chip = gpiod_chip_open("/dev/gpiochip0");
    if (chip == NULL) return -1;
    struct gpiod_line *line = gpiod_chip_get_line(chip, num), *power = gpiod_chip_get_line(chip, power_num);
    if ((line == NULL) || (power == NULL))
    {
        gpiod_chip_close(chip);
        return -2;
    }
    /* a reader has it for a moment: as in libreimu, a second of that */
    int rv = -1;
    for (int attempt = 0; attempt < 20; ++attempt)
    {
        if (attempt) reimu_msleep(50, NULL);
        rv = gpiod_line_request_output(line, "libreimu", 0);
        if (!rv || (errno != EBUSY)) break;
    }
    if (rv)
    {
        gpiod_chip_close(chip);
        return -3;
    }
    long long start = monotonic_ms();
    for (;;)
    {
        long long left = start + delay - monotonic_ms();
        if (left <= 0) break;
        reimu_msleep((left < PRESS_POLL_MS) ? left : PRESS_POLL_MS, NULL);
        if (monotonic_ms() >= start + delay) break;
        if (gpiod_line_request_input(power, "libreimu")) continue;
        int good = gpiod_line_get_value(power);
        gpiod_line_release(power);
        if (good == 0)
        {
            syslog(LOG_NOTICE, "power good dropped %lld ms into the press: released", monotonic_ms() - start);
            break;
        }
    }
    /* released: it would stay driven; an input again floats it */
    gpiod_line_release(line);
    rv = gpiod_line_request_input(line, "libreimu");
    if (!rv) gpiod_line_release(line);
    gpiod_chip_close(chip);
    return rv ? -4 : 0;
}

/* off_only: a press meant to power the host off. The power state seen by the
 * caller may be 15 s old by now (the lock), and the button of a host that is off
 * powers it on: looked at again, with the lock; not known: not pressed. */
static int press_button(const char *sym, int delay, int off_only)
{
    int rv = pns_guard();
    if (rv) return rv;
    int power_button = !strcmp(sym, "GPIO_PWR_BTN");
    int power_state = power_button ? get_power_state() : -1;
    if (off_only && (power_state == 0))
    {
        fprintf(stderr, "The host is off already: the power button isn't pressed\n");
        syslog(LOG_NOTICE, "the host went off meanwhile: the power button isn't pressed");
        return 0;
    }
    if (off_only && (power_state < 0)) return power_state;
    /* the power button of a platform that is off: it powers up */
    int powering_on = power_button && (power_state == 0);
    /* The pulse isn't cut short: the power units' mutual Conflicts= stop this
     * one (TimeoutStopSec is much longer than a press), and a line given up half
     * way stays driven low, the button held. A signal takes effect after it. */
    sigset_t block, old;
    sigemptyset(&block);
    sigaddset(&block, SIGTERM);
    sigaddset(&block, SIGINT);
    sigaddset(&block, SIGHUP);
    sigprocmask(SIG_BLOCK, &block, &old);
    if (power_button && (power_state == 1) && (delay >= LONG_PRESS_MS)) rv = press_until_power_drops(sym, delay);
    else rv = reimu_set_gpio_by_name(sym, 0, delay);
    sigprocmask(SIG_SETMASK, &old, NULL);
    if (rv || !powering_on) return rv;
    for (int i = 0; i < POWER_ON_WAIT_S * 5; ++i)
    {
        if (get_power_state() == 1) return 0;
        reimu_msleep(200, NULL);
    }
    fprintf(stderr, "No power good %d s after the power button\n", POWER_ON_WAIT_S);
    return 0;
}

static int server_pwrbut_s(int argc __attribute__((unused)), char *argv[] __attribute__((unused)))
{
    return press_button("GPIO_PWR_BTN", SHORT_PRESS_MS, 0);
}

static int server_pwrbut_h(int argc __attribute__((unused)), char *argv[] __attribute__((unused)))
{
    return press_button("GPIO_PWR_BTN", LONG_PRESS_MS, 0);
}

static int server_reset(int argc __attribute__((unused)), char *argv[] __attribute__((unused)))
{
    return press_button("GPIO_RST_BTN", SHORT_PRESS_MS, 0);
}

static int server_pwr_on(int argc, char *argv[])
{
    int power_state = get_power_state();
    switch(power_state)
    {
        case 1:  return 0;
        case 0:  return server_pwrbut_s(argc, argv);
        default: return power_state;
    }
}

static int server_pwr_off(int argc, char *argv[])
{
    int power_state = get_power_state();
    switch(power_state)
    {
        case 0:  return 0;
        case 1:  return press_button("GPIO_PWR_BTN", SHORT_PRESS_MS, 1);
        default: return power_state;
    }
}

static int server_pwr_off_hard(int argc, char *argv[])
{
    int power_state = get_power_state();
    switch(power_state)
    {
        case 0:  return 0;
        case 1:  return press_button("GPIO_PWR_BTN", LONG_PRESS_MS, 1);
        default: return power_state;
    }
}

static int dbus_watchdog_off(void)
{
    return reimu_dbus_set_property_bool("xyz.openbmc_project.Watchdog", "/xyz/openbmc_project/watchdog/host0", "xyz.openbmc_project.State.Watchdog", "Enabled", 0);
}

static int server_watchdog_reset(int argc, char *argv[])
{
    /* (host-poweron, after server_pwr_on) the lock while power good may
     * still come: no PNS job meanwhile; nothing is pressed here, so it goes
     * on without it */
    pns_lock(2000);
    /* 10 seconds should be fair enough for power to became on. */
    for(int sec = 10; sec > 0; --sec)
    {
        int power_state = get_power_state();
        switch(power_state)
        {
            case 1:
                /* the host watchdog is started along with the platform, it
                 * may not be on D-Bus yet; without one nothing is to be
                 * turned off: the power-on itself didn't fail */
                for (int try = 0; try < 5; ++try)
                {
                    if (!dbus_watchdog_off()) return 0;
                    reimu_msleep(1000, NULL);
                }
                fprintf(stderr, "Warning: can't turn the host watchdog off (not running?)\n");
                return 0;
            case 0: break;
            default: return power_state;
        }
        reimu_msleep(1000, NULL);
    }
    return 35;
}

enum { UID_API_NONE, UID_API_DBUS, UID_API_TSPI, UID_API_GPIO } s_uid_api = UID_API_NONE;

static int uid_init(void)
{
    /* Return if already initialized */
    if (s_uid_api != UID_API_NONE) return 0;

    /* Try to introspect LED manager. If success, consider DBUS protocol applicable. */
    if (!reimu_dbus_call_method("xyz.openbmc_project.LED.GroupManager", "/", "org.freedesktop.DBus.Introspectable", "Introspect"))
    {
        s_uid_api = UID_API_DBUS;
        return 0;
    }

    /* Try to check whether TinySPI is enabled, and tinyspi sysfs directory exist. If success, consider TSPI protocol applicable. */
    if (!reimu_find_in_file("/etc/reimu.conf", "TINYSPI=yes") || !reimu_find_in_file("/var/volatile/reimu.conf", "TINYSPI=yes") || !reimu_find_in_file("/var/volatile/boardid", "1111"))
    {
        if(reimu_chkdir("/sys/kernel/tinyspi/")) return 39;
        s_uid_api = UID_API_TSPI;
        return 0;
    }

    /* Try to check is LED sysfs directory exist. If success, consider GPIO protocol applicable. */
    if(!reimu_chkdir("/sys/class/leds/platform:blue:uid/"))
    {
        s_uid_api = UID_API_GPIO;
        return 0;
    }

    /* No suitable UID control method found */
    return 40;
}

static int uid_set_dbus(int state)
{
    return reimu_dbus_set_property_bool("xyz.openbmc_project.LED.GroupManager", "/xyz/openbmc_project/led/groups/enclosure_identify", "xyz.openbmc_project.Led.Group", "Asserted", state);
}

static int uid_get_dbus(void)
{
    return reimu_dbus_get_property_bool("xyz.openbmc_project.LED.GroupManager", "/xyz/openbmc_project/led/groups/enclosure_identify", "xyz.openbmc_project.Led.Group", "Asserted");
}

static int uid_set_gpio(int state)
{
    char *trig = state ? "default-on\n" : "none\n";
    return reimu_writefile("/sys/class/leds/platform:blue:uid/trigger", trig, strlen(trig));
}

static int uid_get_gpio(void)
{
    char *led_state = NULL;
    int rv = reimu_readfile("/sys/class/leds/platform:blue:uid/brightness", &led_state, NULL);
    if (rv)
    {
        rv = -1;
    }
    else
    {
        rv = -2;
        if (!strncmp(led_state, "0\n", 2)) rv = 0;
        if (!strncmp(led_state, "255\n", 4)) rv = 1;
    }
    free(led_state);
    return rv;
}

static int uid_set_tspi(int state)
{
    char *bits = state ? "00000004\n" : "00000008\n";
    if(reimu_writefile("/sys/kernel/tinyspi/command_bits_set", bits, strlen(bits))) return 43;
    reimu_msleep(10, NULL);
    if(reimu_writefile("/sys/kernel/tinyspi/command_bits_reset", bits, strlen(bits))) return 45;
    return 0;
}

static int uid_get_tspi(void)
{
    char *led_state = NULL;
    int rv = reimu_readfile("/sys/kernel/tinyspi/state_reg", &led_state, NULL);
    if (rv)
    {
        rv = -1;
    }
    else
    {
        rv = -2;
        char *endptr;
        unsigned long res = strtoul(led_state, &endptr, 16);
        if (*endptr == '\n') rv = ((res & 0x00000008) ? 1 : 0);
    }
    free(led_state);
    return rv;
}

static int uid_set(int state)
{
    int rv = uid_init();
    if (rv) return rv;

    switch(s_uid_api)
    {
        case UID_API_DBUS: return uid_set_dbus(state);
        case UID_API_TSPI: return uid_set_tspi(state);
        case UID_API_GPIO: return uid_set_gpio(state);
        default: return 48;
    }
}

static int uid_get(void)
{
    int rv = uid_init();
    if (rv) return rv;

    switch(s_uid_api)
    {
        case UID_API_DBUS: return uid_get_dbus();
        case UID_API_TSPI: return uid_get_tspi();
        case UID_API_GPIO: return uid_get_gpio();
        default: return 49;
    }
}

static int uid_usage(int argc __attribute__((unused)), char *argv[])
{
    printf
    (
        "    Unit ID management utility for REIMU.\n"
        "    Usage:\n"
        "        %s off    - to set UID state off.\n"
        "        %s on     - to set UID state on.\n"
        "        %s switch - to invert current UID state.\n"
        "        %s query  - to query UID state (return code: 0 = off, 1 = on).\n"
        "        %s show   - to do the same as 'query', but also show UID state in terminal.\n",
        argv[0], argv[0], argv[0], argv[0], argv[0]
    );
    return 0;
}

static int uid_on(int argc __attribute__((unused)), char *argv[] __attribute__((unused)))
{
    return uid_set(1);
}

static int uid_off(int argc __attribute__((unused)), char *argv[] __attribute__((unused)))
{
    return uid_set(0);
}

static int uid_query(int argc __attribute__((unused)), char *argv[] __attribute__((unused)))
{
    return uid_get();
}

static int uid_show(int argc __attribute__((unused)), char *argv[] __attribute__((unused)))
{
    int uid_state = uid_get();
    switch(uid_state)
    {
        case 0:  printf("UID is OFF\n"); break;
        case 1:  printf("UID is ON\n"); break;
        default: fprintf(stderr, "Can't identify UID state!\n"); break;
    }
    return uid_state;
}

static int uid_switch(int argc __attribute__((unused)), char *argv[] __attribute__((unused)))
{
    int uid_state = uid_get();
    switch(uid_state)
    {
        case 0:  return uid_set(1);
        case 1:  return uid_set(0);
        default: fprintf(stderr, "Can't identify UID state!\n"); return uid_state;
    }
}

struct func_list_t { const char *basename; int (*func)(int argc, char *argv[]); };

/* SIGKILL can't be held off: a press killed before left its button driven low.
 * Reading a line makes it an input again; busy: someone is pressing it right now.
 * The reset line is the BMC's on purpose until allow_host_boot releases it. */
static void release_stuck_buttons(void)
{
    reimu_get_gpio_by_name("GPIO_PWR_BTN");
    if (!access("/run/reimu-reset-released", F_OK)) reimu_get_gpio_by_name("GPIO_RST_BTN");
}

struct func_list_t uid_cmds[] =
{
    { "on", uid_on },
    { "off", uid_off },
    { "query", uid_query },
    { "show", uid_show },
    { "switch", uid_switch },
    { "-h", uid_usage },
    { "--help", uid_usage }
};

static int server_uid(int argc, char *argv[])
{
    if (argc > 1)
    {
        for (int i = 0; uid_cmds[i].basename != NULL; ++i)
        {
            if (strcmp(argv[1], uid_cmds[i].basename)) continue;
            return uid_cmds[i].func(argc, argv);
        }
        fprintf(stderr, "You should specify a correct command.\n");
        uid_usage(argc, argv);
        return 50;
    }
    return uid_usage(argc, argv);
}

struct func_list_t main_cmds[] =
{
    { "server_pwr_on", server_pwr_on },
    { "server_pwr_off", server_pwr_off },
    { "server_pwr_off_hard", server_pwr_off_hard },
    { "server_pwrbut_s", server_pwrbut_s },
    { "server_pwrbut_h", server_pwrbut_h },
    { "server_reset", server_reset },
    { "server_watchdog_reset", server_watchdog_reset },
    { "server_uid", server_uid },
    { NULL, NULL }
};

int main(int argc, char *argv[])
{
    char *cmd = basename(argv[0]);
    openlog(cmd, LOG_PID, LOG_DAEMON);
    for (int i = 0; main_cmds[i].basename != NULL; ++i)
    {
        if (strcmp(cmd, main_cmds[i].basename)) continue;
        if (strcmp(cmd, "server_uid")) release_stuck_buttons();
        return main_cmds[i].func(argc, argv);
    }
    return 51;
}
