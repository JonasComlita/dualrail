# Ternary Logic Gates

| Status | Last Updated | Related Code |
| :--- | :--- | :--- |
| ✅ **Stable** | 2026-05-15 | `ternary_lanes.h`, `ternary_vm.h` |

---

## 🌩️ Fundamental Per-Trit Gates
The Trit-Stack logic layer operates on **Dual-Rail encoded trits**. These gates are used in the `TL*` (Trit-Lane) instructions.

### 1. NOT / NEG (Inverter)
Performs a trit-flip. In balanced ternary, this is its own inverse.
*Code Ref: `tritwiseNeg`*

| Input | Output |
| :--- | :--- |
| `-1` | `+1` |
| ` 0` | ` 0` |
| `+1` | `-1` |

### 2. AND / TLAND (Lattice Min)
Returns the lower of the two inputs.
*Code Ref: `tritwiseAnd` (using `std::min`)*

| A \ B | `-1` | ` 0` | `+1` |
| :--- | :--- | :--- | :--- |
| **-1** | `-1` | `-1` | `-1` |
| ** 0** | `-1` | ` 0` | ` 0` |
| **+1** | `-1` | ` 0` | `+1` |

### 3. OR / TLOR (Lattice Max)
Returns the higher of the two inputs.
*Code Ref: `tritwiseOr` (using `std::max`)*

| A \ B | `-1` | ` 0` | `+1` |
| :--- | :--- | :--- | :--- |
| **-1** | `-1` | ` 0` | `+1` |
| ** 0** | ` 0` | ` 0` | `+1` |
| **+1** | `+1` | `+1` | `+1` |

### 4. XSUM / TLADD (Modular Sum)
Carryless addition. Also known as the ternary XOR or Half-Adder sum.
*Code Ref: `tritwiseAddCarryless`*

| A \ B | `-1` | ` 0` | `+1` |
| :--- | :--- | :--- | :--- |
| **-1** | `+1` | `-1` | ` 0` |
| ** 0** | `-1` | ` 0` | `+1` |
| **+1** | ` 0` | `+1` | `-1` |

---

## 🔀 Selection & Multiplexing

### TSEL (Ternary Select)
The most powerful control gate. It selects one of three source registers based on the sign of a condition trit.

**Instruction Syntax:** `TSEL rd, rCond, rNeg, rZero, rPos`

| Condition Trit | Selected Output |
| :--- | :--- |
| **Negative (-1)** | `rNeg` |
| **Neutral (0)** | `rZero` |
| **Positive (+1)** | `rPos` |

---

## 🛠️ Hardware Mapping (Dual-Rail)
When implemented in FPGA/ASIC, these gates are mapped to 2-bit binary pairs. 

> [!IMPORTANT]
> **Propagating Invalidity**: If any input to a gate is the `0b11` (Invalid) pattern, the gate output is forced to `0b11`. This ensures hardware faults are "sticky" and trigger traps immediately.
