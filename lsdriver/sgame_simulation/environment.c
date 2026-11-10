/* Read-only environment inputs consumed by the application's ARM64 VM. */
#include <linux/fdtable.h>
#include <linux/file.h>
#include <linux/fs.h>
#include <linux/kdev_t.h>
#include <linux/pid.h>
#include <linux/sched/signal.h>
#include <linux/stat.h>
#include <linux/user_namespace.h>
#include "internal.h"

#ifndef SGAME_SIM_IDS_510
#ifdef __KERNEL__
#include <linux/version.h>
#if LINUX_VERSION_CODE >= KERNEL_VERSION(5, 10, 0) && LINUX_VERSION_CODE < KERNEL_VERSION(5, 11, 0) && !defined(CONFIG_USER_NS)
#define SGAME_SIM_IDS_510 1
#endif
#endif
#ifndef SGAME_SIM_IDS_510
#define SGAME_SIM_IDS_510 0
#endif
#endif

#if SGAME_SIM_IDS_510
static int environment_overflow_id(const char *symbol, u32 *value)
{
    const int *overflow;

    if (*value != (u32)-1)
        return 0;
    overflow = (const int *)sgame_sim_lookup_name(symbol);
    if (!overflow)
        return -EOPNOTSUPP;
    *value = READ_ONCE(*overflow);
    return 0;
}
#endif

static struct task_struct *environment_task(int pid)
{
    struct task_struct *task;

    rcu_read_lock();
    task = pid_task(find_vpid(pid), PIDTYPE_PID);
    if (task)
        get_task_struct(task);
    rcu_read_unlock();
    return task;
}

long sgame_sim_mem_process(struct sgame_sim_process_req *req)
{
    struct task_struct *task = environment_task(req->pid);

    if (!task)
        return -ESRCH;
    req->out_start_time = task->start_boottime;
    put_task_struct(task);
    return 0;
}

static struct file *environment_file(int pid, int fd, u64 start_time)
{
    struct task_struct *task = environment_task(pid);
    struct file *file = NULL;
    struct files_struct *files;
    struct fdtable *table;
    int ret;

    if (!task)
        return ERR_PTR(-ESRCH);
    ret = -ESRCH;
    if (task->start_boottime != start_time)
        goto out_task;
    ret = mutex_lock_killable(&task->signal->cred_guard_mutex);
    if (ret)
        goto out_task;
    ret = -EBADF;
    task_lock(task);
    files = task->files;
    if (files) {
        spin_lock(&files->file_lock);
        table = files_fdtable(files);
        if (fd >= 0 && fd < table->max_fds) {
            file = rcu_dereference_raw(table->fd[fd]);
            if (file)
                get_file(file);
        }
        spin_unlock(&files->file_lock);
    }
    task_unlock(task);
    mutex_unlock(&task->signal->cred_guard_mutex);
out_task:
    put_task_struct(task);
    return file ? file : ERR_PTR(ret);
}

long sgame_sim_mem_file_read8(struct sgame_sim_file_read8_req *req)
{
    struct file *file = environment_file(req->pid, req->fd, req->start_time);
    loff_t position = 0;
    ssize_t bytes;
    ssize_t (*read_file)(struct file *, void *, size_t, loff_t *);

    if (IS_ERR(file))
        return PTR_ERR(file);
    /* The reviewed VM call consumes an eight-byte memfd. Holding this file
     * reference is enough; never duplicate it into the caller or move f_pos. */
    if (!S_ISREG(file_inode(file)->i_mode) ||
        i_size_read(file_inode(file)) != sizeof(req->out_value) ||
        strncmp(file->f_path.dentry->d_name.name, "memfd:", 6)) {
        fput(file);
        return -EPROTO;
    }
    read_file = (void *)sgame_sim_lookup_name("kernel_read");
    if (!read_file) {
        fput(file);
        return -EOPNOTSUPP;
    }
    bytes = read_file(file, &req->out_value, sizeof(req->out_value), &position);
    fput(file);
    return bytes == sizeof(req->out_value) ? 0 : bytes < 0 ? bytes : -EIO;
}

long sgame_sim_mem_file_stat(struct sgame_sim_file_stat_req *req)
{
    struct file *file = environment_file(req->pid, req->fd, req->start_time);
    struct kstat stat;
    u32 uid, gid;
    int ret;
    int (*getattr)(const struct path *, struct kstat *, u32, unsigned int);

    if (IS_ERR(file))
        return PTR_ERR(file);
    getattr = (void *)sgame_sim_lookup_name("vfs_getattr");
    if (!getattr) {
        fput(file);
        return -EOPNOTSUPP;
    }
    ret = getattr(&file->f_path, &stat, STATX_BASIC_STATS, AT_STATX_SYNC_AS_STAT);
    fput(file);
    if (ret)
        return ret;
#if SGAME_SIM_IDS_510
    /* No namespace translation is needed on this 5.10 configuration.
     * The overflow sysctls are not in every vendor KMI. Resolve them only
     * when an invalid ID actually needs the configured overflow value. */
    uid = __kuid_val(stat.uid);
    gid = __kgid_val(stat.gid);
    ret = environment_overflow_id("overflowuid", &uid);
    if (!ret)
        ret = environment_overflow_id("overflowgid", &gid);
    if (ret)
        return ret;
#else
    uid = from_kuid_munged(current_user_ns(), stat.uid);
    gid = from_kgid_munged(current_user_ns(), stat.gid);
#endif
    req->out_stat = (struct sgame_sim_stat) {
        .dev = huge_encode_dev(stat.dev), .ino = stat.ino,
        .mode = stat.mode, .nlink = stat.nlink,
        .uid = uid, .gid = gid,
        .rdev = huge_encode_dev(stat.rdev), .size = stat.size,
        .blksize = stat.blksize, .blocks = stat.blocks,
        .atime_sec = stat.atime.tv_sec, .atime_ns = stat.atime.tv_nsec,
        .mtime_sec = stat.mtime.tv_sec, .mtime_ns = stat.mtime.tv_nsec,
        .ctime_sec = stat.ctime.tv_sec, .ctime_ns = stat.ctime.tv_nsec,
    };
    return 0;
}
