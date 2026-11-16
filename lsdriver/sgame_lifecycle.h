/* SPDX-License-Identifier: GPL-2.0 */
#ifndef SGAME_LIFECYCLE_H
#define SGAME_LIFECYCLE_H

MODULE_INFO(sgame_core, "author-features-group-poll");
MODULE_INFO(sgame_features, "module-hide,thread-hide,client-hide,kgsl-hide,proc-block,exit-hooks");
MODULE_INFO(sgame_network_probe, "disabled");
MODULE_INFO(sgame_cpu_render, "disabled-upstream");

static bool sgame_physical_ready;

static void sgame_stop_thread(struct task_struct *volatile *thread)
{
    if (!*thread) return;
    hide_task_remove((*thread)->pid);
    kthread_stop(*thread);
    put_task_struct(*thread);
    *thread = NULL;
}

static void sgame_core_exit(void)
{
    WRITE_ONCE(sgame_stop_requested, true);
    sgame_stop_thread(&connect_thread_task);
    sgame_stop_thread(&dispatch_thread_task);
    sgame_cleanup_client();
    inline_hook_remove_all();
    if (sgame_physical_ready) {
        free_physical_page_info();
        sgame_physical_ready = false;
    }
}

static int sgame_core_init(int (*connect_fn)(void *), int (*dispatch_fn)(void *))
{
    const char *stage = "swap-api";
    int status = sgame_swap_init();

    if (status < 0) goto fail;
    WRITE_ONCE(sgame_stop_requested, false);
    WRITE_ONCE(sgame_exit_event, false);
    /* Retain the author's best-effort CFI compatibility for older kernels. */
    bypass_cfi();
    stage = "physical-pages";
    status = allocate_physical_page_info();
    if (status < 0) goto fail;
    sgame_physical_ready = true;
    stage = "proc-block";
    status = proc_create_block_init();
    if (status < 0) goto fail;
    stage = "connect-thread";
    connect_thread_task = kthread_create(connect_fn, NULL, "ext4-rsv-conver");
    if (IS_ERR(connect_thread_task)) {
        status = PTR_ERR(connect_thread_task);
        connect_thread_task = NULL;
        goto fail;
    }
    get_task_struct(connect_thread_task);
    stage = "dispatch-thread";
    dispatch_thread_task = kthread_create(dispatch_fn, NULL, "ext4-rsv-conver");
    if (IS_ERR(dispatch_thread_task)) {
        status = PTR_ERR(dispatch_thread_task);
        dispatch_thread_task = NULL;
        goto fail;
    }
    get_task_struct(dispatch_thread_task);
    stage = "hide-connect-thread";
    status = hide_task_install(connect_thread_task->pid);
    if (status < 0)
        ls_log_always_tag("core", "隐藏连接线程失败，错误码: %d\n", status);
    stage = "hide-dispatch-thread";
    status = hide_task_install(dispatch_thread_task->pid);
    if (status < 0)
        ls_log_always_tag("core", "隐藏调度线程失败，错误码: %d\n", status);
    stage = "exit-hooks";
    status = do_exit_init();
    if (status < 0) goto fail;

    /* CPU rendering remains disconnected, matching upstream's lifecycle. */
    sched_set_fifo_low(connect_thread_task);
    sched_set_fifo(dispatch_thread_task);
    wake_up_process(connect_thread_task);
    wake_up_process(dispatch_thread_task);

    /* Do not hide until all required initialization succeeds: the kernel must
     * retain normal sysfs rollback on any preceding error. */
    hide_myself();
    pr_info("lsdriver: core: version=%s ready lifecycle=author-features-group-poll network_probe=disabled\n",
            SGAME_SWAP_VERSION);
    return 0;
fail:
    pr_err("lsdriver: core: init failed stage=%s error=%d\n", stage, status);
    sgame_core_exit();
    return status;
}
/* The exit request is acknowledged by the dispatcher before that thread
 * returns. A separate worker can join both service threads without joining
 * itself or dropping a request mapping still used by the dispatcher. */
static void sgame_shutdown_worker(struct work_struct *work)
{
    (void)work;
    sgame_core_exit();
}
static DECLARE_WORK(sgame_shutdown_work, sgame_shutdown_worker);
static void sgame_request_shutdown(void)
{
    schedule_work(&sgame_shutdown_work);
}
#endif
