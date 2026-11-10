#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/.."
OUT_DIR="build/tests/driver-simulation"
mkdir -p "$OUT_DIR"
SIM_CC="${CC:-clang}"
SIM_CXX="${CXX:-clang++}"
SIM_FLAGS=(-Wall -Wextra -Werror -g -fsanitize=address,undefined -fno-omit-frame-pointer)
"$SIM_CC" -std=gnu11 "${SIM_FLAGS[@]}" sgame/tests/driver_simulation_test.c -o "$OUT_DIR/dispatch-test"
"$OUT_DIR/dispatch-test"
"$SIM_CC" -std=gnu11 "${SIM_FLAGS[@]}" sgame/tests/driver_simulation_xarray_test.c -o "$OUT_DIR/xarray-test"
"$OUT_DIR/xarray-test"
# Empty include files let the fixture supply kernel API types and operations;
# the source being exercised is the actual environment.c compiled into .ko.
python3 - "$OUT_DIR/kernel-stubs" <<'PY'
from pathlib import Path
import sys
root = Path(sys.argv[1])
for name in ('fdtable', 'file', 'fs', 'kdev_t', 'pid', 'sched/signal', 'stat', 'user_namespace'):
    path = root / ('linux/' + name + '.h')
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text('/* kernel API supplied by environment test fixture */\n')
PY
"$SIM_CC" -std=gnu11 "${SIM_FLAGS[@]}" -Wno-sign-compare -I"$OUT_DIR/kernel-stubs" \
    sgame/tests/driver_simulation_environment_test.c -o "$OUT_DIR/environment-test"
"$OUT_DIR/environment-test"
"$SIM_CC" -std=gnu11 "${SIM_FLAGS[@]}" -Wno-sign-compare -I"$OUT_DIR/kernel-stubs" \
    -DSGAME_SIM_IDS_510=1 sgame/tests/driver_simulation_environment_test.c -o "$OUT_DIR/environment-510-test"
"$OUT_DIR/environment-510-test"
"$SIM_CC" -std=gnu11 "${SIM_FLAGS[@]}" sgame/tests/driver_vma_name_510_test.c -o "$OUT_DIR/vma-name-510-test"
"$OUT_DIR/vma-name-510-test"
"$SIM_CC" -std=gnu11 "${SIM_FLAGS[@]}" -DSGAME_SIM_TEST_LIBRARY \
    -c sgame/tests/driver_simulation_test.c -o "$OUT_DIR/fixture.o"
"$SIM_CXX" -std=c++20 "${SIM_FLAGS[@]}" -pthread -Iandroid/jni \
    sgame/tests/driver_simulation_client_test.cpp "$OUT_DIR/fixture.o" -o "$OUT_DIR/client-test"
"$OUT_DIR/client-test"
SIM_NDK="${ANDROID_NDK:-/home/flyingdonkey/Android/android-ndk-r27c}"
"$SIM_NDK/toolchains/llvm/prebuilt/linux-x86_64/bin/aarch64-linux-android30-clang++" \
    -std=c++20 -Wall -Wextra -Werror -I android/jni -c \
    sgame/tests/driver_simulation_abi_compile.cpp -o "$OUT_DIR/android-abi.o"
printf 'Android ARM64: real driver header + all simulation client methods compiled\n'
