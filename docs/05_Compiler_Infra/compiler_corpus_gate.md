# Compiler validation surface

The former quantitative compiler-corpus harness was retired with the old test
and tooling trees. It is not a current release gate and no historical report
is consumed by the build.

The current compiler validation is intentionally consolidated:

```text
cmake --build build_current_cleanup --target current_validate
cmake --build build_current_cleanup --target ci_production
python tools/trit_tool.py contract-check
```

The contract check verifies ISA v2, executable/function ABI v3, and the tBoot
v3/tDisk v2 image boundary. If a future optimization program is needed, add a
new focused target with a current schema and explicit ownership rather than
restoring the retired corpus, benchmark, or wrapper paths.
