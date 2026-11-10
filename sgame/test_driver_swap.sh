#!/usr/bin/env bash
# Production Swap/connection code with versioned kernel API substitutes.
set -euo pipefail
cd "$(dirname "$0")/.."
mkdir -p build/tests
compiler="${CC:-cc}"
flags=(-std=gnu11 -fsanitize=address,undefined -g)
for variant in 510-4k 61-4k 612-4k 612-16k; do
    reader_flags=()
    session_flags=()
    if [[ "$variant" == 510-* ]]; then
        reader_flags+=(-DSGAME_SWAP_LEGACY=1)
    fi
    if [[ "$variant" == 612-* ]]; then
        reader_flags+=(-DSGAME_SWAP_MODERN=1)
        session_flags+=(-DSGAME_SESSION_MODERN_GUP=1)
    fi
    if [[ "$variant" == *-16k ]]; then
        reader_flags+=(-DPAGE_SHIFT=14)
        session_flags+=(-DPAGE_SIZE=16384)
    fi
    for read_mode in 0 1; do
        "$compiler" "${flags[@]}" "${reader_flags[@]}" -DSGAME_READ_NC="$read_mode" \
            sgame/tests/driver_swap_test.c -o "build/tests/swap-$variant-$read_mode"
        "build/tests/swap-$variant-$read_mode"
    done
    "$compiler" "${flags[@]}" "${session_flags[@]}" -pthread sgame/tests/driver_swap_session_test.c -o "build/tests/session-$variant"
    "build/tests/session-$variant"
done
"$compiler" "${flags[@]}" sgame/tests/driver_swap_init_test.c -o build/tests/driver_swap_init_test
build/tests/driver_swap_init_test
