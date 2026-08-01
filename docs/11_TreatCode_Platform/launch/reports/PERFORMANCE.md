# P13 Performance Report

**State: BLOCKED pending P10 completion.**

The authoritative performance protocol is P10, not an uncontrolled local
benchmark. A release candidate must include the fixed workload, runner profile,
environment fingerprint, baseline/candidate comparison, and reference-check
result from P10. The launch gate is the P10 evidence artifact plus its required
architecture approval.

Required commands:

- `python tools/trit_tool.py test benchmark`
- `npm --prefix treatcode run test:benchmarks`
- `npm --prefix treatcode run test:e2e:implementation-arena`
- `python tools/trit_tool.py website benchmarks verify-reference`

No performance claim is promoted by this report while P10 is `not_started`.
