/*
 * arch/arm/mach-tegra/board-paz00.h
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

#ifndef _MACH_TEGRA_BOARD_PAZ00_H
#define _MACH_TEGRA_BOARD_PAZ00_H

#include "gpio-names.h"

void paz00_pinmux_init(void);
int paz00_power_init(void);
int paz00_panel_init(void);
int paz00_sdhci_init(void);
void paz00_emc_init(void);

/* WIFI */
#define PAZ00_WIFI_LED		TEGRA_GPIO_PD0
#define PAZ00_WIFI_RST		TEGRA_GPIO_PD1
#define PAZ00_WIFI_PWRN		TEGRA_GPIO_PK5
#define PAZ00_EXT_WIFI_DSBL	TEGRA_GPIO_PV4

/* EC */
#define PAZ00_NVEC_REQ		TEGRA_GPIO_PV2

/* ULPI */
#define PAZ00_ULPI_RST		TEGRA_GPIO_PV0

/* MMC */
#define PAZ00_SD1_CD		TEGRA_GPIO_PV5
#define PAZ00_SD1_WP		TEGRA_GPIO_PH1
#define PAZ00_SD1_POWER		TEGRA_GPIO_PT3

/* Panel */
#define PAZ00_BACKLIGHT		TEGRA_GPIO_PU4
#define PAZ00_BACKLIGHT_VDD	TEGRA_GPIO_PW0
#define PAZ00_BACKLIGHT_PWM	TEGRA_GPIO_PU3
#define PAZ00_LVDS_SHUTDOWN	TEGRA_GPIO_PM6
#define PAZ00_EN_VDD_PNL	TEGRA_GPIO_PA4
#define PAZ00_HDMI_HPD		TEGRA_GPIO_PN7

/* Audio */
#define PAZ00_HP_DET		TEGRA_GPIO_PW2

/* PEX */
#define PAZ00_PEX_WAKE		TEGRA_GPIO_PV5
#define PAZ00_PEX_RST		TEGRA_GPIO_PV6

/* Bluetooth */
#define PAZ00_BT_RST		TEGRA_GPIO_PU0
#define PAZ00_BT_WAKE		TEGRA_GPIO_PU1

#endif
