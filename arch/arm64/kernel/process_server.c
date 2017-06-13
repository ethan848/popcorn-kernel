/*
 * File:
 * 	process_server.c
 *
 * Description:
 * 	Helper functionality of the process server
 *
 * Created on:
 * 	Sep 19, 2014
 *
 * Author:
 * 	Ajithchandra Saya, SSRG, VirginiaTech
 * 	Sang-Hoon Kim, SSRG, Virginia Tech
 *
 */

/* File includes */
#include <linux/sched.h>
#include <linux/uaccess.h>
#include <linux/cpu_namespace.h>
#include <linux/ptrace.h>

#include <asm/compat.h>

#include <popcorn/regset.h>
#include <popcorn/debug.h>

/*
 * Function:
 *		save_thread_info
 *
 * Description:
 *		this function saves the architecture specific info of the task
 *		to the struct field_arch structure passed
 *
 * Input:
 *	task,	pointer to the task structure of the task of which the
 *			architecture specific info needs to be saved
 *
 *	regs,	pointer to the pt_regs field of the task
 *
 * Output:
 *	arch,	pointer to the struct field_arch structure type where the
 *			architecture specific information of the task has to be
 *			saved
 *
 * Return value:
 *	on success, returns 0
 * 	on failure, returns negative integer
 */
int save_thread_info(struct task_struct *task, struct field_arch *arch)
{
	int cpu;

	cpu = get_cpu();

	arch->tls = task->thread.tp_value;

	put_cpu();

	PSPRINTK(KERN_INFO"%s [%d]: tls: %lx\n", __func__, task->pid, arch->tls);

	return 0;
}

/*
 * Function:
 *		restore_thread_info
 *
 * Description:
 *		this function restores the architecture specific info of the
 *		task from the struct field_arch structure passed
 *
 * Input:
 * 	task,	pointer to the task structure of the task of which the
 * 			architecture specific info needs to be restored
 *
 * 	arch,	pointer to the struct field_arch structure type from which the
 *			architecture specific information of the task has to be
 *			restored
 *
 * Output:
 *	none
 *
 * Return value:
 *	on success, returns 0
 * 	on failure, returns negative integer
 */
int restore_thread_info(struct task_struct *task, struct field_arch *arch, bool restore_segments)
{
	struct pt_regs *regs = task_pt_regs(task);
	struct regset_aarch64 *regset = &arch->regs_aarch;
	int cpu, i;

	BUG_ON(restore_segments && current != task);

	cpu = get_cpu();

	regs->sp = regset->sp;
	regs->pc = regset->pc;
	regs->pstate = PSR_MODE_EL0t;

	for (i = 0; i < 31; i++)
		regs->regs[i] =  regset->x[i];

	if (restore_segments) {
		unsigned long tpidr, tpidrro;

		*task_user_tls(task) = arch->tls;

		tpidr = *task_user_tls(task);
		tpidrro = is_compat_thread(task_thread_info(task)) ?
			task->thread.tp_value : 0;
		asm("msr tpidr_el0, %0;"
			"msr tpidrro_el0, %1;"
			: : "r" (tpidr), "r" (tpidrro));
	}

	put_cpu();

	PSPRINTK(KERN_INFO"%s [%d]: pc %llx sp %llx tls %lx\n", __func__, task->pid,
			regs->pc, regs->sp, *task_user_tls(task));
	show_regs(regs);

	return 0;
}


/*
 * Function:
 *		dump_processor_regs
 *
 * Description:
 *		this function prints the architecture specific registers specified
 *		in the input argument
 *
 * Input:
 * 	task,	pointer to the architecture specific registers
 *
 * Output:
 * 	none
 *
 * Return value:
 *	on success, returns 0
 * 	on failure, returns negative integer
 */
int dump_processor_regs(struct pt_regs *regs)
{
	int i;

	if (regs == NULL) {
		printk(KERN_ERR"process_server: invalid params to dump_processor_regs()");
		return 0;
	}

	dump_stack();

	printk("DUMP REGS %s\n", __func__);

	if (NULL != regs) {
		printk("sp: 0x%llx\n", regs->sp);
		printk("pc: 0x%llx\n", regs->pc);
		printk("pstate: 0x%llx\n", regs->pstate);

		for (i = 0; i < 31; i++) {
			printk("regs[%d]: 0x%llx\n", i, regs->regs[i]);
		}
	}

	return 0;
}

unsigned long futex_atomic_add(unsigned long ptr, unsigned long val)
{
	atomic64_t v;
	unsigned long result;
	v.counter = ptr;

	result = atomic64_add_return(val, &v);
	return (result - val);
}
