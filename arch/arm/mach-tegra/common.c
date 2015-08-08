/*
 * arch/arm/mach-tegra/common.c
 *
 * Copyright (C) 2010 Google, Inc.
 * Copyright (C) 2010-2012, NVIDIA Corporation. All rights reserved.
 *
 * Author:
 *	Colin Cross <ccross@android.com>
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

#include <linux/platform_device.h>
#include <linux/console.h>
#include <linux/init.h>
#include <linux/io.h>
#include <linux/clk.h>
#include <linux/delay.h>
#include <linux/highmem.h>
#include <linux/memblock.h>
#include <linux/bitops.h>
#include <linux/sched.h>
#include <linux/cpufreq.h>
#include <linux/of.h>
//#include <linux/dma-mapping.h>
#include <linux/bootmem.h>

#include <asm/hardware/cache-l2x0.h>
#include <asm/system.h>
//#include <asm/dma-mapping.h>

#include <mach/gpio.h>
#include <mach/iomap.h>
#include <mach/pinmux.h>
#include <mach/powergate.h>
#include <mach/system.h>
#include <mach/tegra_smmu.h>

#include "apbio.h"
#include "board.h"
#include "clock.h"
#include "fuse.h"
#include "pm.h"
#include "reset.h"
//                                                                                        
#include "gpio-names.h"
#if defined(CONFIG_MFD_MAX77663)
#include <linux/mfd/max77663-core.h>
#endif
//                                                                                        
#include "devices.h"

#define MC_SECURITY_CFG2	0x7c

#define AHB_ARBITRATION_PRIORITY_CTRL		0x4
#define   AHB_PRIORITY_WEIGHT(x)	(((x) & 0x7) << 29)
#define   PRIORITY_SELECT_USB	BIT(6)
#define   PRIORITY_SELECT_USB2	BIT(18)
#define   PRIORITY_SELECT_USB3	BIT(17)

#define AHB_GIZMO_AHB_MEM		0xc
#define   ENB_FAST_REARBITRATE	BIT(2)
#define   DONT_SPLIT_AHB_WR     BIT(7)

#define   RECOVERY_MODE	BIT(31)
#define   BOOTLOADER_MODE	BIT(30)
#define   FORCED_RECOVERY_MODE	BIT(1)

#define AHB_GIZMO_USB		0x1c
#define AHB_GIZMO_USB2		0x78
#define AHB_GIZMO_USB3		0x7c
#define   IMMEDIATE	BIT(18)

#define AHB_MEM_PREFETCH_CFG3	0xe0
#define AHB_MEM_PREFETCH_CFG4	0xe4
#define AHB_MEM_PREFETCH_CFG1	0xec
#define AHB_MEM_PREFETCH_CFG2	0xf0
#define   PREFETCH_ENB	BIT(31)
#define   MST_ID(x)	(((x) & 0x1f) << 26)
#define   AHBDMA_MST_ID	MST_ID(5)
#define   USB_MST_ID	MST_ID(6)
#define   USB2_MST_ID	MST_ID(18)
#define   USB3_MST_ID	MST_ID(17)
#define   ADDR_BNDRY(x)	(((x) & 0xf) << 21)
#define   INACTIVITY_TIMEOUT(x)	(((x) & 0xffff) << 0)

unsigned long tegra_bootloader_fb_start;
unsigned long tegra_bootloader_fb_size;
unsigned long tegra_fb_start;
unsigned long tegra_fb_size;
unsigned long tegra_fb2_start;
unsigned long tegra_fb2_size;
unsigned long tegra_carveout_start;
unsigned long tegra_carveout_size;
unsigned long tegra_vpr_start;
unsigned long tegra_vpr_size;
unsigned long tegra_lp0_vec_start;
unsigned long tegra_lp0_vec_size;
bool tegra_lp0_vec_relocate;
unsigned long tegra_grhost_aperture = ~0ul;
static   bool is_tegra_debug_uart_hsport;
static struct board_info pmu_board_info;
static struct board_info display_board_info;
static struct board_info camera_board_info;

#ifdef CONFIG_GPU_OVERCLOCK
static int pmu_core_edp = 1200;	/* default 1.2V EDP limit */
#else
static int pmu_core_edp = 1200;	/* default 1.2V EDP limit */
#endif

static int board_panel_type;
static enum power_supply_type pow_supply_type = POWER_SUPPLY_TYPE_MAINS;

void (*arch_reset)(char mode, const char *cmd) = tegra_assert_system_reset;

#define NEVER_RESET 0

//            
static int bootmode = 1;      
int is_tegra_bootmode(void);
//            

void tegra_assert_system_reset(char mode, const char *cmd)
{
#if defined(CONFIG_TEGRA_FPGA_PLATFORM) || NEVER_RESET
	pr_info("tegra_assert_system_reset() ignored.....");
	do { } while (1);
#else
	void __iomem *reset = IO_ADDRESS(TEGRA_PMC_BASE + 0x00);
	u32 reg;

	reg = readl_relaxed(reset + PMC_SCRATCH0);
	/* Writing recovery kernel or Bootloader mode in SCRATCH0 31:30:1 */
	if (cmd) {
		if (!strcmp(cmd, "recovery"))
			reg |= RECOVERY_MODE;
		else if (!strcmp(cmd, "bootloader"))
			reg |= BOOTLOADER_MODE;
		else if (!strcmp(cmd, "forced-recovery"))
			reg |= FORCED_RECOVERY_MODE;
		else
			reg &= ~(BOOTLOADER_MODE | RECOVERY_MODE | FORCED_RECOVERY_MODE);
	}
	else {
		/* Clearing SCRATCH0 31:30:1 on default reboot */
		reg &= ~(BOOTLOADER_MODE | RECOVERY_MODE | FORCED_RECOVERY_MODE);
	}
	writel_relaxed(reg, reset + PMC_SCRATCH0);
	/* use *_related to avoid spinlock since caches are off */
	reg = readl_relaxed(reset);
	reg |= 0x10;
	writel_relaxed(reg, reset);
#endif
}
static int modem_id;
static int commchip_id;
static int sku_override;
static int debug_uart_port_id;
static enum audio_codec_type audio_codec_name;
static enum image_type board_image_type = system_image;
static int max_cpu_current;
static int bootbatteryexist = 1;
static int bootbatteryVerified = 1;

/* WARNING: There is implicit client of pllp_out3 like i2c, uart, dsi
 * and so this clock (pllp_out3) should never be disabled.
 */
static __initdata struct tegra_clk_init_table common_clk_init_table[] = {
	/* name		parent		rate		enabled */
	{ "clk_m",	NULL,		0,		true },
	{ "emc",	NULL,		0,		true },
	{ "cpu",	NULL,		0,		true },
	{ "kfuse",	NULL,		0,		true },
	{ "fuse",	NULL,		0,		true },
	{ "sclk",	NULL,		0,		true },
#ifdef CONFIG_TEGRA_SILICON_PLATFORM
#ifdef CONFIG_ARCH_TEGRA_2x_SOC
	{ "pll_p",	NULL,		216000000,	true },
	{ "pll_p_out1",	"pll_p",	28800000,	true },
	{ "pll_p_out2",	"pll_p",	48000000,	false },
	{ "pll_p_out3",	"pll_p",	72000000,	true },
	{ "pll_p_out4",	"pll_p",	108000000,	false },
	{ "pll_m",	"clk_m",	0,		true },
	{ "pll_m_out1",	"pll_m",	120000000,	true },
	{ "sclk",	"pll_c_out1",	40000000,	true },
	{ "hclk",	"sclk",		40000000,	true },
	{ "pclk",	"hclk",		40000000,	true },
	{ "mpe",	"pll_c",	0,		false },
	{ "epp",	"pll_c",	0,		false },
	{ "vi_sensor",	"pll_c",	0,		false },
	{ "vi",		"pll_c",	0,		false },
	{ "2d",		"pll_c",	0,		false },
	{ "3d",		"pll_c",	0,		false },
#else //2x_SOC
//                    
//#ifdef CONFIG_MACH_X3
#if 0
	{ "pll_p",	NULL,		408000000,	true },
	{ "pll_p_out1",	"pll_p",	9600000,	true },
	{ "pll_p_out2",	"pll_p",	48000000,	true },
	{ "pll_p_out3",	"pll_p",	102000000,	true },
#else
	{ "pll_p",	NULL,		0,		true },
	{ "pll_p_out1",	"pll_p",	0,		false },
	{ "pll_p_out2",	"pll_p",	48000000,	false },
	{ "pll_p_out3",	"pll_p",	0,		false },
#endif //mach
	{ "pll_m_out1",	"pll_m",	275000000,	false },
	{ "pll_p_out4",	"pll_p",	102000000,	true },
	{ "sclk",	"pll_p_out4",	102000000,	true },
	{ "hclk",	"sclk",		102000000,	true },
	{ "pclk",	"hclk",		51000000,	true },
#endif //2x_SOC
#else //SILICON_PLATFORM
	{ "pll_p",	NULL,		216000000,	true },
	{ "pll_p_out1",	"pll_p",	28800000,	false },
	{ "pll_p_out2",	"pll_p",	48000000,	false },
	{ "pll_p_out3",	"pll_p",	72000000,	true },
	{ "pll_m_out1",	"pll_m",	275000000,	true },
	{ "pll_p_out4",	"pll_p",	108000000,	false },
	{ "sclk",	"pll_p_out4",	108000000,	true },
	{ "hclk",	"sclk",		108000000,	true },
	{ "pclk",	"hclk",		54000000,	true },
#endif //SILICON_PLATFORM
#ifdef CONFIG_TEGRA_SLOW_CSITE
	{ "csite",	"clk_m",	1000000, 	true },
#else
//                    
#ifdef CONFIG_MACH_X3
	{ "csite",	NULL,		4250000,	true },
#else
	{ "csite",      NULL,           0,              true },
#endif /* CONFIG_MACH_X3 */
#endif
	{ "pll_u",	NULL,		480000000,	false },
	{ "sdmmc1",	"pll_p",	48000000,	false},
	{ "sdmmc3",	"pll_p",	48000000,	false},
	{ "sdmmc4",	"pll_p",	48000000,	false},
#if defined(CONFIG_MACH_LGE)	
	{ "vi_sensor", "pll_p", 0, false }, //                                                   
#endif	
	{ "sbc1.sclk",  NULL,           40000000,       false},
	{ "sbc2.sclk",  NULL,           40000000,       false},
	{ "sbc3.sclk",  NULL,           40000000,       false},
	{ "sbc4.sclk",  NULL,           40000000,       false},
#ifndef CONFIG_ARCH_TEGRA_2x_SOC
	{ "sbc5.sclk",  NULL,           40000000,       false},
	{ "sbc6.sclk",  NULL,           40000000,       false},
	{ "wake.sclk",	NULL,		40000000,	true },
	{ "cpu_mode.sclk", NULL,	80000000,	false },
	{ "cbus",	"pll_c",	416000000,	false },
	{ "pll_c_out1", "pll_c",        208000000,      false },
	{ "mselect",	"pll_p",	102000000,	true },
#endif
	{ NULL,		NULL,		0,		0},
};

#ifdef CONFIG_TRUSTED_FOUNDATIONS
static inline void tegra_l2x0_disable_tz(void)
{
	static u32 l2x0_way_mask;
	BUG_ON(smp_processor_id() != 0);

	if (!l2x0_way_mask) {
		void __iomem *p = IO_ADDRESS(TEGRA_ARM_PERIF_BASE) + 0x3000;
		u32 aux_ctrl;
		u32 ways;

		aux_ctrl = readl_relaxed(p + L2X0_AUX_CTRL);
		ways = (aux_ctrl & (1 << 16)) ? 16 : 8;
		l2x0_way_mask = (1 << ways) - 1;
	}
#ifdef CONFIG_ARCH_TEGRA_2x_SOC
	/* flush all ways on any disable */
	tegra_generic_smc_uncached(0xFFFFF100, 0x00000002, l2x0_way_mask);
#elif defined(CONFIG_ARCH_TEGRA_3x_SOC)
	if (tegra_is_cpu_in_lp2(0) == false) {
		/*
		 * If entering LP0/LP1, ask secureos to fully flush and
		 * disable the L2.
		 *
		 * If entering LP2, L2 disable is handled by the secureos
		 * as part of the tegra_sleep_cpu() SMC. This SMC indicates
		 * no more secureos tasks will be scheduled, allowing it
		 * to optimize out L2 flushes on its side.
		 */
		tegra_generic_smc_uncached(0xFFFFF100,
						0x00000002, l2x0_way_mask);
	}
#endif
}

static inline void tegra_init_cache_tz(bool init)
{
	void __iomem *p = IO_ADDRESS(TEGRA_ARM_PERIF_BASE) + 0x3000;
	u32 aux_ctrl;

	BUG_ON(smp_processor_id() != 0);

	if (init) {
		/* init L2 from secureos */
		tegra_generic_smc(0xFFFFF100, 0x00000001, 0x0);

		/* common init called for outer call hookup */
		aux_ctrl = readl_relaxed(p + L2X0_AUX_CTRL);
		l2x0_init(p, aux_ctrl, 0xFFFFFFFF);

		/* use our outer_disable() routine */
		outer_cache.disable = tegra_l2x0_disable_tz;
	} else {
		/* reenable L2 in secureos */
		aux_ctrl = readl_relaxed(p + L2X0_AUX_CTRL);
		tegra_generic_smc_uncached(0xFFFFF100, 0x00000004, aux_ctrl);
	}
}
#endif	/* CONFIG_TRUSTED_FOUNDATIONS  */

#ifdef CONFIG_CACHE_L2X0
/*
 * We define our own outer_disable() to avoid L2 flush upon LP2 entry.
 * Since the Tegra kernel will always be in single core mode when
 * L2 is being disabled, we can omit the locking. Since we are not
 * accessing the spinlock we also avoid the problem of the spinlock
 * storage getting out of sync.
 */
static inline void tegra_l2x0_disable(void)
{
	void __iomem *p = IO_ADDRESS(TEGRA_ARM_PERIF_BASE) + 0x3000;
	writel_relaxed(0, p + L2X0_CTRL);
	dsb();
}

void tegra_init_cache(bool init)
{
#ifdef CONFIG_TRUSTED_FOUNDATIONS
	/* enable/re-enable of L2 handled by secureos */
	return tegra_init_cache_tz(init);
#else
	void __iomem *p = IO_ADDRESS(TEGRA_ARM_PERIF_BASE) + 0x3000;
	u32 aux_ctrl;

#if defined(CONFIG_ARCH_TEGRA_2x_SOC)
	writel_relaxed(0x331, p + L2X0_TAG_LATENCY_CTRL);
	writel_relaxed(0x441, p + L2X0_DATA_LATENCY_CTRL);

#elif defined(CONFIG_ARCH_TEGRA_3x_SOC)
#ifdef CONFIG_TEGRA_SILICON_PLATFORM
	/* PL310 RAM latency is CPU dependent. NOTE: Changes here
	   must also be reflected in __cortex_a9_l2x0_restart */

	if (is_lp_cluster()) {
		writel(0x221, p + L2X0_TAG_LATENCY_CTRL);
		writel(0x221, p + L2X0_DATA_LATENCY_CTRL);
	} else {
		u32 speedo;

		/* relax l2-cache latency for speedos 4,5,6 (T33's chips) */
		speedo = tegra_cpu_speedo_id();
		if (speedo == 4 || speedo == 5 || speedo == 6 ||
		    speedo == 12 || speedo == 13) {
			writel(0x442, p + L2X0_TAG_LATENCY_CTRL);
			writel(0x552, p + L2X0_DATA_LATENCY_CTRL);
		} else {
			writel(0x441, p + L2X0_TAG_LATENCY_CTRL);
			writel(0x551, p + L2X0_DATA_LATENCY_CTRL);
		}
	}
#else
	writel(0x770, p + L2X0_TAG_LATENCY_CTRL);
	writel(0x770, p + L2X0_DATA_LATENCY_CTRL);
#endif
#endif
	writel(0x3, p + L2X0_POWER_CTRL);
	aux_ctrl = readl(p + L2X0_CACHE_TYPE);
	aux_ctrl = (aux_ctrl & 0x700) << (17-8);
	aux_ctrl |= 0x7C000001;
	if (init) {
		l2x0_init(p, aux_ctrl, 0x8200c3fe);
		/* use our outer_disable() routine to avoid flush */
		outer_cache.disable = tegra_l2x0_disable;
	} else {
		u32 tmp;

		tmp = aux_ctrl;
		aux_ctrl = readl(p + L2X0_AUX_CTRL);
		aux_ctrl &= 0x8200c3fe;
		aux_ctrl |= tmp;
		writel(aux_ctrl, p + L2X0_AUX_CTRL);
	}
	l2x0_enable();
#endif
}
#endif /* CONFIG_CACHE_L2X0 */

static void __init tegra_init_power(void)
{
#ifdef CONFIG_ARCH_TEGRA_HAS_SATA
	tegra_powergate_partition_with_clk_off(TEGRA_POWERGATE_SATA);
#endif
#ifdef CONFIG_ARCH_TEGRA_HAS_PCIE
	tegra_powergate_partition_with_clk_off(TEGRA_POWERGATE_PCIE);
#endif
}

static inline unsigned long gizmo_readl(unsigned long offset)
{
	return readl(IO_TO_VIRT(TEGRA_AHB_GIZMO_BASE + offset));
}

static inline void gizmo_writel(unsigned long value, unsigned long offset)
{
	writel(value, IO_TO_VIRT(TEGRA_AHB_GIZMO_BASE + offset));
}

static void __init tegra_init_ahb_gizmo_settings(void)
{
	unsigned long val;

	val = gizmo_readl(AHB_GIZMO_AHB_MEM);
	val |= ENB_FAST_REARBITRATE | IMMEDIATE | DONT_SPLIT_AHB_WR;
	gizmo_writel(val, AHB_GIZMO_AHB_MEM);

	val = gizmo_readl(AHB_GIZMO_USB);
	val |= IMMEDIATE;
	gizmo_writel(val, AHB_GIZMO_USB);

	val = gizmo_readl(AHB_GIZMO_USB2);
	val |= IMMEDIATE;
	gizmo_writel(val, AHB_GIZMO_USB2);

	val = gizmo_readl(AHB_GIZMO_USB3);
	val |= IMMEDIATE;
	gizmo_writel(val, AHB_GIZMO_USB3);

	val = gizmo_readl(AHB_ARBITRATION_PRIORITY_CTRL);
	val |= PRIORITY_SELECT_USB | PRIORITY_SELECT_USB2 | PRIORITY_SELECT_USB3
				| AHB_PRIORITY_WEIGHT(7);
	gizmo_writel(val, AHB_ARBITRATION_PRIORITY_CTRL);

	val = gizmo_readl(AHB_MEM_PREFETCH_CFG1);
	val &= ~MST_ID(~0);
	val |= PREFETCH_ENB | AHBDMA_MST_ID | ADDR_BNDRY(0xc) | INACTIVITY_TIMEOUT(0x1000);
	gizmo_writel(val, AHB_MEM_PREFETCH_CFG1);

	val = gizmo_readl(AHB_MEM_PREFETCH_CFG2);
	val &= ~MST_ID(~0);
	val |= PREFETCH_ENB | USB_MST_ID | ADDR_BNDRY(0xc) | INACTIVITY_TIMEOUT(0x1000);
	gizmo_writel(val, AHB_MEM_PREFETCH_CFG2);

	val = gizmo_readl(AHB_MEM_PREFETCH_CFG3);
	val &= ~MST_ID(~0);
	val |= PREFETCH_ENB | USB3_MST_ID | ADDR_BNDRY(0xc) | INACTIVITY_TIMEOUT(0x1000);
	gizmo_writel(val, AHB_MEM_PREFETCH_CFG3);

	val = gizmo_readl(AHB_MEM_PREFETCH_CFG4);
	val &= ~MST_ID(~0);
	val |= PREFETCH_ENB | USB2_MST_ID | ADDR_BNDRY(0xc) | INACTIVITY_TIMEOUT(0x1000);
	gizmo_writel(val, AHB_MEM_PREFETCH_CFG4);
}

/*
static bool console_flushed;


static void tegra_pm_flush_console(void)
{
	if (console_flushed)
		return;
	console_flushed = true;

	pr_emerg("Restarting %s\n", linux_banner);
	if (console_trylock()) {
		console_unlock();
		return;
	}

	mdelay(50);

	local_irq_disable();
	if (!console_trylock())
		pr_emerg("%s: Console was locked! Busting\n", __func__);
	else
		pr_emerg("%s: Console was locked!\n", __func__);
	console_unlock();
}
*/

/*
                                                        
 
                          
                                                                                          
                                                   
 

                                
                        
  
                             
  
                                 
     
                                
      
                                                                                          

 
*/

void __init tegra_init_early(void)
{
#ifndef CONFIG_SMP
	/* For SMP system, initializing the reset handler here is too
	   late. For non-SMP systems, the function that calls the reset
	   handler initializer is not called, so do it here for non-SMP. */
	tegra_cpu_reset_handler_init();
#endif
	tegra_init_fuse();
	tegra_gpio_resume_init();
	tegra_init_clock();
	tegra_init_pinmux();
	tegra_clk_init_from_table(common_clk_init_table);
	tegra_init_power();
	tegra_init_cache(true);
	tegra_init_ahb_gizmo_settings();
	tegra_init_debug_uart_rate();
}

static int __init tegra_lp0_vec_arg(char *options)
{
	char *p = options;

	tegra_lp0_vec_size = memparse(p, &p);
	if (*p == '@')
		tegra_lp0_vec_start = memparse(p+1, &p);
	if (!tegra_lp0_vec_size || !tegra_lp0_vec_start) {
		tegra_lp0_vec_size = 0;
		tegra_lp0_vec_start = 0;
	}

	return 0;
}
early_param("lp0_vec", tegra_lp0_vec_arg);

static int __init tegra_bootloader_fb_arg(char *options)
{
	char *p = options;

	tegra_bootloader_fb_size = memparse(p, &p);
	if (*p == '@')
		tegra_bootloader_fb_start = memparse(p+1, &p);

	pr_info("Found tegra_fbmem: %08lx@%08lx\n",
		tegra_bootloader_fb_size, tegra_bootloader_fb_start);

	return 0;
}
early_param("tegra_fbmem", tegra_bootloader_fb_arg);

static int __init tegra_sku_override(char *id)
{
	char *p = id;

	sku_override = memparse(p, &p);

	return 0;
}
early_param("sku_override", tegra_sku_override);

int tegra_get_sku_override(void)
{
	return sku_override;
}

static int __init tegra_vpr_arg(char *options)
{
	char *p = options;

	tegra_vpr_size = memparse(p, &p);
	if (*p == '@')
		tegra_vpr_start = memparse(p+1, &p);
	pr_info("Found vpr, start=0x%lx size=%lx",
		tegra_vpr_start, tegra_vpr_size);
	return 0;
}
early_param("vpr", tegra_vpr_arg);

enum panel_type get_panel_type(void)
{
	return board_panel_type;
}
static int __init tegra_board_panel_type(char *options)
{
	if (!strcmp(options, "lvds"))
		board_panel_type = panel_type_lvds;
	else if (!strcmp(options, "dsi"))
		board_panel_type = panel_type_dsi;
	else
		return 0;
	return 1;
}
__setup("panel=", tegra_board_panel_type);

enum power_supply_type get_power_supply_type(void)
{
	return pow_supply_type;
}
static int __init tegra_board_power_supply_type(char *options)
{
	if (!strcmp(options, "Adapter"))
		pow_supply_type = POWER_SUPPLY_TYPE_MAINS;
	if (!strcmp(options, "Mains"))
		pow_supply_type = POWER_SUPPLY_TYPE_MAINS;
	else if (!strcmp(options, "Battery"))
		pow_supply_type = POWER_SUPPLY_TYPE_BATTERY;
	else
		return 0;
	return 1;
}
__setup("power_supply=", tegra_board_power_supply_type);

int get_core_edp(void)
{
	return pmu_core_edp;
}
static int __init tegra_pmu_core_edp(char *options)
{
	char *p = options;
	int core_edp = memparse(p, &p);
	if (core_edp != 0)
		pmu_core_edp = core_edp;
	return 0;
}
early_param("core_edp_mv", tegra_pmu_core_edp);

int get_maximum_cpu_current_supported(void)
{
	return max_cpu_current;
}
static int __init tegra_max_cpu_current(char *options)
{
	char *p = options;
	max_cpu_current = memparse(p, &p);
	return 1;
}
__setup("max_cpu_cur_ma=", tegra_max_cpu_current);

static int __init tegra_debug_uartport(char *info)
{
	char *p = info;
	unsigned long long port_id;
	if (!strncmp(p, "hsport", 6))
		is_tegra_debug_uart_hsport = true;
	else if (!strncmp(p, "lsport", 6))
		is_tegra_debug_uart_hsport = false;

	if (p[6] == ',') {
		if (p[7] == '-') {
			debug_uart_port_id = -1;
		} else {
			port_id = memparse(p + 7, &p);
			debug_uart_port_id = (int) port_id;
		}
	} else {
		debug_uart_port_id = -1;
	}

	return 1;
}

bool is_tegra_debug_uartport_hs(void)
{
	return is_tegra_debug_uart_hsport;
}

int get_tegra_uart_debug_port_id(void)
{
	return debug_uart_port_id;
}
__setup("debug_uartport=", tegra_debug_uartport);

//            
#if defined(CONFIG_MACH_LGE)
static int __init tegra_bootmode(char *info)
{
	char *p = info;

	if (!strncmp(p, "normal", 6))
	{
		bootmode = 1;
	}
	else if (!strncmp(p, "charger", 7))
	{
		bootmode = 0;
	}

	return 1;
}

int is_tegra_bootmode(void)
{
	printk("%s [%d] \n", __func__, bootmode);
	return bootmode;
}


__setup("androidboot.mode=", tegra_bootmode);

static int __init tegra_batteryexist(char *options)
{
	char *p = options;
	bootbatteryexist = memparse(p, &p);

	printk("%s [%d]\n", __func__, bootbatteryexist);
	return 1;
}

int is_tegra_batteryexistWhenBoot(void)
{
	//printk("%s [%d]\n", __func__, bootbatteryexist);
	return bootbatteryexist;
}


__setup("batteryexist=", tegra_batteryexist);

static int __init tegra_batteryVerified(char *options)
{
	char *p = options;
	bootbatteryVerified = memparse(p, &p);

	printk("%s [%d]\n", __func__, bootbatteryVerified);
	return 1;
}


__setup("batteryVerified=", tegra_batteryVerified);

int is_tegra_batteryVerified(void)
{
	printk("%s [%d]\n", __func__, bootbatteryVerified);
	return bootbatteryVerified;
}
#endif
//            
static int __init tegra_image_type(char *options)
{
	if (!strcmp(options, "RCK"))
		board_image_type = rck_image;

	return 0;
}

enum image_type get_tegra_image_type(void)
{
	return board_image_type;
}

__setup("image=", tegra_image_type);

static int __init tegra_audio_codec_type(char *info)
{
	char *p = info;
	if (!strncmp(p, "wm8903", 6))
		audio_codec_name = audio_codec_wm8903;
	else
		audio_codec_name = audio_codec_none;

	return 1;
}

enum audio_codec_type get_audio_codec_type(void)
{
	return audio_codec_name;
}
__setup("audio_codec=", tegra_audio_codec_type);


void tegra_get_board_info(struct board_info *bi)
{
#ifdef CONFIG_OF
	struct device_node *board_info;
	u32 prop_val;
	int err;

	board_info = of_find_node_by_path("/chosen/board_info");
	if (!IS_ERR_OR_NULL(board_info)) {
		memset(bi, 0, sizeof(*bi));

		err = of_property_read_u32(board_info, "id", &prop_val);
		if (err)
			pr_err("failed to read /chosen/board_info/id\n");
		else
			bi->board_id = prop_val;

		err = of_property_read_u32(board_info, "sku", &prop_val);
		if (err)
			pr_err("failed to read /chosen/board_info/sku\n");
		else
			bi->sku = prop_val;

		err = of_property_read_u32(board_info, "fab", &prop_val);
		if (err)
			pr_err("failed to read /chosen/board_info/fab\n");
		else
			bi->fab = prop_val;

		err = of_property_read_u32(board_info, "major_revision", &prop_val);
		if (err)
			pr_err("failed to read /chosen/board_info/major_revision\n");
		else
			bi->major_revision = prop_val;

		err = of_property_read_u32(board_info, "minor_revision", &prop_val);
		if (err)
			pr_err("failed to read /chosen/board_info/minor_revision\n");
		else
			bi->minor_revision = prop_val;
	} else {
#endif
		bi->board_id = (system_serial_high >> 16) & 0xFFFF;
		bi->sku = (system_serial_high) & 0xFFFF;
		bi->fab = (system_serial_low >> 24) & 0xFF;
		bi->major_revision = (system_serial_low >> 16) & 0xFF;
		bi->minor_revision = (system_serial_low >> 8) & 0xFF;
#ifdef CONFIG_OF
	}
#endif
}

static int __init tegra_pmu_board_info(char *info)
{
	char *p = info;
	pmu_board_info.board_id = memparse(p, &p);
	pmu_board_info.sku = memparse(p+1, &p);
	pmu_board_info.fab = memparse(p+1, &p);
	pmu_board_info.major_revision = memparse(p+1, &p);
	pmu_board_info.minor_revision = memparse(p+1, &p);
	return 1;
}

void tegra_get_pmu_board_info(struct board_info *bi)
{
	memcpy(bi, &pmu_board_info, sizeof(struct board_info));
}

__setup("pmuboard=", tegra_pmu_board_info);

static int __init tegra_display_board_info(char *info)
{
	char *p = info;
	display_board_info.board_id = memparse(p, &p);
	display_board_info.sku = memparse(p+1, &p);
	display_board_info.fab = memparse(p+1, &p);
	display_board_info.major_revision = memparse(p+1, &p);
	display_board_info.minor_revision = memparse(p+1, &p);
	return 1;
}

void tegra_get_display_board_info(struct board_info *bi)
{
	memcpy(bi, &display_board_info, sizeof(struct board_info));
}

__setup("displayboard=", tegra_display_board_info);

static int __init tegra_camera_board_info(char *info)
{
	char *p = info;
	camera_board_info.board_id = memparse(p, &p);
	camera_board_info.sku = memparse(p+1, &p);
	camera_board_info.fab = memparse(p+1, &p);
	camera_board_info.major_revision = memparse(p+1, &p);
	camera_board_info.minor_revision = memparse(p+1, &p);
	return 1;
}

void tegra_get_camera_board_info(struct board_info *bi)
{
	memcpy(bi, &camera_board_info, sizeof(struct board_info));
}

__setup("cameraboard=", tegra_camera_board_info);

static int __init tegra_modem_id(char *id)
{
	char *p = id;

	modem_id = memparse(p, &p);
	return 1;
}

int tegra_get_modem_id(void)
{
	return modem_id;
}

__setup("modem_id=", tegra_modem_id);

static int __init tegra_commchip_id(char *id)
{
	char *p = id;

	if (get_option(&p, &commchip_id) != 1)
		return 0;
	return 1;
}

int tegra_get_commchip_id(void)
{
	return commchip_id;
}

__setup("commchip_id=", tegra_commchip_id);

/*
 * Tegra has a protected aperture that prevents access by most non-CPU
 * memory masters to addresses above the aperture value.  Enabling it
 * secures the CPU's memory from the GPU, except through the GART.
 */
void __init tegra_protected_aperture_init(unsigned long aperture)
{
#ifndef CONFIG_NVMAP_ALLOW_SYSMEM
	void __iomem *mc_base = IO_ADDRESS(TEGRA_MC_BASE);
	pr_info("Enabling Tegra protected aperture at 0x%08lx\n", aperture);
	writel(aperture, mc_base + MC_SECURITY_CFG2);
#else
	pr_err("Tegra protected aperture disabled because nvmap is using "
		"system memory\n");
#endif
}

static void __tegra_move_framebuffer_kmap(phys_addr_t to, phys_addr_t from,
	size_t size)
{
	size_t i;

	BUG_ON(!pfn_valid(page_to_pfn(phys_to_page(from))));

	for (i = 0; i < size; i += PAGE_SIZE) {
		struct page *from_page = phys_to_page(from + i);
		void *from_virt = kmap(from_page);
		struct page *to_page = phys_to_page(to + i);
		void *to_virt = kmap(to_page);

		memcpy(to_virt, from_virt, PAGE_SIZE);
		kunmap(from_virt);
		kunmap(to_virt);
	}
}

static void __tegra_move_framebuffer_ioremap(struct platform_device *pdev,
	unsigned long to, unsigned long from,
	unsigned long size)
{
	struct page *page;
	void __iomem *to_io;
	void *from_virt;
	unsigned long i, addr[] = { to, from, };

	to_io = ioremap_wc(to, size);
	if (!to_io) {
		pr_err("%s: Failed to map target framebuffer\n", __func__);
		return;
	}

	if (pfn_valid(page_to_pfn(phys_to_page(from)))) {
		for (i = 0 ; i < size; i += PAGE_SIZE) {
			page = phys_to_page(from + i);
			from_virt = kmap(page);
			memcpy(to_io + i, from_virt, PAGE_SIZE);
			kunmap(page);
		}
	} else {
		void __iomem *from_io = ioremap_wc(from, size);
		if (!from_io) {
			pr_err("%s: Failed to map source framebuffer\n",
				__func__);
			goto out;
		}

		for (i = 0; i < size; i += 4)
			writel_relaxed(readl_relaxed(from_io + i), to_io + i);
		dmb();

		iounmap(from_io);
	}
//
	for (i = 0; i < ARRAY_SIZE(addr); i++)
		dma_map_linear_at(NULL, addr[i], size, DMA_TO_DEVICE);
out:
	iounmap(to_io);
}

/*
 * Due to conflicting restrictions on the placement of the framebuffer,
 * the bootloader is likely to leave the framebuffer pointed at a location
 * in memory that is outside the grhost aperture.  This function will move
 * the framebuffer contents from a physical address that is anywhere (lowmem,
 * highmem, or outside the memory map) to a physical address that is outside
 * the memory map.
 */
void __tegra_move_framebuffer(struct platform_device *pdev,
	phys_addr_t to, phys_addr_t from,
	size_t size)
{
	BUG_ON(PAGE_ALIGN((unsigned long)to) != (unsigned long)to);
	BUG_ON(PAGE_ALIGN(from) != from);
	BUG_ON(PAGE_ALIGN(size) != size);

	if (!from)
		return;

	if (pfn_valid(page_to_pfn(phys_to_page(to))))
		__tegra_move_framebuffer_kmap(to, from, size);
	else
		__tegra_move_framebuffer_ioremap(pdev, to, from, size);
}

void __tegra_clear_framebuffer(struct platform_device *pdev,
				unsigned long to, unsigned long size)
{
	void __iomem *to_io;
	unsigned long i;

	BUG_ON(PAGE_ALIGN((unsigned long)to) != (unsigned long)to);
	BUG_ON(PAGE_ALIGN(size) != size);

	to_io = ioremap_wc(to, size);
	if (!to_io) {
		pr_err("%s: Failed to map target framebuffer\n", __func__);
		return;
	}

	if (pfn_valid(page_to_pfn(phys_to_page(to)))) {
		for (i = 0 ; i < size; i += PAGE_SIZE)
			memset(to_io + i, 0, PAGE_SIZE);
	} else {
		for (i = 0; i < size; i += 4)
			writel_relaxed(0, to_io + i);
		dmb();
	}
//TODO add support for dma_map_linear (include/asm-generic/dma-mapping-common.h)
//	if (!pdev)
//		goto out;
//
//	dma_map_linear(&pdev->dev, to, size, DMA_TO_DEVICE);
//out:
	iounmap(to_io);
}

void __init tegra_reserve(unsigned long carveout_size, unsigned long fb_size,
	unsigned long fb2_size)
{
	if (carveout_size) {
		tegra_carveout_start = memblock_end_of_DRAM() - carveout_size;
		if (memblock_remove(tegra_carveout_start, carveout_size)) {
			pr_err("Failed to remove carveout %08lx@%08lx "
				"from memory map\n",
				carveout_size, tegra_carveout_start);
			tegra_carveout_start = 0;
			tegra_carveout_size = 0;
		} else
			tegra_carveout_size = carveout_size;
	}

	if (fb2_size) {
		tegra_fb2_start = memblock_end_of_DRAM() - fb2_size;
		if (memblock_remove(tegra_fb2_start, fb2_size)) {
			pr_err("Failed to remove second framebuffer "
				"%08lx@%08lx from memory map\n",
				fb2_size, tegra_fb2_start);
			tegra_fb2_start = 0;
			tegra_fb2_size = 0;
		} else
			tegra_fb2_size = fb2_size;
	}

	if (fb_size) {
		tegra_fb_start = memblock_end_of_DRAM() - fb_size;
		if (memblock_remove(tegra_fb_start, fb_size)) {
			pr_err("Failed to remove framebuffer %08lx@%08lx "
				"from memory map\n",
				fb_size, tegra_fb_start);
			tegra_fb_start = 0;
			tegra_fb_size = 0;
		} else
			tegra_fb_size = fb_size;
	}

	if (tegra_fb_size)
		tegra_grhost_aperture = tegra_fb_start;

	if (tegra_fb2_size && tegra_fb2_start < tegra_grhost_aperture)
		tegra_grhost_aperture = tegra_fb2_start;

	if (tegra_carveout_size && tegra_carveout_start < tegra_grhost_aperture)
		tegra_grhost_aperture = tegra_carveout_start;

	if (tegra_lp0_vec_size &&
	   (tegra_lp0_vec_start < memblock_end_of_DRAM())) {
		if (memblock_reserve(tegra_lp0_vec_start, tegra_lp0_vec_size)) {
			pr_err("Failed to reserve lp0_vec %08lx@%08lx\n",
				tegra_lp0_vec_size, tegra_lp0_vec_start);
			tegra_lp0_vec_start = 0;
			tegra_lp0_vec_size = 0;
		}
		tegra_lp0_vec_relocate = false;
	} else
		tegra_lp0_vec_relocate = true;

	/*
	 * We copy the bootloader's framebuffer to the framebuffer allocated
	 * above, and then free this one.
	 * */
	if (tegra_bootloader_fb_size) {
		tegra_bootloader_fb_size = PAGE_ALIGN(tegra_bootloader_fb_size);
		if (memblock_reserve(tegra_bootloader_fb_start,
				tegra_bootloader_fb_size)) {
			pr_err("Failed to reserve bootloader frame buffer "
				"%08lx@%08lx\n", tegra_bootloader_fb_size,
				tegra_bootloader_fb_start);
			tegra_bootloader_fb_start = 0;
			tegra_bootloader_fb_size = 0;
		}
	}

	pr_info("Tegra reserved memory:\n"
		"LP0:                    %08lx - %08lx\n"
		"Bootloader framebuffer: %08lx - %08lx\n"
		"Framebuffer:            %08lx - %08lx\n"
		"2nd Framebuffer:        %08lx - %08lx\n"
		"Carveout:               %08lx - %08lx\n"
		"Vpr:                    %08lx - %08lx\n",
		tegra_lp0_vec_start,
		tegra_lp0_vec_size ?
			tegra_lp0_vec_start + tegra_lp0_vec_size - 1 : 0,
		tegra_bootloader_fb_start,
		tegra_bootloader_fb_size ?
			tegra_bootloader_fb_start + tegra_bootloader_fb_size - 1 : 0,
		tegra_fb_start,
		tegra_fb_size ?
			tegra_fb_start + tegra_fb_size - 1 : 0,
		tegra_fb2_start,
		tegra_fb2_size ?
			tegra_fb2_start + tegra_fb2_size - 1 : 0,
		tegra_carveout_start,
		tegra_carveout_size ?
			tegra_carveout_start + tegra_carveout_size - 1 : 0,
		tegra_vpr_start,
		tegra_vpr_size ?
			tegra_vpr_start + tegra_vpr_size - 1 : 0);
}

static struct resource ram_console_resources[] = {
	{
		.flags = IORESOURCE_MEM,
	},
};

static struct platform_device ram_console_device = {
	.name 		= "ram_console",
	.id 		= -1,
	.num_resources	= ARRAY_SIZE(ram_console_resources),
	.resource	= ram_console_resources,
};

void __init tegra_ram_console_debug_reserve(phys_addr_t ram_console_start,
					    unsigned long ram_console_size)
{
	struct resource *res;
	long ret;
	unsigned long real_start, real_size;

	res = platform_get_resource(&ram_console_device, IORESOURCE_MEM, 0);
	if (!res)
		goto fail;

	if (ram_console_start == USE_DEFAULT_START_ADDR)
		ram_console_start = memblock_end_of_DRAM() - ram_console_size;
	res->start = ram_console_start;
	res->end = res->start + ram_console_size - 1;

	// Register an extra 1M before ramconsole to store kexec stuff
	real_start = res->start - SZ_1M;
	real_size = ram_console_size + SZ_1M;

	ret = memblock_remove(real_start, real_size);
	if (ret)
		goto fail;
//            
#ifdef CONFIG_REBOOT_MONITOR
	res->end = res->start + ram_console_size/2 -1;
 #endif
//            
	return;

fail:
	ram_console_device.resource = NULL;
	ram_console_device.num_resources = 0;
	pr_err("Failed to reserve memory block for ram console\n");
}

void __init tegra_ram_console_debug_init(void)
{
	int err;

	err = platform_device_register(&ram_console_device);
	if (err)
		pr_err("%s: ram console registration failed (%d)!\n",
			__func__, err);
}

static int __init tegra_release_bootloader_fb(void)
{
	/* Since bootloader fb is reserved in common.c, it is freed here. */
	if (tegra_bootloader_fb_size) {
		if (memblock_free(tegra_bootloader_fb_start,
						tegra_bootloader_fb_size))
			pr_err("Failed to free bootloader fb.\n");
		else
			free_bootmem_late(tegra_bootloader_fb_start,
						tegra_bootloader_fb_size);
	}
	return 0;
}
late_initcall(tegra_release_bootloader_fb);

#ifdef CONFIG_TEGRA_CONVSERVATIVE_GOV_ON_EARLYSUPSEND
char cpufreq_default_gov[CONFIG_NR_CPUS][MAX_GOV_NAME_LEN];
char *cpufreq_conservative_gov = "conservative";
/*
static char *cpufreq_sysfs_place_holder =
		"/sys/devices/system/cpu/cpu%i/cpufreq/scaling_governor";
static char *cpufreq_gov_conservative_param =
		"/sys/devices/system/cpu/cpufreq/conservative/%s";
*/
void cpufreq_store_default_gov(void)
{
	unsigned int cpu = 0;
	struct cpufreq_policy *policy;

#ifndef CONFIG_TEGRA_AUTO_HOTPLUG
	for_each_online_cpu(cpu)
#endif
	{
		policy = cpufreq_cpu_get(cpu);
		if (policy && policy->governor) {
			sprintf(cpufreq_default_gov[cpu], "%s",
					policy->governor->name);
			cpufreq_cpu_put(policy);
		} else {
			/* No policy or no gov set for this
			  * online cpu. If we are here, require
			  * serious debugging hence setting
			  * as pr_error.
			  */
			pr_err("No gov or No policy for online cpu:%d,"
					, cpu);
		}
	}
}
 
void cpufreq_change_gov(char *target_gov)
{
	int ret = -EINVAL;
	unsigned int cpu = 0;

#ifndef CONFIG_TEGRA_AUTO_HOTPLUG
	for_each_online_cpu(cpu)
#endif
	{
		ret = cpufreq_set_gov(target_gov, cpu);
		if (ret < 0)
			/* Unable to set gov for the online cpu.
			 * If it happens, needs to debug.
			 */
			pr_info("Unable to set gov:%s for online cpu:%d,"
				, cpufreq_default_gov[cpu]
					, cpu);
	}
}
 
void cpufreq_restore_default_gov(void)
{
	int ret = -EINVAL;
	unsigned int cpu = 0;
  
#ifndef CONFIG_TEGRA_AUTO_HOTPLUG
	for_each_online_cpu(cpu)
#endif
	{
		if (&cpufreq_default_gov[cpu] &&
			strlen((const char *)&cpufreq_default_gov[cpu])) {
			ret = cpufreq_set_gov(cpufreq_default_gov[cpu], cpu);
			if (ret < 0)
				/* Unable to restore gov for the cpu as
				  * It was online on suspend and becomes
				  * offline on resume.
				  */
				pr_info("Unable to restore gov:%s for cpu:%d,"
						, cpufreq_default_gov[cpu]
							, cpu);
		}
		cpufreq_default_gov[cpu][0] = '\0';
	}
}
#endif /* CONFIG_TEGRA_CONVSERVATIVE_GOV_ON_EARLYSUPSEND */
