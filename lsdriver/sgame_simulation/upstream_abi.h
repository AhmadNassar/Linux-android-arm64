/* Adapted from enenH/enkernel @ 1c3eb28b4cae3302714d96716aab55bcaf331fc6.
 * Only read-only emulation environment queries are included. See source.json.
 */
#ifndef SGAME_SIM_UPSTREAM_ABI_H
#define SGAME_SIM_UPSTREAM_ABI_H

#ifdef __KERNEL__
#include <linux/types.h>
#include <linux/ioctl.h>
#else
#include <stdint.h>
#include <sys/ioctl.h>
#endif
#define MEM_IOC_GET_PID          _IOWR('D', 1, struct mem_get_pid_req)
#define MEM_IOC_GET_PIDS         _IOWR('D', 19, struct mem_get_pids_req)
#define MEM_IOC_GET_MODULE_BASE  _IOWR('D', 2, struct mem_get_module_base_req)
#define MEM_IOC_GET_MODULE_BASE2 _IOWR('D', 24, struct mem_get_module_base2_req)
#define MEM_IOC_GET_MODULE_PATH  _IOWR('D', 13, struct mem_get_module_path_req)
#define MEM_IOC_GET_TLS          _IOWR('D', 8, struct mem_get_tls_req)
#define MEM_IOC_GET_THREAD_TID   _IOWR('D', 9, struct mem_get_thread_tid_req)
#define MEM_IOC_GET_THREAD_TIDS  _IOWR('D', 25, struct mem_get_thread_tids_req)
#define MEM_IOC_GET_THREAD_STACK _IOWR('D', 18, struct mem_get_thread_stack_req)
#define MEM_IOC_EXECUTE_PACGA    _IOWR('D', 12, struct mem_execute_pacga_req)
#define MEM_IOC_QUERY_MAPPING    _IOWR('D', 29, struct mem_query_mapping_req)
#define MEM_IOC_QUERY_ACCESS     _IOWR('D', 30, struct mem_query_access_req)
#define MEM_IOC_QUERY_SIGNAL     _IOWR('D', 31, struct mem_query_signal_req)
#define DFM_IOCTL_KGSL_QUERY _IOWR('D', 35, struct dfm_kgsl_query_req)

#define MEM_MAPPING_F_READ       (1U << 0)
#define MEM_MAPPING_F_WRITE      (1U << 1)
#define MEM_MAPPING_F_EXEC       (1U << 2)
#define MEM_MAPPING_F_SHARED     (1U << 3)
#define MEM_ACCESS_UNMAPPED   0U
#define MEM_ACCESS_RESIDENT   1U
#define MEM_ACCESS_NONRESIDENT 2U
#define MEM_ACCESS_UNSUPPORTED 3U
#define MEM_GET_PIDS_MAX     64U
#define MEM_GET_THREAD_TIDS_MAX 64U
#define MEM_THREAD_TID_F_ON_CPU  (1U << 0)
#define MEM_THREAD_TID_F_RUNNING (1U << 1)
#define DFM_KGSL_QUERY_ABI_VERSION 1U
#define DFM_KGSL_QUERY_VALIDATE_FD 1U
#define DFM_KGSL_QUERY_SYNCSOURCE_PEEK 2U
#define DFM_KGSL_QUERY_GPUOBJ_INFO 3U
#define DFM_KGSL_QUERY_MAX_EXCLUDED_IDS 4096U
#define DFM_KGSL_QUERY_BACKEND_FILE 1U
#define DFM_KGSL_QUERY_BACKEND_IDR_RCU 2U
#define DFM_KGSL_QUERY_BACKEND_NATIVE_INFO 3U
#define DFM_KGSL_QUERY_FAIL_TASK 101U
#define DFM_KGSL_QUERY_FAIL_ACCESS 102U
#define DFM_KGSL_QUERY_FAIL_FD 103U
#define DFM_KGSL_QUERY_FAIL_INFO_SYMBOL 104U
#define DFM_KGSL_QUERY_FAIL_INFO_RESULT 105U

#define DFM_KGSL_SYNCSOURCE_PEEK_FAIL_FILE_IDENTITY   1U
#define DFM_KGSL_SYNCSOURCE_PEEK_FAIL_NOT_KGSL_FILE   2U
#define DFM_KGSL_SYNCSOURCE_PEEK_FAIL_RUNTIME_SYMBOLS 3U
#define DFM_KGSL_SYNCSOURCE_PEEK_FAIL_CREATE_LAYOUT   4U
#define DFM_KGSL_SYNCSOURCE_PEEK_FAIL_DESTROY_LAYOUT  5U
#define DFM_KGSL_SYNCSOURCE_PEEK_FAIL_LAYOUT_MISMATCH 6U
#define DFM_KGSL_SYNCSOURCE_PEEK_FAIL_LAYOUT_OFFSETS  7U
#define DFM_KGSL_SYNCSOURCE_PEEK_FAIL_PROCESS_PRIV    8U
#define DFM_KGSL_SYNCSOURCE_PEEK_FAIL_LIVE_IDR        9U
#define DFM_KGSL_SYNCSOURCE_PEEK_FAIL_LOWEST_FREE     10U

struct mem_get_pid_req {
    char name[256];
    int  out_pid;
};

struct mem_get_pids_req {
    char     name[256];
    uint32_t out_count;
    uint32_t out_total;
    int      out_pids[MEM_GET_PIDS_MAX];
};

struct mem_get_module_base_req {
    int      pid;
    char     name[256];
    uint64_t out_base;
    uint64_t out_size;
};

struct mem_get_module_base2_req {
    int      pid;
    uint32_t reserved;
    uint64_t max_vma_size;
    char     name[256];
    uint64_t out_base;
    uint64_t out_size;
};

struct mem_get_module_path_req {
    int  pid;
    char name[256];      /* module filename substring to match, e.g. "libUE4.so" */
    char out_path[256];  /* filled with the resolved d_path on success */
};

struct mem_get_tls_req {
    int      pid;
    uint64_t out_tls;
};

struct mem_get_thread_tid_req {
    int  pid;
    char comm[256];
    int  out_tid;
};

struct mem_thread_tid_info {
    int32_t  tid;
    uint32_t flags;
    uint64_t exec_ns;
};

struct mem_get_thread_tids_req {
    int      pid;
    uint32_t out_count;
    uint32_t out_total;
    uint32_t reserved;
    char     comm[256];
    struct mem_thread_tid_info out_threads[MEM_GET_THREAD_TIDS_MAX];
};

struct mem_get_thread_stack_req {
    int      tid;
    int      out_named;
    uint64_t out_low;
    uint64_t out_high;
    uint64_t out_tls;
};

struct mem_execute_pacga_req {
    int      pid;
    int      out_supported;
    uint64_t data;
    uint64_t modifier;
    uint64_t out_result;
};

struct mem_query_mapping_req {
    int32_t  pid;
    uint32_t flags;
    uint64_t addr;
    uint64_t out_phys_addr;
    uint64_t out_page_size;
    uint32_t out_flags;
    uint32_t reserved;
};

struct mem_query_access_req {
    int32_t pid;
    uint32_t flags; /* input: zero */
    uint64_t addr;
    uint64_t out_start;
    uint64_t out_end;
    uint32_t out_permissions; /* MEM_MAPPING_F_{READ,WRITE,EXEC,SHARED} */
    uint32_t out_state;       /* MEM_ACCESS_* */
};

struct mem_query_signal_req {
    int32_t tid;
    uint32_t signo;
    uint64_t out_handler;
    uint64_t out_flags;
    uint64_t out_restorer;
    uint64_t out_action_mask;
    uint64_t out_blocked;
    uint64_t out_alt_sp;
    uint64_t out_alt_size;
    uint32_t out_alt_flags;
    int32_t out_tgid;
};

struct dfm_kgsl_gpuobj_info {
    uint64_t gpuaddr;
    uint64_t flags;
    uint64_t size;
    uint64_t va_len;
    uint64_t va_addr;
    uint32_t id;
    uint32_t padding;
};

struct dfm_kgsl_query_req {
    uint32_t abi_version;
    uint32_t op;
    int32_t  pid;
    uint32_t target_fd;
    uint32_t min_id;
    uint32_t object_id;
    uint32_t excluded_count;
    uint32_t flags;
    uint64_t excluded_ids; /* caller user pointer; never a kernel pointer */
    uint32_t out_id;
    uint32_t out_backend;
    int32_t  out_errno;
    uint32_t out_fail_stage;
    struct dfm_kgsl_gpuobj_info out_info;
    uint64_t reserved[2];
};

#endif
