/*
 * Copyright (C) 2010 NVIDIA, Inc.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA
 * 02111-1307, USA
 */
#include <linux/i2c.h>
#include <linux/regulator/machine.h>
#include <linux/mfd/tps6586x.h>
#include <linux/gpio.h>
#include <linux/power/gpio-charger.h>
#include <linux/platform_device.h>
#include <linux/err.h>

#include "board-seaboard.h"
#include "gpio-names.h"

static struct regulator_consumer_supply tps658621_sm0_supply[] = {
	REGULATOR_SUPPLY("vdd_core", NULL),
};
static struct regulator_consumer_supply tps658621_sm1_supply[] = {
	REGULATOR_SUPPLY("vdd_cpu", NULL),
};
static struct regulator_consumer_supply tps658621_sm2_supply[] = {
	REGULATOR_SUPPLY("vdd_sm2", NULL),
};
static struct regulator_consumer_supply tps658621_ldo0_supply[] = {
	REGULATOR_SUPPLY("p_cam_avdd", NULL),
};
static struct regulator_consumer_supply tps658621_ldo1_supply[] = {
	REGULATOR_SUPPLY("avdd_pll", NULL),
};
static struct regulator_consumer_supply tps658621_ldo2_supply[] = {
	REGULATOR_SUPPLY("vdd_rtc", NULL),
};
static struct regulator_consumer_supply tps658621_ldo3_supply[] = {
	REGULATOR_SUPPLY("avdd_usb", NULL),
	REGULATOR_SUPPLY("avdd_usb_pll", NULL),
};
static struct regulator_consumer_supply tps658621_ldo4_supply[] = {
	REGULATOR_SUPPLY("avdd_osc", NULL),
	REGULATOR_SUPPLY("vddio_sys", "panjit_touch"),
};
static struct regulator_consumer_supply tps658621_ldo5_supply[] = {
	REGULATOR_SUPPLY("vcore_mmc", "sdhci-tegra.1"),
	REGULATOR_SUPPLY("vcore_mmc", "sdhci-tegra.3"),
};
static struct regulator_consumer_supply tps658621_ldo6_supply[] = {
	REGULATOR_SUPPLY("vddio_vi", NULL),
};
static struct regulator_consumer_supply tps658621_ldo7_supply[] = {
	REGULATOR_SUPPLY("avdd_hdmi", NULL),
	REGULATOR_SUPPLY("vdd_fuse", NULL),
};
static struct regulator_consumer_supply tps658621_ldo8_supply[] = {
	REGULATOR_SUPPLY("avdd_hdmi_pll", NULL),
};
static struct regulator_consumer_supply tps658621_ldo9_supply[] = {
	REGULATOR_SUPPLY("avdd_2v85", NULL),
	REGULATOR_SUPPLY("vdd_ddr_rx", NULL),
	REGULATOR_SUPPLY("avdd_amp", NULL),
};

#define REGULATOR_INIT(_id, _minmv, _maxmv)				\
	{								\
		.constraints = {					\
			.min_uV = (_minmv)*1000,			\
			.max_uV = (_maxmv)*1000,			\
			.valid_modes_mask = (REGULATOR_MODE_NORMAL |	\
					     REGULATOR_MODE_STANDBY),	\
			.valid_ops_mask = (REGULATOR_CHANGE_MODE |	\
					   REGULATOR_CHANGE_STATUS |	\
					   REGULATOR_CHANGE_VOLTAGE),	\
		},							\
		.num_consumer_supplies = ARRAY_SIZE(tps658621_##_id##_supply),\
		.consumer_supplies = tps658621_##_id##_supply,		\
	}

static struct regulator_init_data sm0_data = REGULATOR_INIT(sm0, 725, 1500);
static struct regulator_init_data sm1_data = REGULATOR_INIT(sm1, 725, 1500);
static struct regulator_init_data sm2_data = REGULATOR_INIT(sm2, 3000, 4550);
static struct regulator_init_data ldo0_data = REGULATOR_INIT(ldo0, 1250, 3300);
static struct regulator_init_data ldo1_data = REGULATOR_INIT(ldo1, 725, 1500);
static struct regulator_init_data ldo2_data = REGULATOR_INIT(ldo2, 725, 1500);
static struct regulator_init_data ldo3_data = REGULATOR_INIT(ldo3, 1250, 3300);
static struct regulator_init_data ldo4_data = REGULATOR_INIT(ldo4, 1700, 2475);
static struct regulator_init_data ldo5_data = REGULATOR_INIT(ldo5, 1250, 3300);
static struct regulator_init_data ldo6_data = REGULATOR_INIT(ldo6, 1250, 3300);
static struct regulator_init_data ldo7_data = REGULATOR_INIT(ldo7, 1250, 3300);
static struct regulator_init_data ldo8_data = REGULATOR_INIT(ldo8, 1250, 3300);
static struct regulator_init_data ldo9_data = REGULATOR_INIT(ldo9, 1250, 3300);

#define TPS_REG(_id, _data)			\
	{					\
		.id = TPS6586X_ID_##_id,	\
		.name = "tps6586x-regulator",	\
		.platform_data = _data,		\
	}

static struct tps6586x_subdev_info tps_devs[] = {
	TPS_REG(SM_0, &sm0_data),
	TPS_REG(SM_1, &sm1_data),
	TPS_REG(SM_2, &sm2_data),
	TPS_REG(LDO_0, &ldo0_data),
	TPS_REG(LDO_1, &ldo1_data),
	TPS_REG(LDO_2, &ldo2_data),
	TPS_REG(LDO_3, &ldo3_data),
	TPS_REG(LDO_4, &ldo4_data),
	TPS_REG(LDO_5, &ldo5_data),
	TPS_REG(LDO_6, &ldo6_data),
	TPS_REG(LDO_7, &ldo7_data),
	TPS_REG(LDO_8, &ldo8_data),
	TPS_REG(LDO_9, &ldo9_data),
};

static struct tps6586x_platform_data tps_platform = {
	.num_subdevs = ARRAY_SIZE(tps_devs),
	.subdevs = tps_devs,
	.gpio_base = TEGRA_NR_GPIOS,
};

static struct i2c_board_info __initdata seaboard_regulators[] = {
	{
		I2C_BOARD_INFO("tps6586x", 0x34),
		.platform_data = &tps_platform,
	},
};

int __init seaboard_regulator_init(void)
{
	i2c_register_board_info(4, seaboard_regulators, 1);
	return 0;
}

/* ac power */
static char *tegra_batteries[] = {
	"battery",
};

static struct resource seaboard_ac_resources[] = {
	[0] = {
		.name = "ac",
		.start = TEGRA_GPIO_TO_IRQ(TEGRA_GPIO_AC_ONLINE),
		.end = TEGRA_GPIO_TO_IRQ(TEGRA_GPIO_AC_ONLINE),
		.flags = (IORESOURCE_IRQ | IORESOURCE_IRQ_HIGHEDGE |
				IORESOURCE_IRQ_LOWEDGE),
	},
};

static struct gpio_charger_platform_data seaboard_ac_platform_data = {
	.name			= "ac",
	.gpio			= TEGRA_GPIO_AC_ONLINE,
	.gpio_active_low	= 1,
	.supplied_to		= tegra_batteries,
	.type			= POWER_SUPPLY_TYPE_MAINS,
	.num_supplicants	= ARRAY_SIZE(tegra_batteries),
};

static struct platform_device seaboard_ac_power_device = {
	.name		= "gpio-charger",
	.id		= 0,
	.resource	= seaboard_ac_resources,
	.num_resources	= ARRAY_SIZE(seaboard_ac_resources),
	.dev = {
		.platform_data	= &seaboard_ac_platform_data,
	},
};

int __init seaboard_ac_power_init(void)
{
	int err;

	tegra_gpio_enable(TEGRA_GPIO_AC_ONLINE);

	err = gpio_request(TEGRA_GPIO_AC_ONLINE, "ac online");
	if (err < 0) {
		pr_err("could not acquire ac online GPIO\n");
	} else {
		gpio_direction_input(TEGRA_GPIO_AC_ONLINE);
		gpio_free(TEGRA_GPIO_AC_ONLINE);
	}

	err = platform_device_register(&seaboard_ac_power_device);
	return err;
}

static void reg_off(const char *reg)
{
	int rc;
	struct regulator *regulator;

	regulator = regulator_get(NULL, reg);

	if (IS_ERR(regulator)) {
		pr_err("%s: regulator_get returned %ld\n", __func__,
		       PTR_ERR(regulator));
		return;
	}

	regulator_enable(regulator);
	rc = regulator_disable(regulator);
	if (rc)
		pr_err("%s: regulator_disable returned %d\n", __func__, rc);
	regulator_put(regulator);
}

static void seaboard_power_off(void)
{
	reg_off("vdd_sm2");
	reg_off("vdd_core");
	reg_off("vdd_cpu");
	local_irq_disable();
	while (1) {
		dsb();
		__asm__ ("wfi");
	}
}

int __init seaboard_power_init(void)
{
	int err;

	err = seaboard_regulator_init();
	if (err < 0)
		pr_warning("Unable to initialize regulator\n");

	err = seaboard_ac_power_init();
	if (err < 0)
		pr_warning("Unable to initialize ac power\n");

	pm_power_off = seaboard_power_off;

	return 0;
}
