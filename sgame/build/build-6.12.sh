#!/usr/bin/env bash
# Pinned Android16 KMI 5/6 environments, using the author's module builder.
set -euo pipefail
WORK_ROOT="$(realpath -m "${1:?usage: build-6.12.sh WORK_ROOT PREPARED_DRIVER a16-5|a16-6 [4k|16k|all|sync]}")"
PREPARED_SRC="$(realpath "${2:?prepared driver source required}")"
PROFILE="${3:?Android16 KMI profile required: a16-5 or a16-6}"
VARIANT="${4:-4k}"
SGAME_READ_NC=$(python3 - "$PREPARED_SRC/sgame-source.json" <<'PY_MODE'
import json,sys
print(int(json.load(open(sys.argv[1]))['layer']['read_mode']['name']=='nc'))
PY_MODE
)
export SGAME_READ_NC
case "$PROFILE" in
    a16-5) KERNEL_VERSION=6.12.23; KMI_GENERATION=5; SOURCE_KEY=kernel_6_12 ;;
    a16-6) KERNEL_VERSION=6.12.58; KMI_GENERATION=6; SOURCE_KEY=kernel_6_12_kmi6 ;;
    *) exit 2 ;;
esac
case "$VARIANT" in all|4k|16k|sync) ;; *) exit 2 ;; esac
ADAPTER_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
KERNEL_DIR="$WORK_ROOT/kernels/6.12-Android16"
mkdir -p "$KERNEL_DIR" "$WORK_ROOT/tools" "$WORK_ROOT/manifest"
export PATH="$WORK_ROOT/tools:$PATH" GIT_TERMINAL_PROMPT=0
export GIT_CONFIG_GLOBAL="$WORK_ROOT/gitconfig"
export REPO_URL="${REPO_URL:-https://gerrit-googlesource.proxy.ustclug.org/git-repo}"
AOSP_MIRROR="${AOSP_MIRROR:-https://mirrors.ustc.edu.cn/aosp}"
COMMON_MIRROR="${COMMON_MIRROR:-https://mirrors.bfsu.edu.cn/git/AOSP}"
git config --global user.name 'SGame build'
git config --global user.email 'sgame-build@localhost'
git config --global http.version HTTP/1.1
python3 "$ADAPTER_DIR/prepare_host_tools.py" "$WORK_ROOT"
cat > "$WORK_ROOT/tools/kleaf-bash" <<'SH'
#!/bin/sh
export PATH="$(dirname "$0"):${PATH:-/usr/bin:/bin}"
exec /bin/bash "$@"
SH
chmod 755 "$WORK_ROOT/tools/kleaf-bash"
if ! command -v repo >/dev/null; then
    curl --fail --location --retry 3 https://mirrors.tuna.tsinghua.edu.cn/git/git-repo -o "$WORK_ROOT/tools/repo"
    chmod 755 "$WORK_ROOT/tools/repo"
fi
# The checked-in manifest pins every dependency to the contemporaneous Google
# superproject, so current toolchain branches cannot change this build.
python3 - "$ADAPTER_DIR/$KERNEL_VERSION-manifest.xml" "$WORK_ROOT/manifest/default.xml" "$AOSP_MIRROR" "$COMMON_MIRROR" <<'PY'
import pathlib, sys, xml.etree.ElementTree as ET
source, dest, aosp, common = sys.argv[1:]
tree = ET.parse(source)
tree.find('remote').set('fetch', aosp)
ET.SubElement(tree.getroot(), 'remote', name='sgame-common', fetch=common)
tree.find("project[@name='kernel/common']").set('remote', 'sgame-common')
tree.write(dest, encoding='unicode', xml_declaration=True)
PY
if [[ ! -d "$WORK_ROOT/manifest/.git" ]]; then
    git -C "$WORK_ROOT/manifest" init -b main
fi
git -C "$WORK_ROOT/manifest" add default.xml
if ! git -C "$WORK_ROOT/manifest" diff --cached --quiet; then
    git -C "$WORK_ROOT/manifest" commit -m "Pin Android16 $KERNEL_VERSION KMI $KMI_GENERATION build dependencies"
fi
cd "$KERNEL_DIR"
if [[ ! -f .repo/repo/main.py ]]; then
    mkdir -p .repo
    git clone --depth=1 --single-branch --branch=stable "$REPO_URL" .repo/repo
fi
if [[ ! -f .repo/manifests.git/HEAD ]]; then
    reference=()
    if [[ -n "${REPO_REFERENCE:-}" ]]; then
        reference=(--reference "$REPO_REFERENCE")
    fi
    repo init -u "$WORK_ROOT/manifest" -b main --depth=1 --no-clone-bundle --repo-rev=stable "${reference[@]}"
fi
if [[ "${SYNC_LOCAL_ONLY:-0}" == 1 ]]; then
    repo sync -l -j"${SYNC_JOBS:-4}" --fail-fast
else
    repo sync -c -j"${SYNC_JOBS:-4}" --no-clone-bundle --fail-fast
fi
[[ "$VARIANT" != sync ]] || exit 0
python3 - "$KERNEL_DIR/common" "$PREPARED_SRC" "$ADAPTER_DIR/kernel-baselines.json" "$SOURCE_KEY" <<'PY'
import hashlib, json, pathlib, re, subprocess, sys
kernel, driver, source_path = map(pathlib.Path, sys.argv[1:4])
expected = json.loads(source_path.read_text())[sys.argv[4]]
actual = subprocess.check_output(['git', '-C', str(kernel), 'rev-parse', 'HEAD'], text=True).strip()
values = dict(re.findall(r'^(VERSION|PATCHLEVEL|SUBLEVEL) = (\d+)$', (kernel / 'Makefile').read_text(), re.M))
version = '.'.join(values[k] for k in ('VERSION', 'PATCHLEVEL', 'SUBLEVEL'))
constants = (kernel / 'build.config.constants').read_text()
generation = re.search(r'^KMI_GENERATION=(\d+)$', constants, re.M).group(1)
if actual != expected['common_commit'] or version != expected['version'] or int(generation) != expected['kmi_generation']:
    raise SystemExit(f'Wrong kernel: {actual}, {version}, KMI {generation}')
source = json.loads((driver / 'sgame-source.json').read_text())
layer = source['layer']
files = source['files_sha256']
for name, expected in files.items():
    if hashlib.sha256((driver / name).read_bytes()).hexdigest() != expected:
        raise SystemExit(f'Prepared source changed: {name}')
print(f'Verified kernel {version}, KMI {generation} and prepared swap {layer["version"]}', flush=True)
PY
CLANG_PATH="$KERNEL_DIR/prebuilts/clang/host/linux-x86/clang-r536225"
for page_size in 4k 16k; do
    [[ "$VARIANT" == all || "$VARIANT" == "$page_size" ]] || continue
    target=kernel_aarch64
    [[ "$page_size" != 16k ]] || target=kernel_aarch64_16k
    artifacts="$WORK_ROOT/artifacts/$page_size"
    mkdir -p "$artifacts"
    repo manifest -r -o "$artifacts/kernel-manifest.xml"
    tools/bazel build --jobs="${JOBS:-16}" --shell_executable="$WORK_ROOT/tools/kleaf-bash" \
        "//common:$target" "//common:${target}_modules_prepare"
    # Adapt only the generated function copy's output target; the original
    # author's source, CRC handling, toolchain flags and public headers stay intact.
    python3 - "$PREPARED_SRC/build_all.sh" "$WORK_ROOT/author-functions-$page_size.sh" "$target" <<'PY'
import pathlib, sys
source = pathlib.Path(sys.argv[1]).read_text()
entry = '\nmain "$@"'
if source.count(entry) != 1:
    raise SystemExit('Upstream build entry changed; review adapter')
source = source.replace(entry, '\n# main called by sgame adapter')
pathlib.Path(sys.argv[2]).write_text(source.replace('kernel_aarch64', sys.argv[3]))
PY
    # A subshell isolates the author's readonly variables across both variants.
    (
        source "$WORK_ROOT/author-functions-$page_size.sh"
        KERNELS_ROOT="$WORK_ROOT/kernels"
        DRIVER_SRC="$PREPARED_SRC/lsdriver"
        readonly STRIP_CHOICE=n
        build_kernel '6.12-Android16' "$CLANG_PATH" 'aarch64-linux-gnu-' 'CLANG_TRIPLE=aarch64-linux-gnu-'
    )
    cp "$PREPARED_SRC/lsdriver/6.12-Android16.ko" "$artifacts/lsdriver-swap-$KERNEL_VERSION-A16-$KMI_GENERATION-$page_size.ko"
    cp "$PREPARED_SRC/sgame-source.json" "$artifacts/driver-source.json"
    cp "$WORK_ROOT/host-tools/packages.json" "$artifacts/host-tools.json" 2>/dev/null || true
    python3 "$ADAPTER_DIR/verify_gki_module.py" "$artifacts" "$KERNEL_DIR" "$CLANG_PATH" "$target" "$page_size" "$KERNEL_VERSION" "$KMI_GENERATION"
done
