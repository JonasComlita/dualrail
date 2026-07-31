# P09 — Secure Execution and Immutable Evidence

## Metadata

- **Plan ID:** P09
- **Version:** 1
- **Status:** `not_started`
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

1. Asynchronous job queue and runner protocol.
2. Disposable workers with read-only base images, ephemeral writable storage,
   disabled network by default, and fixed CPU, memory, process, output, and time
   limits.
3. Argument-safe process invocation with no user-controlled shell strings.
4. Cancellation, timeout, retry, and cleanup behavior.
5. Immutable run records and content-addressed logs, traces, diagnostics, and
   result artifacts.
6. Isolation and abuse test suite.

## Non-Goals

- Performance leaderboards; P10 owns benchmark interpretation.
- Upload intake and GitHub integration.
- Treating a successful process exit as sufficient correctness evidence.

## Acceptance Criteria

- [ ] Submitted code never executes in the public API or static-site process.
- [ ] Workers cannot access external networks or host secrets in default jobs.
- [ ] CPU, memory, process, output, and wall-time limits are enforced.
- [ ] Command, argument, path, archive, timeout, fork, and output-flood attacks
      fail without escaping the worker.
- [ ] Cancellation and worker failure leave no mutable authoritative state.
- [ ] Every run records source commit, input hashes, runner image, toolchain,
      commands, environment, exit status, and artifact hashes.
- [ ] Re-running a deterministic fixture reproduces its correctness result.
- [ ] A security reviewer approves the isolation report.

## Verification

```powershell
npm --prefix treatcode run test:runners
npm --prefix treatcode run test:runners-adversarial
npm --prefix treatcode run test:e2e:isolated-run
python tools/trit_tool.py website plan verify P09
```

## Required Evidence

- Isolation and adversarial-test reports.
- Deterministic rerun comparison.
- Security-review approval.

## Completion Record

- **Verified commit:**
- **Evidence artifact:**
- **Human approvals:** Security reviewer
- **Date:**

