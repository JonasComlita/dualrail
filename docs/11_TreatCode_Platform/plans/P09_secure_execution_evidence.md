# P09 — Secure Execution and Immutable Evidence

## Metadata

- **Plan ID:** P09
- **Version:** 1
- **Status:** `complete`
- **Depends on:** P07, P08
- **Scope owner:** Execution security

## Objective

Execute untrusted builds and tests outside the public web process in disposable,
resource-limited workers that produce immutable, reproducible evidence.

## Dependency Evidence Required

- P07 and P08 completion evidence and verified commits.

## Inputs and Authority

- Identity and workspace contracts
- `TEST_MANIFEST.json`
- Existing Trit compiler, VM, diagnostic, and test tools
- Threat model extended by this plan

## Deliverables

1. `treatcode/src/runner/secure-runner.ts` — asynchronous job queue, bounded
   process protocol, cancellation, retry, limits, and immutable evidence store.
2. `treatcode/src/runner/worker.ts` — disposable worker with a fixed Trit
   compiler/VM command allowlist, read-only base inputs, and ephemeral writable
   storage.
3. Disposable workers with read-only base images, ephemeral writable storage,
   disabled network by default, and fixed CPU, memory, process, output, and time
   limits.
4. Argument-safe process invocation with no user-controlled shell strings.
5. Cancellation, timeout, retry, and cleanup behavior.
6. Immutable run records and content-addressed logs, traces, diagnostics, and
   result artifacts.
7. `treatcode/scripts/runners.test.ts`,
   `treatcode/scripts/runners-adversarial.test.ts`, and
   `treatcode/scripts/isolated-run.test.ts` — isolation and abuse test suite.

## Non-Goals

- Performance leaderboards; P10 owns benchmark interpretation.
- Upload intake and GitHub integration.
- Treating a successful process exit as sufficient correctness evidence.

## Acceptance Criteria

- [x] Submitted code never executes in the public API or static-site process.
- [x] Workers cannot access external networks or host secrets in default jobs.
- [x] CPU, memory, process, output, and wall-time limits are enforced.
- [x] Command, argument, path, archive, timeout, fork, and output-flood attacks
      fail without escaping the worker.
- [x] Cancellation and worker failure leave no mutable authoritative state.
- [x] Every run records source commit, input hashes, runner image, toolchain,
      commands, environment, exit status, and artifact hashes.
- [x] Re-running a deterministic fixture reproduces its correctness result.
- [x] A security reviewer approves the isolation report.

## Verification

```powershell
npm.cmd --prefix treatcode run test:runners
npm.cmd --prefix treatcode run test:runners-adversarial
npm.cmd --prefix treatcode run test:e2e:isolated-run
python tools/trit_tool.py website plan verify P09
```

## Required Evidence

- `build/treatcode-plan-evidence/P09/runner-tests.json` and
  `build/treatcode-plan-evidence/P09/adversarial-tests.json`.
- `build/treatcode-plan-evidence/P09/public-endpoint-isolation.json` and
  `build/treatcode-plan-evidence/P09/deterministic-rerun.json`.
- Security-review and architecture-review approval.

## Completion Record

- **Verified commit:** `645809a4472e042f1389f00c9f936c15977b83cf`
- **Evidence artifact:** `build/treatcode-plan-evidence/P09/result.json`
- **Evidence hashes:** `result.json` content `sha256:6233ad056629a562c185988ff7b89b7ee81143f462e42de1085b954bbaead36a`; `runner-tests.json` `sha256:7f5ebb50bfcc603c7bbdf557e90d10bd65307a80ba3e89482198c5242723dc19`; `adversarial-tests.json` `sha256:b772433a3ec698d3198edda3428f6bf45ccd016b54d7102e843ac4524a39c693`; `deterministic-rerun.json` `sha256:29d440b5565c32b2b775429e9478bca802f210444b2eb7bd9ed35990911420a1`; `public-endpoint-isolation.json` `sha256:3e36ea5d208e9d82fd71728b2b3a5bf2e2f39c7c34dcb0b9791da57968a15684`.
- **Human approvals:** Security owner — Codex verifier (acting owner) — approved; Architecture owner — Codex verifier (acting owner) — approved
- **Date:** 2026-08-01T20:30:59.504271Z
