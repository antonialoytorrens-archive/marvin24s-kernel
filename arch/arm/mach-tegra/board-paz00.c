/*
 * arch/arm/mach-tegra/board-paz00.c
 *
 * Copyright (C) 2011 Marc Dietrich <marvin24@gmx.de>
 *
 * Based on board-harmony.c
 * Copyright (C) 2010 Google, Inc.
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
#include <linux/dma-mapping.h>
#include <linux/pda_power.h>
#include <linux/io.h>
#include <linux/i2c.h>
#include <linux/i2c-tegra.h>
#include <linux/platform_data/tegra_usb.h>

#include <asm/mach-types.h>
#include <asm/mach/arch.h>
#include <asm/mach/time.h>
#include <asm/setup.h>

#include <sound/alc5632.h>

#include <mach/iomap.h>
#include <mach/irqs.h>
#include <mach/sdhci.h>
#include <mach/usb_phy.h>
#include <mach/gpio.h>
#include <mach/suspend.h>
#include <mach/paz00_audio.h>

#include "board.h"
#include "board-paz00.h"
#include "clock.h"
#include "devices.h"
#include "gpio-names.h"
#include "../../../drivers/staging/nvec/nvec.h"

/* output atags / or not */
#define PRINT_ATAGS

#ifdef PRINT_ATAGS

#define ATAG_NVIDIA             0x41000801
#define MAX_MEMHDL		8

struct tag_tegra {
	__u32 bootarg_len;
	__u32 bootarg_key;
	__u32 bootarg_nvkey;
	__u32 bootarg[];
};

struct memhdl {
	__u32 id;
	__u32 start;
	__u32 size;
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

int num_memhdl = 0;

struct memhdl nv_memhdl[MAX_MEMHDL];


static int __init parse_tag_nvidia(const struct tag *tag)
{
	int i;
	struct tag_tegra *nvtag = (struct tag_tegra *)tag;
	__u32 id;

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
			id = nvtag->bootarg[1];
			for(i=0; i<num_memhdl; i++) {
				if (nv_memhdl[i].id == id) {
					tegra_lp0_vec_start = nv_memhdl[i].start;
					tegra_lp0_vec_size = nv_memhdl[i].size;
				}
			}
			printk("WARMBOOT       ");
			break;
		default:
			if(nvtag->bootarg_nvkey & 0x10000) {
				id = nvtag->bootarg_nvkey;
				if (num_memhdl < MAX_MEMHDL) {
					nv_memhdl[num_memhdl].id = id;
					nv_memhdl[num_memhdl].start = nvtag->bootarg[1];
					nv_memhdl[num_memhdl].size = nvtag->bootarg[2];
					num_memhdl++;
				}
				printk("PreMemHdl %4d ", id & 0xffff);
			}
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
		.membase	= IO_ADDRESS(TEGRA_UARTA_BASE),
		.mapbase	= TEGRA_UARTA_BASE,
		.irq		= INT_UARTA,
		.flags		= UPF_BOOT_AUTOCONF,
		.iotype		= UPIO_MEM,
		.regshift	= 2,
		.uartclk	= 216000000,
	}, {
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

static struct tegra_i2c_platform_data paz00_i2c1_platform_data = {
	.adapter_nr	= 0,
	.bus_count	= 1,
	.bus_clk_rate   = { 400000, 0 },
	.slave_addr	= 0xfc,
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
	.bus_clk_rate   = { 400000, 100000 },
	.bus_mux	= { &i2c2_ddc, &i2c2_gen2 },
	.bus_mux_len	= { 1, 1 },
	.slave_addr	= 0xfc,
};

static struct nvec_platform_data paz00_nvec_platform_data = {
	.i2c_addr	= 0x8a,
	.gpio		= TEGRA_NVEC_REQ,
};

static struct alc5632_platform_data alc5632_pdata = {
	.add_ctrl = 0x0400,
	.jack_det_ctrl = -1,
};

static struct paz00_audio_platform_data audio_pdata = {
/* speaker enable goes via nvec */
	.gpio_hp_det	= TEGRA_HP_DET,
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

static struct tegra_i2c_platform_data paz00_dvc_platform_data = {
	.adapter_nr	= 4,
	.bus_count	= 1,
	.bus_clk_rate   = { 400000, 0 },
	.is_dvc		= true,
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
}

static struct tegra_ulpi_config ulpi_phy_config = {
		.reset_gpio = TEGRA_ULPI_RST,
		.clk = "cdev2",
};

static struct tegra_ehci_platform_data tegra_ehci_pdata[] = {
		[0] = {
			.operating_mode = TEGRA_USB_HOST,
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

static void paz00_usb_init(void)
{
	tegra_ehci2_device.dev.platform_data = &tegra_ehci_pdata[1];
	tegra_ehci3_device.dev.platform_data = &tegra_ehci_pdata[2];

	platform_device_register(&tegra_ehci2_device);
	platform_device_register(&tegra_ehci3_device);
}

static void __init tegra_paz00_fixup(struct machine_desc *desc,
	struct tag *tags, char **cmdline, struct meminfo *mi)
{
	mi->nr_banks = 1;
	mi->bank[0].start = PHYS_OFFSET;
	mi->bank[0].size = 448 * SZ_1M;
}

static struct gpio_led gpio_leds[] = {
	{
		.name			= "wifi-led",
		.default_trigger	= "rfkill0",
		.gpio			= TEGRA_WIFI_LED,
	},
};

static struct gpio_led_platform_data gpio_led_info = {
		.leds           = gpio_leds,
		.num_leds       = ARRAY_SIZE(gpio_leds),
};

static struct platform_device leds_gpio = {
	.name   = "leds-gpio",
	.id     = -1,
	.dev    = {
		.platform_data  = &gpio_led_info,
	},
};

static struct platform_device *paz00_devices[] __initdata = {
	&debug_uart,
	&tegra_pmu_device,
//	&tegra_rtc_device,
	&tegra_udc_device,
	&tegra_sdhci_device4,
	&tegra_sdhci_device1,
	&tegra_spi_device1,
	&tegra_spi_device2,
	&tegra_spi_device3,
	&tegra_spi_device4,
	&leds_gpio,
	&tegra_gart_device,
	&tegra_i2s_device1,
	&tegra_das_device,
	&tegra_pcm_device,
	&audio_device,
	&tegra_avp_device,
};

static void __init paz00_wifi_init(void)
{
	int ret;

	/* unlock hw rfkill */
	ret = gpio_request_one(TEGRA_WIFI_PWRN, GPIOF_OUT_INIT_HIGH,
			"wifi_pwrn");
	if(ret) {
		pr_warning("WIFI: could not request PWRN gpio!\n");
		return;
	}
	gpio_export(TEGRA_WIFI_PWRN, 0);
}

static __initdata struct tegra_clk_init_table paz00_clk_init_table[] = {
	/* name		parent		rate		enabled */
/* for serial port */
	{ "uartd",	"pll_p",	216000000,	true },

/* these are switched on by bootloader */
	{ "audio",	"pll_a_out0",	11289600,	true },
	{ "uarta",	"pll_p",	216000000,	true },
	{ "i2c3_i2c",	"pll_p_out3",	72000000,	true },
	{ "clk_d",	"clk_m",	24000000,	true },
	{ "pll_a_out0",	"pll_a",	11289600,	true },
	{ "pll_a",	"pll_p_out1",	56448000,	true },
	{ "pll_p_out4",	"pll_p",	24000000,	true },
	{ "pll_c_out1",	"pll_c",	240000000,	true },
	{ "pll_c",	"clk_m",        600000000,      true },
	
/* these are used for audio */
	{ "cdev1",	"pll_a_out0",	11289600,	true},
	{ "audio_2x",	"audio",	22579200,	false},
	{ "i2s1",	"pll_a_out0",	11289600,	false},

	{ NULL,		NULL,		0,		0},
};

static struct tegra_sdhci_platform_data sdhci_pdata1 = {
	.cd_gpio	= TEGRA_GPIO_SD1_CD,
	.wp_gpio	= TEGRA_GPIO_SD1_WP,
	.power_gpio	= TEGRA_GPIO_SD1_POWER,
};

static struct tegra_sdhci_platform_data sdhci_pdata4 = {
	.cd_gpio	= -1,
	.wp_gpio	= -1,
	.power_gpio	= -1,
	.is_8bit	= 1,
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
	tegra_init_suspend(&paz00_suspend);

	tegra_clk_init_from_table(paz00_clk_init_table);

	paz00_pinmux_init();

	tegra_sdhci_device1.dev.platform_data = &sdhci_pdata1;
	tegra_sdhci_device4.dev.platform_data = &sdhci_pdata4;

	platform_add_devices(paz00_devices, ARRAY_SIZE(paz00_devices));

	paz00_i2c_init();
	paz00_power_init();
	paz00_panel_init();
	paz00_usb_init();
	paz00_emc_init();
	paz00_wifi_init();
}

MACHINE_START(PAZ00, "Toshiba AC100 / Dynabook AZ")
	.boot_params	= 0x00000100,
	.fixup		= tegra_paz00_fixup,
	.map_io         = tegra_map_common_io,
	.init_early	= tegra_init_early,
	.init_irq       = tegra_init_irq,
	.timer          = &tegra_timer,
	.init_machine   = tegra_paz00_init,
MACHINE_END

MACHINE_START(HARMONY, "harmony")
MACHINE_END
