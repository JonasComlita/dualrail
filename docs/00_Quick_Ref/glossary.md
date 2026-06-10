# Glossary

Definitions for terms used throughout the Trit-Stack codebase.

---

## Core Terms

**Balanced Ternary**
A number system using three digit values: −1, 0, +1 (written T, 0, 1 or NEG, ZER, POS). Unlike binary (0/1), this system is symmetric around zero — no separate sign bit needed.

**Trit**
The ternary equivalent of a binary bit. One trit holds one of three values: {−1, 0, +1}.

**Tryte**
Three trits. Analogous to a byte (3 bits in binary). Rarely used in this codebase — `Word` is more common.

**Word**
27 trits. The native instruction word size of the ISA. 27 = 3^3, a power of three.

**T_NEG / T_ZER / T_POS**
Symbolic constants for the three trit values: `T_NEG = -1`, `T_ZER = 0`, `T_POS = +1`.

---

## Type System

**T1**
1-trit signed integer. Values: {−1, 0, +1}. Stored as `uint8_t` in positional form.

**T5**
5-trit signed integer. Range: [−121, +121]. Stored as `uint8_t` (243 states < 256).

**T10**
10-trit floating-point. 6-trit mantissa, 4-trit exponent. Range: ±3^40. Stored as `uint16_t`. Alias for `TernaryScalar<10>`.

**T20**
20-trit floating-point. 14-trit mantissa, 6-trit exponent. Stored as `uint32_t`. Alias for `TernaryScalar<20>`.

**Triple / T40**
40-trit floating-point. 33-trit mantissa, 7-trit exponent. The **native word size** of the ISA. Stored in a single `uint64_t` (3^40 < 2^64). Alias for `TernaryScalar<40>`.

**LongTriple / T50**
50-trit floating-point. 41-trit mantissa, 9-trit exponent. The widest standard type. Stored in `UInt128`. Alias for `TernaryScalar<50>`.

**TernaryScalar\<N\>**
Generic template for all ternary scalar types. Uses positional base-3 storage.

**TritLane\<N\>**
SIMD transport type. N trits packed as 2 bits each. Used for GPU/vector operations. NOT the same encoding as `TernaryScalar`.

---

## Instruction Format Terms

**R-type**
Register-register instruction. Format discriminant trit[26] = +1. Fields: opcode(4), Rd(3), Rs1(3), Rs2(3), func(3), pad(10).

**I-type**
Immediate instruction. Format discriminant trit[26] = 0. Fields: opcode(4), Rd(3), Rs1(3), imm16(16).

**B-type**
Branch/jump instruction. Format discriminant trit[26] = −1. Fields: opcode(4), Rs(3), offset19(19).

**R4-type**
Extended register format for TWCMP and TCLAMP. Uses 4 source registers.

**R5-type**
Extended register format for TSEL and VSEL. Uses a condition register + 3 source registers (neg/zero/pos).

**func field**
3-trit field in R-type instructions that selects width (T1–T50, L1–L50) or atomic ordering.

**imm16**
16-trit signed immediate. Range: ±21,523,360.

**offset19**
19-trit signed branch offset. Range: ±581,130,733.

---

## VM & Runtime Terms

**VMState**
The complete state of the virtual machine: register file, IMEM, DMEM, CSR file, vector register file, AI accumulator, PC, privilege mode.

**IMEM / DMEM**
Instruction Memory and Data Memory. Separate address spaces. Word-addressed.

**TernaryValue**
A tagged union of all ternary types (T1–T50, L1–L50) plus a mode tag. The VM's runtime value representation.

**TernaryMode**
Enum selecting the active width: T1, T5, T10, T20, T40, T50, L1, L5, L10, L20, L40, L50.

**Lane mode vs Numeric mode**
`isNumericMode()` → positional floating-point value (arithmetic applies). `isLaneMode()` → 2-bit-per-trit packed wire value (only bitwise operations apply).

**Accumulator**
A special internal register for dot-product accumulation. Cleared by ACLR, loaded by ALOAD, written by AADD/ASUB/AMUL, read by ASTORE.

---

## OS / Kernel Terms

**TCL**
Ternary C-Like — the high-level language used for kernel, apps, and libraries. Source files use the `.trit` extension.

**Syscall**
A kernel service invoked by `SYSCALL` opcode after writing a service ID to `CSR syscall_id`. Arguments in r13–r16, result in r13 (status), r14 (payload), r15 (detail).

**Privilege mode**
Kernel = T_NEG (most privileged), Supervisor = T_ZER, User = T_POS (least privileged). Set in `status` CSR. `ERET` restores mode.

**EPC**
Exception Program Counter. The PC value saved by the hardware when a trap occurs.

**TVEC**
Trap Vector. The address the VM jumps to when a trap fires.

**VFS**
Virtual Filesystem. Inode-based filesystem with path lookup, file open/read/write, directory listing.

**IPC**
Inter-Process Communication. Syscalls: `sys_ipc_send`, `sys_ipc_recv`, `sys_ipc_recv_blocking`.

---

## Image Format Terms

**.tboot**
TernaryOS boot image file. Contains the kernel, all apps, and a rootfs image. Magic: `0x31544f4f424f5354`.

**.tdisk**
TernaryOS virtual disk image. Sparse block format. Magic: `0x54524954535031`. Block size: 27 words.

**rootfs**
Root filesystem embedded in a `.tboot` image as a `rootfs_words` array (length must be a multiple of 27).

---

## Build & Tool Terms

**trit_tool.py**
The master agent tool. Subcommands: `doctor`, `test`, `export-diagnostics`, `build-image`, `inspect-image`, `run`, `bench`.

**trit-test.ps1**
PowerShell test runner. Usage: `trit-test.ps1 smoke | os | production`

**stage_tos_release / smoke_tos_release**
CMake targets to build and smoke-test the production `.tboot` image.

**T40 word**
The native machine word: a 40-trit floating-point number fitting in one `uint64_t`. The VM's default register width.
