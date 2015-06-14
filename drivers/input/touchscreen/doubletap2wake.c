/*
 * drivers/input/touchscreen/doubletap2wake.c
 *
 * Copyright (c) 2015, Vineeth Raj <contact.twn@openmailbox.org>
 * Copyright (c) 2013, Dennis Rassmann <showp1984@gmail.com>
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
#include <linux/module.h>
#include <linux/types.h>
#include <linux/delay.h>
#include <linux/init.h>
#include <linux/err.h>
#include <linux/input/doubletap2wake.h>
#include <linux/slab.h>
#include <linux/workqueue.h>
#include <linux/input.h>
#include <linux/hrtimer.h>
#include <asm-generic/cputime.h>

/* Version, author, desc, etc */
#define DRIVER_AUTHOR "Vineeth Raj <contact.twn@openmailbox.org>"
#define DRIVER_DESCRIPTION "uber simple d2w for almost any device"
#define DRIVER_VERSION "0.1"
#define LOGTAG "[doubletap2wake]: "

MODULE_AUTHOR(DRIVER_AUTHOR);
MODULE_DESCRIPTION(DRIVER_DESCRIPTION);
MODULE_VERSION(DRIVER_VERSION);
MODULE_LICENSE("GPLv2");

/* Tuneables */
#define D2W_DEFAULT     1
#define D2W_DEBUG       1
#define D2W_PWRKEY_DUR  60
#define D2W_FEATHER     50
#define D2W_TIME        700
/* Tuneables (end) */

/* Resources */
int d2w_switch = D2W_DEFAULT;
static cputime64_t tap_time_pre = 0;
static int x_pre = 0, y_pre = 0;
static int touch_x = 0, touch_y = 0;
static bool touch_cnt = true;
bool scr_suspended = false;

static struct input_dev * doubletap2wake_pwrdev;
static DEFINE_MUTEX(pwrkeyworklock);

static struct workqueue_struct *d2w_input_wq;
static struct work_struct d2w_input_work;
/* Resources (end) */

/* Configs */
#ifdef CONFIG_INPUT_CAPELLA_CM3628_POCKETMOD
#define CUSTOM_CHECK_DEF
#include <linux/cm3628_pocketmod.h>
static int (*nyx_check) (void) = pocket_detection_check;
#endif
/* Configs (end) */

void doubletap2wake_reset(void) {
	tap_time_pre = 0;
	x_pre = 0;
	y_pre = 0;
	touch_cnt = false;
	touch_x = 0; touch_y = 0;
}

static void doubletap2wake_presspwr(struct work_struct * doubletap2wake_presspwr_work) {
	if (!mutex_trylock(&pwrkeyworklock))
		return;
	input_event(doubletap2wake_pwrdev, EV_KEY, KEY_POWER, 1);
	input_event(doubletap2wake_pwrdev, EV_SYN, 0, 0);
	msleep(D2W_PWRKEY_DUR);
	input_event(doubletap2wake_pwrdev, EV_KEY, KEY_POWER, 0);
	input_event(doubletap2wake_pwrdev, EV_SYN, 0, 0);
	msleep(D2W_PWRKEY_DUR);
	mutex_unlock(&pwrkeyworklock);
	return;
}
static DECLARE_WORK(doubletap2wake_presspwr_work, doubletap2wake_presspwr);

/* PowerKey trigger */
static void doubletap2wake_pwrtrigger(void) {
	schedule_work(&doubletap2wake_presspwr_work);
	return;
}

static inline unsigned int calc_feather(int coord, int prev_coord) {
	return abs(coord - prev_coord);
}

static inline void new_touch(int x, int y) {
	tap_time_pre = ktime_to_ms(ktime_get());
	x_pre = x;
	y_pre = y;
}

static void detect_doubletap2wake(int x, int y)
{
	if (touch_cnt == false) {
		new_touch(x, y);
	} else {
		if ((calc_feather(x, x_pre) < D2W_FEATHER) &&
				(calc_feather(y, y_pre) < D2W_FEATHER) &&
				(((ktime_to_ms(ktime_get()))-tap_time_pre) < D2W_TIME)) {
			doubletap2wake_reset();
#ifdef CUSTOM_CHECK_DEF
			if (nyx_check() <= 0)
#endif
			doubletap2wake_pwrtrigger();
		} else {
			doubletap2wake_reset();
			new_touch(x, y);
		}
	}
} //detect_doubletap2wake

static void d2w_input_callback(struct work_struct *unused) {

	detect_doubletap2wake(touch_x, touch_y);

	return;
}

static void d2w_input_event(struct input_handle *handle, unsigned int type,
				unsigned int code, int value) {
#if D2W_DEBUG
	pr_info(LOGTAG"code: %s|%u, val: %i\n",
		((code==ABS_MT_POSITION_X) ? "X" :
		(code==ABS_MT_POSITION_Y) ? "Y" :
		(code==ABS_MT_TRACKING_ID) ? "ID" :
		"undef"), code, value);
#endif
	if (!scr_suspended)
#ifdef CONFIG_HTC_ONMODE_CHARGING
		if (!in_onchg_resume)
#endif
			return;

	if (code == ABS_MT_SLOT) {
		doubletap2wake_reset();
		return;
	}

	if (code == ABS_MT_TRACKING_ID && value == -1) {
		touch_cnt = true;
		touch_x = 0; touch_y = 0;
		return;
	}

	if (code == ABS_MT_POSITION_X) {
		touch_x = value;
	}

	if (code == ABS_MT_POSITION_Y) {
		touch_y = value;
	}

#if D2W_DEBUG
	pr_info(LOGTAG"touch_x: %d, touch_y: %d\n", touch_x, touch_y);
#endif

	if (touch_x && touch_y) {
		touch_x = 0; touch_y = 0;
		queue_work_on(0, d2w_input_wq, &d2w_input_work);
	}
}

static int input_dev_filter(struct input_dev *dev) {
	if (strstr(dev->name, "touch")||
		strstr(dev->name, "himax-touchscreen")) {
		return 0;
	} else {
		return 1;
	}
}

static int d2w_input_connect(struct input_handler *handler,
				struct input_dev *dev, const struct input_device_id *id) {
	struct input_handle *handle;
	int error;

	if (input_dev_filter(dev))
		return -ENODEV;

	handle = kzalloc(sizeof(struct input_handle), GFP_KERNEL);
	if (!handle)
		return -ENOMEM;

	handle->dev = dev;
	handle->handler = handler;
	handle->name = "d2w";

	error = input_register_handle(handle);
	if (error)
		goto err2;

	error = input_open_device(handle);
	if (error)
		goto err1;

	return 0;
err1:
	input_unregister_handle(handle);
err2:
	kfree(handle);
	return error;
}

static void d2w_input_disconnect(struct input_handle *handle) {
	input_close_device(handle);
	input_unregister_handle(handle);
	kfree(handle);
}

static const struct input_device_id d2w_ids[] = {
	{ .driver_info = 1 },
	{ },
};

static struct input_handler d2w_input_handler = {
	.event			= d2w_input_event,
	.connect		= d2w_input_connect,
	.disconnect	= d2w_input_disconnect,
	.name				= "d2w_inputreq",
	.id_table		= d2w_ids,
};

/*
 * SYSFS stuff below here
 */
static ssize_t d2w_show(struct device *dev,
		struct device_attribute *attr, char *buf)
{
	size_t count = 0;

	count += sprintf(buf, "%d\n", d2w_switch);

	return count;
}

static ssize_t d2w_dump(struct device *dev,
		struct device_attribute *attr, const char *buf, size_t count)
{
	if (buf[0] == '0') {
		input_unregister_handler(&d2w_input_handler);
		d2w_switch = 0;
	} else if (buf[0] == '1') {
		if (!d2w_switch) {
			if (!input_register_handler(&d2w_input_handler)) {
				d2w_switch = 1;
			}
		}
	}

	return count;
}

static DEVICE_ATTR(d2w_switch, (S_IWUSR|S_IRUGO),
	d2w_show, d2w_dump);

/*
 * INIT / EXIT stuff below here
 */
struct kobject *ts_mods_kobj;
EXPORT_SYMBOL_GPL(ts_mods_kobj);
static int __init doubletap2wake_init(void)
{
	int rc = 0;

	doubletap2wake_pwrdev = input_allocate_device();
	if (!doubletap2wake_pwrdev) {
		pr_err("Can't allocate suspend autotest power button\n");
		goto err_alloc_dev;
	}

	input_set_capability(doubletap2wake_pwrdev, EV_KEY, KEY_POWER);
	doubletap2wake_pwrdev->name = "d2w_pwrkey";
	doubletap2wake_pwrdev->phys = "d2w_pwrkey/input0";

	rc = input_register_device(doubletap2wake_pwrdev);
	if (rc) {
		pr_err("%s: input_register_device err=%d\n", __func__, rc);
		goto err_input_dev;
	}

	d2w_input_wq = create_workqueue("d2wiwq");
	if (!d2w_input_wq) {
		pr_err("%s: Failed to create d2wiwq workqueue\n", __func__);
		return -EFAULT;
	}
	INIT_WORK(&d2w_input_work, d2w_input_callback);
	rc = input_register_handler(&d2w_input_handler);
	if (rc)
		pr_err("%s: Failed to register d2w_input_handler\n", __func__);

	ts_mods_kobj = kobject_create_and_add("ts_mods", NULL) ;
	if (ts_mods_kobj == NULL) {
		pr_warn("%s: ts_mods_kobj create_and_add failed\n", __func__);
	}
	rc = sysfs_create_file(ts_mods_kobj, &dev_attr_d2w_switch.attr);
	if (rc) {
		pr_warn("%s: sysfs_create_file failed for d2w_switch\n", __func__);
	}

err_input_dev:
	input_free_device(doubletap2wake_pwrdev);
err_alloc_dev:
	pr_info(LOGTAG"%s done\n", __func__);

	return 0;
}

static void __exit doubletap2wake_exit(void)
{
	kobject_del(ts_mods_kobj);
	input_unregister_handler(&d2w_input_handler);
	destroy_workqueue(d2w_input_wq);
	input_unregister_device(doubletap2wake_pwrdev);
	input_free_device(doubletap2wake_pwrdev);
	return;
}

module_init(doubletap2wake_init);
module_exit(doubletap2wake_exit);
