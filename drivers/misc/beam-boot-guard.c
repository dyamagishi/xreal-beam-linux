// SPDX-License-Identifier: GPL-2.0-only
/* Opt-in timeout for diagnostic initramfs with read-only root. */
#include <linux/init.h>
#include <linux/jiffies.h>
#include <linux/kernel.h>
#include <linux/kobject.h>
#include <linux/reboot.h>
#include <linux/sysfs.h>
#include <linux/workqueue.h>

static unsigned int guard_seconds;
static struct delayed_work guard_work;

static int __init beam_guard_setup(char *str)
{
	unsigned int seconds;

	if (!kstrtouint(str, 10, &seconds) && seconds >= 30 && seconds <= 1800)
		guard_seconds = seconds;
	return 1;
}
__setup("beam.boot_guard=", beam_guard_setup);

static void beam_guard_expired(struct work_struct *work)
{
	pr_emerg("Beam RAM-boot guard expired; rebooting to on-disk kernel\n");
	/* Driver removal/shutdown may be the fault under investigation. */
	emergency_restart();
}

static int __init beam_guard_arm(void)
{
	if (guard_seconds) {
		INIT_DELAYED_WORK(&guard_work, beam_guard_expired);
		schedule_delayed_work(&guard_work, guard_seconds * HZ);
		pr_info("Beam RAM-boot guard armed for %u seconds\n", guard_seconds);
	}
	return 0;
}
core_initcall(beam_guard_arm);

static ssize_t ready_store(struct kobject *kobj, struct kobj_attribute *attr,
                           const char *buf, size_t count)
{
	bool ready;

	if (kstrtobool(buf, &ready) || !ready)
		return -EINVAL;
	cancel_delayed_work_sync(&guard_work);
	pr_info("Beam RAM-boot guard cancelled by userspace\n");
	return count;
}
static struct kobj_attribute ready_attr = __ATTR_WO(ready);

static int __init beam_guard_sysfs(void)
{
	struct kobject *kobj;

	if (!guard_seconds)
		return 0;
	kobj = kobject_create_and_add("beam_boot_guard", kernel_kobj);
	if (!kobj)
		return -ENOMEM;
	if (sysfs_create_file(kobj, &ready_attr.attr)) {
		kobject_put(kobj);
		return -ENOMEM;
	}
	return 0;
}
late_initcall(beam_guard_sysfs);
