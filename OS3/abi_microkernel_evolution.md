# Systems Design Masterclass: How the ABI Unlocked Mutexes, Semaphores, and Microkernel Scheduling

In OS engineering, there is a temptation to write code from the top down: "Let's build a shell, then write some mutexes, then figure out how tasks are scheduled." 

By reversing this order—**building and freezing the ABI (Application Binary Interface) first**—we laid down an immutable contract of trust. This architectural decision was the key that unlocked **atomic locks (mutexes)**, **counting guards (semaphores)**, **blocking wait channels**, and **preemption safety** inside our microkernel.

This document breaks down exactly how the ABI served as the master key.

---

## 1. The Immutable Contract: What is our ABI?
An **ABI** is the physical contract between User Space, Kernel Space, and the CPU hardware. It defines:
1. **Register Roles**: Which registers are scratch, which store arguments, and which are preserved across calls (e.g. `sp` for stack pointer, `lr` for return jumps).
2. **Syscall Calling Conventions**: How a program requests kernel work (placing syscall IDs in `syscall_id` and arguments in `r13..r18`).
3. **The Trap Frame Structure**: The exact `32-word` memory layout of a process's saved state (`ctx_shell`, `ctx_a`, `ctx_b`, `idle_ctx`).

```
    ┌────────────────────────────────────────────────────────┐
    │                       USER SPACE                       │
    └───────────────────────────┬────────────────────────────┘
                                │ Syscall ABI (r13..r18)
                                ▼
    ┌────────────────────────────────────────────────────────┐
    │                      KERNEL GATE                       │
    └───────────────────────────┬────────────────────────────┘
                                │ Trap Frame ABI (sp + offset)
                                ▼
    ┌────────────────────────────────────────────────────────┐
    │                     HARDWARE STATE                     │
    └────────────────────────────────────────────────────────┘
```

---

## 2. How the ABI Enabled Preemptive Mutexes

A **Mutex (Mutual Exclusion)** guarantees that only one process can enter a critical section (e.g. writing to a shared console or reading database records). It relies on atomic instruction sequences:
1. `TLDR` (Ternary Load Reserved): Reads a memory lock value and locks the bus.
2. `TSTR` (Ternary Store Conditional): Attempts to write a `1` to the lock. Succeeds ONLY if no other process touched the lock since `TLDR`.

### The Preemption Hazard
Without our ABI, consider what happens if a task is interrupted by the timer **midway** through its atomic lock evaluation:
1. `Prog A` reads the lock, performs a conditional check, and is about to write to the lock.
2. **Preemption Tick Fires!** The hardware switches to Kernel Mode.
3. The kernel runs `Prog B`, which also tries to acquire the lock.

### The ABI Cure
Because our ABI guarantees a rigid, hardware-saved **32-word Trap Frame**, the kernel preserves `Prog A`'s register states (`r0..r26`, `status`, `epc`) with absolute precision. 
When `Prog A` is eventually rescheduled, **its exact atomic retry flags, stack pointer, and register comparison states are restored down to the exact trit**. The lock state machine resumes without a single leak or race condition. 

---

## 3. From CPU-Burning "Spinlocks" to Elegant "Semaphores"

In early operating systems, tasks used **Spinlocks** to wait for locks:
```assembly
; CPU-burning Spinlock
wait_loop:
    load r1, zero, lock_addr
    brp r1, wait_loop          ; Spin forever if lock is positive (held)
```
This is a disaster for performance. It keeps the CPU running at 100% utilization, burning clock cycles and power while doing zero actual work.

### The Wait Channel Revolution (Unlocked by the ABI)
Because we standardized our **Process Control Block (PCB) Metadata Layout** in the ABI first (`proc_state[]`, `proc_wait_channel[]`, `proc_wait_target[]`), we unlocked true non-blocking **Semaphores**:

1. **The Request**: `Prog A` tries to acquire a locked Mutex.
2. **The Deschedule (Syscall)**: Instead of spinning, it calls a yield-type syscall. The kernel modifies the process’s metadata:
   * Sets `proc_state[Prog A] = PROC_STATE_BLOCKED`
   * Sets `proc_wait_channel[Prog A] = PROC_WAIT_MUTEX`
   * Sets `proc_wait_target[Prog A] = mutex_address`
3. **Zero-Power Sleep**: The scheduler dequeues `Prog A` from the ready queue and switches to `Idle`. The CPU consumes **zero active execution cycles** while waiting!
4. **The Wakeup**: When the mutex-holder releases the lock, the kernel scans the blocked queue, sees `Prog A` is waiting on that address, marks it `RUNNABLE`, and enqueues it back to run.

This entire sequence is **conceptually impossible** without the ABI defining exactly *how* tasks deschedule, *where* their sleep target is logged, and *how* they are awoken.

---

## 4. Unlocking the Rest of the Microkernel Ecosystem

Freezing the ABI first was the foundational domino that unlocked everything else in Phase 4:

*   **Interactive Command Shell (`waitpid`)**: The shell works exactly like a process-level semaphore. When the shell spawns a child, it blocks on a wait-channel target (`PROC_WAIT_CHILD`). The shell idles securely until the child's `exit` syscall triggers a wakeup event, returning control and the child's exit status.
*   **Console I/O Event Routing**: The shell blocks on `PROC_WAIT_CONSOLE_INPUT`. It burns **zero CPU cycles** until the host enqueues a character, triggering the hardware interrupt loop that wakes the shell.
*   **Dynamic Program Loaders (Phase 4.12)**: By defining a strict executable header ABI (`.execheader`), we can compile separate binaries independently. The kernel doesn't need to know anything about the program's code; it just reads the self-describing header, allocates page translation entries (PTEs), and routes the starting EPC.

### The Takeaway
By stabilizing the **bottom of the stack** (the ISA, ABI, and VM states), we created a clean, unchanging bedrock. Because this contract never changes, we can write complex higher-level code (like shell loops, semaphores, and compilers) knowing the ground beneath us will never shift!
