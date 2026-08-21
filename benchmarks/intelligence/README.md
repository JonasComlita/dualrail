# TC-SWE-001 — Repair the Three-Way Median Package

This directory is the versioned mini-repository fixture for the TreatCode
intelligence benchmark. It is intentionally small enough for an agent to
inspect, edit, and test in one session while still requiring a real package
repair: `compare.trit` and `median.trit` have related three-way ordering bugs.

The participant may edit only these files:

- `src/compare.trit`
- `src/median.trit`

The three public cases in `tests/public.metadata.v1.json` cover distinct
values. The server-only `tests/hidden.server.v1.json` contains deterministic
permutations, duplicates, negative/zero/mixed values, and safe-range values.
The hidden fixture is loaded by the TreatCode service but is never copied into
trial workspaces or returned by the public catalog API.

Each benchmark run creates four independent clean trial fixtures. A public
test may be run repeatedly while the trial is open. A hidden submission is
one-shot per trial; its individual result remains sealed until all four trials
have submitted. The aggregate score is `passed / 4 * 100`.

The source files below are intentionally defective. A participant's corrected
files should keep the public function names and signatures (`compare` and
`median`).
