// SPDX-License-Identifier: GPL-2.0+
/*
 * Copyright (c) 2023 Andi Powers-Holmes <aholmes@omnom.net>
 * This is a DRM panel driver for the display in the ClockworkPi uConsole, "CWU50"
 * It is a 5" 720x1280 display with an ILI9881C controller.
 */

#include <linux/delay.h>
#include <linux/gpio/consumer.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/regulator/consumer.h>

#include <drm/drm_mipi_dsi.h>
#include <drm/drm_modes.h>
#include <drm/drm_panel.h>

#include <video/mipi_display.h>

#define CWU50_INIT_CMD_LEN		2


struct cwu50_panel {
	struct drm_panel panel;
	struct mipi_dsi_device *dsi;
	struct regulator *vci;
	struct regulator *iovcc;
	struct gpio_desc *reset_gpio;
	bool prepared;
};

static inline struct cwu50_panel *to_cwu50_panel(struct drm_panel *panel)
{
	return container_of(panel, struct cwu50_panel, panel);
}

struct cwu50_init_cmd {
	u8 data[CWU50_INIT_CMD_LEN];
};

static const struct cwu50_init_cmd cwu50_panel_init_cmds[] = {
	/* Switch to page 0 */
	{ .data = { 0xE0, 0x00 } },

	/* Unlock programming registers */
	{ .data = { 0xE1, 0x93 } },
	{ .data = { 0xE2, 0x65 } },
	{ .data = { 0xE3, 0xF8 } },

	/* Sequence control? */
	{ .data = { 0x70, 0x20 } },
	{ .data = { 0x71, 0x13 } },
	{ .data = { 0x72, 0x06 } },

	/* Lane count? */
	{ .data = { 0x75, 0x03 } },

	/* Switch to page 1 */
	{ .data = { 0xE0, 0x01 } },

	/* Set VCOM */
	{ .data = { 0x00, 0x00 } },
	{ .data = { 0x01, 0x47 } }, // VCOM 0x47
	/* Set VCOM_Reverse */
	{ .data = { 0x03, 0x00 } },
	{ .data = { 0x04, 0x4D } },
	/* Set a mystery regulator */
	{ .data = { 0x0C, 0x64 } },

	/* Set Gamma Power, VG[MS][PN] */
	{ .data = { 0x17, 0x00 } },
	{ .data = { 0x18, 0xBF } },
	{ .data = { 0x19, 0x00 } },
	{ .data = { 0x1A, 0x00 } },
	{ .data = { 0x1B, 0xBF } },
	{ .data = { 0x1C, 0x00 } },

	/* Set Gate Power */
	{ .data = { 0x1F, 0x7E } },
	{ .data = { 0x20, 0x24 } },
	{ .data = { 0x21, 0x24 } },
	{ .data = { 0x22, 0x4E } },
	{ .data = { 0x24, 0xFE } },

	/* Set Panel */
	{ .data = { 0x37, 0x09 } }, // SS = 1, BGR = 1

	/* Set RGBCYC */
	{ .data = { 0x38, 0x04 } },
	{ .data = { 0x3C, 0x76 } },
	{ .data = { 0x3D, 0xFF } }, // set CHGEN_ON
	{ .data = { 0x3E, 0xFF } }, // set CHGEN_OFF
	{ .data = { 0x3F, 0x7F } }, // set CHGEN_OFF2

	/* Set TCON parameter */
	{ .data = { 0x40, 0x04 } }, // Dot inversion type
	{ .data = { 0x41, 0xA0 } }, // LN = 1280 lines
	{ .data = { 0x44, 0x11 } }, // ????

	/* Set power voltage */
	{ .data = { 0x55, 0x02 } },
	{ .data = { 0x56, 0x01 } },
	{ .data = { 0x57, 0x49 } },
	{ .data = { 0x58, 0x09 } },
	{ .data = { 0x59, 0x2A } },
	{ .data = { 0x5A, 0x1A } },
	{ .data = { 0x5B, 0x1A } },

	/* Set gamma */
	{ .data = { 0x5D, 0x78 } },
	{ .data = { 0x5E, 0x6E } },
	{ .data = { 0x5F, 0x66 } },
	{ .data = { 0x60, 0x5E } },
	{ .data = { 0x61, 0x60 } },
	{ .data = { 0x62, 0x54 } },
	{ .data = { 0x63, 0x5C } },
	{ .data = { 0x64, 0x47 } },
	{ .data = { 0x65, 0x5F } },
	{ .data = { 0x66, 0x5D } },
	{ .data = { 0x67, 0x5B } },
	{ .data = { 0x68, 0x76 } },
	{ .data = { 0x69, 0x61 } },
	{ .data = { 0x6A, 0x63 } },
	{ .data = { 0x6B, 0x50 } },
	{ .data = { 0x6C, 0x45 } },
	{ .data = { 0x6D, 0x34 } },
	{ .data = { 0x6E, 0x1C } },
	{ .data = { 0x6F, 0x07 } },
	{ .data = { 0x70, 0x78 } },
	{ .data = { 0x71, 0x6E } },
	{ .data = { 0x72, 0x66 } },
	{ .data = { 0x73, 0x5E } },
	{ .data = { 0x74, 0x60 } },
	{ .data = { 0x75, 0x54 } },
	{ .data = { 0x76, 0x5C } },
	{ .data = { 0x77, 0x47 } },
	{ .data = { 0x78, 0x5F } },
	{ .data = { 0x79, 0x5D } },
	{ .data = { 0x7A, 0x5B } },
	{ .data = { 0x7B, 0x76 } },
	{ .data = { 0x7C, 0x61 } },
	{ .data = { 0x7D, 0x63 } },
	{ .data = { 0x7E, 0x50 } },
	{ .data = { 0x7F, 0x45 } },
	{ .data = { 0x80, 0x34 } },
	{ .data = { 0x81, 0x1C } },
	{ .data = { 0x82, 0x07 } },

	/* Switch to page 2, for GIP */
	{ .data = { 0xE0, 0x02 } },

	{ .data = { 0x00, 0x44 } },
	{ .data = { 0x01, 0x46 } },
	{ .data = { 0x02, 0x48 } },
	{ .data = { 0x03, 0x4A } },
	{ .data = { 0x04, 0x40 } },
	{ .data = { 0x05, 0x42 } },
	{ .data = { 0x06, 0x1F } },
	{ .data = { 0x07, 0x1F } },
	{ .data = { 0x08, 0x1F } },
	{ .data = { 0x09, 0x1F } },
	{ .data = { 0x0A, 0x1F } },
	{ .data = { 0x0B, 0x1F } },
	{ .data = { 0x0C, 0x1F } },
	{ .data = { 0x0D, 0x1F } },
	{ .data = { 0x0E, 0x1F } },
	{ .data = { 0x0F, 0x1F } },
	{ .data = { 0x10, 0x1F } },
	{ .data = { 0x11, 0x1F } },
	{ .data = { 0x12, 0x1F } },
	{ .data = { 0x13, 0x1F } },
	{ .data = { 0x14, 0x1E } },
	{ .data = { 0x15, 0x1F } },

	{ .data = { 0x16, 0x45 } },
	{ .data = { 0x17, 0x47 } },
	{ .data = { 0x18, 0x49 } },
	{ .data = { 0x19, 0x4B } },
	{ .data = { 0x1A, 0x41 } },
	{ .data = { 0x1B, 0x43 } },
	{ .data = { 0x1C, 0x1F } },
	{ .data = { 0x1D, 0x1F } },
	{ .data = { 0x1E, 0x1F } },
	{ .data = { 0x1F, 0x1F } },
	{ .data = { 0x20, 0x1F } },
	{ .data = { 0x21, 0x1F } },
	{ .data = { 0x22, 0x1F } },
	{ .data = { 0x23, 0x1F } },
	{ .data = { 0x24, 0x1F } },
	{ .data = { 0x25, 0x1F } },
	{ .data = { 0x26, 0x1F } },
	{ .data = { 0x27, 0x1F } },
	{ .data = { 0x28, 0x1F } },
	{ .data = { 0x29, 0x1F } },
	{ .data = { 0x2A, 0x1E } },
	{ .data = { 0x2B, 0x1F } },

	{ .data = { 0x2C, 0x0B } },
	{ .data = { 0x2D, 0x09 } },
	{ .data = { 0x2E, 0x07 } },
	{ .data = { 0x2F, 0x05 } },
	{ .data = { 0x30, 0x03 } },
	{ .data = { 0x31, 0x01 } },
	{ .data = { 0x32, 0x1F } },
	{ .data = { 0x33, 0x1F } },
	{ .data = { 0x34, 0x1F } },
	{ .data = { 0x35, 0x1F } },
	{ .data = { 0x36, 0x1F } },
	{ .data = { 0x37, 0x1F } },
	{ .data = { 0x38, 0x1F } },
	{ .data = { 0x39, 0x1F } },
	{ .data = { 0x3A, 0x1F } },
	{ .data = { 0x3B, 0x1F } },
	{ .data = { 0x3C, 0x1F } },
	{ .data = { 0x3D, 0x1F } },
	{ .data = { 0x3E, 0x1F } },
	{ .data = { 0x3F, 0x1F } },
	{ .data = { 0x40, 0x1F } },
	{ .data = { 0x41, 0x1E } },

	{ .data = { 0x42, 0x0A } },
	{ .data = { 0x43, 0x08 } },
	{ .data = { 0x44, 0x06 } },
	{ .data = { 0x45, 0x04 } },
	{ .data = { 0x46, 0x02 } },
	{ .data = { 0x47, 0x00 } },
	{ .data = { 0x48, 0x1F } },
	{ .data = { 0x49, 0x1F } },
	{ .data = { 0x4A, 0x1F } },
	{ .data = { 0x4B, 0x1F } },
	{ .data = { 0x4C, 0x1F } },
	{ .data = { 0x4D, 0x1F } },
	{ .data = { 0x4E, 0x1F } },
	{ .data = { 0x4F, 0x1F } },
	{ .data = { 0x50, 0x1F } },
	{ .data = { 0x51, 0x1F } },
	{ .data = { 0x52, 0x1F } },
	{ .data = { 0x53, 0x1F } },
	{ .data = { 0x54, 0x1F } },
	{ .data = { 0x55, 0x1F } },
	{ .data = { 0x56, 0x1F } },
	{ .data = { 0x57, 0x1E } },

	{ .data = { 0x58, 0x40 } },
	{ .data = { 0x59, 0x00 } },
	{ .data = { 0x5A, 0x00 } },
	{ .data = { 0x5B, 0x30 } },
	{ .data = { 0x5C, 0x02 } },
	{ .data = { 0x5D, 0x40 } },
	{ .data = { 0x5E, 0x01 } },
	{ .data = { 0x5F, 0x02 } },
	{ .data = { 0x60, 0x00 } },
	{ .data = { 0x61, 0x01 } },
	{ .data = { 0x62, 0x02 } },
	{ .data = { 0x63, 0x65 } },
	{ .data = { 0x64, 0x66 } },
	{ .data = { 0x65, 0x00 } },
	{ .data = { 0x66, 0x00 } },
	{ .data = { 0x67, 0x74 } },
	{ .data = { 0x68, 0x06 } },
	{ .data = { 0x69, 0x65 } },
	{ .data = { 0x6A, 0x66 } },
	{ .data = { 0x6B, 0x10 } },

	{ .data = { 0x6C, 0x00 } },
	{ .data = { 0x6D, 0x04 } },
	{ .data = { 0x6E, 0x04 } },
	{ .data = { 0x6F, 0x88 } },
	{ .data = { 0x70, 0x00 } },
	{ .data = { 0x71, 0x00 } },
	{ .data = { 0x72, 0x06 } },
	{ .data = { 0x73, 0x7B } },
	{ .data = { 0x74, 0x00 } },
	{ .data = { 0x75, 0x87 } },
	{ .data = { 0x76, 0x00 } },
	{ .data = { 0x77, 0x5D } },
	{ .data = { 0x78, 0x17 } },
	{ .data = { 0x79, 0x1F } },
	{ .data = { 0x7A, 0x00 } },
	{ .data = { 0x7B, 0x00 } },
	{ .data = { 0x7C, 0x00 } },
	{ .data = { 0x7D, 0x03 } },
	{ .data = { 0x7E, 0x7B } },

	/* Switch to page 4 and do... something */
	{ .data = { 0xE0, 0x04 } },
	{ .data = { 0x09, 0x10 } },

	/* Switch back to page 0 and re-lock the programming registers */
	{ .data = { 0xE0, 0x00 } },
	{ .data = { 0xE6, 0x02 } },
	{ .data = { 0xE7, 0x02 } },
};

static void cwu50_reset(struct cwu50_panel *ctx)
{
	dev_dbg(&ctx->dsi->dev, "Resetting panel\n");
	gpiod_set_value_cansleep(ctx->reset_gpio, 1);
	usleep_range(10000, 20000);
	gpiod_set_value_cansleep(ctx->reset_gpio, 0);
	usleep_range(10000, 20000);
	gpiod_set_value_cansleep(ctx->reset_gpio, 1);
	msleep(120);
}

static int cwu50_init_sequence(struct cwu50_panel *ctx)
{
	struct mipi_dsi_device *dsi = ctx->dsi;

	int ret, i = 0;

	if (ctx->prepared)
		return 0;

	/* Send the init sequence */
	dev_dbg(&dsi->dev, "Sending initialization sequence\n");
	for (i = 0; i < ARRAY_SIZE(cwu50_panel_init_cmds); i++) {
		const struct cwu50_init_cmd *cmd = &cwu50_panel_init_cmds[i];

		ret = mipi_dsi_dcs_write_buffer(dsi, cmd->data, CWU50_INIT_CMD_LEN);
		if (ret < 0) {
			dev_err_ratelimited(&dsi->dev, "sending command %#02x failed: %d\n", cmd->data[0], ret);
			return ret;
		}
	}

	/* Exit sleep mode */
	dev_dbg(&dsi->dev, "Exiting sleep mode\n");
	ret = mipi_dsi_dcs_exit_sleep_mode(dsi);
	if (ret < 0) {
		dev_err(&dsi->dev, "failed to exit sleep mode at init: %d\n", ret);
		return ret;
	}

	return 0;
}

static int cwu50_prepare(struct drm_panel *panel)
{
	struct cwu50_panel *ctx = to_cwu50_panel(panel);
	int ret;

	if (ctx->prepared)
		return 0;

	dev_dbg(panel->dev, "Enabling regulators\n");
	ret = regulator_enable(ctx->iovcc);
	if (ret < 0) {
		dev_err(panel->dev, "failed to enable iovcc supply: %d\n", ret);
		return ret;
	}
	ret = regulator_enable(ctx->vci);
	if (ret < 0) {
		dev_err(panel->dev, "failed to enable vci supply: %d\n", ret);
		goto disable_iovcc;
	}

	/* Cycle reset pin */
	cwu50_reset(ctx);

	/* Send initialization sequence */
	ret = cwu50_init_sequence(ctx);
	if (ret < 0) {
		dev_err(panel->dev, "Failed to initialize panel: %d\n", ret);
		gpiod_set_value_cansleep(ctx->reset_gpio, 0);
		goto disable_vci;
	}

	ctx->prepared = true;

	return 0;

disable_vci:
	regulator_disable(ctx->vci);
disable_iovcc:
	regulator_disable(ctx->iovcc);
	return ret;
}

static int cwu50_enable(struct drm_panel *panel)
{
	struct cwu50_panel *ctx = to_cwu50_panel(panel);
	int ret;

	msleep(120);
	ret = mipi_dsi_dcs_set_display_on(ctx->dsi);
	if (ret < 0) {
		dev_err(panel->dev, "failed to set display on: %d\n", ret);
		return ret;
	}
	msleep(20);

	/* Set tearing on */
	ret = mipi_dsi_dcs_set_tear_on(ctx->dsi, MIPI_DSI_DCS_TEAR_MODE_VBLANK);
	if (ret < 0) {
		dev_err(panel->dev, "failed to enable vblank TE: %d\n", ret);
		return ret;
	}
	return 0;
}

static int cwu50_disable(struct drm_panel *panel)
{
	struct cwu50_panel *ctx = to_cwu50_panel(panel);

	return mipi_dsi_dcs_set_display_off(ctx->dsi);
}

static int cwu50_unprepare(struct drm_panel *panel)
{
	struct cwu50_panel *ctx = to_cwu50_panel(panel);
	struct mipi_dsi_device *dsi = ctx->dsi;
	int ret;

	if (!ctx->prepared)
		return 0;

	/* Set display off */
	ret = mipi_dsi_dcs_set_display_off(dsi);
	if (ret < 0)
		dev_err(panel->dev, "Failed to turn off panel: %d\n", ret);
	msleep(20);

	/* Enter sleep mode */
	ret = mipi_dsi_dcs_enter_sleep_mode(dsi);
	if (ret < 0)
		dev_err(panel->dev, "Failed to turn off panel: %d\n", ret);
	msleep(120);

	/* Put panel in RESET */
	gpiod_set_value_cansleep(ctx->reset_gpio, 0);
	msleep(20);

	/* Disable regulators */
	ret = regulator_disable(ctx->vci);
	if (ret < 0)
		dev_err(panel->dev, "failed to disable vci supply: %d\n", ret);
	usleep_range(1000, 20000);

	ret = regulator_disable(ctx->iovcc);
	if (ret < 0)
		dev_err(panel->dev, "failed to disable iovcc supply: %d\n", ret);

	ctx->prepared = false;

	return 0;
}

static const struct drm_display_mode cwu50_default_mode = {
	.clock = 62500,

	.hdisplay = 720,
	.hsync_start = 720 + 43,
	.hsync_end = 720+ 43 + 20,
	.htotal = 720 + 43 + 20 + 20,

	.vdisplay = 1280,
	.vsync_start = 1280 + 8,
	.vsync_end = 1280 + 8+ 2,
	.vtotal = 1280 + 8 + 2 + 16,

	.width_mm = 64,
	.height_mm = 114,
};

static int cwu50_get_modes(struct drm_panel *panel, struct drm_connector *connector)
{
	struct drm_display_mode *mode;

	mode = drm_mode_duplicate(connector->dev, &cwu50_default_mode);
	if (!mode) {
		dev_err(panel->dev, "failed to add mode %ux%u@%u\n",
			cwu50_default_mode.hdisplay,
			cwu50_default_mode.vdisplay,
			drm_mode_vrefresh(&cwu50_default_mode));
		return -ENOMEM;
	}

	drm_mode_set_name(mode);

	mode->type = DRM_MODE_TYPE_DRIVER | DRM_MODE_TYPE_PREFERRED;
	connector->display_info.width_mm = mode->width_mm;
	connector->display_info.height_mm = mode->height_mm;
	drm_mode_probed_add(connector, mode);

	return 1; // number of modes
}

static const struct drm_panel_funcs cwu50_panel_funcs = {
	.prepare = cwu50_prepare,
	.enable = cwu50_enable,
	.disable = cwu50_disable,
	.unprepare = cwu50_unprepare,
	.get_modes = cwu50_get_modes,
};

static int cwu50_probe(struct mipi_dsi_device *dsi)
{
	struct device *dev = &dsi->dev;
	struct cwu50_panel *ctx;
	int ret;

	ctx = devm_kzalloc(dev, sizeof(*ctx), GFP_KERNEL);
	if (!ctx)
		return -ENOMEM;

	ctx->reset_gpio = devm_gpiod_get(dev, "reset", GPIOD_OUT_HIGH);
	if (IS_ERR(ctx->reset_gpio))
		return dev_err_probe(dev, PTR_ERR(ctx->reset_gpio), "Failed to get reset-gpios\n");

	ctx->vci = devm_regulator_get(dev, "vci");
	if (IS_ERR(ctx->vci))
		return dev_err_probe(dev, PTR_ERR(ctx->vci), "Failed to get vci regulator\n");

	ctx->iovcc = devm_regulator_get(dev, "iovcc");
	if (IS_ERR(ctx->iovcc))
		return dev_err_probe(dev, PTR_ERR(ctx->iovcc), "Failed to get iovcc regulator\n");

	ctx->dsi = dsi;
	mipi_dsi_set_drvdata(dsi, ctx);

	dsi->lanes = 4;
	dsi->format = MIPI_DSI_FMT_RGB888;
	dsi->mode_flags = MIPI_DSI_MODE_VIDEO | MIPI_DSI_MODE_VIDEO_BURST |
			  MIPI_DSI_MODE_VIDEO_SYNC_PULSE;

	drm_panel_init(&ctx->panel, dev, &cwu50_panel_funcs, DRM_MODE_CONNECTOR_DSI);
	/* Ensure DSI host is ready before _prepare() runs */
	ctx->panel.prepare_upstream_first = true;

	ret = drm_panel_of_backlight(&ctx->panel);
	if (ret < 0)
		return dev_err_probe(dev, ret, "Failed to get backlight\n");

	drm_panel_add(&ctx->panel);

	ret = mipi_dsi_attach(dsi);
	if (ret < 0) {
		dev_err(dev, "mipi_dsi_attach() failed: %d\n", ret);
		drm_panel_remove(&ctx->panel);
		return ret;
	}

	return 0;
}

static void cwu50_remove(struct mipi_dsi_device *dsi)
{
	struct cwu50_panel *ctx = mipi_dsi_get_drvdata(dsi);
	int ret;

	ret = mipi_dsi_detach(dsi);
	if (ret < 0)
		dev_err(&dsi->dev, "failed to detach from DSI host: %d\n", ret);

	drm_panel_remove(&ctx->panel);
}


static const struct of_device_id cwu50_of_match[] = {
	{ .compatible = "cw,cwu50" },
	{ /* sentinel */ },
};
MODULE_DEVICE_TABLE(of, cwu50_of_match);

static struct mipi_dsi_driver cwu50_driver = {
	.probe = cwu50_probe,
	.remove = cwu50_remove,
	.driver = {
		.name = "panel-cw-cwu50",
		.of_match_table = cwu50_of_match,
	},
};
module_mipi_dsi_driver(cwu50_driver);

MODULE_AUTHOR("Andi Powers-Holmes <aholmes@omnom.net>");
MODULE_DESCRIPTION("DRM Driver for cwu50 MIPI DSI panel");
MODULE_LICENSE("GPL v2");
