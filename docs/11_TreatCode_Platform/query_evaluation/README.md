# TreatCode Query Evaluation

This corpus verifies that TreatCode retrieves current Trit/TCL implementation
facts instead of substituting generic ternary-computing material.

## Corpus

- `questions/Q001-Q034.json`: authority, representation, ISA, numeric, VM,
  execution, assembler, and low-level IR.
- `questions/Q035-Q067.json`: TCL, compiler, runtime, boot, traps, kernel,
  syscalls, and IPC.
- `questions/Q068-Q100.json`: storage, devices, host integration, applications,
  releases, products, security, benchmarks, and known gaps.
- `findings/`: source/index inconsistencies found during each shard review.
- `AGENT_PROMPT.md`: reusable instructions for an agent performing the audit.

## Completion Gate

Run from the repository root:

```powershell
npm.cmd --prefix treatcode run generate:public
npm.cmd --prefix treatcode run test:queries
```

Completion is unambiguous only when the evaluator reports all four gates as
`100/100`: retrieval, provenance, source support, and generic contamination.
The machine-readable result is written to
`build/treatcode-query-evaluation/result.json`; the concise report is
`build/treatcode-query-evaluation/REPORT.md`.

The corpus is versioned repository data. Any source, manifest, or search-index
change that alters an expected result must update the relevant question and
rerun the full gate; expectations must not be weakened simply to obtain a pass.
