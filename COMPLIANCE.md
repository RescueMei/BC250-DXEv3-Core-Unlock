# Core Unlock Compliance

Driver: `BC250-DXEv3-Core-Unlock`

Use this checklist before merging. Do not use the README as the compliance record.

- [x] Shared package submodule/pin present.
- [x] Configuration ABI uses `MeiMeiDXEv3SharedPkg`; no new local config duplicate.
- [ ] SMU SRAM allocations match `docs/SMU_MEMORY_MAP.md`.
- [ ] Depex and protocol installation reviewed.
- [ ] Disabled/missing/malformed configuration path performs no feature writes.
- [ ] Factory learning behavior reviewed (Core/CU).
- [ ] No cross-driver private headers or internal calls.
- [ ] No forced-VID/force-state from SMU tick context.
- [ ] SMU writes are read-back verified.
- [ ] Protocol ABI drift check passed.
- [x] Individual driver build passed.
- [ ] Eight-driver integrated build passed.
- [ ] BC250 deployment reached `DEPLOY_OK`.
- [ ] Light/heavy workload validation completed with actual clock and thermal checks.
- [ ] No generated artifacts, secrets, or temporary files staged.
