# Phase F: Ternary Acceleration Layer
## Post-Self-Hosting & Bare-Metal Deployment

**Status:** Planned. Do not begin until Phase E (self-hosting) is complete.
**Depends on:** Buffer pool manager, WAL, relational kernel store, COW fork, native `.trit` compiler.

---

## Why This Waits

The compiler is not yet self-hosting. The CPU execution model is still transitioning to a relational kernel design. The WAL and buffer pool manager are not yet implemented. Beginning acceleration work now would mean designing an API against a kernel that will change substantially before the API can be used.

The dependency chain is real and cannot be shortcut:

```
Phase E: Self-Hosting Complete
         ↓
Phase F1: MMIO, DMA, and Page Pinning Kernel Primitives
         ↓
Phase F2: Ternary Bus Protocol and Wire Format
         ↓
Phase F3: Data-Parallel Compiler Backend (tritc targeting wide execution units)
         ↓
Phase F4: Unified Stateless Compute API
```

---

## Phase F1: Hardware Abstraction (The Kernel Plane)

Before any API design, the kernel needs mechanical plumbing to talk to an external accelerator.

### DMA and Page Pinning

The buffer pool manager from Phase D must learn to pin pages in physical memory so an external execution unit can access them via DMA without CPU intervention. This is harder than it sounds, and the interactions are non-trivial.

Pinning a page means the page allocator must know the page cannot be evicted from the buffer pool while the accelerator holds a reference. This requires a pin count field in each page's metadata — an extension to the buffer pool page header struct:

```
struct PageHeader {
    ppn:        T40,
    pin_count:  T40,     // >0: page is DMA-pinned, cannot evict
    dirty:      T1,
    version:    T40,
    lru_clock:  T40,
    namespace:  T40,
}
```

Three interactions with existing kernel subsystems must be resolved before F1 is complete:

**COW interaction.** The COW fault handler remaps pages on write. A DMA-pinned page cannot be remapped while the accelerator holds a physical address reference to it. The COW handler must check `pin_count` before remapping and block or return an error if the page is pinned.

**Fork interaction.** `fork()` copies page table entries. If a parent has pinned DMA pages, the child's address space cannot safely share those physical pages with an in-flight DMA operation. Fork must either refuse to copy pinned pages into the child, or wait for all DMA operations on those pages to complete before proceeding.

**MVCC version chain interaction.** The buffer pool's version chain allows readers to hold references to old page versions. A DMA operation pinning a page for write must ensure no reader holds a reference to a version that will be overwritten. This is the same acquire-release problem as `shared<T, ACQ_REL>` in the type system — and should use the same `TLDR`/`TSTR` primitives to coordinate.

These are not afterthoughts. DMA page pinning is the hardest part of F1 and the one most likely to expose gaps in the kernel design.

### MMIO Mapping

MMIO regions for the accelerator are mapped into kernel virtual address space with PTEs marked execute=false, write=true, and a new `mmio` permission trit that prevents the page from being included in snapshots or COW copies. CSR-mapped control registers on the FPGA accelerator are accessed through this region.

---

## Phase F2: The Ternary Bus Protocol

This phase has no equivalent in the original document. It is the question on which the ternary advantage is either real or illusory for the acceleration path, and it must be answered before F3 begins.

Every existing GPU and FPGA accelerator speaks binary — 32-bit or 64-bit words over PCIe. A ternary compute unit attached to this system has two options:

**Option A: Binary bridge.** The host serializes ternary values into binary words for DMA transfer, and the accelerator deserializes them back. This works. It requires no new hardware. It also entirely discards the claimed advantage of higher data density per wire and makes every DMA transfer carry encoding overhead. The "balanced ternary handles signs implicitly" advantage disappears the moment the values cross the DMA bus in binary encoding.

**Option B: Native ternary bus.** DMA transfers use the 2-bits-per-trit encoding natively. The accelerator speaks the same format as the VM's `TritLane` types. This preserves the data density advantage and eliminates the encoding round-trip.

Option B requires hardware support — either a custom FPGA configuration or, eventually, an ASIC. Option A is implementable immediately in software but undermines the platform's value proposition for compute-intensive workloads.

The decision between these options must be made in Phase F2, before the compiler backend in F3 makes assumptions about the wire format. Documenting this as a question to resolve at the start of F2 is the right way to ensure it isn't discovered mid-implementation.

---

## Phase F3: Data-Parallel Compiler Backend

The original framing compared this to CUDA and suggested that TCL's width-parametric functions (`fn f<W: TritWidth>`) are "already architecturally optimized" for this purpose. This is imprecise and should be corrected before it influences the design.

### What Width Polymorphism Actually Provides

Width-parametric functions solve scalar precision specialization — the compiler instantiates one copy of `fn dot<W: TritWidth>` per concrete width at call sites. This is monomorphization over numeric precision.

What data-parallel acceleration requires is a different abstraction: expressing that the same function runs simultaneously across N independent data lanes with no communication between them between the start and end of the dispatch. That is not what width polymorphism expresses.

### What the Ternary ISA Already Provides

The vector ISA that already exists in the architecture is the right starting point. The instructions `VADD`, `VSUB`, `VMUL`, `VDIV`, `VCMP`, `VDOT`, `VMAC`, `VACT`, `VLOAD`, `VSTORE`, `VPERMUTE`, `VGATHER`, `VSCATTER` across 27-lane vector registers already express data-parallel operations at the ISA level. The default vector length of 27 lanes is not accidental — it is the ternary cube.

The compiler backend for acceleration is therefore not a new language feature. It is a new instruction selection pass that:

1. Identifies loops where iterations are provably independent (no loop-carried dependencies, no aliasing between iterations as proven by the borrow checker)
2. Maps those loops to vector instruction sequences
3. Emits `VLOAD`/`VSTORE` for the memory access pattern
4. Emits the appropriate vector arithmetic instructions for the body
5. Emits `VDOT`/`VMAC` for reduction patterns

For a wide execution unit beyond 27 lanes, this becomes a tiling problem: the compiler tiles the iteration space into 27-lane chunks, emits one vector instruction sequence per tile, and lets the hardware pipeline them. This is loop tiling from the 1990s compiler literature, applied to ternary vector widths.

### The Compiler Extension

The TCL language extension needed for this is minimal. One annotation:

```
#[parallel]
fn map_kernel<W: TritWidth>(data: borrow<[T<W>]>, out: borrow_mut<[T<W>]>) -> void {
    // body executes independently per element
    // compiler verifies: no cross-element dependencies
    // codegen: emit as vector instruction sequence
}
```

The `#[parallel]` annotation is a contract from the programmer that iterations are independent. The compiler verifies the contract by checking that the loop body contains no borrows that alias across iteration boundaries (already provable from the ownership system) and no calls to non-parallel functions. If the contract holds, the compiler selects vector instructions. If it fails, the compiler reports an error rather than silently falling back to scalar code.

---

## Phase F4: Unified Stateless Compute API

The original document correctly identifies that the binary world's graphics/compute split was a historical accident. OpenGL and OpenCL designed separately, converged painfully into Vulkan and WebGPU. The ternary OS should not repeat this.

### The Relational Connection

The original document treats this phase as a separate invention. It is not. The stateless descriptor model falls directly out of the relational kernel design established in Phase D.

A compute pipeline descriptor is a row in a `compute_pipelines` kernel relation:

```
struct ComputePipeline {
    id:           T40,
    shader_inode: T40,       // path to compiled .trit kernel
    namespace:    T40,
    input_layout: T40,       // buffer schema descriptor
    output_layout: T40,
    vector_width: T40,       // lanes per dispatch
}
```

A compute dispatch is an insert into a `work_queue` kernel relation:

```
struct WorkItem {
    pipeline_id:  T40,
    input_ppn:    T40,       // physical page number, DMA-pinned
    output_ppn:   T40,
    element_count: T40,
    completion_channel: T40, // ring buffer address for completion signal
}
```

The scheduler reconciliation loop — the same loop that manages processes — drains the work queue and dispatches items to the execution unit via MMIO. Completion signals arrive via the `completion_channel` ring buffer, which is the same `ring_new`/`ring_write`/`ring_read` primitive from `ulib.trit`.

There is no separate graphics API. A render pass is a compute dispatch over a pixel buffer. A shader is a `#[parallel]` TCL function. The framebuffer is a pinned DMA page. The display controller reads from that page via MMIO.

### What This Replaces

| Binary World | Ternary OS Equivalent |
|---|---|
| Vulkan pipeline object | Row in `compute_pipelines` relation |
| Vulkan descriptor set | Borrow of a buffer-pool-pinned page |
| CUDA kernel launch | Insert into `work_queue` relation |
| OpenGL framebuffer | DMA-pinned page read by display MMIO |
| `poll()`/`epoll()`/`kqueue()` for completion | `ring_read()` on completion channel |
| Graphics vs. Compute split | One API, different pipeline rows |

The reconciliation loop handles scheduling, retry on resource exhaustion, and completion signaling. The programmer declares what compute should happen. The kernel makes it happen. This is the Kubernetes insight applied to compute dispatch.

---

## The Ternary Advantage: Where It Is Real

The original document claims balanced ternary gives a "massive mathematical edge" in parallel computing. This is true in specific places and false in others. Precision matters.

**Real advantage — MAC operations.** The `VMAC` instruction accumulates a dot product of two L1 vectors into the T40 accumulator in a single instruction. Binary neural network accelerators spend significant silicon on sign handling and two's complement negation. Balanced ternary's {-1, 0, +1} weight representation means multiply is a conditional negate or zero — no full multiplier required for T1 weights. This is directly useful for ternary neural network inference.

**Real advantage — three-valued predication.** The `VCMP` instruction produces an L1 predicate vector in one pass. Binary SIMD requires a separate mask register and a blend operation. Ternary predication (`neg`/`zero`/`pos`) eliminates the blend for three-way comparisons.

**Real advantage — data density on the ternary bus (Option B only).** If the native ternary wire protocol is implemented, 27 trits fit in 54 bits rather than the 27 bytes a binary system would use for 27 separate 8-bit values. This is real. It requires Option B from Phase F2.

**Not an advantage — the sign bit claim.** The original document says binary GPUs "spend enormous silicon dealing with sign bits and 2's complement conversions." Modern binary FPUs handle sign bits in one trit-equivalent operation. This is not a meaningful source of overhead in contemporary hardware. The claim overstates the advantage.

**Unknown — pipeline latency.** Balanced ternary arithmetic requires carry propagation in base 3 rather than base 2. Whether this is faster or slower than binary depends on the FPGA or ASIC implementation. This is an open empirical question, not a theoretical advantage.

---

## Summary:

1. The buffer pool page header must include a `pin_count` field from the start. Retrofitting this after Phase D is complete is expensive.
2. The PTE layout must reserve a trit for the `mmio` permission flag.
3. The ternary bus protocol question (Option A vs Option B) must be the first decision made when Phase F1 begins.
4. The `#[parallel]` annotation design should be noted in the TCL spec as a planned 1.1 extension so the borrow checker is not designed in a way that makes independence verification impossible later.
5. The completion channel mechanism (ring buffer from `ulib.trit`) is already implemented. No new primitive is needed for async dispatch signaling.