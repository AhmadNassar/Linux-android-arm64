/* Exercise production initialization, feature failures and rollback with host API substitutes. */
#include <assert.h>
#include <errno.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

enum { OK, SWAP, PAGES, PROC, CONNECT, DISPATCH, HIDE_CONNECT, HIDE_DISPATCH, EXIT_HOOKS, SIGNAL_TRACE };
struct task_struct { int pid, refs, woken, stopped; };
static struct task_struct tasks[3];
static struct task_struct *volatile connect_thread_task, *volatile dispatch_thread_task;
static bool sgame_stop_requested, sgame_exit_event;
static int failure, created, pages, cleanup_calls, live_threads, hidden, module_hidden, renderer_calls, hook_cleanup;
static char last_error[160];
struct work_struct { void (*run)(struct work_struct *); };
static struct work_struct *queued_work;
#define DECLARE_WORK(name,fn) struct work_struct name={fn}
static void schedule_work(struct work_struct *work) { assert(!queued_work);queued_work=work; }
#define WRITE_ONCE(dst,value) ((dst)=(value))
#define SGAME_SWAP_VERSION "test"
#define MODULE_INFO(key,value)
#define IS_ERR(ptr) ((intptr_t)(ptr)<0 && (intptr_t)(ptr)>-4096)
#define PTR_ERR(ptr) ((long)(intptr_t)(ptr))
#define pr_info(...) ((void)0)
#define pr_err(...) snprintf(last_error,sizeof(last_error),__VA_ARGS__)
#define ls_log_always_tag(tag,...) ((void)0)
static int sgame_swap_init(void) { return failure==SWAP ? -ENOENT : 0; }
static void bypass_cfi(void) {}
static int allocate_physical_page_info(void) { assert(!pages);if(failure==PAGES)return -ENOMEM;pages=1;return 0; }
static void free_physical_page_info(void) { assert(pages==1);pages=0; }
static int proc_create_block_init(void) { return failure==PROC ? -EOPNOTSUPP : 0; }
static int expected_hidden(void) {
    return failure==HIDE_CONNECT ? 4 : failure==HIDE_DISPATCH ? 2 : 6;
}
static int do_exit_init(void) { assert(hidden==expected_hidden());return failure==EXIT_HOOKS ? -EOPNOTSUPP : 0; }
static int signal_registered;
static int signal_register(void) {
    if (failure == SIGNAL_TRACE) return -ENOENT;
    signal_registered = 1;
    return 0;
}
static void signal_unregister(void) { signal_registered = 0; }
static void sgame_cleanup_client(void) { ++cleanup_calls; }
static void inline_hook_remove_all(void) { ++hook_cleanup; }
#define cpu_render_init() (++renderer_calls, 0)
#define cpu_render_exit() ((void)++renderer_calls)
static void hide_myself(void) { assert(pages && live_threads==2 && hidden==expected_hidden());++module_hidden; }
static int hide_task_install(int pid) {
    assert(pid==1 || pid==2);
    if((pid==1 && failure==HIDE_CONNECT) || (pid==2 && failure==HIDE_DISPATCH))return -EOPNOTSUPP;
    hidden|=1<<pid;return 0;
}
static void hide_task_remove(int pid) { assert(pid>=1 && pid<=3);hidden &= ~(1<<pid); }
static struct task_struct *kthread_create(int (*fn)(void *),void *arg,const char *name) {
    (void)fn;(void)arg;
    assert(pages && !live_threads && created<2);
    assert(strcmp(name,"ext4-rsv-conver")==0);
    if((created==0 && failure==CONNECT)||(created==1 && failure==DISPATCH))
        return (void *)(intptr_t)-EAGAIN;
    struct task_struct *task=&tasks[created];task->pid=++created;return task;
}
static void get_task_struct(struct task_struct *task) { ++task->refs; }
static void put_task_struct(struct task_struct *task) { assert(task->refs==1);--task->refs; }
static int kthread_stop(struct task_struct *task) {
    assert(sgame_stop_requested && task->refs==1 && !task->stopped);
    task->stopped=1;if(task->woken)--live_threads;return 0;
}
static void sched_set_fifo_low(struct task_struct *task) { assert(task->refs==1); }
static void sched_set_fifo(struct task_struct *task) { assert(task->refs==1); }
static int wake_up_process(struct task_struct *task) {
    assert(created==2 && pages && hidden==expected_hidden() && !module_hidden);
    assert(task->refs==1 && !task->woken);task->woken=1;++live_threads;return 1;
}
#include "../../lsdriver/sgame_lifecycle.h"
static int entry(void *data) { (void)data;return 0; }
int main(void) {
    const char *stages[]={"success","swap-api","physical-pages","proc-block","connect-thread","dispatch-thread",
                          "hide-connect-thread","hide-dispatch-thread","exit-hooks","signal-trace"};
    for(failure=OK;failure<=SIGNAL_TRACE;++failure) {
        memset(tasks,0,sizeof(tasks));last_error[0]=0;
        created=pages=cleanup_calls=live_threads=hidden=module_hidden=renderer_calls=hook_cleanup=0;
        sgame_stop_requested=false;
        int result=sgame_core_init(entry,entry);
        bool succeeds=failure==OK || failure==HIDE_CONNECT || failure==HIDE_DISPATCH || failure==SIGNAL_TRACE;
        if(succeeds) {
            assert(result==0 && module_hidden==1 && live_threads==2);
            assert(signal_registered == (failure != SIGNAL_TRACE));
            assert(!cleanup_calls && !hook_cleanup);
            if(failure==OK) {
                sgame_request_shutdown();
                assert(queued_work && live_threads==2 && !cleanup_calls);
                queued_work->run(queued_work);
                queued_work=NULL;
            } else sgame_core_exit();
        } else {
            assert(result<0 && strstr(last_error,stages[failure]));
            assert(!module_hidden);
            for(int i=0;i<created;++i)assert(!tasks[i].woken && tasks[i].stopped);
        }
        assert(cleanup_calls==1 && hook_cleanup==1 && !pages && !hidden && !renderer_calls && !live_threads);
        assert(!signal_registered);
        assert(!connect_thread_task && !dispatch_thread_task);
        for(int i=0;i<3;++i)assert(!tasks[i].refs);
        printf("PASS: %s; best-effort thread hooks, renderer disabled and balanced cleanup\n",stages[failure]);
    }
    return 0;
}
