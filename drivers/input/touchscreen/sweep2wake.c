/*
 * drivers/input/touchscreen/sweep2wake.c
 *
 *
 * Copyright (c) 2013, Dennis Rassmann <showp1984@gmail.com>
 * Copyright (c) 2015, Vineeth Raj <contact.twn@openmailbox.org>
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
#include <linux/input/sweep2wake.h>
#include <linux/slab.h>
#include <linux/workqueue.h>
#include <linux/input.h>
#include <linux/hrtimer.h>

#ifdef CONFIG_TOUCHSCREEN_DOUBLETAP2WAKE
#define ANDROID_TOUCH_DECLARED
#endif

/* Version, author, desc, etc */
#define DRIVER_AUTHOR "Dennis Rassmann <showp1984@gmail.com>, , Vineeth Raj <contact.twn@openmailbox.org>"
#define DRIVER_DESCRIPTION "uber simple s2w for almost any device"
#define DRIVER_VERSION "0.1"
#define LOGTAG "[sweep2wake]: "

MODULE_AUTHOR(DRIVER_AUTHOR);
MODULE_DESCRIPTION(DRIVER_DESCRIPTION);
MODULE_VERSION(DRIVER_VERSION);
MODULE_LICENSE("GPLv2");

/* Tuneables */
#define S2W_DEBUG           0
#define S2W_DEFAULT         1
#define S2W_PWRKEY_DUR      60

#if defined(CONFIG_MACH_PICO)
/* HTC Pico 2011 */
#define S2W_Y_LIMIT			910
#define S2W_Y_MAX				1050
#define S2W_X_MAX				1024
#define S2W_X_LIMIT			300
#define S2W_X_FINAL			S2W_X_MAX-S2W_X_LIMIT
#else
/* defaults */
/* todo: add standard defines later */
#define S2W_Y_LIMIT			910
#define S2W_Y_MAX				1050
#define S2W_X_MAX				1024
#define S2W_X_LIMIT			300
#define S2W_X_FINAL			S2W_X_MAX-S2W_X_LIMIT
#endif

/* Resources */
int s2w_switch = S2W_DEFAULT;
static int x_pre = 0;
static int touch_x = 0;
static bool touch_cnt = true;
#ifndef CONFIG_TOUCHSCREEN_DOUBLETAP2WAKE
/* todo: add s2s_only */
bool scr_suspended = false;
#endif

static struct input_dev * sweep2wake_pwrdev;
static DEFINE_MUTEX(pwrkeyworklock);

static struct workqueue_struct *s2w_input_wq;
static struct work_struct s2w_input_work;

/* PowerKey work func */
static void sweep2wake_presspwr(struct work_struct * sweep2wake_presspwr_work) {
	if (!mutex_trylock(&pwrkeyworklock))
		return;
	input_event(sweep2wake_pwrdev, EV_KEY, KEY_POWER, 1);
	input_event(sweep2wake_pwrdev, EV_SYN, 0, 0);
	msleep(S2W_PWRKEY_DUR);
	input_event(sweep2wake_pwrdev, EV_KEY, KEY_POWER, 0);
	input_event(sweep2wake_pwrdev, EV_SYN, 0, 0);
	msleep(S2W_PWRKEY_DUR);
	mutex_unlock(&pwrkeyworklock);
	return;
}
static DECLARE_WORK(sweep2wake_presspwr_work, sweep2wake_presspwr);

/* PowerKey trigger */
static void sweep2wake_pwrtrigger(void) {
	schedule_work(&sweep2wake_presspwr_work);
	return;
}

/* Sweep2wake main function */
static void detect_sweep2wake(int *x)
{
	if (!x_pre) {
		x_pre = *x;
	} else {
		if (x_pre < S2W_X_LIMIT) {
			if ( *x > S2W_X_FINAL ) {
				sweep2wake_pwrtrigger();
				x_pre = 0;
			}
		} else if (x_pre > S2W_X_FINAL) {
			if ( *x < S2W_X_LIMIT ) {
				sweep2wake_pwrtrigger();
				x_pre = 0;
			}
		}
	}
}

static void s2w_input_callback(struct work_struct *unused) {

	detect_sweep2wake(&touch_x);

	return;
}

static void s2w_input_event(struct input_handle *handle, unsigned int type,
				unsigned int code, int value) {
#if S2W_DEBUG
	pr_info("sweep2wake: code: %s|%u, val: %i\n",
		((code==ABS_MT_POSITION_X) ? "X" :
		(code==ABS_MT_POSITION_Y) ? "Y" :
		(code==ABS_MT_TRACKING_ID) ? "ID" :
		"undef"), code, value);
#endif
	if (code == ABS_MT_SLOT) {
		x_pre = 0;
		return;
	}

	if (code == ABS_MT_TRACKING_ID && value == -1) {
		x_pre = 0;
		return;
	}

	if (code == ABS_MT_POSITION_Y) {
		if ((value < S2W_Y_LIMIT) || (value > S2W_Y_MAX)) {
			x_pre = 0;
			return;
		}
	}

	if (code == ABS_MT_POSITION_X) {
		touch_x = value;
		queue_work_on(0, s2w_input_wq, &s2w_input_work);
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

static int s2w_input_connect(struct input_handler *handler,
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
	handle->name = "s2w";

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

static void s2w_input_disconnect(struct input_handle *handle) {
	input_close_device(handle);
	input_unregister_handle(handle);
	kfree(handle);
}

static const struct input_device_id s2w_ids[] = {
	{ .driver_info = 1 },
	{ },
};

static struct input_handler s2w_input_handler = {
	.event		= s2w_input_event,
	.connect	= s2w_input_connect,
	.disconnect	= s2w_input_disconnect,
	.name		= "s2w_inputreq",
	.id_table	= s2w_ids,
};

/*
 * SYSFS stuff below here
 */
static ssize_t s2w_show(struct device *dev,
		struct device_attribute *attr, char *buf)
{
	size_t count = 0;

	count += sprintf(buf, "%d\n", s2w_switch);

	return count;
}

static ssize_t s2w_dump(struct device *dev,
		struct device_attribute *attr, const char *buf, size_t count)
{
	if (buf[0] == '0') {
		s2w_switch = 0;
	} else if (buf[0] == '1') {
		s2w_switch = 1;
	}

	return count;
}

static DEVICE_ATTR(s2w_switch, (S_IWUSR|S_IRUGO),
	s2w_show, s2w_dump);

/*
 * INIT / EXIT stuff below here
 */
#ifdef ANDROID_TOUCH_DECLARED
extern struct kobject *ts_mods_kobj;
#else
struct kobject *ts_mods_kobj;
EXPORT_SYMBOL_GPL(ts_mods_kobj);
#endif
static int __init sweep2wake_init(void)
{
	int rc = 0;

	sweep2wake_pwrdev = input_allocate_device();
	if (!sweep2wake_pwrdev) {
		pr_err("Can't allocate suspend autotest power button\n");
		goto err_alloc_dev;
	}

	input_set_capability(sweep2wake_pwrdev, EV_KEY, KEY_POWER);
	sweep2wake_pwrdev->name = "s2w_pwrkey";
	sweep2wake_pwrdev->phys = "s2w_pwrkey/input0";

	rc = input_register_device(sweep2wake_pwrdev);
	if (rc) {
		pr_err("%s: input_register_device err=%d\n", __func__, rc);
		goto err_input_dev;
	}

	s2w_input_wq = create_workqueue("s2wiwq");
	if (!s2w_input_wq) {
		pr_err("%s: Failed to create s2wiwq workqueue\n", __func__);
		return -EFAULT;
	}
	INIT_WORK(&s2w_input_work, s2w_input_callback);
	rc = input_register_handler(&s2w_input_handler);
	if (rc)
		pr_err("%s: Failed to register s2w_input_handler\n", __func__);

#ifndef ANDROID_TOUCH_DECLARED
	ts_mods_kobj = kobject_create_and_add("ts_mods", NULL) ;
	if (ts_mods_kobj == NULL) {
		pr_warn("%s: ts_mods_kobj create_and_add failed\n", __func__);
	}
#endif
	rc = sysfs_create_file(ts_mods_kobj, &dev_attr_s2w_switch.attr);
	if (rc) {
		pr_warn("%s: sysfs_create_file failed for s2w_switch\n", __func__);
	}

err_input_dev:
	input_free_device(sweep2wake_pwrdev);
err_alloc_dev:
	pr_info(LOGTAG"%s done\n", __func__);

	return 0;
}

static void __exit sweep2wake_exit(void)
{
#ifndef ANDROID_TOUCH_DECLARED
	kobject_del(ts_mods_kobj);
#endif
	input_unregister_handler(&s2w_input_handler);
	destroy_workqueue(s2w_input_wq);
	input_unregister_device(sweep2wake_pwrdev);
	input_free_device(sweep2wake_pwrdev);
	return;
}

module_init(sweep2wake_init);
module_exit(sweep2wake_exit);
