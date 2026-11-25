# Upstream integration, November 25, 2026

Upstream: 0a33fa0ef468873a2c94b2876eb88a6dfb482ec9.
Recorded previous upstream: df4ef96c90365c0ba7e638a8b7fadbe245c701f4.
Project baseline: 9b2aaa869f3b47b79690785aae98bc4040eab39b.
Backup branch: backup/sgame-before-upstream-20261125.

Upstream rewrote its dump archive history. Source integration uses the recorded
previous upstream as the effective three-way baseline and records both real
parents in the merge commit. Project history is retained without rebasing.

The PTE/UDF implementation, single-step implementation, breakpoint dispatcher
and PTE compatibility header are unchanged from the project baseline. This
retains the preparation/publication ordering in ff7ec74 and the fault handling,
mmap trylock and stale-session rollback changes in 9b2aaa8. The native PTE
dependencies restored in 7c580f3 also remain present. Earlier DPT thread scopes,
shared-table updates and syscall-exit changes remain in the combined source.

Overlapping DPT and hardware breakpoint files incorporate upstream's locking
and CFI fixes while retaining the project's page-table ownership tracking.
Client discovery and upstream signal diagnostics use the project's retained
references and deferred teardown. Existing exception hooks remain present.
Public driver/loader/protocol headers match the selected upstream exactly.
CPU rendering and the fixed outbound probe remain disabled.

The NC matrix adds pinned Android 13 5.15.206, Android 15 6.6.89 and Android 17
6.18.21 configurations to the existing five configurations. Swap 1.3.0 accounts
for the different device reference, folio entry and swap-table APIs. Host reader
fixtures cover each reviewed API family, including reference balancing and
large-folio tail reads. PTE, simulation and source-policy checks also pass.
Kernel compilation and device validation are separate from these host checks;
each resulting module receives its own source record and build metadata.
