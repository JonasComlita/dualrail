# Unary Logic Gates (The 27 States)

| Status | Last Updated | Related Code |
| :--- | :--- | :--- |
| ✅ **Stable** | 2026-05-15 | `ternary_isa.h`, `ternary_vm.h` |

---

## 🏗️ The Universal Synthesizer: TSEL
In binary, you need dedicated hardware for different unary gates. In the Trit-Stack, the **`TSEL`** instruction can synthesize **any** of the 27 possible unary gates in a single cycle.

**Formula**: `f(input) = TSEL(input, f(-1), f(0), f(1))`

---

## 📋 The 27 Possible Unary Gates
Each gate is defined by its output for the inputs `(-1, 0, +1)`.

### 1. The Standard Gates (Native)
| Name | Table | Description | Opcode |
| :--- | :--- | :--- | :--- |
| **Identity** | `[-1, 0, 1]` | Returns input unchanged. | `COPY` |
| **Negation** | `[1, 0, -1]` | The standard Inverter. | `NEG` |
| **Absolute** | `[1, 0, 1]` | Flips negative to positive. | `ABS` |

### 2. The Constant Gates
| Name | Table | Description |
| :--- | :--- | :--- |
| **Zero** | `[0, 0, 0]` | Always Neutral. |
| **One** | `[1, 1, 1]` | Always Positive. |
| **Minus One** | `[-1, -1, -1]` | Always Negative. |

### 3. The Threshold (Predicate) Gates
These gates are fundamental for conditional branching and AI activations.

| Name | Table | Description | Semantic |
| :--- | :--- | :--- | :--- |
| **Is Positive?** | `[-1, -1, 1]` | `+1` if input is `+1`, else `-1`. | $x > 0$ |
| **Is Negative?** | `[1, -1, -1]` | `+1` if input is `-1`, else `-1`. | $x < 0$ |
| **Is Non-Zero?** | `[1, -1, 1]` | `+1` if input is non-zero, else `-1`. | $x \neq 0$ |
| **Is Zero?** | `[-1, 1, -1]` | `+1` if input is zero, else `-1`. | $x = 0$ |

### 4. The Cyclic (Shift) Gates
Used in modular arithmetic and pointer rotating.

| Name | Table | Description |
| :--- | :--- | :--- |
| **Successor** | `[0, 1, -1]` | $x + 1 \pmod 3$ |
| **Predecessor** | `[1, -1, 0]` | $x - 1 \pmod 3$ |

---

## 🧠 Strategic Usage in Software
While the VM only provides dedicated opcodes for `NEG` and `ABS`, the assembler provides macros for the others using `TSEL`.

**Example: "Is Positive" Predicate**
```asm
; Input in r1, Result in r2
; We want r2 = (r1 > 0) ? +1 : -1
; Constants: r0=0, r25=1, r26=-1 (Hypothetical)
TSEL r2, r1, r26, r26, r25  ; [-1, -1, +1]
```

> [!TIP]
> Most AI activation functions in BitNet (like ReLU-ternary) are simply Threshold Gates from Category 3.
