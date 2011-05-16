/*
 * arch/arm/mach-tegra/board-paz00-sdhci.c
 *
 * Copyright (C) 2010 Google, Inc.
 *               2010 Marc Dietrich <marvin24@gmx.de>
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

#include <linux/resource.h>
#include <linux/platform_device.h>
#include <linux/delay.h>
#include <linux/gpio.h>

#include <asm/mach-types.h>
#include <mach/irqs.h>
#include <mach/iomap.h>
#include <mach/sdhci.h>
#include <mach/pinmux.h>

#include "board-paz00.h"

static struct resource sdhci_resource1[] = {
	[0] = {
		.start  = INT_SDMMC1,
		.end    = INT_SDMMC1,
		.flags  = IORESOURCE_IRQ,
	},
	[1] = {
		.start  = TEGRA_SDMMC1_BASE,
		.end    = TEGRA_SDMMC1_BASE + TEGRA_SDMMC1_SIZE-1,
		.flags  = IORESOURCE_MEM,
	},
};

static struct resource sdhci_resource4[] = {
	[0] = {
		.start  = INT_SDMMC4,
		.end    = INT_SDMMC4,
		.flags  = IORESOURCE_IRQ,
	},
	[1] = {
		.start	= TEGRA_SDMMC4_BASE,
		.end	= TEGRA_SDMMC4_BASE + TEGRA_SDMMC4_SIZE-1,
		.flags	= IORESOURCE_MEM,
	},
};

static struct tegra_sdhci_platform_data tegra_sdhci_platform_data1 = {
	.cd_gpio = PAZ00_SD1_CD,
	.wp_gpio = PAZ00_SD1_WP,
	.power_gpio = PAZ00_SD1_POWER,
};

static struct tegra_sdhci_platform_data tegra_sdhci_platform_data4 = {
	.cd_gpio = -1,  /* no card detect on emmc */
	.wp_gpio = -1,
	.power_gpio = -1,
	.is_8bit = 1,
};

static struct platform_device tegra_sdhci_device1 = {
	.name		= "sdhci-tegra",
	.id		= 0,
	.resource	= sdhci_resource1,
	.num_resources	= ARRAY_SIZE(sdhci_resource1),
	.dev = {
		.platform_data = &tegra_sdhci_platform_data1,
	},
};

static struct platform_device tegra_sdhci_device4 = {
	.name		= "sdhci-tegra",
	.id		= 3,
	.resource	= sdhci_resource4,
	.num_resources	= ARRAY_SIZE(sdhci_resource4),
	.dev = {
		.platform_data = &tegra_sdhci_platform_data4,
	},
};

int __init paz00_sdhci_init(void)
{
	platform_device_register(&tegra_sdhci_device4);
	platform_device_register(&tegra_sdhci_device1);

	return 0;
}
