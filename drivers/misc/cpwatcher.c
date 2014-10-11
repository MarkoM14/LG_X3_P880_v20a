
/*
 * LGE - CP Watcher Driver
 *
 * Copyright (C) 2010 LGE, Inc.
 *
 *
 * Author: Kyungsik Lee <kyungsik.lee@lge.com>
 *
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA 02111-1307 USA
 *
 */

#include <linux/module.h>
#include <linux/device.h>
#include <linux/init.h>
#include <linux/interrupt.h>
#include <linux/platform_device.h>
#include <linux/delay.h>
#include <linux/moduleparam.h>
#include <linux/kthread.h>
#include <linux/workqueue.h>

#include <asm/gpio.h>
#include <asm/irq.h>
#include <asm/io.h>
#include <asm/system.h>

#include "../../arch/arm/mach-tegra/gpio-names.h"
#include "../../arch/arm/mach-tegra/lge/x3/include/lge/board-x3-nv.h"

#include <linux/slab.h>
#include <linux/errno.h>
#include <linux/kernel.h>
#include <linux/wakelock.h> 
#include <linux/rtc.h>

#include "../../arch/arm/mach-tegra/baseband-xmm-power.h"

#define NV_DEBUG 0

#if 1 //defined(CONFIG_DEBUG_FS)

#include <linux/debugfs.h>

#define ENABLE_DEBUG_MESSAGE 0x01
#define ENABLE_TEST_MODE 0x02
#define KEYBACKLIGHT_LEDS_GPIO 138


#define CPW_SIMPLE_DEBUG

#ifdef CPW_SIMPLE_DEBUG
static int debug_enable_flag = 1;//Debug
#else
static int debug_enable_flag = 0;//Release
#endif

#endif//CONFIG_DEBUG_FS

/*
 * Debug
 */
#define DEBUG_CP
#ifdef DEBUG_CP
#define DBG(x...) if (debug_enable_flag & ENABLE_DEBUG_MESSAGE) { \
						printk(x); \
				  }
#else
#define DBG(x...) do { } while(0)
#endif

#if !defined(TRUE)
#define TRUE 0x01
#endif
#if !defined(FALSE)
#define FALSE 0x00 
#endif

#define CPWATCHER_DELAY_TIME msecs_to_jiffies(50)

static int cp_crash_int_pin		= TEGRA_GPIO_PS5;		//Rev.C

extern int is_cp_crash;  //RIP-13119 : RIL recovery should be started by USB-disconnection.

struct cpwatcher_dev {

        unsigned char onoff;
        
	struct delayed_work delayed_work_cpwatcher;
	struct mutex lock;
        struct wake_lock wake_lock;
};
static struct cpwatcher_dev *cpwatcher;

static int lge_is_ril_recovery_mode_enabled(void)
{
	char data[2] = {0x00,0x00};

	lge_nvdata_read(LGE_NVDATA_RIL_RECOVERY_MODE_OFFSET,data,1);
	msleep(100);

	if(data[0] == 0x01)
		return 1;
	else
		return 0;
}

#if 1 //defined(CONFIG_DEBUG_FS)
static int debug_control_set(void *data, u64 val)
{

	if (val & ENABLE_DEBUG_MESSAGE) {
		
		debug_enable_flag |= ENABLE_DEBUG_MESSAGE;
	}
	if (val & ENABLE_TEST_MODE) {
		debug_enable_flag |= ENABLE_TEST_MODE;
	}

	if (!val) {

		debug_enable_flag = 0;
	}

	return 0;
}


static int debug_control_get(void *data, u64 *val)
{
	*val = debug_enable_flag;
	 
	return 0;
}

DEFINE_SIMPLE_ATTRIBUTE(debug_fops, 
                        debug_control_get, debug_control_set, "%llu\n");

static int __init cpwatcher_debug_init(void)
{
	debugfs_create_file("cpwatcher", 0664,
					NULL, NULL, &debug_fops);

	return 0;
}
#endif//CONFIG_DEBUG_FS

static void platform_release(struct device *dev);

static struct platform_device cp_device = { 
		    .name       = "cpwatcher",
			    .id     = -1,
        {
		.platform_data = NULL, 		
		.release = platform_release,    /* a warning is thrown during rmmod if this is absent */
	},			    
};

static void platform_release(struct device *dev) 
{	
    printk("tspdrv: platform_release.\n");
}

static irqreturn_t cpwatcher_irq_handler(int irq, void *dev_id)
{
	struct cpwatcher_dev *dev = cpwatcher;

	if ((dev->onoff) && (is_cp_crash == 0)){
		printk("[CPW] %s()\n", __FUNCTION__);
		//NvOdmGpioGetState(cpwatcher_dev.hGpio, cpwatcher_dev.hCP_status, &pinValue);
		schedule_delayed_work(&dev->delayed_work_cpwatcher, CPWATCHER_DELAY_TIME);
	}
	return IRQ_HANDLED;
}

static void cpwatcher_work_func(struct work_struct *wq)
{
	struct cpwatcher_dev *dev = cpwatcher;
	int status = 0;
	int retry = 4;

	printk("[CPW] cpwatcher_work_func()\n");
	  
	if (dev->onoff) {
		status = gpio_get_value(cp_crash_int_pin);
		printk("[CPW] %s(), status: %d\n", __FUNCTION__, status);

		if (status == 0) {
			wake_lock(&dev->wake_lock);
			is_cp_crash = 1; //RIP-13119 : RIL recovery should be started by USB-disconnection.

			if (lge_is_ril_recovery_mode_enabled() != 1) {
				extern void muic_proc_set_cp_usb_force(void);
				gpio_set_value(KEYBACKLIGHT_LEDS_GPIO, 1); //Keybacklight on when occurs CP Crash
				do {
					printk("[CPW] CP Crash : lge_is_ril_recovery_mode_enabled() = 1 ...change to CP_USB mode\n");
					muic_proc_set_cp_usb_force();
					msleep(400);
					status = gpio_get_value(cp_crash_int_pin);
					printk("[CPW] cp_crash_int_pin status: %d (1=Modem is OK)\n", status);
					retry--;
				} while ((!status) && (retry));
				if (retry) {
					printk("[CPW] CP Crash : modem crash is normal \n");
					is_cp_crash = 0;
					gpio_set_value(KEYBACKLIGHT_LEDS_GPIO, 0);
				} else {
					printk("[CPW] CP Crash : modem crash is abnormal, force xmm restart\n");
					//reset baseband_xmm
					baseband_xmm_power_switch(0);
					//ts0710_mux will turn it back on
				}
			}
			wake_unlock(&dev->wake_lock);
			return;
		}
	}
}

static ssize_t
cpwatcher_show_onoff(struct device *dev, struct device_attribute *attr, char *buf)
{
	int result = 0;

	if (!cpwatcher) return 0;
	result  = snprintf(buf, PAGE_SIZE, "%s\n", (cpwatcher->onoff == TRUE)  ? "1":"0");

	return result;
}

static ssize_t
cpwatcher_store_onoff(struct device *dev, struct device_attribute *attr, const char *buf, size_t count)
{
	int onoff;
	
	if (!count)
		return -EINVAL;

	mutex_lock(&cpwatcher->lock);
	sscanf(buf, "%d", &onoff);

	if (onoff) {
		cpwatcher->onoff = TRUE;
		printk("[CPW] On\n");
	} else {
		cpwatcher->onoff = FALSE;
		printk("[CPW] Off\n");
	}
	mutex_unlock(&cpwatcher->lock);

	return count;
}

//RIP-73256 : [X3] CTS issue : permission error - byeonggeun.kim [START]
static DEVICE_ATTR(onoff, 0664, cpwatcher_show_onoff, cpwatcher_store_onoff);
//RIP-73256 : [X3] CTS issue : permission error - byeonggeun.kim [END]

static struct attribute *cpwatcher_attributes[] = {
	&dev_attr_onoff.attr,
	NULL,
};

static const struct attribute_group cpwatcher_group = {
	.attrs = cpwatcher_attributes,
};

static int cpwatcher_probe(struct platform_device *pdev)
{
	struct cpwatcher_dev *dev; 
	struct device *dev_sys = &pdev->dev;
	int ret;

	dev = kzalloc(sizeof(struct cpwatcher_dev), GFP_KERNEL);
	if (!dev) {
		ret = -ENOMEM;
		goto fail_gpio_open;
	}
	cpwatcher = dev;

	dev->onoff= TRUE;
	INIT_DELAYED_WORK(&dev->delayed_work_cpwatcher, cpwatcher_work_func);

	/* GPIO */
        //Rev.B : temporary port (TEGRA_GPIO_PP1) for verifiy the CP CRASH
        //Rev.C: KB_ROW13 / TEGRA_GPIO_PS5

        tegra_gpio_enable(cp_crash_int_pin);
        
        ret = gpio_request(cp_crash_int_pin, "CP_CRASH_INT");
        if (ret < 0) {
             printk("CP_CRASH[1] : Fail to request CP_CRASH_INT enabling\n");
             goto fail_gpio_open;
        }

        ret = gpio_direction_input(cp_crash_int_pin);
        if (ret < 0) {
             printk("CP_CRASH[2] : Fail to direct CP_CRASH_INT enabling\n");
             gpio_free(cp_crash_int_pin);
             goto fail_gpio_open;
        }               
	printk("[CPW] CP_CRASH_INT : %d, Line[%d]\n",	gpio_get_value(cp_crash_int_pin),__LINE__); 	
	
	ret = request_threaded_irq(TEGRA_GPIO_TO_IRQ(cp_crash_int_pin), NULL,
		cpwatcher_irq_handler,IRQF_TRIGGER_FALLING, "cpwatcher", NULL);
	if (ret < 0) {
		printk("%s Could not allocate APDS990x_INT !\n", __func__);
		goto fail_gpio_open;
	}

	if (sysfs_create_group(&dev_sys->kobj, &cpwatcher_group)) {
		printk("[CPW] Failed to create sys filesystem\n");
	}

	mutex_init(&dev->lock);
        wake_lock_init(&dev->wake_lock, WAKE_LOCK_SUSPEND, "cpwatcher");
	printk("[CPW] CP Watcher Initialization completed\n");

	return 0;

fail_gpio_open:
	free_irq(TEGRA_GPIO_TO_IRQ(cp_crash_int_pin), dev);
	kfree(dev);
	return 0;
}

static int __devexit cpwatcher_remove(struct platform_device *pdev)
{
	free_irq(TEGRA_GPIO_TO_IRQ(cp_crash_int_pin), NULL);
        wake_lock_destroy(&cpwatcher->wake_lock);

	kfree(cpwatcher);

	DBG("[CPW] %s(): success\n", __FUNCTION__);

	return 0;
}

static struct platform_driver cpwatcher_driver = {
	.probe		= cpwatcher_probe,
	.remove		= __devexit_p(cpwatcher_remove),
	.driver		= 
	{
		.name	= "cpwatcher",
		.owner	= THIS_MODULE,
	},
};

static int __init cpwatcher_init(void)
{
	printk("[CPW] CP Watcher Initialization started\n");
#ifdef DEBUG_CP
	cpwatcher_debug_init();
#endif//DEBUG_CP

	if (platform_device_register(&cp_device)) {

		printk("[CPW] %s(): fail\n", __FUNCTION__);
	}

	return platform_driver_register(&cpwatcher_driver);
}
module_init(cpwatcher_init);

static void __exit cpwatcher_exit(void)
{
	platform_driver_unregister(&cpwatcher_driver);
	printk("[CPW] %s(): success\n", __FUNCTION__);
}
module_exit(cpwatcher_exit);

MODULE_AUTHOR("LG Electronics");
MODULE_DESCRIPTION("LGE CP Watcher Driver");
MODULE_LICENSE("GPL");
