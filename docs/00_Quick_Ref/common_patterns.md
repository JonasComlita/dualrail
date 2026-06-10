# Common Assembly Patterns

Reference for agents writing or patching ternary assembly (`.tasm`).

---

## Basic Patterns

### Load an integer constant into a register

```asm
; Small value (fits in 16 trits = ±21,523,360):
MOV  r13, 42

; Large value: split into upper and lower halves
MOV  r13, <low16>
MOVH r13, <high16>     ; shifts high16 left by 16 trits, ORs into r13
```

### Copy a register

```asm
COPY r14, r13          ; r14 ← r13
```

### Negate a value

```asm
NEG  r14, r13          ; r14 ← -r13 (trit flip)
```

### Arithmetic

```asm
ADD  r15, r13, r14     ; r15 ← r13 + r14
SUB  r15, r13, r14     ; r15 ← r13 - r14
MUL  r15, r13, r14     ; r15 ← r13 × r14
DIV  r15, r13, r14     ; r15 ← r13 ÷ r14  (r14=0 → TRAP)
ABS  r14, r13          ; r14 ← |r13|
SQRT r14, r13          ; r14 ← √r13
```

---

## Function Call and Return

```asm
; Caller:
MOV  r13, <arg0>
MOV  r14, <arg1>
CALL target_label      ; r25 ← PC+1; jumps to target_label

; Callee:
; (optional: push callee-saved registers)
; ... body ...
; (optional: pop callee-saved registers)
MOV  r13, <return_value>
RET                    ; PC ← r25
```

### Save and restore callee-saved registers

```asm
; Prologue (r1–r12 are callee-saved):
SUB  r26, r26, 3       ; allocate 3 stack slots
STORE r26, r1, 0       ; save r1
STORE r26, r2, 1       ; save r2
STORE r26, r3, 2       ; save r3

; Epilogue:
LOAD r1, r26, 0        ; restore r1
LOAD r2, r26, 1
LOAD r3, r26, 2
ADD  r26, r26, 3       ; deallocate
RET
```

---

## Branching

### Unconditional jump

```asm
JMP  target_label
```

### Conditional branch (three-way compare)

```asm
TCMP r5, r1, r2        ; r5 ← sign(r1 - r2): -1, 0, or +1
BRN  r5, neg_branch    ; jump if r5 == -1
BRP  r5, pos_branch    ; jump if r5 == +1
; fall through: r5 == 0
```

### Check if zero

```asm
BRZ  r5, zero_branch   ; jump if r5 == 0
```

### Three-way select (TSEL)

```asm
TSEL r10, r5, r_neg_val, r_zero_val, r_pos_val
; r10 ← r_neg_val if r5 == -1
;        r_zero_val if r5 == 0
;        r_pos_val  if r5 == +1
```

---

## Memory Access

```asm
; Load word from mem[base + offset]:
LOAD  r14, r13, 5      ; r14 ← mem[r13 + 5]

; Store word to mem[base + offset]:
STORE r13, r14, 5      ; mem[r13 + 5] ← r14

; Pointer arithmetic:
ADD   r15, r13, r14    ; r15 ← r13 + r14  (word offset)
```

---

## Syscall Pattern

```asm
; Set syscall ID
MOV   r0, 15           ; sys_write = 15
CSRW  14, r0           ; CSR 14 = syscall_id

; Args
MOV   r13, <fd>
MOV   r14, <buf_ptr>
MOV   r15, <len>

; Invoke
SYSCALL

; Check result
BRZ   r13, ok_label    ; r13 == 0 means success
; handle error...
ok_label:
```

---

## CSR Operations

```asm
; Read CSR
CSRR  r14, 5           ; r14 ← CSR[5] (cycle counter)

; Write CSR
MOV   r15, 1
CSRW  22, r15          ; CSR[22] ← 1 (console_out)

; Atomic read-write
CSRRW r14, r15, 14     ; r14 ← CSR[14]; CSR[14] ← r15
```

---

## Vector Operations

```asm
; Set vector length
MOV   r13, 16
CSRW  <vlen_csr>, r13   ; (actual CSR TBD — see ternary_isa.h)

; Broadcast scalar to all lanes
VBCAST  v0, r13         ; v0[*] ← r13

; Vector add (T40 width)
VADD.t40  v2, v0, v1    ; v2[i] ← v0[i] + v1[i] for all lanes

; Dot product (T1 vectors)
VDOT  r13, v0, v1       ; r13 ← sum(v0[i] × v1[i])  → T50 result

; Horizontal sum
VSUM  r14, v2           ; r14 ← sum of all v2 lanes
```

---

## Width Selection

Append width suffix to arithmetic instructions:

```asm
ADD.t1   r3, r1, r2    ; 1-trit add
ADD.t5   r3, r1, r2    ; 5-trit add
ADD.t10  r3, r1, r2    ; 10-trit float add
ADD.t20  r3, r1, r2    ; 20-trit float add
ADD      r3, r1, r2    ; default = T40 (40-trit float)
ADD.t50  r3, r1, r2    ; 50-trit float add
```

---

## Common Idioms

### Absolute value

```asm
ABS   r14, r13
```

### Max of two values

```asm
TMAX  r15, r13, r14    ; r15 ← max(r13, r14)
```

### Min of two values

```asm
TMIN  r15, r13, r14    ; r15 ← min(r13, r14)
```

### Swap two registers

```asm
SWAP  r13, r14         ; r13 ↔ r14
```

### Modulo

```asm
TMOD  r15, r13, r14    ; r15 ← r13 mod r14
```

### Trit shift

```asm
TLSHIFT r14, r13, r15  ; r14 ← r13 shifted left by r15 trit positions
TRSHIFT r14, r13, r15  ; r14 ← r13 shifted right by r15 trit positions
```

---

## Skeleton: Hello World in Assembly

```asm
.text
.global main
main:
  ; sys_write_char in a loop
  MOV   r1, hello_str   ; pointer to string

loop:
  LOAD  r13, r1, 0      ; load char
  BRZ   r13, done       ; 0 = end of string

  ; CSRW console_out directly (faster than syscall)
  CSRW  22, r13         ; write char to console

  ADD   r1, r1, 1       ; advance pointer
  JMP   loop

done:
  MOV   r0, 44          ; sys_exit = 44
  CSRW  14, r0
  MOV   r13, 0          ; exit code 0
  SYSCALL

.data
hello_str: .word 72, 101, 108, 108, 111, 10, 0  ; "Hello\n\0"
```
