# Build and test

The supported surface is one CMake graph, one `trit_current_conformance`
executable, and four commands in `tools/trit_tool.py`. The current tuple is ISA2,
executable/function ABI3, syscall ABI2, vector ABI1, tBoot3, tDisk2. Older artifacts
are rejected; callers must rebuild from source.

```powershell
python tools/trit_tool.py doctor
python tools/trit_tool.py contract-check
python tools/trit_tool.py test
cmake --build build_current_cleanup --target ci_production
python tools/trit_tool.py export-diagnostics
```

`test` checks inputs/contracts, configures `build_current_cleanup`, then builds
`current_validate`. That target depends on the conformance executable, which
also depends on `current_images`. Image outputs are `release/TernaryOS/ternary-os.tboot`
and `.tdisk`, produced by `build_tos_image`. CMake is execution authority;
`TEST_MANIFEST.json` describes the graph and exact selectors.

With `BUILD_TESTING=ON`, `ci_production` depends on `tritc`, `build_tos_image`, and
`ctest`; that target runs the single CTest entry. With testing disabled it depends
on the compiler, builder and `current_validate`. The current CTest timeout is 0
(no imposed CTest timeout); guest scenarios retain their bounded execution budgets.

SDL targets exist only with `TRIT_ENABLE_SDL_RELEASE`, SDL2, and `run_tos_sdl.cpp`.
`stage_tos_release` depends on `current_images` and `run_tos_sdl`;
`smoke_tos_release` depends on staging and runs ten frames on the same formats.

```powershell
cmake --build build_current_cleanup --target stage_tos_release
cmake --build build_current_cleanup --target smoke_tos_release
```

The default conformance path retains full WAL (16 guards and all original
pressure/recovery/fault/targeted-fsync workloads), SDK, numeric, metrics, clock,
original desktop, stream, full33 service/coexistence, and CLI E2E workflows.
`--service-auto-modes-only` separately runs exactly normal, spawn failure and
post-launch startup failure with the original GUI actions. It does not claim
full33/default coverage. The original `--service-auto-only` still runs full33 plus
those three modes. Focused service/WAL/clock/link selectors are diagnostics;
a focused PASS does not replace a full gate. See the manifest for all23 selectors.

The original desktop body is unchanged. Its outer driver copies the release
image pair to an artifact-local input directory, fingerprints pristine initial
copies, and uses a separate writable disk. Ordinary GUI/VFS writes therefore
leave the reusable release images unchanged.

## CLI E2E and diagnostics

`--cli-only` invokes the actual copied CLI on generated artifact-local current
inputs, including valid and missing/malformed/schema/path/mapping cases and
completion-coupling failures. Copied reports are explicitly validation fixtures,
not guest-runtime PASS evidence. It runs doctor, contract-check and export only;
it never recursively invokes `test`. The outer `test`/CMake gates exercise that
fourth command and the full executable. Initial Red and command logs are retained
under `service_workflow_artifact/final_gate`.

Outer workflow drivers publish pending completion before artifact mutation and
complete only after return and closed evidence. Sidecars live under
`service_workflow_artifact/completion`, outside workflow reset directories. They
couple actual outcome, selector/scope, exact scenario coverage, the84 accepted
source inputs, and required report/evidence bytes. A workflow completion does not
assert that the entire conformance executable or shell command exited0.

`export-diagnostics` writes `current-platform-diagnostics.json` with the complete
tuple, explicitly labelled SHA256 source provenance and per-group status:
missing, pending, invalid, stale, failed or verified. Completion integrity uses
standard FNV-1a-64 (offset14695981039346656037); it is not cryptographic authentication
and differs from the native image-checksum basis. Report existence or separately
fresh source hashes cannot establish PASS. Evidence must match source bytes,
report bytes, scope and required files. Legitimate zero-record logs remain present
and fingerprinted; unexpected appends invalidate proof. Unexecuted/stale groups
remain unverified. Historical Red and old root-level phase1 reports are retained
separately and do not become current failures or PASS. Graphify/Obsidian are advisory.


## Owner acceptance

Owner accepted the rollout on2026-10-11(local time) with remaining validation waived
to save usage. Ten final-source default workflows and independent CLI94, desktop and
automatic-three checks passed. The full suite was intentionally stopped at982,001,000
steps in history_cap1, with zero guest failures and3,421 words of arena use.
Remaining full/CI/SDL/fresh-review validation is waived, without PASS claims.
Acceptance and preserved results: `build_current_cleanup/service_workflow_artifact/final_gate/parent-full-test-final94-20261011/user_acceptance_result.json`.

The actual stopped command exited-1. Its pending service completion remains pending;
owner acceptance does not change supported commands or convert unfinished evidence
into a passed gate.
