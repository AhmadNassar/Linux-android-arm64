# Driver source development

- Develop `sgame` directly in the tracked C/H files. Builds export a Git revision and never apply patch series or rewrite source files.
- Keep upstream provenance in `sgame/upstream.json`. Keep author public driver/loader/protocol files unchanged for the selected upstream revision; place project transport extensions in `lsdriver/sgame_simulation/`.
- Preserve module hiding, connection/dispatch thread hiding, client hiding, KGSL hiding, proc blocking, exit/exception hooks and upstream CPU rendering. The fixed outbound network probe is disabled by user request.
- Log client hook return codes before publishing handshake completion. Preserve upstream best-effort behavior for client hook failures; do not claim runtime hiding from a successful memory handshake.
- Keep task/mm/GUP/vmap references and Swap request serialization. Exit hooks signal the connection thread; sleeping cleanup must not run under taskstats_exit locks.
- Run the affected `sgame/test_driver_*.sh` tests and `python3 sgame/test_source_policy.py`. A host fixture pass is not a kernel build or device validation.
- Record new source commits and resulting module SHA-256/build metadata. Do not replace historical artifacts in the application repository.
