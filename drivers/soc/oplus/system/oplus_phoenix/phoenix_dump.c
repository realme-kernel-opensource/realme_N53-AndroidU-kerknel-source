// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (C) 2018-2020 Oplus. All rights reserved.
 */
/*
###############################################################################
## File: phoenix_dump.c
## Description : responsible for log management, PHOENIX_PROJECT
##
## Version:  1.0
################################################################################
*/
/* #include <linux/syscalls.h> */
#include "oplus_phoenix.h"
#include <linux/completion.h>
#include <linux/cred.h>
#include <linux/delay.h>
#include <linux/errno.h>
#include <linux/fs.h>
#include <linux/kmsg_dump.h>
#include <linux/kthread.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/printk.h>
#include <linux/reboot.h>
#include <linux/sched/signal.h>
#include <linux/slab.h>
#include <linux/version.h>
//#include <asm-generic/uaccess.h>

#define TASK_INIT_COMM "init"
#define KE_LOG_GENERATE_TIMEOUT msecs_to_jiffies(4 * 500)
#define KMSG_DEV_FILE "/dev/kmsg"
#define KMSG_BUFFER_SIZE (8 * 1024)
#define KEYINFO_FILE_SIZE (8 * 1024)
#define FILE_MODE_0666 0666
#define AID_SYSTEM 1000
#define FILE_DMESG "/data/persist_log/oplusreserve/phoenix/kmsg"
#define PHOENIX_TEMP_DIR "/data/persist_log/oplusreserve/phoenix"
#define FILE_KERNEL_EXPETION_INFO                                              \
	"/data/persist_log/oplusreserve/phoenix/ERROR_KERNEL_EXCEPTION"

#define PHX_KEYINFO_FORMAT                                                     \
	"stage: %s\n\
error: %s\n\
happen_time: %s\n\
is_boot_complete: %d\n"

typedef struct _phx_keyinfo {
	phx_baseinfo *baseinfo;
	int is_system_boot_completed;
	/* TODO: more information */
} phx_keyinfo;

/* Kun.Hu@PSW.TECH.RELIABILTY.1890222, 2019/03/20, remove all fs opreration in kernel, fix extfs crash */
extern int phx_is_system_boot_completed(void);
extern int phx_is_filesystem_ready(void);
extern void phx_get_base_info(phx_baseinfo **);

static DECLARE_COMPLETION(phx_comp);
/* mutex for write file */
static DEFINE_MUTEX(phx_wf_mutex);

static void phx_dump_hang_oplus_log(const char *);
static void phx_dump_kernel_exception_log(const char *);

static struct kmsg_dumper phx_kmsg_dumper;

static phx_action_mapping phx_log_dumper_mapping[] = {
	{ ERROR_HANG_OPLUS, phx_dump_hang_oplus_log },
	{ ERROR_KERNEL_PANIC, phx_dump_kernel_exception_log },
	{ ERROR_HWT, phx_dump_kernel_exception_log },
	/* TODO: dump hw reboot log */
};

static void find_task_by_comm(const char *pcomm, struct task_struct **t_result)
{
	struct task_struct *g, *t;
	rcu_read_lock();
	for_each_process_thread(g, t) {
		if (!strcmp(t->comm, pcomm)) {
			*t_result = t;
			rcu_read_unlock();
			return;
		}
	}
	t_result = NULL;
	rcu_read_unlock();
}

/*
 handle two situation
 1. hang and filesystem ready
 2. hang and filesystem not ready(hang kernel or early native)
*/
static void phx_dump_hang_oplus_log(const char *happen_time)
{
	struct task_struct *t_init;
	t_init = NULL;
	find_task_by_comm(TASK_INIT_COMM, &t_init);
	if (NULL != t_init && phx_is_filesystem_ready()) {
		PHX_KLOG_INFO("send hang oplus signal %d at %s", SIGPHX_HANG,
			      happen_time);
		send_sig_info(SIGPHX_HANG, SEND_SIG_NOINFO, t_init);
		/* native process will handle remaining work */
		schedule_timeout_interruptible(30 * HZ);
	}
	/* incase native error handle process hang or filesystem not ready */
	panic(ERROR_HANG_OPLUS);
}

/* dump kmsg to file or raw block */
static int dump_kmsg(struct file *opfile, size_t offset_of_start,
		     struct kmsg_dumper *kmsg_dumper)
{
	loff_t offset;
	char line[1024] = { 0 };
	size_t len = 0;
	size_t bytes_writed = 0;

	offset = offset_of_start;
	kmsg_dumper->active = true;
	while (kmsg_dump_get_line(kmsg_dumper, true, line, sizeof(line),
				  &len)) {
		line[len] = '\0';
		mutex_lock(&phx_wf_mutex);
		if (len != bytes_writed) {
			PHX_KLOG_ERROR("kernel_write failed, error: %lu\n",
				       bytes_writed);
			mutex_unlock(&phx_wf_mutex);
			return -1;
		}
		mutex_unlock(&phx_wf_mutex);
	}

	return 0;
}

static int phx_store_kmsg(void)
{
	int result = -1;
	struct file *opfile;

	opfile = filp_open(FILE_DMESG, O_CREAT | O_WRONLY | O_TRUNC,
			   FILE_MODE_0666);
	if (IS_ERR(opfile)) {
		PHX_KLOG_ERROR("filp_open failed, error: %ld\n",
			       PTR_ERR(opfile));
		return -1;
	}

	if (dump_kmsg(opfile, 0, &phx_kmsg_dumper)) {
		PHX_KLOG_ERROR("dump kmsg failed\n");
		result = -1;
	}

	vfs_fsync(opfile, 0);
	filp_close(opfile, NULL);

	return result;
}

static void phx_format_keyinfo(const phx_keyinfo *kinfo, char *buffer,
			       int buffer_size)
{
	const phx_baseinfo *binfo = kinfo->baseinfo;
	snprintf(buffer, buffer_size, PHX_KEYINFO_FORMAT, binfo->stage,
		 binfo->error, binfo->happen_time,
		 kinfo->is_system_boot_completed);
}

static int phx_generate_keyinfo(const char *keyinfo_file,
				const phx_keyinfo *kinfo)
{
	//mm_segment_t old_fs;
	struct file *file;
	loff_t file_pos = 0;
	size_t bytes_to_write = 0;
	int result = -1;
	char *buf = NULL;
	//old_fs = get_fs();
	//set_fs(KERNEL_DS);

	file = filp_open(keyinfo_file, O_CREAT | O_WRONLY | O_TRUNC,
			 FILE_MODE_0666);

	if (IS_ERR(file)) {
		PHX_KLOG_ERROR("file_open %s failed, error: %ld\n",
			       keyinfo_file, PTR_ERR(file));
		result = -1;
		goto phx_bail;
	}
	buf = (char *)kmalloc(KEYINFO_FILE_SIZE, GFP_KERNEL);
	if (!buf) {
		PHX_KLOG_ERROR("kmalloc %u bytes failed\n", KEYINFO_FILE_SIZE);
		result = -1;
		goto phx_bail;
	}
	memset(buf, 0, KEYINFO_FILE_SIZE);
	phx_format_keyinfo(kinfo, buf, KEYINFO_FILE_SIZE);
	bytes_to_write = strlen(buf);
	if (!bytes_to_write) {
		result = -1;
		goto phx_bail;
	}
	mutex_lock(&phx_wf_mutex);
	if (strlen(buf) != (bytes_to_write = kernel_write(
				    file, buf, strlen(buf), &file_pos))) {
		PHX_KLOG_ERROR("kernel_write %s failed, error: %lu\n",
			       keyinfo_file, bytes_to_write);
		mutex_unlock(&phx_wf_mutex);
		result = -1;
		goto phx_bail;
	}

	mutex_unlock(&phx_wf_mutex);
	result = 0;

phx_bail:
	//set_fs(old_fs);
	if (!IS_ERR(file)) {
		vfs_fsync(file, 0);
		filp_close(file, NULL);
	}
	if (buf) {
		kfree(buf);
		buf = NULL;
	}
	return result;
}

static int phx_generate_ke_keyinfo(void)
{
	int result = -1;
	phx_keyinfo *kinfo =
		(phx_keyinfo *)kmalloc(sizeof(phx_keyinfo), GFP_KERNEL);
	if (!kinfo) {
		PHX_KLOG_ERROR("kmalloc %lu bytes failed\n",
			       sizeof(phx_keyinfo));
		return result;
	}

	/* creds_change_dac(); */
	/* creds_change_id(); */

	memset(kinfo, 0, sizeof(phx_keyinfo));
	phx_get_base_info(&kinfo->baseinfo);
	kinfo->is_system_boot_completed = phx_is_system_boot_completed();
	result = phx_generate_keyinfo(FILE_KERNEL_EXPETION_INFO, kinfo);
	kfree(kinfo);
	return result;
}

static int phx_collect_ke_log(void *args)
{
	if (0 != phx_generate_ke_keyinfo()) {
		PHX_KLOG_ERROR("generate panic keyinfo failed\n");
		goto phx_bail;
	}

	/* we boot in post-fs-data, save kmsg to file */
	if (phx_store_kmsg()) {
		PHX_KLOG_ERROR("store kmsg failed\n");
		/* goto phx_bail; */
	}

	/* TODO: we have not boot in post-fs-data, save kmsg to raw block */

phx_bail:
	complete(&phx_comp);
	return -1;
}

static void phx_dump_kernel_exception_log(const char *error)
{
	struct task_struct *tsk;
	tsk = kthread_run(phx_collect_ke_log, NULL, "phx_collect_ke_log");
	if (IS_ERR(tsk)) {
		PHX_KLOG_ERROR(
			"create kernel thread phx_collect_ke_log failed\n");
		return;
	}
	if (!wait_for_completion_timeout(&phx_comp, KE_LOG_GENERATE_TIMEOUT)) {
		PHX_KLOG_ERROR("collect kernel exception %s log timeout\n",
			       error);
	}
}

void phx_log_dump(const char *phx_error)
{
	size_t i = 0;
	for (; i < ARRAY_SIZE(phx_log_dumper_mapping); i++) {
		if (!strcmp(phx_log_dumper_mapping[i].action, phx_error)) {
			phx_log_dumper_mapping[i].map_func(phx_error);
			return;
		}
	}
}

EXPORT_SYMBOL(phx_log_dump);
