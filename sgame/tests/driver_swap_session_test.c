/* Exercise the production connection owner across failed pinning, exit and
 * reconnect while a reader sleeps. No host process memory or kernel is used. */
#include <assert.h>
#include <pthread.h>
#include <semaphore.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef uint64_t u64;
struct signal_struct { int live; };
struct mm_struct { int refs; };
struct task_struct {
    char comm[16];
    u64 start_time;
    int pid, tgid, refs;
    struct signal_struct *signal;
    struct mm_struct *mm;
    struct task_struct *next;
};
struct request_obj { int user, kernel, status; char data[8192]; };
struct page { int unused; };
static struct request_obj *sgame_connected_req;
static struct task_struct *volatile ls_process_task;
static struct task_struct *processes;
static int page_refs, maps, releases, cleanups, rcu_depth;
static int gup_limit = -1, fail_vmap;
static sem_t new_map_ready;
static bool notify_mapping;
static int g_syscall_monitor_pids[2], g_cntvct_monitor_tgid;

#define SGAME_COMPAT_SESSION_TEST
#define TASK_COMM_LEN 16
#define ARM_SYSCALL_MONITOR_MAX_PIDS 2
#ifndef PAGE_SIZE
#define PAGE_SIZE 4096
#endif
#define SHARED_OFFSET (0x2025827000UL & (PAGE_SIZE - 1))
#define SHARED_PAGES DIV_ROUND_UP(SHARED_OFFSET + sizeof(struct request_obj), PAGE_SIZE)
#define DIV_ROUND_UP(n, d) (((n) + (d) - 1) / (d))
#define GFP_KERNEL 0
#define FOLL_WRITE 1
#define VM_MAP 0
#define PAGE_KERNEL 0
#define READ_ONCE(x) (x)
#define WRITE_ONCE(x,v) ((x)=(v))
#define DEFINE_MUTEX(x) pthread_mutex_t x = PTHREAD_MUTEX_INITIALIZER
#define for_each_process(t) for ((t) = processes; (t); (t) = (t)->next)
#define rcu_read_lock() (++rcu_depth)
#define rcu_read_unlock() (--rcu_depth)
#define atomic_read(x) (*(x))
#define mutex_lock(x) assert(pthread_mutex_lock(x) == 0)
#define mutex_unlock(x) assert(pthread_mutex_unlock(x) == 0)
#define mmap_read_lock(x) ((void)(x))
#define mmap_read_unlock(x) ((void)(x))
static void get_task_struct(struct task_struct *task) { ++task->refs; }
static void put_task_struct(struct task_struct *task) { assert(task->refs > 0); --task->refs; }
static void get_task_comm(char *comm, struct task_struct *task) { memcpy(comm, task->comm, 16); }
static struct task_struct *get_task_by_pid(int pid) {
    struct task_struct *task;
    for_each_process(task) if (task->pid == pid) { get_task_struct(task); return task; }
    return NULL;
}
static struct mm_struct *get_task_mm(struct task_struct *task) {
    if (task->mm) ++task->mm->refs;
    return task->mm;
}
static void mmput(struct mm_struct *mm) { assert(mm->refs == 1); --mm->refs; }
static void *kmalloc_array(int count, size_t size, int flags) {
    (void)flags;
    return calloc((size_t)count, size);
}
#define kfree free
static int get_user_pages_remote(struct mm_struct *mm, unsigned long addr, int count,
                                 int flags, struct page **pages,
#if !SGAME_SESSION_MODERN_GUP
                                 void *vmas,
#endif
                                 void *locked) {
    assert(mm->refs == 1 && addr == 0x2025827000UL - SHARED_OFFSET && flags == FOLL_WRITE);
#if !SGAME_SESSION_MODERN_GUP
    assert(!vmas && !locked);
#else
    assert(!locked);
#endif
    if (gup_limit >= 0) count = gup_limit;
    for (int i = 0; i < count; ++i) { pages[i] = malloc(sizeof(*pages[i])); ++page_refs; }
    return count;
}
static void release_pages(struct page **pages, int count) {
    for (int i = 0; i < count; ++i) { free(pages[i]); --page_refs; }
    assert(page_refs >= 0);
}
static void *vmap(struct page **pages, int count, int flags, int prot) {
    (void)flags; (void)prot;
    assert(pages && count == SHARED_PAGES);
    if (fail_vmap) return NULL;
    void *mapping = calloc(1, SHARED_OFFSET + sizeof(struct request_obj));
    ++maps;
    if (notify_mapping) assert(sem_post(&new_map_ready) == 0);
    return mapping;
}
static void vunmap(void *mapping) { free(mapping); --maps; ++releases; assert(maps >= 0); }
static void sgame_vmem_drop_mm(void) {}
static void v_touch_destroy(void) { ++cleanups; }
static void v_gnss_destroy(void) {}
static void v_gyro_destroy(void) {}
static void remove_process_hwbp(void) {}
static void remove_process_ptebp(void) {}
static void remove_process_dptdbg(void) {}
static void remove_process_stepbp(void) {}
static void syscall_monitor_remove_all(void) { memset(g_syscall_monitor_pids, 0, sizeof(g_syscall_monitor_pids)); }
static void cntvct_monitor_remove_all(void) { g_cntvct_monitor_tgid = 0; }
static void syscall_monitor_remove(int pid) {
    for (int i = 0; i < 2; ++i) if (g_syscall_monitor_pids[i] == pid) g_syscall_monitor_pids[i] = 0;
}
static void cntvct_monitor_remove(int pid) { if (g_cntvct_monitor_tgid == pid) g_cntvct_monitor_tgid = 0; }
static int task_installs, gpu_installs, task_removes, gpu_removes;
static int gpu_status;
static char connection_log[256];
static int hide_task_install(int pid) {
    assert(pid > 0 && sgame_connected_req && !sgame_connected_req->user);
    ++task_installs; return 0;
}
static int hide_kgsl_install(int pid) {
    assert(pid > 0 && sgame_connected_req && !sgame_connected_req->user);
    assert(task_installs == gpu_installs + 1);
    ++gpu_installs; return gpu_status;
}
static void hide_task_remove(int pid) { assert(pid > 0); ++task_removes; }
static void hide_kgsl_remove(int pid) { assert(pid > 0); ++gpu_removes; }
#define ls_log_always_tag(tag, ...) snprintf(connection_log,sizeof(connection_log),__VA_ARGS__)
#include "../../lsdriver/sgame_session.h"

static void connect_client(struct task_struct *task) {
    get_task_struct(task);
    sgame_connect_client(task);
}
static void *reconnect(void *task) { connect_client(task); return NULL; }

int main(void) {
    struct signal_struct signals[] = {{1}, {1}, {1}};
    struct mm_struct mm[] = {{0}, {0}, {0}};
    struct task_struct tasks[] = {
        {.comm="LS", .start_time=10, .pid=10, .tgid=10, .signal=&signals[0], .mm=&mm[0]},
        {.comm="LS", .start_time=20, .pid=20, .tgid=20, .signal=&signals[1], .mm=&mm[1]},
        {.comm="other", .start_time=30, .pid=30, .tgid=30, .signal=&signals[2], .mm=&mm[2]}
    };
    tasks[0].next = &tasks[1]; tasks[1].next = &tasks[2]; processes = tasks;
    assert(sem_init(&new_map_ready, 0, 0) == 0);

    for (gup_limit = 0; gup_limit < SHARED_PAGES; ++gup_limit) {
        connect_client(&tasks[0]);
        assert(!page_refs && !maps && !tasks[0].refs && !mm[0].refs);
        assert(!ls_process_task && !sgame_connected_req);
    }
    puts("PASS: zero/partial GUP failure releases all page/task/mm references");
    gup_limit = -1; fail_vmap = 1;
    connect_client(&tasks[0]);
    assert(!page_refs && !maps && !tasks[0].refs && !mm[0].refs);
    puts("PASS: vmap failure releases all pinned pages");
    fail_vmap = 0; sgame_stop_requested = true;
    connect_client(&tasks[0]);
    assert(!page_refs && !maps && !tasks[0].refs);
    puts("PASS: shutdown during connection rejects publication and releases mapping");
    sgame_stop_requested = false; signals[0].live = 0;
    connect_client(&tasks[0]);
    assert(!page_refs && !maps && !tasks[0].refs);
    puts("PASS: owner exits during pinning, no stale request published");
    signals[0].live = 1;
    connect_client(&tasks[0]);
    assert(page_refs == SHARED_PAGES && maps == 1 && tasks[0].refs == 1);
    assert(sgame_connected_req->user == 1 && !mm[0].refs);
    sgame_reap_exited_targets();
    assert(ls_process_task == &tasks[0]);
    assert(task_installs == 1 && gpu_installs == 1);
    assert(strstr(connection_log, "task_hide=0 kgsl_hide=0 handshake=ready"));
    puts("PASS: both client hooks execute before handshake publication");
    puts("PASS: live worker group retains client even after leader exit");

    /* Main holds the same lock as a sleeping swap request. New pages can be
     * pinned concurrently, but replacing/releasing the request must wait. */
    mutex_lock(&sgame_session_mutex);
    struct request_obj *old_request = sgame_connected_req;
    const int before = releases;
    pthread_t connector;
    notify_mapping = true;
    assert(pthread_create(&connector, NULL, reconnect, &tasks[1]) == 0);
    assert(sem_wait(&new_map_ready) == 0);
    assert(releases == before && tasks[0].refs == 1 && maps == 2 && page_refs == 2 * SHARED_PAGES);
    old_request->status = 1752;
    __atomic_store_n(&old_request->user, 1, __ATOMIC_RELEASE);
    mutex_unlock(&sgame_session_mutex);
    assert(pthread_join(connector, NULL) == 0);
    notify_mapping = false;
    assert(ls_process_task == &tasks[1] && !tasks[0].refs && tasks[1].refs == 1);
    assert(releases == before + 1 && page_refs == SHARED_PAGES && maps == 1);
    puts("PASS: reconnect waits for sleeping reader and then releases old mapping");

    signals[1].live = 0;
    sgame_reap_exited_targets();
    assert(!ls_process_task && !sgame_connected_req && !page_refs && !maps && !tasks[1].refs);
    assert(cleanups == 2);
    puts("PASS: last worker exit releases request mapping, pages and client task");
    assert(sgame_find_client() == NULL && rcu_depth == 0);
    sgame_client_start_time = 0;
    struct task_struct *newest = sgame_find_client();
    assert(newest == &tasks[0] && newest->refs == 1 && !rcu_depth);
    put_task_struct(newest);
    puts("PASS: RCU discovery excludes old, dead and differently named clients");
    g_syscall_monitor_pids[0] = 999;
    g_syscall_monitor_pids[1] = tasks[0].pid;
    g_cntvct_monitor_tgid = tasks[1].pid;
    sgame_reap_exited_targets();
    assert(!g_syscall_monitor_pids[0] && g_syscall_monitor_pids[1] == tasks[0].pid);
    assert(!g_cntvct_monitor_tgid && !tasks[0].refs && !tasks[1].refs);
    puts("PASS: dead monitor targets removed in sleeping thread context");
    sgame_cleanup_client();
    assert(!page_refs && !maps && !tasks[0].refs && !tasks[1].refs && !tasks[2].refs);
    assert(task_installs == 2 && gpu_installs == 2);
    assert(task_removes == 2 && gpu_removes == 2);
    gpu_status = -95;
    connect_client(&tasks[0]);
    assert(sgame_connected_req->user == 1);
    assert(strstr(connection_log,"kgsl_hide=-95 handshake=ready"));
    sgame_cleanup_client();
    assert(task_removes == 3 && gpu_removes == 3 && !page_refs && !maps);
    puts("PASS: upstream best-effort GPU hook failure is observable and cleaned up");
    assert(sem_destroy(&new_map_ready) == 0);
    return 0;
}
