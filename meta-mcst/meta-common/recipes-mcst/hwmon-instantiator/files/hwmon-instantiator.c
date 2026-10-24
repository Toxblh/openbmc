#include <stdio.h>
#include <string.h>
#include <errno.h>
#include <stdlib.h>
#include <unistd.h>
#include <sys/wait.h>
#include <dirent.h>
#include <fcntl.h>
#include <libfdt.h>
#include <reimu.h>

/* libreimu: the device tree, loaded by reimu_traverse_all_i2c() */
extern char *reimu_dtb;

// phosphor-hwmon reads <config_dir><device path in /sys, ":" as "--">.conf
const char *config_dir = "/etc/default/obmc/hwmon";
const char *supported_devices[] = { "lm96163", "pmbus", "emc2305", "l_pcs_i2c", NULL };

/* E8C2 Power Control System: CPU N's temperature sensors answer at
 * 0x20 + 0x10 * N + k on this BMC bus (device tree: a pcs@N node outside the
 * I2C buses), read by the l_pcs_i2c driver; their places as the host's l_pcs
 * driver names T0..T6 */
#define PCS_BUS 5
static const char *const pcs_names[] = { "Cores 0 1", "Cores 2 3", "Cores 4 5", "Cores 6 7",
                                         "Memory Controllers 0 1", "Memory Controllers 2 3", "System Commutator" };
#define PCS_SENSORS (int)(sizeof(pcs_names) / sizeof(pcs_names[0]))

/* Read errors of an I2C device while the platform powers up or down: EIO
 * ENXIO EAGAIN EBUSY ENODATA EPROTO EBADMSG ETIMEDOUT EREMOTEIO. In
 * REMOVERCS, phosphor-hwmon takes the sensor off D-Bus until it reads again
 * (instead of exiting, or with update-functional-on-fail marking it not
 * functional with its last value) */
#define REMOVE_RCS "5,6,11,16,61,71,74,110,121"

static void detect_config_dir(int i2c, char *dir, size_t size, char *devpath, size_t devsize);
static void run(const char *path, const char *arg1, const char *arg2);

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
            /* no driver or no chip: the client goes, not to be in the way of the next start */
            snprintf(sysfspath, 511, "/sys/bus/i2c/devices/i2c-%d/delete_device", bus);
            snprintf(sysfsdata, 255, "0x%02x\n", reg);
            reimu_writefile(sysfspath, sysfsdata, strlen(sysfsdata));
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

    /* its phosphor-hwmon goes too, whether or not udev sees the device go
     * (a start killed at a power-off may have started it already) */
    char config_dir[1024], devpath[1024];
    detect_config_dir(bus, config_dir, sizeof(config_dir), devpath, sizeof(devpath));
    size_t len = strlen(devpath);
    snprintf(devpath + len, sizeof(devpath) - len, "/%d-%04x", bus, reg);
    run("/usr/bin/start_hwmon.sh", "stop", devpath);

    char sysfspath[512];
    char sysfsdata[256];
    snprintf(sysfspath, 511, "/sys/bus/i2c/devices/%d-%04x", bus, reg);
    if (reimu_chkdir(sysfspath)) return 1; /* not instantiated */
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

// The adapter's directory of the configs: the controller's name differs
// between kernels (1e78a0c0.i2c-bus in 5.10, 1e78a0c0.i2c in 6.x), so it's
// taken from sysfs, not built from the addresses. devpath: the same path
// without the config dir (/devices/...), for start_hwmon.sh
static void detect_config_dir(int i2c, char *dir, size_t size, char *devpath, size_t devsize)
{
    char bus_path[1024], *real_path;
    snprintf(bus_path, 1023, "/sys/bus/i2c/devices/i2c-%d", i2c);
    if((real_path = realpath(bus_path, NULL)) == NULL) reimu_cancel(40, "Can't get real path of device node %s: %s\n", bus_path, strerror(errno));
    if(strncmp(real_path, "/sys/devices/", 13)) reimu_cancel(41, "Can't parse real path of device node %s\n", bus_path);

    size_t len = snprintf(dir, size, "%s", config_dir);
    for(const char *c = real_path + 4; *c && len + 3 < size; ++c)
    {
        if(*c == ':') { dir[len++] = '-'; dir[len++] = '-'; }
        else dir[len++] = *c;
    }
    dir[len] = '\0';
    snprintf(devpath, devsize, "%s", real_path + 4);
    free(real_path);
}

static int s_sensors; /* LABEL_ lines in the config being written */

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
                ++s_sensors;
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

/* a D-Bus object name of "<device> <sensor>": [A-Za-z0-9_] only (D-Bus Spec,
 * "Valid Object Paths"), no repeated '_', unique among this run's sensors */
static void object_name(char *out, size_t size, const char *devlabel, const char *label)
{
    static char used[256][96];
    static int nused;
    char raw[256];
    snprintf(raw, sizeof(raw), "%s %s", devlabel, label);
    size_t n = 0;
    for (const char *p = raw; *p && n + 1 < size; ++p)
    {
        int ok = (*p >= 'A' && *p <= 'Z') || (*p >= 'a' && *p <= 'z') || (*p >= '0' && *p <= '9');
        if (ok) out[n++] = *p;
        else if (n && out[n - 1] != '_') out[n++] = '_';
    }
    while (n && out[n - 1] == '_') --n;
    out[n] = '\0';
    char base[96];
    snprintf(base, sizeof(base), "%s", out);
    for (int k = 2, i = 0; i < nused; ++i)
        if (!strcmp(used[i], out))
        {
            snprintf(out, size, "%s_%d", base, k++);
            i = -1;
        }
    if (nused < 256) snprintf(used[nused++], sizeof(used[0]), "%s", out);
}

/* a sysfs attribute's text, without the newline; -1 on a read error
 * (reimu_readfile() doesn't see those, nor ends the text) */
static int read_attr(const char *path, char *buf, size_t size)
{
    int fd = open(path, O_RDONLY);
    if (fd < 0) return -1;
    ssize_t n = read(fd, buf, size - 1);
    close(fd);
    if (n < 0) return -1;
    buf[n] = '\0';
    buf[strcspn(buf, "\n")] = '\0';
    return 0;
}

/* Device trees of 1Э8СВ (1.0_2025, 2.0) don't have the per-sensor child
 * nodes create_sensor() reads, but label a device's inputs with properties
 * as temp1_label = "Internal Temp" ("not connected": none). So: every input
 * of the hwmon device, named by such a property, else by the driver's label
 * (PMBus: vin, iout1...), else as the input; no thresholds */
static void create_sensors_from_hwmon(int node, const char *hwmon, const char *devlabel)
{
    static const char *const types[] = { "temp", "in", "curr", "power", "fan", NULL };
    for (int t = 0; types[t]; ++t)
        for (int i = 0; i <= 16; ++i)
        {
            char attr[64], path[1024], label[128], name[256];
            snprintf(path, sizeof(path), "%s/%s%d_input", hwmon, types[t], i);
            if (access(path, R_OK)) continue;
            snprintf(attr, sizeof(attr), "%s%d_label", types[t], i);
            const char *dtlabel = node >= 0 ? reimu_getprop(BE_SILENT, node, attr, 0, 0, "%s", "") : NULL;
            if (dtlabel && !reimu_is_prop_empty(dtlabel))
            {
                if (strstr(dtlabel, "not connected"))
                {
                    printf(" - - sensor %s%d is not connected, skipping\n", types[t], i);
                    continue;
                }
                snprintf(label, sizeof(label), "%s", dtlabel);
            }
            else
            {
                snprintf(path, sizeof(path), "%s/%s", hwmon, attr);
                if (read_attr(path, label, sizeof(label)) || !label[0]) snprintf(label, sizeof(label), "%s%d", types[t], i);
            }
            object_name(name, sizeof(name), devlabel, label);
            printf(" - - adding sensor %s%d (%s)\n", types[t], i, name);
            reimu_textfile_write(CANCEL_ON_ERROR, "LABEL_%s%d=%s\n", types[t], i, name);
            ++s_sensors;
        }
}

/* /sys/bus/i2c/devices/<bus>-<addr>/hwmon/hwmonN */
static int hwmon_dir(int bus, int dev, char *out, size_t size)
{
    char path[256];
    snprintf(path, sizeof(path), "/sys/bus/i2c/devices/%d-%04x/hwmon", bus, dev);
    DIR *d = opendir(path);
    if (!d) return -1;
    int rv = -1;
    for (struct dirent *e; rv && (e = readdir(d)) != NULL;)
        if (!strncmp(e->d_name, "hwmon", 5)) rv = snprintf(out, size, "%s/%s", path, e->d_name) < (int)size ? 0 : -1;
    closedir(d);
    return rv;
}

static void run(const char *path, const char *arg1, const char *arg2)
{
    pid_t pid = fork();
    if (pid == 0)
    {
        execl(path, path, arg1, arg2, (char *)NULL);
        _exit(127);
    }
    if (pid > 0) waitpid(pid, NULL, 0);
}

/* the config of an instantiated device: written to a temporary file and
 * renamed, as phosphor-hwmon may be started by udev at any time; then its
 * phosphor-hwmon is started (udev's start, at the hwmon device's arrival,
 * found no config yet when the device is new) */
static void create_config(int parent, const char *pcompatible, int bus, int dev, const char *devlabel)
{
    char config_path[1024], devpath[1024], tmp_path[1100];
    detect_config_dir(bus, config_path, sizeof(config_path), devpath, sizeof(devpath));
    if(reimu_recurse_mkdir(config_path)) reimu_cancel(31, "Error while creating directory for config file for device %d-%04x (%s): %s\n", bus, dev, config_path, strerror(errno));
    size_t len = strlen(config_path);
    snprintf(config_path + len, sizeof(config_path) - len, "/%d-%04x.conf", bus, dev);
    len = strlen(devpath);
    snprintf(devpath + len, sizeof(devpath) - len, "/%d-%04x", bus, dev);
    snprintf(tmp_path, sizeof(tmp_path), "%s.tmp", config_path);
    if (reimu_textfile_create(tmp_path)) reimu_cancel(32, "Error while creating config file for device %d-%04x (%s): %s\n", bus, dev, tmp_path, strerror(errno));

    printf(" - creating %s sensors for %s (%d:%02x):\n", devlabel, pcompatible, bus, dev);
    reimu_textfile_write(CANCEL_ON_ERROR, "REMOVERCS=%s\n", REMOVE_RCS);
    s_sensors = 0;
    if (parent >= 0 && reimu_for_each_subnode(CANCEL_ON_ERROR, parent, NULL, (const void *)devlabel, 0, create_sensor))
    {
        reimu_cancel(21, "Error traversing i2c device %d-%04x (%s), node 0x%08x\n", bus, dev, devlabel, parent);
    }
    if (!s_sensors)
    {
        char hwmon[1024];
        if (!hwmon_dir(bus, dev, hwmon, sizeof(hwmon))) create_sensors_from_hwmon(parent, hwmon, devlabel);
    }
    reimu_textfile_close();
    if (rename(tmp_path, config_path)) reimu_cancel(33, "Error while renaming config file for device %d-%04x (%s): %s\n", bus, dev, config_path, strerror(errno));
    if (s_sensors) run("/usr/bin/start_hwmon.sh", "start", devpath);
}

enum serv_op { SERVICE_START, SERVICE_STOP, SERVICE_OP_UNKNOWN };

static int instantiator_callback(enum cancel_type_t unused __attribute__((unused)), const char *pcompatible, int node, int bus, int reg, const char *label, const void *data)
{
    enum serv_op op = *(const enum serv_op*)data;
    switch(op)
    {
        case SERVICE_START:
            if (!instantiate(node, pcompatible, bus, reg)) create_config(node, pcompatible, bus, reg, label);
            break;

        case SERVICE_STOP:
            deinstantiate(node, pcompatible, bus, reg);
            break;

        default:
            break;
    }
    return 0;
}

/* pcs@N nodes at the top of the device tree (1.0: numa-node-id = <N>) */
static void pcs_devices(enum serv_op op)
{
    int node;
    fdt_for_each_subnode(node, reimu_dtb, 0)
    {
        const char *name = fdt_get_name(reimu_dtb, node, NULL);
        if (!name || strncmp(name, "pcs@", 4)) continue;
        int len;
        const fdt32_t *numa = fdt_getprop(reimu_dtb, node, "numa-node-id", &len);
        int cpu = (numa && len >= 4) ? (int)fdt32_to_cpu(*numa) : atoi(name + 4);
        if (cpu < 0 || cpu > 3) continue;
        char devlabel[16];
        snprintf(devlabel, sizeof(devlabel), "CPU%d", cpu);
        for (int k = 0; k < PCS_SENSORS; ++k)
        {
            int reg = 0x20 + 0x10 * cpu + k;
            if (op == SERVICE_STOP)
            {
                deinstantiate(node, "l_pcs_i2c", PCS_BUS, reg);
                continue;
            }
            if (instantiate(node, "l_pcs_i2c", PCS_BUS, reg)) continue;
            /* the driver doesn't touch the chip at probe: a sensor that
             * doesn't answer (the CPU has fewer) goes, rather than be a
             * phosphor-hwmon sensor that never reads */
            char path[1024], devpath[1024], tmp_path[1100], oname[128];
            int answers = 0;
            if (!hwmon_dir(PCS_BUS, reg, path, sizeof(path)))
            {
                size_t n = strlen(path);
                snprintf(path + n, sizeof(path) - n, "/temp1_input");
                for (int attempt = 0; attempt < 3 && !answers; ++attempt)
                {
                    char value[32];
                    if (attempt) reimu_msleep(500, NULL);
                    answers = !read_attr(path, value, sizeof(value));
                }
            }
            if (!answers)
            {
                printf(" - device l_pcs_i2c (%d:%02x) doesn't answer, skipping\n", PCS_BUS, reg);
                deinstantiate(node, "l_pcs_i2c", PCS_BUS, reg);
                continue;
            }
            detect_config_dir(PCS_BUS, path, sizeof(path), devpath, sizeof(devpath));
            if (reimu_recurse_mkdir(path)) continue;
            size_t n = strlen(path);
            snprintf(path + n, sizeof(path) - n, "/%d-%04x.conf", PCS_BUS, reg);
            n = strlen(devpath);
            snprintf(devpath + n, sizeof(devpath) - n, "/%d-%04x", PCS_BUS, reg);
            snprintf(tmp_path, sizeof(tmp_path), "%s.tmp", path);
            if (reimu_textfile_create(tmp_path)) continue;
            object_name(oname, sizeof(oname), devlabel, pcs_names[k]);
            printf(" - - adding sensor temp1 (%s)\n", oname);
            reimu_textfile_write(CANCEL_ON_ERROR, "REMOVERCS=%s\nLABEL_temp1=%s\n", REMOVE_RCS, oname);
            reimu_textfile_close();
            if (!rename(tmp_path, path)) run("/usr/bin/start_hwmon.sh", "start", devpath);
        }
    }
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
    if (access("/var/volatile/motherboard_devtree.dtb", R_OK))
    {
        printf("No platform device tree (see mcst-fruid.service): no platform sensors\n");
        return 0;
    }
    /* start: first whatever an earlier run left (a start that failed, a
     * stop that didn't run), so devices are never instantiated twice */
    enum serv_op stop = SERVICE_STOP;
    reimu_traverse_all_i2c((void *)&stop, instantiator_callback, CANCEL_ON_ERROR);
    pcs_devices(SERVICE_STOP);
    if (op == SERVICE_START)
    {
        /* the l_pcs_i2c driver is a module */
        run("/sbin/modprobe", "l-pcs-i2c", NULL);
        reimu_traverse_all_i2c((void *)&op, instantiator_callback, CANCEL_ON_ERROR);
        pcs_devices(SERVICE_START);
    }
    return 0;
}
