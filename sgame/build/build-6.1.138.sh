#!/usr/bin/env bash
# Google GKI environment + author's build_kernel(), without changing originals.
set -euo pipefail
WORK_ROOT="$(realpath -m "${1:?usage: build-6.1.138.sh WORK_ROOT PREPARED_DRIVER}")"
PREPARED_SRC="$(realpath "${2:?prepared driver source required}")"
BUILD_JOBS="${JOBS:-16}"
SGAME_READ_NC=$(python3 - "$PREPARED_SRC/sgame-source.json" <<'PY_MODE'
import json,sys
print(int(json.load(open(sys.argv[1]))['layer']['read_mode']['name']=='nc'))
PY_MODE
)
export SGAME_READ_NC
ADAPTER_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
# Verify the prepared extension before a potentially long kernel build.
python3 - "$PREPARED_SRC" <<'PY'
import hashlib, json, pathlib, sys
root = pathlib.Path(sys.argv[1])
source = json.loads((root / "sgame-source.json").read_text())
simulation = source["layer"]["simulation"]
files = source['files_sha256']
for name, expected in files.items():
    actual = hashlib.sha256((root / name).read_bytes()).hexdigest()
    if actual != expected:
        raise SystemExit(f"prepared source changed: {name}; prepare a fresh source record")
print(f"Prepared simulation {simulation['version']} from {simulation['commit']}", flush=True)
PY
KERNEL_DIR="$WORK_ROOT/kernels/6.1-Android14"
mkdir -p "$KERNEL_DIR" "$WORK_ROOT/tools" "$WORK_ROOT/artifacts"
export PATH="$WORK_ROOT/tools:$PATH"
export GIT_TERMINAL_PROMPT=0
export REPO_URL="${REPO_URL:-https://gerrit-googlesource.proxy.ustclug.org/git-repo}"
AOSP_MIRROR="${AOSP_MIRROR:-https://mirrors.ustc.edu.cn/aosp}"
COMMON_MIRROR="${COMMON_MIRROR:-https://mirrors.bfsu.edu.cn/git/AOSP}"
export GIT_CONFIG_GLOBAL="$WORK_ROOT/gitconfig"
git config --global user.name "SGame build"
git config --global user.email "sgame-build@localhost"
git config --global http.version HTTP/1.1
python3 "$ADAPTER_DIR/prepare_host_tools.py" "$WORK_ROOT"
if [[ -f "$WORK_ROOT/host-tools/packages.json" ]]; then
    cp "$WORK_ROOT/host-tools/packages.json" "$WORK_ROOT/artifacts/host-tools.json"
fi
# Kleaf's host-tool actions ignore --action_env and otherwise use /usr/bin.
# Make the private host rsync visible without changing upstream Bazel rules.
cat > "$WORK_ROOT/tools/kleaf-bash" <<'SH'
#!/bin/sh
export PATH="$(dirname "$0"):${PATH:-/usr/bin:/bin}"
exec /bin/bash "$@"
SH
chmod 755 "$WORK_ROOT/tools/kleaf-bash"

if ! command -v repo >/dev/null; then
    curl --fail --location --retry 3 https://mirrors.tuna.tsinghua.edu.cn/git/git-repo \
        -o "$WORK_ROOT/tools/repo"
    chmod 755 "$WORK_ROOT/tools/repo"
fi

cd "$KERNEL_DIR"
# The launcher otherwise fetches every repo-tool branch/tag during bootstrap.
# A shallow stable checkout avoids that large initial fetch on domestic links.
if [[ ! -f .repo/repo/main.py ]]; then
    mkdir -p .repo
    git clone --depth=1 --single-branch --branch=stable "$REPO_URL" .repo/repo
fi
if [[ ! -f .repo/manifests.git/HEAD ]]; then
    repo init -u "$AOSP_MIRROR/kernel/manifest" \
        -b common-android14-6.1-2025-06 --depth=1 --no-clone-bundle \
        --repo-rev=stable
fi
mkdir -p .repo/local_manifests
cat > .repo/local_manifests/sgame-6.1.138.xml <<XML
<?xml version="1.0" encoding="UTF-8"?>
<manifest>
  <remote name="sgame-common" fetch="$COMMON_MIRROR" />
  <extend-project name="kernel/common" remote="sgame-common" revision="refs/tags/android14-6.1-2025-06_r1" />
</manifest>
XML
if [[ "${SYNC_LOCAL_ONLY:-0}" == 1 ]]; then
    repo sync -l -j"${SYNC_JOBS:-4}" --fail-fast
else
    repo sync -c -j"${SYNC_JOBS:-4}" --no-clone-bundle --fail-fast
fi
repo manifest -r -o "$WORK_ROOT/artifacts/kernel-manifest.xml"
python3 - "$KERNEL_DIR/common" <<'PY'
import pathlib, re, subprocess, sys
root = pathlib.Path(sys.argv[1])
expected = "098619d7de5518919c2cb3dec6cc832967029b04"
actual = subprocess.check_output(["git", "-C", str(root), "rev-parse", "HEAD"], text=True).strip()
values = dict(re.findall(r"^(VERSION|PATCHLEVEL|SUBLEVEL) = (\d+)$", (root / "Makefile").read_text(), re.M))
version = ".".join(values[k] for k in ("VERSION", "PATCHLEVEL", "SUBLEVEL"))
if actual != expected or version != "6.1.138":
    raise SystemExit(f"wrong kernel source: {actual} version {version}")
print(f"Kernel source pinned: {actual} version {version}", flush=True)
PY

tools/bazel build --jobs="$BUILD_JOBS" --shell_executable="$WORK_ROOT/tools/kleaf-bash" \
    //common:kernel_aarch64 //common:kernel_aarch64_modules_prepare

# Import the author's functions, omit only its automatic main() invocation.
# This leaves the source copy and original interface/version headers unchanged.
python3 - "$PREPARED_SRC/build_all.sh" "$WORK_ROOT/author-functions.sh" <<'PY'
import pathlib, sys
source = pathlib.Path(sys.argv[1]).read_text()
entry = '\nmain "$@"'
if source.count(entry) != 1:
    raise SystemExit("upstream build entry changed; review adapter")
pathlib.Path(sys.argv[2]).write_text(source.replace(entry, "\n# main called by sgame adapter"))
PY
source "$WORK_ROOT/author-functions.sh"
KERNELS_ROOT="$WORK_ROOT/kernels"
DRIVER_SRC="$PREPARED_SRC/lsdriver"
readonly STRIP_CHOICE=n
CLANG_PATH="$KERNEL_DIR/prebuilts/clang/host/linux-x86/clang-r487747c"
build_kernel "6.1-Android14" "$CLANG_PATH" "aarch64-linux-gnu-" \
    "LLVM_TOOLCHAIN_PATH=$CLANG_PATH"
cp "$DRIVER_SRC/6.1-Android14.ko" "$WORK_ROOT/artifacts/lsdriver-swap-6.1.138.ko"
cp "$PREPARED_SRC/sgame-source.json" "$WORK_ROOT/artifacts/driver-source.json"

python3 - "$WORK_ROOT" "$KERNEL_DIR" "$CLANG_PATH" "$ADAPTER_DIR/build-6.1.138.sh" <<'PY'
import hashlib, json, pathlib, subprocess, sys
work, kernel, clang, build_script = map(pathlib.Path, sys.argv[1:])
artifacts = work / "artifacts"
module = artifacts / "lsdriver-swap-6.1.138.ko"
readelf = clang / "bin/llvm-readelf"
symbols = subprocess.check_output([str(readelf), "--symbols", str(module)], text=True)
imports = sorted({line.split()[-1] for line in symbols.splitlines()
                  if " UND " in line and len(line.split()) >= 8})
output = (kernel / "bazel-bin/common/kernel_aarch64").resolve()
exports = {line.split()[1] for line in (output / "Module.symvers").read_text().splitlines() if line.strip()}
missing = sorted(set(imports) - exports)
if missing:
    raise SystemExit("module imports missing from kernel exports: " + ", ".join(missing))
required = {"get_swap_device", "swap_duplicate", "swap_cache_get_folio", "read_swap_cache_async",
            "kernel_read", "vfs_getattr"}
release_candidates = {"swap_free", "swap_free_nr", "free_swap_and_cache_nr", "free_swap_and_cache"}
lookups = {line.split()[-1]: line for line in (output / "System.map").read_text().splitlines()
           if line.split() and line.split()[-1] in (required | release_candidates)}
if required - lookups.keys():
    raise SystemExit("runtime lookup symbols missing from kernel: " + ", ".join(sorted(required - lookups.keys())))
releases = sorted(release_candidates & lookups.keys())
if not releases:
    raise SystemExit("no supported swap slot release symbol in kernel")
source = json.loads((artifacts / "driver-source.json").read_text())
version = source["layer"]["version"]
metadata = subprocess.check_output([str(readelf), "--string-dump=.modinfo", str(module)], text=True)
if f"sgame_swap={version}" not in metadata or "vermagic=6.1.138" not in metadata:
    raise SystemExit("module has wrong swap version or vermagic")
simulation = source["layer"]["simulation"]
if f"sgame_read_mode={source['layer']['read_mode']['name']}" not in metadata or \
        f"sgame_simulation={simulation['version']}" not in metadata or \
        f"sgame_simulation_source=enkernel-{simulation['commit']}" not in metadata:
    raise SystemExit("module has wrong simulation extension metadata")
for expected in ('sgame_core=author-features-group-poll', 'sgame_network_probe=disabled',
                 'sgame_features=module-hide,thread-hide,client-hide,kgsl-hide,proc-block,exit-hooks,cpu-render'):
    if expected not in metadata:
        raise SystemExit('Missing restored feature metadata: ' + expected)
(artifacts / "module-info.txt").write_text(metadata)
(artifacts / "module-imports.txt").write_text("\n".join(imports) + "\n")
(artifacts / "lookup-symbols.json").write_text(json.dumps(lookups, indent=2) + "\n")
config = (output / ".config").read_text()
if "CONFIG_ARM64_4K_PAGES=y" not in config:
    raise SystemExit("wrong page size in 6.1 GKI build")
(artifacts / "kernel.config").write_text(config)
subprocess.run([sys.executable, str(build_script.with_name("verify_pac_scs.py")),
                str(module), "--output", str(artifacts / "pac-scs.json")], check=True)
record = {"kernel": "6.1.138", "swap_version": version, "module": module.name,
          "page_size": "4k", "common_commit": "098619d7de5518919c2cb3dec6cc832967029b04",
          "read_mode": source["layer"]["read_mode"]["name"],
          "simulation_version": simulation["version"], "simulation_source_commit": simulation["commit"],
          "sha256": hashlib.sha256(module.read_bytes()).hexdigest(), "size": module.stat().st_size,
          "imports_checked": len(imports), "missing_exports": missing,
          "runtime_lookup_symbols_checked": sorted(lookups),
          "runtime_release_symbols_available": releases,
          "pte_breakpoints": "unavailable in baseline source" if "unavailable_ptebp" in source["layer"] else "upstream implementation",
          "build_script_sha256": hashlib.sha256(build_script.read_bytes()).hexdigest(),
          "driver_source_sha256": hashlib.sha256((artifacts / "driver-source.json").read_bytes()).hexdigest(),
          "kernel_manifest_sha256": hashlib.sha256((artifacts / "kernel-manifest.xml").read_bytes()).hexdigest(),
          "kernel_config_sha256": hashlib.sha256((artifacts / "kernel.config").read_bytes()).hexdigest(),
          "pac_scs_verification_sha256": hashlib.sha256((artifacts / "pac-scs.json").read_bytes()).hexdigest(),
          "compiler": subprocess.check_output([str(clang / "bin/clang"), "--version"], text=True),
          "device_validation": "pending"}
(artifacts / "build.json").write_text(json.dumps(record, indent=2) + "\n")
print(json.dumps(record, indent=2))
PY
