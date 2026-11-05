#ifndef SGAME_SIM_HOST_TEST
#include <linux/errno.h>
#include <linux/slab.h>
#include <linux/string.h>
#endif

#include "internal.h"

int sgame_sim_dispatch(void *buffer, size_t buffer_bytes,
                       const void *extra, size_t extra_bytes)
{
    struct sgame_sim_packet local;
    uint32_t bytes;
    uint32_t *excluded = NULL;
    int ret;

    if (buffer_bytes < sizeof(local.header))
        return -EMSGSIZE;
    memcpy(&local.header, buffer, sizeof(local.header));
    if (local.header.abi_version != SGAME_SIM_ABI_VERSION)
        return -EPROTONOSUPPORT;
    if (local.header.reserved)
        return -EINVAL;
    bytes = sgame_sim_payload_bytes(local.header.command);
    if (!bytes)
        return -ENOTTY;
    if (local.header.payload_bytes != bytes ||
        buffer_bytes - sizeof(local.header) < bytes)
        return -EMSGSIZE;

    /* Never run a handler against mutable shared inputs. No handler dereferences
     * client userspace: the dispatch kthread has no client mm. */
    memset(&local.payload, 0, sizeof(local.payload));
    memcpy(&local.payload, (char *)buffer + sizeof(local.header), bytes);
    if (local.header.command == DFM_IOCTL_KGSL_QUERY) {
        const struct dfm_kgsl_query_req *kgsl = &local.payload.kgsl;
        size_t excluded_bytes;

        if (kgsl->excluded_ids ||
            kgsl->excluded_count > DFM_KGSL_QUERY_MAX_EXCLUDED_IDS)
            return -EINVAL;
        excluded_bytes = kgsl->excluded_count * sizeof(*excluded);
        if (excluded_bytes > extra_bytes)
            return -EMSGSIZE;
        if (excluded_bytes) {
            excluded = kmemdup(extra, excluded_bytes, GFP_KERNEL);
            if (!excluded)
                return -ENOMEM;
        }
    }

    switch (local.header.command) {
    case SGAME_SIM_COMMAND_CAPS:
        local.payload.caps = (struct sgame_sim_caps) {
            .abi_version = SGAME_SIM_ABI_VERSION,
            .version_code = SGAME_SIM_VERSION_CODE,
            .capabilities = SGAME_SIM_CAP_ALL,
            .packet_bytes = sizeof(local),
            .max_excluded_ids = DFM_KGSL_QUERY_MAX_EXCLUDED_IDS,
            .extra_bytes = SGAME_SIM_EXTRA_BYTES,
            .read_mode = SGAME_READ_NC ? SGAME_SIM_READ_NC : SGAME_SIM_READ_CACHED,
        };
        ret = 0;
        break;
#define SGAME_SIM_DISPATCH(command, name, type) \
    case command: ret = sgame_sim_mem_##name(&local.payload.name); break;
        SGAME_SIM_MEMORY_COMMANDS(SGAME_SIM_DISPATCH)
#undef SGAME_SIM_DISPATCH
    case DFM_IOCTL_KGSL_QUERY:
        ret = sgame_sim_kgsl_query(&local.payload.kgsl, excluded);
        break;
    default:
        ret = -ENOTTY;
        break;
    }
    kfree(excluded);
    /* KGSL returns diagnostic stage information even on infrastructure errors.
     * Other failed queries have no defined output and must not retain old data. */
    if (ret && local.header.command != DFM_IOCTL_KGSL_QUERY)
        memset(&local.payload, 0, bytes);
    memcpy((char *)buffer + sizeof(local.header), &local.payload, bytes);
    return ret;
}
