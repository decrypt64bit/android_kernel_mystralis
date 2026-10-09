/*
 * ksu_status.c -- KSUN manual-hook diagnostics.
 *
 * Writes driver integration state to /data/local/tmp/ksu_status, readable by
 * adb shell without root. Every other surface on this device is closed:
 * dmesg is klogctl-blocked, /proc/kmsg and /sys/fs/pstore are
 * permission-denied, bugreport carries only logcat, and files created with
 * proc_create() inherit the generic "proc" SELinux type which the shell
 * domain is denied. /data/local/tmp is shell_data_file, which adb shell can
 * read.
 *
 * The snapshot callback is scheduled on init's task_work rather than called
 * inline. ksu_filp_open_compat() is a plain filp_open() with no credential
 * elevation, so on_boot_completed() -> track_throne() ->
 * filp_open("/data/system/packages.list") runs with whatever credentials the
 * caller has. When that caller is an app process (which is where our
 * setresuid hook fires) the open fails on DAC and the manager is never
 * crowned. Running the callback from init's context is what gives it root.
 */

#include <linux/fs.h>
#include <linux/init.h>
#include <linux/kernel.h>
#include <linux/printk.h>
#include <linux/sched.h>
#include <linux/slab.h>
#include <linux/task_work.h>
#include <linux/workqueue.h>

#include <linux/ksu.h>

extern struct cred *ksu_cred;

/*
 * dmesg_restrict is 1 on this ROM, which is why dmesg, /dev/kmsg and
 * /proc/sys/kernel/dmesg_restrict all return EACCES to adb shell. Without the
 * kernel log nothing about the driver can be observed, which is why five rounds
 * of changes have been flown blind. Clear it so adb shell can read dmesg.
 *
 * init re-asserts it from a sysctl after late_initcall, so it is also cleared
 * periodically from the delayed work below rather than only once.
 */
static void ksu_allow_dmesg(void)
{
	dmesg_restrict = 0;
}

#define KSU_STATUS_PATH "/data/local/tmp/ksu_status"

static void ksu_status_snapshot(void)
{
	const struct cred *old_cred = NULL;
	struct file *f;
	char buf[512];
	ssize_t written;
	int len;

	if (!ksu_cred)
		return;

	len = snprintf(buf, sizeof(buf),
		       "hook_mode\t\tmanual\n"
		       "late_loaded\t\t%d\n"
		       "ksu_boot_completed\t%d\n"
		       "manager_appid\t\t%d\n"
		       "manager_appid_valid\t%d\n"
		       "ksu_status_written\t1\n",
		       ksu_late_loaded ? 1 : 0,
		       ksu_boot_completed ? 1 : 0,
		       ksu_manager_appid,
		       ksu_manager_appid != (uid_t)-1);

	/*
	 * ksu_cred is prepare_creds()'d but never commit_creds()'d on the
	 * built-in path, so it carries the kernel init context rather than a su
	 * domain until setup_ksu_cred() has run. It is still uid 0, which is all
	 * the DAC check needs.
	 */
	old_cred = override_creds(ksu_cred);

	f = filp_open(KSU_STATUS_PATH, O_WRONLY | O_CREAT | O_TRUNC, 0644);
	if (IS_ERR(f)) {
		pr_warn("ksu_status: open %s failed: %ld\n", KSU_STATUS_PATH,
			PTR_ERR(f));
		goto out;
	}

	/*
	 * This tree's kernel_write() is fs/splice.c's variant, which leaves a
	 * zero-length file behind: it opens fine but the write never lands.
	 * fs/read_write.c's __kernel_write() is the path the rest of this kernel
	 * uses, and it takes pos as a pointer.
	 */
	{
		loff_t off = 0;

		written = __kernel_write(f, buf, len, &off);
	}
	if (written < 0)
		pr_warn("ksu_status: write failed: %zd\n", written);
	filp_close(f, NULL);

out:
	revert_creds(old_cred);
}

static void ksu_boot_completed_cb(struct callback_head *cb)
{
	kfree(cb);

	/*
	 * Mirror the late-load bootstrap that the built-in path skips.
	 *
	 * apply_kernelsu_rules() installs the su domain into the policy. It
	 * normally runs from the second-stage task_work, but if that did not
	 * fire there is no su domain to transition to, so retry here.
	 *
	 * setup_ksu_cred() is the important one: track_throne() does its own
	 * override_creds(ksu_cred), and on the built-in path ksu_cred is
	 * prepare_creds()'d from the kernel init context and never given the su
	 * domain. That context cannot read /data/system/packages.list, so the
	 * crown silently fails and the manager is never recognised.
	 */
	apply_kernelsu_rules();
	cache_sid();
	setup_ksu_cred();

	pr_info("ksu: firing on_boot_completed() from init context\n");
	on_boot_completed();

	/*
	 * ksu_manager_appid is only set once track_throne() has read
	 * packages.list, so this snapshot reflects the post-crown state.
	 */
	ksu_status_snapshot();
}

/*
 * Called from the setresuid hook the first time an app-range uid shows up,
 * which means zygote is running and /data is mounted. Schedules the real work
 * on init's task_work instead of doing it here: the caller is an app process
 * and track_throne() needs root to read packages.list.
 */
void ksu_fire_boot_completed(void)
{
	struct callback_head *cb;

	cb = kmalloc(sizeof(*cb), GFP_ATOMIC);
	if (!cb) {
		pr_warn("ksu: no memory for boot-completed task_work\n");
		on_boot_completed();
		ksu_status_snapshot();
		return;
	}

	cb->func = ksu_boot_completed_cb;

	/*
	 * This tree's task_work_add() takes a plain bool for "notify", not the
	 * TWA_RESUME/TWA_EXIT enum that arrived in 4.11. true == TWA_RESUME.
	 */
	if (task_work_add(&init_task, cb, true)) {
		pr_warn("ksu: task_work_add on init failed, running inline\n");
		kfree(cb);
		on_boot_completed();
		ksu_status_snapshot();
	}
}

void ksu_status_snapshot_now(void)
{
	ksu_status_snapshot();
}

/*
 * /data is not mounted at late_initcall, so the first snapshot always fails.
 * Retry a few times over the first minute so the file lands once it is, and
 * keep dmesg readable while we are at it.
 */
static void ksu_diag_work(struct work_struct *work);
static DECLARE_DELAYED_WORK(ksu_diag_dwork, ksu_diag_work);

static void ksu_diag_work(struct work_struct *work)
{
	static int rounds;

	ksu_allow_dmesg();
	ksu_status_snapshot();

	/* Retry for a few minutes rather than a minute: the crown can land late
	 * and the write itself has been unreliable. */
	if (++rounds < 48)
		schedule_delayed_work(&ksu_diag_dwork, 5 * HZ);
}

static int __init ksu_status_init(void)
{
	ksu_allow_dmesg();
	ksu_status_snapshot();
	schedule_delayed_work(&ksu_diag_dwork, 5 * HZ);
	return 0;
}
late_initcall(ksu_status_init);
