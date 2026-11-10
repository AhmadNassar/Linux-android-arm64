/* SPDX-License-Identifier: GPL-2.0 */
#ifndef SGAME_SIM_VMA_NAME_510_H
#define SGAME_SIM_VMA_NAME_510_H

/* Android 5.10 stores PR_SET_VMA's name as a target userspace pointer.
 * Follow fs/proc/task_mmu.c: GUP the target mm while its mmap lock is held.
 * The dispatch kthread must never dereference this pointer in its own mm. */
static bool sgame_sim_vma_name_510(struct mm_struct *mm,
                                  const char __user *name,
                                  const char *expected)
{
    struct page *pages[2];
    char observed[32];
    unsigned long addr, offset;
    size_t bytes, copied = 0;
    int count, pinned, i;

    if (!name)
        return false;
    bytes = strlen(expected) + 1;
    if (bytes > sizeof(observed))
        return false;
    addr = untagged_addr((unsigned long)name);
    offset = offset_in_page(addr);
    count = DIV_ROUND_UP(offset + bytes, PAGE_SIZE);
    pinned = get_user_pages_remote(mm, addr & PAGE_MASK, count,
                                   FOLL_FORCE, pages, NULL, NULL);
    if (pinned != count) {
        if (pinned > 0)
            release_pages(pages, pinned);
        return false;
    }
    for (i = 0; i < count; ++i) {
        size_t width = min_t(size_t, PAGE_SIZE - offset, bytes - copied);
        void *mapped = kmap(pages[i]);

        memcpy(observed + copied, (char *)mapped + offset, width);
        kunmap(pages[i]);
        copied += width;
        offset = 0;
    }
    release_pages(pages, count);
    return memcmp(observed, expected, bytes) == 0;
}

#endif
