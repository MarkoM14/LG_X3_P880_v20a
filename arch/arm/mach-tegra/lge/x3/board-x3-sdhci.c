/*
 * arch/arm/mach-tegra/board-x3-sdhci.c
 *
 * Copyright (C) 2011 NVIDIA Corporation.
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
#include <linux/wlan_plat.h>
#include <linux/delay.h>
#include <linux/gpio.h>
#include <linux/clk.h>
#include <linux/err.h>
#include <linux/mmc/host.h>
#include <linux/fs.h>

#include <asm/mach-types.h>
#include <mach/irqs.h>
#include <mach/iomap.h>
#include <mach/sdhci.h>

#include <mach-tegra/gpio-names.h>
#include <mach-tegra/board.h>
#include <lge/board-x3.h>
#include "../../../include/asm/hw_irq.h"

#ifdef CONFIG_DHD_USE_STATIC_BUF
#include <linux/skbuff.h>
#include <linux/wlan_plat.h>
#endif

#define X3_WLAN_RST	TEGRA_GPIO_PV3
#define X3_WLAN_WOW	TEGRA_GPIO_PU6
#define X3_SD_CD TEGRA_GPIO_PW5
#define X3_SD_WP TEGRA_GPIO_PT3

static void (*wifi_status_cb)(int card_present, void *dev_id);
static void *wifi_status_cb_devid;
static int x3_wifi_status_register(void (*callback)(int , void *), void *);

static int x3_wifi_reset(int on);
static int x3_wifi_power(int on);
static int x3_wifi_set_carddetect(int val);

#ifdef CONFIG_DHD_USE_STATIC_BUF
#define WLAN_STATIC_SCAN_BUF0		5
#define WLAN_STATIC_SCAN_BUF1		6
#define PREALLOC_WLAN_SEC_NUM		4
#define PREALLOC_WLAN_BUF_NUM		160 *2
#define PREALLOC_WLAN_SECTION_HEADER	24

#define WLAN_SECTION_SIZE_0	(PREALLOC_WLAN_BUF_NUM * 128)  //20K *2
#define WLAN_SECTION_SIZE_1	(PREALLOC_WLAN_BUF_NUM * 128)  //20K *2
#define WLAN_SECTION_SIZE_2	(PREALLOC_WLAN_BUF_NUM * 512)  //80K *2
#define WLAN_SECTION_SIZE_3	(PREALLOC_WLAN_BUF_NUM * 1024) //160K* 2

#define DHD_SKB_HDRSIZE			336
#define DHD_SKB_1PAGE_BUFSIZE	((PAGE_SIZE*1)-DHD_SKB_HDRSIZE)
#define DHD_SKB_2PAGE_BUFSIZE	((PAGE_SIZE*2)-DHD_SKB_HDRSIZE)
#define DHD_SKB_4PAGE_BUFSIZE	((PAGE_SIZE*4)-DHD_SKB_HDRSIZE)

#define WLAN_SKB_BUF_NUM	17

static struct sk_buff *wlan_static_skb[WLAN_SKB_BUF_NUM];

struct wlan_mem_prealloc {
	void *mem_ptr;
	unsigned long size;
};

static struct wlan_mem_prealloc wlan_mem_array[PREALLOC_WLAN_SEC_NUM] = {
	{NULL, (WLAN_SECTION_SIZE_0 + PREALLOC_WLAN_SECTION_HEADER)},
	{NULL, (WLAN_SECTION_SIZE_1 + PREALLOC_WLAN_SECTION_HEADER)},
	{NULL, (WLAN_SECTION_SIZE_2 + PREALLOC_WLAN_SECTION_HEADER)},
	{NULL, (WLAN_SECTION_SIZE_3 + PREALLOC_WLAN_SECTION_HEADER)}
};

void *wlan_static_scan_buf0;
void *wlan_static_scan_buf1;
static void *x3_wifi_mem_prealloc(int section, unsigned long size)
{
	if (section == PREALLOC_WLAN_SEC_NUM)
		return wlan_static_skb;
	if (section == WLAN_STATIC_SCAN_BUF0)
		return wlan_static_scan_buf0;
	if (section == WLAN_STATIC_SCAN_BUF1)
		return wlan_static_scan_buf1;
	if ((section < 0) || (section > PREALLOC_WLAN_SEC_NUM))
		return NULL;

	if (wlan_mem_array[section].size < size)
		return NULL;

	return wlan_mem_array[section].mem_ptr;
}

static int x3_init_wlan_mem(void)
{
	int i;
	int j;

	for (i = 0; i < 8; i++) {
		wlan_static_skb[i] = dev_alloc_skb(DHD_SKB_1PAGE_BUFSIZE);
		if (!wlan_static_skb[i])
			goto err_skb_alloc;
	}

	for (; i < 16; i++) {
		wlan_static_skb[i] = dev_alloc_skb(DHD_SKB_2PAGE_BUFSIZE);
		if (!wlan_static_skb[i])
			goto err_skb_alloc;
	}

	wlan_static_skb[i] = dev_alloc_skb(DHD_SKB_4PAGE_BUFSIZE);
	if (!wlan_static_skb[i])
		goto err_skb_alloc;

	for (i = 0 ; i < PREALLOC_WLAN_SEC_NUM ; i++) {
		wlan_mem_array[i].mem_ptr =
				kmalloc(wlan_mem_array[i].size, GFP_KERNEL);

		if (!wlan_mem_array[i].mem_ptr)
			goto err_mem_alloc;
	}
	wlan_static_scan_buf0 = kmalloc (65536, GFP_KERNEL);
	if(!wlan_static_scan_buf0)
		goto err_mem_alloc;
	wlan_static_scan_buf1 = kmalloc (65536, GFP_KERNEL);
	if(!wlan_static_scan_buf1)
		goto err_mem_alloc;

	printk("%s: WIFI MEM Allocated\n", __FUNCTION__);
	return 0;

 err_mem_alloc:
	pr_err("Failed to mem_alloc for WLAN\n");
	for (j = 0 ; j < i ; j++)
		kfree(wlan_mem_array[j].mem_ptr);

	i = WLAN_SKB_BUF_NUM;

 err_skb_alloc:
	pr_err("Failed to skb_alloc for WLAN\n");
	for (j = 0 ; j < i ; j++)
		dev_kfree_skb(wlan_static_skb[j]);

	return -ENOMEM;
}
#endif /* CONFIG_DHD_USE_STATIC_BUF */

static struct wifi_platform_data x3_wifi_control = {
	.set_power      = x3_wifi_power,
	.set_reset      = x3_wifi_reset,
	.set_carddetect = x3_wifi_set_carddetect,
#if defined(CONFIG_DHD_USE_STATIC_BUF)
	.mem_prealloc   = x3_wifi_mem_prealloc,
#endif
};

static struct resource wifi_resource[] = {
	[0] = {
		.name	= "bcmdhd_wlan_irq",
		.start	= TEGRA_GPIO_TO_IRQ(X3_WLAN_WOW),
		.end	= TEGRA_GPIO_TO_IRQ(X3_WLAN_WOW),
		.flags	= IORESOURCE_IRQ | IORESOURCE_IRQ_HIGHLEVEL | IORESOURCE_IRQ_SHAREABLE,
	},
};

static struct platform_device x3_wifi_device = {
	.name           = "bcmdhd_wlan",
	.id             = 1,
	.num_resources	= 1,
	.resource	= wifi_resource,
	.dev            = {
		.platform_data = &x3_wifi_control,
	},
};

static struct resource sdhci_resource0[] = {
	[0] = {
		.start  = INT_SDMMC1,
		.end    = INT_SDMMC1,
		.flags  = IORESOURCE_IRQ,
	},
	[1] = {
		.start	= TEGRA_SDMMC1_BASE,
		.end	= TEGRA_SDMMC1_BASE + TEGRA_SDMMC1_SIZE-1,
		.flags	= IORESOURCE_MEM,
	},
};

static struct resource sdhci_resource2[] = {
	[0] = {
		.start  = INT_SDMMC3,
		.end    = INT_SDMMC3,
		.flags  = IORESOURCE_IRQ,
	},
	[1] = {
		.start	= TEGRA_SDMMC3_BASE,
		.end	= TEGRA_SDMMC3_BASE + TEGRA_SDMMC3_SIZE-1,
		.flags	= IORESOURCE_MEM,
	},
};

static struct resource sdhci_resource3[] = {
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

#ifdef CONFIG_MMC_EMBEDDED_SDIO
static struct embedded_sdio_data embedded_sdio_data0 = {
	.cccr   = {
		.sdio_vsn       = 2,
		.multi_block    = 1,
		.low_speed      = 0,
		.wide_bus       = 0,
		.high_power     = 1,
		.high_speed     = 1,
	},
	.cis  = {
		.vendor         = 0x02d0,
		.device         = 0x4330,
	},
};
#endif

static struct tegra_sdhci_platform_data tegra_sdhci_platform_data0 = {
	.mmc_data = {
		.register_status_notify	= x3_wifi_status_register,
#ifdef CONFIG_MMC_EMBEDDED_SDIO
		.embedded_sdio = &embedded_sdio_data0,
#endif
		.built_in = 0,
		.ocr_mask = MMC_OCR_1V8_MASK,
	},
#ifndef CONFIG_MMC_EMBEDDED_SDIO
	.pm_flags = MMC_PM_KEEP_POWER,
#endif
	.cd_gpio = -1,
	.wp_gpio = -1,
	.power_gpio = -1,
	.tap_delay = 0x0F,
	.max_clk_limit = 45000000,
	.ddr_clk_limit = 41000000,
};

static struct tegra_sdhci_platform_data tegra_sdhci_platform_data2 = {
	.cd_gpio = -1,
	.wp_gpio = -1,
	.power_gpio = -1,
	.tap_delay = 0x0F,
	.ddr_clk_limit = 41000000,
	.mmc_data = {
		.built_in = 0,
	}
};

static struct tegra_sdhci_platform_data tegra_sdhci_platform_data3 = {
	.cd_gpio = -1,
	.wp_gpio = -1,
	.power_gpio = -1,
	.is_8bit = 1,
	.tap_delay = 0x0F,
	.ddr_clk_limit = 41000000,
	.mmc_data = {
		.built_in = 1,
		.ocr_mask = MMC_OCR_1V8_MASK,
	}
};

static struct platform_device tegra_sdhci_device0 = {
	.name		= "sdhci-tegra",
	.id		= 0,
	.resource	= sdhci_resource0,
	.num_resources	= ARRAY_SIZE(sdhci_resource0),
	.dev = {
		.platform_data = &tegra_sdhci_platform_data0,
	},
};

static struct platform_device tegra_sdhci_device2 = {
	.name		= "sdhci-tegra",
	.id		= 2,
	.resource	= sdhci_resource2,
	.num_resources	= ARRAY_SIZE(sdhci_resource2),
	.dev = {
		.platform_data = &tegra_sdhci_platform_data2,
	},
};

static struct platform_device tegra_sdhci_device3 = {
	.name		= "sdhci-tegra",
	.id		= 3,
	.resource	= sdhci_resource3,
	.num_resources	= ARRAY_SIZE(sdhci_resource3),
	.dev = {
		.platform_data = &tegra_sdhci_platform_data3,
	},
};

static int x3_wifi_status_register(
		void (*callback)(int card_present, void *dev_id),
		void *dev_id)
{
	if (wifi_status_cb)
		return -EAGAIN;
	wifi_status_cb = callback;
	wifi_status_cb_devid = dev_id;
	return 0;
}

static int x3_wifi_set_carddetect(int val)
{
	pr_debug("%s: %d\n", __func__, val);
	if (wifi_status_cb)
		wifi_status_cb(val, wifi_status_cb_devid);
	else
		pr_warning("%s: Nobody to notify\n", __func__);
	return 0;
}

static int x3_wifi_power(int on)
{
	pr_debug("%s: %d\n", __func__, on);
#if 0
/* does not use dpd for sdmmc */
	/*
	 * FIXME : we need to revisit IO DPD code
	 * on how should multiple pins under DPD get controlled
	 *
	 * enterprise GPIO WLAN enable is part of SDMMC1 pin group
	 */
	sd_dpd = tegra_io_dpd_get(&tegra_sdhci_device0.dev);
	if (sd_dpd) {
		mutex_lock(&sd_dpd->delay_lock);
		tegra_io_dpd_disable(sd_dpd);
		mutex_unlock(&sd_dpd->delay_lock);
	}
#endif

	if (on) {
		gpio_set_value(X3_WLAN_RST, 1);
		mdelay(100);
		gpio_set_value(X3_WLAN_RST, 0);
		mdelay(100);
		gpio_set_value(X3_WLAN_RST, 1);
		mdelay(200);
	} else {
		gpio_set_value(X3_WLAN_RST, 0);
	}

#if 0
/* does not use dpd for sdmmc */
	if (sd_dpd) {
		mutex_lock(&sd_dpd->delay_lock);
		tegra_io_dpd_enable(sd_dpd);
		mutex_unlock(&sd_dpd->delay_lock);
	}
#endif

	return 0;
}

static int x3_wifi_reset(int on)
{
	pr_debug("%s: do nothing\n", __func__);
	return 0;
}

#if 0
#ifdef CONFIG_TEGRA_PREPOWER_WIFI
static int __init enterprise_wifi_prepower(void)
{
	if (!machine_is_tegra_enterprise())
		return 0;

	enterprise_wifi_power(1);

	return 0;
}

subsys_initcall_sync(enterprise_wifi_prepower);
#endif
#endif

static int __init x3_wifi_init(void)
{
	int rc;

	rc = gpio_request(X3_WLAN_RST, "wlan_rst");
	if (rc)
		pr_err("WLAN_RST gpio request failed:%d\n", rc);

	rc = gpio_request(X3_WLAN_WOW, "bcmsdh_sdmmc");
	if (rc)
		pr_err("WLAN_WOW gpio request failed:%d\n", rc);

	tegra_gpio_enable(X3_WLAN_RST);
	tegra_gpio_enable(X3_WLAN_WOW);

	rc = gpio_direction_output(X3_WLAN_RST, 0);
	if (rc)
		pr_err("WLAN_RST gpio direction configuration failed:%d\n", rc);
	rc = gpio_direction_input(X3_WLAN_WOW);
	if (rc)
		pr_err("WLAN_WOW gpio direction configuration failed:%d\n", rc);

	platform_device_register(&x3_wifi_device);

	return 0;
}

int __init x3_sdhci_init(void)
{
	platform_device_register(&tegra_sdhci_device3);
	platform_device_register(&tegra_sdhci_device0);

	tegra_gpio_enable(X3_SD_CD);
	tegra_sdhci_platform_data2.cd_gpio = X3_SD_CD;
	platform_device_register(&tegra_sdhci_device2);

#ifdef CONFIG_DHD_USE_STATIC_BUF
	x3_init_wlan_mem();
#endif

	x3_wifi_init();	

	return 0;
}
