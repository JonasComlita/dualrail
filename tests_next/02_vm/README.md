# 02 VM

Focused tests for VM execution semantics.

Required areas:

- numeric and lane execution;
- load/store and memory faults;
- traps, `ERET`, privilege, and CSRs;
- MMU/PTE permissions and zero PTE faulting;
- sparse memory and sparse disk hooks;
- timers and multicore stepping;
- future profiling/block-cache/JIT equivalence.

Old reference tests: `tests/test_kernel.cpp`, `tests/test_multiwidth_vm_main.cpp`,
`tests/test_vm_widths.cpp`, `tests/test_scaling_profile.cpp`.
