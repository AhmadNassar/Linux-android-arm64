#!/usr/bin/env bash
# Build a pinned, prepared GKI workspace using the author's module functions.
set -euo pipefail
WORK_ROOT="$(realpath -m "${1:?work root}")"
PREPARED_SRC="$(realpath "${2:?committed source export}")"
PROFILE="${3:?profile: a13-515, a15-66 or a17-618}"
ADAPTER_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
case "$PROFILE" in
    a13-515) SOURCE_KEY=kernel_5_15; KERNEL_NAME=5.15-Android13 ;;
    a15-66) SOURCE_KEY=kernel_6_6; KERNEL_NAME=6.6-Android15 ;;
    a17-618) SOURCE_KEY=kernel_6_18; KERNEL_NAME=6.18-Android17 ;;
    *) exit 2 ;;
esac
KERNEL_DIR="$WORK_ROOT/kernels/$KERNEL_NAME"
mkdir -p "$WORK_ROOT/tools" "$WORK_ROOT/artifacts/4k"
export PATH="$WORK_ROOT/tools:$PATH" GIT_TERMINAL_PROMPT=0
mapfile -t profile < <(python3 - "$ADAPTER_DIR/kernel-baselines.json" "$SOURCE_KEY" "$KERNEL_DIR/common" "$PREPARED_SRC" <<'PY'
import hashlib, json, pathlib, subprocess, sys
baseline = json.loads(pathlib.Path(sys.argv[1]).read_text())[sys.argv[2]]
common, source = map(pathlib.Path, sys.argv[3:])
actual = subprocess.check_output(['git', '-C', str(common), 'rev-parse', 'HEAD'], text=True).strip()
if actual != baseline['common_commit']:
    raise SystemExit('Wrong kernel source: ' + actual)
for name, expected in baseline['reference_sha256'].items():
    path = common / name
    content = str(path.readlink()).encode() if path.is_symlink() else path.read_bytes()
    if hashlib.sha256(content).hexdigest() != expected:
        raise SystemExit('Kernel reference differs: ' + name)
record = json.loads((source / 'sgame-source.json').read_text())
for name, expected in record['files_sha256'].items():
    if hashlib.sha256((source / name).read_bytes()).hexdigest() != expected:
        raise SystemExit('Source export differs: ' + name)
for value in (baseline['version'], baseline['android'], baseline['kmi_generation'], baseline['clang'], int(record['layer']['read_mode']['name'] == 'nc')):
    print(value)
PY
)
[[ "${#profile[@]}" == 5 ]] || exit 1
VERSION="${profile[0]}"
ANDROID="${profile[1]}"
GENERATION="${profile[2]}"
CLANG_PATH="$KERNEL_DIR/prebuilts/clang/host/linux-x86/clang-${profile[3]}"
export SGAME_READ_NC="${profile[4]}"
python3 "$ADAPTER_DIR/prepare_host_tools.py" "$WORK_ROOT"
cat > "$WORK_ROOT/tools/kleaf-bash" <<'SH'
#!/bin/sh
export PATH="$(dirname "$0"):${PATH:-/usr/bin:/bin}"
exec /bin/bash "$@"
SH
chmod 755 "$WORK_ROOT/tools/kleaf-bash"
cd "$KERNEL_DIR"
repo manifest -r -o "$WORK_ROOT/artifacts/4k/kernel-manifest.xml"
tools/bazel build --jobs="${JOBS:-16}" --shell_executable="$WORK_ROOT/tools/kleaf-bash" \
    //common:kernel_aarch64 //common:kernel_aarch64_modules_prepare
python3 - "$PREPARED_SRC/build_all.sh" "$WORK_ROOT/author-functions.sh" <<'PY'
import pathlib, sys
text = pathlib.Path(sys.argv[1]).read_text()
entry = '\nmain "$@"'
if text.count(entry) != 1:
    raise SystemExit('Upstream build entry changed; review adapter')
pathlib.Path(sys.argv[2]).write_text(text.replace(entry, '\n# main called by sgame adapter'))
PY
source "$WORK_ROOT/author-functions.sh"
KERNELS_ROOT="$WORK_ROOT/kernels"
DRIVER_SRC="$PREPARED_SRC/lsdriver"
readonly STRIP_CHOICE=n
extra=CLANG_TRIPLE=aarch64-linux-gnu-
[[ "$PROFILE" != a13-515 ]] || extra="LLVM_TOOLCHAIN_PATH=$CLANG_PATH"
build_kernel "$KERNEL_NAME" "$CLANG_PATH" aarch64-linux-gnu- "$extra"
ARTIFACTS="$WORK_ROOT/artifacts/4k"
cp "$DRIVER_SRC/$KERNEL_NAME.ko" "$ARTIFACTS/lsdriver-swap-$VERSION-A$ANDROID-$GENERATION-4k.ko"
cp "$PREPARED_SRC/sgame-source.json" "$ARTIFACTS/driver-source.json"
cp "$WORK_ROOT/host-tools/packages.json" "$ARTIFACTS/host-tools.json" 2>/dev/null || true
python3 "$ADAPTER_DIR/verify_gki_module.py" "$ARTIFACTS" "$KERNEL_DIR" "$CLANG_PATH" kernel_aarch64 4k "$VERSION" "$GENERATION" "$ANDROID"
python3 - "$PREPARED_SRC" <<'PY'
import hashlib, json, pathlib, sys
root = pathlib.Path(sys.argv[1])
for name, expected in json.loads((root / 'sgame-source.json').read_text())['files_sha256'].items():
    if hashlib.sha256((root / name).read_bytes()).hexdigest() != expected:
        raise SystemExit('Build changed committed source: ' + name)
print('Committed source unchanged after build')
PY
