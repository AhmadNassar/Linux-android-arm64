#include <linux/kallsyms.h>
#include "internal.h"
#include "symbol_span.h"

int sgame_sim_symbol_span(unsigned long addr, unsigned long *size,
                          unsigned long *offset)
{
    char text[KSYM_SYMBOL_LEN];

    sprint_symbol(text, addr);
    return sgame_sim_parse_symbol_span(text, size, offset);
}
