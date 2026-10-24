#include <linux/module.h>
#include <linux/i2c.h>
#include <linux/hwmon.h>
#include <linux/hwmon-sysfs.h>

struct l_pcs_i2c_data
{
    struct i2c_client   *client;
    struct mutex        lock;
    char                valid;
    unsigned long       last_updated;
    int                 temp;   /* m°C, or -errno */
};

static void l_pcs_i2c_update_device(struct device *dev)
{
    struct l_pcs_i2c_data *data = dev_get_drvdata(dev);
    mutex_lock(&data->lock);
    if (time_after(jiffies, data->last_updated + HZ + HZ / 2) || !data->valid)
    {
        /* 1/8 °C, big endian; 0: no sensor. In m°C that's up to
         * 8191875, more than an u16 holds (it wrapped at 65.5 °C) */
        u8 bytes[2];
        if (i2c_master_recv(data->client, bytes, 2) != 2 || !(bytes[0] | bytes[1]))
        {
            data->temp = -EIO;
            data->valid = 0;
        }
        else
        {
            data->temp = ((bytes[0] << 8) | bytes[1]) * 125;
            data->last_updated = jiffies;
            data->valid = 1;
        }
    }
    mutex_unlock(&data->lock);
}

static ssize_t temp_show(struct device *dev, struct device_attribute *da, char *buf)
{
    int temp;
    l_pcs_i2c_update_device(dev);
    temp = ((struct l_pcs_i2c_data *)dev_get_drvdata(dev))->temp;
    /* a failed read is an error, not 0 °C */
    return temp < 0 ? temp : sprintf(buf, "%d\n", temp);
}

static SENSOR_DEVICE_ATTR_RO(temp1_input, temp, 0);
static struct attribute *l_pcs_i2c_attrs[] = { &sensor_dev_attr_temp1_input.dev_attr.attr, NULL };
ATTRIBUTE_GROUPS(l_pcs_i2c);

static int l_pcs_i2c_probe(struct i2c_client *client)
{
    struct device *dev = &client->dev;
    struct l_pcs_i2c_data *data;
    struct device *hwmon_dev;
    if((data = devm_kzalloc(dev, sizeof(struct l_pcs_i2c_data), GFP_KERNEL)) == NULL) return -ENOMEM;
    i2c_set_clientdata(client, data);
    mutex_init(&data->lock);
    data->client = client;
    hwmon_dev = devm_hwmon_device_register_with_groups(dev, client->name, data, l_pcs_i2c_groups);
    return PTR_ERR_OR_ZERO(hwmon_dev);
}

static const struct i2c_device_id l_pcs_i2c_ids[] = { { KBUILD_MODNAME, 0 }, { } };
MODULE_DEVICE_TABLE(i2c, l_pcs_i2c_ids);

static struct i2c_driver l_pcs_i2c_driver =
{
    .driver         = { .name = KBUILD_MODNAME },
    .probe_new      = l_pcs_i2c_probe,
    .id_table       = l_pcs_i2c_ids,
};
module_i2c_driver(l_pcs_i2c_driver);

MODULE_AUTHOR("Igor Molchanov <akemi_homura@kurisa.ch>");
MODULE_DESCRIPTION("Elbrus PCS I2C access hwmon module");
MODULE_LICENSE("GPL");
MODULE_VERSION("1.0");
