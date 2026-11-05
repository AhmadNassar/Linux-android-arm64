#ifndef SGAME_SIM_INTERNAL_H
#define SGAME_SIM_INTERNAL_H

#include "abi.h"

#ifdef __KERNEL__
#include <linux/stddef.h>
#else
#include <stddef.h>
#endif

#define SGAME_SIM_DECLARE(command, name, type) \
    long sgame_sim_mem_##name(struct type *req);
SGAME_SIM_MEMORY_COMMANDS(SGAME_SIM_DECLARE)
#undef SGAME_SIM_DECLARE

long sgame_sim_kgsl_query(struct dfm_kgsl_query_req *req,
                         const uint32_t *excluded);
unsigned long sgame_sim_lookup_name(const char *name);
int sgame_sim_symbol_span(unsigned long addr, unsigned long *size,
                          unsigned long *offset);

/* Called while the existing session mutex owns the mapped request. */
int sgame_sim_dispatch(void *buffer, size_t buffer_bytes,
                       const void *extra, size_t extra_bytes);

#endif
