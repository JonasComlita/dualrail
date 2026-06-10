# Trit Verification Suite Next

`tests_next/` is the clean-room replacement plan for the historical `tests/`
tree. Do not delete or weaken `tests/` while this tree is being filled out:
the old suite is still the current regression memory for completed milestones.

The purpose of this tree is to let agents continue each other's verification
work without rediscovering the system every time. Every new test should be
small, named, categorized, and recorded in the manifests.

## Operating Rules

1. Prefer many focused tests over monolithic files.
2. Add grouped subsystem gates after the focused tests exist.
3. Add only a few end-to-end tests, and make each one prove a user-visible
   scenario.
4. Keep old and new suites running side by side until `tests_next` covers all
   milestone claims currently proven by `tests/`.
5. When adding a test, update the relevant manifest before or in the same
   change as the test.

## Directory Map

| Directory | Purpose |
|-----------|---------|
| `00_harness` | shared test harness, helpers, fixtures, golden file conventions |
| `01_isa` | ISA encoding, opcodes, registers, immediates, traps, CSR names |
| `02_vm` | VM execution, memory, MMU, traps, CSRs, timers, multicore, profiling |
| `03_assembler` | `.tasm` parsing, labels, directives, encoding, diagnostics |
| `04_compiler` | lexer, parser, typechecker, IR, register allocation, codegen, linker |
| `05_language` | source-language behavior and golden `.trit` programs |
| `06_runtime_ulib` | `ulib`, SDK wrappers, malloc/free, formatting, collections |
| `07_kernel` | trap path, syscalls, scheduler, process model, IPC, signals, capabilities |
| `08_filesystem_vfs` | VFS, WAL, persistence, root image builder, crash recovery |
| `09_process_ipc_scheduler` | process lifecycle, futex/event waits, IPC, scheduler fairness |
| `10_drivers_hal` | device tree, framebuffer, block device, timer, input, console, network/audio |
| `11_graphics_gui` | six GUI layers, rendering, compositor, widgets, shell/desktop |
| `12_apps` | bundled app behavior, isolated and inside the OS |
| `13_distribution_images` | `.tboot/.tiso`, `.tdisk`, release builder, host runtime diagnostics |
| `14_security_hardening` | fuzzing boundaries, capabilities, signed packages, crash cleanup |
| `15_fuzz_property` | fuzz/property tests across parser, VM, syscalls, VFS, GUI events |
| `16_perf_benchmarks` | measured performance baselines and trend tests |
| `17_full_system` | minimal full-system acceptance scenarios |
| `manifests` | machine-readable state, coverage, gates, and test templates |
| `tools` | small helper scripts for agents and CI |

## Agent Workflow

1. Read `AGENTS.md`.
2. Run `python tests_next/tools/validate_manifests.py`.
3. Open `manifests/status.json` and choose the first unblocked task.
4. Add or port focused tests first, then group tests.
5. Update `manifests/coverage_matrix.json` and `manifests/status.json`.
6. Run the focused target and the old suite target that currently proves the
   same claim.
7. Leave clear notes in `status.json` if something remains blocked.

## Completion Rule

The old `tests/` tree can be retired only after `tests_next` has:

- focused coverage for every major layer;
- grouped subsystem gates for every major layer;
- at least one end-to-end release/OS scenario;
- a coverage matrix row for every old milestone claim;
- a full green run of both old and new suites.
