#ifndef ARM64_STEPDBG_H
#define ARM64_STEPDBG_H

#include <linux/atomic.h>
#include <linux/errno.h>
#include <linux/hashtable.h>
#include <linux/jiffies.h>
#include <linux/kernel.h>
#include <linux/mutex.h>
#include <linux/sched.h>
#include <linux/sched/signal.h>
#include <linux/sched/task_stack.h>
#include <linux/smp.h>
#include <linux/slab.h>
#include <linux/spinlock.h>
#include <linux/thread_info.h>
#include <linux/wait.h>
#include <linux/version.h>
#include <asm/debug-monitors.h>
#include <asm/ptrace.h>
#include <asm/sysreg.h>
#include <asm/thread_info.h>

#include "inline_hook_frame.h"
#include "klog/lsdriver_log.h"

#if LINUX_VERSION_CODE >= KERNEL_VERSION(6, 18, 0)
#include <asm/uprobes.h>
#define SGAME_STEPBP_DIRECT 1
static typeof(&uprobe_single_step_handler) g_stepbp_uprobe_single_step;
#else
#define SGAME_STEPBP_DIRECT 0
#endif

static struct break_point *g_stepbp_info;
static DEFINE_SPINLOCK(g_stepbp_lock);
static DEFINE_SPINLOCK(g_stepbp_hit_lock);
static DEFINE_MUTEX(g_stepbp_mutex);
static bool g_stepbp_stopping;
static unsigned long g_stepbp_generation;
#if !SGAME_STEPBP_DIRECT
static struct list_head *g_stepbp_native_hooks;
static void (*g_stepbp_register_hook)(struct step_hook *hook);
static void (*g_stepbp_unregister_hook)(struct step_hook *hook);
static bool g_stepbp_hook_registered;
#endif

struct stepbp_task_step
{
    struct hlist_node node;
    pid_t tid;
    u64 start_time;
    unsigned long generation;
    unsigned long pc;
    bool valid;
    unsigned int syscall_exit_depth;
};
static DEFINE_HASHTABLE(g_stepbp_task_steps, 8);

/* An interrupt can return to active-pending step state without executing an
 * instruction, producing an ISV=0 trap at the PC already delivered. Track every
 * thread's previous step, so only that replay is skipped. ISV=1 self branches
 * and first arrivals after an instruction with no syndrome still count. */
static struct stepbp_task_step *stepbp_task_state_locked(struct task_struct *task, bool create)
{
    struct stepbp_task_step *state;

    hash_for_each_possible(g_stepbp_task_steps, state, node, (unsigned long)task->pid)
    {
        if (state->tid == task->pid) goto found;
    }
    if (!create) return NULL;
    state = kzalloc(sizeof(*state), GFP_ATOMIC);
    if (!state) return NULL;
    state->tid = task->pid;
    hash_add(g_stepbp_task_steps, &state->node, (unsigned long)state->tid);
found:
    if (state->start_time != task->start_time)
    {
        state->start_time = task->start_time;
        state->valid = false;
        state->syscall_exit_depth = 0;
    }
    return state;
}

static bool stepbp_task_exiting_syscall_locked(struct task_struct *task)
{
    struct stepbp_task_step *state = stepbp_task_state_locked(task, false);

    return state && state->syscall_exit_depth;
}

static bool stepbp_replayed_pc_locked(struct pt_regs *regs, unsigned long esr, int native_result)
{
    struct stepbp_task_step *state = stepbp_task_state_locked(current, true);
    bool replay;

    if (!state) return false;
    replay = state->valid && state->generation == g_stepbp_generation && state->pc == regs->pc && !(esr & BIT(24));
    state->generation = g_stepbp_generation;
    state->pc = regs->pc;
    state->valid = native_result != DBG_HOOK_HANDLED;
    return replay;
}

static void stepbp_free_task_steps(void)
{
    struct stepbp_task_step *state;
    struct hlist_node *next;
    unsigned int bucket;

    hash_for_each_safe(g_stepbp_task_steps, bucket, next, state, node)
    {
        hash_del(&state->node);
        kfree(state);
    }
}

static atomic_t g_stepbp_returns_inflight = ATOMIC_INIT(0);
static DECLARE_WAIT_QUEUE_HEAD(g_stepbp_return_wait);

struct stepbp_return_frame
{
    unsigned long return_addr;
    unsigned long generation;
    struct pt_regs *regs;
    struct stepbp_task_step *task_state;
};

static inline void stepbp_publish_monitor(struct break_point *info, bool stopping)
{
    g_stepbp_generation++;
    WRITE_ONCE(g_stepbp_info, info);
    WRITE_ONCE(g_stepbp_stopping, stopping);
}

// 设置返回现场的 SPSR.SS 位，配合 ret_to_user 中的 TIF_SINGLESTEP 打开 MDSCR.SS。
static inline void stepbp_set_regs_single_step(struct pt_regs *regs)
{
    if (regs) regs->pstate |= DBG_SPSR_SS;
}

// 清理返回现场的 SPSR.SS 位，停用后不再续发单步异常。
static inline void stepbp_clear_regs_single_step(struct pt_regs *regs)
{
    if (regs) regs->pstate &= ~DBG_SPSR_SS;
}

static inline void stepbp_enable_task_single_step(struct task_struct *task)
{
    if (!task) return;

    // TIF_SINGLESTEP 是线程级状态，必须对每个目标 task 单独设置。
    // ret_to_user 看到该 flag 后会打开当前 CPU 的 MDSCR.SS。
    set_ti_thread_flag(task_thread_info(task), TIF_SINGLESTEP);
    stepbp_set_regs_single_step(task_pt_regs(task));
}

static inline void stepbp_disable_task_single_step(struct task_struct *task)
{
    if (!task) return;

    clear_ti_thread_flag(task_thread_info(task), TIF_SINGLESTEP);
    stepbp_clear_regs_single_step(task_pt_regs(task));
}

static inline void stepbp_disable_current_hardware_step(struct pt_regs *regs)
{
    clear_thread_flag(TIF_SINGLESTEP);
    stepbp_clear_regs_single_step(regs);
    write_sysreg(read_sysreg(mdscr_el1) & ~DBG_MDSCR_SS, mdscr_el1);
    isb();
}

static inline void stepbp_apply_task_single_step(struct task_struct *task, bool enable)
{
    unsigned long flags;

    spin_lock_irqsave(&g_stepbp_lock, flags);
    if (enable && !stepbp_task_exiting_syscall_locked(task)) stepbp_enable_task_single_step(task);
    else stepbp_disable_task_single_step(task);
    spin_unlock_irqrestore(&g_stepbp_lock, flags);
}

struct stepbp_cpu_update
{
    struct break_point *info;
    bool enable;
};

static void stepbp_update_current_cpu(void *data)
{
    struct stepbp_cpu_update *update = data;
    unsigned long flags;

    if (!bp_info_targets_task(update->info, current)) return;

    spin_lock_irqsave(&g_stepbp_lock, flags);
    if (update->enable && bp_info_find_type_for_task(update->info, BP_BREAKPOINT_X, current) && !stepbp_task_exiting_syscall_locked(current)) stepbp_enable_task_single_step(current);
    else stepbp_disable_current_hardware_step(task_pt_regs(current));
    spin_unlock_irqrestore(&g_stepbp_lock, flags);
}

static int stepbp_apply_info_tasks(struct break_point *info, bool enable)
{
    struct stepbp_cpu_update update = {
        .info = info,
        .enable = enable,
    };
    struct task_struct *target_task;
    struct task_struct *process;
    struct task_struct *task;
    pid_t target_tgid;
    int touched_count = 0;

    if (!bp_info_is_valid(info)) return 0;
    target_tgid = READ_ONCE(info->tgid);

    rcu_read_lock();
    target_task = find_task_by_vpid(target_tgid);
    if (!target_task)
    {
        for_each_process_thread(process, task)
        {
            if (!bp_info_targets_task(info, task)) continue;
            stepbp_apply_task_single_step(task, enable && bp_info_find_type_for_task(info, BP_BREAKPOINT_X, task));
            touched_count++;
        }
        goto out_unlock;
    }

    if (target_task->tgid == target_tgid)
    {
        stepbp_apply_task_single_step(target_task, enable && bp_info_find_type_for_task(info, BP_BREAKPOINT_X, target_task));
        touched_count++;
        for_each_thread(target_task, task)
        {
            stepbp_apply_task_single_step(task, enable && bp_info_find_type_for_task(info, BP_BREAKPOINT_X, task));
            touched_count++;
        }
    }
    else
    {
        stepbp_apply_task_single_step(target_task, enable && bp_info_find_type_for_task(info, BP_BREAKPOINT_X, target_task));
        touched_count = 1;
    }

out_unlock:
    rcu_read_unlock();
    stepbp_update_current_cpu(&update);
    smp_call_function(stepbp_update_current_cpu, &update, 1);
    return touched_count;
}

static void __attribute__((used, __noinline__)) stepbp_finish_syscall_trace_exit(struct stepbp_return_frame *frame);
__attribute__((naked, used)) void ret_trampoline_stepbp_syscall_trace_exit(void)
{
    asm volatile("mov x0, sp\n"
                 "bl stepbp_finish_syscall_trace_exit\n"
                 "ldp x16, xzr, [sp], #304\n"
                 "ret x16\n");
}

static void __attribute__((used, __noinline__)) stepbp_finish_syscall_trace_exit(struct stepbp_return_frame *frame)
{
    unsigned long flags;
    struct break_point *info;

    spin_lock_irqsave(&g_stepbp_lock, flags);
    if (frame->task_state && frame->task_state->syscall_exit_depth) frame->task_state->syscall_exit_depth--;
    if (frame->generation == g_stepbp_generation)
    {
        info = g_stepbp_info;
        if (!g_stepbp_stopping && bp_info_find_type_for_task(info, BP_BREAKPOINT_X, current)) stepbp_enable_task_single_step(current);
        else stepbp_disable_current_hardware_step(frame->regs);
    }
    spin_unlock_irqrestore(&g_stepbp_lock, flags);

    if (atomic_dec_and_test(&g_stepbp_returns_inflight)) wake_up_all(&g_stepbp_return_wait);
}

// syscall_trace_exit() 会在 _TIF_SINGLESTEP 下调用 report_syscall(PTRACE_SYSCALL_EXIT)，
// 对用户态表现为 ptrace pseudo-step SIGTRAP。入口临时清 flag，并用返回跳板在
// syscall_trace_exit() 完整执行 audit/trace/rseq 后恢复单步状态。
static int work_trampoline_stepbp_syscall_trace_exit(struct pt_regs *hook_regs)
{
    unsigned long flags;
    unsigned long generation;
    struct stepbp_return_frame *frame;
    struct pt_regs *regs;
    struct break_point *info;
    bool target_task;

    if (!hook_regs) return 0;

    regs = (struct pt_regs *)hook_regs->regs[0];
    if (!regs || !user_mode(regs)) return 0;

    spin_lock_irqsave(&g_stepbp_lock, flags);
    info = g_stepbp_info;
    target_task = g_stepbp_stopping ? bp_info_targets_task(info, current) : !!bp_info_find_type_for_task(info, BP_BREAKPOINT_X, current);
    generation = g_stepbp_generation;
    if (target_task)
    {
        frame = hook_frame_metadata(hook_regs);
        frame->return_addr = hook_regs->regs[30];
        frame->generation = generation;
        frame->regs = regs;
        frame->task_state = stepbp_task_state_locked(current, true);
        if (frame->task_state) frame->task_state->syscall_exit_depth++;
        atomic_inc(&g_stepbp_returns_inflight);
        hook_regs->sp = (unsigned long)frame;
        hook_regs->regs[30] = (unsigned long)ret_trampoline_stepbp_syscall_trace_exit;
        stepbp_set_regs_single_step(regs);
        clear_thread_flag(TIF_SINGLESTEP);
    }
    spin_unlock_irqrestore(&g_stepbp_lock, flags);

    return 0;
}

// __switch_to 入口补 arm：覆盖安装后才创建/切入的目标线程。
static int work_trampoline_stepbp_switch(struct pt_regs *hook_regs)
{
    struct task_struct *next;
    struct break_point *info;
    unsigned long flags;
    bool enable = false;
    pid_t target_tgid = 0;

    if (!hook_regs) return 0;

    next = (struct task_struct *)hook_regs->regs[1];
    spin_lock_irqsave(&g_stepbp_lock, flags);
    info = g_stepbp_info;
    if (!g_stepbp_stopping && bp_info_targets_task(info, next))
    {
        target_tgid = READ_ONCE(info->tgid);
        enable = !!bp_info_find_type_for_task(info, BP_BREAKPOINT_X, next);
    }
    spin_unlock_irqrestore(&g_stepbp_lock, flags);

    if (target_tgid) stepbp_apply_task_single_step(next, enable);

    return 0;
}

static int __attribute__((used, __noinline__)) stepbp_finish_call_step_hook(int native_result, struct stepbp_return_frame *frame, unsigned long esr)
{
    int result = native_result;
    unsigned long flags;
    struct bp_point *hit_point = NULL;
    struct break_point *info;
    void (*hit_callback)(void *regs, void *fp_regs, void *hit_point) = NULL;
    unsigned long hit_generation = 0;
    bool generation_matches;
    bool target_task = false;
    bool stopping = false;
    struct pt_regs *regs;

    regs = frame->regs;
    spin_lock_irqsave(&g_stepbp_lock, flags);
    generation_matches = frame->generation == g_stepbp_generation;
    if (generation_matches)
    {
        info = g_stepbp_info;
        stopping = g_stepbp_stopping;
        target_task = stopping ? bp_info_targets_task(info, current) : !!bp_info_find_type_for_task(info, BP_BREAKPOINT_X, current);
        if (target_task && !stopping)
        {
            bool replay = stepbp_replayed_pc_locked(regs, esr, native_result);
            if (!replay && native_result != DBG_HOOK_HANDLED) hit_point = bp_info_find_point_by_pc_for_task(info, regs->pc, current);
            if (hit_point)
            {
                hit_callback = READ_ONCE(hit_point->on_hit);
                hit_generation = g_stepbp_generation;
            }
        }
    }
    spin_unlock_irqrestore(&g_stepbp_lock, flags);

    if (!generation_matches || target_task) result = DBG_HOOK_HANDLED;

    if (generation_matches && target_task && !stopping && native_result != DBG_HOOK_HANDLED && hit_point && hit_callback)
    {
        spin_lock_irqsave(&g_stepbp_hit_lock, flags);
        if (!READ_ONCE(g_stepbp_stopping) && READ_ONCE(g_stepbp_generation) == hit_generation)
        {
            struct fp_regs fp_regs __attribute__((__uninitialized__));
#ifdef SGAME_STEPBP_TRACE
            ls_log_always_tag("stepbp-trace", "tid=%d pc=0x%llx x0=0x%llx esr=0x%llx pstate=0x%llx\n", current->pid, (unsigned long long)regs->pc, (unsigned long long)regs->regs[0], (unsigned long long)read_sysreg(esr_el1), (unsigned long long)regs->pstate);
#endif
            read_all_q_regs(&fp_regs);
            hit_callback(regs, &fp_regs, hit_point);
            write_all_q_regs(&fp_regs);
        }
        spin_unlock_irqrestore(&g_stepbp_hit_lock, flags);
    }

    spin_lock_irqsave(&g_stepbp_lock, flags);
    if (frame->generation == g_stepbp_generation)
    {
        info = g_stepbp_info;
        if (!g_stepbp_stopping && bp_info_find_type_for_task(info, BP_BREAKPOINT_X, current)) stepbp_enable_task_single_step(current);
        else stepbp_disable_current_hardware_step(frame->regs);
    }
    spin_unlock_irqrestore(&g_stepbp_lock, flags);

    if (atomic_dec_and_test(&g_stepbp_returns_inflight)) wake_up_all(&g_stepbp_return_wait);
    return result;
}

#if SGAME_STEPBP_DIRECT
typedef unsigned long stepbp_esr_t;
#else
static struct step_hook g_stepbp_user_hook;

/* Older GKI kernels use a 32-bit ESR callback. Derive the exact native type so
 * CFI also agrees when a vendor backports the widened prototype. */
typedef typeof(__builtin_choose_expr(
    __builtin_types_compatible_p(typeof(((struct step_hook *)0)->fn), int (*)(struct pt_regs *, unsigned int)),
    (unsigned int)0, (unsigned long)0)) stepbp_esr_t;
#endif

/* The compiler can inline call_step_hook into single_step_handler. Register a
 * native hook so both the inlined and out-of-line dispatch paths reach us.
 * Preserve the remaining native hooks before recording our own step. */
static int stepbp_user_single_step(struct pt_regs *regs, stepbp_esr_t esr)
{
    unsigned long flags;
    unsigned long generation;
    bool target_task;
    struct stepbp_return_frame frame;
    struct break_point *info;
#if !SGAME_STEPBP_DIRECT
    struct step_hook *hook = &g_stepbp_user_hook;
#endif
    int native_result = DBG_HOOK_ERROR;

    if (!regs || !user_mode(regs)) return DBG_HOOK_ERROR;

    spin_lock_irqsave(&g_stepbp_lock, flags);
    info = g_stepbp_info;
    target_task = g_stepbp_stopping ? bp_info_targets_task(info, current) : !!bp_info_find_type_for_task(info, BP_BREAKPOINT_X, current);
    generation = g_stepbp_generation;
    if (target_task)
    {
        frame.return_addr = 0;
        frame.generation = generation;
        frame.regs = regs;
        atomic_inc(&g_stepbp_returns_inflight);
    }
    spin_unlock_irqrestore(&g_stepbp_lock, flags);
    if (!target_task) return DBG_HOOK_ERROR;

#if SGAME_STEPBP_DIRECT
    if (g_stepbp_uprobe_single_step) native_result = g_stepbp_uprobe_single_step(regs, esr);
#else
    list_for_each_entry_continue_rcu(hook, g_stepbp_native_hooks, node)
    {
        native_result = hook->fn(regs, esr);
        if (native_result == DBG_HOOK_HANDLED) break;
    }
#endif
    return stepbp_finish_call_step_hook(native_result, &frame, esr);
}

#if SGAME_STEPBP_DIRECT
/* 6.18 dispatches EL0 steps directly instead of registering step_hook objects.
 * Untargeted traps replay the kernel handler unchanged. Targeted traps retain
 * native uprobe precedence and the shared scope/replay/lifecycle handling. */
static int work_trampoline_stepbp_el0_softstep(struct pt_regs *hook_regs)
{
    if (!hook_regs) return 0;
    return stepbp_user_single_step((struct pt_regs *)hook_regs->regs[1],
                                  hook_regs->regs[0]) == DBG_HOOK_HANDLED;
}
#endif

static struct hook_entry g_stepbp_required_hooks[] = {
    HOOK_ENTRY("syscall_trace_exit", work_trampoline_stepbp_syscall_trace_exit),
#if SGAME_STEPBP_DIRECT
    HOOK_ENTRY("do_el0_softstep", work_trampoline_stepbp_el0_softstep),
#endif
};

static struct hook_entry g_stepbp_switch_hook[] = {
    HOOK_ENTRY("__switch_to", work_trampoline_stepbp_switch),
};

static int stepbp_install_required_hooks(void)
{
    int count = sizeof(g_stepbp_required_hooks) / sizeof(g_stepbp_required_hooks[0]);

#if SGAME_STEPBP_DIRECT
    if (IS_ENABLED(CONFIG_UPROBES) && !g_stepbp_uprobe_single_step)
    {
        g_stepbp_uprobe_single_step = (void *)generic_kallsyms_lookup_name("uprobe_single_step_handler");
        if (!g_stepbp_uprobe_single_step) return -ENOENT;
    }
#else
    if (!g_stepbp_register_hook) g_stepbp_register_hook = (void *)generic_kallsyms_lookup_name("register_user_step_hook");
    if (!g_stepbp_unregister_hook) g_stepbp_unregister_hook = (void *)generic_kallsyms_lookup_name("unregister_user_step_hook");
    if (!g_stepbp_native_hooks) g_stepbp_native_hooks = (void *)generic_kallsyms_lookup_name("user_step_hook");
    if (!g_stepbp_register_hook || !g_stepbp_unregister_hook || !g_stepbp_native_hooks) return -ENOENT;
#endif

    for (int i = 0; i < count; i++)
    {
        int ret = hook_entry_install(&g_stepbp_required_hooks[i]);
        if (ret)
        {
            ls_log_always_tag("stepbp", "required hook failed index=%d symbol=%s status=%d target=0x%llx patch_text=0x%llx\n", i, g_stepbp_required_hooks[i].target_sym, ret, (unsigned long long)g_stepbp_required_hooks[i].target_addr, (unsigned long long)fn_aarch64_insn_patch_text);
            while (--i >= 0) hook_entry_remove(&g_stepbp_required_hooks[i]);
            return ret;
        }
    }

#if !SGAME_STEPBP_DIRECT
    if (!g_stepbp_hook_registered)
    {
        g_stepbp_user_hook.fn = stepbp_user_single_step;
        g_stepbp_register_hook(&g_stepbp_user_hook);
        g_stepbp_hook_registered = true;
    }
#endif

    return 0;
}

static void stepbp_remove_required_hooks(void)
{
#if !SGAME_STEPBP_DIRECT
    if (g_stepbp_hook_registered)
    {
        g_stepbp_unregister_hook(&g_stepbp_user_hook);
        g_stepbp_hook_registered = false;
    }
#endif
    for (int i = (int)(sizeof(g_stepbp_required_hooks) / sizeof(g_stepbp_required_hooks[0])) - 1; i >= 0; i--) hook_entry_remove(&g_stepbp_required_hooks[i]);
}

static void stepbp_install_optional_switch_hook(void)
{
    int status = inline_hook_install_count(g_stepbp_switch_hook, sizeof(g_stepbp_switch_hook) / sizeof(g_stepbp_switch_hook[0]));
    if (status)
    {
        ls_log_always_tag("stepbp", "optional switch hook skipped status=%d target=0x%llx\n", status, (unsigned long long)g_stepbp_switch_hook[0].target_addr);
        return;
    }
}

// 调用方持有 g_stepbp_mutex；先清理目标线程保存现场，再移除 hook。
static void stepbp_stop_monitor_locked(void)
{
    struct break_point *info;
    unsigned long flags;

    spin_lock_irqsave(&g_stepbp_lock, flags);
    info = g_stepbp_info;
    stepbp_publish_monitor(info, true);
    spin_unlock_irqrestore(&g_stepbp_lock, flags);

    spin_lock_irqsave(&g_stepbp_hit_lock, flags);
    spin_unlock_irqrestore(&g_stepbp_hit_lock, flags);

    if (info) stepbp_apply_info_tasks(info, false);

    inline_hook_remove_count(g_stepbp_switch_hook, sizeof(g_stepbp_switch_hook) / sizeof(g_stepbp_switch_hook[0]));
    stepbp_remove_required_hooks();
    wait_event(g_stepbp_return_wait, atomic_read(&g_stepbp_returns_inflight) == 0);
    stepbp_free_task_steps();

    spin_lock_irqsave(&g_stepbp_lock, flags);
    stepbp_publish_monitor(NULL, false);
    spin_unlock_irqrestore(&g_stepbp_lock, flags);
}

// 停止 STEPBP：串行化安装/卸载，避免并发替换 hook 和全局配置。
static inline void stop_stepbp_monitor(void)
{
    mutex_lock(&g_stepbp_mutex);
    stepbp_stop_monitor_locked();
    mutex_unlock(&g_stepbp_mutex);
}

// 安装 STEPBP：持有共享配置指针、注册原生 step hook，并启用目标线程单步。
static inline int start_stepbp_monitor(struct break_point *info)
{
    pid_t target_tgid;
    int status;
    unsigned long flags;
    struct bp_point *first_point;

    first_point = bp_info_find_configured_scoped_type(info, BP_BREAKPOINT_X);
    if (!first_point)
    {
        ls_log_always_tag("stepbp", "start rejected tgid=%d no active execute point\n", info ? READ_ONCE(info->tgid) : -1);
        return -EINVAL;
    }

    target_tgid = READ_ONCE(info->tgid);

    mutex_lock(&g_stepbp_mutex);

    status = stepbp_install_required_hooks();
    if (status)
    {
        ls_log_always_tag("stepbp", "hook install failed tgid=%d status=%d\n", target_tgid, status);
        goto out_unlock;
    }

    stepbp_install_optional_switch_hook();

    spin_lock_irqsave(&g_stepbp_lock, flags);
    stepbp_publish_monitor(info, false);
    spin_unlock_irqrestore(&g_stepbp_lock, flags);

    stepbp_apply_info_tasks(info, true);
    status = 0;

out_unlock:
    mutex_unlock(&g_stepbp_mutex);
    return status;
}

#endif // ARM64_STEPDBG_H
