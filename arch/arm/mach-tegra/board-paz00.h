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

void paz00_pinmux_init(void);
int paz00_power_init(void);
int paz00_panel_init(void);
int paz00_sdhci_init(void);

#endif
