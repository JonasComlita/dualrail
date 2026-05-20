---

## True CPU/GPU Unification — Triton-27 Unified Execution Model

In conventional architectures, CPU and GPU integration is a packaging decision, not a design one. Even Apple Silicon — the best current example — keeps separate ISAs, separate schedulers, and separate compiler toolchains. The unified memory pool eliminates PCIe transfer overhead, but the software divide remains: you still write application code in one language and shader/compute kernels in another, compiled by different toolchains targeting different execution models.

The Triton-27 eliminates this at the ISA level. There is one instruction set, one compiler, and one execution model for both sequential and parallel workloads.

---

### Pipeline Structure

The 5-stage pipeline has fully shared Fetch, Decode, and Write-Back stages. Within the Execute stage, two parallel tracks operate side by side:

**Scalar track** — sequential operations, address calculation, OS bookkeeping, using r1–r24.

**Vector track** — 27 parallel execution lanes, operating on v0–v7 vector registers.

Both tracks share the same physical register file. A scalar register and a vector lane are not different memory structures with a transfer cost between them — they are addresses into the same file. This is the architectural basis for zero-copy streaming: data loaded for scalar processing is immediately available to the vector track with no move instruction.

One important caveat: the two tracks have different execution latencies. The vector multiplier in particular is a multi-cycle unit. The decode stage must implement dual-track hazard detection — tracking in-flight latencies for both the scalar ALU and the vector unit simultaneously, and stalling or forwarding accordingly. This is non-trivial but well-understood; it is the same problem solved by any out-of-order scalar pipeline, applied to two parallel latency domains.

---

### Sub-Word Packing and Memory Efficiency

The 27-trit word width was chosen to divide cleanly into three 9-trit sub-words. A 9-trit balanced ternary value covers the range [−9,841, +9,841], which accommodates standard-precision spatial coordinates, color channel values, and weight parameters without truncation.

The practical consequence: a single 9-word memory load delivers 27 distinct sub-word elements directly into a vector register in one memory tick. The very next cycle, the vector track can execute a parallel accumulation or transform on all 27 elements in place, inside the L1 cache line, without any shuffle or gather instruction.

This is a genuine architectural advantage that falls out of the word-width choice naturally, not as an afterthought.

---

### Lane-Local Fault Model

This is the strongest argument for unification and deserves to be understood precisely.

In a conventional system, if a GPU thread hits an unmapped page during a parallel compute pass, there is no graceful recovery path. The GPU cannot service its own page fault — it has to asynchronously interrupt the CPU, which then runs a software fault handler, maps the page, and signals the GPU to retry. This round-trip crosses an ISA boundary, a scheduler boundary, and usually a memory bus. It is slow enough that most GPU runtime systems simply treat page faults as fatal context losses.

Because the Triton-27's vector track lives inside the primary core pipeline, it uses the same trap mechanism as scalar code. Each vector register carries a per-lane `fault_valid` and `fault_class` field. If lane 14 hits an unmapped address during a parallel pass, it records the fault locally in ternary status encoding and the pipeline triggers the standard `csrrw sp, scratch, sp` trap entry — the identical path used by a scalar null-pointer dereference. The kernel, already sharing the same address space and page table, services the fault through the buffer pool, maps the page, and resumes the vector pipeline. No context loss, no cross-ISA boundary, no asynchronous notification loop.

This is not just a performance improvement. It makes vector code as debuggable and fault-recoverable as scalar code, which is categorically not true of any current GPU architecture.

---

### Single Compiler Target

TCL 1.0's trit-width polymorphism maps directly onto this hardware model. A width-parametric function:

```
fn apply_transform<W: TritWidth>(data: T<W>) -> T<W>
```

compiles to scalar instructions when instantiated at narrow widths and to 27-lane vector instructions when instantiated at the full word width. The compiler's monomorphisation pass handles the instantiation; the programmer writes one function. The OS kernel, the VFS data plane, the transaction logger, and any rendering or signal-processing code are all written in the same language, compiled by the same pipeline, and executed on the same silicon.

---

### Honest Performance Characterisation

The Triton-27 vector unit will not outperform a high-end discrete GPU on throughput-bound workloads. A 27-lane vector unit is not competitive with thousands of shader cores on tasks like large matrix multiplication or rasterisation at high resolution. That is not the claim.

The honest characterisation is:

- **Performance-per-area and performance-per-watt** at the Triton-27's scale are strong, because the vector unit shares decode, fetch, cache, and fault infrastructure with the scalar core rather than duplicating it.
- **Software complexity** is dramatically lower. There is no SPIR-V, no CUDA, no Metal, no driver translation layer, no separate shader compiler. A single toolchain covers the full compute range.
- **Fault recovery and debuggability** are categorically better than any current GPU architecture for the reasons described above.
- **Latency-sensitive parallel workloads** — signal processing, physics simulation, cryptographic batch operations, database scans — are well served by 27 wide lanes with zero dispatch overhead.

The right framing is not that binary architectures cannot compete. It is that this architecture achieves a level of stack coherence — from silicon to language to OS — that fragmented binary stacks structurally cannot reach at any price point.

---

### Open Engineering Items

Two things must be resolved before the vector track can be finalised in RTL:

**Memory bandwidth budget.** 27 parallel lanes can execute faster than the memory system can feed them. The L1 data cache port width must be specified: can it deliver 27 × 27-trit elements per cycle? If not, the vector track stalls on memory regardless of ALU count. The cache port width and the vector lane count must be co-designed, not decided independently.

**Dual-track hazard table.** The decode stage hazard logic must be extended to track in-flight operations across both the scalar ALU and the vector co-unit simultaneously. Write this into the pipeline specification before RTL begins, or the first integration will produce subtle forwarding bugs that are very difficult to reproduce.