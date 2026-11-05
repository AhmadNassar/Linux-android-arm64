/* Target PID/FD query layer; included by kgsl.c and the host fixture. */
#ifndef SGAME_SIM_KGSL_QUERY_H
#define SGAME_SIM_KGSL_QUERY_H

/* These forward declarations intentionally match the vendor handler's CFI
 * type. Never cast a file ioctl callback to the internal INFO signature. */
struct kgsl_device_private;
typedef long (*kgsl_info_fn)(struct kgsl_device_private *, unsigned int, void *);

static DEFINE_MUTEX(g_kgsl_query_runtime_lock);
static struct kgsl_runtime_symbols g_kgsl_query_identity;
static kgsl_info_fn g_kgsl_query_info;
static bool g_kgsl_query_ready;

static unsigned long kgsl_callable_symbol(const char *name)
{
    unsigned long jt = kgsl_lookup_cfi_symbol(name);

    /* Preserve the CFI jump-table entry for an indirect call on older GKI.
     * The unwrapped text addresses above are for fingerprinting, not calls. */
    return jt ? jt : sgame_sim_lookup_name(name);
}

static int kgsl_query_runtime(u32 *fail_stage)
{
    int ret = 0;

    if (smp_load_acquire(&g_kgsl_query_ready))
        return 0;
    mutex_lock(&g_kgsl_query_runtime_lock);
    if (g_kgsl_query_ready)
        goto out;
    ret = kgsl_resolve_file_identity(&g_kgsl_query_identity);
    if (ret) {
        *fail_stage = KGSL_PEEK_FAIL_FILE_IDENTITY;
        goto out;
    }
    /* Optional: absence must not disable VALIDATE_FD or SYNCSOURCE_PEEK. */
    g_kgsl_query_info = (kgsl_info_fn)
        kgsl_callable_symbol("kgsl_ioctl_gpuobj_info");
    smp_store_release(&g_kgsl_query_ready, true);
out:
    mutex_unlock(&g_kgsl_query_runtime_lock);
    return ret;
}

static struct file *kgsl_target_file(int pid, u32 fd, u32 *fail_stage)
{
    struct task_struct *task;
    struct file *file = NULL;
    struct files_struct *files;
    struct fdtable *table;
    int ret;

    *fail_stage = DFM_KGSL_QUERY_FAIL_TASK;
    rcu_read_lock();
    task = pid_task(find_vpid(pid), PIDTYPE_PID);
    if (task)
        get_task_struct(task);
    rcu_read_unlock();
    if (!task)
        return ERR_PTR(-ESRCH);

    /* The existing lsdriver connection grants access. Query the target fd
     * directly; no caller-owned fd is opened or installed.
     * Serialize the fd lookup with exec/credential changes. Unlike
     * lockless hand-written file refcounting, the locked fdtable lookup also
     * works on kernels whose file slab uses SLAB_TYPESAFE_BY_RCU. */
    ret = mutex_lock_killable(&task->signal->cred_guard_mutex);
    if (ret)
        goto out_task;
    *fail_stage = DFM_KGSL_QUERY_FAIL_FD;
    ret = -EBADF;
    task_lock(task);
    files = task->files;
    if (files) {
        spin_lock(&files->file_lock);
        table = files_fdtable(files);
        if (fd <= INT_MAX && fd < table->max_fds) {
            file = rcu_dereference_raw(table->fd[fd]);
            if (file)
                get_file(file);
        }
        spin_unlock(&files->file_lock);
    } else {
        ret = -ESRCH;
    }
    task_unlock(task);
    mutex_unlock(&task->signal->cred_guard_mutex);
out_task:
    put_task_struct(task);
    return file ? file : ERR_PTR(ret);
}

static int kgsl_query_gpuobj(struct file *file, struct dfm_kgsl_query_req *req)
{
    struct dfm_kgsl_gpuobj_info info = { .id = req->object_id };
    struct kgsl_device_private *dev_priv;
    long result;

    req->out_fail_stage = DFM_KGSL_QUERY_FAIL_INFO_SYMBOL;
    if (!g_kgsl_query_info)
        return -EOPNOTSUPP;
    dev_priv = READ_ONCE(file->private_data);
    if (!dev_priv) {
        req->out_fail_stage = KGSL_PEEK_FAIL_NOT_KGSL_FILE;
        return -ENODEV;
    }

    /* Native INFO obtains/releases its own temporary mem_entry reference and
     * reads the vendor-specific memdesc fields (including footprint/guard-page
     * rules). Only this fixed read-only handler can be invoked. There is no
     * user command, user pointer, allocation/import or SET_INFO path here.
     * The file reference keeps dev_priv and its owning module alive. */
    result = g_kgsl_query_info(dev_priv,
        _IOWR(0x09, 0x47, struct dfm_kgsl_gpuobj_info), &info);
    req->out_fail_stage = DFM_KGSL_QUERY_FAIL_INFO_RESULT;
    if (result < -MAX_ERRNO || result > 0)
        return -EPROTO;
    if (result < 0) {
        req->out_errno = (s32)-result;
    } else {
        if (info.id != req->object_id || info.id == 0)
            return -EPROTO;
        info.padding = 0;
        req->out_info = info;
    }
    req->out_backend = DFM_KGSL_QUERY_BACKEND_NATIVE_INFO;
    req->out_fail_stage = 0;
    return 0;
}

long sgame_sim_kgsl_query(struct dfm_kgsl_query_req *arg, const u32 *excluded)
{
    struct dfm_kgsl_query_req *req = arg;
    struct file *file;
    u32 i;
    int ret;
    enum kgsl_peek_fail_stage stage = KGSL_PEEK_FAIL_NONE;

    BUILD_BUG_ON(sizeof(*req) != 120U);
    BUILD_BUG_ON(offsetof(struct dfm_kgsl_query_req, out_info) != 56U);
    BUILD_BUG_ON(sizeof(struct dfm_kgsl_gpuobj_info) != 48U);
    req->out_id = req->out_backend = req->out_errno = req->out_fail_stage = 0;
    memset(&req->out_info, 0, sizeof(req->out_info));
    if (req->abi_version != DFM_KGSL_QUERY_ABI_VERSION || req->pid <= 0 ||
        req->flags || req->reserved[0] || req->reserved[1])
        return -EINVAL;
    if (req->op < DFM_KGSL_QUERY_VALIDATE_FD ||
        req->op > DFM_KGSL_QUERY_GPUOBJ_INFO)
        return -EINVAL;
    if (req->op == DFM_KGSL_QUERY_SYNCSOURCE_PEEK) {
        if (req->object_id || !req->min_id || req->min_id > INT_MAX ||
            req->excluded_count > DFM_KGSL_QUERY_MAX_EXCLUDED_IDS ||
            req->excluded_ids || (req->excluded_count && !excluded))
            return -EINVAL;
        if (req->excluded_count) {
            for (i = 0; i < req->excluded_count; i++) {
                if (!excluded[i] || excluded[i] > INT_MAX ||
                    (i && excluded[i] <= excluded[i - 1U])) {
                    ret = -EINVAL;
                    goto out;
                }
            }
        }
    } else if (req->min_id || req->excluded_count || req->excluded_ids ||
               (req->op == DFM_KGSL_QUERY_VALIDATE_FD && req->object_id)) {
        return -EINVAL;
    }

    ret = kgsl_query_runtime(&req->out_fail_stage);
    if (ret)
        goto out;
    file = kgsl_target_file(req->pid, req->target_fd, &req->out_fail_stage);
    if (IS_ERR(file)) {
        ret = PTR_ERR(file);
        if (ret == -EBADF) {
            req->out_errno = EBADF;
            req->out_fail_stage = 0;
            ret = 0;
        }
        goto out;
    }
    req->out_fail_stage = KGSL_PEEK_FAIL_NOT_KGSL_FILE;
    if (!kgsl_file_matches(file, &g_kgsl_query_identity)) {
        ret = -ENODEV;
        goto out_file;
    }
    req->out_fail_stage = 0;
    switch (req->op) {
    case DFM_KGSL_QUERY_VALIDATE_FD:
        req->out_backend = DFM_KGSL_QUERY_BACKEND_FILE;
        ret = 0;
        break;
    case DFM_KGSL_QUERY_SYNCSOURCE_PEEK:
        ret = kgsl_syncsource_peek_file(file, req->min_id, excluded,
            req->excluded_count, &req->out_id, &stage);
        req->out_fail_stage = stage;
        if (ret == -ENOSPC) {
            req->out_errno = ENOSPC;
            req->out_fail_stage = 0;
            ret = 0;
        }
        if (!ret)
            req->out_backend = DFM_KGSL_QUERY_BACKEND_IDR_RCU;
        break;
    case DFM_KGSL_QUERY_GPUOBJ_INFO:
        ret = kgsl_query_gpuobj(file, req);
        break;
    }
out_file:
    fput(file);
out:
    return ret;
}

#endif
