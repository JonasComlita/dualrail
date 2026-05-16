# Assembler Syntax Guide

| Status | Last Updated | Related Code |
| :--- | :--- | :--- |
| ✅ **Stable** | 2026-05-15 | `ternary_asm.h` |

---

## 📝 General Rules
The Trit-Stack Assembler converts human-readable source into a raw binary image for the VM.

*   **Case Sensitivity**: Mnemonics and register names are **case-insensitive** (`ADD` == `add`).
*   **Comments**: Start with a semicolon (`;`) and continue to the end of the line.
*   **Delimiters**: Commas are optional. `ADD r1, r2, r3` is identical to `ADD r1 r2 r3`.
*   **Labels**: Must be followed by a colon (`label_name:`). Labels resolve to absolute addresses in IMEM or DMEM.

---

## 🛠️ Instruction Grammar

### 1. Width Selection (The Dot-Suffix)
Instructions that support variable precision (R-Type) use a suffix to specify the `func` field.

| Suffix | Mode | Description |
| :--- | :--- | :--- |
| `.t1` | **Trit** | Single-trit arithmetic |
| `.t5` | **Tryte** | 5-trit (8-bit equivalent) arithmetic |
| `.t40`| **Triple** | Standard 40-trit arithmetic |
| `.l1` | **Lane** | Single-trit logical gates |

**Example**: `ADD.t5 r1, r2, r3` (Performs a 5-trit modular sum).

### 2. Immediate Formats
*   **Integers**: Standard decimal integers (e.g., `123`, `-45`).
*   **Labels**: The assembler automatically calculates the relative offset for branches (`BRN r1, loop`) and absolute addresses for loads (`LOAD r1, r0, data_label`).

---

## 🏗️ Directives & Sections
The assembler organizes memory using hardware-mapped sections.

| Directive | Segment | Purpose |
| :--- | :--- | :--- |
| **`.text`** | **IMEM** | Contains executable code. This is the default section. |
| **`.data`** | **DMEM** | Contains static variables and constants. |
| **`.word`** | **Value** | Defines a 50-trit word in the `.data` section. |

---

## 📑 Example Code
```asm
.text
main:
    MOV   r1, 10       ; Load decimal 10 into r1
    CALL  compute      ; Jump to compute, save return to lr
    HALT               ; Stop VM

compute:
    ADD.t40 r1, r1, r1 ; Double r1 using 40-trit precision
    RET                ; Return to caller
```

> [!NOTE]
> **Label Resolution**: The assembler is two-pass. Pass 1 collects label addresses; Pass 2 resolves offsets and emits the 27-trit binary words.
