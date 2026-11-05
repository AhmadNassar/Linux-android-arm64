/* Adapted from enenH/enkernel @ 1c3eb28b4cae3302714d96716aab55bcaf331fc6.
 * Only read-only emulation environment queries are included. See source.json.
 */
#define pr_fmt(fmt) KBUILD_MODNAME ": " fmt

#include <linux/dcache.h>
#include <linux/err.h>
#include <linux/fdtable.h>
#include <linux/file.h>
#include <linux/fs.h>
#include <linux/idr.h>
#include <linux/kallsyms.h>
#include <linux/kernel.h>
#include <linux/limits.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/pid.h>
#include <linux/rcupdate.h>
#include <linux/sched.h>
#include <linux/sched/signal.h>
#include <linux/sched/task.h>
#include <linux/slab.h>
#include <linux/spinlock.h>
#include <linux/string.h>
#include <linux/uaccess.h>
#include <linux/xarray.h>

#include "internal.h"

/*
 * Read-only KGSL syncsource ID prediction
 * ----------------------------------------
 *
 * KGSL keeps syncsource IDs in a struct idr embedded in its private
 * kgsl_process_private type.  That type is vendor-private and its offset moves
 * between KGSL releases, while this GKI module is intentionally built without
 * vendor graphics headers.  Hard-coding an offset selected by
 * LINUX_VERSION_CODE would therefore be unsafe: the same GKI version ships
 * with several different KGSL revisions.
 *
 * Instead, resolve the two stable KGSL handler symbols and accept a layout
 * only when both ARM64 functions independently encode the same three facts:
 *
 *   dev_priv + P -> process_priv
 *   process_priv + I -> syncsource_idr
 *   process_priv + L -> syncsource_lock, where L == I + sizeof(struct idr)
 *
 * The two field addresses must then flow without an X0 clobber into the exact
 * idr_alloc/idr_find and _raw_spin_lock call targets (directly, through a
 * decoded arm64 module PLT, or through a Clang CFI jump table). CREATE must
 * also materialise start=1/end=0.  This is a code-shape/callsite fingerprint,
 * not a blind memory scan.  We additionally verify the live IDR's base and
 * marker before the first lookup.  Any compiler or KGSL layout that does not
 * satisfy all checks is explicitly unsupported.
 *
 * Android 12 Clang CFI emits `BL <name>.cfi_jt` (or a PLT that lands on the
 * JT). Android 14+ kCFI keeps a direct `BL <name>`. Matching only the
 * unwrapped symbol therefore fails closed on 5.10 CFI kernels even when the
 * public KGSL IDR shape is unchanged.
 *
 * Vendor CREATE is also larger than a short prefix. On OPPO CPH2415
 * msm_kgsl.ko the handler is 135 insns (IDA) / 202 insns (kallsyms gap to
 * DESTROY). A 128-insn window is below both, so the decoder must copy the
 * kallsyms span up to KGSL_LAYOUT_SCAN_INSNS rather than fail closed on
 * size, then cut the scan at the first RET so the next named symbol is
 * not fingerprinted. PAC + shadow-call-stack `STR X30, [X18], #8` sits
 * in the prologue before `LDR Xt, [X0, #process_priv]`; only that SCS
 * push and the matching `LDR X30, [X18, #-8]!` pop are skipped the same
 * way SP spills are. Any other X18 load/store stays unknown and fails
 * closed.
 *
 * The actual lookup walks the IDR's XArray XA_FREE_MARK inside
 * rcu_read_lock().  This is the same allocation metadata used by idr_alloc(),
 * so an allocated ID whose associated pointer is NULL is still treated as
 * occupied.  No syncsource object is dereferenced, refcounted, allocated or
 * removed.  Two matching passes are required to reject an obvious concurrent
 * writer.  The result still cannot be a reservation: the target may mutate
 * its IDR after this ioctl returns.
 *
 * Kallsyms discovery and the two handler fingerprints are far more expensive
 * than the IDR lookup itself.  The first successfully validated KGSL fd
 * therefore publishes a runtime/layout cache.  Later peeks still validate
 * their fd against the cached KGSL identity and always reload private_data,
 * process_priv and the live IDR; no per-process pointer or ID result is
 * cached.  KGSL is assumed to remain resident for the lifetime of this cache.
 */

#define KGSL_LAYOUT_SCAN_INSNS        256U
#define KGSL_LAYOUT_MAX_ADDS           48U
#define KGSL_LAYOUT_MAX_CANDIDATES     16U
#define KGSL_LAYOUT_PROC_SCAN_INSNS    24U
#define KGSL_LAYOUT_FIELD_USE_WINDOW    3U
#define KGSL_LAYOUT_CALL_WINDOW         12U
#define KGSL_LAYOUT_PAIR_DISTANCE      24U
#define KGSL_LAYOUT_MAX_FIELD_OFFSET 0x4000U
#define KGSL_LAYOUT_STUB_MAX_BYTES      16U
#define KGSL_LAYOUT_STUB_MAX_HOPS        3U
#define KGSL_SYNCSOURCE_STABLE_TRIES     3U
#define KGSL_SYNCSOURCE_XAS_RETRIES      8U

enum kgsl_peek_fail_stage {
    KGSL_PEEK_FAIL_NONE = 0,
    KGSL_PEEK_FAIL_FILE_IDENTITY = 1,
    KGSL_PEEK_FAIL_NOT_KGSL_FILE = 2,
    KGSL_PEEK_FAIL_RUNTIME_SYMBOLS = 3,
    KGSL_PEEK_FAIL_CREATE_LAYOUT = 4,
    KGSL_PEEK_FAIL_DESTROY_LAYOUT = 5,
    KGSL_PEEK_FAIL_LAYOUT_MISMATCH = 6,
    KGSL_PEEK_FAIL_LAYOUT_OFFSETS = 7,
    KGSL_PEEK_FAIL_PROCESS_PRIV = 8,
    KGSL_PEEK_FAIL_LIVE_IDR = 9,
    KGSL_PEEK_FAIL_LOWEST_FREE = 10,
};

struct kgsl_layout {
    unsigned int process_priv_offset;
    unsigned int syncsource_idr_offset;
    unsigned int syncsource_lock_offset;
};

struct kgsl_layout_candidate {
    struct kgsl_layout layout;
    unsigned int process_reg;
};

struct kgsl_add_imm {
    unsigned int index;
    unsigned int rd;
    unsigned int rn;
    unsigned int imm;
};

struct kgsl_runtime_symbols {
    unsigned long create;
    unsigned long destroy;
    unsigned long fops;
    unsigned long ioctl;
    unsigned long ioctl_cfi;
    unsigned long idr_alloc;
    unsigned long idr_alloc_cfi;
    unsigned long idr_find;
    unsigned long idr_find_cfi;
    unsigned long raw_spin_lock;
    unsigned long raw_spin_lock_cfi;
};

struct kgsl_runtime_cache {
    struct kgsl_runtime_symbols symbols;
    struct kgsl_layout layout;
    unsigned int ready;
};

static DEFINE_MUTEX(g_kgsl_runtime_cache_lock);
static struct kgsl_runtime_cache g_kgsl_runtime_cache;

struct kgsl_call_targets {
    unsigned long direct;
    unsigned long cfi_jt;
};

static s64 kgsl_sign_extend(u64 value, unsigned int sign_bit)
{
    u64 sign = 1ULL << sign_bit;

    return (s64)((value ^ sign) - sign);
}

static bool kgsl_a64_bl(u32 insn, unsigned long pc,
                        unsigned long *target)
{
    s64 offset;

    if ((insn & 0xFC000000U) != 0x94000000U)
        return false;
    offset = kgsl_sign_extend(insn & 0x03FFFFFFU, 25U) * 4;
    *target = pc + offset;
    return true;
}

static bool kgsl_a64_unconditional_b(u32 insn)
{
    return (insn & 0xFC000000U) == 0x14000000U;
}

static unsigned long kgsl_resolve_plt_target(unsigned long plt)
{
    u32 code[4];

    if (copy_from_kernel_nofault(code, (const void *)plt, sizeof(code)))
        return 0;

    /* Current arm64 module PLT: ADRP X16; ADD X16, X16, #lo12; BR X16. */
    if ((code[0] & 0x9F00001FU) == 0x90000010U &&
        (code[1] & 0xFFC003FFU) == 0x91000210U &&
        code[2] == 0xD61F0200U) {
        u64 immlo = (code[0] >> 29) & 0x3U;
        u64 immhi = (code[0] >> 5) & 0x7FFFFU;
        s64 page_offset =
            kgsl_sign_extend((immhi << 2) | immlo, 20U) * 4096;
        unsigned long page = (unsigned long)
            ((s64)(plt & ~0xFFFUL) + page_offset);
        unsigned long lo12 = (code[1] >> 10) & 0xFFFU;

        return page + lo12;
    }

    /* Older arm64 module PLT: MOVN/MOVK/MOVK X16; BR X16. */
    if ((code[0] & 0xFFE0001FU) == 0x92800010U &&
        (code[1] & 0xFF80001FU) == 0xF2800010U &&
        (code[2] & 0xFF80001FU) == 0xF2800010U &&
        ((code[1] >> 21) & 0x3U) == 1U &&
        ((code[2] >> 21) & 0x3U) == 2U &&
        code[3] == 0xD61F0200U) {
        u64 value = ~((u64)(code[0] >> 5) & 0xFFFFULL);
        u64 imm16 = ((u64)(code[1] >> 5) & 0xFFFFULL) << 16;
        u64 imm32 = ((u64)(code[2] >> 5) & 0xFFFFULL) << 32;

        value = (value & ~0x00000000FFFF0000ULL) | imm16;
        value = (value & ~0x0000FFFF00000000ULL) | imm32;
        return (unsigned long)value;
    }

    return 0;
}

static bool kgsl_a64_hint(u32 insn)
{
    return (insn & 0xFFFFF01FU) == 0xD503201FU;
}

static bool kgsl_a64_b_imm(u32 insn, unsigned long pc,
                           unsigned long *target)
{
    s64 offset;

    if ((insn & 0xFC000000U) != 0x14000000U)
        return false;
    offset = kgsl_sign_extend(insn & 0x03FFFFFFU, 25U) * 4;
    *target = pc + (unsigned long)offset;
    return true;
}

static bool kgsl_symbol_is_short_stub(unsigned long addr)
{
    unsigned long size = 0;
    unsigned long offset = 0;

    if (!addr)
        return false;
    if (!sgame_sim_symbol_span(addr, &size, &offset) ||
        offset != 0UL)
        return false;
    return size > 0UL && size <= KGSL_LAYOUT_STUB_MAX_BYTES;
}

/* Clang CFI jump tables are a tiny trampoline: optional BTI/PAC HINT, then
 * an unconditional B to the real function. Never follow a B out of a
 * full-sized function body. */
static unsigned long kgsl_resolve_cfi_jt(unsigned long jt)
{
    u32 code[4];
    unsigned int i;
    unsigned int limit;

    if (!jt || !kgsl_symbol_is_short_stub(jt))
        return 0;
    if (copy_from_kernel_nofault(code, (const void *)jt, sizeof(code)))
        return 0;

    limit = 4U;
    {
        unsigned long size = 0;
        unsigned long offset = 0;

        if (sgame_sim_symbol_span(jt, &size, &offset) &&
            offset == 0UL && size >= sizeof(u32) &&
            size <= sizeof(code) && (size % sizeof(u32)) == 0UL)
            limit = (unsigned int)(size / sizeof(u32));
    }

    for (i = 0; i < limit; i++) {
        unsigned long pc = jt + (unsigned long)i * sizeof(u32);
        unsigned long target;

        if (kgsl_a64_hint(code[i]))
            continue;
        if (kgsl_a64_b_imm(code[i], pc, &target))
            return target;
        return 0;
    }
    return 0;
}

static unsigned long kgsl_resolve_call_target(unsigned long target)
{
    unsigned int hop;

    for (hop = 0; hop < KGSL_LAYOUT_STUB_MAX_HOPS; hop++) {
        unsigned long next = kgsl_resolve_plt_target(target);

        if (!next)
            next = kgsl_resolve_cfi_jt(target);
        if (!next || next == target)
            break;
        target = next;
    }
    return target;
}

static bool kgsl_addr_in_targets(unsigned long addr,
                                 const struct kgsl_call_targets *expected)
{
    if (!addr || !expected)
        return false;
    return (expected->direct && addr == expected->direct) ||
           (expected->cfi_jt && addr == expected->cfi_jt);
}

static bool kgsl_call_matches(u32 insn, unsigned long pc,
                              const struct kgsl_call_targets *expected)
{
    unsigned long target;
    unsigned long resolved;

    if (!expected || (!expected->direct && !expected->cfi_jt) ||
        !kgsl_a64_bl(insn, pc, &target))
        return false;
    if (kgsl_addr_in_targets(target, expected))
        return true;
    resolved = kgsl_resolve_call_target(target);
    return kgsl_addr_in_targets(resolved, expected);
}

static bool kgsl_a64_add_imm(u32 insn, unsigned int *rd,
                             unsigned int *rn, unsigned int *imm)
{
    unsigned int value;

    if ((insn & 0xFF000000U) != 0x91000000U)
        return false;

    value = (insn >> 10) & 0xFFFU;
    if (insn & (1U << 22))
        value <<= 12;

    *rd = insn & 0x1FU;
    *rn = (insn >> 5) & 0x1FU;
    *imm = value;
    return true;
}

static bool kgsl_a64_sub_imm(u32 insn, unsigned int *rd,
                             unsigned int *rn, unsigned int *imm)
{
    unsigned int value;

    if ((insn & 0xFF000000U) != 0xD1000000U)
        return false;

    value = (insn >> 10) & 0xFFFU;
    if (insn & (1U << 22))
        value <<= 12;

    *rd = insn & 0x1FU;
    *rn = (insn >> 5) & 0x1FU;
    *imm = value;
    return true;
}

static bool kgsl_a64_ldr_x_unsigned(u32 insn, unsigned int *rt,
                                    unsigned int *rn, unsigned int *imm)
{
    if ((insn & 0xFFC00000U) != 0xF9400000U)
        return false;

    *rt = insn & 0x1FU;
    *rn = (insn >> 5) & 0x1FU;
    *imm = ((insn >> 10) & 0xFFFU) * sizeof(u64);
    return true;
}

static bool kgsl_a64_mov_x(u32 insn, unsigned int *rd, unsigned int *rm)
{
    /* MOV Xd, Xm is the ORR Xd, XZR, Xm alias with no shift. */
    if ((insn & 0xFFE0FFE0U) != 0xAA0003E0U)
        return false;

    *rd = insn & 0x1FU;
    *rm = (insn >> 16) & 0x1FU;
    return true;
}

static bool kgsl_a64_mov_w(u32 insn, unsigned int *rd, unsigned int *rm)
{
    /* MOV Wd, Wm is the ORR Wd, WZR, Wm alias with no shift. */
    if ((insn & 0x7FE0FFE0U) != 0x2A0003E0U)
        return false;

    *rd = insn & 0x1FU;
    *rm = (insn >> 16) & 0x1FU;
    return true;
}

static bool kgsl_a64_mov_w_constant(u32 insn, unsigned int *rd,
                                    u32 *value)
{
    /* MOV Wd, #imm using the MOVZ alias. */
    if ((insn & 0xFF800000U) == 0x52800000U) {
        unsigned int shift = ((insn >> 21) & 0x3U) * 16U;

        if (shift >= 32U)
            return false;
        *rd = insn & 0x1FU;
        *value = ((insn >> 5) & 0xFFFFU) << shift;
        return true;
    }

    /* MOV Wd, WZR is ORR Wd, WZR, WZR and materialises zero. */
    if ((insn & 0x7FE0FFE0U) == 0x2A0003E0U &&
        ((insn >> 16) & 0x1FU) == 31U) {
        *rd = insn & 0x1FU;
        *value = 0;
        return true;
    }

    return false;
}

static bool kgsl_a64_ret(u32 insn)
{
    return (insn & 0xFFFFFC1FU) == 0xD65F0000U ||
           insn == 0xD65F0BFFU || /* RETAA */
           insn == 0xD65F0FFFU;   /* RETAB */
}

static bool kgsl_a64_stack_store(u32 insn)
{
    unsigned int rn = (insn >> 5) & 0x1FU;

    if (rn != 31U)
        return false;

    /* Store-register pair, including offset/pre/post-index forms. */
    if ((insn & 0x3A000000U) == 0x28000000U &&
        !(insn & (1U << 22)))
        return true;

    /* STR Wt/Xt, [SP, #imm12]. */
    return (insn & 0xFFC00000U) == 0xB9000000U ||
           (insn & 0xFFC00000U) == 0xF9000000U;
}

/* AAPCS64 shadow-call-stack: STR X30, [X18], #8 and
 * LDR X30, [X18, #-8]!. Any other X18 load/store is still unknown. */
static bool kgsl_a64_scs_x18(u32 insn)
{
    unsigned int rt = insn & 0x1FU;
    unsigned int rn = (insn >> 5) & 0x1FU;
    unsigned int imm9 = (insn >> 12) & 0x1FFU;
    s32 simm;

    if (rt != 30U || rn != 18U)
        return false;

    simm = (s32)kgsl_sign_extend(imm9, 8U);
    /* 64-bit STR post-index #8 */
    if ((insn & 0xFFE00C00U) == 0xF8000400U && simm == 8)
        return true;
    /* 64-bit LDR pre-index #-8 */
    if ((insn & 0xFFE00C00U) == 0xF8400C00U && simm == -8)
        return true;
    return false;
}

/* Decode only instruction classes needed in the short argument-setup
 * windows accepted below.  An unknown instruction is never assumed to
 * preserve a register: callers reject the candidate instead. */
static bool kgsl_a64_known_gpr_write(u32 insn, unsigned int *rd)
{
    unsigned int ignored;
    u32 value;

    if (kgsl_a64_add_imm(insn, rd, &ignored, &ignored) ||
        kgsl_a64_sub_imm(insn, rd, &ignored, &ignored) ||
        kgsl_a64_ldr_x_unsigned(insn, rd, &ignored, &ignored) ||
        kgsl_a64_mov_x(insn, rd, &ignored) ||
        kgsl_a64_mov_w_constant(insn, rd, &value) ||
        kgsl_a64_mov_w(insn, rd, &ignored))
        return true;

    /* LDR Wt, [Xn, #imm12]. */
    if ((insn & 0xFFC00000U) == 0xB9400000U) {
        *rd = insn & 0x1FU;
        return true;
    }

    return false;
}

static bool kgsl_field_add_used_as_x0(const u32 *code, unsigned int count,
                                      const struct kgsl_add_imm *add)
{
    unsigned int i;

    if (add->rd == 0U)
        return true;

    for (i = add->index + 1U;
         i < count && i <= add->index + KGSL_LAYOUT_FIELD_USE_WINDOW;
         i++) {
        unsigned int rd;
        unsigned int rm;
        unsigned int rn;
        unsigned int imm;
        unsigned int written;
        unsigned long target;

        if (kgsl_a64_mov_x(code[i], &rd, &rm) && rd == 0U && rm == add->rd)
            return true;
        if (kgsl_a64_add_imm(code[i], &rd, &rn, &imm) &&
            rd == 0U && rn == add->rd && imm == 0U)
            return true;
        if (kgsl_a64_bl(code[i], (unsigned long)i * sizeof(u32), &target) ||
            kgsl_a64_unconditional_b(code[i]) || kgsl_a64_ret(code[i]))
            return false;
        if (kgsl_a64_hint(code[i]))
            continue;
        if (!kgsl_a64_known_gpr_write(code[i], &written) ||
            written == 0U || written == add->rd)
            return false;
    }

    return false;
}

static bool kgsl_field_add_reaches_call(const u32 *code, unsigned int count,
                                        unsigned long function,
                                        const struct kgsl_add_imm *add,
                                        const struct kgsl_call_targets *expected)
{
    unsigned int active;
    unsigned int i;

    if (add->rd == 0U) {
        active = add->index;
    } else {
        active = count;
        for (i = add->index + 1U;
             i < count && i <= add->index + KGSL_LAYOUT_FIELD_USE_WINDOW;
             i++) {
            unsigned int rd;
            unsigned int rm;
            unsigned int rn;
            unsigned int imm;
            unsigned int written;
            unsigned long target;

            if ((kgsl_a64_mov_x(code[i], &rd, &rm) &&
                 rd == 0U && rm == add->rd) ||
                (kgsl_a64_add_imm(code[i], &rd, &rn, &imm) &&
                 rd == 0U && rn == add->rd && imm == 0U)) {
                active = i;
                break;
            }
            if (kgsl_a64_bl(code[i],
                            function + (unsigned long)i * sizeof(u32),
                            &target) ||
                kgsl_a64_unconditional_b(code[i]) || kgsl_a64_ret(code[i]))
                return false;
            if (kgsl_a64_hint(code[i]))
                continue;
            if (!kgsl_a64_known_gpr_write(code[i], &written) ||
                written == 0U || written == add->rd)
                return false;
        }
        if (active == count)
            return false;
    }

    for (i = active + 1U;
         i < count && i <= active + KGSL_LAYOUT_CALL_WINDOW;
         i++) {
        unsigned long pc = function + (unsigned long)i * sizeof(u32);
        unsigned long target;
        unsigned int written;

        if (kgsl_a64_bl(code[i], pc, &target))
            return kgsl_call_matches(code[i], pc, expected);
        if (kgsl_a64_unconditional_b(code[i]) || kgsl_a64_ret(code[i]))
            return false;
        if (kgsl_a64_hint(code[i]))
            continue;
        if (!kgsl_a64_known_gpr_write(code[i], &written) || written == 0U)
            return false;
    }

    return false;
}

static bool kgsl_idr_alloc_uses_native_bounds(const u32 *code,
                                              unsigned int count,
                                              unsigned long function,
                                              const struct kgsl_add_imm *add,
                                              const struct kgsl_call_targets *expected)
{
    bool start_is_one = false;
    bool end_is_zero = false;
    unsigned int i;

    for (i = add->index + 1U;
         i < count && i <= add->index + KGSL_LAYOUT_CALL_WINDOW;
         i++) {
        unsigned int rd;
        u32 value;
        unsigned long target;
        unsigned long pc = function + (unsigned long)i * sizeof(u32);

        if (kgsl_a64_bl(code[i], pc, &target))
            return kgsl_call_matches(code[i], pc, expected) &&
                   start_is_one && end_is_zero;
        if (kgsl_a64_unconditional_b(code[i]) || kgsl_a64_ret(code[i]))
            return false;
        if (kgsl_a64_hint(code[i]))
            continue;
        if (kgsl_a64_mov_w_constant(code[i], &rd, &value)) {
            if (rd == 2U)
                start_is_one = value == 1U;
            else if (rd == 3U)
                end_is_zero = value == 0U;
            continue;
        }
        if (!kgsl_a64_known_gpr_write(code[i], &rd))
            return false;
        if (rd == 2U || rd == 3U)
            return false;
    }

    return false;
}

static bool kgsl_find_process_offset(const u32 *code, unsigned int count,
                                     unsigned int process_reg,
                                     unsigned int before_index,
                                     unsigned int *process_offset)
{
    unsigned int limit = min(count, KGSL_LAYOUT_PROC_SCAN_INSNS);
    unsigned int i;

    limit = min(limit, before_index);
    if (process_reg < 19U || process_reg > 28U)
        return false;

    for (i = 0; i < limit; i++) {
        unsigned int rt;
        unsigned int rn;
        unsigned int imm;
        unsigned int written;
        unsigned long target;

        if (kgsl_a64_ldr_x_unsigned(code[i], &rt, &rn, &imm) && rn == 0U) {
            if (imm <= 8U * sizeof(void *) &&
                (imm % sizeof(void *)) == 0U && rt == process_reg) {
                *process_offset = imm;
                return true;
            }
            continue;
        }
        if (kgsl_a64_hint(code[i]) || kgsl_a64_stack_store(code[i]) ||
            kgsl_a64_scs_x18(code[i]))
            continue;
        if (kgsl_a64_bl(code[i], (unsigned long)i * sizeof(u32), &target) ||
            kgsl_a64_unconditional_b(code[i]) || kgsl_a64_ret(code[i]))
            return false;
        if (!kgsl_a64_known_gpr_write(code[i], &written) || written == 0U)
            return false;
    }

    return false;
}

static bool kgsl_same_layout(const struct kgsl_layout *a,
                             const struct kgsl_layout *b)
{
    return a->process_priv_offset == b->process_priv_offset &&
           a->syncsource_idr_offset == b->syncsource_idr_offset &&
           a->syncsource_lock_offset == b->syncsource_lock_offset;
}

static int kgsl_add_layout_candidate(struct kgsl_layout_candidate *out,
                                     unsigned int *out_count,
                                     const struct kgsl_layout_candidate *value)
{
    unsigned int i;

    for (i = 0; i < *out_count; i++) {
        if (kgsl_same_layout(&out[i].layout, &value->layout))
            return 0;
    }
    if (*out_count >= KGSL_LAYOUT_MAX_CANDIDATES)
        return -E2BIG;

    out[*out_count] = *value;
    (*out_count)++;
    return 0;
}

static int kgsl_decode_layout_candidates(
    unsigned long function,
    const struct kgsl_call_targets *idr_call,
    const struct kgsl_call_targets *lock_call,
    bool require_native_bounds,
    struct kgsl_layout_candidate *out,
    unsigned int *out_count)
{
    u32 *code = NULL;
    struct kgsl_add_imm adds[KGSL_LAYOUT_MAX_ADDS];
    unsigned long symbol_size = 0;
    unsigned long symbol_offset = 0;
    unsigned long copy_bytes;
    unsigned int code_count;
    unsigned int add_count = 0;
    unsigned int i;
    unsigned int j;
    bool has_ret = false;
    int ret = -EOPNOTSUPP;

    *out_count = 0;
    if (!function)
        return -ENOENT;
    /* kallsyms size is the gap to the next symbol, not IDA's function
     * length. Copy a prefix when the gap exceeds the scan buffer, then
     * keep only bytes through the first RET so padding or the next
     * handler is not treated as part of this function. The insn buffer
     * is heap-allocated: 256 insns exceed the 2KiB frame-size cap. */
    if (!sgame_sim_symbol_span(function, &symbol_size,
                                     &symbol_offset) ||
        symbol_offset != 0UL || symbol_size < sizeof(u32) ||
        (symbol_size % sizeof(u32)) != 0UL)
        return -EOPNOTSUPP;
    copy_bytes = min(symbol_size,
                     (unsigned long)KGSL_LAYOUT_SCAN_INSNS * sizeof(u32));
    code_count = (unsigned int)(copy_bytes / sizeof(u32));
    code = kmalloc(copy_bytes, GFP_KERNEL);
    if (!code)
        return -ENOMEM;
    if (copy_from_kernel_nofault(code, (const void *)function,
                                 copy_bytes)) {
        ret = -EFAULT;
        goto out_code;
    }

    for (i = 0; i < code_count; i++) {
        if (kgsl_a64_ret(code[i])) {
            has_ret = true;
            code_count = i + 1U;
            break;
        }
    }
    if (!has_ret) {
        ret = -EOPNOTSUPP;
        goto out_code;
    }

    for (i = 0; i < code_count; i++) {
        struct kgsl_add_imm add;

        if (!kgsl_a64_add_imm(code[i], &add.rd, &add.rn, &add.imm))
            continue;
        if (add.rn == 31U || add.imm > KGSL_LAYOUT_MAX_FIELD_OFFSET)
            continue;
        add.index = i;
        if (!kgsl_field_add_used_as_x0(code, code_count, &add))
            continue;
        if (add_count >= KGSL_LAYOUT_MAX_ADDS) {
            ret = -E2BIG;
            goto out_code;
        }
        adds[add_count++] = add;
    }

    for (i = 0; i < add_count; i++) {
        for (j = i + 1U; j < add_count; j++) {
            struct kgsl_layout_candidate candidate;
            unsigned int lower;
            unsigned int upper;
            const struct kgsl_add_imm *idr_add;
            const struct kgsl_add_imm *lock_add;

            if (adds[i].rn != adds[j].rn)
                continue;
            if (adds[j].index - adds[i].index > KGSL_LAYOUT_PAIR_DISTANCE)
                continue;

            lower = min(adds[i].imm, adds[j].imm);
            upper = max(adds[i].imm, adds[j].imm);
            if (upper - lower != sizeof(struct idr))
                continue;
            if ((lower % __alignof__(struct idr)) != 0U ||
                (upper % __alignof__(spinlock_t)) != 0U)
                continue;
            idr_add = adds[i].imm == lower ? &adds[i] : &adds[j];
            lock_add = adds[i].imm == upper ? &adds[i] : &adds[j];
            if (!kgsl_field_add_reaches_call(code, code_count, function,
                                             idr_add, idr_call) ||
                !kgsl_field_add_reaches_call(code, code_count, function,
                                             lock_add, lock_call))
                continue;
            if (require_native_bounds &&
                !kgsl_idr_alloc_uses_native_bounds(code, code_count,
                                                   function, idr_add,
                                                   idr_call))
                continue;

            memset(&candidate, 0, sizeof(candidate));
            candidate.process_reg = adds[i].rn;
            if (!kgsl_find_process_offset(code, code_count,
                                          candidate.process_reg,
                                          adds[i].index,
                                          &candidate.layout.process_priv_offset))
                continue;
            candidate.layout.syncsource_idr_offset = lower;
            candidate.layout.syncsource_lock_offset = upper;

            ret = kgsl_add_layout_candidate(out, out_count, &candidate);
            if (ret)
                goto out_code;
        }
    }

    ret = *out_count ? 0 : -EOPNOTSUPP;
out_code:
    kfree(code);
    return ret;
}

static unsigned long kgsl_lookup_cfi_symbol(const char *name)
{
    char cfi_name[KSYM_NAME_LEN];
    ssize_t len;

    if (!name)
        return 0;
    len = strscpy(cfi_name, name, sizeof(cfi_name));
    if (len <= 0 || (size_t)len + sizeof(".cfi_jt") > sizeof(cfi_name))
        return 0;
    strscpy(cfi_name + len, ".cfi_jt", sizeof(cfi_name) - len);
    return sgame_sim_lookup_name(cfi_name);
}

static int kgsl_resolve_file_identity(struct kgsl_runtime_symbols *symbols)
{
    memset(symbols, 0, sizeof(*symbols));
    symbols->fops = sgame_sim_lookup_name("kgsl_fops");
    symbols->ioctl = sgame_sim_lookup_name("kgsl_ioctl");
    symbols->ioctl_cfi = kgsl_lookup_cfi_symbol("kgsl_ioctl");

    return (symbols->fops || symbols->ioctl || symbols->ioctl_cfi) ?
        0 : -EOPNOTSUPP;
}

static int kgsl_resolve_runtime(struct kgsl_runtime_symbols *symbols,
                                struct kgsl_layout *layout,
                                enum kgsl_peek_fail_stage *fail_stage)
{
    struct kgsl_layout_candidate create[KGSL_LAYOUT_MAX_CANDIDATES];
    struct kgsl_layout_candidate destroy[KGSL_LAYOUT_MAX_CANDIDATES];
    struct kgsl_call_targets idr_alloc_t;
    struct kgsl_call_targets idr_find_t;
    struct kgsl_call_targets spin_lock_t;
    unsigned int create_count = 0;
    unsigned int destroy_count = 0;
    unsigned int matches = 0;
    unsigned int i;
    unsigned int j;
    int ret;

    memset(layout, 0, sizeof(*layout));
    /* The cache initializer has already resolved and validated file identity.
     * Avoid repeating those three full symbol-table scans, but keep a
     * defensive check for future callers. */
    if (!symbols->fops && !symbols->ioctl && !symbols->ioctl_cfi) {
        if (fail_stage)
            *fail_stage = KGSL_PEEK_FAIL_FILE_IDENTITY;
        return -EOPNOTSUPP;
    }
    symbols->create = kgsl_resolve_call_target(
        sgame_sim_lookup_name("kgsl_ioctl_syncsource_create"));
    symbols->destroy = kgsl_resolve_call_target(
        sgame_sim_lookup_name("kgsl_ioctl_syncsource_destroy"));
    symbols->idr_alloc = sgame_sim_lookup_name("idr_alloc");
    symbols->idr_alloc_cfi = kgsl_lookup_cfi_symbol("idr_alloc");
    symbols->idr_find = sgame_sim_lookup_name("idr_find");
    symbols->idr_find_cfi = kgsl_lookup_cfi_symbol("idr_find");
    symbols->raw_spin_lock = sgame_sim_lookup_name("_raw_spin_lock");
    symbols->raw_spin_lock_cfi = kgsl_lookup_cfi_symbol("_raw_spin_lock");
    if (symbols->idr_alloc)
        symbols->idr_alloc = kgsl_resolve_call_target(symbols->idr_alloc);
    if (symbols->idr_find)
        symbols->idr_find = kgsl_resolve_call_target(symbols->idr_find);
    if (symbols->raw_spin_lock)
        symbols->raw_spin_lock =
            kgsl_resolve_call_target(symbols->raw_spin_lock);
    if (!symbols->create || !symbols->destroy ||
        !symbols->idr_alloc || !symbols->idr_find ||
        !symbols->raw_spin_lock) {
        if (fail_stage)
            *fail_stage = KGSL_PEEK_FAIL_RUNTIME_SYMBOLS;
        return -EOPNOTSUPP;
    }

    memset(&idr_alloc_t, 0, sizeof(idr_alloc_t));
    memset(&idr_find_t, 0, sizeof(idr_find_t));
    memset(&spin_lock_t, 0, sizeof(spin_lock_t));
    idr_alloc_t.direct = symbols->idr_alloc;
    idr_alloc_t.cfi_jt = symbols->idr_alloc_cfi;
    idr_find_t.direct = symbols->idr_find;
    idr_find_t.cfi_jt = symbols->idr_find_cfi;
    spin_lock_t.direct = symbols->raw_spin_lock;
    spin_lock_t.cfi_jt = symbols->raw_spin_lock_cfi;

    ret = kgsl_decode_layout_candidates(symbols->create,
                                        &idr_alloc_t,
                                        &spin_lock_t,
                                        true,
                                        create,
                                        &create_count);
    if (ret) {
        if (fail_stage)
            *fail_stage = KGSL_PEEK_FAIL_CREATE_LAYOUT;
        return ret;
    }
    ret = kgsl_decode_layout_candidates(symbols->destroy,
                                        &idr_find_t,
                                        &spin_lock_t,
                                        false,
                                        destroy,
                                        &destroy_count);
    if (ret) {
        if (fail_stage)
            *fail_stage = KGSL_PEEK_FAIL_DESTROY_LAYOUT;
        return ret;
    }

    for (i = 0; i < create_count; i++) {
        for (j = 0; j < destroy_count; j++) {
            if (!kgsl_same_layout(&create[i].layout, &destroy[j].layout))
                continue;
            *layout = create[i].layout;
            matches++;
        }
    }

    if (matches != 1U) {
        if (fail_stage)
            *fail_stage = KGSL_PEEK_FAIL_LAYOUT_MISMATCH;
        return -EOPNOTSUPP;
    }
    if (layout->syncsource_idr_offset > KGSL_LAYOUT_MAX_FIELD_OFFSET ||
        layout->syncsource_lock_offset !=
            layout->syncsource_idr_offset + sizeof(struct idr)) {
        if (fail_stage)
            *fail_stage = KGSL_PEEK_FAIL_LAYOUT_OFFSETS;
        return -EOPNOTSUPP;
    }

    return 0;
}

static bool kgsl_file_matches(const struct file *file,
                              const struct kgsl_runtime_symbols *symbols)
{
    const struct file_operations *fops;
    unsigned long unlocked_ioctl;
    unsigned long compat_ioctl;

    if (!file || !file_inode(file) ||
        !S_ISCHR(file_inode(file)->i_mode))
        return false;
    fops = READ_ONCE(file->f_op);
    if (!fops)
        return false;
    if (symbols->fops && (unsigned long)fops == symbols->fops)
        return true;

    unlocked_ioctl = (unsigned long)READ_ONCE(fops->unlocked_ioctl);
#ifdef CONFIG_COMPAT
    compat_ioctl = (unsigned long)READ_ONCE(fops->compat_ioctl);
#else
    compat_ioctl = 0;
#endif
    return (symbols->ioctl &&
            (unlocked_ioctl == symbols->ioctl ||
             compat_ioctl == symbols->ioctl)) ||
           (symbols->ioctl_cfi &&
            (unlocked_ioctl == symbols->ioctl_cfi ||
             compat_ioctl == symbols->ioctl_cfi));
}

static int kgsl_use_cached_runtime(
    const struct file *file,
    struct kgsl_layout *layout,
    enum kgsl_peek_fail_stage *fail_stage)
{
    if (!kgsl_file_matches(file, &g_kgsl_runtime_cache.symbols)) {
        if (fail_stage)
            *fail_stage = KGSL_PEEK_FAIL_NOT_KGSL_FILE;
        return -ENODEV;
    }

    *layout = g_kgsl_runtime_cache.layout;
    return 0;
}

static int kgsl_get_runtime(
    struct file *file,
    struct kgsl_layout *layout,
    enum kgsl_peek_fail_stage *fail_stage)
{
    struct kgsl_runtime_symbols symbols;
    struct kgsl_layout resolved_layout;
    int ret;

    if (smp_load_acquire(&g_kgsl_runtime_cache.ready))
        return kgsl_use_cached_runtime(file, layout, fail_stage);

    mutex_lock(&g_kgsl_runtime_cache_lock);
    if (READ_ONCE(g_kgsl_runtime_cache.ready)) {
        ret = kgsl_use_cached_runtime(file, layout, fail_stage);
        goto out_unlock;
    }

    /* Until identity succeeds, do not inspect vendor text through symbols
     * that a foreign fd cannot keep resident. */
    ret = kgsl_resolve_file_identity(&symbols);
    if (ret) {
        if (fail_stage)
            *fail_stage = KGSL_PEEK_FAIL_FILE_IDENTITY;
        goto out_unlock;
    }
    if (!kgsl_file_matches(file, &symbols)) {
        if (fail_stage)
            *fail_stage = KGSL_PEEK_FAIL_NOT_KGSL_FILE;
        ret = -ENODEV;
        goto out_unlock;
    }

    ret = kgsl_resolve_runtime(&symbols, &resolved_layout, fail_stage);
    if (ret)
        goto out_unlock;

    g_kgsl_runtime_cache.symbols = symbols;
    g_kgsl_runtime_cache.layout = resolved_layout;
    smp_store_release(&g_kgsl_runtime_cache.ready, 1U);
    *layout = resolved_layout;
    ret = 0;
    goto out_unlock;

out_unlock:
    mutex_unlock(&g_kgsl_runtime_cache_lock);
    return ret;
}

#include "kgsl_idr.h"

static int kgsl_syncsource_peek_file(struct file *file, u32 min_id,
                                    const u32 *excluded, u32 excluded_count,
                                    u32 *out_id,
                                    enum kgsl_peek_fail_stage *fail_stage)
{
    struct kgsl_layout layout;
    void *dev_priv;
    void *process_priv;
    struct idr *idr;
    u32 index = 0;
    u32 attempt;
    int ret;

    ret = kgsl_get_runtime(file, &layout, fail_stage);
    if (ret)
        return ret;

    dev_priv = READ_ONCE(file->private_data);
    if (!dev_priv) {
        *fail_stage = KGSL_PEEK_FAIL_NOT_KGSL_FILE;
        return -ENODEV;
    }
    if (copy_from_kernel_nofault(
            &process_priv,
            (const char *)dev_priv + layout.process_priv_offset,
            sizeof(process_priv)) || !process_priv) {
        *fail_stage = KGSL_PEEK_FAIL_PROCESS_PRIV;
        return -EOPNOTSUPP;
    }

    idr = (struct idr *)((char *)process_priv +
                         layout.syncsource_idr_offset);
    ret = kgsl_validate_live_idr(idr);
    if (ret) {
        if (ret == -EOPNOTSUPP)
            *fail_stage = KGSL_PEEK_FAIL_LIVE_IDR;
        return ret;
    }

    /* Pin one file for the whole exclusion search. Re-resolving PID/FD for
     * each candidate could mix two IDRs after a concurrent close/reuse. */
    for (attempt = 0; attempt <= excluded_count; attempt++) {
        u32 candidate;

        ret = kgsl_find_stable_lowest_free(idr, min_id, &candidate);
        if (ret) {
            *fail_stage = KGSL_PEEK_FAIL_LOWEST_FREE;
            return ret;
        }
        while (index < excluded_count && excluded[index] < candidate)
            index++;
        if (index == excluded_count || excluded[index] != candidate) {
            *out_id = candidate;
            return 0;
        }
        if (candidate == INT_MAX)
            return -ENOSPC;
        min_id = candidate + 1U;
        index++;
    }
    return -EAGAIN;
}

#include "kgsl_query.h"
