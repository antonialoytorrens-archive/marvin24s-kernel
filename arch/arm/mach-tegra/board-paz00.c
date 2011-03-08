/*
 * arch/arm/mach-tegra/board-paz00.c
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

#include <linux/kernel.h>
#include <linux/init.h>
#include <linux/platform_device.h>
#include <linux/serial_8250.h>
#include <linux/clk.h>
#include <linux/mtd/mtd.h>
#include <linux/mtd/partitions.h>
#include <linux/dma-mapping.h>
#include <linux/pda_power.h>
#include <linux/i2c.h>
#include <linux/i2c-tegra.h>
#include <linux/io.h>
#include <linux/delay.h>
#include <linux/tegra_usb.h>
#include <linux/fsl_devices.h>
#include <linux/mfd/nvtegra-ec.h>
#include <linux/gpio.h>

#include <asm/mach-types.h>
#include <asm/mach/arch.h>
#include <asm/mach/time.h>
#include <asm/setup.h>

#include <mach/audio.h>
#include <mach/i2s.h>
#include <mach/iomap.h>
#include <mach/irqs.h>
#include <mach/clk.h>
#include <mach/usb_phy.h>
#include <mach/suspend.h>

#include "clock.h"
#include "board.h"
#include "board-paz00.h"
#include "devices.h"
#include "gpio-names.h"

static struct plat_serial8250_port debug_uart_platform_data[] = {
	{
		.membase	= IO_ADDRESS(TEGRA_UARTD_BASE),
		.mapbase	= TEGRA_UARTD_BASE,
		.irq		= INT_UARTD,
		.flags		= UPF_BOOT_AUTOCONF,
		.iotype		= UPIO_MEM,
		.regshift	= 2,
		.uartclk	= 216000000,
	}, {
		.flags		= 0
	}
};

static struct platform_device debug_uart = {
	.name = "serial8250",
	.id = PLAT8250_DEV_PLATFORM,
	.dev = {
		.platform_data = debug_uart_platform_data,
	},
};

static struct tegra_utmip_config utmi_phy_config[] = {
	[0] = {
		.hssync_start_delay = 9,
		.idle_wait_delay = 17,
		.elastic_limit = 16,
		.term_range_adj = 6,
		.xcvr_setup = 15,
		.xcvr_lsfslew = 1,
		.xcvr_lsrslew = 1,
	},
	[1] = {
		.hssync_start_delay = 9,
		.idle_wait_delay = 17,
		.elastic_limit = 16,
		.term_range_adj = 6,
		.xcvr_setup = 9,
		.xcvr_lsfslew = 2,
		.xcvr_lsrslew = 2,
	},
};

static struct tegra_ulpi_config ulpi_phy_config = {
	.reset_gpio = TEGRA_GPIO_PV0,
	.clk = "clk_dev2",
};

static struct tegra_ehci_platform_data tegra_ehci_pdata[] = {
	[0] = {
		.phy_config = &utmi_phy_config[0],
		.operating_mode = TEGRA_USB_OTG,
		.power_down_on_bus_suspend = 0,
	},
	[1] = {
		.phy_config = &ulpi_phy_config,
		.operating_mode = TEGRA_USB_HOST,
		.power_down_on_bus_suspend = 1,
	},
	[2] = {
		.phy_config = &utmi_phy_config[1],
		.operating_mode = TEGRA_USB_HOST,
		.power_down_on_bus_suspend = 1,
	},
};

static struct tegra_i2c_platform_data paz00_i2c1_platform_data = {
	.adapter_nr	= 0,
	.bus_count	= 1,
	.bus_clk_rate	= { 400000, 0 },
};

static const struct tegra_pingroup_config i2c2_ddc = {
	.pingroup	= TEGRA_PINGROUP_DDC,
	.func		= TEGRA_MUX_I2C2,
};

static const struct tegra_pingroup_config i2c2_gen2 = {
	.pingroup	= TEGRA_PINGROUP_PTA,
	.func		= TEGRA_MUX_I2C2,
};

static struct tegra_i2c_platform_data paz00_i2c2_platform_data = {
	.adapter_nr	= 1,
	.bus_count	= 2,
	.bus_clk_rate	= { 400000, 100000 },
	.bus_mux	= { &i2c2_ddc, &i2c2_gen2 },
	.bus_mux_len	= { 1, 1 },
};

#ifdef CONFIG_I2C_TEGRA_SLAVE
static struct tegra_i2c_platform_data paz00_i2c3_platform_data = {
	.adapter_nr	= 3,
	.bus_count	= 1,
	.bus_clk_rate	= { 400000, 0 },
	.slave_addr	= 0x45,
};
#endif

static struct tegra_i2c_platform_data paz00_dvc_platform_data = {
	.adapter_nr	= 4,
	.bus_count	= 1,
	.bus_clk_rate	= { 400000, 0 },
	.is_dvc		= true,
};

/* FIXME: Audio codec on PAZ00 is alc5632
 * no codec exists yet
 * propably requires userspace */

static struct i2c_board_info __initdata paz00_i2c_bus1_board_info[] = {
	{
		I2C_BOARD_INFO("alc5632", 0x1e),
	},
};

#ifdef CONFIG_I2C_TEGRA_SLAVE
static struct nvec_platform_data __initdata nvec_data[] = {
	{
		.req_gpio = TEGRA_GPIO_PV2,
	},
};

static struct i2c_board_info __initdata paz00_i2c_bus3_board_info[] = {
	{
		I2C_BOARD_INFO("nvec", 0x45),
		.platform_data = &nvec_data,
	},
};
#endif

static struct i2c_board_info __initdata paz00_i2c_bus4_board_info[] = {
	{
		I2C_BOARD_INFO("atd7461", 0x4c), /* aka lm90 */
	},
};

static struct tegra_audio_platform_data tegra_audio_pdata = {
	.i2s_master	= false,
	.dsp_master	= false,
	.dma_on		= true,  /* use dma by default */
	.i2s_clk_rate	= 240000000,
	.dap_clk	= "clk_dev1",
	.audio_sync_clk = "audio_2x",
	.mode		= I2S_BIT_FORMAT_I2S,
	.fifo_fmt	= I2S_FIFO_16_LSB,
	.bit_size	= I2S_BIT_SIZE_16,
};

static void paz00_i2c_init(void)
{
	tegra_i2c_device1.dev.platform_data = &paz00_i2c1_platform_data;
	tegra_i2c_device2.dev.platform_data = &paz00_i2c2_platform_data;
#ifdef CONFIG_I2C_TEGRA_SLAVE
	tegra_i2c_device3.dev.platform_data = &paz00_i2c3_platform_data;
#endif
	tegra_i2c_device4.dev.platform_data = &paz00_dvc_platform_data;

	platform_device_register(&tegra_i2c_device1);
	platform_device_register(&tegra_i2c_device2);
#ifdef CONFIG_I2C_TEGRA_SLAVE
	platform_device_register(&tegra_i2c_device3);
#endif	
	platform_device_register(&tegra_i2c_device4);

/* no audio yet */
	i2c_register_board_info(0, paz00_i2c_bus1_board_info,
				ARRAY_SIZE(paz00_i2c_bus1_board_info));

/* NVEC has its own init for now */
/*	i2c_register_board_info(3, paz00_i2c_bus3_board_info,
				ARRAY_SIZE(paz00_i2c_bus3_board_info)); */

	i2c_register_board_info(4, paz00_i2c_bus4_board_info,
				ARRAY_SIZE(paz00_i2c_bus4_board_info));

}

/* set wifi power gpio */
static void __init paz00_wifi_init(void)
{
	int ret;

	tegra_gpio_enable(PAZ00_WIFI_PWRN);

	ret = gpio_request(PAZ00_WIFI_PWRN, "wlan_pwrn");
	if (ret) {
		pr_warning("WIFI: could not request WIFI PWR gpio!\n");
		return;
	}

	ret = gpio_direction_output(PAZ00_WIFI_PWRN, 0);
	if (ret) {
		pr_warning("WIFI: could not set WIFI PWR gpio direction!\n");
		return;
	}

	gpio_set_value(PAZ00_WIFI_PWRN, 0);
}

static struct platform_device *paz00_devices[] __initdata = {
	&debug_uart,
	&pmu_device,
	&tegra_udc_device,
	&tegra_spi_device1,
	&tegra_spi_device2,
	&tegra_spi_device3,
	&tegra_spi_device4,
	&tegra_gart_device,
	&tegra_i2s_device1,
};

static void __init tegra_paz00_fixup(struct machine_desc *desc,
	struct tag *tags, char **cmdline, struct meminfo *mi)
{
	mi->nr_banks = 1;
	mi->bank[0].start = PHYS_OFFSET;
	mi->bank[0].size = 448 * SZ_1M;
/*	mi->bank[1].start = SZ_512M;
	mi->bank[1].size = SZ_512M; */
}

static __initdata struct tegra_clk_init_table paz00_clk_init_table[] = {
	/* name		parent		rate		enabled */
/*	{ "clk_dev1",	NULL,		26000000,	true},
	{ "clk_dev2",	NULL,		26000000,	true},
	{ "3d",		"pll_m",	266400000,	true},
	{ "2d",		"pll_m",	266400000,	true},
	{ "vi",		"pll_m",	50000000,	true},
	{ "vi_sensor",	"pll_m",	111000000,	false},
	{ "epp",	"pll_m",	266400000,	true},
	{ "mpe",	"pll_m",	111000000,	false},
	{ "vde",	"pll_c",	240000000,	false},
	{ "i2s1",	"pll_a_out0",	11289600,	true},
	{ "pll_p_out2",	"pll_p",	48000000,	true},
	{ "pll_p_out3",	"pll_p",	72000000,	true},
	{ "csi",	"pll_p_out3",	72000000,	false},
	{ "spdif_in",	"pll_p",	36000000,	false}, 
	{ "uartb",	"clk_m",	12000000,	false},
	{ "uartc",	"clk_m",	12000000,	false},
	{ "uarte",	"clk_m",	12000000,	false}, 
	{ "host1x",	"pll_p",	144000000,	true},
	{ "disp1",	"pll_p",	216000000,	true},
	{ "pll_d",	"clk_m",	1000000,	false},
	{ "pll_d_out0",	"pll_d",	500000,		false},
	{ "dsi",	"pll_d",	1000000,	false},
	{ "i2s2",	"clk_m",	11289600,	false},
	{ "spdif_out",	"clk_m",	12000000,	false},
	{ "spi",	"clk_m",	12000000,	false},
	{ "xio",	"clk_m",	12000000,	false},
	{ "twc",	"clk_m",	12000000,	false},
	{ "sbc1",	"clk_m",	12000000,	false},
	{ "sbc2",	"clk_m",	12000000,	false},
	{ "sbc3",	"clk_m",	12000000,	false},
	{ "sbc4",	"clk_m",	12000000,	false},
	{ "ide",	"clk_m",	12000000,	false},
	{ "ndflash",	"clk_m",	108000000,	true},
	{ "vfir",	"clk_m",	12000000,	false},
	{ "la",		"clk_m",	12000000,	false},
	{ "owr",	"clk_m",	12000000,	false},
	{ "nor",	"clk_m",	12000000,	false},
	{ "mipi",	"clk_m",	12000000,	false},
	{ "dvc",	"clk_m",	3000000,	false},
	{ "cve",	"clk_m",	12000000,	false},
	{ "tvo",	"clk_m",	12000000,	false},
	{ "hdmi",	"clk_m",	12000000,	false},
	{ "tvdac",	"clk_m",	12000000,	false},
	{ "disp2",	"clk_m",	12000000,	false},
	{ "usbd",	"clk_m",	12000000,	false},
	{ "usb2",	"clk_m",	12000000,	false},
	{ "usb3",	"clk_m",	12000000,	true},
	{ "isp",	"clk_m",	12000000,	false},
	{ "csus",	"clk_m",	12000000,	false},
	{ "clk_32k",	NULL,		32768,		true},
	{ "pll_s",	"clk_32k",	32768,		false},
	{ "kbc",	"clk_32k",	32768,		true}, */

	{ "apbdma",	"hclk",		54000000,	true},
	{ "audio",	"pll_a_out0",	11289600,	true},
	{ "audio_2x",	"audio",	22579200,	false},
	{ "uarta",	"clk_m",	12000000,	true},
	{ "uartd",	"pll_p",	216000000,	true},
	{ "pwm",	"clk_32k",	32768,		true},
	{ "clk_d",	"clk_m",	24000000,	true},
	{ "pll_a",	"pll_p_out1",	56448000,	true},
	{ "pll_a_out0",	"pll_a",	11289600,	true},
	{ "pll_c",	"clk_m",	600000000,	true},
	{ "pll_c_out1",	"pll_c",	240000000,	true},
	{ "pll_p_out4", "pll_p",        24000000,       true},
	{ "i2c1_i2c",	"pll_p_out3",	72000000,	true},
	{ "i2c2_i2c",	"pll_p_out3",	72000000,	true},
	{ "i2c3_i2c",	"pll_p_out3",	72000000,	true},
	{ "dvc_i2c",	"pll_p_out3",	72000000,	true},
	{ "i2c1",	"clk_m",	3000000,	false},
	{ "i2c2",	"clk_m",	3000000,	false},
	{ "i2c3",	"clk_m",	3000000,	false},
	{ NULL,		NULL,		0,		0},
};

static struct tegra_suspend_platform_data paz00_suspend = {
	.cpu_timer = 5000,
	.cpu_off_timer = 5000,
	.core_timer = 0x7e7e,
	.core_off_timer = 0x7f,
	.separate_req = true,
	.corereq_high = false,
	.sysclkreq_high = true,
	.suspend_mode = TEGRA_SUSPEND_LP0,
};

static void __init tegra_paz00_init(void)
{

	tegra_common_init();

	tegra_init_suspend(&paz00_suspend);

	tegra_clk_init_from_table(paz00_clk_init_table);

	paz00_pinmux_init();
	tegra_ehci2_device.dev.platform_data = &tegra_ehci_pdata[1];
	tegra_ehci3_device.dev.platform_data = &tegra_ehci_pdata[2];

	platform_device_register(&tegra_ehci2_device);
	platform_device_register(&tegra_ehci3_device);

	tegra_i2s_device1.dev.platform_data = &tegra_audio_pdata;

	platform_add_devices(paz00_devices, ARRAY_SIZE(paz00_devices));

	paz00_sdhci_init();
	paz00_i2c_init();
	paz00_power_init();
	paz00_panel_init();
	paz00_wifi_init();
}

MACHINE_START(PAZ00, "paz00")
	.boot_params  = 0x00000100,
	.phys_io        = IO_APB_PHYS,
	.io_pg_offst    = ((IO_APB_VIRT) >> 18) & 0xfffc,
	.fixup		= tegra_paz00_fixup,
	.init_irq       = tegra_init_irq,
	.init_machine   = tegra_paz00_init,
	.map_io         = tegra_map_common_io,
	.timer          = &tegra_timer,
MACHINE_END

MACHINE_START(TEGRA_LEGACY, "tegra_legacy")
	.boot_params  = 0x00000100,
	.phys_io        = IO_APB_PHYS,
	.io_pg_offst    = ((IO_APB_VIRT) >> 18) & 0xfffc,
	.fixup		= tegra_paz00_fixup,
	.init_irq       = tegra_init_irq,
	.init_machine   = tegra_paz00_init,
	.map_io         = tegra_map_common_io,
	.timer          = &tegra_timer,
MACHINE_END

MACHINE_START(HARMONY, "harmony")
	.boot_params  = 0x00000100,
	.phys_io        = IO_APB_PHYS,
	.io_pg_offst    = ((IO_APB_VIRT) >> 18) & 0xfffc,
	.fixup		= tegra_paz00_fixup,
	.init_irq       = tegra_init_irq,
	.init_machine   = tegra_paz00_init,
	.map_io         = tegra_map_common_io,
	.timer          = &tegra_timer,
MACHINE_END
