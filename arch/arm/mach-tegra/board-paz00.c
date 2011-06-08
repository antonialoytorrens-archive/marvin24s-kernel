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
#include <linux/i2c.h>
#include <linux/i2c-tegra.h>
#include <linux/io.h>
#include <linux/tegra_usb.h>
#include <linux/fsl_devices.h>
#include "../../../drivers/staging/nvec/nvec.h"
#include <linux/gpio.h>
#include <linux/leds.h>

#include <sound/alc5632.h>

#include <asm/mach-types.h>
#include <asm/mach/arch.h>
#include <asm/setup.h>

#include <mach/iomap.h>
#include <mach/irqs.h>
#include <mach/clk.h>
#include <mach/usb_phy.h>
#include <mach/suspend.h>
#include <mach/paz00_audio.h>

#include "clock.h"
#include "board.h"
#include "board-paz00.h"
#include "devices.h"


/* output atags / or not */
#define PRINT_ATAGS

#ifdef PRINT_ATAGS

#define ATAG_NVIDIA             0x41000801

struct tag_tegra {
	__u32 bootarg_len;
	__u32 bootarg_key;
	__u32 bootarg_nvkey;
	__u32 bootarg[];
};

enum {
	RM = 1,
	DISPLAY,
	FRAMEBUFFER,
	CHIPSHMOO,
	CHIPSHMOO_PHYS,
	CARVEOUT,
	WARMBOOT,
};

static int __init parse_tag_nvidia(const struct tag *tag)
{
	int i;
	struct tag_tegra *nvtag = (struct tag_tegra *)tag;

	switch(nvtag->bootarg_nvkey) {
		case RM:
			printk("RM             ");
			break;
		case DISPLAY:
			printk("DISPLAY        ");
			break;
		case FRAMEBUFFER:
			printk("FRAMEBUFFER    ");
			break;
		case CHIPSHMOO:
			printk("CHIPSHMOO      ");
			break;
		case CHIPSHMOO_PHYS:
			printk("CHIPSHMOO_PHYS ");
			break;
		case CARVEOUT:
			printk("CARVEOUT       ");
			break;
		case WARMBOOT:
			printk("WARMBOOT       ");
			break;
		default:
			if(nvtag->bootarg_nvkey & 0x10000)
				printk("PreMemHdl %4d ", nvtag->bootarg_nvkey & 0xFFFF);
			else
				printk("unknown (%d) ", nvtag->bootarg_nvkey);
			break;
	}

	for(i=0; i < tag->hdr.size-2; i++)
		printk("%08x ", nvtag->bootarg[i]);
	printk("\n");

	return 0;
}
__tagtable(ATAG_NVIDIA, parse_tag_nvidia);

#endif

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

static struct tegra_ulpi_config ulpi_phy_config = {
	.reset_gpio = PAZ00_ULPI_RST,
	.clk = "cdev2",
};

static struct tegra_ehci_platform_data tegra_ehci_pdata[] = {
	[0] = {
		.operating_mode = TEGRA_USB_OTG,
		.power_down_on_bus_suspend = 1,
	},
	[1] = {
		.phy_config = &ulpi_phy_config,
		.operating_mode = TEGRA_USB_HOST,
		.power_down_on_bus_suspend = 1,
	},
	[2] = {
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

static struct tegra_i2c_platform_data paz00_dvc_platform_data = {
	.adapter_nr	= 4,
	.bus_count	= 1,
	.bus_clk_rate	= { 400000, 0 },
	.is_dvc		= true,
};

static struct nvec_platform_data paz00_nvec_platform_data = {
	.i2c_addr = 0x8a,
	.gpio = PAZ00_NVEC_REQ,
};

static struct alc5632_platform_data alc5632_pdata = {
	.add_ctrl = 0x0400,
	.jack_det_ctrl = -1,
};

static struct paz00_audio_platform_data audio_pdata = {
/* speaker enable goes via nvec */
	.gpio_hp_det	= PAZ00_HP_DET,
};

static struct platform_device audio_device = {
	.name	= "tegra-snd-paz00",
	.id	= 0,
	.dev	= {
		.platform_data = &audio_pdata,
	},
};

static struct i2c_board_info __initdata alc5632_board_info = {
	I2C_BOARD_INFO("alc5632", 0x1e),
	.platform_data = &alc5632_pdata,
};


static struct i2c_board_info __initdata atd7461_board_info = {
	I2C_BOARD_INFO("atd7461", 0x4c), /* aka lm90 */
};

static void paz00_i2c_init(void)
{
	tegra_i2c_device1.dev.platform_data = &paz00_i2c1_platform_data;
	tegra_i2c_device2.dev.platform_data = &paz00_i2c2_platform_data;
	tegra_i2c_device3.name = "nvec";
	tegra_i2c_device3.dev.platform_data = &paz00_nvec_platform_data;
	tegra_i2c_device4.dev.platform_data = &paz00_dvc_platform_data;

	platform_device_register(&tegra_i2c_device1);
	platform_device_register(&tegra_i2c_device2);
	platform_device_register(&tegra_i2c_device3);
	platform_device_register(&tegra_i2c_device4);

	i2c_register_board_info(0, &alc5632_board_info, 1);

	i2c_register_board_info(4, &atd7461_board_info, 1);
}

static struct resource tegra_rtc_resources[] = {
	[0] = {
		.start = TEGRA_RTC_BASE,
		.end = TEGRA_RTC_BASE + TEGRA_RTC_SIZE - 1,
		.flags = IORESOURCE_MEM,
	},
	[1] = {
		.start = INT_RTC,
		.end = INT_RTC,
		.flags = IORESOURCE_IRQ,
	},
};

static struct platform_device tegra_rtc_device = {
	.name = "tegra_rtc",
	.id   = -1,
	.resource = tegra_rtc_resources,
	.num_resources = ARRAY_SIZE(tegra_rtc_resources),
};

static struct gpio_led gpio_leds[] = {
	{
		.name			= "wifi-led",
		.default_trigger	= "rfkill0",
		.gpio			= PAZ00_WIFI_LED,
	},
};

static struct gpio_led_platform_data gpio_led_info = {
	.leds		= gpio_leds,
	.num_leds	= ARRAY_SIZE(gpio_leds),
};

static struct platform_device leds_gpio = {
	.name   = "leds-gpio",
	.id     = -1,
	.dev    = {
		.platform_data  = &gpio_led_info,
	},
};

/* set wifi power gpio */
static void __init paz00_wifi_init(void)
{
	int ret;

	/* unlock hw rfkill */
	ret = gpio_request_one(PAZ00_WIFI_PWRN, GPIOF_OUT_INIT_HIGH, "wifi_pwrn");
	if (ret) {
		pr_warning("WIFI: could not request WIFI PWR gpio!\n");
		return;
	}
}

static struct platform_device *paz00_devices[] __initdata = {
	&debug_uart,
//	&tegra_rtc_device,
	&pmu_device,
	&tegra_udc_device,
//	&tegra_ehci1_device,
	&tegra_ehci2_device,
	&tegra_ehci3_device,
	&tegra_spi_device1,
	&tegra_spi_device2,
	&tegra_spi_device3,
	&tegra_spi_device4,
	&tegra_gart_device,
	&leds_gpio,
	&tegra_i2s_device1,
	&tegra_das_device,
	&tegra_pcm_device,
	&audio_device,
	&tegra_avp_device,
};

static void __init tegra_paz00_fixup(struct machine_desc *desc,
	struct tag *tags, char **cmdline, struct meminfo *mi)
{
	mi->nr_banks = 1;
	mi->bank[0].start = PHYS_OFFSET;
	mi->bank[0].size = 448 * SZ_1M;
}

static __initdata struct tegra_clk_init_table paz00_clk_init_table[] = {
	/* name		parent		rate		enabled */
/*	{ "clk_dev1",	NULL,		26000000,	true},
	{ "vi_sensor",	"pll_m",	111000000,	false},
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
	{ "i2s2",	"clk_m",	11289600,	false},
	{ "spdif_out",	"clk_m",	12000000,	false},
	{ "spi",	"clk_m",	12000000,	false},
	{ "xio",	"clk_m",	12000000,	false},
	{ "twc",	"clk_m",	12000000,	false},
	{ "sbc1",	"clk_m",	12000000,	false},
	{ "sbc2",	"clk_m",	12000000,	false},
	{ "sbc3",	"clk_m",	12000000,	false},
	{ "sbc4",	"clk_m",	12000000,	false},
	{ "vfir",	"clk_m",	12000000,	false},
	{ "la",		"clk_m",	12000000,	false},
	{ "owr",	"clk_m",	12000000,	false},
	{ "dvc",	"clk_m",	3000000,	false},
	{ "cve",	"clk_m",	12000000,	false},
	{ "tvdac",	"clk_m",	12000000,	false},
	{ "usbd",	"clk_m",	12000000,	false},
	{ "usb2",	"clk_m",	12000000,	false},
	{ "usb3",	"clk_m",	12000000,	true},
	{ "isp",	"clk_m",	12000000,	false},
	{ "csus",	"clk_m",	12000000,	false},
	{ "clk_32k",	NULL,		32768,		true},
	{ "pll_s",	"clk_32k",	32768,		false},
	{ "kbc",	"clk_32k",	32768,		true}, */

	{ "uartd",	"pll_p",	216000000,	true},

	{ "audio",	"pll_a_out0",	11289600,	true},
	{ "pll_a",	"pll_p_out1",	56448000,	true},
	{ "pll_a_out0",	"pll_a",	11289600,	true},
	{ "cdev1",	"pll_a_out0",	11289600,	true},
	{ "audio_2x",	"audio",	22579200,	false},
	{ "i2s1",	"pll_a_out0",	11289600,	false},

	{ "apbdma",	"hclk",		54000000,	true},
	{ "clk_d",	"clk_m",	24000000,	true},
	{ "uarta",	"clk_m",	12000000,	true},
	{ "pwm",	"clk_32k",	32768,		true},
	{ "pll_c",	"clk_m",	600000000,	true},
	{ "pll_c_out1",	"pll_c",	240000000,	true},
	{ "pll_p_out4", "pll_p",        24000000,       true},
	{ "i2c1_i2c",	"pll_p_out3",	72000000,	true},
	{ "i2c2_i2c",	"pll_p_out3",	72000000,	true},
	{ "i2c3_i2c",	"pll_p_out3",	72000000,	true},
	{ "dvc_i2c",	"pll_p_out3",	72000000,	true},
	{ "i2c1",	"clk_m",	3000000,	false},
	{ "i2c2",	"clk_m",	3000000,	false},
	{ "i2c3",	"clk_m",	3000000,	true},

	{ "3d",		"pll_m",	266400000,	true},
	{ "2d",		"pll_m",	266400000,	true},
	{ "vi",		"pll_m",	50000000,	true},
	{ "epp",	"pll_m",	266400000,	true},
	{ "mpe",	"pll_m",	111000000,	false},
	{ "disp2",	"clk_m",	12000000,	true},
	{ "dsi",	"pll_d",	1000000,	false},
	{ "hdmi",	"clk_m",	12000000,	false},
	{ "vde",	"pll_c",	240000000,	false},
	{ "nor",	"clk_m",	12000000,	false},
	{ "mipi",	"clk_m",	12000000,	false},
	{ "ide",	"clk_m",	12000000,	false},
	{ "tvo",	"clk_m",	12000000,	false},
	{ "ndflash",	"clk_m",	108000000,	true},

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

/* don't init ehci1 hub here, because firmware set this to otg mode
	tegra_ehci1_device.dev.platform_data = &tegra_ehci_pdata[0]; */
	tegra_ehci2_device.dev.platform_data = &tegra_ehci_pdata[1];
	tegra_ehci3_device.dev.platform_data = &tegra_ehci_pdata[2];

	platform_add_devices(paz00_devices, ARRAY_SIZE(paz00_devices));

	paz00_sdhci_init();
	paz00_i2c_init();
	paz00_power_init();
	paz00_panel_init();

/* This is still a bit unstable
   leave it off for now
	paz00_emc_init(); */
	paz00_wifi_init();
}

/* for newer bootloaders */
MACHINE_START(PAZ00, "Toshiba AC100 / Dynabook AZ")
	.boot_params	= 0x00000100,
	.fixup		= tegra_paz00_fixup,
	.init_irq	= tegra_init_irq,
	.init_machine	= tegra_paz00_init,
	.map_io		= tegra_map_common_io,
	.timer		= &tegra_timer,
MACHINE_END

/* for newer bootloaders */
MACHINE_START(LEGACY, "Legacy")
	.boot_params	= 0x00000100,
	.fixup		= tegra_paz00_fixup,
	.init_irq	= tegra_init_irq,
	.init_machine	= tegra_paz00_init,
	.map_io		= tegra_map_common_io,
	.timer		= &tegra_timer,
MACHINE_END

/* this is need for some unknown reason */
MACHINE_START(HARMONY, "harmony")
MACHINE_END
