#ifndef __KERNEL_POPCORN_PROCESS_SERVER_H__
#define __KERNEL_POPCORN_PROCESS_SERVER_H__

struct task_struct;
struct field_arch;

extern int save_thread_info(struct task_struct *task, struct field_arch *arch);
extern int restore_thread_info(struct task_struct *task, struct field_arch *arch, bool restore_segments);

#endif
