# Acceptance Criteria

Use these gates when deciding whether a change is complete.

## Narrow Code Change

- Focused target from `TEST_MANIFEST.json` passes.
- Existing behavior covered by nearby tests is not weakened.
- Manifests are updated if source-of-truth contracts changed.

## Kernel Or Syscall Change

- `tools/trit-test.ps1 os` passes, or any failing target is documented with diagnostics.
- `SYSCALL_MANIFEST.json` matches `kernel.trit`, `ternary_compiler_ir.h`, and `apps/os_sdk.trit`.
- Failure paths return clear status/detail values.
- Diagnostics expose enough process or VM state to debug a failure.

## Host Runtime Or Image Change

- `test_host_runtime` passes.
- `tools/trit-inspect-image.ps1` validates generated `.tboot` output.
- Runtime diagnostics export all files listed in `DEBUGGING.md`.
- `smoke_tos_release` or `tools/trit-run.ps1 --smoke-test` passes when SDL is available.

## Production Health Claim

Run:

```powershell
tools/trit-test.ps1 production
```

A production health claim is not complete until this gate passes or the exact failing target and diagnostics path are recorded.
