/*
 * arch/arm/mach-tegra/board-paz00-panel.c
 *
 * Copyright (c) 2010, NVIDIA Corporation.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General Public License for
 * more details.
 *
 * You should have received a copy of the GNU General Public License along
 * with this program; if not, write to the Free Software Foundation, Inc.,
 * 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301, USA.
 */

#include <linux/delay.h>
#include <linux/gpio.h>
#include <linux/regulator/consumer.h>
#include <linux/resource.h>
#include <asm/mach-types.h>
#include <linux/platform_device.h>
#include <linux/pwm_backlight.h>
#include <mach/nvhost.h>
#include <mach/nvmap.h>
#include <mach/irqs.h>
#include <mach/iomap.h>
#include <mach/dc.h>
#include <mach/fb.h>

#include "devices.h"
#include "gpio-names.h"
#include "fb.h"

#include "board-paz00.h"

static int paz00_backlight_init(struct device *dev)
{
	return gpio_request_one(TEGRA_BACKLIGHT, GPIOF_OUT_INIT_HIGH, "backlight_enb");
}

static void paz00_backlight_exit(struct device *dev) {
	gpio_set_value(TEGRA_BACKLIGHT, 0);
	gpio_free(TEGRA_BACKLIGHT);
}

static int paz00_backlight_notify(struct device *unused, int brightness)
{
	gpio_set_value(TEGRA_EN_VDD_PNL, !!brightness);
	gpio_set_value(TEGRA_LVDS_SHUTDOWN, !!brightness);
	gpio_set_value(TEGRA_BACKLIGHT, !!brightness);
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
	gpio_set_value(TEGRA_LVDS_SHUTDOWN, 1);
	return 0;
}

static int paz00_panel_disable(void)
{
	gpio_set_value(TEGRA_LVDS_SHUTDOWN, 0);
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
		.name	= "irq",
		.start	= INT_DISPLAY_GENERAL,
		.end	= INT_DISPLAY_GENERAL,
		.flags	= IORESOURCE_IRQ,
	},
	{
		.name	= "regs",
		.start	= TEGRA_DISPLAY_BASE,
		.end	= TEGRA_DISPLAY_BASE + TEGRA_DISPLAY_SIZE-1,
		.flags	= IORESOURCE_MEM,
	},
	{
		.name	= "fbmem",
		.start	= 0x1c012000,
		.end	= 0x1c012000 + 0x258000 - 1,
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
	.bits_per_pixel	= 16,
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
	.hotplug_gpio	= TEGRA_HDMI_HPD,

	.align		= TEGRA_DC_ALIGN_MSB,
	.order		= TEGRA_DC_ORDER_RED_BLUE,

	.enable		= paz00_hdmi_enable,
	.disable	= paz00_hdmi_disable,

	/* DVFS tables only updated up to 148.5MHz for HDMI currently */
	.max_pclk_khz	= 148500,
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

static struct nvmap_platform_carveout paz00_carveouts[] = {
	[0] = {
		.name		= "iram",
		.usage_mask	= NVMAP_HEAP_CARVEOUT_IRAM,
		.base		= TEGRA_IRAM_BASE,
		.size		= TEGRA_IRAM_SIZE,
		.buddy_size	= 0, /* no buddy allocation for IRAM */
	},
	[1] = {
		.name		= "generic-0",
		.usage_mask	= NVMAP_HEAP_CARVEOUT_GENERIC,
		.base		= 0x1C000000, /* Carveout starts at 448 MB */
		.size		= SZ_64M - 0xC00000,
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

static struct platform_device *paz00_gfx_devices[] __initdata = {
	&paz00_nvmap_device,
	&tegra_grhost_device,
	&tegra_pwfm0_device,
	&paz00_backlight_device,
};

static int __init paz00_panel_init(void)
{
	int err;

	if (!of_machine_is_compatible("compal,paz00") &&
	    !machine_is_paz00())
		return -ENODEV;

	gpio_request_one(TEGRA_EN_VDD_PNL, GPIOF_OUT_INIT_HIGH, "en_vdd_pnl");
	gpio_request_one(TEGRA_BACKLIGHT_VDD, GPIOF_OUT_INIT_HIGH, "bl_vdd");
	gpio_request_one(TEGRA_LVDS_SHUTDOWN, GPIOF_OUT_INIT_HIGH, "lvds_shdn");
	gpio_request_one(TEGRA_HDMI_HPD, GPIOF_IN, "hdmi_hpd");

	err = platform_add_devices(paz00_gfx_devices,
				   ARRAY_SIZE(paz00_gfx_devices));

	if (!err)
		err = nvhost_device_register(&paz00_disp1_device);

	if (!err)
		err = nvhost_device_register(&paz00_disp2_device);

	return err;
}
device_initcall(paz00_panel_init);

int __init paz00_fb_init(void)
{
	unsigned long fb_size;

	if (!of_machine_is_compatible("compal,paz00") &&
	    !machine_is_paz00())
		return -ENODEV;

	/*
	 * reserve 64MB for carveout, 1024*768*4*2 for fb_size,
	 * and 0 for fb2_size.
	 */
	fb_size = round_up((1024 * 768 * 4 * 2), PAGE_SIZE);
	tegra_reserve(64 * 1024 * 1024, fb_size, 0);

	return 0;
};
