/* Read-only IDR allocation snapshot, adapted from the pinned enkernel source. */
#ifndef SGAME_SIM_KGSL_IDR_H
#define SGAME_SIM_KGSL_IDR_H

#include "xarray_walk.h"

static int kgsl_validate_live_idr(struct idr *idr)
{
    unsigned int base;
    unsigned int next;
    gfp_t flags;
    void *head;
    struct xa_node *node;
    struct xa_node *parent;
    struct xarray *array;
    u8 shift;
    u8 offset;
    u8 count;
    u8 nr_values;
    int ret = 0;

    if (copy_from_kernel_nofault(&base, &idr->idr_base, sizeof(base)) ||
        copy_from_kernel_nofault(&next, &idr->idr_next, sizeof(next)) ||
        copy_from_kernel_nofault(&flags, &idr->idr_rt.xa_flags,
                                 sizeof(flags)))
        return -EFAULT;
    if (base != 0U || next != 0U ||
        (flags & IDR_RT_MARKER) != IDR_RT_MARKER)
        return -EOPNOTSUPP;

    rcu_read_lock();
    if (copy_from_kernel_nofault(&head, &idr->idr_rt.xa_head,
                                 sizeof(head))) {
        ret = -EFAULT;
        goto out_rcu;
    }
    if (!head)
        goto out_rcu;
    if (xa_is_retry(head)) {
        ret = -EAGAIN;
        goto out_rcu;
    }
    if (!xa_is_node(head)) {
        /* syncsource allocation starts at one, so a non-empty valid tree
         * cannot use XArray's direct index-zero head representation. */
        ret = -EOPNOTSUPP;
        goto out_rcu;
    }

    node = xa_to_node(head);
    if (copy_from_kernel_nofault(&parent, &node->parent, sizeof(parent)) ||
        copy_from_kernel_nofault(&array, &node->array, sizeof(array)) ||
        copy_from_kernel_nofault(&shift, &node->shift, sizeof(shift)) ||
        copy_from_kernel_nofault(&offset, &node->offset, sizeof(offset)) ||
        copy_from_kernel_nofault(&count, &node->count, sizeof(count)) ||
        copy_from_kernel_nofault(&nr_values, &node->nr_values,
                                 sizeof(nr_values))) {
        ret = -EFAULT;
        goto out_rcu;
    }
    if (parent || array != &idr->idr_rt || offset != 0U || count == 0U ||
        count > XA_CHUNK_SIZE || nr_values > count ||
        shift >= BITS_PER_LONG || (shift % XA_CHUNK_SHIFT) != 0U)
        ret = -EOPNOTSUPP;

out_rcu:
    rcu_read_unlock();
    return ret;
}

static int kgsl_find_lowest_free_once(struct idr *idr, u32 min_id,
                                      u32 *out_id)
{
    XA_STATE(xas, &idr->idr_rt, min_id);
    void *entry = NULL;
    unsigned int attempt;
    int ret = 0;

    rcu_read_lock();
    for (attempt = 0; attempt < KGSL_SYNCSOURCE_XAS_RETRIES; attempt++) {
        entry = sgame_sim_xas_find_marked(&xas, (unsigned long)INT_MAX,
                                XA_FREE_MARK);
        if (!xas_retry(&xas, entry))
            break;
    }

    if (attempt == KGSL_SYNCSOURCE_XAS_RETRIES) {
        ret = -EAGAIN;
    } else if (xas_error(&xas)) {
        ret = xas_error(&xas);
    } else if (xas.xa_node == XAS_RESTART) {
        ret = -ENOSPC;
    } else if (entry) {
        /* A free-mark walk should stop on a NULL slot.  A non-NULL entry
         * means a concurrent allocator published the object before clearing
         * XA_FREE_MARK; retry the whole snapshot rather than guessing. */
        ret = -EAGAIN;
    } else if (xas.xa_index < min_id ||
               xas.xa_index > (unsigned long)INT_MAX) {
        ret = -EAGAIN;
    } else {
        *out_id = (u32)xas.xa_index;
    }
    rcu_read_unlock();
    return ret;
}

static int kgsl_find_stable_lowest_free(struct idr *idr, u32 min_id,
                                        u32 *out_id)
{
    unsigned int attempt;

    for (attempt = 0; attempt < KGSL_SYNCSOURCE_STABLE_TRIES; attempt++) {
        u32 first;
        u32 second;
        int ret;

        ret = kgsl_find_lowest_free_once(idr, min_id, &first);
        if (ret)
            return ret;
        ret = kgsl_find_lowest_free_once(idr, min_id, &second);
        if (ret)
            return ret;
        if (first == second) {
            *out_id = first;
            return 0;
        }
        cpu_relax();
    }

    return -EAGAIN;
}

#endif
