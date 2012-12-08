/*
 * drivers/video/tegra/dc/clock.c
 *
 * Copyright (C) 2010 Google, Inc.
 *
 * Copyright (c) 2010-2012, NVIDIA CORPORATION, All rights reserved.
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

#include <linux/err.h>
#include <linux/types.h>
#include <linux/clk.h>

#include <mach/clk.h>
#include <mach/dc.h>

#include "dc_reg.h"
#include "dc_priv.h"

/*
 * Find the best divider and resulting clock given an input clock rate and
 * desired pixel clock, taking into account restrictions on the divider and
 * output device.
 */
static unsigned long tegra_dc_pclk_best_div(const struct tegra_dc *dc,
					    int pclk,
					    unsigned long input_rate)
{
	/* Multiply by 2 since the divider works in .5 increments */
	unsigned long div = DIV_ROUND_CLOSEST(input_rate * 2, pclk);

	if (!div)
		return 0;

	/* Don't attempt to exceed this output's maximum pixel clock */
	WARN_ON(!dc->out->max_pclk_khz);
	while (input_rate * 2 / div > dc->out->max_pclk_khz * 1000)
		div++;

	/* We have a u7.1 divider, where 0 means "divide by 1" */
	if (div < 2)
		div = 2;
	if (div > 257)
		div = 257;

	return div;
}

unsigned long tegra_dc_pclk_round_rate(const struct tegra_dc *dc,
						int pclk,
						unsigned long *div_out)
{
	long rate = clk_round_rate(dc->clk, pclk);
	unsigned long div;

	if (rate < 0)
		rate = clk_get_rate(dc->clk);

	div = tegra_dc_pclk_best_div(dc, pclk, rate);

	*div_out = div;

	return rate;
}

unsigned long tegra_dc_find_pll_d_rate(const struct tegra_dc *dc,
						unsigned long pclk,
						unsigned long *rate_out,
						unsigned long *div_out)
{
	/*
	 * These are the only freqs we can get from pll_d currently.
	 * TODO: algorithmically determine pll_d's m, n, p values so it can
	 * output more frequencies.
	*/
	const unsigned long pll_d_freqs[] = {
		216000000,
		252000000,
		594000000,
		1000000000,
	};
	long best_pclk_ratio = 0;
	unsigned long best_pclk = 0;
	unsigned long best_rate = 0;
	unsigned long best_div = 0;
	int i;

	if (dc->out->type != TEGRA_DC_OUT_HDMI)
		return pclk;

	for (i = 0; i < ARRAY_SIZE(pll_d_freqs); i++) {
		const unsigned long rate = pll_d_freqs[i];
		unsigned long rounded, div;
		long ratio;
		u64 tmp;

		/* Divide rate by 2 since pll_d_out0 is always 1/2 pll_d */
		div = tegra_dc_pclk_best_div(dc, pclk, rate / 2);
		if (!div)
			continue;

		rounded = rate / div;
		if (rounded > dc->out->max_pclk_khz * 1000)
			continue;

		tmp = (u64)rounded * 1000;
		do_div(tmp, pclk);
		ratio = lower_32_bits(tmp);

		/* Ignore anything outside of 95%-105% of the target */
		if (ratio < 950 || ratio > 1050)
			continue;

		if (abs(ratio - 1000) < abs(best_pclk_ratio - 1000)) {
			best_pclk = rounded;
			best_pclk_ratio = ratio;
			best_rate = rate;
			best_div = div;
		}
	}
	if (rate_out)
		*rate_out = best_rate;
	if (div_out)
		*div_out = best_div;

	return best_pclk;
}

void tegra_dc_setup_clk(struct tegra_dc *dc, struct clk *clk)
{
	/*
	* We should always have a valid rate here, since modes should
	* go through tegra_dc_set_mode() before attempting to program them.
	*/
	WARN_ON(!dc->pll_rate);

	if (dc->out->type == TEGRA_DC_OUT_HDMI) {
		struct clk *parent_clk = clk_get_sys(NULL,
			dc->out->parent_clk ? : "pll_d_out0");
		struct clk *base_clk = clk_get_parent(parent_clk);

		if (dc->pll_rate != clk_get_rate(base_clk))
			clk_set_rate(base_clk, dc->pll_rate);

		if (clk_get_parent(clk) != parent_clk)
			clk_set_parent(clk, parent_clk);
	} else {
		tegra_dvfs_set_rate(clk, dc->pll_rate);
	}
}
