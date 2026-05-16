# Common Assembly Patterns (The Cookbook)

| Status | Last Updated | Related Code |
| :--- | :--- | :--- |
| ✅ **Stable** | 2026-05-15 | `ternary_asm.h`, `ternary_isa.h` |

---

## 🧹 Basic Housekeeping

### Clearing a Register
The register `r0` (alias `zero`) is hardwired to zero.
```asm
COPY  r1, zero      ; The preferred method (1 cycle)
MOV   r1, 0         ; Also works, uses an immediate
```

### Swapping Two Registers
The Trit-Stack provides a native 1-cycle atomic swap, eliminating the need for a temporary register or "XOR-swap" tricks.
```asm
SWAP  r1, r2        ; Atomic exchange of r1 and r2 (1 cycle)
```

> [!TIP]
> **The Ternary Advantage**: Because our register file uses symmetric dual-rail wiring, the hardware can "cross" the output buses of two registers simultaneously. In x86 or RISC-V, this typically requires 3 instructions and a temporary register.
```

### Sign Flipping
In balanced ternary, negating a number is a simple trit-flip.
```asm
NEG   r1, r1        ; Arithmetic negation: r1 = -r1
TINV  r1, r1        ; Logical inversion: alias for NEG
```

---

## ⚖️ Conditional Branching (The 3-Way Branch)
Ternary doesn't use "flags" (Zero, Carry). Instead, it uses the result of a comparison directly. `TCMP` returns `-1`, `0`, or `+1`.

### Pattern: If (r1 == r2)
```asm
TCMP  r5, r1, r2    ; r5 = sign(r1 - r2)
BRZ   r5, label_eq  ; Branch if the result was 0
```

### Pattern: Full 3-Way Split
```asm
TCMP  r5, r1, r2    ; r5 = sign(r1 - r2)
BRN   r5, label_lt  ; Branch if r1 < r2  (-1)
BRZ   r5, label_eq  ; Branch if r1 == r2 (0)
BRP   r5, label_gt  ; Branch if r1 > r2  (+1)
```

---

## 🔄 Loops

### Pattern: Standard "For" Loop (N to 0)
```asm
    MOV   r1, 10        ; counter = 10
loop:
    ; ... [loop body] ...
    
    SUB   r1, r1, 1     ; counter -= 1
    TCMP  r5, r1, zero  ; r5 = sign(counter)
    BRP   r5, loop      ; Keep going if counter > 0
    BRZ   r5, loop      ; Keep going if counter == 0 (optional)
```

---

## 📞 Functions & Subroutines

### Basic Call and Return
```asm
    CALL  my_function   ; r25 = PC+1, jump to my_function
    ; ... execution resumes here ...

my_function:
    ; ... [function body] ...
    RET                 ; jump to r25
```

---

## 💾 Memory Access

### Loading from a Global Label
```asm
.data
    my_var: .word 12345

.text
    LOAD  r1, zero, my_var ; Load address 'my_var' with 0 offset
```

### Array Access (Base + Index)
```asm
    ; r2 = base address of array
    ; r3 = index (word offset)
    ADD   r4, r2, r3    ; Compute effective address
    LOAD  r1, r4, 0     ; Load value at array[index]
```

---

## 🚀 AI / BitNet Specifics

### Clearing the Accumulator
```asm
ACLR.t50                ; Reset internal 50-trit accumulator to zero
```

### T1 Activation (ReLU equivalent)
```asm
VACT.t1  v1, v1         ; Apply ternary activation to vector v1
```
