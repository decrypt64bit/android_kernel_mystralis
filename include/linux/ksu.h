/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Declarations for the KernelSU-Next manual hook entry points.
 *
 * KSUN v3.4.0 intercepts syscalls with either kprobes (KSU_KPROBES_HOOK) or
 * syscall table patching (KSU_SYSCALL_TABLE_HOOK). Neither works on 4.9:
 * the kprobe path additionally compiles the whole LSM hook framework out
 * (hook/lsm_hooks.c is guarded by #ifndef KSU_KPROBES_HOOK), which leaves the
 * manager with no way to talk to the driver, and the syscall table hook needs
 * 4.17+.
 *
 * CONFIG_KSU_MANUAL_HOOK keeps the LSM hooks (which register natively through
 * the LSM framework and are what actually grant root on old kernels) and
 * requires explicit call sites for the syscall side. These are the handlers
 * those call sites must use. Prototypes mirror the driver headers in
 * drivers/kernelsu/ and must stay in sync with them.
 */

#ifndef _LINUX_KSU_H
#define _LINUX_KSU_H

#include <linux/types.h>

#ifdef CONFIG_KSU_MANUAL_HOOK

struct filename;

/*
 * The KSU install magic pair, from uapi/supercall.h in the driver. A call to
 * reboot() with magic1/magic2 set to these values is how the manager asks the
 * driver for the anon inode fd that every supercall travels over. Must stay in
 * sync with the driver's uapi.
 */
#define KSU_INSTALL_MAGIC1 0xDEADBEEF
#define KSU_INSTALL_MAGIC2 0xCAFEBABE

#ifdef __KERNEL__
/* feature/sucompat.h */
extern int ksu_handle_faccessat(int *dfd, const char __user **filename_user,
				int *mode, int *flags);
extern int ksu_handle_stat(int *dfd, const char __user **filename_user,
			   int *flags);

/* core/init.c: dispatches to both the sucompat and ksud execveat handlers */
extern int ksu_handle_execveat(int *fd, struct filename **filename_ptr,
			       void *argv, void *envp, int *flags);

/* hook/setuid_hook.h: also drives ksu_handle_umount() internally */
extern int ksu_handle_setresuid(uid_t old_uid, uid_t new_uid);

/* feature/kernel_umount.h, kept for direct call sites */
extern int ksu_handle_umount(uid_t old_uid, uid_t new_uid);

/* supercall/supercall.c: how the manager obtains its anon inode fd */
extern int ksu_handle_sys_reboot(int magic1, int magic2, unsigned int cmd,
				 void __user **arg);

/*
 * runtime/boot_event.c: sets ksu_boot_completed and calls track_throne(),
 * which is what crowns the manager (ksu_set_manager_appid). Normally this
 * only runs from the REPORT_EVENT supercall, which is root-only.
 */
void on_boot_completed(void);

/* Driver globals, for the /proc/ksu_status diagnostic in kernel/sys.c's
 * sibling ksu_status.c. */
extern bool ksu_boot_completed;
extern bool ksu_late_loaded;
extern uid_t ksu_manager_appid;
#endif /* __KERNEL__ */

#endif /* CONFIG_KSU_MANUAL_HOOK */

#endif /* _LINUX_KSU_H */
