#include <stdio.h>
#include <string.h>
#include <errno.h>
#include <stdlib.h>
#include <reimu.h>

const char *config_dir = "/etc/default/obmc/hwmon/devices/platform/ahb/ahb--apb/ahb--apb--bus";
const char *supported_devices[] = { "lm96163", "pmbus", "emc2305", "l_pcs_i2c", NULL };

static int instantiate(int node, const char *compatible, int bus, int reg)
{
    if (!reimu_is_in_dict(supported_devices, compatible))
    {
        printf(" - device %s (%d:%02x) is unsupported, skipping\n", compatible, bus, reg);
        return 1;
    }
    const char *status = reimu_getprop(CANCEL_ON_ERROR, node, "status", 0, 29, "Error reading status value from node 0x%08x (device %s, %d:%02x):", node, compatible, bus, reg);
    if (!strcmp(status, "disabled"))
    {
        printf(" - device %s (%d:%02x) is disabled, skipping\n", compatible, bus, reg);
        return 1;
    }

    char sysfspath[512];
    char sysfsdata[256];
    snprintf(sysfspath, 511, "/sys/bus/i2c/devices/i2c-%d/new_device", bus);
    snprintf(sysfsdata, 255, "%s 0x%02x\n", compatible, reg);
    if(reimu_writefile(sysfspath, sysfsdata, strlen(sysfsdata)))
    {
        printf(" - instantiating of %s (%d:%02x) failed (%s), skipping\n", compatible, bus, reg, strerror(errno));
        return 2;
    }

    snprintf(sysfspath, 511, "/sys/bus/i2c/devices/i2c-%d/%d-%04x/hwmon", bus, bus, reg);

    int attempt = 0;
    while(reimu_chkdir(sysfspath))
    {
        if (++attempt == 10 || errno != ENOENT)
        {
            printf(" - device %s (%d:%02x) exported no hwmon directory (%s), skipping\n", compatible, bus, reg, strerror(errno));
            return 3;
        }
        printf(" - device %s (%d:%02x) exported no hwmon directory yet, delaying (attempt %d/10)\n", compatible, bus, reg, attempt);
        reimu_msleep(100, NULL);
    }

    printf(" - instantiated device %s (%d:%02x) \n", compatible, bus, reg);
    return 0;
}

static int deinstantiate(int node, const char *compatible, int bus, int reg)
{
    if (!reimu_is_in_dict(supported_devices, compatible))
    {
        printf(" - device %s (%d:%02x) is unsupported, skipping\n", compatible, bus, reg);
        return 1;
    }
    const char *status = reimu_getprop(BE_SILENT, node, "status", 1, 29, "Error reading status value from node 0x%08x (device %s, %d:%02x):", node, compatible, bus, reg);
    if ((status != NULL) && !strcmp(status, "disabled"))
    {
        printf(" - device %s (%d:%02x) is disabled, skipping\n", compatible, bus, reg);
        return 1;
    }

    char sysfspath[512];
    char sysfsdata[256];
    snprintf(sysfspath, 511, "/sys/bus/i2c/devices/i2c-%d/delete_device", bus);
    snprintf(sysfsdata, 255, "0x%02x\n", reg);
    if(reimu_writefile(sysfspath, sysfsdata, strlen(sysfsdata)))
    {
        printf(" - deinstantiating of %s (%d:%02x) failed (%s), skipping\n", compatible, bus, reg, strerror(errno));
        return 2;
    }

    printf(" - deinstantiated device %s (%d:%02x) \n", compatible, bus, reg);
    return 0;
}

static void detect_addresses(int i2c, int *apb, int *bus)
{
    char bus_path[1024], *real_path;
    snprintf(bus_path, 1023, "/sys/bus/i2c/devices/i2c-%d/device", i2c);
    if((real_path = realpath(bus_path, NULL)) == NULL) reimu_cancel(40, "Can't get real path of device node %s: %s\n", bus_path, strerror(errno));

    char *apb_path = strchr(real_path, '@');
    if (apb_path != NULL)
    {
        ++apb_path;
        char *i2c_path = strchr(apb_path, '/');
        if (i2c_path != NULL)
        {
            *i2c_path = '\0';
            ++i2c_path;
            char *i2c_path_end = strchr(i2c_path, '.');
            if (i2c_path_end != NULL)
            {
                *i2c_path_end = '\0';

                char *apb_end, *bus_end;
                *apb = strtoul(apb_path, &apb_end, 16);
                *bus = strtoul(i2c_path, &bus_end, 16);
                if (!*apb_end && !*bus_end)
                {
                    free(real_path);
                    return;
                }
            }
        }
    }
    reimu_cancel(41, "Can't parse real path of device node %s\n", bus_path);
    free(real_path);
}

static int create_sensor(enum cancel_type_t unused __attribute__((unused)), int node, const char *nodename, traverse_callback_t unused1 __attribute__((unused)), const void *data, int unused2)
{
    if (nodename == NULL) reimu_cancel(22, "Error reading name of node 0x%08x\n", node);

    const char *devlabel = (const char *)data;
    const char *type = NULL, *minvalue = NULL, *maxvalue = NULL;
    if (!strncmp(nodename, "fan", 3))   { type = "fan";   minvalue = "0";       maxvalue = "50000";      }
    if (!strncmp(nodename, "temp", 4))  { type = "temp";  minvalue = "-125000"; maxvalue = "125000";     }
    if (!strncmp(nodename, "in", 2))    { type = "in";    minvalue = "0";       maxvalue = "300000";     }
    if (!strncmp(nodename, "curr", 4))  { type = "curr";  minvalue = "0";       maxvalue = "100000";     }
    if (!strncmp(nodename, "power", 5)) { type = "power"; minvalue = "0";       maxvalue = "1000000000"; }

    if (type != NULL)
    {
        const int *preg = reimu_getprop(CANCEL_ON_ERROR, node, "reg", 1, 23, "Error reading reg value from node 0x%08x:", node);
        int reg = *preg >> 24;
        const char *label = reimu_getprop(CANCEL_ON_ERROR, node, "label", 0, 24, "Error reading label value from node 0x%08x:", node);
        const char *status = reimu_getprop(CANCEL_ON_ERROR, node, "status", 0, 29, "Error reading status value from node 0x%08x:", node);

        if (!reimu_is_prop_empty(label) && !strcmp(status, "okay"))
        {
            const char *warnlo = reimu_getprop(CANCEL_ON_ERROR, node, "min_alert", 0, 25, "Error reading min alert value from node 0x%08x:", node);
            const char *warnhi = reimu_getprop(CANCEL_ON_ERROR, node, "max_alert", 0, 26, "Error reading max alert value from node 0x%08x:", node);
            const char *critlo = reimu_getprop(CANCEL_ON_ERROR, node, "min_crit", 0, 27, "Error reading min crit value from node 0x%08x:", node);
            const char *crithi = reimu_getprop(CANCEL_ON_ERROR, node, "max_crit", 0, 28, "Error reading max crit value from node 0x%08x:", node);

            if (reimu_is_prop_empty(warnhi) && reimu_is_prop_empty(warnlo) && reimu_is_prop_empty(crithi) && reimu_is_prop_empty(critlo))
            {
                printf(" - - sensor %s lacks required properties, skipping\n", nodename);
            }
            else
            {
                char sensor_label[1024];
                snprintf(sensor_label, 1023, "%s %s", devlabel, label);

                for (char *p = sensor_label; *p; ++p)
                {
                    /* The label shall comply with "Valid Object Paths" of D-Bus Spec, that shall only contain the ASCII characters "[A-Z][a-z][0-9]_". */
                    if ((*p >= 'A') && (*p <= 'Z')) continue;
                    if ((*p >= 'a') && (*p <= 'z')) continue;
                    if ((*p >= '0') && (*p <= '9')) continue;
                    *p = '_';
                }

                printf(" - - adding sensor %s as %s%d (%s)\n", nodename, type, reg, sensor_label);
                reimu_textfile_write(CANCEL_ON_ERROR, "LABEL_%s%d=%s\n", type, reg, sensor_label);
                printf(" - - sensor %s: { ", nodename);

                if (!reimu_is_prop_empty(warnlo))
                {
                    printf("warn low %s ", warnlo);
                    reimu_textfile_write(CANCEL_ON_ERROR, "WARNLO_%s%d=%s\n", type, reg, warnlo);
                }

                if (!reimu_is_prop_empty(warnhi))
                {
                    printf("warn high %s ", warnhi);
                    reimu_textfile_write(CANCEL_ON_ERROR, "WARNHI_%s%d=%s\n", type, reg, warnhi);
                }

                if (!reimu_is_prop_empty(critlo))
                {
                    printf("crit low %s ", critlo);
                    reimu_textfile_write(CANCEL_ON_ERROR, "CRITLO_%s%d=%s\n", type, reg, critlo);
                }

                if (!reimu_is_prop_empty(crithi))
                {
                    printf("crit high %s ", crithi);
                    reimu_textfile_write(CANCEL_ON_ERROR, "CRITHI_%s%d=%s\n", type, reg, crithi);
                }

                printf("}\n");
                reimu_textfile_write(CANCEL_ON_ERROR, "MINVALUE_%s%d=%s\nMAXVALUE_%s%d=%s\nASYNC_READ_TIMEOUT_%s%d=\"1000\"\n\n", type, reg, minvalue, type, reg, maxvalue, type, reg);
            }
        }
        else
        {
            printf(" - - sensor %s is disabled or lacks required properties, skipping\n", nodename);
        }
    }
    else
    {
        printf(" - - sensor %s is not of supported types, skipping\n", nodename);
    }
    return 0;
}

static void create_config(int parent, const char *pcompatible, int bus, int dev, const char *devlabel)
{
    int apb_addr, bus_addr;
    detect_addresses(bus, &apb_addr, &bus_addr);

    char config_path[1024];
    snprintf(config_path, 1023, "%s@%08x/%08x.i2c-bus/i2c-%d", config_dir, apb_addr, bus_addr, bus);
    if(reimu_recurse_mkdir(config_path)) reimu_cancel(31, "Error while creating directory for config file for device %d-%04x (%s): %s\n", bus, dev, config_path, strerror(errno));
    snprintf(config_path, 1023, "%s@%08x/%08x.i2c-bus/i2c-%d/%d-%04x.conf", config_dir, apb_addr, bus_addr, bus, bus, dev);
    if (reimu_textfile_create(config_path)) reimu_cancel(32, "Error while creating config file for device %d-%04x (%s): %s\n", bus, dev, config_path, strerror(errno));

    printf(" - creating %s sensors for %s (%d:%02x):\n", devlabel, pcompatible, bus, dev);
    if(reimu_for_each_subnode(CANCEL_ON_ERROR, parent, NULL, (const void *)devlabel, 0, create_sensor))
    {
        reimu_cancel(21, "Error traversing i2c device %d-%04x (%s), node 0x%08x\n", bus, dev, devlabel, parent);
    }
    reimu_textfile_close();
}

enum serv_op { SERVICE_START, SERVICE_STOP, SERVICE_OP_UNKNOWN };

static int instantiator_callback(enum cancel_type_t unused __attribute__((unused)), const char *pcompatible, int node, int bus, int reg, const char *label, const void *data)
{
    enum serv_op op = *(const enum serv_op*)data;
    switch(op)
    {
        case SERVICE_START:
            create_config(node, pcompatible, bus, reg, label);
            instantiate(node, pcompatible, bus, reg);
            break;

        case SERVICE_STOP:
            deinstantiate(node, pcompatible, bus, reg);
            break;
    }
    return 0;
}

int main(int argc, char *argv[])
{
    enum serv_op op = SERVICE_OP_UNKNOWN;
    if (argc > 1)
    {
        if (!strcmp(argv[1], "start")) op = SERVICE_START;
        if (!strcmp(argv[1], "stop")) op = SERVICE_STOP;
    }
    if (op == SERVICE_OP_UNKNOWN) reimu_cancel(3, "Incorrect parameters, use {start|stop}\n");
    reimu_traverse_all_i2c((void *)&op, instantiator_callback, CANCEL_ON_ERROR);
    return 0;
}
