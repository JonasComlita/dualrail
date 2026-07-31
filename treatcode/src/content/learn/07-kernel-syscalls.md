---
{
  "schema": "trit.treatcode_learning_page.v1",
  "id": "kernel-syscalls",
  "title": "Kernel and syscalls",
  "module": "7. Operating system",
  "level": "eecs",
  "order": 70,
  "summary": "Trace a user request through the kernel dispatch and the documented syscall ABI.",
  "prerequisites": ["boot-and-traps"],
  "sources": [
    { "path": "kernel.trit", "label": "Kernel dispatch and services", "kind": "source" },
    { "path": "ternary_os.h", "label": "Host OS/runtime surface", "kind": "source" },
    { "path": "SYSCALL_MANIFEST.json", "label": "Syscall ABI manifest", "kind": "manifest" },
    { "path": "apps/os_sdk.trit", "label": "Application SDK wrappers", "kind": "source" }
  ],
  "evidence": [
    { "path": "tests/test_os_platform.cpp", "label": "OS platform tests", "kind": "test" },
    { "path": "tests/test_phase_d_kernel.cpp", "label": "Kernel phase-D tests", "kind": "test" }
  ],
  "next": "storage-and-images",
  "interactive": {
    "kind": "choice",
    "title": "ABI check",
    "prompt": "Where does the syscall service ID live according to the documented ABI?",
    "options": ["CSR syscall_id", "The stack only", "The PC low trits"],
    "answer": 0,
    "explanation": "The syscall ID is in CSR syscall_id. Primary arguments use r13 through r16 and returns use r13 through r15."
  }
}
---

## A syscall is a contract

The kernel is written in TCL and owns process state, the VFS, device-facing
services, and syscall dispatch. Application code should use the SDK wrappers;
the manifest remains the ABI source of truth.

The current documented convention places the service ID in CSR `syscall_id`,
primary arguments in `r13` through `r16`, and returns in `r13` (status), `r14`
(payload), and `r15` (detail). A wrapper is useful only when it preserves those
semantics.

## Prerequisites

Complete the boot and trap page. You should be able to distinguish a malformed
instruction trap from a deliberate kernel service request.

## Production and evidence

Use the kernel, OS runtime, syscall manifest, and SDK together. The focused OS
tests validate the integration boundary; this page does not duplicate kernel
code in the website.

## Next step

Continue to **Storage and images** and follow persistence from a syscall to a
recoverable disk image.
