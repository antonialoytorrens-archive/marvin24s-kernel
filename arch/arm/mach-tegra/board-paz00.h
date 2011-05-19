/*
 * arch/arm/mach-tegra/board-paz00.h
 *
 * Copyright (C) 2010 Marc Dietrich <marvin24@gmx.de>
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

#ifndef _MACH_TEGRA_BOARD_PAZ00_H
#define _MACH_TEGRA_BOARD_PAZ00_H

#include "gpio-names.h"

/* SDMMC */
#define TEGRA_GPIO_SD1_CD		TEGRA_GPIO_PV5
#define TEGRA_GPIO_SD1_WP		TEGRA_GPIO_PH1
#define TEGRA_GPIO_SD1_POWER		TEGRA_GPIO_PT3

/* ULPI */
#define TEGRA_ULPI_RST			TEGRA_GPIO_PV0

/* Wifi */
#define TEGRA_WIFI_LED			TEGRA_GPIO_PD0
#define TEGRA_WIFI_PWRN			TEGRA_GPIO_PK5

/* Panel */
#define TEGRA_BACKLIGHT			TEGRA_GPIO_PU4
#define TEGRA_BACKLIGHT_VDD		TEGRA_GPIO_PW0
#define TEGRA_BACKLIGHT_PWM		TEGRA_GPIO_PU3
#define TEGRA_LVDS_SHUTDOWN		TEGRA_GPIO_PM6
#define TEGRA_EN_VDD_PNL		TEGRA_GPIO_PA4
#define TEGRA_HDMI_HPD			TEGRA_GPIO_PN7

/* EC */
#define TEGRA_NVEC_REQ			TEGRA_GPIO_PV2

void paz00_pinmux_init(void);
int paz00_power_init(void);
int paz00_panel_init(void);

#endif
