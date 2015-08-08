/*
 * arch/arm/mach-tegra/board-x3.c
 *
 * Copyright (c) 2011, NVIDIA Corporation.
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

#include <linux/kernel.h>
#include <linux/init.h>
#include <linux/slab.h>
#include <linux/ctype.h>
#include <linux/platform_device.h>
#include <linux/clk.h>
#include <linux/i2c.h>
#include <linux/dma-mapping.h>
#include <linux/delay.h>
#include <linux/i2c-tegra.h>
#include <linux/gpio.h>
#include <linux/input.h>

#include <mach/clk.h>
#include <mach/iomap.h>
#include <mach/irqs.h>
#include <mach/pinmux.h>
#include <mach/iomap.h>
#include <mach/io.h>
#include <asm/mach-types.h>
#include <asm/mach/arch.h>

#include <mach-tegra/board.h>
#include <lge/board-x3.h>
#include <lge/board-x3-bt.h>
#include <mach-tegra/devices.h>
#include <mach-tegra/gpio-names.h>

//                                                 
#ifdef CONFIG_BCM4330_RFKILL
#include <linux/lbee9qmb-rfkill.h>

#define GPIO_BT_RESET		TEGRA_GPIO_PCC5
#define GPIO_BT_WAKE		TEGRA_GPIO_PS3
#define GPIO_BT_HOSTWAKE	TEGRA_GPIO_PS4


static struct lbee9qmb_platform_data lbee9qmb_platform = {
	.gpio_reset = GPIO_BT_RESET,
#ifdef CONFIG_BRCM_BT_WAKE
	.gpio_btwake = GPIO_BT_WAKE,
#endif
#ifdef CONFIG_BRCM_HOST_WAKE
	.gpio_hostwake = GPIO_BT_HOSTWAKE,
#endif
	.active_low = 0, /* 0: active high, 1: active low */
	.delay = 100,
};
static struct platform_device lbee9qmb_device = {
    .name = "lbee9qmb-rfkill",
    .dev = {
        .platform_data = &lbee9qmb_platform,
    },
};

void x3_bt_rfkill(void)
{
    tegra_gpio_enable(GPIO_BT_RESET);
    printk(KERN_DEBUG "%s : tegra_gpio_enable(reset) [%d]", __func__, GPIO_BT_RESET);
    tegra_gpio_enable(GPIO_BT_WAKE);
    printk(KERN_DEBUG "%s : tegra_gpio_enable(btwake) [%d]", __func__, GPIO_BT_WAKE);
    tegra_gpio_enable(GPIO_BT_HOSTWAKE);
    printk(KERN_DEBUG "%s : tegra_gpio_enable(hostwake) [%d]", __func__, GPIO_BT_HOSTWAKE);

    if (platform_device_register(&lbee9qmb_device))
        printk(KERN_DEBUG "%s: lbee9qmb_device registration failed \n", __func__);
	else
        printk(KERN_DEBUG "%s: lbee9qmb_device registration OK \n", __func__);
		
    return;
}
#endif /* CONFIG_BCM4330_RFKILL */
//                                                 

static struct resource x3_bluesleep_resources[] = {
	[0] = {
		.name = "gpio_host_wake",
			.start  = TEGRA_GPIO_PS4,
			.end    = TEGRA_GPIO_PS4,
			.flags  = IORESOURCE_IO,
	},
	[1] = {
		.name = "gpio_ext_wake",
			.start  = TEGRA_GPIO_PS3,
			.end    = TEGRA_GPIO_PS3,
			.flags  = IORESOURCE_IO,
	},
	[2] = {
		.name = "host_wake",
			.flags  = IORESOURCE_IRQ | IORESOURCE_IRQ_HIGHEDGE,
	},
};

static struct platform_device x3_bluesleep_device = {
	.name           = "bluesleep",
	.id             = -1,
	.num_resources  = ARRAY_SIZE(x3_bluesleep_resources),
	.resource       = x3_bluesleep_resources,
};

extern void bluesleep_setup_uart_port(struct platform_device *uart_dev);

void __init x3_setup_bluesleep(void)
{
	x3_bluesleep_resources[2].start =
		x3_bluesleep_resources[2].end =
			gpio_to_irq(TEGRA_GPIO_PS4);
	platform_device_register(&x3_bluesleep_device);
	bluesleep_setup_uart_port(&tegra_uartc_device);
	tegra_gpio_enable(TEGRA_GPIO_PS4);
	tegra_gpio_enable(TEGRA_GPIO_PS3);

	return;
}
