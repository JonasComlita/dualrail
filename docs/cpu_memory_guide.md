# Pocket Guide: Understanding CPU Internals (CSRs, PTEs, and TLBs)

Welcome to the low-level systems club! If you've been working on microkernels and virtual memory today, you've just crossed paths with three of the most crucial hardware-software boundaries in computer architecture: **CSRs**, **PTEs**, and **TLBs**.

This cheat sheet uses simple analogies, structural details, and direct links to our **Ternary OS3 Microkernel** code so you can lock these terms into your long-term memory.

---

## 🗺️ High-Level Map: How They Relate
Think of running a user-space program like running a high-security prison:
1. **CSRs** are the **Control Dashboard buttons** in the Warden's office that lock/unlock doors and monitor system alarms.
2. **PTEs** are the **Security Clearance Logs** showing which prisoner has access to which cells.
3. **TLBs** are the **Warden's Quick-Reference Sticky Notes** of recently checked clearances, so they don't have to walk back to the filing cabinet every time a prisoner takes a step.

---

## 1. CSR (Control and Status Register)
> **The Metaphor**: The dashboard buttons and warning lights of a race car.

### What is it?
Unlike general-purpose registers (`r0..r26`) which are used to do basic math (like `add` or `sub`), a **CSR** is a specialized, hardware-wired register inside the CPU. 
* **Control**: Used to toggle CPU features on or off (e.g., turning on Virtual Memory, enabling interrupts).
* **Status**: Used by the hardware to report events to the operating system (e.g., "Why did we just trap?", "What cycle is it?", "Who just sent a console write?").

### Privileged Shield 🛡️
CSRs are heavily guarded. User programs are **never** allowed to read or write to critical CSRs (like the trap handler route). Doing so triggers an immediate privilege exception, jumping the CPU to Kernel Mode.

### Where is it in our Ternary VM?
We defined all our CSRs in [ternary_isa.h:L615-L643](file:///c:/Users/jonas/Documents/trit/ternary_isa.h#L615-L643):
* `CSR_TVEC` (Trap Vector): Stores the memory address of the Kernel's trap handler (`trap_handler`).
* `CSR_SCRATCH`: A temporary holding slot used by the kernel to save the User stack pointer before loading the Kernel stack.
* `CSR_MMU_ENABLE`: A 1-trit switch that turns page translation on (`1`) or off (`0`).
* `CSR_CYCLE`: Reports the total number of hardware clock cycles executed.

---

## 2. PTE (Page Table Entry)
> **The Metaphor**: A post-office forwarding card.

### What is it?
In virtual memory systems, the CPU pretends that each program has its own massive, unbroken memory space starting at address `0` (Virtual Addresses). But in reality, their code is scattered across random physical chunks of RAM (Physical Addresses).
A **PTE (Page Table Entry)** is a single word in memory that acts as a translator. It tells the CPU:
> *"When the User process asks for Virtual Page 3, go fetch physical DRAM Page 54 instead."*

### The Permission Flags
A PTE doesn't just map addresses; it enforces rules via hardware permission flags:
* `present` (Is this page loaded into RAM?)
* `user` (Is user-space code allowed to touch this page?)
* `read` / `write` / `execute` (Can they read it? Write to it? Execute it as code? Prevents code from overwriting itself or running data as instructions).

### Where is it in our Ternary VM?
We declare Page Table Entries directly in our kernel data segment using the custom assembler directive `.pte` in [minimal_kernel_bringup.tasm:L741-L750](file:///c:/Users/jonas/Documents/trit/OS3/minimal_kernel_bringup.tasm#L741-L750):
```assembly
imem_pt_shell: .pte 50, 1, 0, 0, 1  ; Maps Virtual Page 0 to Physical Page 50, User=1, Executable=1
dmem_pt_shell: .pte 16, 1, 1, 1, 0  ; Maps Virtual Page 0 to Physical Page 16, User=1, Read/Write=1
```

---

## 3. TLB (Translation Lookaside Buffer)
> **The Metaphor**: The postman's sticky note of recently looked-up forwarding addresses.

### What is it?
Because **PTEs** are stored in standard memory (DRAM), translating a virtual memory address is incredibly slow. To read *one* instruction, the CPU would have to:
1. Read the PTE from memory (slow).
2. Finally read the actual instruction (slow).
This double-memory lookup would make the CPU 50% slower!

To solve this, CPU designers added a tiny, super-fast hardware cache right inside the CPU core called the **TLB (Translation Lookaside Buffer)**. It stores the last 32 to 512 virtual-to-physical translations. 
* **TLB Hit**: The CPU checks the TLB, finds the address in 1 cycle, and translates it instantly.
* **TLB Miss**: The CPU must search standard RAM (a "page table walk"), which costs 100+ cycles, and then cache that result back into the TLB.

### What is a "TLB Flush"?
When the operating system context-switches from the Shell to `Prog A`, the virtual memory mappings change completely. If the CPU didn't clear the TLB, `Prog A` would read the cached physical translations of the Shell and access private memory! 
Therefore, on a context switch, the kernel must execute a special instruction (like `SFENCE.VMA` in RISC-V) to **flush the TLB**, forcing the new process to perform clean lookups.

---

## 🔄 Sequence: When a User Instruction Executes
Here is the step-by-step journey of an instruction:

```
[User runs: LOAD r1, 100]
       │
       ▼
1. CPU extracts Virtual Page number from address 100
       │
       ▼
2. CPU checks TLB (Translation Cache)
       ├─── [TLB HIT]  ───► Translates instantly! (1 cycle)
       │
       └─── [TLB MISS] ───► CPU walks physical memory to read the PTE (100+ cycles)
                                  │
                                  ▼
                            Is Permission Valid?
                               ├─── [NO]  ───► Trigger PAGE FAULT Exception! (Trap to CSR TVEC)
                               │
                               └─── [YES] ───► Cache in TLB & Load physical address
```
