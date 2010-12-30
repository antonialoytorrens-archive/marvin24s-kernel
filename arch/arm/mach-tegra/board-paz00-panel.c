/*
 * arch/arm/mach-tegra/board-paz00-panel.c
 *
 * Copyright (c) 2010, NVIDIA Corporation.
 *               2010, Marc Dietrich <marvin24@gmx.de>
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
#include <linux/err.h>
#include <mach/nvhost.h>
#include <mach/nvmap.h>
#include <mach/irqs.h>
#include <mach/iomap.h>
#include <mach/dc.h>
#include <mach/fb.h>

#include "devices.h"
#include "gpio-names.h"

#define paz00_bl_enb		TEGRA_GPIO_PU4  // PB5
#define paz00_lvds_shutdown	TEGRA_GPIO_PM6  // PB2
#define paz00_en_vdd_pnl	TEGRA_GPIO_PA4  // PC6
#define paz00_bl_vdd		TEGRA_GPIO_PW0  // PW0
#define paz00_bl_pwm		TEGRA_GPIO_PU3  // PB4
#define paz00_hdmi_hpd		TEGRA_GPIO_PN7  // enable Hotplug

static int paz00_backlight_init(struct device *dev)
{
	static struct regulator *reg = NULL;
	int ret;

	pr_warning(">>> backlight_enable\n");

	reg = regulator_get(NULL, "vddio_lcd");
	if (IS_ERR(reg)) {
		pr_warning("Couldn't get regulator vddio_lcd\n");
		return -1;
	}

	if (regulator_is_enabled(reg)) {
		pr_warning("regulator avdd_lvds already enabled\n");
	} else {
		ret = regulator_set_voltage(reg, 1800000, 1800000);
		if (ret) {
			pr_warning("Couldn't set regulator voltage vddio_lcd\n");
			return ret;
		}

		ret = regulator_enable(reg);
		if (ret)
			pr_warning("Couldn't enable regualtor vddio_lcd\n");
	}

	ret = gpio_request(paz00_bl_enb, "blacklight_enable");
	if (ret) {
		pr_warning("could not request paz00_bl_enb gpio\n");
		return ret;
	}

	ret = gpio_direction_output(paz00_bl_enb, 1);
	if (ret) {
		pr_warning("could not set output direction of lvds_shutdown\n");
		return ret;
	}

	gpio_set_value(paz00_bl_enb, 1);

	return 0;

};

static void paz00_backlight_exit(struct device *dev)
{
	gpio_set_value(paz00_bl_enb, 0);
	gpio_free(paz00_bl_enb);
}

static int paz00_backlight_notify(struct device *unused, int brightness)
{
	gpio_set_value(paz00_en_vdd_pnl, !!brightness);
	gpio_set_value(paz00_lvds_shutdown, !!brightness);
	gpio_set_value(paz00_bl_enb, !!brightness);
	return brightness;
}

static struct platform_pwm_backlight_data paz00_backlight_data = {
	.pwm_id		= 0,
	.max_brightness	= 255,
	.dft_brightness	= 224,
	.pwm_period_ns	= 5000000,
	.init		= paz00_backlight_init,
	.exit		= paz00_backlight_exit,
	.notify		= paz00_backlight_notify,
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
	static struct regulator *reg = NULL;
	int ret = 0;

	pr_warning(">>> panel_enable\n");

	reg = regulator_get(NULL, "avdd_lvds");
	if (IS_ERR(reg)) {
		pr_warning("Couldn't get regulator avdd_lvds\n");
		return -1;
	}

	if (regulator_is_enabled(reg)) {
		pr_warning("regulator avdd_lvds already enabled\n");
	} else {

		ret = regulator_set_voltage(reg, 3300000, 3300000);
		if (ret) {
			pr_warning("Couldn't set regulator voltage add_lvds\n");
			return -1;
		}

		ret = regulator_enable(reg);
		if (ret)
			pr_warning("Couldn't enable regualtor avdd_lvds\n");
	}

	ret = gpio_request(paz00_lvds_shutdown, "lvds_shdn");
	if (ret) {
		pr_warning("could not request lvds_shutdown gpio\n");
		return ret;
	}

	ret = gpio_direction_output(paz00_lvds_shutdown, 1);
	if (ret) {
		pr_warning("could not set output direction of lvds_shutdown\n");
		return ret;
	}

	gpio_set_value(paz00_lvds_shutdown, 1);

/* Backlight */

	pr_warning(">>> backlight_enable\n");

	reg = regulator_get(NULL, "vddio_lcd");
	if (IS_ERR(reg)) {
		pr_warning("Couldn't get regulator vddio_lcd\n");
		return -1;
	}

	if (regulator_is_enabled(reg)) {
		pr_warning("regulator vddio_lcd already enabled\n");
	} else {
		ret = regulator_set_voltage(reg, 1800000, 1800000);
		if (ret) {
			pr_warning("Couldn't set regulator voltage vddio_lcd\n");
			return -1;
		}

		ret = regulator_enable(reg);
		if (ret)
			pr_warning("Couldn't enable regualtor vddio_lcd\n");
	}

	ret = gpio_request(paz00_bl_enb, "blacklight_enable");
	if (ret) {
		pr_warning("could not request paz00_bl_enb gpio\n");
		return ret;
	}

	ret = gpio_direction_output(paz00_bl_enb, 1);
	if (ret) {
		pr_warning("could not set output direction of lvds_shutdown\n");
		return ret;
	}

	gpio_set_value(paz00_bl_enb, 1);

	return 0;
}

static int paz00_panel_disable(void)
{
	gpio_set_value(paz00_lvds_shutdown, 0);
	return 0;
}

static int paz00_hdmi_enable(void)
{
	pr_warning(">>> hdmi enable\n");
	return 0;
}

static int paz00_hdmi_disable(void)
{
	pr_warning(">>> hdmi disable\n");
	return 0;
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
		.end	= 0x1c012000 + 0x258000 - 1,   /* 2.4 MB @ 448 MB */
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
		.end	= TEGRA_DISPLAY2_BASE + TEGRA_DISPLAY2_SIZE-1,
		.flags	= IORESOURCE_MEM,
	},
	{
		.name	= "fbmem",
		.start	= 0x1c26A000,
		.end	= 0x1c26A000 + 0x500000 - 1,  /* 5 MB */
		.flags	= IORESOURCE_MEM,
	},
	{
		.name	= "hdmi_regs",
		.start	= TEGRA_HDMI_BASE,
		.end	= TEGRA_HDMI_BASE + TEGRA_HDMI_SIZE - 1,
		.flags	= IORESOURCE_MEM,
	}
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
	.xres		= 1024,
	.yres		= 600,
	.bits_per_pixel	= 16,
};

static struct tegra_dc_out paz00_disp1_out = {
	.type		= TEGRA_DC_OUT_RGB,

	.align		= TEGRA_DC_ALIGN_MSB,
	.order		= TEGRA_DC_ORDER_RED_BLUE,

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
	.flags		= TEGRA_DC_FLAG_ENABLED,
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
		.base		= 0x1C000000,   /* carveout starts at 448 */
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

int __init paz00_panel_init(void)
{
	int err;

	gpio_request(paz00_en_vdd_pnl, "en_vdd_pnl");
	gpio_direction_output(paz00_en_vdd_pnl, 1);
	tegra_gpio_enable(paz00_en_vdd_pnl);
	gpio_free(paz00_en_vdd_pnl);

	gpio_request(paz00_bl_vdd, "bl_vdd");
	gpio_direction_output(paz00_bl_vdd, 1);
	tegra_gpio_enable(paz00_bl_vdd);
	gpio_free(paz00_bl_vdd);

	gpio_request(paz00_hdmi_hpd, "hdmi_hpd");
	gpio_direction_input(paz00_hdmi_hpd);
	tegra_gpio_enable(paz00_hdmi_hpd);
	gpio_free(paz00_hdmi_hpd);

	err = platform_add_devices(paz00_gfx_devices,
				   ARRAY_SIZE(paz00_gfx_devices));

	if (!err)
		err = nvhost_device_register(&paz00_disp1_device);
	else
		pr_warning("registered disp1 device\n");

	if (!err)
		err = nvhost_device_register(&paz00_disp2_device);
	else
		pr_warning("registered disp2 device\n");

	return err;
}
