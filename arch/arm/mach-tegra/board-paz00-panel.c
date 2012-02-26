/*
 * arch/arm/mach-tegra/board-paz00-panel.c
 *
 * Copyright (c) 2010-2011, NVIDIA Corporation.
 *
 * This software is licensed under the terms of the GNU General Public
 * License version 2, as published by the Free Software Foundation, and
 * may be copied, distributed, and modified under those terms.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 */

#include <linux/delay.h>
#include <linux/resource.h>
#include <linux/platform_device.h>
#include <asm/mach-types.h>
#include <linux/nvhost.h>
#include <linux/gpio.h>
#include <linux/regulator/consumer.h>
#include <linux/pwm_backlight.h>

#include <mach/dc.h>
#include <mach/irqs.h>
#include <mach/iomap.h>
#include <mach/nvmap.h>
#include <mach/tegra_fb.h>
#include <mach/fb.h>

#include "devices.h"
#include "gpio-names.h"
#include "board.h"

#define paz00_bl_enb		TEGRA_GPIO_PU4
#define paz00_lvds_shutdown	TEGRA_GPIO_PM6
#define paz00_en_vdd_pnl	TEGRA_GPIO_PA4
#define paz00_bl_vdd		TEGRA_GPIO_PW0
#define paz00_bl_pwm		TEGRA_GPIO_PU3
#define paz00_hdmi_hpd		TEGRA_GPIO_PN7

/* panel power on sequence timing */
#define paz00_pnl_to_lvds_ms	0
#define paz00_lvds_to_bl_ms	200

static int paz00_backlight_init(struct device *dev)
{
	int ret;

	ret = gpio_request(paz00_bl_enb, "backlight_enb");
	if (ret < 0)
		return ret;

	ret = gpio_direction_output(paz00_bl_enb, 1);
	if (ret < 0)
		gpio_free(paz00_bl_enb);
	else
		tegra_gpio_enable(paz00_bl_enb);

	return ret;
}

static void paz00_backlight_exit(struct device *dev)
{
	gpio_set_value(paz00_bl_enb, 0);
	gpio_free(paz00_bl_enb);
	tegra_gpio_disable(paz00_bl_enb);
}

static int paz00_backlight_notify(struct device *unused, int brightness)
{
	gpio_set_value(paz00_en_vdd_pnl, !!brightness);
	gpio_set_value(paz00_lvds_shutdown, !!brightness);
	gpio_set_value(paz00_bl_enb, !!brightness);
	return brightness;
}

static int paz00_disp1_check_fb(struct device *dev, struct fb_info *info);

static struct platform_pwm_backlight_data paz00_backlight_data = {
	.pwm_id		= 0,
	.max_brightness	= 255,
	.dft_brightness	= 224,
	.pwm_period_ns	= 5000000,
	.init		= paz00_backlight_init,
	.exit		= paz00_backlight_exit,
	.notify		= paz00_backlight_notify,
	/* Only toggle backlight on fb blank notifications for disp1 */
	.check_fb	= paz00_disp1_check_fb,
};

static struct platform_device paz00_backlight_device = {
	.name	= "pwm-backlight",
	.id	= -1,
	.dev	= {
		.platform_data = &paz00_backlight_data,
	},
};

static int paz00_panel_enable(void)
{
	gpio_set_value(paz00_en_vdd_pnl, 1);
	mdelay(paz00_pnl_to_lvds_ms);
	gpio_set_value(paz00_lvds_shutdown, 1);
	mdelay(paz00_lvds_to_bl_ms);
	return 0;
}

static int paz00_panel_disable(void)
{
	gpio_set_value(paz00_lvds_shutdown, 0);
	gpio_set_value(paz00_en_vdd_pnl, 0);
	return 0;
}

static int paz00_set_hdmi_power(bool enable)
{
	static struct {
		struct regulator *regulator;
		const char *name;
	} regs[] = {
		{ .name = "avdd_hdmi" },
		{ .name = "avdd_hdmi_pll" },
	};
	int i;

	for (i = 0; i < ARRAY_SIZE(regs); i++) {
		if (!regs[i].regulator) {
			regs[i].regulator = regulator_get(NULL, regs[i].name);

			if (IS_ERR(regs[i].regulator)) {
				int ret = PTR_ERR(regs[i].regulator);
				regs[i].regulator = NULL;
				return ret;
			}
		}

		if (enable)
			regulator_enable(regs[i].regulator);
		else
			regulator_disable(regs[i].regulator);
	}

	return 0;
}

static int paz00_hdmi_enable(void)
{
	return paz00_set_hdmi_power(true);
}

static int paz00_hdmi_disable(void)
{
	return paz00_set_hdmi_power(false);
}

static struct resource paz00_disp1_resources[] = {
	{
		.name = "irq",
		.start  = INT_DISPLAY_GENERAL,
		.end    = INT_DISPLAY_GENERAL,
		.flags  = IORESOURCE_IRQ,
	},
	{
		.name = "regs",
		.start	= TEGRA_DISPLAY_BASE,
		.end	= TEGRA_DISPLAY_BASE + TEGRA_DISPLAY_SIZE-1,
		.flags	= IORESOURCE_MEM,
	},
	{
		.name = "fbmem",
		.flags	= IORESOURCE_MEM,
	},
};

static struct resource paz00_disp2_resources[] = {
	{
		.name	= "irq",
		.start	= INT_DISPLAY_B_GENERAL,
		.end	= INT_DISPLAY_B_GENERAL,
		.flags	= IORESOURCE_IRQ,
	},
	{
		.name	= "regs",
		.start	= TEGRA_DISPLAY2_BASE,
		.end	= TEGRA_DISPLAY2_BASE + TEGRA_DISPLAY2_SIZE - 1,
		.flags	= IORESOURCE_MEM,
	},
	{
		.name	= "fbmem",
		.flags	= IORESOURCE_MEM,
	},
	{
		.name	= "hdmi_regs",
		.start	= TEGRA_HDMI_BASE,
		.end	= TEGRA_HDMI_BASE + TEGRA_HDMI_SIZE - 1,
		.flags	= IORESOURCE_MEM,
	},
};

static struct tegra_dc_mode paz00_panel_modes[] = {
	{
		.pclk = 42430000,
		.h_ref_to_sync = 4,
		.v_ref_to_sync = 2,
		.h_sync_width = 136,
		.v_sync_width = 4,
		.h_back_porch = 138,
		.v_back_porch = 21,
		.h_active = 1024,
		.v_active = 600,
		.h_front_porch = 34,
		.v_front_porch = 4,
	},
};

static struct tegra_fb_data paz00_fb_data = {
	.win		= 0,
	.xres		= 1024,
	.yres		= 600,
	.bits_per_pixel	= 32,
	.flags		= TEGRA_FB_FLIP_ON_PROBE,
};

static struct tegra_fb_data paz00_hdmi_fb_data = {
	.win		= 0,
	.xres		= 1280,
	.yres		= 720,
	.bits_per_pixel	= 16,
};

static struct tegra_dc_out paz00_disp1_out = {
	.type		= TEGRA_DC_OUT_RGB,

	.align		= TEGRA_DC_ALIGN_MSB,
	.order		= TEGRA_DC_ORDER_RED_BLUE,
	.depth		= 18,
	.dither		= TEGRA_DC_ORDERED_DITHER,

	.modes		= paz00_panel_modes,
	.n_modes	= ARRAY_SIZE(paz00_panel_modes),

	.enable		= paz00_panel_enable,
	.disable	= paz00_panel_disable,
};

static struct tegra_dc_out paz00_disp2_out = {
	.type		= TEGRA_DC_OUT_HDMI,
	.flags		= TEGRA_DC_OUT_HOTPLUG_HIGH,

	.dcc_bus	= 1,
	.hotplug_gpio	= paz00_hdmi_hpd,

	.align		= TEGRA_DC_ALIGN_MSB,
	.order		= TEGRA_DC_ORDER_RED_BLUE,

	.enable		= paz00_hdmi_enable,
	.disable	= paz00_hdmi_disable,
};

static struct tegra_dc_platform_data paz00_disp1_pdata = {
	.flags		= TEGRA_DC_FLAG_ENABLED,
	.default_out	= &paz00_disp1_out,
	.fb		= &paz00_fb_data,
};

static struct tegra_dc_platform_data paz00_disp2_pdata = {
	.flags		= 0,
	.default_out	= &paz00_disp2_out,
	.fb		= &paz00_hdmi_fb_data,
};

static struct nvhost_device paz00_disp1_device = {
	.name		= "tegradc",
	.id		= 0,
	.resource	= paz00_disp1_resources,
	.num_resources	= ARRAY_SIZE(paz00_disp1_resources),
	.dev = {
		.platform_data = &paz00_disp1_pdata,
	},
};

static int paz00_disp1_check_fb(struct device *dev, struct fb_info *info)
{
	return info->device == &paz00_disp1_device.dev;
}

static struct nvhost_device paz00_disp2_device = {
	.name		= "tegradc",
	.id		= 1,
	.resource	= paz00_disp2_resources,
	.num_resources	= ARRAY_SIZE(paz00_disp2_resources),
	.dev = {
		.platform_data = &paz00_disp2_pdata,
	},
};

#if defined(CONFIG_TEGRA_NVMAP)
static struct nvmap_platform_carveout paz00_carveouts[] = {
	[0] = NVMAP_HEAP_CARVEOUT_IRAM_INIT,
	[1] = {
		.name		= "generic-0",
		.usage_mask	= NVMAP_HEAP_CARVEOUT_GENERIC,
		.buddy_size	= SZ_32K,
	},
};

static struct nvmap_platform_data paz00_nvmap_data = {
	.carveouts	= paz00_carveouts,
	.nr_carveouts	= ARRAY_SIZE(paz00_carveouts),
};

static struct platform_device paz00_nvmap_device = {
	.name	= "tegra-nvmap",
	.id	= -1,
	.dev	= {
		.platform_data = &paz00_nvmap_data,
	},
};
#endif

static struct platform_device *paz00_gfx_devices[] __initdata = {
#if defined(CONFIG_TEGRA_NVMAP)
	&paz00_nvmap_device,
#endif
	&tegra_grhost_device,
	&tegra_pwfm0_device,
	&paz00_backlight_device,
};

int __init paz00_panel_init(void) {
	int err;
	struct resource *res;

	gpio_request(paz00_en_vdd_pnl, "en_vdd_pnl");
	gpio_direction_output(paz00_en_vdd_pnl, 1);
	tegra_gpio_enable(paz00_en_vdd_pnl);

	gpio_request(paz00_bl_vdd, "bl_vdd");
	gpio_direction_output(paz00_bl_vdd, 1);
	tegra_gpio_enable(paz00_bl_vdd);

	gpio_request(paz00_lvds_shutdown, "lvds_shdn");
	gpio_direction_output(paz00_lvds_shutdown, 1);
	tegra_gpio_enable(paz00_lvds_shutdown);

	gpio_request(paz00_hdmi_hpd, "hdmi_hpd");
	gpio_direction_input(paz00_hdmi_hpd);
	tegra_gpio_enable(paz00_hdmi_hpd);

#if defined(CONFIG_TEGRA_NVMAP)
	paz00_carveouts[1].base = tegra_carveout_start;
	paz00_carveouts[1].size = tegra_carveout_size;
#endif

	err = platform_add_devices(paz00_gfx_devices,
				   ARRAY_SIZE(paz00_gfx_devices));
	if (err)
		return err;

	res = nvhost_get_resource_byname(&paz00_disp1_device,
		IORESOURCE_MEM, "fbmem");
	if (res) {
		res->start = tegra_fb_start;
		res->end = tegra_fb_start + tegra_fb_size - 1;
	}

	res = nvhost_get_resource_byname(&paz00_disp2_device,
		IORESOURCE_MEM, "fbmem");
	if (res) {
		res->start = tegra_fb2_start;
		res->end = tegra_fb2_start + tegra_fb2_size - 1;
	}

	/* Copy the bootloader fb to the fb. */
	if (tegra_bootloader_fb_start)
		tegra_move_framebuffer(tegra_fb_start,
			tegra_bootloader_fb_start,
			min(tegra_fb_size, tegra_bootloader_fb_size));
	err = nvhost_device_register(&paz00_disp1_device);
	if (err)
		return err;

	err = nvhost_device_register(&paz00_disp2_device);
	if (err)
		return err;

	return 0;
}

