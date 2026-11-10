# SGAME driver development branch

This branch is based on upstream `adef23a7e537efc937f01cfbbd44c3c7c33be216`.
Develop the complete source directly. There is no patch-application step in the build.
`main` remains the upstream comparison branch. Provenance is recorded in `upstream.json`.

## Behavior

- Author module hiding, service-thread hiding, client hiding, KGSL hiding, proc blocking,
  taskstats exit hook, exception/exit diagnostics and CPU rendering are retained.
- The fixed outbound initialization Ping is disabled. No other author feature group is disabled.
- The upstream CPU renderer and its five-second initialization demonstration are retained,
  including upstream's nonfatal handling of unsupported rendering.
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
  in userspace. The Swap/core version is 1.2.0 (`author-features-group-poll`).
- Android 12/13 5.10 use the page-based Swap API, including its boolean `do_poll`
  argument. Their RCU device guard ends after the slot reference is acquired and
  before cache/backend waits. Existing folio paths for 6.1/6.12 are retained.
- Ordinary C inherits target Kbuild PAC/BTI/SCS settings; hardware template ISA declarations
  remain local to the assembly blocks, retaining the project's PAC/SCS fix.

## Local checks

```bash
bash sgame/test_driver_swap.sh
bash sgame/test_driver_simulation.sh
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
The three kernel environments are pinned independently of upstream driver updates.

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

No rebuilt `.ko` is included in this source migration. Existing application/phone modules remain
historical artifacts until the server builds and device verification complete. Hidden modules
cannot be identified by relying on `/proc/modules` or `/sys/module`; retain the exact image hash
and source record before insertion and capture the new kernel feature results after loading.
