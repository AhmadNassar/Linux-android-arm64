/* SPDX-License-Identifier: GPL-2.0 */
#ifndef SGAME_COMPAT_SESSION_H
#define SGAME_COMPAT_SESSION_H

/* The memory service must not depend on relocating vendor function entries.
 * One sleeping mutex owns the shared request and its GUP/vmap references.
 * In particular, it remains held while a swap request waits for backend I/O. */
#ifndef SGAME_COMPAT_SESSION_TEST
#include <linux/mutex.h>
#include <linux/rcupdate.h>
#include <linux/version.h>
#if LINUX_VERSION_CODE >= KERNEL_VERSION(6, 5, 0)
#define SGAME_SESSION_MODERN_GUP 1
#endif
#endif

#ifndef SGAME_SESSION_MODERN_GUP
#define SGAME_SESSION_MODERN_GUP 0
#endif

static DEFINE_MUTEX(sgame_session_mutex);
static bool sgame_stop_requested;
static bool sgame_exit_event;
static struct page **sgame_client_pages;
static int sgame_client_nr_pages;
static void *sgame_client_mapping;
static u64 sgame_client_start_time;

/* Caller holds sgame_session_mutex, excluding an in-flight dispatch. */
static void sgame_release_client(void)
{
    void *mapping = sgame_client_mapping;
    struct task_struct *owner = ls_process_task;

    sgame_connected_req = NULL;
    sgame_client_mapping = NULL;
    ls_process_task = NULL;
    if (owner) {
        hide_kgsl_remove(owner->tgid);
        hide_task_remove(owner->tgid);
        v_touch_destroy();
        v_gnss_destroy();
        v_gyro_destroy();
        remove_process_hwbp();
        remove_process_ptebp();
        remove_process_dptdbg();
        remove_process_stepbp();
        syscall_monitor_remove_all();
        cntvct_monitor_remove_all();
    }
    sgame_vmem_drop_mm();
    if (mapping) vunmap(mapping);
    if (sgame_client_pages) {
        release_pages(sgame_client_pages, sgame_client_nr_pages);
        kfree(sgame_client_pages);
        sgame_client_pages = NULL;
        sgame_client_nr_pages = 0;
    }
    if (owner) put_task_struct(owner);
}

static void sgame_cleanup_client(void)
{
    mutex_lock(&sgame_session_mutex);
    sgame_release_client();
    mutex_unlock(&sgame_session_mutex);
}

static bool sgame_target_dead(pid_t pid)
{
    struct task_struct *task = get_task_by_pid(pid);
    bool dead;

    if (!task) return true;
    dead = atomic_read(&task->signal->live) == 0;
    put_task_struct(task);
    return dead;
}

/* Called in the connection thread, never in an atomic exit callback. A leader
 * can exit while its workers remain alive, hence signal->live rather than
 * PF_EXITING or pid_alive(leader). The retained task owns its signal object. */
static void sgame_reap_exited_targets(void)
{
    int i;
    pid_t pid;

    WRITE_ONCE(sgame_exit_event, false);
    mutex_lock(&sgame_session_mutex);
    if (ls_process_task && atomic_read(&ls_process_task->signal->live) == 0)
        sgame_release_client();
    for (i = 0; i < ARM_SYSCALL_MONITOR_MAX_PIDS; ++i) {
        pid = READ_ONCE(g_syscall_monitor_pids[i]);
        if (pid && sgame_target_dead(pid)) syscall_monitor_remove(pid);
    }
    pid = READ_ONCE(g_cntvct_monitor_tgid);
    if (pid && sgame_target_dead(pid)) cntvct_monitor_remove(pid);
    mutex_unlock(&sgame_session_mutex);
}

static struct task_struct *sgame_find_client(void)
{
    struct task_struct *task, *candidate = NULL;

    rcu_read_lock();
    for_each_process(task) {
        char comm[TASK_COMM_LEN];

        get_task_comm(comm, task);
        if (strcmp(comm, "LS") || task->start_time <= sgame_client_start_time)
            continue;
        if (atomic_read(&task->signal->live) == 0) continue;
        if (!candidate || task->start_time > candidate->start_time)
            candidate = task;
    }
    if (candidate) get_task_struct(candidate);
    rcu_read_unlock();
    return candidate;
}

static void sgame_connect_client(struct task_struct *task)
{
    const unsigned long shared_addr = 0x2025827000;
    const unsigned long shared_offset = shared_addr & (PAGE_SIZE - 1);
    const int nr_pages = DIV_ROUND_UP(shared_offset + sizeof(struct request_obj), PAGE_SIZE);
    struct page **pages;
    struct request_obj *mapping;
    void *base;
    struct mm_struct *mm;
    int pinned;

    mm = get_task_mm(task);
    if (!mm) goto put_task;
    pages = kmalloc_array(nr_pages, sizeof(*pages), GFP_KERNEL);
    if (!pages) goto put_mm;
    mmap_read_lock(mm);
#if SGAME_SESSION_MODERN_GUP
    pinned = get_user_pages_remote(mm, shared_addr - shared_offset, nr_pages,
                                  FOLL_WRITE, pages, NULL);
#else
    pinned = get_user_pages_remote(mm, shared_addr - shared_offset, nr_pages,
                                  FOLL_WRITE, pages, NULL, NULL);
#endif
    mmap_read_unlock(mm);
    mmput(mm);
    if (pinned != nr_pages) {
        if (pinned > 0) release_pages(pages, pinned);
        goto free_pages;
    }
    base = vmap(pages, nr_pages, VM_MAP, PAGE_KERNEL);
    if (!base) goto unpin_pages;
    /* The unchanged wire address is 4K-aligned, with a 12K offset on a
     * native 16K kernel. Pin/map the enclosing pages and retain their base. */
    mapping = (struct request_obj *)((char *)base + shared_offset);

    mutex_lock(&sgame_session_mutex);
    if (READ_ONCE(sgame_stop_requested) || atomic_read(&task->signal->live) == 0) {
        mutex_unlock(&sgame_session_mutex);
        vunmap(base);
        goto unpin_pages;
    }
    /* Waits for the previous request to finish before releasing its mapping.
     * A reconnect never kills the previous userspace process. */
    sgame_release_client();
    sgame_client_pages = pages;
    sgame_client_nr_pages = nr_pages;
    sgame_client_mapping = base;
    sgame_client_start_time = task->start_time;
    ls_process_task = task;
    sgame_connected_req = mapping;
    /* Install before userspace can initialize its GPU. Preserve upstream's
     * best-effort client hook policy, with explicit per-feature results. */
    {
        int task_status = hide_task_install(task->tgid);
        int kgsl_status = hide_kgsl_install(task->tgid);

        ls_log_always_tag("connection", "pid=%d task_hide=%d kgsl_hide=%d handshake=ready\n",
                          task->tgid, task_status, kgsl_status);
    }
    __atomic_store_n(&mapping->user, 1, __ATOMIC_RELEASE);
    mutex_unlock(&sgame_session_mutex);
    return;

unpin_pages:
    release_pages(pages, nr_pages);
free_pages:
    kfree(pages);
    goto put_task;
put_mm:
    mmput(mm);
put_task:
    put_task_struct(task);
}

#endif
