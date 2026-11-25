# SGAME driver development branch

This branch integrates upstream `0a33fa0ef468873a2c94b2876eb88a6dfb482ec9` and retains the project's subsequent breakpoint fixes.
Develop the complete source directly. There is no patch-application step in the build.
`main` remains the upstream comparison branch. Provenance is recorded in `upstream.json`.

## Behavior

- Author module hiding, service-thread hiding, client hiding, KGSL hiding, proc blocking,
  taskstats exit hook and exception/exit diagnostics are retained.
- The fixed outbound initialization Ping is disabled. No other author feature group is disabled.
- CPU rendering remains disabled in the lifecycle, following the selected upstream revision.
- Client and GPU hide calls run before the handshake completion flag. Kernel logs include
  `connection: pid=... task_hide=... kgsl_hide=... handshake=ready`.
  As upstream, failed client hide hooks do not disable the memory transport. A successful
  handshake alone is not evidence of hiding; inspect the two return codes and actual nodes.
- Required proc blocking, service-thread hiding and taskstats hook installation failures
  fail initialization with an explicit stage. Module hiding happens after required resources
  succeed, so failure rollback still has normal kernel module/sysfs ownership.
- The original author hide implementations are byte-for-byte preserved. Vendor symbol resolution
  and inline-hook installation still require validation on each device/kernel.
- Swap-aware reads, cached/NC resident reads, request serialization and task/mm/page references
  are retained from the project. Writes retain the author's path.
- The taskstats exit event wakes the connection owner; sleeping cleanup happens outside the hook.
  Explicit kernel-exit requests schedule a separate worker to join both service threads and clean
  resources. Reconnection waits for in-flight requests and does not kill the previous client.
- The real PTE breakpoint header now provided by upstream is used. The old unavailable stub
  is not imported. Public author headers are preserved at the selected upstream revision.
- SG6 environment extension 1.1.0 remains in `lsdriver/sgame_simulation/`; emulation remains
  in userspace. The Swap/core version is 1.3.0 (`author-features-group-poll`).
- Android 12/13 5.10 use the page-based Swap API, including its boolean `do_poll`
  argument. Their RCU device guard ends after the slot reference is acquired and
  before cache/backend waits. Android 13 5.15 also uses page APIs, retaining its
  sleepable device reference through I/O. Android 15 6.6 uses the head folio's
  base swap entry with page-returning backend APIs; 6.12 uses folio-returning APIs.
  Android 17 6.18 additionally uses the one-argument swap-table cache lookup.
- Inherited LS children do not replace their connected parent. Upstream client
  candidate, SIGKILL and final process-exit diagnostics are integrated; existing
  exception hooks and deferred sleeping cleanup remain in place.
- Ordinary C inherits target Kbuild PAC/BTI/SCS settings; hardware template ISA declarations
  remain local to the assembly blocks, retaining the project's PAC/SCS fix.

## Local checks

```bash
bash sgame/test_driver_swap.sh
bash sgame/test_driver_simulation.sh
bash sgame/test_driver_ptebp.sh
python3 sgame/test_source_policy.py
```

The fixtures exercise production Swap/session/lifecycle/environment code with kernel API substitutes,
including partial pinning, sleeping readers, reconnects, pre-handshake hook calls, hook errors,
required feature failures, shutdown and reference rollback. The real author header is also compiled
with the Android arm64 NDK. These checks do not install hooks or load a module on the host/phone.

## Pinned source builds

Commit changes first. Export reads committed Git objects; it does not silently use uncommitted edits.
The selected mode is a normal `SGAME_READ_NC=0/1` make variable, not a rewritten source header.
Python 3.9+ and GNU tar are required for export.

```bash
python3 sgame/export_source.py --read-mode cached --output build/source-cached
python3 sgame/export_source.py --read-mode nc --output build/source-nc
bash sgame/build/build-6.1.138.sh /path/to/work61 "$PWD/build/source-cached"
bash sgame/build/build-6.12.23.sh /path/to/work-a16-5 "$PWD/build/source-cached"
bash sgame/build/build-6.12.58.sh /path/to/work-a16-6 "$PWD/build/source-cached"
# Build both modes for all three pinned kernel configurations:
bash sgame/build/build-read-matrix.sh /path/to/work61 /path/to/work-a16-5 /path/to/work-a16-6 \
  "$PWD/build/source-cached" "$PWD/build/source-nc" /path/to/new-artifacts
```

Builds record the Git commit, exact exported file hashes, kernel/KMI/page size, mode, module metadata,
imports and output SHA-256. Native 4K is the application's delivery configuration; the author's
fixed user mapping address still requires separate userspace work for native 16K devices.
Kernel environments are pinned independently of upstream driver updates.

### Full NC matrix

The November 25, 2026 build includes eight 4K NC modules. Cached variants are
not built by `build/build_nc_matrix.py`. Each profile uses its own prepared source copy.

| Profile | Kernel | Android / KMI |
| --- | --- | --- |
| a12-510 | 5.10.237 | 12 / 9 |
| a13-510 | 5.10.237 | 13 / 4 |
| a13-515 | 5.15.206 | 13 / 8 |
| a14-61 | 6.1.138 | 14 / 11 |
| a15-66 | 6.6.89 | 15 / 8 |
| a16-5 | 6.12.23 | 16 / 5 |
| a16-6 | 6.12.58 | 16 / 6 |
| a17-618 | 6.18.21 | 17 / 5 |

New 5.15/6.6/6.18 manifests pin GKI dependencies to their recorded Google
superprojects. Vendor modules, bootloader targets and kernel testing projects
are omitted. The source commit and reference hashes are in `build/kernel-baselines.json`.

Export NC source with `--read-mode nc` and pass the prepared workspaces and a new
artifact directory to `build/build_nc_matrix.py`. `--profiles` selects a subset for resuming a
matrix. Legacy inputs live under `WORK/deps` and `WORK/host-inputs`; modern workspaces use
`WORK/PROFILE/kernels/KERNEL-AndroidVERSION`. All module metadata and imports are checked against real GKI outputs.

### Android 5.10

`build/5.10-baselines.json` pins Android 12 KMI 9 and Android 13 KMI 4,
both 5.10.237/4K, with their matching Clang 12/14 revisions and API hashes.
Prepare the pinned kernel and Clang trees, then build each mode from a separate export:

```bash
bash sgame/build/build-5.10.sh /path/to/work-a12 /path/to/source-cached a12 \
  --kernel /path/to/a12/common --clang /path/to/clang-r416183b --host-tools /path/to/prebuilts/build-tools
bash sgame/build/build-5.10.sh /path/to/work-a13 /path/to/source-cached a13 \
  --kernel /path/to/a13/common --clang /path/to/clang-r450784e --host-tools /path/to/prebuilts/build-tools
```

Repeat with the NC export and the same kernel workspaces to reuse the GKI build.
The script retains GKI CFI/LTO, builds the kernel symbol tables, checks actual
imports and required dynamic lookups, and records source/ELF hashes and metadata.
The host reader fixture includes both 5.10 modes and rejects RCU held across I/O.
Android 5.10 Bionic stack names are read from the target mm using the same GUP
protocol as its proc maps reader. The name fixture checks every page offset,
cross-page names, exact matches and cleanup after partial pinning.
For minimal build hosts, `--host-sysroot` accepts privately extracted native
development packages (including zlib headers); system package changes are unnecessary.

No rebuilt `.ko` is included in this source migration. Existing application/phone modules remain
historical artifacts until the server builds and device verification complete. Hidden modules
cannot be identified by relying on `/proc/modules` or `/sys/module`; retain the exact image hash
and source record before insertion and capture the new kernel feature results after loading.
