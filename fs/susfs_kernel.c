// SPDX-License-Identifier: GPL-2.0
/*
 * Kernel-side integration for the SUSFS driver (SUSFS v1.5.5, kernel 4.9).
 *
 * The official SUSFS KernelSU patch injects the susfs command dispatcher and
 * a few helpers into weishu's KernelSU. This tree runs the backslashxx
 * KernelSU rewrite (ioctl "supercall" uapi, no prctl commands, no SUSFS
 * hooks), so the pieces the driver needs are provided here instead:
 *
 *   - susfs_is_current_ksu_domain() / susfs_is_current_zygote_domain(),
 *     the SELinux domain checks used by the mount hooks in fs/namespace.c
 *   - susfs_handle_prctl(), the ksu_susfs command ABI
 *     (prctl(0xdeadbeef, CMD_SUSFS_*, ...))
 *   - ksu_try_umount() / susfs_try_umount_all(), the try_umount helpers
 *
 * KernelSU itself is only patched (susfs/20_ksu_susfs_minimal.patch) to mark
 * non-root app tasks and to run the SUSFS try_umount list when an app starts.
 */

#include <linux/kernel.h>
#include <linux/init.h>
#include <linux/types.h>
#include <linux/string.h>
#include <linux/errno.h>
#include <linux/sched.h>
#include <linux/cred.h>
#include <linux/uaccess.h>
#include <linux/security.h>
#include <linux/namei.h>
#include <linux/fs.h>
#include <linux/mount.h>
#include <linux/path.h>
#include <linux/susfs.h>
#include <linux/susfs_def.h>

#ifdef CONFIG_KSU_SUSFS

/* defined in fs/namespace.c */
extern bool susfs_is_mnt_devname_ksu(struct path *path);
#ifdef CONFIG_KSU_SUSFS_TRY_UMOUNT
extern void susfs_run_try_umount_for_current_mnt_ns(void);
#endif
/* also defined in fs/namespace.c, consumes the references of the path */
extern int path_umount(struct path *path, int flags);
/* defined in fs/susfs.c */
#ifdef CONFIG_KSU_SUSFS_ENABLE_LOG
extern bool susfs_is_log_enabled;
#endif

/*******************/
/* SELinux domains */
/*******************/
#define KERNEL_SU_DOMAIN	"u:r:su:s0"
#define KERNEL_INIT_DOMAIN	"u:r:init:s0"
#define KERNEL_ZYGOTE_DOMAIN	"u:r:zygote:s0"

static u32 susfs_ksu_sid;
static u32 susfs_zygote_sid;
static u32 susfs_init_sid;
static bool susfs_sids_ready;

static void susfs_set_sid(const char *secctx_name, u32 *out_sid)
{
	if (security_secctx_to_secid(secctx_name, strlen(secctx_name), out_sid))
		pr_err("susfs: failed setting sid for '%s'\n", secctx_name);
}

/*
 * SELinux is not up yet during early boot, so resolve the sids on first use
 * instead of from an initcall that would run too early.
 */
static bool susfs_ensure_sids(void)
{
	if (susfs_sids_ready)
		return true;

	susfs_ksu_sid = 0;
	susfs_zygote_sid = 0;
	susfs_init_sid = 0;
	susfs_set_sid(KERNEL_SU_DOMAIN, &susfs_ksu_sid);
	susfs_set_sid(KERNEL_ZYGOTE_DOMAIN, &susfs_zygote_sid);
	susfs_set_sid(KERNEL_INIT_DOMAIN, &susfs_init_sid);

	susfs_sids_ready = susfs_ksu_sid && susfs_zygote_sid && susfs_init_sid;
	return susfs_sids_ready;
}

bool susfs_is_current_ksu_domain(void)
{
	u32 sid = 0;

	if (!susfs_ensure_sids())
		return false;
	security_task_getsecid(current, &sid);
	return sid == susfs_ksu_sid;
}

bool susfs_is_current_zygote_domain(void)
{
	u32 sid = 0;

	if (!susfs_ensure_sids())
		return false;
	security_task_getsecid(current, &sid);
	return sid == susfs_zygote_sid;
}

/**************/
/* try_umount */
/**************/
#ifdef CONFIG_KSU_SUSFS_TRY_UMOUNT
void ksu_try_umount(const char *mnt, bool check_mnt, int flags, uid_t uid)
{
	struct path path;
	int err;

	err = kern_path(mnt, 0, &path);
	if (err)
		return;

	if (path.dentry != path.mnt->mnt_root) {
		/* it is not root mountpoint, maybe umounted by others already */
		path_put(&path);
		return;
	}

	if (check_mnt && !susfs_is_mnt_devname_ksu(&path)) {
		path_put(&path);
		return;
	}

#ifdef CONFIG_KSU_SUSFS_ENABLE_LOG
	if (susfs_is_log_enabled)
		pr_info("susfs: umounting '%s' for uid: %d\n", mnt, uid);
#endif

	/* path_umount() releases the references taken by kern_path() */
	err = path_umount(&path, flags);
	if (err)
		pr_warn("susfs: umount %s failed: %d\n", mnt, err);
}

void susfs_try_umount_all(uid_t uid)
{
	susfs_try_umount(uid);
	/* fixed KSU mount points, same list the official patch uses */
	ksu_try_umount("/system", true, 0, uid);
	ksu_try_umount("/system_ext", true, 0, uid);
	ksu_try_umount("/vendor", true, 0, uid);
	ksu_try_umount("/product", true, 0, uid);
	ksu_try_umount("/odm", true, 0, uid);
	/*
	 * '/data/adb/modules' is a loop device, we can't tell whether its
	 * dev_name is KSU or not, and it is safe to just umount it if it is
	 * really a mountpoint.
	 */
	ksu_try_umount("/data/adb/modules", false, MNT_DETACH, uid);
	ksu_try_umount("/debug_ramdisk", true, MNT_DETACH, uid);
}
#endif /* CONFIG_KSU_SUSFS_TRY_UMOUNT */

/**********************/
/* ksu_susfs commands */
/**********************/
#define KSU_SUSFS_PRCTL_OPTION	0xdeadbeef

static bool susfs_access_ok(const void __user *addr, unsigned long size)
{
	return access_ok(VERIFY_READ, addr, size);
}

static void susfs_put_error(unsigned long arg5, int error)
{
	if (!access_ok(VERIFY_WRITE, (void __user *)arg5, sizeof(error))) {
		pr_err("susfs: arg5 is not accessible\n");
		return;
	}
	if (copy_to_user((void __user *)arg5, &error, sizeof(error)))
		pr_info("susfs: copy_to_user() failed\n");
}

/*
 * Dispatcher for the ksu_susfs userspace ABI:
 *     prctl(0xdeadbeef, CMD_SUSFS_*, arg3, arg4, &error)
 * Returns true when the call was a SUSFS command and has been handled here.
 */
bool susfs_handle_prctl(int option, unsigned long arg2, unsigned long arg3,
			unsigned long arg4, unsigned long arg5)
{
	if (option != KSU_SUSFS_PRCTL_OPTION)
		return false;

	/* like the official patch, only root may drive the driver */
	if (current_uid().val != 0)
		return false;

#ifdef CONFIG_KSU_SUSFS_SUS_PATH
	if (arg2 == CMD_SUSFS_ADD_SUS_PATH) {
		int error;

		if (!susfs_access_ok((void __user *)arg3, sizeof(struct st_susfs_sus_path))) {
			pr_err("susfs: CMD_SUSFS_ADD_SUS_PATH -> arg3 is not accessible\n");
			return true;
		}
		error = susfs_add_sus_path((struct st_susfs_sus_path __user *)arg3);
		pr_info("susfs: CMD_SUSFS_ADD_SUS_PATH -> ret: %d\n", error);
		susfs_put_error(arg5, error);
		return true;
	}
#endif /* CONFIG_KSU_SUSFS_SUS_PATH */

#ifdef CONFIG_KSU_SUSFS_SUS_MOUNT
	if (arg2 == CMD_SUSFS_ADD_SUS_MOUNT) {
		int error;

		if (!susfs_access_ok((void __user *)arg3, sizeof(struct st_susfs_sus_mount))) {
			pr_err("susfs: CMD_SUSFS_ADD_SUS_MOUNT -> arg3 is not accessible\n");
			return true;
		}
		error = susfs_add_sus_mount((struct st_susfs_sus_mount __user *)arg3);
		pr_info("susfs: CMD_SUSFS_ADD_SUS_MOUNT -> ret: %d\n", error);
		susfs_put_error(arg5, error);
		return true;
	}
#endif /* CONFIG_KSU_SUSFS_SUS_MOUNT */

#ifdef CONFIG_KSU_SUSFS_SUS_KSTAT
	if (arg2 == CMD_SUSFS_ADD_SUS_KSTAT) {
		int error;

		if (!susfs_access_ok((void __user *)arg3, sizeof(struct st_susfs_sus_kstat))) {
			pr_err("susfs: CMD_SUSFS_ADD_SUS_KSTAT -> arg3 is not accessible\n");
			return true;
		}
		error = susfs_add_sus_kstat((struct st_susfs_sus_kstat __user *)arg3);
		pr_info("susfs: CMD_SUSFS_ADD_SUS_KSTAT -> ret: %d\n", error);
		susfs_put_error(arg5, error);
		return true;
	}
	if (arg2 == CMD_SUSFS_UPDATE_SUS_KSTAT) {
		int error;

		if (!susfs_access_ok((void __user *)arg3, sizeof(struct st_susfs_sus_kstat))) {
			pr_err("susfs: CMD_SUSFS_UPDATE_SUS_KSTAT -> arg3 is not accessible\n");
			return true;
		}
		error = susfs_update_sus_kstat((struct st_susfs_sus_kstat __user *)arg3);
		pr_info("susfs: CMD_SUSFS_UPDATE_SUS_KSTAT -> ret: %d\n", error);
		susfs_put_error(arg5, error);
		return true;
	}
	if (arg2 == CMD_SUSFS_ADD_SUS_KSTAT_STATICALLY) {
		int error;

		if (!susfs_access_ok((void __user *)arg3, sizeof(struct st_susfs_sus_kstat))) {
			pr_err("susfs: CMD_SUSFS_ADD_SUS_KSTAT_STATICALLY -> arg3 is not accessible\n");
			return true;
		}
		error = susfs_add_sus_kstat((struct st_susfs_sus_kstat __user *)arg3);
		pr_info("susfs: CMD_SUSFS_ADD_SUS_KSTAT_STATICALLY -> ret: %d\n", error);
		susfs_put_error(arg5, error);
		return true;
	}
#endif /* CONFIG_KSU_SUSFS_SUS_KSTAT */

#ifdef CONFIG_KSU_SUSFS_TRY_UMOUNT
	if (arg2 == CMD_SUSFS_ADD_TRY_UMOUNT) {
		int error;

		if (!susfs_access_ok((void __user *)arg3, sizeof(struct st_susfs_try_umount))) {
			pr_err("susfs: CMD_SUSFS_ADD_TRY_UMOUNT -> arg3 is not accessible\n");
			return true;
		}
		error = susfs_add_try_umount((struct st_susfs_try_umount __user *)arg3);
		pr_info("susfs: CMD_SUSFS_ADD_TRY_UMOUNT -> ret: %d\n", error);
		susfs_put_error(arg5, error);
		return true;
	}
	if (arg2 == CMD_SUSFS_RUN_UMOUNT_FOR_CURRENT_MNT_NS) {
		susfs_run_try_umount_for_current_mnt_ns();
		pr_info("susfs: CMD_SUSFS_RUN_UMOUNT_FOR_CURRENT_MNT_NS -> ret: 0\n");
		susfs_put_error(arg5, 0);
		return true;
	}
#endif /* CONFIG_KSU_SUSFS_TRY_UMOUNT */

#ifdef CONFIG_KSU_SUSFS_SPOOF_UNAME
	if (arg2 == CMD_SUSFS_SET_UNAME) {
		int error;

		if (!susfs_access_ok((void __user *)arg3, sizeof(struct st_susfs_uname))) {
			pr_err("susfs: CMD_SUSFS_SET_UNAME -> arg3 is not accessible\n");
			return true;
		}
		error = susfs_set_uname((struct st_susfs_uname __user *)arg3);
		pr_info("susfs: CMD_SUSFS_SET_UNAME -> ret: %d\n", error);
		susfs_put_error(arg5, error);
		return true;
	}
#endif /* CONFIG_KSU_SUSFS_SPOOF_UNAME */

#ifdef CONFIG_KSU_SUSFS_ENABLE_LOG
	if (arg2 == CMD_SUSFS_ENABLE_LOG) {
		if (arg3 != 0 && arg3 != 1) {
			pr_err("susfs: CMD_SUSFS_ENABLE_LOG -> arg3 can only be 0 or 1\n");
			return true;
		}
		susfs_set_log(arg3);
		susfs_put_error(arg5, 0);
		return true;
	}
#endif /* CONFIG_KSU_SUSFS_ENABLE_LOG */

#ifdef CONFIG_KSU_SUSFS_SPOOF_CMDLINE_OR_BOOTCONFIG
	if (arg2 == CMD_SUSFS_SET_CMDLINE_OR_BOOTCONFIG) {
		int error;

		if (!susfs_access_ok((void __user *)arg3, SUSFS_FAKE_CMDLINE_OR_BOOTCONFIG_SIZE)) {
			pr_err("susfs: CMD_SUSFS_SET_CMDLINE_OR_BOOTCONFIG -> arg3 is not accessible\n");
			return true;
		}
		error = susfs_set_cmdline_or_bootconfig((char __user *)arg3);
		pr_info("susfs: CMD_SUSFS_SET_CMDLINE_OR_BOOTCONFIG -> ret: %d\n", error);
		susfs_put_error(arg5, error);
		return true;
	}
#endif /* CONFIG_KSU_SUSFS_SPOOF_CMDLINE_OR_BOOTCONFIG */

#ifdef CONFIG_KSU_SUSFS_OPEN_REDIRECT
	if (arg2 == CMD_SUSFS_ADD_OPEN_REDIRECT) {
		int error;

		if (!susfs_access_ok((void __user *)arg3, sizeof(struct st_susfs_open_redirect))) {
			pr_err("susfs: CMD_SUSFS_ADD_OPEN_REDIRECT -> arg3 is not accessible\n");
			return true;
		}
		error = susfs_add_open_redirect((struct st_susfs_open_redirect __user *)arg3);
		pr_info("susfs: CMD_SUSFS_ADD_OPEN_REDIRECT -> ret: %d\n", error);
		susfs_put_error(arg5, error);
		return true;
	}
#endif /* CONFIG_KSU_SUSFS_OPEN_REDIRECT */

	if (arg2 == CMD_SUSFS_SHOW_VERSION) {
		int error;
		int len = strlen(SUSFS_VERSION);

		if (!susfs_access_ok((void __user *)arg3, len + 1)) {
			pr_err("susfs: CMD_SUSFS_SHOW_VERSION -> arg3 is not accessible\n");
			return true;
		}
		error = copy_to_user((void __user *)arg3, SUSFS_VERSION, len + 1);
		pr_info("susfs: CMD_SUSFS_SHOW_VERSION -> ret: %d\n", error);
		susfs_put_error(arg5, error);
		return true;
	}
	if (arg2 == CMD_SUSFS_SHOW_ENABLED_FEATURES) {
		int error;
		u64 enabled_features = 0;

		if (!susfs_access_ok((void __user *)arg3, sizeof(u64))) {
			pr_err("susfs: CMD_SUSFS_SHOW_ENABLED_FEATURES -> arg3 is not accessible\n");
			return true;
		}
#ifdef CONFIG_KSU_SUSFS_SUS_PATH
		enabled_features |= (1 << 0);
#endif
#ifdef CONFIG_KSU_SUSFS_SUS_MOUNT
		enabled_features |= (1 << 1);
#endif
#ifdef CONFIG_KSU_SUSFS_AUTO_ADD_SUS_KSU_DEFAULT_MOUNT
		enabled_features |= (1 << 2);
#endif
#ifdef CONFIG_KSU_SUSFS_AUTO_ADD_SUS_BIND_MOUNT
		enabled_features |= (1 << 3);
#endif
#ifdef CONFIG_KSU_SUSFS_SUS_KSTAT
		enabled_features |= (1 << 4);
#endif
#ifdef CONFIG_KSU_SUSFS_SUS_OVERLAYFS
		enabled_features |= (1 << 5);
#endif
#ifdef CONFIG_KSU_SUSFS_TRY_UMOUNT
		enabled_features |= (1 << 6);
#endif
#ifdef CONFIG_KSU_SUSFS_AUTO_ADD_TRY_UMOUNT_FOR_BIND_MOUNT
		enabled_features |= (1 << 7);
#endif
#ifdef CONFIG_KSU_SUSFS_SPOOF_UNAME
		enabled_features |= (1 << 8);
#endif
#ifdef CONFIG_KSU_SUSFS_ENABLE_LOG
		enabled_features |= (1 << 9);
#endif
#ifdef CONFIG_KSU_SUSFS_HIDE_KSU_SUSFS_SYMBOLS
		enabled_features |= (1 << 10);
#endif
#ifdef CONFIG_KSU_SUSFS_SPOOF_CMDLINE_OR_BOOTCONFIG
		enabled_features |= (1 << 11);
#endif
#ifdef CONFIG_KSU_SUSFS_OPEN_REDIRECT
		enabled_features |= (1 << 12);
#endif
		error = copy_to_user((void __user *)arg3, &enabled_features,
				     sizeof(enabled_features));
		pr_info("susfs: CMD_SUSFS_SHOW_ENABLED_FEATURES -> ret: %d\n", error);
		susfs_put_error(arg5, error);
		return true;
	}
	if (arg2 == CMD_SUSFS_SHOW_VARIANT) {
		int error;
		int len = strlen(SUSFS_VARIANT);

		if (!susfs_access_ok((void __user *)arg3, len + 1)) {
			pr_err("susfs: CMD_SUSFS_SHOW_VARIANT -> arg3 is not accessible\n");
			return true;
		}
		error = copy_to_user((void __user *)arg3, SUSFS_VARIANT, len + 1);
		pr_info("susfs: CMD_SUSFS_SHOW_VARIANT -> ret: %d\n", error);
		susfs_put_error(arg5, error);
		return true;
	}

	/* unknown SUSFS command, let the prctl syscall report -EINVAL */
	return false;
}

static int __init susfs_kernel_init(void)
{
	/* only sets up locks and the spoofed uname, safe to run early */
	susfs_init();
	return 0;
}
fs_initcall(susfs_kernel_init);

#endif /* CONFIG_KSU_SUSFS */
