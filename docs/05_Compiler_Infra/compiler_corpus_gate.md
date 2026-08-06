# Deterministic SSA Compiler Corpus Gate

`test_compiler_corpus_gate` is the quantitative acceptance gate for the
compiler's direct SSA target path.  It compiles the same representative corpus
with `OptimizationLevel::None` (O0) and default `CompilerOptions`, links both
images, and executes each image in the portable interpreter.

The corpus covers loops and phis, calls, register-pressure spills, aggregates,
ownership, atomics, branches, and memory-alias boundaries.  Every workload has
an explicit return-value oracle.  A fresh VM executes each mode twice; the two
architectural `RunResult::steps` values must be equal.  These are dynamic guest
instruction counts, not assembly length or optimizer/pass metadata.

The gate policy is fixed in [`COMPILER_CORPUS_SCHEMA.json`](../../COMPILER_CORPUS_SCHEMA.json):

- the median optimized count must be at least 15% lower than the O0 median;
- no individual workload may regress by more than 5%;
- both modes must compile, link, halt, match their oracle, and be deterministic.

Run the focused gate with:

```powershell
cmake --build build --target compiler_corpus_gate
ctest --test-dir build -R test_compiler_corpus_gate --output-on-failure
```

The harness emits one stable JSON object per invocation.  It includes the
current Git commit and dirty state, a corpus hash, source hashes per workload,
both mode counts/repetitions, correctness evidence, aggregate medians, and the
gate verdict.  The production `ci_production` target depends on this custom
target; no prior report in the ignored build tree is consumed.
