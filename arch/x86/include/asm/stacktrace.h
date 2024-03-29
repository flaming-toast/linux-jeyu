/*
 *  Copyright (C) 1991, 1992  Linus Torvalds
 *  Copyright (C) 2000, 2001, 2002 Andi Kleen, SuSE Labs
 */

#ifndef _ASM_X86_STACKTRACE_H
#define _ASM_X86_STACKTRACE_H

#include <linux/uaccess.h>
#include <linux/ptrace.h>
#include <asm/switch_to.h>

extern int kstack_depth_to_print;

struct thread_info;
struct stacktrace_ops;

typedef unsigned long (*walk_stack_t)(struct task_struct *task,
				      unsigned long *stack,
				      unsigned long bp,
				      const struct stacktrace_ops *ops,
				      void *data,
				      unsigned long *end,
				      int *graph);

extern unsigned long
print_context_stack(struct task_struct *task,
		    unsigned long *stack, unsigned long bp,
		    const struct stacktrace_ops *ops, void *data,
		    unsigned long *end, int *graph);

extern unsigned long
print_context_stack_bp(struct task_struct *task,
		       unsigned long *stack, unsigned long bp,
		       const struct stacktrace_ops *ops, void *data,
		       unsigned long *end, int *graph);

/* Generic stack tracer with callbacks */

struct stacktrace_ops {
	int (*address)(void *data, unsigned long address, int reliable);
	/* On negative return stop dumping */
	int (*stack)(void *data, char *name);
	walk_stack_t	walk_stack;
};

void dump_trace(struct task_struct *tsk, struct pt_regs *regs,
		unsigned long *stack, unsigned long bp,
		const struct stacktrace_ops *ops, void *data);

#ifdef CONFIG_X86_32
#define STACKSLOTS_PER_LINE 8
#else
#define STACKSLOTS_PER_LINE 4
#endif

#ifdef CONFIG_FRAME_POINTER
static inline unsigned long *
get_frame_pointer(struct task_struct *task, struct pt_regs *regs)
{
	if (regs)
		return (unsigned long *)regs->bp;

	if (!task || task == current)
		return __builtin_frame_address(0);

	return (unsigned long *)((struct inactive_task_frame *)task->thread.sp)->bp;
}
#else
static inline unsigned long *
get_frame_pointer(struct task_struct *task, struct pt_regs *regs)
{
	return NULL;
}
#endif /* CONFIG_FRAME_POINTER */

static inline unsigned long *
get_stack_pointer(struct task_struct *task, struct pt_regs *regs)
{
	if (regs)
		return (unsigned long *)kernel_stack_pointer(regs);

	if (!task || task == current)
		return __builtin_frame_address(0);

	return (unsigned long *)task->thread.sp;
}

extern void
show_trace_log_lvl(struct task_struct *task, struct pt_regs *regs,
		   unsigned long *stack, unsigned long bp, char *log_lvl);

extern void
show_stack_log_lvl(struct task_struct *task, struct pt_regs *regs,
		   unsigned long *sp, unsigned long bp, char *log_lvl);

extern unsigned int code_bytes;

/* The form of the top of the frame on the stack */
struct stack_frame {
	struct stack_frame *next_frame;
	unsigned long return_address;
};

struct stack_frame_ia32 {
    u32 next_frame;
    u32 return_address;
};

static inline unsigned long caller_frame_pointer(void)
{
	struct stack_frame *frame;

	frame = __builtin_frame_address(0);

#ifdef CONFIG_FRAME_POINTER
	frame = frame->next_frame;
#endif

	return (unsigned long)frame;
}

#endif /* _ASM_X86_STACKTRACE_H */
