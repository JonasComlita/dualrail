# Host Interface (The Substrate)

| Status | Last Updated | Related Code |
| :--- | :--- | :--- |
| ✅ **Stable** | 2026-05-15 | `ternary_vm.h` |

---

## 🌉 The "Substrate" Concept
The Trit-Stack VM does not exist in a vacuum. The **Substrate** is the interface layer that connects the ternary machine to the host operating system (e.g., C++/Windows).

### 1. The SYSCALL Instruction
Programs communicate with the host via the `SYSCALL` opcode (Opcode 69). 

*   **Arguments**: `r1` is the canonical register used for passing values to the host.
*   **Errors**: Any `SYSCALL` with an unrecognized immediate triggers an immediate **`TRAP_ILLEGAL_OP`**.

| Immediate | Action | Semantic |
| :--- | :--- | :--- |
| **1** | **PRINT_INT** | Appends the decimal value of `r1` to the host's `syscall_buffer`. |
| **2** | **PRINT_NEWLINE** | Appends `\n` to the `syscall_buffer`. |
| **3** | **CLEAR_BUFFER** | Flushes the `syscall_buffer`. |

### 2. The Host Execution Loop
The host environment is responsible for driving the machine. A typical host implementation follows this pattern:

```cpp
while (vm.status == VMStatus::RUNNING) {
    vm.step();
    if (!vm.syscall_buffer.empty()) {
        std::cout << vm.syscall_buffer;
        vm.syscall_buffer.clear();
    }
}
```

### 3. VM Hooks (Observability)
The VM supports an external `VMHooks` structure that allows the host to "listen" to execution without modifying the instruction stream:
*   **OnStep**: Fired after every instruction cycle.
*   **OnTrap**: Fired when a hardware fault occurs.
*   **OnMemory**: Fired on every `LOAD` or `STORE`.

---

## 📂 Binary Loading
The host is responsible for "imaging" the machine before execution begins.
*   **IMEM Loading**: The host writes binary `TritWord27` instructions into the IMEM array.
*   **DMEM Pre-population**: The host can pre-load constants or static data into DMEM.

---

## ⚡ Host Time vs Machine Time
*   **Cycle Counting**: The VM tracks the number of `step()` calls.
*   **Determinism**: For a given IMEM image and initial state, the VM will always produce the same memory and register results, regardless of host CPU speed or OS.
