/* Compile production dispatch and target-PID KGSL code with kernel API fixtures.
 * No device, target process, kernel module or userspace syscall is involved. */
#include <assert.h>
#include <errno.h>
#include <limits.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../../lsdriver/sgame_simulation/internal.h"
#include "../../lsdriver/sgame_simulation/symbol_span.h"

typedef uint32_t u32;
typedef int32_t s32;
struct fixture_lock { int held; };
struct file { int refs; void *private_data; };
struct fdtable { unsigned int max_fds; struct file **fd; };
struct files_struct { struct fixture_lock file_lock; struct fdtable table; };
struct signal_struct { struct fixture_lock cred_guard_mutex; };
struct task_struct {
    int refs;
    struct fixture_lock lock;
    struct signal_struct *signal;
    struct files_struct *files;
};
struct kgsl_device_private { int marker; };
struct kgsl_runtime_symbols { unsigned long fops; };
enum kgsl_peek_fail_stage {
    KGSL_PEEK_FAIL_NONE = 0,
    KGSL_PEEK_FAIL_FILE_IDENTITY = 1,
    KGSL_PEEK_FAIL_NOT_KGSL_FILE = 2,
    KGSL_PEEK_FAIL_LOWEST_FREE = 10,
};

#define SGAME_SIM_HOST_TEST
#define GFP_KERNEL 0
#define MAX_ERRNO 4095
#define PIDTYPE_PID 0
#define BUILD_BUG_ON(x) _Static_assert(!(x), #x)
#define DEFINE_MUTEX(name) struct fixture_lock name
#define READ_ONCE(x) (x)
#define smp_load_acquire(p) (*(p))
#define smp_store_release(p, v) (*(p) = (v))
#define ERR_PTR(e) ((void *)(intptr_t)(e))
#define IS_ERR(p) ((uintptr_t)(p) >= (uintptr_t)-MAX_ERRNO)
#define PTR_ERR(p) ((long)(intptr_t)(p))
#define rcu_dereference_raw(p) (p)
#define find_vpid(pid) (pid)
#define files_fdtable(files) (&(files)->table)

static int rcu_depth, allocs, fail_alloc, handler_calls, last_command;
static int identity_error, lock_error, target_missing, wrong_file;
static int native_available = 1, native_calls, peek_calls;
static int file_gets, file_puts, task_gets, task_puts, peek_error;
static long native_result;
static struct kgsl_device_private device;
static struct file target_file;
static struct file *fd_array[8];
static struct files_struct target_files;
static struct signal_struct target_signal;
static struct task_struct target_task;
static struct sgame_sim_packet *mutable_packet;
static uint32_t *mutable_extra;
static int memory_error;

static void lock(struct fixture_lock *l) { assert(!l->held); l->held = 1; }
static void unlock(struct fixture_lock *l) { assert(l->held); l->held = 0; }
#define mutex_lock(p) lock(p)
#define mutex_unlock(p) unlock(p)
#define spin_lock(p) lock(p)
#define spin_unlock(p) unlock(p)
#define task_lock(p) lock(&(p)->lock)
#define task_unlock(p) unlock(&(p)->lock)
static int mutex_lock_killable(struct fixture_lock *l)
{
    if (lock_error) return lock_error;
    lock(l);
    return 0;
}
static void rcu_read_lock(void) { ++rcu_depth; }
static void rcu_read_unlock(void) { assert(rcu_depth > 0); --rcu_depth; }
static struct task_struct *pid_task(int pid, int type)
{
    assert(rcu_depth && type == PIDTYPE_PID);
    return pid == 77 && !target_missing ? &target_task : NULL;
}
static void get_task_struct(struct task_struct *t) { ++t->refs; ++task_gets; }
static void put_task_struct(struct task_struct *t)
{
    assert(t->refs == 2);
    --t->refs;
    ++task_puts;
}
static void get_file(struct file *f)
{
    assert(target_task.lock.held && target_files.file_lock.held);
    assert(target_signal.cred_guard_mutex.held);
    ++f->refs;
    ++file_gets;
}
static void fput(struct file *f) { assert(f->refs == 2); --f->refs; ++file_puts; }
static void *kmemdup(const void *src, size_t bytes, int flags)
{
    assert(flags == GFP_KERNEL && bytes <= SGAME_SIM_EXTRA_BYTES);
    if (fail_alloc) return NULL;
    void *p = malloc(bytes);
    assert(p);
    memcpy(p, src, bytes);
    ++allocs;
    return p;
}
static void kfree(void *p) { if (p) { assert(allocs > 0); --allocs; free(p); } }
static int kgsl_resolve_file_identity(struct kgsl_runtime_symbols *symbols)
{
    symbols->fops = 1;
    return identity_error;
}
static bool kgsl_file_matches(const struct file *f, const struct kgsl_runtime_symbols *s)
{
    assert(f == &target_file && s->fops == 1 && f->refs == 2);
    return !wrong_file;
}
static long native_info(struct kgsl_device_private *priv, unsigned int cmd, void *data)
{
    struct dfm_kgsl_gpuobj_info *info = data;
    assert(priv == &device && target_file.refs == 2);
    assert(cmd == _IOWR(0x09, 0x47, struct dfm_kgsl_gpuobj_info));
    assert(!target_files.file_lock.held && !target_task.lock.held);
    assert(!target_signal.cred_guard_mutex.held);
    ++native_calls;
    if (native_result) return native_result;
    info->size = 0x3000;
    info->gpuaddr = 0x12345000;
    info->padding = 0xaaaaaaaa;
    return 0;
}
unsigned long sgame_sim_lookup_name(const char *name)
{
    assert(!strcmp(name, "kgsl_ioctl_gpuobj_info"));
    return native_available ? (unsigned long)(uintptr_t)native_info : 0;
}
static unsigned long kgsl_lookup_cfi_symbol(const char *name)
{
    return sgame_sim_lookup_name(name);
}
static int kgsl_syncsource_peek_file(struct file *f, u32 min_id,
    const u32 *excluded, u32 count, u32 *out, enum kgsl_peek_fail_stage *stage)
{
    assert(f->refs == 2 && file_gets == file_puts + 1);
    assert(!target_task.lock.held && !target_files.file_lock.held);
    ++peek_calls;
    if (mutable_extra) {
        assert(excluded != mutable_extra);
        memset(mutable_extra, 0, count * sizeof(*excluded));
    }
    if (mutable_packet) mutable_packet->payload.kgsl.min_id = INT_MAX;
    if (peek_error) { *stage = KGSL_PEEK_FAIL_LOWEST_FREE; return peek_error; }
    u32 candidate = min_id;
    for (u32 i = 0; i < count; ++i)
        if (excluded[i] == candidate) ++candidate;
    *out = candidate;
    return 0;
}

#include "../../lsdriver/sgame_simulation/kgsl_query.h"
#include "../../lsdriver/sgame_simulation/dispatch.c"

static long memory_handler(uint32_t cmd, void *data)
{
    ++handler_calls;
    last_command = cmd;
    if (cmd == MEM_IOC_GET_TLS) {
        struct mem_get_tls_req *req = data;
        int pid = req->pid;
        if (mutable_packet) mutable_packet->payload.get_tls.pid = 999;
        assert(req->pid == pid); /* the handler owns a private snapshot */
        req->out_tls = (uint64_t)pid * 100;
    }
    if (cmd == MEM_IOC_GET_THREAD_TID)
        ((struct mem_get_thread_tid_req *)data)->out_tid = 99;
    if (cmd == MEM_IOC_GET_THREAD_TIDS) {
        struct mem_get_thread_tids_req *req = data;
        req->out_count = req->out_total = 2;
        req->out_threads[0].tid = 88;
        req->out_threads[1].tid = 99;
        req->out_threads[1].flags = MEM_THREAD_TID_F_ON_CPU;
        req->out_threads[1].exec_ns = 1000000000;
    }
    if (cmd == SGAME_SIM_COMMAND_PROCESS)
        ((struct sgame_sim_process_req *)data)->out_start_time = 123456789;
    if (cmd == SGAME_SIM_COMMAND_FILE_READ8)
        ((struct sgame_sim_file_read8_req *)data)->out_value = 0x1029384756abcdefULL;
    if (cmd == SGAME_SIM_COMMAND_FILE_STAT) {
        struct sgame_sim_stat *out = &((struct sgame_sim_file_stat_req *)data)->out_stat;
        out->dev = 0x123456789ULL;
        out->ino = 0xabcdef01ULL;
        out->blksize = 4096;
        out->mtime_sec = -123;
    }
    return memory_error;
}
void sgame_fixture_memory_error(int error) { memory_error = error; }
#define MEMORY_HANDLER(command, name, type) \
    long sgame_sim_mem_##name(struct type *req) { return memory_handler(command, req); }
SGAME_SIM_MEMORY_COMMANDS(MEMORY_HANDLER)
#undef MEMORY_HANDLER

void sgame_fixture_reset(void)
{
    assert(!allocs && !rcu_depth);
    memset(&target_file, 0, sizeof(target_file));
    memset(&target_task, 0, sizeof(target_task));
    memset(&target_files, 0, sizeof(target_files));
    memset(&target_signal, 0, sizeof(target_signal));
    memset(fd_array, 0, sizeof(fd_array));
    target_file.refs = target_task.refs = 1;
    target_file.private_data = &device;
    fd_array[3] = &target_file;
    target_files.table = (struct fdtable) {8, fd_array};
    target_task.signal = &target_signal;
    target_task.files = &target_files;
    fail_alloc = handler_calls = last_command = memory_error = 0;
    identity_error = lock_error = target_missing = wrong_file = 0;
    native_result = native_calls = peek_calls = peek_error = 0;
    file_gets = file_puts = task_gets = task_puts = 0;
    native_available = 1;
    mutable_packet = NULL;
    mutable_extra = NULL;
    g_kgsl_query_ready = false;
    g_kgsl_query_info = NULL;
}

#ifndef SGAME_SIM_TEST_LIBRARY
static void balanced(void)
{
    assert(file_gets == file_puts && task_gets == task_puts);
    assert(target_file.refs == 1 && target_task.refs == 1 && !allocs && !rcu_depth);
    assert(!target_task.lock.held && !target_files.file_lock.held);
    assert(!target_signal.cred_guard_mutex.held);
}
static struct sgame_sim_packet packet(uint32_t cmd)
{
    struct sgame_sim_packet p = {0};
    p.header = (struct sgame_sim_header) {SGAME_SIM_ABI_VERSION, cmd, sgame_sim_payload_bytes(cmd), 0};
    return p;
}
static struct sgame_sim_packet kgsl(uint32_t op)
{
    struct sgame_sim_packet p = packet(DFM_IOCTL_KGSL_QUERY);
    p.payload.kgsl = (struct dfm_kgsl_query_req) {
        .abi_version = DFM_KGSL_QUERY_ABI_VERSION, .op = op, .pid = 77, .target_fd = 3};
    return p;
}
static int dispatch(struct sgame_sim_packet *p, const void *extra, size_t bytes)
{
    int ret = sgame_sim_dispatch(p, sizeof(*p), extra, bytes);
    balanced();
    return ret;
}
static void test_protocol(void)
{
    struct sgame_sim_packet p = packet(SGAME_SIM_COMMAND_CAPS);
    assert(!dispatch(&p, NULL, 0));
    assert(p.payload.caps.packet_bytes == 1312);
    assert(p.payload.caps.max_excluded_ids == 4096);
    assert(p.payload.caps.capabilities == SGAME_SIM_CAP_ALL);
    p.header.abi_version++;
    assert(dispatch(&p, NULL, 0) == -EPROTONOSUPPORT);
    p = packet(MEM_IOC_GET_TLS);
    p.header.reserved = 1;
    assert(dispatch(&p, NULL, 0) == -EINVAL);
    p.header.reserved = 0;
    p.header.payload_bytes = UINT_MAX;
    assert(dispatch(&p, NULL, 0) == -EMSGSIZE);
    p.header.command = 0xdeadbeef;
    assert(dispatch(&p, NULL, 0) == -ENOTTY);
    assert(!handler_calls);
    assert(sgame_sim_dispatch(&p, 15, NULL, 0) == -EMSGSIZE);
    p = packet(MEM_IOC_GET_TLS);
    assert(sgame_sim_dispatch(&p, 31, NULL, 0) == -EMSGSIZE);
    p.payload.get_tls.pid = 77;
    mutable_packet = &p;
    assert(!dispatch(&p, NULL, 0));
    assert(p.payload.get_tls.out_tls == 7700 && p.payload.get_tls.pid == 77);
    assert(last_command == (int)MEM_IOC_GET_TLS && handler_calls == 1);
    memory_error = -ESRCH;
    assert(dispatch(&p, NULL, 0) == -ESRCH);
    assert(!p.payload.get_tls.out_tls);
    mutable_packet = NULL;
}
static void test_target_file_and_info(void)
{
    sgame_fixture_reset();
    struct sgame_sim_packet p = kgsl(DFM_KGSL_QUERY_VALIDATE_FD);
    assert(!dispatch(&p, NULL, 0));
    assert(p.payload.kgsl.out_backend == DFM_KGSL_QUERY_BACKEND_FILE && file_gets == 1);
    p.payload.kgsl.target_fd = 7;
    assert(!dispatch(&p, NULL, 0));
    assert(p.payload.kgsl.out_errno == EBADF && !p.payload.kgsl.out_fail_stage);
    p.payload.kgsl.target_fd = UINT_MAX;
    assert(!dispatch(&p, NULL, 0) && p.payload.kgsl.out_errno == EBADF);
    p.payload.kgsl.pid = 999;
    assert(dispatch(&p, NULL, 0) == -ESRCH);
    assert(p.payload.kgsl.out_fail_stage == DFM_KGSL_QUERY_FAIL_TASK);
    p = kgsl(DFM_KGSL_QUERY_VALIDATE_FD);
    lock_error = -EINTR;
    assert(dispatch(&p, NULL, 0) == -EINTR);
    lock_error = 0;
    wrong_file = 1;
    assert(dispatch(&p, NULL, 0) == -ENODEV);
    assert(p.payload.kgsl.out_fail_stage == 2);
    wrong_file = 0;
    p = kgsl(DFM_KGSL_QUERY_GPUOBJ_INFO);
    p.payload.kgsl.object_id = 17;
    assert(!dispatch(&p, NULL, 0));
    assert(native_calls == 1 && p.payload.kgsl.out_info.id == 17);
    assert(p.payload.kgsl.out_info.size == 0x3000 && !p.payload.kgsl.out_info.padding);
    native_result = -EINVAL;
    assert(!dispatch(&p, NULL, 0));
    assert(p.payload.kgsl.out_errno == EINVAL && !p.payload.kgsl.out_info.size);
    native_result = 1;
    assert(dispatch(&p, NULL, 0) == -EPROTO);
    assert(p.payload.kgsl.out_fail_stage == DFM_KGSL_QUERY_FAIL_INFO_RESULT);
    sgame_fixture_reset();
    native_available = 0;
    assert(dispatch(&p, NULL, 0) == -EOPNOTSUPP);
    assert(p.payload.kgsl.out_fail_stage == DFM_KGSL_QUERY_FAIL_INFO_SYMBOL);
    sgame_fixture_reset();
    identity_error = -EOPNOTSUPP;
    assert(dispatch(&p, NULL, 0) == -EOPNOTSUPP && !file_gets);
    assert(p.payload.kgsl.out_fail_stage == 1);
}
static void test_excluded_snapshot(void)
{
    sgame_fixture_reset();
    struct sgame_sim_packet p = kgsl(DFM_KGSL_QUERY_SYNCSOURCE_PEEK);
    uint32_t ids[4096];
    for (unsigned i = 0; i < 4096; ++i) ids[i] = i + 1;
    p.payload.kgsl.min_id = 1;
    p.payload.kgsl.excluded_count = 4096;
    mutable_packet = &p;
    mutable_extra = ids;
    assert(!dispatch(&p, ids, sizeof(ids)));
    assert(p.payload.kgsl.out_id == 4097 && p.payload.kgsl.min_id == 1);
    assert(!ids[0] && file_gets == 1 && file_puts == 1 && peek_calls == 1);
    mutable_packet = NULL;
    mutable_extra = NULL;
    assert(dispatch(&p, ids, sizeof(ids)) == -EINVAL); /* now zeroed IDs */
    assert(file_gets == 1); /* invalid arrays never acquire a target file */
    p.payload.kgsl.excluded_ids = 0x1234;
    assert(dispatch(&p, ids, sizeof(ids)) == -EINVAL);
    p.payload.kgsl.excluded_ids = 0;
    p.payload.kgsl.excluded_count = 4097;
    assert(dispatch(&p, ids, sizeof(ids)) == -EINVAL);
    p.payload.kgsl.excluded_count = 4096;
    assert(dispatch(&p, ids, sizeof(ids) - 1) == -EMSGSIZE);
    fail_alloc = 1;
    assert(dispatch(&p, ids, sizeof(ids)) == -ENOMEM);
    fail_alloc = 0;
    p.payload.kgsl.excluded_count = 0;
    peek_error = -ENOSPC;
    assert(!dispatch(&p, NULL, 0));
    assert(p.payload.kgsl.out_errno == ENOSPC && !p.payload.kgsl.out_fail_stage);
    peek_error = -EAGAIN;
    assert(dispatch(&p, NULL, 0) == -EAGAIN);
    assert(p.payload.kgsl.out_fail_stage == KGSL_PEEK_FAIL_LOWEST_FREE);
}
static void test_symbol_spans(void)
{
    unsigned long size, offset;
    assert(sgame_sim_parse_symbol_span("foo+0/0x40 [kgsl]", &size, &offset));
    assert(size == 64 && !offset);
    assert(sgame_sim_parse_symbol_span("foo.cfi_jt+0x4/0x10", &size, &offset));
    assert(size == 16 && offset == 4);
    assert(!sgame_sim_parse_symbol_span("0xffff888800000000", &size, &offset));
    assert(!sgame_sim_parse_symbol_span("foo+0x40/0x40", &size, &offset));
    assert(!sgame_sim_parse_symbol_span("foo+0/0", &size, &offset));
    assert(!sgame_sim_parse_symbol_span("foo+0x/0x40", &size, &offset));
    assert(!sgame_sim_parse_symbol_span("foo+0/0x10000000000000000", &size, &offset));
}
int main(void)
{
    _Static_assert(sizeof(struct mem_query_mapping_req) == 40, "mapping ABI");
    _Static_assert(sizeof(struct mem_query_signal_req) == 72, "signal ABI");
    _Static_assert(sizeof(struct mem_get_thread_tids_req) == 1296, "threads ABI");
    _Static_assert(sizeof(struct dfm_kgsl_query_req) == 120, "KGSL ABI");
    _Static_assert(offsetof(struct dfm_kgsl_query_req, out_info) == 56, "KGSL INFO offset");
    _Static_assert(offsetof(struct sgame_sim_packet, payload) == 16, "packet ABI");
    sgame_fixture_reset();
    test_protocol();
    test_target_file_and_info();
    test_excluded_snapshot();
    test_symbol_spans();
    balanced();
    puts("simulation: ABI, dispatch snapshots, target file references, KGSL errors and 4096 IDs passed");
}
#endif
