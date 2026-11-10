/* SPDX-License-Identifier: GPL-2.0 */
#ifndef SGAME_SWAP_READ_H
#define SGAME_SWAP_READ_H

#ifndef SGAME_READ_NC
#define SGAME_READ_NC 0
#endif

/* Included after linear_read_physical() in the unmodified author's reader. */
#ifndef SGAME_SWAP_TEST
#include <linux/highmem.h>
#include <linux/hugetlb.h>
#include <linux/ktime.h>
#include <linux/pagemap.h>
#include <linux/swap.h>
#include <linux/swapops.h>
#include <linux/version.h>
#include <generated/utsrelease.h>

#if !((LINUX_VERSION_CODE >= KERNEL_VERSION(5, 10, 0) && LINUX_VERSION_CODE < KERNEL_VERSION(5, 11, 0)) || \
      (LINUX_VERSION_CODE >= KERNEL_VERSION(6, 1, 0) && LINUX_VERSION_CODE < KERNEL_VERSION(6, 2, 0)) || \
      (LINUX_VERSION_CODE >= KERNEL_VERSION(6, 12, 0) && LINUX_VERSION_CODE < KERNEL_VERSION(6, 13, 0)))
#error "sgame swap supports the reviewed Android 5.10, 6.1 and 6.12 APIs"
#endif
#ifndef CONFIG_SWAP
#error "sgame swap requires CONFIG_SWAP"
#endif
#endif

#ifndef SGAME_SWAP_LEGACY
#if !defined(SGAME_SWAP_TEST) && LINUX_VERSION_CODE < KERNEL_VERSION(5, 11, 0)
#define SGAME_SWAP_LEGACY 1
#else
#define SGAME_SWAP_LEGACY 0
#endif
#endif

#ifndef SGAME_SWAP_MODERN
#ifndef SGAME_SWAP_TEST
#if LINUX_VERSION_CODE >= KERNEL_VERSION(6, 12, 0)
#define SGAME_SWAP_MODERN 1
#else
#define SGAME_SWAP_MODERN 0
#endif
#else
#define SGAME_SWAP_MODERN 0
#endif
#endif

#define SGAME_SWAP_VERSION "1.2.0"
#define SGAME_SWAP_ATTEMPTS 3
MODULE_INFO(sgame_swap, SGAME_SWAP_VERSION);

static struct {
    struct swap_info_struct *(*get_device)(swp_entry_t entry);
    int (*duplicate)(swp_entry_t entry);
    void (*free_entry)(swp_entry_t entry);
    void (*free_nr)(swp_entry_t entry, int nr_pages);
    void (*free_and_cache_nr)(swp_entry_t entry, int nr_pages);
    int (*free_and_cache)(swp_entry_t entry);
#if SGAME_SWAP_LEGACY
    struct page *(*cache_get)(swp_entry_t entry, struct vm_area_struct *vma, unsigned long addr);
    struct page *(*read_async)(swp_entry_t entry, gfp_t mask, struct vm_area_struct *vma,
                               unsigned long addr, bool do_poll);
#else
    struct folio *(*cache_get)(swp_entry_t entry, struct vm_area_struct *vma, unsigned long addr);
#if SGAME_SWAP_MODERN
    struct folio *(*read_async)(swp_entry_t entry, gfp_t mask, struct vm_area_struct *vma,
#else
    struct page *(*read_async)(swp_entry_t entry, gfp_t mask, struct vm_area_struct *vma,
#endif
                               unsigned long addr, struct swap_iocb **plug);
#endif
} sgame_swap_api;

static int sgame_swap_init(void)
{
    const char *release;

#define SGAME_SWAP_SYMBOL(member, name) do { \
    sgame_swap_api.member = (void *)generic_kallsyms_lookup_name(name); \
    if (!sgame_swap_api.member) { \
        pr_err("lsdriver: swap: missing kernel symbol %s\n", name); \
        return -ENOENT; \
    } \
} while (0)
    SGAME_SWAP_SYMBOL(get_device, "get_swap_device");
    SGAME_SWAP_SYMBOL(duplicate, "swap_duplicate");
#if SGAME_SWAP_LEGACY
    SGAME_SWAP_SYMBOL(cache_get, "lookup_swap_cache");
#else
    SGAME_SWAP_SYMBOL(cache_get, "swap_cache_get_folio");
#endif
    SGAME_SWAP_SYMBOL(read_async, "read_swap_cache_async");
#undef SGAME_SWAP_SYMBOL
    /* Some vendor 6.1 kernels backport the batched APIs: swap_free() and
     * free_swap_and_cache() become inline wrappers passing nr=1. Select
     * by available API, never by device model or kernel release string.
     * Resolve exact names only: .isra/.constprop clones can change ABI. */
    sgame_swap_api.free_entry = (void *)generic_kallsyms_lookup_name("swap_free");
    sgame_swap_api.free_nr = (void *)generic_kallsyms_lookup_name("swap_free_nr");
    sgame_swap_api.free_and_cache_nr = (void *)generic_kallsyms_lookup_name("free_swap_and_cache_nr");
    sgame_swap_api.free_and_cache = (void *)generic_kallsyms_lookup_name("free_swap_and_cache");
    if (sgame_swap_api.free_entry) release = "swap_free";
    else if (sgame_swap_api.free_nr) release = "swap_free_nr";
    else if (sgame_swap_api.free_and_cache_nr) release = "free_swap_and_cache_nr";
    else if (sgame_swap_api.free_and_cache) release = "free_swap_and_cache";
    else {
        pr_err("lsdriver: swap: no slot release symbol (swap_free, swap_free_nr, free_swap_and_cache_nr, free_swap_and_cache)\n");
        return -ENOENT;
    }
    pr_info("lsdriver: swap: version=%s kernel=%s release=%s ready\n",
            SGAME_SWAP_VERSION, UTS_RELEASE, release);
    return 0;
}

static void sgame_swap_release_entry(swp_entry_t entry)
{
    if (sgame_swap_api.free_entry)
        sgame_swap_api.free_entry(entry);
    else if (sgame_swap_api.free_nr)
        sgame_swap_api.free_nr(entry, 1);
    else if (sgame_swap_api.free_and_cache_nr)
        sgame_swap_api.free_and_cache_nr(entry, 1);
    else
        sgame_swap_api.free_and_cache(entry);
}

/* The exit callback can drop the cache while the dispatch thread sleeps in I/O.
 * Both the cache and each request own an mm_users reference. Never mmput under
 * the cache spinlock: the final put can run exit_mmap(). */
static DEFINE_SPINLOCK(sgame_mm_lock);
static struct mm_struct *sgame_cached_mm;
static pid_t sgame_cached_pid;

static void sgame_vmem_drop_mm(void)
{
    struct mm_struct *old;
    unsigned long flags;

    spin_lock_irqsave(&sgame_mm_lock, flags);
    old = sgame_cached_mm;
    sgame_cached_mm = NULL;
    sgame_cached_pid = 0;
    spin_unlock_irqrestore(&sgame_mm_lock, flags);
    if (old) mmput(old);
}

static struct mm_struct *sgame_vmem_get_mm(pid_t pid)
{
    struct mm_struct *mm = NULL, *old;
    unsigned long flags;

    spin_lock_irqsave(&sgame_mm_lock, flags);
    if (sgame_cached_mm && sgame_cached_pid == pid) {
        mm = sgame_cached_mm;
        mmget(mm);
    }
    spin_unlock_irqrestore(&sgame_mm_lock, flags);
    if (mm) return mm;

    mm = get_mm_by_pid(pid);
    if (!mm) return NULL;
    spin_lock_irqsave(&sgame_mm_lock, flags);
    old = sgame_cached_mm;
    sgame_cached_mm = mm;
    sgame_cached_pid = pid;
    mmget(mm);
    spin_unlock_irqrestore(&sgame_mm_lock, flags);
    if (old) mmput(old);
    return mm;
}

/* Updated only by the one upstream dispatch thread. */
static struct {
    u64 reads, resident, swapped, hits, backend, retries, errors, bytes;
    u64 io_ns, max_io_ns;
    unsigned long next_log;
} sgame_swap_stats;

static int sgame_swap_find_pmd(struct mm_struct *mm, unsigned long addr, pmd_t **result)
{
    pgd_t *pgd = pgd_offset(mm, addr);
    p4d_t *p4d;
    pud_t *pud;
    pud_t value;

    if (pgd_none(*pgd) || pgd_bad(*pgd)) return -EFAULT;
    p4d = p4d_offset(pgd, addr);
    if (p4d_none(*p4d) || p4d_bad(*p4d)) return -EFAULT;
    pud = pud_offset(p4d, addr);
    value = READ_ONCE(*pud);
    if (pud_leaf(value)) return -EOPNOTSUPP;
    if (pud_none(value) || pud_bad(value)) return -EFAULT;
    *result = pmd_offset(pud, addr);
    return 0;
}

static int sgame_swap_copy_resident(unsigned long pfn, unsigned long addr,
                                    void *buffer, size_t size)
{
    if (!pfn_valid(pfn)) return -EFAULT;
#if SGAME_READ_NC
    return pte_read_physical(((phys_addr_t)pfn << PAGE_SHIFT) + offset_in_page(addr), buffer, size);
#else
    return linear_read_physical(((phys_addr_t)pfn << PAGE_SHIFT) + offset_in_page(addr), buffer, size);
#endif
}

/* 6.12 can remove a PTE table under mmap_read_lock. Follow its RCU + parent
 * recheck protocol, using arm64's lowmem tables without importing helpers
 * outside the GKI KMI. No RCU section or PTL survives a blocking swap read. */
static pte_t *sgame_swap_pte_lock(struct mm_struct *mm, pmd_t *pmd,
                                 unsigned long addr, spinlock_t **ptlp)
{
#if SGAME_SWAP_MODERN
    pmd_t value;
    pte_t *pte;
    spinlock_t *ptl;

    rcu_read_lock();
    value = READ_ONCE(*pmd);
    if (!pmd_present(value) || pmd_leaf(value) || pmd_bad(value)) {
        rcu_read_unlock();
        return NULL;
    }
    pte = pte_offset_kernel(&value, addr);
    ptl = pte_lockptr(mm, &value);
    spin_lock(ptl);
    if (!pmd_same(value, READ_ONCE(*pmd))) {
        spin_unlock(ptl);
        rcu_read_unlock();
        return NULL;
    }
    *ptlp = ptl;
    return pte;
#else
    return pte_offset_map_lock(mm, pmd, addr, ptlp);
#endif
}

static void sgame_swap_pte_unlock(pte_t *pte, spinlock_t *ptl)
{
#if SGAME_SWAP_MODERN
    (void)pte;
    spin_unlock(ptl);
    rcu_read_unlock();
#else
    pte_unmap_unlock(pte, ptl);
#endif
}

/* Leaf PMDs need their own lock; a migration PMD must never be interpreted
 * as a PTE table. The caller retains mmap_read_lock and an mm_users ref. */
static int sgame_swap_read_page(struct mm_struct *mm, struct vm_area_struct *vma,
                                 unsigned long addr, void *buffer, size_t size)
{
    struct swap_info_struct *si;
#if !SGAME_SWAP_LEGACY
    struct folio *folio;
#endif
    struct page *page;
    spinlock_t *ptl;
    pmd_t *pmd, pmdval;
    pte_t *ptep, orig;
    swp_entry_t entry;
    void *mapped;
    u64 started;
    int status;

    status = sgame_swap_find_pmd(mm, addr, &pmd);
    if (status) return status;
    pmdval = READ_ONCE(*pmd);
    if (pmd_none(pmdval)) return -EFAULT;
    if (!pmd_present(pmdval)) return -EAGAIN;
    if (pmd_leaf(pmdval)) {
        ptl = pmd_lock(mm, pmd);
        pmdval = READ_ONCE(*pmd);
        if (pmd_leaf(pmdval) && !pmd_devmap(pmdval)) {
            unsigned long pfn = pmd_pfn(pmdval) + ((addr & ~PMD_MASK) >> PAGE_SHIFT);
            status = sgame_swap_copy_resident(pfn, addr, buffer, size);
            spin_unlock(ptl);
            if (!status) ++sgame_swap_stats.resident;
            return status;
        }
        spin_unlock(ptl);
        if (pmd_none(pmdval)) return -EFAULT;
        if (!pmd_present(pmdval)) return -EAGAIN;
        if (pmd_leaf(pmdval)) return -EOPNOTSUPP;
    }
    if (pmd_bad(pmdval)) return -EFAULT;

    ptep = sgame_swap_pte_lock(mm, pmd, addr, &ptl);
    if (!ptep) return -EAGAIN;
    orig = READ_ONCE(*ptep);
    if (pte_present(orig)) {
        status = pte_devmap(orig) ? -EOPNOTSUPP :
                 sgame_swap_copy_resident(pte_pfn(orig), addr, buffer, size);
        sgame_swap_pte_unlock(ptep, ptl);
        if (!status) ++sgame_swap_stats.resident;
        return status;
    }
    if (!is_swap_pte(orig)) {
        sgame_swap_pte_unlock(ptep, ptl);
        return -EFAULT;
    }
    entry = pte_to_swp_entry(orig);
    if (non_swap_entry(entry)) {
        status = is_migration_entry(entry) ? -EAGAIN :
                 is_hwpoison_entry(entry) ? -EHWPOISON : -EOPNOTSUPP;
        sgame_swap_pte_unlock(ptep, ptl);
        return status;
    }

    /* PTL keeps this PTE's entry alive until we acquire both protections:
     * a device reference prevents swapoff; duplicate prevents slot reuse.
     * swap_duplicate may allocate only with GFP_ATOMIC on count overflow. */
    si = sgame_swap_api.get_device(entry);
    status = si ? sgame_swap_api.duplicate(entry) : -EAGAIN;
#if SGAME_SWAP_LEGACY
    /* 5.10 get_swap_device() holds RCU, not a sleepable percpu reference.
     * The duplicated slot keeps inuse_pages nonzero and prevents swapoff's
     * try_to_unuse() from finishing. Drop RCU before any blocking work. */
    if (si) {
        put_swap_device(si);
        si = NULL;
    }
#endif
    sgame_swap_pte_unlock(ptep, ptl);
    if (status) {
        if (si) put_swap_device(si);
        return status;
    }

    ++sgame_swap_stats.swapped;
    started = ktime_get_ns();
#if SGAME_SWAP_LEGACY
    page = sgame_swap_api.cache_get(entry, vma, addr);
    if (page) {
        ++sgame_swap_stats.hits;
    } else {
        ++sgame_swap_stats.backend;
        page = sgame_swap_api.read_async(entry, GFP_HIGHUSER_MOVABLE, vma, addr, false);
    }
#else
    folio = sgame_swap_api.cache_get(entry, vma, addr);
    if (folio) {
        ++sgame_swap_stats.hits;
        page = folio_file_page(folio, swp_offset(entry));
    } else {
        ++sgame_swap_stats.backend;
        /* NULL plug submits filesystem swap I/O immediately. The kernel
         * selects zRAM/zswap/block/file; there is no device name assumption. */
#if SGAME_SWAP_MODERN
        folio = sgame_swap_api.read_async(entry, GFP_HIGHUSER_MOVABLE, vma, addr, NULL);
        page = folio ? folio_file_page(folio, swp_offset(entry)) : NULL;
#else
        page = sgame_swap_api.read_async(entry, GFP_HIGHUSER_MOVABLE, vma, addr, NULL);
        folio = page ? page_folio(page) : NULL;
#endif
    }
#endif
    status = -ENOMEM;
    if (!page) goto out_entry;

    /* read_async can return a locked, not-yet-uptodate page. No PTL is held
     * while waiting. The folio lock also stabilizes swapcache ownership. */
#if SGAME_SWAP_LEGACY
    status = lock_page_killable(page);
#else
    status = folio_lock_killable(folio);
#endif
    if (status) goto out_page;
    status = -EAGAIN;
#if SGAME_SWAP_LEGACY
    if (!PageSwapCache(page)) goto out_folio;
#else
    if (!folio_test_swapcache(folio)) goto out_folio;
#endif
#if SGAME_SWAP_MODERN
    /* 6.12 stores one base entry in the head folio, including large folios;
     * tail page->private no longer contains its individual swap entry. */
    if (folio->swap.val + folio_page_idx(folio, page) != entry.val) goto out_folio;
#else
    if (page_private(page) != entry.val) goto out_folio;
#endif
    if (PageHWPoison(page)) {
        status = -EHWPOISON;
        goto out_folio;
    }
#if SGAME_SWAP_LEGACY
    if (!PageUptodate(page) || PageError(page)) {
#else
    if (!folio_test_uptodate(folio) || folio_test_error(folio)) {
#endif
        status = -EIO;
        goto out_folio;
    }

    /* An unchanged PTE alone is insufficient without the slot and folio
     * protections above. Copy under PTL so MADV_DONTNEED cannot invalidate
     * the mapping between this check and the copy. Do not install a PTE. */
    ptep = sgame_swap_pte_lock(mm, pmd, addr, &ptl);
    if (!ptep) goto out_folio;
    if (pte_same(READ_ONCE(*ptep), orig)) {
#if SGAME_SWAP_LEGACY
        mapped = kmap(page);
#else
        mapped = kmap_local_page(page);
#endif
        memory_copy(buffer, (u8 *)mapped + offset_in_page(addr), size);
#if SGAME_SWAP_LEGACY
        kunmap(page);
#else
        kunmap_local(mapped);
#endif
        status = 0;
    }
    sgame_swap_pte_unlock(ptep, ptl);
out_folio:
#if SGAME_SWAP_LEGACY
    unlock_page(page);
#else
    folio_unlock(folio);
#endif
out_page:
    put_page(page);
out_entry:
    sgame_swap_release_entry(entry);
    if (si) put_swap_device(si);
    started = ktime_get_ns() - started;
    sgame_swap_stats.io_ns += started;
    if (started > sgame_swap_stats.max_io_ns) sgame_swap_stats.max_io_ns = started;
    return status;
}

static int sgame_swap_read(struct mm_struct *mm, unsigned long addr, void *buffer, size_t size)
{
    size_t processed = 0, done = 0;
    int error = 0, status;
    u64 before = sgame_swap_stats.swapped, started = ktime_get_ns();

    status = mmap_read_lock_killable(mm);
    if (status) return status;
    ++sgame_swap_stats.reads;
    while (processed < size) {
        struct vm_area_struct *vma = find_vma(mm, addr);
        size_t width = min_t(size_t, PAGE_SIZE - offset_in_page(addr), size - processed);
        unsigned int attempt;

        status = -EFAULT;
        if (vma && addr >= vma->vm_start) {
            if ((vma->vm_flags & (VM_IO | VM_PFNMAP)) || is_vm_hugetlb_page(vma)) {
                status = -EOPNOTSUPP;
            } else {
                for (attempt = 0; attempt < SGAME_SWAP_ATTEMPTS; ++attempt) {
                    status = sgame_swap_read_page(mm, vma, addr, (u8 *)buffer + processed, width);
                    if (status != -EAGAIN || attempt + 1 == SGAME_SWAP_ATTEMPTS) break;
                    ++sgame_swap_stats.retries;
                    cond_resched();
                }
            }
        }
        if (!status) done += width;
        else {
            if (!error) error = status;
            ++sgame_swap_stats.errors;
            __builtin_memset((u8 *)buffer + processed, 0, width);
        }
        addr += width;
        processed += width;
    }
    mmap_read_unlock(mm);
    sgame_swap_stats.bytes += done;
    if (error && size <= sizeof(u64)) __builtin_memset(buffer, 0, size);

    /* One aggregate log per 5s of swap activity, including partial byte count.
     * This is diagnostic accounting, not an I/O timeout or cancellation. */
    if (sgame_swap_stats.swapped != before &&
        (!sgame_swap_stats.next_log || time_after_eq(jiffies, sgame_swap_stats.next_log))) {
        sgame_swap_stats.next_log = jiffies + 5 * HZ;
        pr_info("lsdriver: swap: request=%zu done=%zu error=%d elapsed_us=%llu reads=%llu resident=%llu swapped=%llu cache_hits=%llu backend=%llu retries=%llu errors=%llu bytes=%llu io_us=%llu max_io_us=%llu\n",
                size, done, error, (ktime_get_ns() - started) / 1000,
                sgame_swap_stats.reads, sgame_swap_stats.resident, sgame_swap_stats.swapped,
                sgame_swap_stats.hits, sgame_swap_stats.backend, sgame_swap_stats.retries,
                sgame_swap_stats.errors, sgame_swap_stats.bytes,
                sgame_swap_stats.io_ns / 1000, sgame_swap_stats.max_io_ns / 1000);
    }
    return done ? (int)done : error;
}

#endif
