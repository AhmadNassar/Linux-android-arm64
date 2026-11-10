/* Run the production environment reader against explicit kernel API substitutes. */
#include <assert.h>
#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include "../../lsdriver/sgame_simulation/abi.h"

typedef uint64_t u64;
typedef uint32_t u32;
#ifndef SGAME_SIM_IDS_510
#define SGAME_SIM_IDS_510 0
#endif
#define __kuid_val(v) (v)
#define __kgid_val(v) (v)
#define READ_ONCE(v) (v)
#define PIDTYPE_PID 0
#define ERR_PTR(e) ((void *)(intptr_t)(e))
#define PTR_ERR(p) ((long)(intptr_t)(p))
#define IS_ERR(p) ((uintptr_t)(p) >= (uintptr_t)-4095)
#define STATX_BASIC_STATS 0x7ff
#define AT_STATX_SYNC_AS_STAT 0
struct dentry { struct { const char *name; } d_name; };
struct path { struct dentry *dentry; };
struct inode { unsigned mode_pad; unsigned i_mode; int64_t size; };
struct file { struct path f_path; struct inode inode; int refs; int64_t f_pos; uint64_t value; };
struct fdtable { unsigned max_fds; struct file *fd[4]; };
struct files_struct { int file_lock; struct fdtable table; };
struct signal_struct { int cred_guard_mutex; };
struct task_struct { uint64_t start_boottime; struct files_struct *files; struct signal_struct *signal; int refs, locked; };
struct kstat {
    uint64_t dev, ino, rdev; unsigned mode, nlink, uid, gid;
    int64_t size, blksize, blocks;
    struct { int64_t tv_sec, tv_nsec; } atime, mtime, ctime;
};
static struct file original, replacement;
static struct files_struct files;
static struct signal_struct signal_state;
static struct task_struct task;
static struct kstat stat_result;
static struct dentry name;
static int rcu_depth, mutex_error, read_calls, stat_calls, stat_error;
static ssize_t read_result;
static int present = 1, replace_during_read;
static const char *missing_symbol;
static int overflow_uid = 61234, overflow_gid = 62333;

static void rcu_read_lock(void) { assert(!rcu_depth); ++rcu_depth; }
static void rcu_read_unlock(void) { assert(rcu_depth == 1); --rcu_depth; }
static int find_vpid(int pid) { assert(rcu_depth); return pid; }
static struct task_struct *pid_task(int pid, int type) { assert(rcu_depth && type == PIDTYPE_PID); return present && pid == 42 ? &task : NULL; }
static void get_task_struct(struct task_struct *t) { assert(rcu_depth); ++t->refs; }
static void put_task_struct(struct task_struct *t) { assert(t->refs > 1 && !t->locked); --t->refs; }
static int mutex_lock_killable(int *lock) { assert(!rcu_depth && !*lock); if (mutex_error) return mutex_error; *lock = 1; return 0; }
static void mutex_unlock(int *lock) { assert(*lock == 1); *lock = 0; }
static void task_lock(struct task_struct *t) { assert(signal_state.cred_guard_mutex && !t->locked); t->locked = 1; }
static void task_unlock(struct task_struct *t) { assert(t->locked && !files.file_lock); t->locked = 0; }
static void spin_lock(int *lock) { assert(task.locked && !*lock); *lock = 1; }
static void spin_unlock(int *lock) { assert(*lock); *lock = 0; }
#define files_fdtable(f) (&(f)->table)
#define rcu_dereference_raw(p) (p)
static void get_file(struct file *f) { assert(files.file_lock); ++f->refs; }
static void fput(struct file *f) { assert(!files.file_lock && !task.locked && !signal_state.cred_guard_mutex && f->refs == 2); --f->refs; }
#define file_inode(f) (&(f)->inode)
#define i_size_read(i) ((i)->size)
static ssize_t kernel_read(struct file *f, void *out, size_t bytes, loff_t *position) {
    assert(!rcu_depth && !task.locked && !signal_state.cred_guard_mutex && !files.file_lock);
    assert(f == &original && f->refs == 2 && bytes == 8 && *position == 0);
    ++read_calls;
    if (replace_during_read) files.table.fd[3] = &replacement;
    if (read_result > 0) { memcpy(out, &f->value, read_result); *position += read_result; }
    return read_result;
}
static uint64_t huge_encode_dev(uint64_t dev) { return dev + 0x100000000ULL; }
#if !SGAME_SIM_IDS_510
static void *current_user_ns(void) { return &task; }
static unsigned from_kuid_munged(void *ns, unsigned uid) { assert(ns == &task); return uid + 10; }
static unsigned from_kgid_munged(void *ns, unsigned gid) { assert(ns == &task); return gid + 20; }
#endif
static int vfs_getattr(const struct path *p, struct kstat *out, unsigned mask, unsigned flags) {
    assert(p == &original.f_path && original.refs == 2 && !task.locked && !files.file_lock);
    assert(mask == STATX_BASIC_STATS && flags == AT_STATX_SYNC_AS_STAT);
    ++stat_calls; *out = stat_result; return stat_error;
}
unsigned long sgame_sim_lookup_name(const char *symbol) {
    if (missing_symbol && !strcmp(missing_symbol, symbol)) return 0;
    if (!strcmp(symbol, "overflowuid")) return (unsigned long)&overflow_uid;
    if (!strcmp(symbol, "overflowgid")) return (unsigned long)&overflow_gid;
    if (!strcmp(symbol, "kernel_read")) return (unsigned long)kernel_read;
    assert(!strcmp(symbol, "vfs_getattr")); return (unsigned long)vfs_getattr;
}

#include "../../lsdriver/sgame_simulation/environment.c"

static void balanced(void) {
    assert(task.refs == 1 && original.refs == 1 && replacement.refs == 1);
    assert(!rcu_depth && !files.file_lock && !task.locked && !signal_state.cred_guard_mutex);
    assert(original.f_pos == 77 && replacement.f_pos == 88);
}
static void reset(void) {
    name.d_name.name = "memfd:environment";
    original = (struct file){.f_path = {&name}, .inode = {.i_mode = S_IFREG | 0600, .size = 8}, .refs = 1, .f_pos = 77, .value = 0x1029384756abcdefULL};
    replacement = (struct file){.refs = 1, .f_pos = 88, .value = 99};
    files = (struct files_struct){.table = {.max_fds = 4, .fd = {NULL, NULL, NULL, &original}}};
    signal_state = (struct signal_struct){0};
    task = (struct task_struct){.start_boottime = 123456789, .files = &files, .signal = &signal_state, .refs = 1};
    read_calls = stat_calls = stat_error = mutex_error = replace_during_read = 0;
    read_result = 8; present = 1;
    missing_symbol = NULL;
    stat_result = (struct kstat){.dev = 17, .ino = 0x123456789ULL, .mode = 0600, .nlink = 2,
        .uid = 1000, .gid = 1001, .rdev = 18, .size = 8, .blksize = 4096, .blocks = 1,
        .atime = {-7, 11}, .mtime = {12, 13}, .ctime = {14, 15}};
}
int main(void) {
    reset();
    struct sgame_sim_process_req process = {.pid = 42};
    assert(!sgame_sim_mem_process(&process) && process.out_start_time == task.start_boottime); balanced();
    process.pid = 43; assert(sgame_sim_mem_process(&process) == -ESRCH); balanced();
    struct sgame_sim_file_read8_req read = {.pid = 42, .fd = 3, .start_time = task.start_boottime};
    replace_during_read = 1;
    assert(!sgame_sim_mem_file_read8(&read) && read.out_value == original.value && read_calls == 1);
    assert(files.table.fd[3] == &replacement); balanced();
    reset(); read.start_time++;
    assert(sgame_sim_mem_file_read8(&read) == -ESRCH && !read_calls); balanced(); read.start_time--;
    for (int fd = -1; fd <= 4; ++fd) if (fd != 3) {
        read.fd = fd; assert(sgame_sim_mem_file_read8(&read) == -EBADF); balanced();
    }
    read.fd = 3; task.files = NULL;
    assert(sgame_sim_mem_file_read8(&read) == -EBADF); balanced(); task.files = &files;
    mutex_error = -EINTR; assert(sgame_sim_mem_file_read8(&read) == -EINTR); balanced(); mutex_error = 0;
    present = 0; assert(sgame_sim_mem_file_read8(&read) == -ESRCH); balanced(); present = 1;
    original.inode.size = 9; assert(sgame_sim_mem_file_read8(&read) == -EPROTO); balanced(); original.inode.size = 8;
    original.inode.i_mode = S_IFCHR; assert(sgame_sim_mem_file_read8(&read) == -EPROTO); balanced(); original.inode.i_mode = S_IFREG;
    name.d_name.name = "ordinary"; assert(sgame_sim_mem_file_read8(&read) == -EPROTO && !read_calls); balanced(); name.d_name.name = "memfd:environment";
    for (ssize_t bytes = -EIO; bytes < 8; ++bytes) {
        if (bytes < 0 && bytes != -EIO) continue;
        read_result = bytes;
        assert(sgame_sim_mem_file_read8(&read) == -EIO); balanced();
    }
    struct sgame_sim_file_stat_req req = {.pid = 42, .fd = 3, .start_time = task.start_boottime};
    memset(&req.out_stat, 0xff, sizeof req.out_stat);
    assert(!sgame_sim_mem_file_stat(&req) && stat_calls == 1); balanced();
    assert(req.out_stat.dev == 0x100000011ULL && req.out_stat.rdev == 0x100000012ULL);
    assert(req.out_stat.ino == stat_result.ino);
    assert(req.out_stat.uid == (SGAME_SIM_IDS_510 ? 1000 : 1010));
    assert(req.out_stat.gid == (SGAME_SIM_IDS_510 ? 1001 : 1021));
    assert(req.out_stat.size == 8 && req.out_stat.blksize == 4096 && req.out_stat.blocks == 1);
    assert(req.out_stat.atime_sec == -7 && req.out_stat.atime_ns == 11 && req.out_stat.mtime_sec == 12 && req.out_stat.ctime_ns == 15);
    assert(!req.out_stat.pad0 && !req.out_stat.pad1 && !req.out_stat.unused);
    const struct sgame_sim_stat before = req.out_stat;
    stat_error = -EACCES; assert(sgame_sim_mem_file_stat(&req) == -EACCES);
    assert(!memcmp(&req.out_stat, &before, sizeof before)); balanced();
    missing_symbol = "vfs_getattr";
    assert(sgame_sim_mem_file_stat(&req) == -EOPNOTSUPP); balanced();
    missing_symbol = "kernel_read";
    assert(sgame_sim_mem_file_read8(&read) == -EOPNOTSUPP); balanced();
#if SGAME_SIM_IDS_510
    reset();
    missing_symbol = "overflowgid";
    assert(!sgame_sim_mem_file_stat(&req) && req.out_stat.size == 8 && req.out_stat.gid == 1001); balanced();
    stat_result.gid = (u32)-1;
    assert(sgame_sim_mem_file_stat(&req) == -EOPNOTSUPP); balanced();
    missing_symbol = NULL;
    assert(!sgame_sim_mem_file_stat(&req) && req.out_stat.gid == (u32)overflow_gid); balanced();
    stat_result.uid = (u32)-1;
    assert(!sgame_sim_mem_file_stat(&req) && req.out_stat.uid == (u32)overflow_uid); balanced();
    puts("PASS: 5.10 stat IDs use configured overflow values; missing overflow symbol does not gate ordinary file stat");
#endif
    puts("environment: process identity, one pinned file/read, FD replacement, unchanged f_pos, ARM64 stat and error/reference paths passed");
}
