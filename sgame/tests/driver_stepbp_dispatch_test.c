/* Exercise the production 6.18 dispatch, shared replay handling and rollback. */
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <errno.h>

#define SGAME_STEPBP_DIRECT 1
#define DBG_HOOK_HANDLED 0
#define DBG_HOOK_ERROR 1
#define BP_BREAKPOINT_X 1
#define BIT(n) (1UL << (n))
#define READ_ONCE(x) (x)
#define IS_ENABLED(x) (x)
#define spin_lock_irqsave(p, flags) do { (flags) = 0; assert(!*(p)); ++*(p); } while (0)
#define spin_unlock_irqrestore(p, flags) do { (void)(flags); assert(*(p) == 1); --*(p); } while (0)
#define ls_log_always_tag(...) ((void)0)
struct pt_regs { unsigned long regs[31], pc, pstate; bool user; };
struct fp_regs { int marker; };
struct bp_point { void (*on_hit)(void *, void *, void *); };
struct break_point { bool active; struct bp_point point; };
struct task_struct { bool targeted; } task;
#define current (&task)
#define user_mode(regs) ((regs)->user)
struct stepbp_task_step { bool valid; unsigned long generation, pc; } state;
struct stepbp_return_frame { unsigned long return_addr, generation; struct pt_regs *regs; };
typedef unsigned long stepbp_esr_t;
static struct break_point info, *g_stepbp_info;
static bool g_stepbp_stopping;
static unsigned long g_stepbp_generation;
static int g_stepbp_lock, g_stepbp_hit_lock, g_stepbp_returns_inflight, g_stepbp_return_wait;
static int native_calls, original_calls, hit_calls, enables, disables, wakes;
static int native_result, install_fail, removed, removal_order[2];
static bool native_teardown, missing_native;
static unsigned long expected_esr;
static int (*g_stepbp_uprobe_single_step)(struct pt_regs *, unsigned long);
static void atomic_inc(int *value) { ++*value; }
static bool atomic_dec_and_test(int *value) { assert(*value > 0); return --*value == 0; }
static void wake_up_all(int *queue) { (void)queue; ++wakes; }
static bool bp_info_targets_task(struct break_point *bp, struct task_struct *owner) { return bp && owner->targeted; }
static struct bp_point *bp_info_find_type_for_task(struct break_point *bp, int type, struct task_struct *owner) {
    assert(type == BP_BREAKPOINT_X);
    return bp && bp->active && owner->targeted ? &bp->point : NULL;
}
static struct bp_point *bp_info_find_point_by_pc_for_task(struct break_point *bp, unsigned long pc, struct task_struct *owner) {
    assert(pc == 0x1234);
    return bp_info_find_type_for_task(bp, BP_BREAKPOINT_X, owner);
}
static struct stepbp_task_step *stepbp_task_state_locked(struct task_struct *owner, bool create) {
    assert(owner == current && create && g_stepbp_lock == 1); return &state;
}
static void stepbp_enable_task_single_step(struct task_struct *owner) { assert(owner == current); ++enables; }
static void stepbp_disable_current_hardware_step(struct pt_regs *regs) { assert(regs); ++disables; }
static void read_all_q_regs(struct fp_regs *regs) { regs->marker = 77; }
static void write_all_q_regs(struct fp_regs *regs) { assert(regs->marker == 77); }
static void on_hit(void *raw, void *fp, void *point) {
    struct pt_regs *regs = raw;
    assert(((struct fp_regs *)fp)->marker == 77 && point == &info.point);
    assert(g_stepbp_hit_lock == 1 && !g_stepbp_lock);
    regs->regs[0] = 1752; ++hit_calls;
}
static int native_handler(struct pt_regs *regs, unsigned long esr) {
    assert(regs->user && esr == expected_esr && !g_stepbp_lock && !g_stepbp_hit_lock);
    ++native_calls;
    if (native_teardown) { ++g_stepbp_generation; g_stepbp_info = NULL; }
    return native_result;
}
static int work_trampoline_stepbp_syscall_trace_exit(struct pt_regs *regs) { (void)regs; return 0; }
struct hook_entry { const char *target_sym; int (*work_fn)(struct pt_regs *); bool installed; };
#define HOOK_ENTRY(name, fn) { .target_sym = (name), .work_fn = (fn) }
static unsigned long generic_kallsyms_lookup_name(const char *name) {
    assert(!strcmp(name, "uprobe_single_step_handler"));
    return missing_native ? 0 : (unsigned long)native_handler;
}
static int hook_entry_install(struct hook_entry *entry) {
    int index = !strcmp(entry->target_sym, "do_el0_softstep");
    if (install_fail == index) return -EIO;
    entry->installed = true; return 0;
}
static void hook_entry_remove(struct hook_entry *entry) {
    assert(entry->installed && removed < 2);
    removal_order[removed++] = !strcmp(entry->target_sym, "do_el0_softstep");
    entry->installed = false;
}
#include STEPBP_PRODUCTION

static void reset(void) {
    assert(!g_stepbp_lock && !g_stepbp_hit_lock && !g_stepbp_returns_inflight);
    memset(&state, 0, sizeof(state));
    info.active = true; info.point.on_hit = on_hit; g_stepbp_info = &info;
    task.targeted = true; g_stepbp_stopping = false; g_stepbp_generation = 7;
    native_calls = original_calls = hit_calls = enables = disables = wakes = removed = 0;
    install_fail = -1; missing_native = native_teardown = false;
    native_result = DBG_HOOK_ERROR;
    g_stepbp_uprobe_single_step = native_handler;
    expected_esr = BIT(40);
}
static int dispatch(struct pt_regs *regs) {
    struct pt_regs hook = { .regs = { expected_esr, (unsigned long)regs } };
    int consumed = work_trampoline_stepbp_el0_softstep(&hook);
    if (!consumed) { ++original_calls; native_handler(regs, expected_esr); }
    assert(!g_stepbp_returns_inflight && !g_stepbp_lock && !g_stepbp_hit_lock);
    return consumed;
}
int main(void) {
    struct pt_regs regs = { .pc = 0x1234, .user = true };
    reset(); task.targeted = false;
    assert(!dispatch(&regs) && original_calls == 1 && native_calls == 1 && !hit_calls && !wakes);
    reset(); info.active = false;
    assert(!dispatch(&regs) && original_calls == 1 && !hit_calls);
    reset();
    assert(dispatch(&regs) && !original_calls && native_calls == 1 && hit_calls == 1 && wakes == 1);
    assert(regs.regs[0] == 1752 && enables == 1 && !disables);
    assert(dispatch(&regs) && native_calls == 2 && hit_calls == 1 && wakes == 2);
    expected_esr |= BIT(24);
    assert(dispatch(&regs) && hit_calls == 2);
    reset(); native_result = DBG_HOOK_HANDLED;
    assert(dispatch(&regs) && !original_calls && native_calls == 1 && !hit_calls && wakes == 1);
    reset(); native_teardown = true;
    assert(dispatch(&regs) && !hit_calls && !enables && !disables && wakes == 1);
    reset(); g_stepbp_stopping = true;
    assert(dispatch(&regs) && !hit_calls && !enables && disables == 1 && wakes == 1);
    reset(); g_stepbp_uprobe_single_step = NULL;
    assert(dispatch(&regs) && hit_calls == 1 && !native_calls);
    assert(!work_trampoline_stepbp_el0_softstep(NULL));
    puts("PASS: 6.18 production step dispatch; thread scopes, native uprobe precedence, replay/self-branch filtering, stale generation and teardown");

    reset(); g_stepbp_uprobe_single_step = NULL; missing_native = true;
#if CONFIG_UPROBES
    assert(stepbp_install_required_hooks() == -ENOENT && !removed);
#endif
    reset(); g_stepbp_uprobe_single_step = NULL; install_fail = 0;
    assert(stepbp_install_required_hooks() == -EIO && !removed);
    reset(); g_stepbp_uprobe_single_step = NULL; install_fail = 1;
    assert(stepbp_install_required_hooks() == -EIO && removed == 1 && removal_order[0] == 0);
    reset(); g_stepbp_uprobe_single_step = NULL;
    assert(stepbp_install_required_hooks() == 0);
    assert(g_stepbp_required_hooks[0].installed && g_stepbp_required_hooks[1].installed);
    stepbp_remove_required_hooks();
    assert(removed == 2 && removal_order[0] == 1 && removal_order[1] == 0);
    puts("PASS: 6.18 production required-hook install failure, rollback and reverse removal");
    return 0;
}

