Ternary OS development release
==============================

This folder contains a staged Windows build of the Ternary OS host runtime.

Files:
- TernaryOS.exe: SDL host runtime for the VM.
- ternary-os.tboot: boot image containing the kernel and app manifest.
- ternary-os.tdisk: mutable sparse disk image used as the root filesystem.
- diagnostics/: default location for exported runtime diagnostics.

Smoke test:
Run TernaryOS.exe with --smoke-test and pass both image files:

  TernaryOS.exe --smoke-test --frames 10 ternary-os.tboot ternary-os.tdisk

Normal run:

  TernaryOS.exe ternary-os.tboot ternary-os.tdisk

The disk image is mutable. Keep a copy of ternary-os.tdisk if you want to
preserve or reset user-visible filesystem state between runs.
