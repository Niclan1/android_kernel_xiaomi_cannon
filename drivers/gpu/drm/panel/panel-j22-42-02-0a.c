// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (c) 2015 MediaTek Inc.
 * Copyright (C) 2021 XiaoMi, Inc.
 *
 * J22 42 02 0A video-mode panel
 *
 * Panel initialization sequence adapted from the GPL-2.0 Cannon 0A driver
 * by MediaTek and Xiaomi.
 */

#include <linux/backlight.h>
#include <linux/delay.h>
#include <linux/fb.h>
#include <linux/gpio/consumer.h>
#include <linux/module.h>
#include <linux/of_graph.h>
#include <linux/regulator/consumer.h>

#include <drm/drm_mipi_dsi.h>
#include <drm/drm_modes.h>
#include <drm/drm_panel.h>

#include <video/mipi_display.h>

struct j22_panel {
	struct drm_panel panel;
	struct mipi_dsi_device *dsi;
	struct gpio_desc *reset_gpio;
	struct gpio_desc *bias_pos_gpio;
	struct gpio_desc *bias_neg_gpio;
	struct regulator *bias_supply;
	bool bias_enabled;
	bool prepared;
};

static inline struct j22_panel *to_j22_panel(struct drm_panel *panel)
{
	return container_of(panel, struct j22_panel, panel);
}

static int j22_dcs_write0(struct j22_panel *ctx, u8 command)
{
	ssize_t ret;

	ret = mipi_dsi_dcs_write(ctx->dsi, command, NULL, 0);
	if (ret < 0)
		dev_err(&ctx->dsi->dev, "DCS write 0x%02x failed: %zd\n",
			command, ret);

	return ret < 0 ? ret : 0;
}

static void j22_power_on(struct j22_panel *ctx)
{
	gpiod_set_value_cansleep(ctx->bias_pos_gpio, 1);
	usleep_range(1000, 1500);
	gpiod_set_value_cansleep(ctx->bias_neg_gpio, 1);
	usleep_range(10000, 11000);

	gpiod_set_value_cansleep(ctx->reset_gpio, 1);
	usleep_range(5000, 5500);
	gpiod_set_value_cansleep(ctx->reset_gpio, 0);
	usleep_range(1000, 1500);
	gpiod_set_value_cansleep(ctx->reset_gpio, 1);
	usleep_range(5000, 5500);
	gpiod_set_value_cansleep(ctx->reset_gpio, 0);
	usleep_range(1000, 1500);
	gpiod_set_value_cansleep(ctx->reset_gpio, 1);
	usleep_range(12000, 13000);
}

static void j22_power_off(struct j22_panel *ctx)
{
	gpiod_set_value_cansleep(ctx->reset_gpio, 0);
	gpiod_set_value_cansleep(ctx->bias_neg_gpio, 0);
	usleep_range(1000, 1500);
	gpiod_set_value_cansleep(ctx->bias_pos_gpio, 0);
}

struct j22_init_cmd {
	u8 len;
	u8 data[9];
};

static int j22_panel_init_sequence(struct j22_panel *ctx)
{
	static const struct j22_init_cmd init_sequence[] = {
		{ 2, { 0xff, 0x24 } },
		{ 2, { 0xc3, 0x01 } },
		{ 2, { 0xc4, 0x25 } },
		{ 2, { 0xc7, 0x08 } },
		{ 2, { 0xff, 0x20 } },
		{ 2, { 0xfb, 0x01 } },
		{ 2, { 0x62, 0xaa } },
		{ 2, { 0x6d, 0x44 } },
		{ 2, { 0xff, 0x24 } },
		{ 2, { 0xfb, 0x01 } },
		{ 2, { 0x38, 0x68 } },
		{ 2, { 0x39, 0x68 } },
		{ 2, { 0x3f, 0x68 } },
		{ 2, { 0x7b, 0x9a } },
		{ 2, { 0x92, 0x73 } },
		{ 2, { 0x93, 0x22 } },
		{ 2, { 0x94, 0x08 } },
		{ 2, { 0xdf, 0x70 } },
		{ 2, { 0xff, 0x25 } },
		{ 2, { 0xfb, 0x01 } },
		{ 2, { 0x21, 0x1a } },
		{ 2, { 0x22, 0x1a } },
		{ 2, { 0x24, 0x73 } },
		{ 2, { 0x25, 0x73 } },
		{ 2, { 0x30, 0x2f } },
		{ 2, { 0x38, 0x2f } },
		{ 2, { 0x3f, 0x22 } },
		{ 2, { 0x40, 0x9b } },
		{ 2, { 0x4b, 0x22 } },
		{ 2, { 0x4c, 0x9b } },
		{ 2, { 0xd9, 0x48 } },
		{ 2, { 0xdb, 0x44 } },
		{ 2, { 0xff, 0x26 } },
		{ 2, { 0xfb, 0x01 } },
		{ 2, { 0x0c, 0x09 } },
		{ 2, { 0x0f, 0x05 } },
		{ 2, { 0x10, 0x06 } },
		{ 2, { 0x12, 0xea } },
		{ 2, { 0x19, 0x14 } },
		{ 2, { 0x1a, 0xda } },
		{ 2, { 0x1d, 0x14 } },
		{ 2, { 0x1e, 0x5d } },
		{ 2, { 0xff, 0x21 } },
		{ 2, { 0xfb, 0x01 } },
		{ 9, { 0xf7, 0x53, 0x42, 0x32, 0x01, 0x4a, 0x22, 0x31, 0x00 } },
		{ 2, { 0xff, 0x20 } },
		{ 2, { 0xfb, 0x01 } },
		{ 2, { 0xff, 0x10 } },
	};
	ssize_t ret;
	int err;
	unsigned int i;

	for (i = 0; i < ARRAY_SIZE(init_sequence); i++) {
		const struct j22_init_cmd *cmd = &init_sequence[i];

		if (cmd->data[0] < 0xb0)
			ret = mipi_dsi_dcs_write_buffer(ctx->dsi, cmd->data,
							cmd->len);
		else
			ret = mipi_dsi_generic_write(ctx->dsi, cmd->data,
						     cmd->len);
		if (ret < 0)
			return dev_err_probe(&ctx->dsi->dev, ret,
					     "failed to write panel init command %u\n",
					     i);
	}

	err = j22_dcs_write0(ctx, MIPI_DCS_EXIT_SLEEP_MODE);
	if (err)
		return err;
	msleep(70);

	return j22_dcs_write0(ctx, MIPI_DCS_SET_DISPLAY_ON);
}

static int j22_panel_prepare(struct drm_panel *panel)
{
	struct j22_panel *ctx = to_j22_panel(panel);
	int err;
	int ret;

	if (ctx->prepared)
		return 0;

	dev_dbg(&ctx->dsi->dev, "DIAG-PANEL prepare begin\n");
	if (!ctx->bias_enabled) {
		err = regulator_enable(ctx->bias_supply);
		if (err)
			return dev_err_probe(&ctx->dsi->dev, err,
					     "failed to enable panel bias supply\n");
		ctx->bias_enabled = true;
	}

	j22_power_on(ctx);
	dev_dbg(&ctx->dsi->dev, "DIAG-PANEL power-on done\n");
	err = j22_panel_init_sequence(ctx);
	dev_dbg(&ctx->dsi->dev, "DIAG-PANEL init sequence err=%d\n", err);
	if (!err) {
		u8 pm = 0xff, dm = 0xff;
		ssize_t r1, r2;

		r1 = mipi_dsi_dcs_read(ctx->dsi, MIPI_DCS_GET_POWER_MODE, &pm, 1);
		r2 = mipi_dsi_dcs_read(ctx->dsi, MIPI_DCS_GET_DISPLAY_MODE, &dm, 1);
		dev_dbg(&ctx->dsi->dev,
			 "DIAG-PANEL power_mode rd=%zd 0x%02x display_mode rd=%zd 0x%02x\n",
			 r1, pm, r2, dm);
	}
	if (err) {
		j22_power_off(ctx);
		ret = regulator_disable(ctx->bias_supply);
		if (ret)
			dev_err(&ctx->dsi->dev,
				"failed to disable panel bias after init error: %d\n",
				ret);
		ctx->bias_enabled = false;
		return err;
	}

	ctx->prepared = true;
	return 0;
}

static int j22_panel_unprepare(struct drm_panel *panel)
{
	struct j22_panel *ctx = to_j22_panel(panel);
	int err = 0;
	int ret;

	if (!ctx->prepared)
		return 0;

	ret = j22_dcs_write0(ctx, 0x28);
	if (ret)
		err = ret;
	ret = j22_dcs_write0(ctx, MIPI_DCS_ENTER_SLEEP_MODE);
	if (!err && ret)
		err = ret;
	msleep(60);

	if (drm_panel_has_wakeup_follower(panel)) {
		ctx->prepared = false;
		return err;
	}

	j22_power_off(ctx);
	if (ctx->bias_enabled) {
		ret = regulator_disable(ctx->bias_supply);
		if (!err && ret)
			err = ret;
		ctx->bias_enabled = false;
	}
	ctx->prepared = false;
	return err;
}

static int j22_panel_enable(struct drm_panel *panel)
{
	pr_debug("DIAG-PANEL enable (backlight on)\n");
	panel->backlight->props.power = FB_BLANK_UNBLANK;
	return backlight_update_status(panel->backlight);
}

static int j22_panel_disable(struct drm_panel *panel)
{
	panel->backlight->props.power = FB_BLANK_POWERDOWN;
	return backlight_update_status(panel->backlight);
}

static int j22_panel_get_modes(struct drm_panel *panel,
			       struct drm_connector *connector)
{
	static const struct drm_display_mode mode = {
		.clock = 184289,
		.hdisplay = 1080,
		.hsync_start = 1220,
		.hsync_end = 1240,
		.htotal = 1295,
		.vdisplay = 2340,
		.vsync_start = 2371,
		.vsync_end = 2373,
		.vtotal = 2381,
	};
	struct drm_display_mode *display_mode;

	display_mode = drm_mode_duplicate(connector->dev, &mode);
	if (!display_mode)
		return -ENOMEM;

	display_mode->type = DRM_MODE_TYPE_DRIVER | DRM_MODE_TYPE_PREFERRED;
	drm_mode_set_name(display_mode);
	drm_mode_probed_add(connector, display_mode);
	connector->display_info.width_mm = 71;
	connector->display_info.height_mm = 155;
	connector->display_info.bpc = 8;

	return 1;
}

static const struct drm_panel_funcs j22_panel_funcs = {
	.prepare = j22_panel_prepare,
	.unprepare = j22_panel_unprepare,
	.enable = j22_panel_enable,
	.disable = j22_panel_disable,
	.get_modes = j22_panel_get_modes,
};

static bool j22_panel_is_selected(struct device *dev)
{
	struct device_node *host, *endpoint, *remote;
	bool selected;

	host = of_get_parent(dev->of_node);
	if (!host)
		return false;

	endpoint = of_graph_get_next_endpoint(host, NULL);
	of_node_put(host);
	if (!endpoint)
		return false;

	remote = of_graph_get_remote_port_parent(endpoint);
	of_node_put(endpoint);
	if (!remote)
		return false;

	selected = remote == dev->of_node;
	of_node_put(remote);
	return selected;
}

static int j22_panel_probe(struct mipi_dsi_device *dsi)
{
	struct device *dev = &dsi->dev;
	struct j22_panel *ctx;
	int err;

	if (!j22_panel_is_selected(dev)) {
		dev_dbg(dev, "panel is not selected by the DSI graph\n");
		return -ENODEV;
	}

	ctx = devm_kzalloc(dev, sizeof(*ctx), GFP_KERNEL);
	if (!ctx)
		return -ENOMEM;

	ctx->dsi = dsi;
	mipi_dsi_set_drvdata(dsi, ctx);
	ctx->reset_gpio = devm_gpiod_get(dev, "reset", GPIOD_OUT_LOW);
	if (IS_ERR(ctx->reset_gpio))
		return dev_err_probe(dev, PTR_ERR(ctx->reset_gpio),
				     "failed to get reset GPIO\n");

	ctx->bias_pos_gpio = devm_gpiod_get_index(dev, "bias", 0,
						 GPIOD_OUT_LOW);
	if (IS_ERR(ctx->bias_pos_gpio))
		return dev_err_probe(dev, PTR_ERR(ctx->bias_pos_gpio),
				     "failed to get positive bias GPIO\n");

	ctx->bias_neg_gpio = devm_gpiod_get_index(dev, "bias", 1,
						 GPIOD_OUT_LOW);
	if (IS_ERR(ctx->bias_neg_gpio))
		return dev_err_probe(dev, PTR_ERR(ctx->bias_neg_gpio),
				     "failed to get negative bias GPIO\n");

	ctx->bias_supply = devm_regulator_get(dev, "bias");
	if (IS_ERR(ctx->bias_supply))
		return dev_err_probe(dev, PTR_ERR(ctx->bias_supply),
				     "failed to get LM36273 bias supply\n");

	dsi->lanes = 4;
	dsi->format = MIPI_DSI_FMT_RGB888;
	dsi->mode_flags = MIPI_DSI_MODE_VIDEO |
			  MIPI_DSI_MODE_VIDEO_SYNC_PULSE |
			  MIPI_DSI_MODE_LPM |
			  MIPI_DSI_CLOCK_NON_CONTINUOUS;

	drm_panel_init(&ctx->panel, dev, &j22_panel_funcs,
		       DRM_MODE_CONNECTOR_DSI);
	/* The MediaTek DSI host must be powered before the DCS init sequence. */
	ctx->panel.prepare_prev_first = true;
	err = drm_panel_of_backlight(&ctx->panel);
	if (err)
		return err;
	if (!ctx->panel.backlight)
		return dev_err_probe(dev, -ENODEV,
				     "missing LM36273 backlight reference\n");

	drm_panel_add(&ctx->panel);
	err = mipi_dsi_attach(dsi);
	if (err) {
		drm_panel_remove(&ctx->panel);
		return dev_err_probe(dev, err, "failed to attach DSI panel\n");
	}

	return 0;
}

static void j22_panel_remove(struct mipi_dsi_device *dsi)
{
	struct j22_panel *ctx = mipi_dsi_get_drvdata(dsi);

	drm_panel_disable(&ctx->panel);
	drm_panel_unprepare(&ctx->panel);
	if (ctx->bias_enabled) {
		j22_power_off(ctx);
		regulator_disable(ctx->bias_supply);
		ctx->bias_enabled = false;
	}
	mipi_dsi_detach(dsi);
	drm_panel_remove(&ctx->panel);
}

static const struct of_device_id j22_panel_of_match[] = {
	{ .compatible = "j22_42_02_0a,vdo" },
	{ }
};
MODULE_DEVICE_TABLE(of, j22_panel_of_match);

static struct mipi_dsi_driver j22_panel_driver = {
	.probe = j22_panel_probe,
	.remove = j22_panel_remove,
	.driver = {
		.name = "panel-j22-42-02-0a",
		.of_match_table = j22_panel_of_match,
	},
};
module_mipi_dsi_driver(j22_panel_driver);

MODULE_DESCRIPTION("J22 42 02 0A VDO DSI panel driver");
MODULE_LICENSE("GPL");
