#ifndef SGAME_SIM_SYMBOL_SPAN_H
#define SGAME_SIM_SYMBOL_SPAN_H

/* sprint_symbol emits name+offset/size [module]. Parse its hexadecimal fields
 * to retain enkernel's symbol-bounded decoder without importing the unexported
 * kallsyms_lookup_size_offset or making a CFI-unsafe indirect call to it. */
static inline int sgame_sim_parse_hex(const char **cursor, unsigned long *value)
{
    const char *p = *cursor;
    unsigned long n = 0;
    unsigned int digits = 0;

    if (p[0] == '0' && p[1] == 'x')
        p += 2;
    for (;;) {
        unsigned int digit;

        if (*p >= '0' && *p <= '9') digit = *p - '0';
        else if (*p >= 'a' && *p <= 'f') digit = *p - 'a' + 10;
        else if (*p >= 'A' && *p <= 'F') digit = *p - 'A' + 10;
        else break;
        if (++digits > sizeof(n) * 2U)
            return 0;
        n = (n << 4) | digit;
        ++p;
    }
    if (!digits)
        return 0;
    *value = n;
    *cursor = p;
    return 1;
}

static inline int sgame_sim_parse_symbol_span(const char *text,
                                             unsigned long *size,
                                             unsigned long *offset)
{
    const char *p = text;
    unsigned long parsed_size, parsed_offset;

    while (*p && *p != '+')
        ++p;
    if (*p++ != '+' || !sgame_sim_parse_hex(&p, &parsed_offset) ||
        *p++ != '/' || !sgame_sim_parse_hex(&p, &parsed_size) ||
        (*p && *p != ' ') || !parsed_size || parsed_offset >= parsed_size)
        return 0;
    *size = parsed_size;
    *offset = parsed_offset;
    return 1;
}

#endif
