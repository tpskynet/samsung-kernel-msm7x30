/* drivers/misc/lowmemorykiller.c
 *
 * The lowmemorykiller driver lets user-space specify a set of memory thresholds
 * where processes with a range of oom_score_adj values will get killed. Specify
 * the minimum oom_score_adj values in
 * /sys/module/lowmemorykiller/parameters/adj and the number of free pages in
 * /sys/module/lowmemorykiller/parameters/minfree. Both files take a comma
 * separated list of numbers in ascending order.
 *
 * For example, write "0,8" to /sys/module/lowmemorykiller/parameters/adj and
 * "1024,4096" to /sys/module/lowmemorykiller/parameters/minfree to kill
 * processes with a oom_score_adj value of 8 or higher when the free memory
 * drops below 4096 pages and kill processes with a oom_score_adj value of 0 or
 * higher when the free memory drops below 1024 pages.
 *
 * The driver considers memory used for caches to be free, but if a large
 * percentage of the cached memory is locked this can be very inaccurate
 * and processes may not get killed until the normal oom killer is triggered.
 *
 * Copyright (C) 2007-2008 Google, Inc.
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

#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/mm.h>
#include <linux/oom.h>
#include <linux/sched.h>
#include <linux/rcupdate.h>
#include <linux/notifier.h>
#include <linux/mutex.h>
#include <linux/delay.h>
#ifdef CONFIG_ANDROID_LOW_MEMORY_KILLER_DO_NOT_KILL_PROCESS
#include <linux/string.h>
#endif

static uint32_t lowmem_debug_level = 1;
static int lowmem_adj[6] = {
	0,
	1,
	6,
	12,
};
static int lowmem_adj_size = 4;
static int lowmem_minfree[6] = {
	3 * 512,	/* 6MB */
	2 * 1024,	/* 8MB */
	4 * 1024,	/* 16MB */
	16 * 1024,	/* 64MB */
};
static int lowmem_minfree_size = 4;

static unsigned long lowmem_deathpending_timeout;

#ifdef CONFIG_ANDROID_LOW_MEMORY_KILLER_DO_NOT_KILL_PROCESS
#define MAX_NOT_KILLABLE_PROCESSES	25	/* Max number of not killable processes */
#define MANAGED_PROCESS_TYPES		3	/* Numer of managed process types (lowmem_process_type) */

/*
 * Enumerator for the managed process types
 */
enum lowmem_process_type {
	KILLABLE_PROCESS,
	DO_NOT_KILL_PROCESS,
	DO_NOT_KILL_SYSTEM_PROCESS
};

/*
 * Data struct for the management of not killable processes
 */
struct donotkill {
	uint enabled;
	char *names[MAX_NOT_KILLABLE_PROCESSES];
	int names_count;
};

static struct donotkill donotkill_proc;		/* User processes to preserve from killing */
static struct donotkill donotkill_sysproc;	/* System processes to preserve from killing */

/*
 * Checks if a process name is inside a list of processes to be preserved from killing
 */
static bool is_in_donotkill_list(char *proc_name, struct donotkill *donotkill_proc)
{
	int i = 0;

	/* If the do not kill feature is enabled and the process names to be preserved
	 * is not empty, then check if the passed process name is contained inside it */
	if (donotkill_proc->enabled && donotkill_proc->names_count > 0) {
		for (i = 0; i < donotkill_proc->names_count; i++) {
			if (strstr(donotkill_proc->names[i], proc_name) != NULL)
				return true; /* The process must be preserved from killing */
		}
	}

	return false; /* The process is not contained inside the process names list */
}

/*
 * Checks if a process name is inside a list of user processes to be preserved from killing
 */
static bool is_in_donotkill_proc_list(char *proc_name)
{
	return is_in_donotkill_list(proc_name, &donotkill_proc);
}

/*
 * Checks if a process name is inside a list of system processes to be preserved from killing
 */
static bool is_in_donotkill_sysproc_list(char *proc_name)
{
	return is_in_donotkill_list(proc_name, &donotkill_sysproc);
}
#else
#define MANAGED_PROCESS_TYPES		1	/* Numer of managed process types (lowmem_process_type) */

/*
 * Enumerator for the managed process types
 */
enum lowmem_process_type {
	KILLABLE_PROCESS
};
#endif

#define lowmem_print(level, x...)			\
	do {						\
		if (lowmem_debug_level >= (level))	\
			printk(x);			\
	} while (0)

static int test_task_flag(struct task_struct *p, int flag)
{
	struct task_struct *t = p;

	do {
		task_lock(t);
		if (test_tsk_thread_flag(t, flag)) {
			task_unlock(t);
			return 1;
		}
		task_unlock(t);
	} while_each_thread(p, t);

	return 0;
}

int can_use_cma_pages(gfp_t gfp_mask)
{
	int can_use = 0;
	int mtype = allocflags_to_migratetype(gfp_mask);
	int i = 0;
	int *mtype_fallbacks = get_migratetype_fallbacks(mtype);

	if (is_migrate_cma(mtype)) {
		can_use = 1;
	} else {
		for (i = 0;; i++) {
			int fallbacktype = mtype_fallbacks[i];

			if (is_migrate_cma(fallbacktype)) {
				can_use = 1;
				break;
			}

			if (fallbacktype == MIGRATE_RESERVE)
				break;
		}
	}
	return can_use;
}

static DEFINE_MUTEX(scan_mutex);

static int lowmem_shrink(struct shrinker *s, struct shrink_control *sc)
{
	struct task_struct *tsk;
	struct task_struct *selected[MANAGED_PROCESS_TYPES] = {NULL};
	int rem = 0;
	int tasksize;
	int i;
	int min_score_adj = OOM_SCORE_ADJ_MAX + 1;
	enum lowmem_process_type proc_type = KILLABLE_PROCESS;
	int selected_tasksize[MANAGED_PROCESS_TYPES] = {0};
	int selected_oom_score_adj[MANAGED_PROCESS_TYPES];
	int array_size = ARRAY_SIZE(lowmem_adj);
	int other_free;
	int other_file;
	unsigned long nr_to_scan = sc->nr_to_scan;

	if (nr_to_scan > 0) {
		if (mutex_lock_interruptible(&scan_mutex) < 0)
			return 0;
	}

	other_free = global_page_state(NR_FREE_PAGES);
	other_file = global_page_state(NR_FILE_PAGES) -
						global_page_state(NR_SHMEM);

	if (lowmem_adj_size < array_size)
		array_size = lowmem_adj_size;
	if (lowmem_minfree_size < array_size)
		array_size = lowmem_minfree_size;
	for (i = 0; i < array_size; i++) {
		if (other_free < lowmem_minfree[i] &&
		    other_file < lowmem_minfree[i]) {
			min_score_adj = lowmem_adj[i];
			break;
		}
	}
	if (nr_to_scan > 0)
		lowmem_print(3, "lowmem_shrink %lu, %x, ofree %d %d, ma %d\n",
				nr_to_scan, sc->gfp_mask, other_free,
				other_file, min_score_adj);
	rem = global_page_state(NR_ACTIVE_ANON) +
		global_page_state(NR_ACTIVE_FILE) +
		global_page_state(NR_INACTIVE_ANON) +
		global_page_state(NR_INACTIVE_FILE);
	if (nr_to_scan <= 0 || min_score_adj == OOM_SCORE_ADJ_MAX + 1) {
		lowmem_print(5, "lowmem_shrink %lu, %x, return %d\n",
			     nr_to_scan, sc->gfp_mask, rem);

		if (nr_to_scan > 0)
			mutex_unlock(&scan_mutex);

		return rem;
	}

	/* Set the initial oom_score_adj for each managed process type */
	for (proc_type = KILLABLE_PROCESS; proc_type < MANAGED_PROCESS_TYPES; proc_type++)
		selected_oom_score_adj[proc_type] = min_score_adj;

	rcu_read_lock();
	for_each_process(tsk) {
		struct task_struct *p;
		int oom_score_adj;

		if (tsk->flags & PF_KTHREAD)
			continue;

		/* if task no longer has any memory ignore it */
		if (test_task_flag(tsk, TIF_MM_RELEASED))
			continue;

		if (time_before_eq(jiffies, lowmem_deathpending_timeout)) {
			if (test_task_flag(tsk, TIF_MEMDIE)) {
				rcu_read_unlock();
				/* give the system time to free up the memory */
				msleep_interruptible(20);
				mutex_unlock(&scan_mutex);
				return 0;
			}
		}

		p = find_lock_task_mm(tsk);
		if (!p)
			continue;

		oom_score_adj = p->signal->oom_score_adj;
		if (oom_score_adj < min_score_adj) {
			task_unlock(p);
			continue;
		}
		tasksize = get_mm_rss(p->mm);
		task_unlock(p);
		if (tasksize <= 0)
			continue;

		/* Initially consider the process as killable */
		proc_type = KILLABLE_PROCESS;

#ifdef CONFIG_ANDROID_LOW_MEMORY_KILLER_DO_NOT_KILL_PROCESS
		/* Check if the process name is contained inside the process to be preserved lists */
		if (is_in_donotkill_proc_list(p->comm)) {
			/* This user process must be preserved from killing */
			proc_type = DO_NOT_KILL_PROCESS;
			lowmem_print(2, "The process '%s' is inside the donotkill_proc_names", p->comm);
		} else if (is_in_donotkill_sysproc_list(p->comm)) {
			/* This system process must be preserved from killing */
			proc_type = DO_NOT_KILL_SYSTEM_PROCESS;
			lowmem_print(2, "The process '%s' is inside the donotkill_sysproc_names", p->comm);
		}
#endif

		if (selected[proc_type]) {
			if (oom_score_adj < selected_oom_score_adj[proc_type])
				continue;
			if (oom_score_adj == selected_oom_score_adj[proc_type] &&
			    tasksize <= selected_tasksize[proc_type])
				continue;
		}
		selected[proc_type] = p;
		selected_tasksize[proc_type] = tasksize;
		selected_oom_score_adj[proc_type] = oom_score_adj;
		lowmem_print(2, "select %d (%s), adj %d, size %d, to kill\n",
			     p->pid, p->comm, oom_score_adj, tasksize);
	}
	if (selected[proc_type]) {
		lowmem_print(1, "send sigkill to %d (%s), adj %d, size %d\n",
			     selected[proc_type]->pid, selected[proc_type]->comm,
			     selected_oom_score_adj[proc_type], selected_tasksize[proc_type]);
		lowmem_deathpending_timeout = jiffies + HZ;
		send_sig(SIGKILL, selected[proc_type], 0);
		set_tsk_thread_flag(selected[proc_type], TIF_MEMDIE);
		rem -= selected_tasksize[proc_type];
		rcu_read_unlock();
		/* give the system time to free up the memory */
		msleep_interruptible(20);
	} else
		rcu_read_unlock();

	lowmem_print(4, "lowmem_shrink %lu, %x, return %d\n",
		     nr_to_scan, sc->gfp_mask, rem);
	mutex_unlock(&scan_mutex);
	return rem;
}

static struct shrinker lowmem_shrinker = {
	.shrink = lowmem_shrink,
	.seeks = DEFAULT_SEEKS * 16
};

static int __init lowmem_init(void)
{
	register_shrinker(&lowmem_shrinker);
	return 0;
}

static void __exit lowmem_exit(void)
{
	unregister_shrinker(&lowmem_shrinker);
}

#ifdef CONFIG_ANDROID_LOW_MEMORY_KILLER_AUTODETECT_OOM_ADJ_VALUES
static int lowmem_oom_adj_to_oom_score_adj(int oom_adj)
{
	if (oom_adj == OOM_ADJUST_MAX)
		return OOM_SCORE_ADJ_MAX;
	else
		return (oom_adj * OOM_SCORE_ADJ_MAX) / -OOM_DISABLE;
}

static void lowmem_autodetect_oom_adj_values(void)
{
	int i;
	int oom_adj;
	int oom_score_adj;
	int array_size = ARRAY_SIZE(lowmem_adj);

	if (lowmem_adj_size < array_size)
		array_size = lowmem_adj_size;

	if (array_size <= 0)
		return;

	oom_adj = lowmem_adj[array_size - 1];
	if (oom_adj > OOM_ADJUST_MAX)
		return;

	oom_score_adj = lowmem_oom_adj_to_oom_score_adj(oom_adj);
	if (oom_score_adj <= OOM_ADJUST_MAX)
		return;

	lowmem_print(1, "lowmem_shrink: convert oom_adj to oom_score_adj:\n");
	for (i = 0; i < array_size; i++) {
		oom_adj = lowmem_adj[i];
		oom_score_adj = lowmem_oom_adj_to_oom_score_adj(oom_adj);
		lowmem_adj[i] = oom_score_adj;
		lowmem_print(1, "oom_adj %d => oom_score_adj %d\n",
			     oom_adj, oom_score_adj);
	}
}

static int lowmem_adj_array_set(const char *val, const struct kernel_param *kp)
{
	int ret;

	ret = param_array_ops.set(val, kp);

	/* HACK: Autodetect oom_adj values in lowmem_adj array */
	lowmem_autodetect_oom_adj_values();

	return ret;
}

static int lowmem_adj_array_get(char *buffer, const struct kernel_param *kp)
{
	return param_array_ops.get(buffer, kp);
}

static void lowmem_adj_array_free(void *arg)
{
	param_array_ops.free(arg);
}

static struct kernel_param_ops lowmem_adj_array_ops = {
	.set = lowmem_adj_array_set,
	.get = lowmem_adj_array_get,
	.free = lowmem_adj_array_free,
};

static const struct kparam_array __param_arr_adj = {
	.max = ARRAY_SIZE(lowmem_adj),
	.num = &lowmem_adj_size,
	.ops = &param_ops_int,
	.elemsize = sizeof(lowmem_adj[0]),
	.elem = lowmem_adj,
};
#endif

module_param_named(cost, lowmem_shrinker.seeks, int, S_IRUGO | S_IWUSR);
#ifdef CONFIG_ANDROID_LOW_MEMORY_KILLER_AUTODETECT_OOM_ADJ_VALUES
__module_param_call(MODULE_PARAM_PREFIX, adj,
		    &lowmem_adj_array_ops,
		    .arr = &__param_arr_adj,
		    S_IRUGO | S_IWUSR, -1);
__MODULE_PARM_TYPE(adj, "array of int");
#else
module_param_array_named(adj, lowmem_adj, int, &lowmem_adj_size,
			 S_IRUGO | S_IWUSR);
#endif
module_param_array_named(minfree, lowmem_minfree, uint, &lowmem_minfree_size,
			 S_IRUGO | S_IWUSR);
module_param_named(debug_level, lowmem_debug_level, uint, S_IRUGO | S_IWUSR);
#ifdef CONFIG_ANDROID_LOW_MEMORY_KILLER_DO_NOT_KILL_PROCESS
module_param_named(donotkill_proc, donotkill_proc.enabled, uint, S_IRUGO | S_IWUSR);
module_param_array_named(donotkill_proc_names, donotkill_proc.names, charp,
			 &donotkill_proc.names_count, S_IRUGO | S_IWUSR);
module_param_named(donotkill_sysproc, donotkill_sysproc.enabled, uint, S_IRUGO | S_IWUSR);
module_param_array_named(donotkill_sysproc_names, donotkill_sysproc.names, charp,
			 &donotkill_sysproc.names_count, S_IRUGO | S_IWUSR);
#endif
module_init(lowmem_init);
module_exit(lowmem_exit);

MODULE_LICENSE("GPL");

