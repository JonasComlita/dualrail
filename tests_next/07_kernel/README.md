# 07 Kernel

Focused native-kernel tests.

Required areas:

- trap entry and syscall dispatch;
- bootstrap allocator, buffer pool, WAL, relational store;
- scheduler and process table;
- process control and capabilities;
- kill/crash cleanup;
- kernel logs and diagnostic hooks.

Old reference tests: `tests/test_phase_d_kernel.cpp`,
`tests/test_production_hardening.cpp`.
