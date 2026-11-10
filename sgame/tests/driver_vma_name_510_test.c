#include <assert.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#define __user
#define PAGE_SIZE 4096UL
#define PAGE_MASK (~(PAGE_SIZE - 1))
#define FOLL_FORCE 16
#define untagged_addr(a) (a)
#define offset_in_page(a) ((a) & (PAGE_SIZE - 1))
#define DIV_ROUND_UP(a, b) (((a) + (b) - 1) / (b))
#define min_t(t, a, b) ((t)(a) < (t)(b) ? (t)(a) : (t)(b))
struct page { char bytes[PAGE_SIZE]; int refs; };
struct mm_struct { struct page pages[2]; bool locked; } mm;
static int pin_result = -99, pin_calls;
static long get_user_pages_remote(struct mm_struct *target, unsigned long start,
                                  int count, int flags, struct page **pages,
                                  void *vmas, void *locked)
{
    assert(target == &mm && mm.locked && flags == FOLL_FORCE && !vmas && !locked);
    assert(count <= 2 && start == PAGE_SIZE);
    ++pin_calls;
    const int result = pin_result == -99 ? count : pin_result;
    for (int i = 0; i < result; ++i) { pages[i] = &mm.pages[i]; ++pages[i]->refs; }
    return result;
}
static void release_pages(struct page **pages, int count)
{
    for (int i = 0; i < count; ++i) { assert(pages[i]->refs == 1); --pages[i]->refs; }
}
static void *kmap(struct page *page) { assert(page->refs == 1); return page->bytes; }
static void kunmap(struct page *page) { assert(page->refs == 1); }
#include "../../lsdriver/sgame_simulation/vma_name_510.h"

int main(void)
{
    const char expected[] = "stack_and_tls:1234";
    mm.locked = true;
    for (unsigned long offset = 0; offset < PAGE_SIZE; ++offset) {
        memset(mm.pages[0].bytes, 0, PAGE_SIZE);
        memset(mm.pages[1].bytes, 0, PAGE_SIZE);
        for (size_t i = 0; i < sizeof(expected); ++i)
            mm.pages[(offset + i) / PAGE_SIZE].bytes[(offset + i) % PAGE_SIZE] = expected[i];
        assert(sgame_sim_vma_name_510(&mm, (char *)(PAGE_SIZE + offset), expected));
        assert(!sgame_sim_vma_name_510(&mm, (char *)(PAGE_SIZE + offset), "stack_and_tls:123"));
        assert(!mm.pages[0].refs && !mm.pages[1].refs);
    }
    pin_result = 1;
    assert(!sgame_sim_vma_name_510(&mm, (char *)(2 * PAGE_SIZE - 4), expected));
    assert(!mm.pages[0].refs && !mm.pages[1].refs);
    pin_result = -14;
    assert(!sgame_sim_vma_name_510(&mm, (char *)PAGE_SIZE, expected));
    const int calls = pin_calls;
    assert(!sgame_sim_vma_name_510(&mm, NULL, expected));
    assert(calls == pin_calls);
    puts("PASS: Android 5.10 target VMA names at all 4096 offsets, exact match, partial pinning and fault cleanup");
}
