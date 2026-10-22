#include <stdio.h>
#include <stdlib.h>
#include <signal.h>
#include <unistd.h>
#include <stdint.h>
#include <string.h>
#include <time.h>
#include <reimu.h>

int overheatd_enabled;
int debug;
long cpu_temp_limit;
long wait_msec;
long blink_msec;

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

int s_cpus[4] = {0, 0, 0, 0};
int s_buses[4] = {0, 0, 0, 0};
long s_hwmons[4][8] = {{-1, -1, -1, -1, -1, -1, -1, -1}, {-1, -1, -1, -1, -1, -1, -1, -1}, {-1, -1, -1, -1, -1, -1, -1, -1}, {-1, -1, -1, -1, -1, -1, -1, -1}};

static void detect_hwmon(int bus, int reg)
{
    int cpu = (reg >> 4) - 2;
    int sensor = reg & 0x0f;
    if(s_hwmons[cpu][sensor] >= 0) return;

    char path[1024];
    snprintf(path, 1023, "/sys/bus/i2c/devices/i2c-%d/%d-00%02x/hwmon", bus, bus, reg);
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
    dbg_printf(stdout, "Sensor %d:%02x is hwmon%d\n", bus, reg, hwmon);

    s_hwmons[cpu][sensor] = hwmon;
}

static int detect_cpus(int unused __attribute__((unused)), const char *pcompatible, int node, int bus, int reg, const char *label, const void *data __attribute__((unused)))
{
    if (!strcmp(pcompatible, "l_pcs_i2c"))
    {
        int cpu = (reg >> 4) - 2;
        int sensor = reg & 0x0f;
        detect_hwmon(bus, reg);
        if ((s_cpus[cpu] == 0) && (s_hwmons[cpu][sensor] >= 0))
        {
            char path[1024];
            snprintf(path, 1023, "/sys/bus/i2c/devices/i2c-%d/%d-00%02x/hwmon/hwmon%ld/temp1_input", bus, bus, reg, s_hwmons[cpu][sensor]);
            dbg_printf(stdout, "Trying to read %s\n", path);
            if(!reimu_chkfile(path))
            {
                s_cpus[cpu] = 1;
                s_buses[cpu] = bus;
            }
        }
    }
    return 0;
}

static long get_temperature(int cpu, int sensor, double *result)
{
    char path[1024];
    int reg = 0x20 + (0x10 * cpu) | sensor;
    snprintf(path, 1023, "/sys/bus/i2c/devices/i2c-%d/%d-00%02x/hwmon/hwmon%ld/temp1_input", s_buses[cpu], s_buses[cpu], reg, s_hwmons[cpu][sensor]);

    char *ctemp, *ctempend;
    if (reimu_readfile(path, &ctemp, NULL)) return -258;

    long temp = strtol(ctemp, &ctempend, 10);
    if (*ctempend && (*ctempend != '\n')) return -259;

    *result = temp;
    *result /= 1000;
    temp /= 1000;
    return temp;
}

static void overheat_set_led_gpio(const char *trigger)
{
    char *triggerlf;
    if ((triggerlf = malloc(strlen(trigger) + 2)) == NULL) reimu_cancel(94, "Out of memory\n");
    strcpy(triggerlf, trigger);
    strcat(triggerlf, "\n");

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
        if(reimu_writefile(triggerfiles[trig], triggerlf, strlen(triggerlf))) reimu_cancel(16, "(%s), Can't set %s to %s\n", reimu_gettime(), triggerfiles[trig], trigger);
    }

    if (!strcmp(trigger,"timer"))
    {
        char msec[16];
        sprintf(msec, "%ld\n", blink_msec);
        for(int trig = 0; delayfiles[trig]; ++trig)
        {
            if(reimu_writefile(delayfiles[trig], msec, strlen(msec))) reimu_cancel(16, "(%s) Can't set delay %ld for %s\n", reimu_gettime(), blink_msec, triggerfiles[trig]);
        }
    }

    dbg_printf(stdout, "Changed LED state trigger to %s through sysfs\n", trigger);
}

int is_atexit_tspi_led_fini = 0;
volatile int s_timer_enable = 0;
volatile int s_tspi_led = 0;
timer_t s_tspi_timer;
int s_tspi_timer_ready = 0;
int s_old_blink_msec = -1;
struct itimerspec s_blink_its = {{0, 0}, {0, 0}};

static void overheat_control_led_tspi(int status, int nodbg)
{
    const char *ledfile = status ? "/sys/kernel/tinyspi/command_bits_reset" : "/sys/kernel/tinyspi/command_bits_set";
    if(reimu_writefile(ledfile, "0x00000040\n", 11)) reimu_cancel(17, "(%s) Can't write to %s\n", reimu_gettime(), ledfile);
    if (!nodbg) dbg_printf(stdout, "Changed LED state through %s\n", ledfile);
}

static void tspi_led_handler(int sig __attribute__((unused)))
{
    if (s_timer_enable)
    {
        s_tspi_led = 1 - s_tspi_led;
        overheat_control_led_tspi(s_tspi_led, 1);
    }
    signal(SIGALRM, tspi_led_handler);
}

static void tspi_led_fini(void)
{
    if (s_tspi_timer_ready) timer_delete(s_tspi_timer);
}

static void tspi_led_init(void)
{
    if (!s_tspi_timer_ready)
    {
        timer_create(CLOCK_MONOTONIC, NULL, &s_tspi_timer);
        signal(SIGALRM, tspi_led_handler);
        reimu_set_atexit(&is_atexit_tspi_led_fini, tspi_led_fini);
        dbg_printf(stdout, "Created TinySPI LED timer\n");
        s_tspi_timer_ready = 1;
    }
    if(s_timer_enable && (s_old_blink_msec != blink_msec))
    {
        if(timer_settime(s_tspi_timer, 0, &s_blink_its, NULL)) reimu_cancel(102, "(%s) Unable to set up TinySPI timer\n", reimu_gettime());
        s_old_blink_msec = blink_msec;
    }
}

static void overheat_set_led_tspi(const char *trigger)
{
    tspi_led_init();
    if(!strcmp(trigger, "timer"))
    {
        if (!s_tspi_timer_ready) reimu_cancel(102, "(%s) TinySPI timer failed\n", reimu_gettime());
        dbg_printf(stdout, "Enabled TinySPI LED timer\n");
        s_timer_enable = 1;
    }
    else
    {
        if (!s_tspi_timer_ready) reimu_cancel(102, "(%s) TinySPI timer failed\n", reimu_gettime());
        dbg_printf(stdout, "Disabled TinySPI LED timer\n");
        s_timer_enable = 0;
        s_tspi_led = (!strcmp(trigger, "default-on")) ? 1 : 0;
        overheat_control_led_tspi(s_tspi_led, 0);
    }
}

static uint32_t get_alerts_tinyspi(void)
{
    /* Reset TinySPI alert */
    if(reimu_writefile("/sys/kernel/tinyspi/command_bits_set", "00000010\n", 9)) reimu_cancel(3, "(%s) Can't start resetting TinySPI alert\n", reimu_gettime());
    reimu_msleep(10, &s_exit_req);
    if(reimu_writefile("/sys/kernel/tinyspi/command_bits_reset", "00000010\n", 9)) reimu_cancel(5, "(%s) Can't finish resetting TinySPI alert\n", reimu_gettime());

    /* Read status register */
    char *tinyspi_state;
    int rv = reimu_readfile("/sys/kernel/tinyspi/state_reg", &tinyspi_state, NULL);
    if (rv) reimu_cancel(64, "(%s) Can't read TinySPI status register\n", reimu_gettime());

    char *endptr;
    uint32_t res = strtoul(tinyspi_state, &endptr, 16);
    if (*endptr != '\n') reimu_cancel(65, "(%s) Wrong data in TinySPI status register\n", reimu_gettime());
    free(tinyspi_state);
    return res;
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

    uint32_t b[32];
    memset(b, 0, sizeof(b));
    b[31] = reimu_get_gpio_by_name("GPIO_RESET_IN");
    b[23] = reimu_get_gpio_by_name("GPIO_APMDZ_LED");
    b[21] = reimu_get_gpio_by_name("GPIO_POWER_IN");
    b[14] = reimu_get_gpio_by_name("GPIO_ALERT_1");
    b[12] = reimu_get_gpio_by_name("GPIO_ALERT_2");
    b[6]  = reimu_get_gpio_by_name("GPIO_ALERT_4");
    b[11] = reimu_get_gpio_by_name("GPIO_ALERT_3");
    b[10] = reimu_get_gpio_by_name("GPIO_ALERT_7");
    b[8]  = reimu_get_gpio_by_name("GPIO_ALERT_5");
    b[9]  = reimu_get_gpio_by_name("GPIO_ALERT_6");
    b[4]  = reimu_get_gpio_by_name("GPIO_INTRUSION");

    uint32_t alerts = 0;
    for (int i = 0; i < 32; ++i)
    {
        if (b[i] < 0) reimu_cancel(3, "(%s) Can't acquire alert GPIO #%d\n", reimu_gettime(), i);
        alerts |= (b[i] << i);
    }

    return alerts | 0x0010a0a5;
}

volatile int s_dbg_state = 0; /* 0 = normal state; 1 = slight overheat; 2 = critical overheat */

static void set_dbg_state(int sig __attribute__((unused)))
{
    s_dbg_state = (s_dbg_state + 1) % 3;
    signal(SIGUSR1, set_dbg_state);
}

static void reload_config(const char *configfile)
{
    if (reimu_chkfile(configfile))
    {
        /* If no configfile exist, create it there */

        const char *defaultconfig =
            "OVERHEATD_ENABLED=yes\n"
            "DEBUG=off\n"
            "CPU_TEMP_LIMIT=85\n"
            "WAIT_MSEC=5000\n"
            "BLINK_MSEC=500\n";

        if(reimu_writefile(configfile, defaultconfig, strlen(defaultconfig))) reimu_cancel(77, "Can't create new config %s\n", defaultconfig);
    }

    char *daemoncfg;
    size_t daemoncfg_len;

    if(reimu_readfile(configfile, &daemoncfg, &daemoncfg_len)) reimu_cancel(91, "Unable to read config file\n");

    overheatd_enabled = reimu_get_conf_bool(daemoncfg, daemoncfg_len, "OVERHEATD_ENABLED");
    debug             = reimu_get_conf_bool(daemoncfg, daemoncfg_len, "DEBUG");
    cpu_temp_limit    = reimu_get_conf_long(daemoncfg, daemoncfg_len, "CPU_TEMP_LIMIT");
    wait_msec         = reimu_get_conf_long(daemoncfg, daemoncfg_len, "WAIT_MSEC");
    blink_msec        = reimu_get_conf_long(daemoncfg, daemoncfg_len, "BLINK_MSEC");

    free(daemoncfg);

    if((overheatd_enabled < 0) || (debug < 0) || (cpu_temp_limit < 0) || (wait_msec < 0) || (blink_msec < 0))
    {
        reimu_cancel(96, "Malformed values in %s\n", configfile);
    }

    s_blink_its.it_value.tv_sec  = blink_msec / 1000L;
    s_blink_its.it_value.tv_nsec = (blink_msec % 1000L) * 10000000L;
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
    char pid[10];
    sprintf(pid, "%d\n", getpid());
    reimu_set_atexit(NULL, rm_pidfile);
    if(reimu_writefile("/run/overheatd.pid", pid, strlen(pid))) reimu_cancel(127, "Can't create pid file\n");
}

static int detect_tinyspi(void)
{
    if (!reimu_find_in_file("/etc/reimu.conf", "TINYSPI=yes") || !reimu_find_in_file("/var/volatile/reimu.conf", "TINYSPI=yes") || !reimu_find_in_file("/var/volatile/boardid", "1111"))
    {
        if(reimu_chkdir("/sys/kernel/tinyspi/")) reimu_cancel(90, "TinySPI is enabled, but no API files exported by kernel\n");
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
    set_signal_handlers();
    create_pidfile();
    if (reimu_textfile_buf_alloc()) reimu_cancel(-250, "Out of memory\n");
    int tinyspi = detect_tinyspi();

    for(;;)
    {
        reload_config("/etc/overheatd.conf");
        dbg_printf(stdout, "Reloaded config: (OVERHEATD_ENABLED = %d, DEBUG = %d; CPU_TEMP_LIMIT = %ld; WAIT_MSEC = %ld; BLINK_MSEC = %ld)\n", overheatd_enabled, debug, cpu_temp_limit, wait_msec, blink_msec);

        if(overheatd_enabled)
        {
            if(reimu_textfile_buf_append("<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n<state date=\"%s\">\n", reimu_gettime())) reimu_cancel(-250, "Out of memory\n");

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
            if (powered_on < 0) reimu_cancel(2, "(%s) Can't acquire GPIO_POWER_IN\n", reimu_gettime());

            /* Re-read alerts, if needed */
            if (tinyspi)
            {
                int check = (tinyspi_alert == 2);
                if ((tinyspi_alert = reimu_get_gpio_by_name("GPIO_TINYSPI_ALERT")) < 0) reimu_cancel(3, "(%s) Can't acquire TinySPI alert\n", reimu_gettime());

                if(check || !tinyspi_alert)
                {
                    dbg_printf(stdout, "Re-reading alerts (tinyspi_alert = %d, old alerts = 0x%08x)\n", tinyspi_alert, alerts);
                    alerts = get_alerts_tinyspi();
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

            /* We are not in critical overheat, and powered on, so check I2C */
            if(!strcmp(led_trigger, "none"))
            {
                if (powered_on)
                {
                    /* Detect available cpus */
                    s_cpus[0] = 0; s_cpus[1] = 0; s_cpus[2] = 0; s_cpus[3] = 0;
                    reimu_traverse_all_i2c(NULL, detect_cpus, JUST_PRINT_ERROR);
                    dbg_printf(stdout, "CPUs detected: %d, %d, %d, %d\n", s_cpus[0], s_cpus[1], s_cpus[2], s_cpus[3]);
                    if(reimu_textfile_buf_append("\t<cpus>")) reimu_cancel(-250, "Out of memory\n");
                    int first = 1;
                    for (int cpu = 0; cpu <= 3; ++cpu)
                    {
                        if (s_cpus[cpu])
                        {
                            if(reimu_textfile_buf_append(first ? "%d" : " %d", cpu)) reimu_cancel(-250, "Out of memory\n");
                            first = 0;
                        }
                    }
                    if(reimu_textfile_buf_append("<cpus>\n")) reimu_cancel(-250, "Out of memory\n");

                    /* Check temperature of each cpu sensor */
                    long maxtemp = 0;
                    for (int cpu = 0; cpu <= 3; ++cpu)
                    {
                        if (s_cpus[cpu])
                        {
                            for (int sensor = 0; sensor <= 7; ++sensor)
                            {
                                if (s_hwmons[cpu][sensor] < 0) continue;
                                long temp;
                                double tempdbl;
                                if((temp = get_temperature(cpu, sensor, &tempdbl)) < -256)
                                {
                                    reimu_message(stderr, "(%s) Failure reading HWMON device (cpu = %d, sensor = %d)\n", reimu_gettime(), cpu, sensor);
                                }
                                else
                                {
                                    dbg_printf(stdout, "Temp (%d:%d): %ld (%f)\n", cpu, sensor, temp, tempdbl);
                                    if(reimu_textfile_buf_append("\t<temperature cpu=\"%d\" sensor=\"%d\">%f</temperature>\n", cpu, sensor, tempdbl)) reimu_cancel(-250, "Out of memory\n");
                                    if (temp > maxtemp) maxtemp = temp;
                                }
                            }
                        }
                    }
                    dbg_printf(stdout, "maxtemp: %ld\n", maxtemp);
                    if (s_dbg_state == 1) { maxtemp = cpu_temp_limit + 1; dbg_printf(stdout, "DEBUG MODE: maxtemp altered to %ld\n", maxtemp); }

                    /* Any of temperatures is greater than limit */
                    if (maxtemp > cpu_temp_limit) led_trigger = "timer";
                }
            }

            /* Update LEDs, if needed */
            dbg_printf(stdout, "LED trigger: %s -> %s\n", oldledtrigger, led_trigger);
            if (strcmp(oldledtrigger, led_trigger))
            {
                if(tinyspi)
                {
                    overheat_set_led_tspi(led_trigger);
                }
                else
                {
                    overheat_set_led_gpio(led_trigger);
                }
            }

            /* Finalize statefile */
            if(reimu_textfile_buf_append("</state>\n")) reimu_cancel(-250, "Out of memory\n");
            if(reimu_textfile_buf_commit("/var/volatile/systemstate.xml")) reimu_cancel(24, "(%s) Can't update statefile\n", reimu_gettime());
        }

        /* Sleep till next cycle */
        reimu_msleep(wait_msec, &s_exit_req);
    }
}
