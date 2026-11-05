#ifndef SGAME_SIM_ABI_H
#define SGAME_SIM_ABI_H

#include "upstream_abi.h"
#ifdef __KERNEL__
#include "../sgame_read_mode.h"
#endif

#define SGAME_SIM_VERSION "1.1.0"
#define SGAME_SIM_VERSION_CODE 0x00010100U
#define SGAME_SIM_ABI_VERSION 1U
#define SGAME_SIM_REQUEST_OP 0x53494dU /* SIM; outside the author's operation IDs */
#define SGAME_SIM_SHARED_ADDRESS 0x2025827000ULL
#define SGAME_SIM_COMMAND_CAPS 0U
#define SGAME_SIM_COMMAND_PROCESS 0x10000U
#define SGAME_SIM_COMMAND_FILE_READ8 0x10001U
#define SGAME_SIM_COMMAND_FILE_STAT 0x10002U
#define SGAME_SIM_READ_CACHED 0U
#define SGAME_SIM_READ_NC 1U
#ifndef SGAME_READ_NC
#define SGAME_READ_NC 0
#endif
#define SGAME_SIM_EXTRA_BYTES (DFM_KGSL_QUERY_MAX_EXCLUDED_IDS * 4U)

#define SGAME_SIM_CAP_PROCESS       (1ULL << 0)
#define SGAME_SIM_CAP_MODULE        (1ULL << 1)
#define SGAME_SIM_CAP_TLS           (1ULL << 2)
#define SGAME_SIM_CAP_THREADS       (1ULL << 3)
#define SGAME_SIM_CAP_THREAD_STACK  (1ULL << 4)
#define SGAME_SIM_CAP_PACGA         (1ULL << 5)
#define SGAME_SIM_CAP_MAPPING       (1ULL << 6)
#define SGAME_SIM_CAP_ACCESS        (1ULL << 7)
#define SGAME_SIM_CAP_SIGNAL        (1ULL << 8)
#define SGAME_SIM_CAP_KGSL_VALIDATE (1ULL << 9)
#define SGAME_SIM_CAP_KGSL_PEEK     (1ULL << 10)
#define SGAME_SIM_CAP_KGSL_INFO     (1ULL << 11)
#define SGAME_SIM_CAP_PROCESS_STATE (1ULL << 12)
#define SGAME_SIM_CAP_FILE_READ8    (1ULL << 13)
#define SGAME_SIM_CAP_FILE_STAT     (1ULL << 14)
#define SGAME_SIM_CAP_ALL           ((1ULL << 15) - 1ULL)

/* Capabilities describe implemented commands. Silicon, kernel configuration
 * and vendor KGSL layout availability are reported by the actual query. */
struct sgame_sim_caps {
    uint32_t abi_version;
    uint32_t version_code;
    uint64_t capabilities;
    uint32_t packet_bytes;
    uint32_t max_excluded_ids;
    uint32_t extra_bytes;
    uint32_t read_mode;
};

struct sgame_sim_process_req {
    int32_t pid;
    uint32_t reserved;
    uint64_t out_start_time; /* task start_boottime, nanoseconds */
};

struct sgame_sim_file_read8_req {
    int32_t pid;
    int32_t fd;
    uint64_t start_time;
    uint64_t out_value;
};

/* AArch64 Linux stat ABI, independent of the userspace host's struct stat. */
struct sgame_sim_stat {
    uint64_t dev, ino;
    uint32_t mode, nlink, uid, gid;
    uint64_t rdev, pad0;
    int64_t size;
    int32_t blksize, pad1;
    int64_t blocks, atime_sec;
    uint64_t atime_ns;
    int64_t mtime_sec;
    uint64_t mtime_ns;
    int64_t ctime_sec;
    uint64_t ctime_ns, unused;
};

struct sgame_sim_file_stat_req {
    int32_t pid;
    int32_t fd;
    uint64_t start_time;
    struct sgame_sim_stat out_stat;
};

struct sgame_sim_header {
    uint32_t abi_version;
    uint32_t command;
    uint32_t payload_bytes;
    uint32_t reserved;
};

/* Keep the selected upstream structures and ioctl command encodings intact.
 * They are command tags on our shared transport, never ioctls on a local fd.
 * KGSL excluded_ids must be zero: excluded_count IDs are sent at the start
 * of legacy vmem_info, rather than through an untrusted user pointer. */
#define SGAME_SIM_MEMORY_COMMANDS(X) \
    X(MEM_IOC_GET_PID, get_pid, mem_get_pid_req) \
    X(MEM_IOC_GET_PIDS, get_pids, mem_get_pids_req) \
    X(MEM_IOC_GET_MODULE_BASE, get_module_base, mem_get_module_base_req) \
    X(MEM_IOC_GET_MODULE_BASE2, get_module_base2, mem_get_module_base2_req) \
    X(MEM_IOC_GET_MODULE_PATH, get_module_path, mem_get_module_path_req) \
    X(MEM_IOC_GET_TLS, get_tls, mem_get_tls_req) \
    X(MEM_IOC_GET_THREAD_TID, get_thread_tid, mem_get_thread_tid_req) \
    X(MEM_IOC_GET_THREAD_TIDS, get_thread_tids, mem_get_thread_tids_req) \
    X(MEM_IOC_GET_THREAD_STACK, get_thread_stack, mem_get_thread_stack_req) \
    X(MEM_IOC_EXECUTE_PACGA, execute_pacga, mem_execute_pacga_req) \
    X(MEM_IOC_QUERY_MAPPING, query_mapping, mem_query_mapping_req) \
    X(MEM_IOC_QUERY_ACCESS, query_access, mem_query_access_req) \
    X(MEM_IOC_QUERY_SIGNAL, query_signal, mem_query_signal_req) \
    X(SGAME_SIM_COMMAND_PROCESS, process, sgame_sim_process_req) \
    X(SGAME_SIM_COMMAND_FILE_READ8, file_read8, sgame_sim_file_read8_req) \
    X(SGAME_SIM_COMMAND_FILE_STAT, file_stat, sgame_sim_file_stat_req)

union sgame_sim_payload {
    struct sgame_sim_caps caps;
#define SGAME_SIM_MEMBER(command, name, type) struct type name;
    SGAME_SIM_MEMORY_COMMANDS(SGAME_SIM_MEMBER)
#undef SGAME_SIM_MEMBER
    struct dfm_kgsl_query_req kgsl;
};

struct sgame_sim_packet {
    struct sgame_sim_header header;
    union sgame_sim_payload payload;
};

static inline uint32_t sgame_sim_payload_bytes(uint32_t command)
{
    switch (command) {
    case SGAME_SIM_COMMAND_CAPS: return sizeof(struct sgame_sim_caps);
#define SGAME_SIM_SIZE(command, name, type) case command: return sizeof(struct type);
    SGAME_SIM_MEMORY_COMMANDS(SGAME_SIM_SIZE)
#undef SGAME_SIM_SIZE
    case DFM_IOCTL_KGSL_QUERY: return sizeof(struct dfm_kgsl_query_req);
    default: return 0;
    }
}

#endif
