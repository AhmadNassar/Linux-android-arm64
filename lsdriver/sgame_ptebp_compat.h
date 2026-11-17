#ifndef SGAME_PTEBP_COMPAT_H
#define SGAME_PTEBP_COMPAT_H

#include <linux/pkeys.h>

#include "arm64_reg.h"

/* Dependencies still used by the unchanged upstream UDF header.
 * Restored verbatim from upstream 1f17cdc62957f089982d8faa31eb0654b01d58cb:
 * arm64_emulate/emulate_inst.h and export_fun.h.
 */
enum emu_inst_result
{
    EMU_INST_HANDLED, // 模拟函数已完成指令语义，并自行更新现场和 PC
    EMU_INST_SKIP,    // 不支持或无法执行，不能推进 PC
};

// 内部原语：把一段连续虚拟地址对应的数据缓存行清理到统一点。
static inline void __arm64_clean_dcache_range_to_pou(const void *address, size_t size)
{
    unsigned long line_size = arm64_dcache_line_size();
    unsigned long start = (unsigned long)address;
    unsigned long end = start + size;

    if (!size) return;

    for (unsigned long line = start & ~(line_size - 1); line < end; line += line_size) asm volatile("dc cvau, %0" : : "r"(line) : "memory");
}

// 内部原语：等待数据缓存清理完成，失效内部共享域全部 CPU 的指令缓存，并同步当前 CPU 的取指流水线。
static inline void __arm64_invalidate_icache_all_cpus(void)
{
    asm volatile("dsb ish\n\t"
                 "ic ialluis\n\t"
                 "dsb ish\n\t"
                 "isb\n\t"
                 :
                 :
                 : "memory");
}

//同步一段连续地址中的新机器码：内部先用 DC CVAU 把数据缓存清理到 PoU，再失效内部共享域全部 CPU 的指令缓存
static inline int arm64_sync_code_range_all_cpus(const void *address, size_t size)
{
    unsigned long start = (unsigned long)address;

    if (!address || !size) return -EINVAL;
    if (size > ULONG_MAX - start) return -EOVERFLOW;

    __arm64_clean_dcache_range_to_pou(address, size);
    __arm64_invalidate_icache_all_cpus();
    return 0;
}

#endif /* SGAME_PTEBP_COMPAT_H */
