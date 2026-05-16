# Processing Media on the Ternary OS

Processing media (image, video, audio) on an OS isn't just about displaying content — it's about managing **high-bandwidth, real-time data streams** with hard timing deadlines. Moving from a minimal kernel to a system that can play video or process audio requires four specific infrastructure layers, each of which maps onto the ternary ISA in ways that binary systems cannot match.

---

## 1. The Isochronous I/O Layer (Drivers)

"Isochronous" means data must move at a **constant, predictable rate.** Missing a deadline by even one millisecond produces an audible pop in audio or a torn frame in video.

*   **Audio**: You need a driver that can talk to a DAC (Digital-to-Analog Converter). The "Strategic Architecture" requirement here is a **Ring Buffer**. The OS fills one half of the buffer while the hardware plays the other half. If the OS misses its deadline by even 1 millisecond, you get an audible "pop."

*   **Image/Video**: You need a **Framebuffer** (a map of memory to pixels) and a **Vertical Sync (VSync)** interrupt. The OS needs to know exactly when the screen is finished drawing a frame so it can swap in the one without "tearing."

### Audio: Ring Buffers

The standard solution is a ring buffer — the OS fills one half while hardware plays the other. In binary systems, ring buffer wraparound requires a power-of-2 size and a bitmask:

```c
// Binary ring buffer wrap
head = (head + 1) & (buffer_size - 1);   // only works if buffer_size is power of 2
```

In ternary, the `TMOD` opcode makes this natural at any power-of-3 size:

```asm
; Ternary ring buffer head increment
TMOD  r_head, r_head, r_buffer_size    ; wraps at 27, 81, 243, 729 — one instruction
```

Buffer sizes of 27, 81, 243, and 729 words align with trit addressing. No bitmask, no branch, no power-of-2 constraint. This is one of the concrete places where ternary is structurally simpler than binary for OS infrastructure.

### Video: Framebuffer and Triple Buffering

The framebuffer maps a region of DMEM to pixels. A VSync interrupt signals when the display has finished drawing a frame so the OS can swap in the next one without tearing.

Binary systems use double buffering — front buffer and back buffer — with explicit state management. Ternary has a more natural solution:

```
Binary:  front / back                   (2 states, manual swap logic)
Ternary: front / back / decode buffer   (3 states, native to trit arithmetic)
```

The ternary state machine for buffer rotation falls out of the arithmetic directly:

```asm
; Framebuffer state machine — r1 ∈ {-1, 0, +1} = {front, back, decode}
MOV.t1   r2, 1
TLADD.l1 r1, r1, r2          ; advance state, wraps naturally in trit lane arithmetic
TSEL     r_active, r1, r_buf0, r_buf1, r_buf2
```

Triple buffering is the standard fix for the GPU-ahead-of-display problem in modern rendering. In binary it requires explicit conditional logic. In ternary it is one `TLADD` and one `TSEL`.

### The Timer Interrupt

Both audio and video require a timer interrupt, which maps to the SYSCALL interface:

```
SYSCALL 8  — set_timer(period)    request periodic interrupt at period trytes
SYSCALL 9  — wait_interrupt()     block until next timer fires, yield CPU
```

The audio driver loop:

```asm
audio_loop:
    SYSCALL 8           ; arm timer for next 44.1kHz deadline
    ; fill next buffer half here
    SYSCALL 9           ; sleep until deadline — yields CPU to other threads
    JMP audio_loop
```

This is the soft real-time model made concrete. Without `set_timer` and `wait_interrupt`, the audio buffer has no implementation path regardless of how the ring buffer is structured.

---

## 2. The Memory Pipeline (DMA and Zero-Copy)

Media files are large. If the CPU manually `LOAD`s and `STORE`s every pixel or sample, it has no time left for anything else.

**DMA (Direct Memory Access)** allows hardware to pull data directly from DMEM to the audio or video device without CPU involvement. The CPU sets up a transfer descriptor and the DMA engine handles the rest.

**Zero-copy networking** for video streaming means the OS moves data from the network card directly to the video buffer. Every intermediate copy is a throughput and latency penalty. The MMU implementation already in the system enables this — a region of DMEM can be mapped as both the network receive buffer and the video decode input simultaneously, with no copy in between.

The `FENCE` opcode is required here:

```asm
; Signal DMA transfer complete before reading result
; Without FENCE, CPU may see stale data from before DMA wrote it
FENCE
LOAD  r1, r_dma_result_addr, 0
```

---

## 3. Real-Time Scheduling

Standard OS schedulers are fair — they give every thread a turn. Media requires **priority-driven scheduling** with explicit deadline awareness.

The audio thread must preempt the compiler, the shell, and any other non-deadline work. If the compiler takes too much CPU time, the audio should not stutter. This is implemented via the scheduler's priority queue, where the audio thread holds the highest static priority and `wait_interrupt` voluntarily yields until its next deadline.

**Multicore scaling** for video decoding maps directly to the Lock ABI already implemented. Video decoding (H.264, or a future ternary-native codec) is embarrassingly parallel — different chunks of the same frame can be decoded independently. The atomic `TSTR` instruction coordinates cores handing off buffer regions:

```asm
; Core 0 signals Core 1 that a frame chunk is ready
MOV.t1    r_flag, 1
TSTR      r_flag, r_chunk_ready_addr   ; atomic store, visible across cores
```

Core 1 polls or waits on `r_chunk_ready_addr` before consuming the decoded data. The three-valued atomic gives a natural three-state handshake: negative means not started, zero means in progress, positive means ready — no separate status register needed.

---

## 4. The Math Layer (Ternary Codecs)

This is where the ternary vector ISA becomes the differentiating factor.

### Quantization Is Truncation

In binary lossy compression, quantization is a separate lossy step that introduces structured rounding error:

```
Binary JPEG pipeline:
  DCT coefficients → divide by quantization table → round → entropy code
  (rounding error accumulates, scale information is destroyed)
```

In ternary, quantization is trit truncation — dropping lower-order trits using `CVT`:

```
Ternary pipeline:
  DCT coefficients → cvt.t40.t10 → entropy code
  (scale is preserved exactly in the exponent field,
   only mantissa precision is reduced)
```

The `cvt` instruction preserves the exponent field exactly and reduces only the mantissa. The *scale* of the value survives; only its precision decreases. Binary quantization has no equivalent — dividing by a quantization step destroys scale information and requires the decoder to reconstruct it.

The ternary quality ladder is one instruction per step:

```
t50 → t40 → t20 → t10 → t5
```

Each conversion is lossless in scale and lossy only in mantissa resolution. This gives a codec five natural quality levels with no additional infrastructure.

### FFT and DCT

Audio and video processing both rely on FFTs (Fast Fourier Transforms) and DCTs (Discrete Cosine Transforms). The FFT butterfly operation maps directly to the vector ISA:

```asm
; One FFT butterfly stage
VMUL.t20  v2, v0, v_twiddle     ; element-wise multiply by twiddle factors
VADD.t20  v3, v0, v2            ; upper butterfly arm
VSUB.t20  v4, v0, v2            ; lower butterfly arm
```

The ternary twiddle factors (roots of unity in balanced ternary) are exact representations, not floating point approximations. Binary FFT implementations accumulate rounding error in twiddle factors across stages. Ternary FFT with exact twiddle factors has better numerical stability for audio work — relevant at high stage counts where binary error accumulation becomes audible.

### Audio Filtering and Image Convolution

The `VMAC.t1` (vector multiply-accumulate) instruction handles the inner loop of both audio FIR filters and image convolution kernels:

```asm
; Audio FIR filter: output[n] = sum(coeffs[k] * input[n-k])
ACLR.t20                         ; clear accumulator
vmac_loop:
    VLOAD.t20  v_in,    r_input_ptr, 0
    VLOAD.t20  v_coeff, r_coeff_ptr, 0
    VMAC.t1    v_in, v_coeff      ; accumulate dot product
    ; advance pointers...
    BRN r_loop_cond, vmac_loop
ASTORE.t20 r_output               ; extract result
```

The same loop structure handles 2D image convolution (blurring, sharpening, edge detection) by treating the image as a stream of row vectors.

### Horizontal Reductions

The proposed `VSUM`, `VHMIN`, and `VHMAX` opcodes close the gap between element-wise vector operations and scalar results. Without them, computing the energy of an audio frame or the peak pixel value in a tile requires a manual scalar reduction loop. With them:

```asm
VSUM.t20   r_energy, v_samples   ; sum all lanes → scalar energy value
VHMAX.t20  r_peak,   v_samples   ; peak sample value for normalization
```

---

## 5. The Ternary Media Stack Roadmap

| Component | Status | Notes |
|-----------|--------|-------|
| **Audio ring buffer** | Possible now | Needs TMOD opcode + SYSCALL 8/9 timer |
| **Image framebuffer** | Possible now | Needs host-side window (Vulkan or SDL) |
| **Triple framebuffer** | Possible now | TLADD + TSEL state machine, three buffers |
| **FFT/DCT kernel** | Possible with vector ISA | VMAC + VSUM opcodes, benefits from compiler |
| **Timer interrupt** | Needs SYSCALL 8/9 | Host thread coordination for deadlines |
| **DMA transfer** | Needs SYSCALL extension | FENCE already in ISA |
| **Video decoding** | Needs compiler | Codecs too complex for hand-written assembly |
| **Ternary native codec** | Long term | Full compiler + vector ops + codec design |
| **GPU acceleration** | In progress | Vulkan bridge for Qwen; same infrastructure |

---

## Strategic Connection

The Lock ABI, MMU, and atomic `TSTR` together enable hardware-accelerated video decode: one core decodes a frame while the GPU (Vulkan) renders the previous one, with `TSTR` signaling buffer readiness across the boundary. The ternary three-state atomic makes the producer-consumer handshake cleaner than the binary equivalent — no separate done/error flags, no ABA problem from two-state CAS. The full media stack is not a detour from the main architecture; it is the same infrastructure applied to a different workload.