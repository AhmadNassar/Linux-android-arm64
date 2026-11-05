/* Exercise the production IDR mark walker, including allocated NULL entries. */
#include <assert.h>
#include <errno.h>
#include <limits.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

typedef uint32_t u32;
typedef uint8_t u8;
typedef unsigned int gfp_t;
typedef unsigned int xa_mark_t;
#define XA_CHUNK_SHIFT 6
#define XA_CHUNK_SIZE (1UL << XA_CHUNK_SHIFT)
#define XA_CHUNK_MASK (XA_CHUNK_SIZE - 1)
#define XA_FREE_MARK 0
#define XA_FLAGS_TRACK_FREE 4
#define IDR_RT_MARKER 4
#define BITS_PER_LONG 64
#define XAS_BOUNDS ((struct xa_node *)1UL)
#define XAS_RESTART ((struct xa_node *)3UL)
#define XA_RETRY_ENTRY ((void *)1026UL)
#define unlikely(x) (x)
#define KGSL_SYNCSOURCE_XAS_RETRIES 8U
#define KGSL_SYNCSOURCE_STABLE_TRIES 3U

struct xarray { gfp_t xa_flags; void *xa_head; };
struct xa_node {
    u8 shift, offset, count, nr_values;
    struct xa_node *parent;
    struct xarray *array;
    void *slots[64];
    unsigned long marks[3][1];
};
struct xa_state {
    struct xarray *xa;
    unsigned long xa_index;
    struct xa_node *xa_node;
    unsigned int xa_offset;
};
struct idr { struct xarray idr_rt; unsigned int idr_base, idr_next; };
#define XA_STATE(name, array, index) \
    struct xa_state name = {.xa = array, .xa_index = index, .xa_node = XAS_RESTART}

static int rcu_depth, retries, flicker;
static const void *bad_address;
static struct xa_node *flickering_node;
static void rcu_read_lock(void) { assert(!rcu_depth); ++rcu_depth; }
static void rcu_read_unlock(void)
{
    assert(rcu_depth == 1);
    --rcu_depth;
    if (flicker) flickering_node->marks[0][0] ^= 6;
}
static long copy_from_kernel_nofault(void *dst, const void *src, size_t bytes)
{
    if (src == bad_address) return -EFAULT;
    memcpy(dst, src, bytes);
    return 0;
}
static bool xa_is_node(const void *entry)
{
    return ((uintptr_t)entry & 3) == 2 && (uintptr_t)entry > 4096;
}
static struct xa_node *xa_to_node(const void *entry)
{
    return (struct xa_node *)((uintptr_t)entry - 2);
}
static void *xa_mk_node(struct xa_node *node) { return (void *)((uintptr_t)node + 2); }
static bool xa_is_retry(const void *entry) { return entry == XA_RETRY_ENTRY; }
static bool xa_is_sibling(const void *entry)
{
    return ((uintptr_t)entry & 3) == 2 && (uintptr_t)entry < 256;
}
static unsigned int xa_to_sibling(const void *entry) { return (uintptr_t)entry >> 2; }
static bool xas_top(struct xa_node *node) { return (uintptr_t)node <= 3; }
static int xas_error(const struct xa_state *xas) { (void)xas; return 0; }
static void *xa_head(const struct xarray *xa) { assert(rcu_depth); return xa->xa_head; }
static bool xa_marked(const struct xarray *xa, xa_mark_t mark)
{
    (void)xa; (void)mark;
    return true;
}
static struct xa_node *xa_parent(const struct xarray *xa, const struct xa_node *node)
{
    (void)xa;
    assert(rcu_depth);
    return node->parent;
}
static void *xa_entry(const struct xarray *xa, const struct xa_node *node, unsigned int offset)
{
    (void)xa;
    assert(rcu_depth && offset < 64);
    if (retries) { --retries; return XA_RETRY_ENTRY; }
    return node->slots[offset];
}
static unsigned int xas_find_chunk(struct xa_state *xas, bool advance, xa_mark_t mark)
{
    unsigned int offset = xas->xa_offset + advance;
    if (offset >= 64) return 64;
    unsigned long data = xas->xa_node->marks[mark][0] & (~0UL << offset);
    return data ? (unsigned int)__builtin_ctzl(data) : 64;
}
static bool xas_retry(struct xa_state *xas, const void *entry)
{
    if (entry != XA_RETRY_ENTRY) return false;
    xas->xa_node = XAS_RESTART;
    return true;
}
static void cpu_relax(void) {}

#include "../../lsdriver/sgame_simulation/kgsl_idr.h"

static void attach(struct idr *idr, struct xa_node *node, unsigned int shift)
{
    memset(node, 0, sizeof(*node));
    node->array = &idr->idr_rt;
    node->shift = shift;
    node->count = 1;
    idr->idr_rt.xa_flags = IDR_RT_MARKER;
    idr->idr_rt.xa_head = xa_mk_node(node);
}

int main(void)
{
    struct idr idr = {.idr_rt.xa_flags = IDR_RT_MARKER};
    struct xa_node root, children[64];
    u32 result;
    assert(!kgsl_validate_live_idr(&idr));
    assert(!kgsl_find_stable_lowest_free(&idr, 1, &result) && result == 1);
    attach(&idr, &root, 0);
    root.marks[0][0] = 1UL << 4;
    /* NULL at ID 3 has its free mark cleared: it is allocated, not a hole. */
    assert(!kgsl_find_stable_lowest_free(&idr, 1, &result) && result == 4);
    assert(!kgsl_validate_live_idr(&idr));
    root.marks[0][0] = 0;
    assert(!kgsl_find_stable_lowest_free(&idr, 1, &result) && result == 64);
    root.marks[0][0] = 1UL << 1;
    root.slots[1] = (void *)0x1000; /* allocator changed a slot before its mark */
    assert(kgsl_find_stable_lowest_free(&idr, 1, &result) == -EAGAIN);
    root.slots[1] = NULL;
    retries = 2;
    assert(!kgsl_find_stable_lowest_free(&idr, 1, &result) && result == 1);
    retries = 100;
    assert(kgsl_find_stable_lowest_free(&idr, 1, &result) == -EAGAIN);
    retries = 0;
    flicker = 1;
    flickering_node = &root;
    assert(kgsl_find_stable_lowest_free(&idr, 1, &result) == -EAGAIN);
    flicker = 0;
    bad_address = &idr.idr_base;
    assert(kgsl_validate_live_idr(&idr) == -EFAULT);
    bad_address = NULL;
    root.nr_values = 2;
    assert(kgsl_validate_live_idr(&idr) == -EOPNOTSUPP);
    root.nr_values = 0;

    /* Two-level trees, checked against a flat occupied-ID model. Include
     * non-NULL allocations and NULL allocations with identical occupancy. */
    uint32_t rng = 0x12345678;
    bool occupied[4096];
    for (unsigned int round = 0; round < 128; ++round) {
        attach(&idr, &root, 6);
        root.count = 64;
        for (unsigned int i = 0; i < 64; ++i) {
            memset(&children[i], 0, sizeof(children[i]));
            children[i].parent = &root;
            children[i].array = &idr.idr_rt;
            children[i].offset = i;
            root.slots[i] = xa_mk_node(&children[i]);
            for (unsigned int j = 0; j < 64; ++j) {
                rng = rng * 1664525U + 1013904223U;
                bool busy = (rng >> 28) != 0;
                occupied[i * 64 + j] = busy;
                if (!busy) children[i].marks[0][0] |= 1UL << j;
                else if (rng & 1) children[i].slots[j] = (void *)0x1000;
            }
            if (children[i].marks[0][0]) root.marks[0][0] |= 1UL << i;
        }
        assert(!kgsl_validate_live_idr(&idr));
        for (unsigned int sample = 0; sample < 16; ++sample) {
            u32 min = 1 + ((round * 67 + sample * 257) % 4095);
            u32 expected = min;
            while (expected < 4096 && occupied[expected]) ++expected;
            assert(!kgsl_find_stable_lowest_free(&idr, min, &result));
            assert(result == expected);
        }
    }
    assert(!rcu_depth);
    puts("simulation IDR: NULL allocations, tree boundaries, retries, races and 2048 model comparisons passed");
}
