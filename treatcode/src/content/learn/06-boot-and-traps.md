---
{
  "schema": "trit.treatcode_learning_page.v1",
  "id": "boot-and-traps",
  "title": "Boot and traps",
  "module": "6. Boot boundary",
  "level": "programmer",
  "order": 60,
  "summary": "Understand image headers, boot handoff, invalid instructions, and why traps are evidence rather than silent failure.",
  "prerequisites": ["compiler-pipeline"],
  "sources": [
    { "path": "bootloader.tasm", "label": "Bootloader entry sequence", "kind": "source" },
    { "path": "native_kernel_boot.tasm", "label": "Native kernel boot stub", "kind": "source" },
    { "path": "native_kernel_trap_stub.tasm", "label": "Native trap stub", "kind": "source" },
    { "path": "executable_header_v2.h", "label": "Executable header contract", "kind": "source" },
    { "path": "IMAGE_FORMAT_MANIFEST.json", "label": "Boot image format manifest", "kind": "manifest" }
  ],
  "evidence": [
    { "path": "tests/test_formats.cpp", "label": "Image format tests", "kind": "test" },
    { "path": "tests/test_host_runtime.cpp", "label": "Host boot/runtime tests", "kind": "test" }
  ],
  "next": "kernel-syscalls",
  "interactive": {
    "kind": "choice",
    "title": "Trap check",
    "prompt": "What is the correct response to the invalid lane pair 11 in an instruction word?",
    "options": ["Decode it as +1", "Ignore the instruction", "Raise the illegal-operation trap"],
    "answer": 2,
    "explanation": "Reserved encodings become observable trap behavior. Silent coercion would hide a malformed image or a compiler/assembler bug."
  }
}
---

## The handoff

Compilation produces an executable image; the bootloader validates its header,
loads its sections, and transfers control to the kernel or program entry. The
`.tboot` and `.tdisk` formats are contracts, not opaque blobs.

## Traps make failure inspectable

An invalid instruction encoding, malformed lane, or forbidden memory operation
must become a recorded trap. The VM and native trap stubs provide a controlled
boundary for diagnostics and recovery. A test that only checks the happy path
would not establish this contract.

## Prerequisites

Know the compiler output and the 27-trit instruction shape. Then read the image
manifest beside the loader and trap sources.

## Next step

Continue to **Kernel syscalls** to see how valid execution crosses from a user
program into OS services.
