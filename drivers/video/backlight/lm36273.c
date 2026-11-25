// SPDX-License-Identifier: GPL-2.0-only
/*
 * TI LM36273 backlight and LCD bias driver
 *
 */

#include <linux/backlight.h>
#include <linux/bitops.h>
#include <linux/delay.h>
#include <linux/i2c.h>
#include <linux/leds.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/of.h>
#include <linux/regmap.h>
#include <linux/regulator/driver.h>

#define LM36273_REG_BC1		0x02
#define LM36273_REG_BC2		0x03
#define LM36273_REG_BRT_LSB	0x04
#define LM36273_REG_BRT_MSB	0x05
#define LM36273_REG_BL_ENABLE	0x08
#define LM36273_REG_BIAS_CONFIG	0x09
#define LM36273_REG_BIAS_BOOST	0x0c
#define LM36273_REG_VPOS		0x0d
#define LM36273_REG_VNEG		0x0e
#define LM36273_REG_MAX		0x13

#define LM36273_MAX_BRIGHTNESS	2047
#define LM36273_BL_ENABLE_VALUE	0x17
#define LM36273_BIAS_VPOS_ENABLE	0x9c
#define LM36273_BIAS_BOTH_ENABLE	0x9e
#define LM36273_BIAS_VNEG_DISABLE	0x98
#define LM36273_BIAS_ALL_DISABLE	0x18

struct lm36273 {
	struct device *dev;
	struct regmap *regmap;
	struct mutex lock;
	struct backlight_device *backlight;
	struct led_classdev led;
	unsigned int brightness;
};

/* DIAG only: dump every register, 0xffff marks a failed read */
static void lm36273_diag_dump(struct lm36273 *chip, const char *tag)
{
	unsigned int r, v;

	for (r = 0x01; r <= LM36273_REG_MAX; r++) {
		if (regmap_read(chip->regmap, r, &v))
			v = 0xffff;
		pr_debug("DIAG-LM36273 %s reg%02x=%04x\n", tag, r, v);
	}
}

static int lm36273_bias_disable_locked(struct lm36273 *chip)
{
	int err = 0;
	int ret;

	ret = regmap_write(chip->regmap, LM36273_REG_BIAS_CONFIG,
			   LM36273_BIAS_VPOS_ENABLE);
	if (ret)
		err = ret;
	usleep_range(3000, 3500);

	ret = regmap_write(chip->regmap, LM36273_REG_BIAS_CONFIG,
			   LM36273_BIAS_VNEG_DISABLE);
	if (!err && ret)
		err = ret;

	ret = regmap_write(chip->regmap, LM36273_REG_BIAS_CONFIG,
			   LM36273_BIAS_ALL_DISABLE);
	if (!err && ret)
		err = ret;

	return err;
}

static int lm36273_bias_enable_locked(struct lm36273 *chip)
{
	int ret;

	ret = regmap_write(chip->regmap, LM36273_REG_BIAS_CONFIG,
			   LM36273_BIAS_VPOS_ENABLE);
	if (ret)
		return ret;
	usleep_range(3000, 3500);

	ret = regmap_write(chip->regmap, LM36273_REG_BIAS_CONFIG,
			   LM36273_BIAS_BOTH_ENABLE);
	if (ret) {
		int err;

		err = lm36273_bias_disable_locked(chip);
		if (err)
			dev_err(chip->dev,
				"failed to shut down bias after enable error: %d\n",
				err);
	}

	pr_debug("DIAG-LM36273 bias-on ret=%d\n", ret);
	lm36273_diag_dump(chip, "bias-on");
	return ret;
}

static int lm36273_regulator_enable(struct regulator_dev *rdev)
{
	struct lm36273 *chip = rdev_get_drvdata(rdev);
	int ret;

	mutex_lock(&chip->lock);
	ret = lm36273_bias_enable_locked(chip);
	mutex_unlock(&chip->lock);

	return ret;
}

static int lm36273_regulator_disable(struct regulator_dev *rdev)
{
	struct lm36273 *chip = rdev_get_drvdata(rdev);
	int ret;

	mutex_lock(&chip->lock);
	ret = lm36273_bias_disable_locked(chip);
	mutex_unlock(&chip->lock);

	return ret;
}

static int lm36273_regulator_is_enabled(struct regulator_dev *rdev)
{
	struct lm36273 *chip = rdev_get_drvdata(rdev);
	unsigned int val;
	int ret;

	mutex_lock(&chip->lock);
	ret = regmap_read(chip->regmap, LM36273_REG_BIAS_CONFIG, &val);
	mutex_unlock(&chip->lock);
	if (ret)
		return ret;

	return (val & (BIT(1) | BIT(2))) == (BIT(1) | BIT(2));
}

static const struct regulator_ops lm36273_regulator_ops = {
	.enable = lm36273_regulator_enable,
	.disable = lm36273_regulator_disable,
	.is_enabled = lm36273_regulator_is_enabled,
};

static const struct regulator_desc lm36273_bias_desc = {
	.name = "lm36273-bias",
	.of_match = "bias",
	.type = REGULATOR_VOLTAGE,
	.owner = THIS_MODULE,
	.n_voltages = 1,
	.fixed_uV = 5500000,
	.ops = &lm36273_regulator_ops,
};

static int lm36273_set_brightness_locked(struct lm36273 *chip,
					 unsigned int brightness)
{
	int ret;

	if (brightness == chip->brightness)
		return 0;

	if (brightness) {
		if (!chip->brightness) {
			ret = regmap_write(chip->regmap, LM36273_REG_BC2, 0xcd);
			if (ret)
				return ret;
		}

		ret = regmap_write(chip->regmap, LM36273_REG_BRT_LSB,
				   brightness & 0x07);
		if (ret)
			return ret;
		ret = regmap_write(chip->regmap, LM36273_REG_BRT_MSB,
				   brightness >> 3);
		if (ret)
			return ret;
		ret = regmap_write(chip->regmap, LM36273_REG_BL_ENABLE,
				   LM36273_BL_ENABLE_VALUE);
		if (ret)
			return ret;
		if (!chip->brightness)
			lm36273_diag_dump(chip, "bl-on");
	} else {
		ret = regmap_write(chip->regmap, LM36273_REG_BRT_LSB, 0);
		if (ret)
			return ret;
		ret = regmap_write(chip->regmap, LM36273_REG_BRT_MSB, 0);
		if (ret)
			return ret;
		ret = regmap_write(chip->regmap, LM36273_REG_BL_ENABLE, 0);
		if (ret)
			return ret;
	}

	chip->brightness = brightness;
	return 0;
}

static int lm36273_backlight_update_status(struct backlight_device *backlight)
{
	struct lm36273 *chip = bl_get_data(backlight);
	int brightness = backlight_get_brightness(backlight);
	int ret;

	mutex_lock(&chip->lock);
	ret = lm36273_set_brightness_locked(chip, brightness);
	mutex_unlock(&chip->lock);

	return ret;
}

static int lm36273_led_set(struct led_classdev *led,
			   enum led_brightness brightness)
{
	struct lm36273 *chip = container_of(led, struct lm36273, led);

	return backlight_device_set_brightness(chip->backlight, brightness);
}

static enum led_brightness lm36273_led_get(struct led_classdev *led)
{
	struct lm36273 *chip = container_of(led, struct lm36273, led);

	return READ_ONCE(chip->backlight->props.brightness);
}

static const struct backlight_ops lm36273_backlight_ops = {
	.update_status = lm36273_backlight_update_status,
};

static const struct regmap_config lm36273_regmap_config = {
	.reg_bits = 8,
	.val_bits = 8,
	.max_register = LM36273_REG_MAX,
};

static const struct reg_sequence lm36273_init_sequence[] = {
	{ LM36273_REG_BC1, 0x60 },
	{ LM36273_REG_BC2, 0x85 },
	{ LM36273_REG_BL_ENABLE, 0x00 },
	{ LM36273_REG_BIAS_BOOST, 0x24 },
	{ LM36273_REG_VPOS, 0x1e },
	{ LM36273_REG_VNEG, 0x1e },
	{ LM36273_REG_BIAS_CONFIG, LM36273_BIAS_ALL_DISABLE },
};

static void lm36273_shutdown(void *data)
{
	struct lm36273 *chip = data;
	int ret;

	mutex_lock(&chip->lock);
	ret = regmap_write(chip->regmap, LM36273_REG_BL_ENABLE, 0);
	if (ret)
		dev_err(chip->dev, "failed to disable backlight: %d\n", ret);
	ret = lm36273_bias_disable_locked(chip);
	if (ret)
		dev_err(chip->dev, "failed to disable panel bias: %d\n", ret);
	mutex_unlock(&chip->lock);
}

static int lm36273_probe(struct i2c_client *client)
{
	struct device *dev = &client->dev;
	struct backlight_properties props = {
		.type = BACKLIGHT_RAW,
		.brightness = 512,
		.max_brightness = LM36273_MAX_BRIGHTNESS,
	};
	struct regulator_config regulator_cfg = {
		.dev = dev,
		.of_node = dev_of_node(dev),
	};
	struct regulator_dev *rdev;
	struct backlight_device *backlight;
	struct lm36273 *chip;
	int ret;

	chip = devm_kzalloc(dev, sizeof(*chip), GFP_KERNEL);
	if (!chip)
		return -ENOMEM;

	chip->dev = dev;
	mutex_init(&chip->lock);
	chip->regmap = devm_regmap_init_i2c(client, &lm36273_regmap_config);
	if (IS_ERR(chip->regmap))
		return dev_err_probe(dev, PTR_ERR(chip->regmap),
				     "failed to initialize register map\n");

	i2c_set_clientdata(client, chip);

	lm36273_diag_dump(chip, "lk");
	ret = regmap_multi_reg_write(chip->regmap, lm36273_init_sequence,
				     ARRAY_SIZE(lm36273_init_sequence));
	if (ret)
		return dev_err_probe(dev, ret, "failed to configure LM36273\n");
	lm36273_diag_dump(chip, "probe");

	ret = devm_add_action_or_reset(dev, lm36273_shutdown, chip);
	if (ret)
		return ret;

	regulator_cfg.driver_data = chip;
	rdev = devm_regulator_register(dev, &lm36273_bias_desc, &regulator_cfg);
	if (IS_ERR(rdev))
		return dev_err_probe(dev, PTR_ERR(rdev),
				     "failed to register panel bias supply\n");

	backlight = devm_backlight_device_register(dev, "lm36273-backlight",
						  dev, chip,
						  &lm36273_backlight_ops,
						  &props);
	if (IS_ERR(backlight))
		return dev_err_probe(dev, PTR_ERR(backlight),
				     "failed to register backlight\n");

	/* Keep the stock cannon light HAL's LED-class interface. */
	chip->backlight = backlight;
	chip->led.name = "lcd-backlight";
	chip->led.max_brightness = LM36273_MAX_BRIGHTNESS;
	chip->led.brightness_set_blocking = lm36273_led_set;
	chip->led.brightness_get = lm36273_led_get;
	ret = devm_led_classdev_register(dev, &chip->led);
	if (ret)
		return dev_err_probe(dev, ret, "failed to register backlight LED\n");

	return 0;
}

static const struct of_device_id lm36273_of_match[] = {
	{ .compatible = "ti,lm36273" },
	{ .compatible = "mediatek,i2c_lcd_bias" },
	{ .compatible = "mediatek,I2C_LCD_BIAS" },
	{}
};
MODULE_DEVICE_TABLE(of, lm36273_of_match);

static const struct i2c_device_id lm36273_i2c_ids[] = {
	{ "lm36273", 0 },
	{ "I2C_LCD_BIAS", 0 },
	{}
};
MODULE_DEVICE_TABLE(i2c, lm36273_i2c_ids);

static struct i2c_driver lm36273_driver = {
	.driver = {
		.name = "lm36273",
		.of_match_table = lm36273_of_match,
	},
	.probe = lm36273_probe,
	.id_table = lm36273_i2c_ids,
};
module_i2c_driver(lm36273_driver);

MODULE_DESCRIPTION("TI LM36273 backlight and LCD bias driver");
MODULE_LICENSE("GPL");
