// SPDX-License-Identifier: GPL-2.0-or-later
#include <linux/delay.h>
#include <linux/dmi.h>
#include <linux/hwmon.h>
#include <linux/io.h>
#include <linux/ioport.h>
#include <linux/ktime.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/platform_device.h>
#include <linux/slab.h>
#include <linux/string.h>

#include "ec_core.h"

struct ec_device {
	/* Serializes complete EC operations, PM snapshots and device teardown. */
	struct mutex lock;
	struct ec ec;
	struct device *dev;
};

static struct platform_device *ec_platform;

static u8 port_in(void *ctx, unsigned int port)
{
	return inb(port);
}

static void port_out(void *ctx, unsigned int port, u8 value)
{
	outb(value, port);
}

static u64 clock_us(void *ctx)
{
	return ktime_to_us(ktime_get());
}

static void sleep_us(void *ctx, unsigned int delay)
{
	usleep_range(delay, delay + 50);
}

static void recovery_error(void *ctx, int error)
{
	struct ec_device *data = ctx;

	dev_err_ratelimited(data->dev,
			    "EC recovery failed (%d); cooling state unconfirmed\n",
			    error);
}

static const struct ec_io port_ops = {
	.in = port_in,
	.out = port_out,
	.now_us = clock_us,
	.sleep_us = sleep_us,
	.recovery_error = recovery_error,
};

static umode_t ec_visible(const void *data, enum hwmon_sensor_types type,
			  u32 attr, int channel)
{
	if (type != hwmon_pwm)
		return 0;
	if (attr == hwmon_pwm_input && channel < EC_FAN_COUNT)
		return 0644;
	if (attr == hwmon_pwm_enable && channel == 0)
		return 0644;
	return 0;
}

static int ec_hwmon_read(struct device *dev, enum hwmon_sensor_types type,
			 u32 attr, int channel, long *value)
{
	struct ec_device *data = dev_get_drvdata(dev);
	int ret;

	if (type != hwmon_pwm)
		return -EOPNOTSUPP;
	mutex_lock(&data->lock);
	if (attr == hwmon_pwm_input)
		ret = ec_read_pwm(&data->ec, channel, value);
	else if (attr == hwmon_pwm_enable && channel == 0)
		ret = ec_read_enable(&data->ec, value);
	else
		ret = -EOPNOTSUPP;
	mutex_unlock(&data->lock);
	return ret;
}

static int ec_hwmon_write(struct device *dev, enum hwmon_sensor_types type,
			  u32 attr, int channel, long value)
{
	struct ec_device *data = dev_get_drvdata(dev);
	int ret;

	if (type != hwmon_pwm)
		return -EOPNOTSUPP;
	mutex_lock(&data->lock);
	if (attr == hwmon_pwm_input)
		ret = ec_write_pwm(&data->ec, channel, value);
	else if (attr == hwmon_pwm_enable && channel == 0)
		ret = ec_write_enable(&data->ec, value);
	else
		ret = -EOPNOTSUPP;
	mutex_unlock(&data->lock);
	if (ret)
		dev_warn_ratelimited(data->dev, "EC write failed: %d\n", ret);
	return ret;
}

static const struct hwmon_ops ec_hwmon_ops = {
	.is_visible = ec_visible,
	.read = ec_hwmon_read,
	.write = ec_hwmon_write,
};

static const struct hwmon_channel_info * const ec_channels[] = {
	HWMON_CHANNEL_INFO(pwm, HWMON_PWM_INPUT | HWMON_PWM_ENABLE,
			   HWMON_PWM_INPUT),
	NULL
};

static const struct hwmon_chip_info ec_chip = {
	.ops = &ec_hwmon_ops,
	.info = ec_channels,
};

static bool supported_machine(void)
{
	const char *vendor = dmi_get_system_info(DMI_SYS_VENDOR);
	const char *board = dmi_get_system_info(DMI_BOARD_NAME);

	return vendor && board && !strcmp(vendor, "ASUSTeK COMPUTER INC.") &&
	       !strncmp(board, "FX608", 5);
}

static int ec_platform_probe(struct platform_device *pdev)
{
	struct ec_device *data;
	struct device *hwmon;
	int ret;

	if (!supported_machine())
		return -ENODEV;
	data = devm_kzalloc(&pdev->dev, sizeof(*data), GFP_KERNEL);
	if (!data)
		return -ENOMEM;
	if (!devm_request_region(&pdev->dev, EC_DATA_PORT, 2, "asus_fan_ec"))
		return dev_err_probe(&pdev->dev, -EBUSY, "EC ports unavailable\n");
	mutex_init(&data->lock);
	data->dev = &pdev->dev;
	data->ec.io = &port_ops;
	data->ec.ctx = data;
	platform_set_drvdata(pdev, data);
	mutex_lock(&data->lock);
	ret = ec_probe(&data->ec);
	mutex_unlock(&data->lock);
	if (ret)
		return dev_err_probe(&pdev->dev, ret,
				     "EC validation/automatic mode failed\n");
	hwmon = devm_hwmon_device_register_with_info(&pdev->dev, "asus_fan_ec",
						     data, &ec_chip, NULL);
	if (IS_ERR(hwmon))
		return PTR_ERR(hwmon);
	dev_info(&pdev->dev, "FX608 EC fan control ready in automatic mode\n");
	return 0;
}

static int ec_pm_prepare(struct device *dev)
{
	struct ec_device *data = dev_get_drvdata(dev);
	int ret;

	mutex_lock(&data->lock);
	ret = ec_prepare(&data->ec);
	mutex_unlock(&data->lock);
	if (ret)
		dev_err(dev, "Cannot prepare cooling for sleep: %d\n", ret);
	return ret;
}

static void ec_pm_complete(struct device *dev)
{
	struct ec_device *data = dev_get_drvdata(dev);
	int ret;

	mutex_lock(&data->lock);
	ret = ec_restore(&data->ec);
	mutex_unlock(&data->lock);
	if (ret)
		dev_err_ratelimited(dev, "Cannot restore cooling after sleep: %d\n",
				    ret);
}

/* complete runs after every resume/thaw/restore, including cancelled sleep. */
static const struct dev_pm_ops ec_pm_ops = {
	.prepare = ec_pm_prepare,
	.complete = ec_pm_complete,
};

static void ec_platform_stop(struct platform_device *pdev)
{
	struct ec_device *data = platform_get_drvdata(pdev);
	int ret;

	mutex_lock(&data->lock);
	ret = ec_remove(&data->ec);
	mutex_unlock(&data->lock);
	if (ret)
		dev_err(&pdev->dev, "Cannot return fans to EC: %d\n", ret);
}

static struct platform_driver ec_driver = {
	.remove = ec_platform_stop,
	.shutdown = ec_platform_stop,
	.driver = {
		.name = "asus_fan_ec",
		.pm = &ec_pm_ops,
		.suppress_bind_attrs = true,
	},
};

static int __init ec_init(void)
{
	int ret;

	if (!supported_machine())
		return -ENODEV;
	ec_platform = platform_device_register_simple("asus_fan_ec", -1,
						      NULL, 0);
	if (IS_ERR(ec_platform))
		return PTR_ERR(ec_platform);
	ret = platform_driver_probe(&ec_driver, ec_platform_probe);
	if (ret)
		platform_device_unregister(ec_platform);
	return ret;
}

static void __exit ec_exit(void)
{
	platform_device_unregister(ec_platform);
	platform_driver_unregister(&ec_driver);
}

module_init(ec_init);
module_exit(ec_exit);
MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("ASUS FX608 proprietary EC fan duty control");
MODULE_AUTHOR("asus-fan-ec-driver contributors");
MODULE_VERSION("0.1.3");
