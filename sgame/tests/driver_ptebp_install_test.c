/* Execute the production installer and rollback with interleaved target reads. */
#include <assert.h>
#include <errno.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

enum { BP_CONFIG_MAX = 16, PAGE_SIZE = 4096, GFP_ATOMIC = 0, __GFP_ZERO = 0 };
#define PAGE_MASK (~(uint64_t)(PAGE_SIZE - 1))
#define ARRAY_SIZE(a) (sizeof(a) / sizeof((a)[0]))
#define PTE_UXN (UINT64_C(1) << 54)
#define PTE_USER (UINT64_C(1) << 6)
#define PTEBP_UDF_INST 0U
#define BP_BREAKPOINT_X 4
#define __pte(x) (x)
#define pte_val(x) (x)
#define READ_ONCE(x) (x)
#define WRITE_ONCE(x, v) ((x) = (v))
#define smp_load_acquire(p) (*(p))
#define smp_store_release(p, v) (*(p) = (v))
#define untagged_addr(x) (x)
#define mutex_lock(x) ((void)(x))
#define mutex_unlock(x) ((void)(x))
#define spin_lock_irqsave(x, f) ((void)(x), (f) = 0)
#define spin_unlock_irqrestore(x, f) ((void)(x), (void)(f))
#define mmap_read_lock(x) ((void)(x), ++mmap_locks)
#define mmap_read_unlock(x) ((void)(x), --mmap_locks)
#define mmap_read_trylock(x) ((void)(x), trylock_succeeds ? (++mmap_locks, 1) : 0)
#define ls_log_always_tag(...) ((void)0)
typedef uint64_t pte_t;
typedef uint64_t pteval_t;
typedef uint64_t phys_addr_t;
struct mm_struct { uint64_t task_size; };
struct bp_point { uint64_t hit_addr; int bt; };
struct break_point { int tgid; struct bp_point points[BP_CONFIG_MAX]; };
struct ptebp_slot {
    pte_t orig_pte;
    uint64_t hook_addr, page_vaddr;
    void *shadow_page;
};
static struct break_point *g_ptebp_info;
static struct mm_struct *g_ptebp_mm;
static struct ptebp_slot g_ptebp_slots[BP_CONFIG_MAX];
static int g_ptebp_lock, g_ptebp_control_lock, g_ptebp_hooks[4][1];
static bool g_ptebp_stopping;
static unsigned long g_ptebp_generation;
static struct mm_struct target_mm = {0x40000};
static uint32_t memory[3][PAGE_SIZE / 4], original[3][PAGE_SIZE / 4];
static pteval_t page_ptes[3];
static int references, allocations, guard_writes, marker_writes;
static int fail_guard, fail_marker;
static unsigned observations;
static int mmap_locks;
static bool trylock_succeeds = true;

static size_t page_index(uint64_t address) {
    assert(address >= 0x10000 && address < 0x40000);
    return (address - 0x10000) / PAGE_SIZE;
}
static bool bp_point_is_configured_type(struct bp_point *p, int type) {
    return p->hit_addr && p->bt == type;
}
static void *bp_info_find_configured_type(struct break_point *p, int type, void *unused) {
    (void)unused;
    for (size_t i = 0; i < BP_CONFIG_MAX; ++i)
        if (bp_point_is_configured_type(&p->points[i], type)) return &p->points[i];
    return NULL;
}
static int inline_hook_install(void *unused) { (void)unused; return 0; }
static void inline_hook_remove(void *unused) { (void)unused; }
static struct mm_struct *get_mm_by_pid(int pid) {
    assert(pid == 123); ++references; return &target_mm;
}
static void mmput(struct mm_struct *mm) { assert(mm == &target_mm); --references; }
static unsigned long __get_free_page(int flags) {
    (void)flags; ++allocations; return (unsigned long)calloc(1, PAGE_SIZE);
}
static void free_page(unsigned long page) { --allocations; free((void *)page); }
static pteval_t ptebp_make_data_guard_pte(pteval_t value) { return value & ~PTE_USER; }
static int read_user_pte_value(struct mm_struct *mm, uint64_t address, pteval_t *value) {
    assert(mm == &target_mm); *value = page_ptes[page_index(address)]; return 0;
}
static int walk_translate_va_to_pa(struct mm_struct *mm, uint64_t address, phys_addr_t *pa) {
    assert(mm == &target_mm); *pa = address; return 0;
}
static int linear_read_physical(phys_addr_t pa, void *out, size_t size) {
    memcpy(out, (char *)memory[page_index(pa)] + (pa & (PAGE_SIZE - 1)), size); return 0;
}
static int ptebp_read_shadow_inst(const struct ptebp_slot *slot, uint32_t *word) {
    memcpy(word, (char *)slot->shadow_page + (slot->hook_addr & (PAGE_SIZE - 1)), 4); return 0;
}
static bool ptebp_validate_guard_pte(struct mm_struct *mm, uint64_t addr, pte_t orig) {
    (void)mm; return page_ptes[page_index(addr)] == ptebp_make_data_guard_pte(orig);
}

/* At each visible mutation, a target thread can fault or read its code. */
static void observe_target(void) {
    assert(g_ptebp_info && g_ptebp_mm == &target_mm);
    for (size_t i = 0; i < BP_CONFIG_MAX; ++i) {
        const struct bp_point *point = &g_ptebp_info->points[i];
        if (!point->hit_addr) continue;
        const struct ptebp_slot *slot = &g_ptebp_slots[i];
        assert(slot->hook_addr == point->hit_addr && slot->shadow_page);
        size_t page = page_index(slot->hook_addr), word = (slot->hook_addr & (PAGE_SIZE - 1)) / 4;
        uint32_t visible = memory[page][word];
        if (!(page_ptes[page] & PTE_USER)) assert(!ptebp_read_shadow_inst(slot, &visible));
        assert(visible == original[page][word]);
    }
    ++observations;
}
static int write_user_pte_value(struct mm_struct *mm, uint64_t address, pteval_t value) {
    assert(mm == &target_mm);
    assert(mmap_locks > 0);
    if (!(value & PTE_USER)) {
        observe_target();
        if (++guard_writes == fail_guard) return -EIO;
    }
    page_ptes[page_index(address)] = value;
    if (!(value & PTE_USER)) observe_target();
    return 0;
}
static int ptebp_write_inst(struct ptebp_slot *slot, const uint32_t *word) {
    size_t page = page_index(slot->hook_addr), offset = (slot->hook_addr & (PAGE_SIZE - 1)) / 4;
    if (*word == PTEBP_UDF_INST) {
        observe_target();
        assert(!(page_ptes[page] & PTE_USER));
        memory[page][offset] = *word;
        observe_target();
        if (++marker_writes == fail_marker) return -EIO;
    } else memory[page][offset] = *word;
    return 0;
}
static int ptebp_restore_inst(struct ptebp_slot *slot) {
    uint32_t word; ptebp_read_shadow_inst(slot, &word); return ptebp_write_inst(slot, &word);
}

#include PTEBP_PRODUCTION

static void check_case(int guard_failure, int marker_failure, bool invalid_last) {
    struct break_point info = {.tgid = 123};
    info.points[0] = (struct bp_point){0x10000, BP_BREAKPOINT_X};
    info.points[1] = (struct bp_point){0x10004, BP_BREAKPOINT_X};
    info.points[2] = (struct bp_point){invalid_last ? 0x50000 : 0x12000, BP_BREAKPOINT_X};
    memcpy(memory, original, sizeof(memory));
    for (size_t i = 0; i < ARRAY_SIZE(page_ptes); ++i) page_ptes[i] = 0x1001 + i * PAGE_SIZE + PTE_USER;
    fail_guard = guard_failure; fail_marker = marker_failure;
    guard_writes = marker_writes = 0;
    int result = start_ptebp_monitor(&info);
    if (guard_failure || marker_failure || invalid_last) {
        assert(result < 0);
        if (invalid_last) assert(!guard_writes && !marker_writes);
    } else {
        assert(!result && guard_writes == 2 && marker_writes == 3);
        observe_target();
        ptebp_drop_all_monitors(false, g_ptebp_generation + 1);
        assert(g_ptebp_info == &info);
        trylock_succeeds = false;
        ptebp_drop_all_monitors(false, g_ptebp_generation);
        assert(g_ptebp_info == &info && !g_ptebp_stopping && !mmap_locks);
        trylock_succeeds = true;
        ptebp_drop_all_monitors(false, g_ptebp_generation);
        ptebp_stop_locked();
    }
    assert(!g_ptebp_info && !g_ptebp_mm && !g_ptebp_stopping);
    assert(!references && !allocations && !mmap_locks);
    assert(!memcmp(memory, original, sizeof(memory)));
    for (size_t i = 0; i < ARRAY_SIZE(page_ptes); ++i) assert(page_ptes[i] & PTE_USER);
}
int main(void) {
    for (size_t i = 0; i < ARRAY_SIZE(original); ++i)
        for (size_t j = 0; j < ARRAY_SIZE(original[i]); ++j) original[i][j] = 0x91001c00U + i + j;
    check_case(0, 0, false);
    check_case(2, 0, false);
    check_case(0, 2, false);
    check_case(0, 0, true);
    printf("PASS: production PTE lifecycle; %u interleaved observations, shared page, failure rollback, mmap trylock retry, stale-generation rejection\n", observations);
}
