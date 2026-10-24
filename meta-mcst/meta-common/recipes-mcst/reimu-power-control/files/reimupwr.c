#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <reimu.h>

int s_update_delay = 10;
static int s_update_delay_init(void)
{
    char *apo_cfg;
    long apo_cfg_len;
    int rv = reimu_readfile("/etc/auto_power_on", &apo_cfg, &apo_cfg_len);
    if (!rv)
    {
        char *psd_time = strstr(apo_cfg, "PS_DISCHARGE_TIME=");
        if(psd_time != NULL)
        {
            psd_time += strlen("PS_DISCHARGE_TIME=");
            char *endptr;
            long update_delay = strtol(psd_time, &endptr, 0);
            if (endptr != psd_time)
            {
                s_update_delay = update_delay;
                reimu_message(stdout, "Power supply discharge delay: %d seconds\n", s_update_delay);
            }
            else reimu_message(stderr, "Wrong data in auto_power_on config, defaulting power supply discharge delay to %d seconds\n", s_update_delay);
        }
        else reimu_message(stdout, "No power supply discharge delay specified in config, defaulting to %d seconds\n", s_update_delay);
        free(apo_cfg);
    }
    else reimu_message(stderr, "Can't read auto_power_on config, defaulting power supply discharge delay to %d seconds\n", s_update_delay);
    return s_update_delay;
}

int success = 0;
static void set_power_state_on_dbus(int pgood)
{
    const char *state = pgood ? "xyz.openbmc_project.State.Host.HostState.Running" : "xyz.openbmc_project.State.Host.HostState.Off";

    reimu_dbus_manage_service("/org/freedesktop/systemd1/unit/hwmon_2dinstantiator_2eservice", pgood ? "Start" : "Stop");

    int rv;
    if ((rv = reimu_dbus_set_property_str("xyz.openbmc_project.State.Host", "/xyz/openbmc_project/state/host0", "xyz.openbmc_project.State.Host", "CurrentHostState", state)) != 0)
    {
        reimu_message(stderr, "Warning: (%s) Can't set host state to %s (error %d), deferring...\n", reimu_gettime(), state, rv);
        success = 0;
    }
    else
    {
        success = 1;
    }
}

int poll_pgood()
{
    int powered_on;
    for(int i = 4; i >= 0; --i)
    {
        if(((powered_on = reimu_get_gpio_by_name("GPIO_POWER_IN")) < 0) && (i > 0))
        {
            reimu_message(stderr, "Warning: (%s) GPIO_POWER_IN is unavailable, retrying in 100 ms, %d attempts remaining\n", reimu_gettime(), i);
            reimu_msleep(100, NULL);
            continue;
        }
        break;
    }
    if (powered_on < 0) reimu_cancel(2, "Can't acquire GPIO_POWER_IN -- can't continue\n");
    return powered_on;
}

int dbus_available()
{
    return !reimu_dbus_call_method("xyz.openbmc_project.State.Host", "/xyz/openbmc_project/state/host0", "org.freedesktop.DBus.Introspectable", "Introspect");
}

int main(void)
{
    int old_pgood = -1;
    int update_delay = -1;

    reimu_message(stdout, "REIMU power control started at %s\n", reimu_gettime());
    reimu_message(stdout, "Power supply discharge time is %d seconds\n", s_update_delay_init());
    for(;;)
    {
        int pgood = poll_pgood();
        if ((pgood != old_pgood) || !success)
        {
            int changed = pgood != old_pgood;
            if (changed) reimu_message(stdout, "(%s) Power good state changed from %d to %d, changing power state\n", reimu_gettime(), old_pgood, pgood);
            if (dbus_available())
            {
                set_power_state_on_dbus(pgood);
            }
            else
            {
                /* State.Host not up yet (BMC boot with the platform on):
                 * again in a second, or the host state and the sensors
                 * (hwmon-instantiator) would wait for the next change */
                if (changed) reimu_message(stdout, "(%s) Host state service not available yet, retrying\n", reimu_gettime());
                success = 0;
            }
            if (changed)
            {
                reimu_message(stdout, "(%s) Deferring power state being saved for %d seconds\n", reimu_gettime(), s_update_delay);
                update_delay = s_update_delay;
            }
            old_pgood = pgood;
        }
        if (update_delay == 0)
        {
            reimu_message(stdout, "(%s) Power good state %d is saved as last power state\n", reimu_gettime(), pgood);
            char powerstate[32];
            snprintf(powerstate, 31, "%d\n", pgood);
            if (reimu_writefile("/var/lib/reimu/last_power_state", powerstate, strlen(powerstate)) != 0)
            {
                reimu_message(stderr, "Warning: can't save current power state (check if /var/lib/reimu/ exist and writable)\n");
            }
        }
        if (update_delay >= 0) --update_delay;
        reimu_msleep(1000, NULL);
    }
}
