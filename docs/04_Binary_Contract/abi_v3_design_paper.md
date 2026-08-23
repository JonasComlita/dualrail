# Executable and Function ABI v3

## Design, architecture, and the case for replacing ABI v2

**Status:** Design paper and implementation rationale  
**Date:** 2026-08-20  
**Scope:** Trit compiler function boundaries, executable image envelopes, vector state, and migration compatibility

## Abstract

Trit ABI v3 is a versioned boundary for values and execution state that v2
could not represent safely. It adds a defined structure-return convention,
first-class vector argument and return rules, exact vector spill geometry, a
versioned executable header, and process-owned vector context that survives
traps and scheduling. It does this while retaining the v2 instruction codec
and a v2 loader path.

The important design decision is therefore not to discard v2 binaries. It is
to replace v2 as the active release profile while keeping v2 readable and
explicitly selectable. This gives existing scalar applications a stable
migration path while making new capabilities impossible to express without a
complete contract.

## 1. Version terminology

“v3” names a coordinated boundary, not a wholesale rewrite of the ISA. The
current implementation has several intentionally independent version axes:

| Layer | v2 behavior | v3 behavior |
| --- | --- | --- |
| Instruction encoding | Trit ISA/codec v2 | Still uses the v2 codec |
| Executable envelope | 15-word `ExecutableImageHeaderV2` | 20-word `ExecutableImageHeaderV3` |
| Compiler function ABI | `trit.compiler.function-abi.v2` | `trit.compiler.function-abi.v3` |
| Syscall ABI | v2 | Remains v2 |
| Vector function ABI | No boundary | Vector ABI 1, fixed VLEN 27 |
| Process context | 32-word scalar task record | Separate 279-word vector context |
| `.tboot` container | Existing container contract | Container version remains distinct from executable ABI |

This separation prevents a common migration error: changing the executable
function boundary does not imply that instruction encodings, syscall IDs, or
the boot-container format have changed.

## 2. What ABI v2 provided

ABI v2 was a coherent scalar contract. Its register model has a zero register,
callee-saved scalar registers, caller-saved scratch registers, a link register
(`r25`), a downward-growing stack pointer (`r26`), and a trap register. Scalar
arguments begin at `r13`; the register/stack cursor advances according to the
value width, with additional arguments placed in caller-owned outgoing stack
storage. T40 returns use `r13`; a wide T50 return uses the ABI pair `r13-r14`.

Aggregate parameters already had a deliberately conservative representation:
each struct or array crossed the boundary as one caller-owned pointer word.
The callee then performed word-wise loads and stores through that pointer. This
avoided inventing an implicit field-splitting rule, but it applied only to
parameters.

The v2 executable header records the common image identity, ISA and syscall
versions, required features, entry point, text/data/stack sizes, scalar width,
page geometry, flags, and a checksum. A v2 task context stores scalar control
state, address-space information, and scalar registers in 32 words.

For scalar programs, these choices are sufficient. The problem is that v2's
contract ends exactly where modern compiler and VM features need a boundary.

## 3. The representational faults of v2

The following are not merely missing conveniences. Each is a boundary where a
caller and callee could disagree about ownership, width, state, or recovery.

### 3.1 Aggregate returns had no ABI representation

V2 defines how an aggregate is passed as a pointer, but not how an aggregate
is returned. A compiler cannot safely put a struct in `r13`: a struct may span
multiple words, may contain mixed widths, and may require caller-owned storage
whose lifetime extends beyond the call. Returning only its first word would
silently truncate the value; inventing a private register convention would
make separately compiled objects incompatible.

The v2 compiler therefore rejects aggregate-valued returns. That fail-closed
diagnostic is correct, but it also means ordinary compositions such as
`make_pair()` followed by a caller-side field access cannot be expressed as a
normal function call.

### 3.2 Vector registers existed without a function boundary

The VM and ISA had vector registers, vector operations, lane faults, an
accumulator, and a configurable vector length. V2 did not specify any of the
rules needed when a vector crosses a call:

- Which registers carry vector arguments and returns?
- Are vector registers caller- or callee-saved?
- What happens to the accumulator and lane-fault state?
- How are vector values passed after the register set is exhausted?
- How large and how aligned is a vector spill?
- Which VLEN must a callee restore?

Without those answers, accepting a `vec<T>` parameter would create an ABI that
worked only inside one compiler invocation. Scalarizing the vector would lose
lane tags and width semantics; passing a scalar pointer would change the
source-level contract. V2 therefore rejects first-class vector boundaries
before target emission.

### 3.3 The executable header could not describe vector requirements

The 15-word v2 header has no fields for vector register count, lane count,
lane-word geometry, vector spill size, or process-context size. A loader could
not distinguish a scalar image from an image that requires eight vector
registers and exact tagged state. A feature bit alone would be insufficient:
the runtime must validate the geometry, not merely learn that “vectors exist.”

### 3.4 The scalar task record could not preserve vector execution

The v2 task record is intentionally 32 words and preserves scalar control
state. It cannot hold eight registers × 27 tagged lanes, lane-fault validity
and classes, the accumulator, VLEN, and context metadata. Saving vector lanes
through scalar conversions would lose tags and could change the result after a
timer interrupt or process switch.

This is a correctness fault, not just a performance issue. A process resumed
after preemption could observe another process's vector state or an altered
lane fault. Fork, exec, and task-slot reuse would have the same ambiguity.

### 3.5 Version checks were too coarse for the new state

V2 validation can establish scalar image compatibility, but it cannot prove
that a runtime supports the exact vector geometry required by a program. The
v3 boundary therefore needed explicit profile identity and feature/geometry
validation, with mixed-object links rejected before an image is produced.

## 4. V3 design goals

V3 was designed around five invariants:

1. **Every cross-function value has one unambiguous representation.**
2. **Every process-owned execution state has an exact save/restore format.**
3. **The image declares the geometry it requires, and the loader verifies it.**
4. **Unsupported combinations fail before code emission or image installation.**
5. **Existing v2 scalar binaries remain usable during migration.**

These goals explain why v3 is a coordinated contract rather than a collection
of independent compiler flags.

## 5. Function ABI v3

### 5.1 Aggregate returns use a hidden `sret` pointer

When a function returns a struct or array, the caller allocates result storage
and passes a hidden caller-owned structure-return pointer as the first ABI word.
The callee copies the aggregate into that storage and returns without a scalar
payload in `r13`.

Conceptually:

```text
caller:  allocate result storage
         pass sret pointer, then user arguments
         call callee
         read aggregate from the caller-owned storage

callee:  receive sret pointer as ABI word 0
         construct/copy the aggregate into that address
         return normally
```

The hidden word advances the same register/stack cursor as any other ABI word.
This makes nested aggregate calls and mixtures of aggregate pointers, scalar
values, T50 pairs, and stack arguments deterministic. The caller owns the
storage lifetime, so the convention does not depend on callee-local buffers.

### 5.2 Vector register and stack rules

V3 fixes the vector boundary to vector ABI 1:

- Eight vector registers, `v0` through `v7`.
- `v0` through `v3` carry the first four vector arguments.
- `v0` carries a vector return value.
- `v4` through `v7` are temporary vector registers.
- All vector registers, the accumulator, and lane-fault state are
  caller-saved.
- VLEN is callee-preserved and must be 27 at ABI entry and exit.
- Additional vector arguments use 27-word, 9-word-aligned outgoing stack
  slots and share the scalar/T50/aggregate stack cursor.
- A vector spill occupies 27 words and is aligned to 9 words.

The first four vector arguments use a separate vector register class; scalar
and vector cursors do not accidentally overwrite one another. Once vector
arguments move to the stack, their exact width and alignment are visible to
both caller and callee.

Vector syscalls, atomics, and foreign-function interfaces remain unsupported.
They are rejected because a complete vector function boundary does not by
itself define a representation for kernel or external ABI calls.

### 5.3 Compiler lowering

The compiler records the selected function contract and boundary metadata in
the object. Under v3 it lowers:

- hidden `sret` parameters and caller scratch areas;
- vector parameter and return locations;
- vector parallel moves and cycle breaking;
- 27-word typed vector spills;
- vector values live across calls;
- vector operations represented by the supported SSA and ISA subset.

Under v2, the same source shapes continue to fail closed with diagnostics that
name the missing ABI representation. There is no AST replay or silent
scalarization fallback.

## 6. The v3 executable envelope

`ExecutableImageHeaderV3` retains the common v2 fields and appends the exact
vector geometry:

| Field | V3 value or meaning |
| --- | --- |
| Header size | 20 words |
| Executable version | 3 |
| Function ABI | 3 |
| ISA encoding | v2 |
| Syscall ABI | v2 |
| Vector registers | 8 |
| Vector lanes | 27 |
| Lane word width | 40 trits (one architectural T40 word) |
| Vector context | 279 words |
| Vector spill | 27 words |

The repeated number 27 describes `VLEN`, the lane count, and therefore the
number of T40 words in a vector spill. It does not describe each lane's width.
Keeping every lane at T40 preserves the native scalar contract and the explicit
design goal of a ternary equivalent of a 64-bit computer; instruction width
remains the separate T27 contract.

Required feature bits identify the v3 profile, vector geometry, vector context,
and vector spilling. The checksum covers both the common fields and the v3
extensions. A v2 decoder fails closed on a v3 envelope; a v3 decoder rejects
wrong geometry, unknown feature bits, mismatched versions, and invalid checksums.

The v3 envelope intentionally rides on the v2 instruction encoding. This keeps
the instruction stream stable while making the new function and process
contracts explicit. It also means v3 is not an excuse to silently reinterpret
old opcodes.

## 7. Exact vector process context

V3 keeps the historical 32-word scalar task record and allocates a separate
279-word vector context. The context is versioned and laid out as follows:

| Range | Contents |
| ---: | --- |
| 0–4 | format version, length, vector ABI, register count, VLEN |
| 5–6 | tagged accumulator and first failing lane |
| 7–33 | one encoded fault record per lane |
| 34–249 | `8 × 27` tagged vector lanes |
| 250–278 | reserved zero words |

`VCTXSTORE` and `VCTXLOAD` are privileged v3 context operations. They first
validate privilege, feature support, alignment, span, version, geometry, and
all encoded values. Restore uses a read/validate phase followed by one mutation
phase, so a malformed context cannot partially overwrite live state.

The kernel and native task layer own one vector context per process. It is
initialized on first entry, preserved across timer and syscall switches,
copied or reset across fork and exec, and released on exit or task-slot reuse.
This ownership model is what turns vector execution from a VM-local feature into
a schedulable process state.

## 8. Loader, linker, and compatibility architecture

The compiler, linker, assembler, image builder, loader, kernel, process
metadata, and diagnostics all carry the selected profile. Linkers reject mixed
v2/v3 object profiles and reject a v3 function contract paired with a v2
executable envelope. Loaders reject v3 images when the required vector context
feature or geometry is unavailable.

Compatibility is deliberately asymmetric:

- v2 executables remain readable and boot unchanged;
- v3 executables require the v3 runtime contract;
- v2 payloads are not silently rewritten into v3;
- a scalar-compatible offline migration may re-envelope preserved inputs, but
  it cannot claim vector compatibility without v3 metadata and runtime support.

The release image path selects v3 by default, while explicit v2 selection
remains available for legacy applications and migration fixtures.

## 9. Major design decisions and their rationale

### Fixed VLEN 27

A variable VLEN would require dynamic stack slots, dynamic context sizes, and
more complicated save/restore rules. V3 chooses the architectural lane count
already exercised by the VM: 27 lanes, 27-word spills, and a fixed 279-word
context. The result is easy to validate and deterministic across processes.

### Separate vector context instead of enlarging the scalar record

The 32-word v2 scalar record is a compatibility surface. Enlarging it would
change trap-stub offsets and every consumer that assumes the v2 layout. A
separate exact-width arena keeps scalar trap compatibility while allowing
tagged vector state to evolve as its own versioned object.

### Caller-saved vectors, callee-saved VLEN

Vector calls can be frequent and vector registers are a scratch-heavy resource,
so v3 makes the registers, accumulator, and lane faults caller-saved. VLEN is
different: it changes the meaning of all subsequent vector instructions and
must be restored by a callee that changes it. This split gives the compiler a
simple call-live spill rule without allowing a callee to leak a changed lane
geometry to its caller.

### Explicit feature and geometry bits

A generic “vector supported” bit cannot prove that the runtime has eight
registers, 27 lanes, 27-word spills, and a 279-word context. V3 records and
validates each required dimension so an image is rejected before execution,
not after a state-corrupting trap.

### Fail closed for unsupported boundaries

The compiler must reject vector literals outside the supported source/SSA
surface, vector syscalls, atomics, foreign calls, unsupported element types,
and ABI mismatches. A precise rejection is safer than generating a binary whose
caller and callee merely happen to agree in one build.

## 10. Evidence and verification

The v3 contract is exercised by focused tests rather than only metadata checks.
The executable ABI suite covers:

- v2/v3 header round trips, checksums, geometry, and feature rejection;
- exact 279-word context initialization, save, restore, and malformed-context
  atomicity;
- privileged versus user-mode `VCTXSTORE`/`VCTXLOAD` behavior;
- compiler opt-in, vector allocation, 27-word spills, vector calls, stack
  arguments, vector returns, and operation execution;
- package, process, fork, exec, scheduler-switch, and task-reuse identity.

The compiler ABI suite covers:

- v2 aggregate-pointer parameters across registers and stack;
- v3 hidden `sret` lowering and nested/shifted aggregate calls;
- object metadata and linker profile matching;
- explicit v2 aggregate-return and vector-boundary diagnostics.

The acceptance criterion is not “a v3 header can be serialized.” It is that a
v3 value can cross a compiler boundary, survive a process switch with its tags
intact, be validated by the loader, and produce the same result after launch.

## 11. Migration guidance

Application and toolchain owners should migrate in this order:

1. Keep scalar applications on v2 until they need aggregate returns or vectors.
2. Compile new aggregate-returning code with function ABI v3 and verify the
   hidden `sret` behavior in object metadata.
3. Enable vector ABI v3 only when the target executable is also v3 and the
   runtime advertises VECTOR_CONTEXT.
4. Test package/image identity and process handoff, not just compiler output.
5. Use explicit v2 selection for legacy images that must remain byte- or
   behavior-compatible.

No application should infer v3 from the presence of vector instructions alone.
The executable profile, function profile, required feature bits, and runtime
context support must agree.

## 12. Conclusion

V2 was a sound scalar baseline, but it had no legal representation for
aggregate returns, no function-level vector contract, no vector spill geometry,
no executable declaration of vector requirements, and no process context that
could preserve vector state. Extending it informally would have created hidden
conventions, silent truncation, or state corruption across calls and
preemption.

V3 replaces v2 as the active release boundary by making those dimensions
explicit: hidden caller-owned `sret`, fixed vector registers and stack rules,
exact tagged context, versioned image geometry, feature negotiation, and
fail-closed validation. It does not discard the working scalar ecosystem. V2
remains readable and selectable, while v3 provides the only coherent path for
aggregate returns and first-class vector execution.

### Related implementation references

- [Calling Convention and ABI](abi_spec.md)
- [Vector ABI Specification](vector_abi.md)
- [Architecture v2 Contract](architecture_v2.md)
- [Executable ABI v3 and vector-boundary tests](../../tests/test_executable_abi_v3.cpp)
- [Compiler ABI contract tests](../../tests/test_compiler_abi_contract.cpp)
