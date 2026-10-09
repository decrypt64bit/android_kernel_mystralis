/*
 * /proc/ksu_status -- a world-readable diagnostic surface for the KSUN
 * manual-hook port.
 *
 * dmesg is klogctl-blocked, /proc/kmsg and /sys/fs/pstore are
 * permission-denied, and the bugreport only carries logcat. That leaves
 * no way to observe kernel-side state while bringing the driver up, which
 * makes an integration problem like "the manager is never crowned"
 * undebuggable from the device.
 *
 * Every field here mirrors a piece of driver state we cannot otherwise see.
 * The file is intentionally 0444 so adb shell can read it without root.
 */

#include <linux/fs.h>
#include <linux/init.h>
#include <linux/kernel.h>
#include <linux/proc_fs.h>
#include <linux/seq_file.h>
#include <linux/uaccess.h>

#include <linux/ksu.h>

static int ksu_status_show(struct seq_file *m, void *v)
{
	seq_printf(m, "hook_mode\t\tmanual\n");
	seq_printf(m, "late_loaded\t\t%d\n", ksu_late_loaded ? 1 : 0);
	seq_printf(m, "ksu_boot_completed\t%d\n", ksu_boot_completed ? 1 : 0);
	seq_printf(m, "manager_appid\t\t%d\n", ksu_manager_appid);
	seq_printf(m, "manager_appid_valid\t%d\n",
		   ksu_manager_appid != (uid_t)-1);

	return 0;
}

static int ksu_status_open(struct inode *inode, struct file *file)
{
	return single_open(file, ksu_status_show, NULL);
}

static const struct file_operations ksu_status_fops = {
	.owner	= THIS_MODULE,
	.open	= ksu_status_open,
	.read	= seq_read,
	.llseek	= seq_lseek,
	.release = single_release,
};

static int __init ksu_status_init(void)
{
	proc_create("ksu_status", 0444, NULL, &ksu_status_fops);
	return 0;
}
late_initcall(ksu_status_init);
