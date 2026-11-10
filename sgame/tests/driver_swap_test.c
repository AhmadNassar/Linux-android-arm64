/* Execute the production reader with deterministic 5.10 / 6.1 / 6.12 API shims.
 * This tests lifetime/lock/error behavior; it does not replace a device test. */
#include <assert.h>
#include <errno.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/types.h>

#define SGAME_SWAP_TEST
#ifndef SGAME_SWAP_LEGACY
#define SGAME_SWAP_LEGACY 0
#endif
#define MODULE_INFO(k, v)
#define UTS_RELEASE "test"
#ifndef PAGE_SHIFT
#define PAGE_SHIFT 12
#endif
#ifndef SGAME_SWAP_MODERN
#define SGAME_SWAP_MODERN 0
#endif
#define PAGE_SIZE (1UL << PAGE_SHIFT)
#define TEST_BASE 0x10000UL
#define TEST_NEXT (TEST_BASE + PAGE_SIZE)
#define PMD_MASK (~((1UL << 21) - 1))
#define HZ 100
#define GFP_HIGHUSER_MOVABLE 0
#define VM_IO 1
#define VM_PFNMAP 2
#define EHWPOISON 133
#define READ_ONCE(v) (v)
#define min_t(t, a, b) ((t)(a) < (t)(b) ? (t)(a) : (t)(b))
#define offset_in_page(v) ((v) & (PAGE_SIZE - 1))
#define time_after_eq(a, b) ((long)((a) - (b)) >= 0)
#define pr_info(...) ((void)0)
#define pr_err(...) ((void)0)
typedef unsigned long long u64;
typedef unsigned char u8;
typedef unsigned long phys_addr_t;
typedef unsigned int gfp_t;
typedef struct { unsigned long val; } pgd_t, p4d_t, pud_t, pmd_t, pte_t, swp_entry_t;
typedef int spinlock_t;
#define DEFINE_SPINLOCK(n) spinlock_t n
struct vm_area_struct { unsigned long vm_start, vm_end, vm_flags; };
struct mm_struct {
    int refs, mmap_locked;
    struct vm_area_struct vma;
    pgd_t pgd; p4d_t p4d; pud_t pud; pmd_t pmd;
    pte_t ptes[8];
};
struct folio;
struct page { struct folio *folio; unsigned long private; int refs; u8 bytes[PAGE_SIZE]; };
struct folio {
    struct page *page;
    swp_entry_t swap;
    unsigned int nr_pages;
    bool swapcache, uptodate, error, locked;
    int lock_error;
};
struct swap_info_struct { int refs; } device;
struct swap_iocb;
static struct mm_struct test_mm, *walking;
static struct page pages[16];
static struct folio folios[16];
static unsigned long jiffies = 1;
static spinlock_t pte_lock;
static int spin_depth, rcu_depth, slot_refs, get_calls, cache_calls, backend_calls, duplicate_error;
static int table_lock_calls, replace_table_at;
static int linear_calls, pte_calls;
static bool hit, missing_device, missing_page;
static void (*during_wait)(void);
static u64 clock_value;

static void spin_lock(spinlock_t *p) { assert(!*p); *p = 1; ++spin_depth; }
static void spin_unlock(spinlock_t *p) { assert(*p); *p = 0; --spin_depth; }
#define spin_lock_irqsave(p, f) do { (f) = 0; spin_lock(p); } while (0)
#define spin_unlock_irqrestore(p, f) do { (void)(f); spin_unlock(p); } while (0)
static void mmget(struct mm_struct *mm) { assert(mm->refs > 0); ++mm->refs; }
static void mmput(struct mm_struct *mm) { assert(!spin_depth); assert(mm->refs > 0); --mm->refs; }
static struct mm_struct *get_mm_by_pid(pid_t pid) {
    ++get_calls;
    if (pid != 42) return NULL;
    mmget(&test_mm); return &test_mm;
}
static int mmap_read_lock_killable(struct mm_struct *mm) { assert(!spin_depth); ++mm->mmap_locked; return 0; }
static void mmap_read_unlock(struct mm_struct *mm) { assert(!spin_depth); --mm->mmap_locked; }
static struct vm_area_struct *find_vma(struct mm_struct *mm, unsigned long addr) {
    assert(mm->mmap_locked); return addr < mm->vma.vm_end ? &mm->vma : NULL;
}
static bool is_vm_hugetlb_page(struct vm_area_struct *vma) { (void)vma; return false; }
#define pgd_offset(mm, addr) ((void)(addr), walking = (mm), &(mm)->pgd)
#define p4d_offset(p, addr) ((void)(p), (void)(addr), &walking->p4d)
#define pud_offset(p, addr) ((void)(p), (void)(addr), &walking->pud)
#define pmd_offset(p, addr) ((void)(p), (void)(addr), &walking->pmd)
#define pgd_none(v) (!(v).val)
#define pgd_bad(v) ((v).val == 99)
#define p4d_none(v) pgd_none(v)
#define p4d_bad(v) pgd_bad(v)
#define pud_none(v) pgd_none(v)
#define pud_bad(v) pgd_bad(v)
#define pud_leaf(v) ((v).val == 2)
#define pmd_none(v) pgd_none(v)
#define pmd_present(v) (((v).val & 1) != 0)
#define pmd_leaf(v) (((v).val & 2) != 0)
#define pmd_devmap(v) (((v).val & 4) != 0)
#define pmd_bad(v) (!(v).val)
#define pmd_pfn(v) ((v).val >> PAGE_SHIFT)
#define pte_present(v) (((v).val & 1) != 0)
#define pte_devmap(v) (((v).val & 4) != 0)
#define pte_pfn(v) ((v).val >> PAGE_SHIFT)
#define is_swap_pte(v) ((v).val && !pte_present(v))
#define pte_to_swp_entry(v) ((swp_entry_t){ (v).val >> PAGE_SHIFT })
#define non_swap_entry(v) ((v).val >= 5000)
#define is_migration_entry(v) ((v).val == 5001)
#define is_hwpoison_entry(v) ((v).val == 5002)
#define swp_offset(v) ((v).val)
#define pte_same(a, b) ((a).val == (b).val)
#define pmd_same(a, b) pte_same(a, b)
#define rcu_read_lock() (++rcu_depth)
#define rcu_read_unlock() assert(rcu_depth-- > 0)
#define pte_offset_kernel(p, addr) ((void)(p), &walking->ptes[((addr) - TEST_BASE) >> PAGE_SHIFT])
static spinlock_t *pte_lockptr(struct mm_struct *mm, pmd_t *value) {
    (void)value;
    if (++table_lock_calls == replace_table_at) mm->pmd.val ^= 8;
    return &pte_lock;
}
static spinlock_t *pmd_lock(struct mm_struct *mm, pmd_t *pmd) { (void)mm; (void)pmd; spin_lock(&pte_lock); return &pte_lock; }
static pte_t *pte_offset_map_lock_fn(struct mm_struct *mm, pmd_t *pmd, unsigned long addr, spinlock_t **ptl) {
    (void)pmd; assert(mm->mmap_locked); *ptl = &pte_lock; spin_lock(*ptl);
    return &mm->ptes[((addr - TEST_BASE) >> PAGE_SHIFT) & 7];
}
#define pte_offset_map_lock(m, p, a, l) pte_offset_map_lock_fn(m, p, a, l)
#define pte_unmap_unlock(p, l) ((void)(p), spin_unlock(l))
#define pfn_valid(v) ((v) < 16)
static int linear_read_physical(phys_addr_t pa, void *out, size_t n) {
    ++linear_calls;
    assert(spin_depth); memcpy(out, pages[pa >> PAGE_SHIFT].bytes + offset_in_page(pa), n); return 0;
}
static int pte_read_physical(phys_addr_t pa, void *out, size_t n) {
    ++pte_calls;
    assert(spin_depth); memcpy(out, pages[pa >> PAGE_SHIFT].bytes + offset_in_page(pa), n); return 0;
}
#define memory_copy memcpy
static u64 ktime_get_ns(void) { return ++clock_value; }
static void cond_resched(void) { assert(!spin_depth); }
static struct page *folio_file_page(struct folio *f, unsigned long idx) { return f->page + (idx & (f->nr_pages - 1)); }
#define folio_page_idx(f, p) ((p) - (f)->page)
static struct folio *page_folio(struct page *p) { return p->folio; }
static int folio_lock_killable(struct folio *f) {
    assert(!spin_depth && !rcu_depth); assert(!f->locked);
    if (during_wait) during_wait();
    if (f->lock_error) return f->lock_error;
    f->locked = true; return 0;
}
static void folio_unlock(struct folio *f) { assert(f->locked); f->locked = false; }
#define folio_test_swapcache(f) ((f)->swapcache)
#define folio_test_uptodate(f) ((f)->uptodate)
#define folio_test_error(f) ((f)->error)
#define page_private(p) ((p)->private)
#define PageHWPoison(p) false
static void *kmap_local_page(struct page *p) { assert(p->folio->locked); return p->bytes; }
static void kunmap_local(void *v) { (void)v; }
#if SGAME_SWAP_LEGACY
static int lock_page_killable(struct page *p) { return folio_lock_killable(p->folio); }
static struct page *compound_head(struct page *p) { return p->folio->page; }
#define might_sleep() assert(!spin_depth && !rcu_depth)
static bool trylock_page(struct page *p) {
    assert(!spin_depth && !rcu_depth && !p->folio->locked);
    if (during_wait || p->folio->lock_error) return false;
    p->folio->locked = true;
    return true;
}
static void unlock_page(struct page *p) { folio_unlock(p->folio); }
#define PageSwapCache(p) ((p)->folio->swapcache)
#define PageUptodate(p) ((p)->folio->uptodate)
#define PageError(p) ((p)->folio->error)
static void *kmap(struct page *p) { return kmap_local_page(p); }
static void kunmap(struct page *p) { (void)p; }
#endif
static void put_page(struct page *p) { assert(!p->folio->locked); assert(p->refs > 0); --p->refs; }
static void put_swap_device(struct swap_info_struct *si) {
    assert(si->refs > 0); --si->refs;
    if (SGAME_SWAP_LEGACY) { rcu_read_unlock(); }
}
enum {
    NO_SWAP_FREE = 1,
    NO_SWAP_FREE_NR = 2,
    NO_FREE_AND_CACHE_NR = 4,
    NO_FREE_AND_CACHE = 8,
    NO_READ_ASYNC = 16,
};
static unsigned int unavailable_symbols;
static unsigned long generic_kallsyms_lookup_name(const char *name);

#include "../../lsdriver/sgame_swap_read.h"

static struct swap_info_struct *get_device(swp_entry_t e) {
    (void)e; assert(spin_depth); if (missing_device) return NULL;
    if (SGAME_SWAP_LEGACY) rcu_read_lock();
    ++device.refs; return &device;
}
static int duplicate(swp_entry_t e) {
    (void)e; assert(spin_depth); if (duplicate_error) return duplicate_error; ++slot_refs; return 0;
}
static void release_slot(swp_entry_t e, unsigned int api) {
    unsigned int expected = !(unavailable_symbols & NO_SWAP_FREE) ? NO_SWAP_FREE :
        !(unavailable_symbols & NO_SWAP_FREE_NR) ? NO_SWAP_FREE_NR :
        !(unavailable_symbols & NO_FREE_AND_CACHE_NR) ? NO_FREE_AND_CACHE_NR : NO_FREE_AND_CACHE;
    (void)e; assert(api == expected);
    assert(!spin_depth && !rcu_depth && slot_refs > 0);
    assert(SGAME_SWAP_LEGACY ? !device.refs : device.refs > 0); --slot_refs;
}
static void free_entry(swp_entry_t e) { release_slot(e, NO_SWAP_FREE); }
static void free_nr(swp_entry_t e, int nr_pages) {
    assert(nr_pages == 1); release_slot(e, NO_SWAP_FREE_NR);
}
static void free_and_cache_nr(swp_entry_t e, int nr_pages) {
    assert(nr_pages == 1); release_slot(e, NO_FREE_AND_CACHE_NR);
}
static int free_and_cache(swp_entry_t e) {
    release_slot(e, NO_FREE_AND_CACHE); return 1;
}
#if SGAME_SWAP_LEGACY
static struct page *cache_get(swp_entry_t e, struct vm_area_struct *v, unsigned long a) {
#else
static struct folio *cache_get(swp_entry_t e, struct vm_area_struct *v, unsigned long a) {
#endif
    (void)v; (void)a; assert(!spin_depth && !rcu_depth); assert(slot_refs);
    assert(SGAME_SWAP_LEGACY ? !device.refs : device.refs > 0); ++cache_calls;
    if (!hit) return NULL;
    ++pages[e.val].refs;
#if SGAME_SWAP_LEGACY
    return &pages[e.val];
#else
    return pages[e.val].folio;
#endif
}
#if SGAME_SWAP_LEGACY
static struct page *read_async(swp_entry_t e, gfp_t g, struct vm_area_struct *v, unsigned long a, bool do_poll) {
    assert(!do_poll);
#elif SGAME_SWAP_MODERN
static struct folio *read_async(swp_entry_t e, gfp_t g, struct vm_area_struct *v, unsigned long a, struct swap_iocb **plug) {
    assert(!plug);
#else
static struct page *read_async(swp_entry_t e, gfp_t g, struct vm_area_struct *v, unsigned long a, struct swap_iocb **plug) {
    assert(!plug);
#endif
    (void)g; (void)v; (void)a; assert(!spin_depth && !rcu_depth); ++backend_calls;
    assert(slot_refs); assert(SGAME_SWAP_LEGACY ? !device.refs : device.refs > 0);
    if (missing_page) return NULL;
    ++pages[e.val].refs;
#if SGAME_SWAP_MODERN
    return pages[e.val].folio;
#else
    return &pages[e.val];
#endif
}
static unsigned long generic_kallsyms_lookup_name(const char *name) {
#if SGAME_SWAP_LEGACY
    if (!strcmp(name, "__lock_page_killable")) return (unsigned long)lock_page_killable;
#endif
    if (!strcmp(name, "get_swap_device")) return (unsigned long)get_device;
    if (!strcmp(name, "swap_duplicate")) return (unsigned long)duplicate;
    if (!strcmp(name, SGAME_SWAP_LEGACY ? "lookup_swap_cache" : "swap_cache_get_folio"))
        return (unsigned long)cache_get;
    if (!strcmp(name, "read_swap_cache_async"))
        return unavailable_symbols & NO_READ_ASYNC ? 0 : (unsigned long)read_async;
    if (!strcmp(name, "swap_free"))
        return unavailable_symbols & NO_SWAP_FREE ? 0 : (unsigned long)free_entry;
    if (!strcmp(name, "swap_free_nr"))
        return unavailable_symbols & NO_SWAP_FREE_NR ? 0 : (unsigned long)free_nr;
    if (!strcmp(name, "free_swap_and_cache_nr"))
        return unavailable_symbols & NO_FREE_AND_CACHE_NR ? 0 : (unsigned long)free_and_cache_nr;
    if (!strcmp(name, "free_swap_and_cache"))
        return unavailable_symbols & NO_FREE_AND_CACHE ? 0 : (unsigned long)free_and_cache;
    return 0;
}
static void clean(void) {
    assert(!spin_depth && !rcu_depth && !slot_refs && !device.refs && !test_mm.mmap_locked);
    for (int i = 0; i < 16; ++i) assert(!pages[i].refs && !folios[i].locked);
}
static void reset(void) {
    clean(); sgame_vmem_drop_mm();
    memset(&test_mm, 0, sizeof(test_mm)); test_mm.refs = 1;
    test_mm.vma = (struct vm_area_struct){TEST_BASE, TEST_BASE + 8 * PAGE_SIZE, 0};
    test_mm.pgd.val = test_mm.p4d.val = test_mm.pud.val = test_mm.pmd.val = 1;
    memset(&sgame_swap_stats, 0, sizeof(sgame_swap_stats));
    for (int i = 0; i < 16; ++i) {
        folios[i] = (struct folio){ .page = &pages[i], .swap = {i}, .nr_pages = 1,
                                   .swapcache = true, .uptodate = true };
        pages[i].folio = &folios[i]; pages[i].private = i; memset(pages[i].bytes, i, PAGE_SIZE);
    }
    hit = true; missing_device = missing_page = false; during_wait = NULL;
    duplicate_error = cache_calls = backend_calls = get_calls = 0;
    linear_calls = pte_calls = 0;
    table_lock_calls = replace_table_at = 0;
    memset(&sgame_swap_api, 0, sizeof(sgame_swap_api));
    assert(sgame_swap_init() == 0);
}
static void make_resident(unsigned int index, unsigned long pfn) { test_mm.ptes[index].val = (pfn << PAGE_SHIFT) | 1; }
static void make_swap(unsigned int index, unsigned long slot) { test_mm.ptes[index].val = (slot << PAGE_SHIFT) | 2; }
static void remap(void) { make_resident(0, 4); during_wait = NULL; }
static void lose_cache(void) { folios[3].swapcache = false; }
static void finish_io(void) { folios[3].uptodate = true; }
static void drop_cache(void) { sgame_vmem_drop_mm(); assert(test_mm.refs == 2); }
static void remove_table(void) { test_mm.pmd.val = 0; during_wait = NULL; }

static void read_cases(unsigned int missing) {
    u8 out[1752];
    unavailable_symbols = missing;
    reset(); make_resident(0, 2);
    assert(sgame_swap_read(&test_mm, TEST_BASE, out, 8) == 8 && out[0] == 2);
    assert(linear_calls == !SGAME_READ_NC && pte_calls == SGAME_READ_NC);
    assert(!cache_calls && !backend_calls); clean();
    reset(); make_swap(0, 3);
    assert(sgame_swap_read(&test_mm, TEST_BASE, out, 8) == 8 && out[0] == 3);
    assert(cache_calls == 1 && !backend_calls); clean();
    assert(!linear_calls && !pte_calls); // Swap folios always use their coherent kernel mapping.
    reset(); hit = false; make_swap(0, 3); folios[3].uptodate = false; during_wait = finish_io;
    assert(sgame_swap_read(&test_mm, TEST_BASE, out, 8) == 8 && out[0] == 3);
    assert(backend_calls == 1); clean();
    reset(); make_resident(0, 2); make_swap(1, 3); hit = false;
    assert(sgame_swap_read(&test_mm, TEST_NEXT - 876, out, 1752) == 1752);
    for (int i = 0; i < 1752; ++i) assert(out[i] == (i < 876 ? 2 : 3)); clean();
    reset(); make_resident(0, 2);
    memset(out, 9, sizeof(out));
    assert(sgame_swap_read(&test_mm, TEST_NEXT - 876, out, 1752) == 876);
    for (int i = 876; i < 1752; ++i) assert(out[i] == 0); clean();
    reset(); assert(sgame_swap_read(&test_mm, TEST_BASE, out, 8) == -EFAULT);
    assert(!cache_calls && !backend_calls); clean();
    reset(); make_swap(0, 5001);
    assert(sgame_swap_read(&test_mm, TEST_BASE, out, 8) == -EAGAIN);
    assert(sgame_swap_stats.retries == 2 && !cache_calls); clean();
    reset(); make_swap(0, 5002);
    assert(sgame_swap_read(&test_mm, TEST_BASE, out, 8) == -EHWPOISON); clean();
    reset(); make_swap(0, 5003);
    assert(sgame_swap_read(&test_mm, TEST_BASE, out, 8) == -EOPNOTSUPP); clean();
    reset(); make_swap(0, 3); during_wait = remap;
    assert(sgame_swap_read(&test_mm, TEST_BASE, out, 8) == 8 && out[0] == 4);
    assert(sgame_swap_stats.retries == 1); clean();
    reset(); make_swap(0, 3); during_wait = lose_cache;
    assert(sgame_swap_read(&test_mm, TEST_BASE, out, 8) == -EAGAIN);
    assert(cache_calls == 3); clean();
    reset(); make_swap(0, 3); pages[3].private = 4; folios[3].swap.val = 4;
    assert(sgame_swap_read(&test_mm, TEST_BASE, out, 8) == -EAGAIN); clean();
    reset(); make_swap(0, 3); folios[3].uptodate = false;
    assert(sgame_swap_read(&test_mm, TEST_BASE, out, 8) == -EIO); clean();
    reset(); make_swap(0, 3); folios[3].lock_error = -EINTR;
    assert(sgame_swap_read(&test_mm, TEST_BASE, out, 8) == -EINTR); clean();
    reset(); make_swap(0, 3); hit = false; missing_page = true;
    assert(sgame_swap_read(&test_mm, TEST_BASE, out, 8) == -ENOMEM); clean();
    reset(); make_swap(0, 3); duplicate_error = -ENOMEM;
    assert(sgame_swap_read(&test_mm, TEST_BASE, out, 8) == -ENOMEM); clean();
    reset(); make_swap(0, 3); missing_device = true;
    assert(sgame_swap_read(&test_mm, TEST_BASE, out, 8) == -EAGAIN); clean();
    reset(); test_mm.vma.vm_flags = VM_PFNMAP;
    assert(sgame_swap_read(&test_mm, TEST_BASE, out, 8) == -EOPNOTSUPP); clean();
    reset(); make_resident(0, 2); test_mm.ptes[0].val |= 4;
    assert(sgame_swap_read(&test_mm, TEST_BASE, out, 8) == -EOPNOTSUPP); clean();
    reset(); make_swap(0, 3);
    struct mm_struct *mm = sgame_vmem_get_mm(42); assert(mm && mm->refs == 3);
    struct mm_struct *other = sgame_vmem_get_mm(42); assert(get_calls == 1 && mm->refs == 4);
    mmput(other); during_wait = drop_cache;
    assert(sgame_swap_read(mm, TEST_BASE, out, 8) == 8); mmput(mm);
    assert(test_mm.refs == 1); clean();
    assert(sgame_vmem_get_mm(123) == NULL); clean();
}

int main(void) {
    read_cases(0);
    assert(sgame_swap_api.free_entry == free_entry);
    read_cases(NO_SWAP_FREE);
    assert(sgame_swap_api.free_nr == free_nr);
    read_cases(NO_SWAP_FREE | NO_SWAP_FREE_NR);
    assert(sgame_swap_api.free_and_cache_nr == free_and_cache_nr);
    read_cases(NO_SWAP_FREE | NO_SWAP_FREE_NR | NO_FREE_AND_CACHE_NR);
    assert(sgame_swap_api.free_and_cache == free_and_cache);
    /* Customer 6.1.138 backport exposes both batched APIs and neither wrapper. */
    read_cases(NO_SWAP_FREE | NO_FREE_AND_CACHE);
    assert(sgame_swap_api.free_nr == free_nr);

#if SGAME_SWAP_LEGACY
    for (int from_cache = 0; from_cache < 2; ++from_cache) {
        u8 out[8];
        reset(); make_swap(0, 3); hit = from_cache;
        folios[2].nr_pages = 2; pages[3].folio = &folios[2];
        assert(sgame_swap_read(&test_mm, TEST_BASE, out, sizeof(out)) == sizeof(out));
        assert(out[0] == 3 && cache_calls == 1 && backend_calls == !from_cache); clean();
    }
    puts("PASS: 5.10 compound-page tails from cache/backend retain the exact slot and head lock");
#endif

#if SGAME_SWAP_MODERN
    u8 out[8];
    reset(); make_resident(0, 2); replace_table_at = 1;
    assert(sgame_swap_read(&test_mm, TEST_BASE, out, sizeof(out)) == sizeof(out));
    assert(out[0] == 2 && sgame_swap_stats.retries == 1); clean();
    reset(); make_swap(0, 3); replace_table_at = 2;
    assert(sgame_swap_read(&test_mm, TEST_BASE, out, sizeof(out)) == sizeof(out));
    assert(out[0] == 3 && sgame_swap_stats.retries == 1 && cache_calls == 2); clean();
    reset(); make_swap(0, 3); during_wait = remove_table;
    assert(sgame_swap_read(&test_mm, TEST_BASE, out, sizeof(out)) == -EFAULT);
    assert(sgame_swap_stats.retries == 1); clean();
    for (int from_cache = 0; from_cache < 2; ++from_cache) {
        reset(); make_swap(0, 3); hit = from_cache;
        folios[2].nr_pages = 2; pages[3].folio = &folios[2];
        pages[3].private = 0; /* 6.12 has no per-tail private swap entry. */
        assert(sgame_swap_read(&test_mm, TEST_BASE, out, sizeof(out)) == sizeof(out));
        assert(out[0] == 3 && cache_calls == 1 && backend_calls == !from_cache); clean();
    }
    puts("PASS: 6.12 PTE table replacement/removal and large-folio tail reads from cache/backend");
#endif

    memset(&sgame_swap_api, 0, sizeof(sgame_swap_api));
    unavailable_symbols = NO_SWAP_FREE | NO_SWAP_FREE_NR | NO_FREE_AND_CACHE_NR | NO_FREE_AND_CACHE;
    assert(sgame_swap_init() == -ENOENT);
    clean();
    memset(&sgame_swap_api, 0, sizeof(sgame_swap_api));
    unavailable_symbols = NO_READ_ASYNC;
    assert(sgame_swap_init() == -ENOENT);
    clean();
    printf("driver_swap_test: API=%s cached/NC=%d: 105 reader cases + 2 initialization failures passed (balanced references, no RCU across I/O)\n",
           SGAME_SWAP_LEGACY ? "5.10-page" : SGAME_SWAP_MODERN ? "6.12-folio" : "6.1-folio", SGAME_READ_NC);
    return 0;
}
