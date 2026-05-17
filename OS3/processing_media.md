# Processing Media on the Ternary OS

Processing media (image, video, audio) on an OS isn't just about displaying content — it's about managing **high-bandwidth, real-time data streams** with hard timing deadlines. Moving from a minimal kernel to a system that can play video or process audio requires four specific infrastructure layers, each of which maps onto the ternary ISA in ways that binary systems cannot match.

---

## 1. The Isochronous I/O Layer (Drivers)

"Isochronous" means data must move at a **constant, predictable rate.** Missing a deadline by even one millisecond produces an audible pop in audio or a torn frame in video.

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

Buffer sizes of 27, 81, 243, and 729 words align directly with trit addressing. No bitmask, no branch, no power-of-2 constraint. This is one of the most concrete places where ternary is structurally simpler than binary for OS infrastructure.

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

Triple buffering is the standard fix for the GPU-ahead-of-display problem in modern rendering. In binary it requires explicit conditional logic. In ternary it is one `TLADD` and one `TSEL`. This is not an optimization — it is the natural expression of a three-state problem in a three-valued system.

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

Without `set_timer` and `wait_interrupt`, the audio buffer has no implementation path regardless of how the ring buffer is structured. These two syscalls are the minimum viable real-time primitive.

---

## 2. The Memory Pipeline (DMA and Zero-Copy)

Media files are large. If the CPU manually `LOAD`s and `STORE`s every pixel or sample, it has no time left for anything else.

**DMA (Direct Memory Access)** allows hardware to pull data directly from DMEM to the audio or video device without CPU involvement. The CPU sets up a transfer descriptor and the DMA engine handles the rest.

**Zero-copy networking** for video streaming means the OS moves data from the network card directly to the video buffer. Every intermediate copy is a throughput and latency penalty. The MMU already in the system enables this — a region of DMEM can be mapped as both the network receive buffer and the video decode input simultaneously, with no copy in between.

The `FENCE` opcode is required at the DMA boundary:

```asm
; Ensure DMA write is visible before CPU reads result
; Without FENCE, CPU may observe stale data from before DMA completed
FENCE
LOAD  r1, r_dma_result_addr, 0
```

---

## 3. Real-Time Scheduling

Standard OS schedulers are fair — they give every thread a turn. Media requires **priority-driven scheduling** with explicit deadline awareness.

The audio thread must preempt the compiler, the shell, and any other non-deadline work. If the compiler takes too much CPU time, the audio must not stutter. This is implemented via the scheduler's priority queue, where the audio thread holds the highest static priority and `wait_interrupt` voluntarily yields until its next deadline.

**Multicore scaling** for video decoding maps directly to the Lock ABI already implemented. Video decoding is embarrassingly parallel — different chunks of the same frame can be decoded independently. The atomic `TSTR` instruction coordinates cores handing off buffer regions:

```asm
; Core 0 signals Core 1 that a frame chunk is ready
MOV.t1    r_flag, 1
TSTR      r_flag, r_chunk_ready_addr   ; atomic store, visible across cores
```

Core 1 polls `r_chunk_ready_addr` before consuming the decoded data. The three-valued atomic gives a natural three-state handshake:

```
-1  not started
 0  in progress
+1  ready
```

No separate status register. No ABA problem from two-state CAS. The handshake state is encoded directly in the trit value.

---

## 4. The Math Layer (Ternary Codecs)

This is where the ternary vector ISA becomes the differentiating factor.

### Quantization Is Truncation

In binary lossy compression, quantization is a separate lossy step that introduces structured rounding error:

```
Binary JPEG pipeline:
  DCT coefficients → divide by quantization table → round → entropy code
  Rounding destroys scale. Decoder must reconstruct it from the quantization table.
```

In ternary, quantization is trit truncation — dropping lower-order trits using `CVT`:

```
Ternary pipeline:
  DCT coefficients → cvt.t40.t10 → entropy code
  Scale is preserved exactly in the exponent field.
  Only mantissa precision is reduced.
```

The `cvt` instruction preserves the exponent field exactly and reduces only the mantissa. The scale of the value survives; only its precision decreases. Binary quantization has no equivalent — dividing by a quantization step destroys scale information and requires the decoder to reconstruct it from a side channel.

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

The `VMAC.t1` instruction handles the inner loop of both audio FIR filters and image convolution kernels:

```asm
; Audio FIR filter: output[n] = sum(coeffs[k] * input[n-k])
ACLR.t20
vmac_loop:
    VLOAD.t20  v_in,    r_input_ptr, 0
    VLOAD.t20  v_coeff, r_coeff_ptr, 0
    VMAC.t1    v_in, v_coeff           ; accumulate dot product into accumulator
    ; advance pointers
    BRN r_loop_cond, vmac_loop
ASTORE.t20 r_output
```

The same loop handles 2D image convolution (blurring, sharpening, edge detection) by treating the image as a stream of row vectors.

### Horizontal Reductions

The proposed `VSUM`, `VHMIN`, and `VHMAX` opcodes close the gap between element-wise vector operations and scalar results. Without them, computing the energy of an audio frame or the peak pixel value in a tile requires a manual scalar reduction loop:

```asm
VSUM.t20   r_energy, v_samples   ; sum all lanes → scalar energy value
VHMAX.t20  r_peak,   v_samples   ; peak sample value for normalization
```

---

## 5. Biologically-Aligned Ternary Color (Opponent-Trit Framebuffer)

This is where ternary moves from math to biology. The human eye does not perceive raw Red, Green, and Blue. After the retina, the visual cortex processes color using **Opponent Color Theory** (Hering's Theory): Red vs. Green, Blue vs. Yellow, and Black vs. White. Balanced ternary (+1, 0, -1) is the unique mathematical structure that maps to this model natively.

### The Opponent-Trit Pixel Format

A naïve 9-trit pixel (3 channels × 3 states) gives only 27 total states — far too coarse for real content. The correct granularity uses T10 per channel, packed into a single T40 word:

```
T10 Luma channel    (6 mantissa trits → 729 luminance levels, perceptually sufficient)
T10 Red-Green       (6 mantissa trits → 729 opponent steps)
T10 Blue-Yellow     (6 mantissa trits → 729 opponent steps)

Total: 30 trits per pixel → fits in one T40 word with 10 trits spare for alpha/metadata
```

Three T10 channels in one T40 word. The perceptual color space is preserved, the precision is sufficient for display use, and the entire pixel fits in a single native-width register.

Compared to binary RGB:

```
Binary 24-bit RGB:      16.7 million colors, perceptually redundant
Opponent T40 pixel:     729 × 729 × 729 ≈ 387 million opponent states
                        mapped to a perceptually uniform space
```

### Zero-Cost Color Inversion

To invert a binary framebuffer, the CPU must XOR every bit of every pixel:

```c
// Binary color inversion — 24 bit flips per pixel
for (int i = 0; i < pixel_count; i++)
    framebuffer[i] ^= 0xFFFFFF;
```

In the opponent-trit format, `TINV` flips the physical sign of every trit simultaneously. A full framebuffer inversion — dark mode, night mode, accessibility inversion — is a single pass with no arithmetic:

```asm
; Invert entire framebuffer — each T40 word is one pixel
tinv_loop:
    LOAD.t40   r1, r_fb_ptr, 0
    TINV.t40   r1, r1              ; physical polarity flip, all 40 trits
    STORE.t40  r1, r_fb_ptr, 0
    ; advance pointer
    BRN r_done, tinv_loop
```

Because `TINV` is a physical polarity flip in hardware rather than a logical operation, this executes at propagation delay speed rather than ALU speed.

### Dual-Array Delta Buffering

Using two arrays separates persistent state from change:

```
Array 1 (Baseline):  Full-resolution T20 image  — the complete current frame
Array 2 (Delta):     Low-resolution T5 motion buffer — only what changed
```

Instead of redrawing the whole screen, the GPU applies only the delta array. Because ternary has a genuine zero state — not a value representing zero, but physical absence of signal in a dual-rail implementation — the delta array is mostly zeros in a static scene.

In a dual-rail physical implementation where zero draws no power, a mostly-zero delta array is a mostly-off hardware state. Power consumption becomes directly proportional to the information density of the change, not the resolution of the screen.

### Fast Sign-Flipping for Sub-Pixel Modulation

In binary, switching a pixel between states requires charging and discharging a capacitor — a full voltage swing. In balanced ternary with differential signaling, the transition from `+1` to `-1` is a phase shift — swapping which wire is high, not charging a new voltage level.

This enables sub-pixel temporal dithering at kilohertz speeds with near-zero power cost. Oscillating a pixel between `+1` and `-1` at high frequency creates perceptually intermediate color states without the capacitive energy cost of binary switching. At 120Hz+ refresh rates, this reduces per-frame power consumption significantly.

### SYSCALL Integration

```
SYSCALL 10  — BLIT_OPPONENT(buffer_addr, width, height)
              Kernel accepts opponent-trit T40 pixel buffer
              and pushes directly to display hardware
```

Three T40 pixels pack into one 27-trit instruction word boundary naturally — the display pipeline is word-aligned at the architecture level.

---

## 6. Biologically-Aligned Ternary Audio (Opponent-Trit Audio)

The same biological alignment that motivates the opponent-trit framebuffer applies to audio. The human auditory system processes sound using opponent mechanisms — comparing pressure differentials across time and between ears. Balanced ternary maps to this structure directly.

### The Binaural Opponent-Trit Frame

Conventional binary stereo encodes two independent channels (L and R), introducing redundancy and requiring arithmetic for every spatial manipulation:

```
Sum (Mono)           = (L + R) / 2
Difference (Spatial) = (L - R) / 2
```

Widening, collapsing, or swapping channels requires continuous addition, subtraction, and shifting on every sample. The opponent-trit encoding separates these components at the data level:

```
Trit 1 — Luma-Acoustic (Mono Pressure):
  +1 : Air compression (positive pressure wave)
   0 : Ambient atmospheric baseline
  -1 : Air rarefaction (negative pressure wave)

Trit 2 — Spatial Opponent (Left vs. Right Differential):
  +1 : Spatial dominance in left ear (ITD/ILD shift left)
   0 : Perfect center (identical phase and level)
  -1 : Spatial dominance in right ear (ITD/ILD shift right)
```

### Vector ISA Integration

The opponent encoding connects directly to the vector ISA. A stereo frame in 9-trit spatial format packs naturally into vector registers. Spatial inversion of nine frames simultaneously:

```asm
; Swap L/R for 9 stereo frames in one vector instruction
; Each vector lane holds Trit 2 of one frame (the spatial opponent trit)
TINV.l1   v_spatial_trit    ; flip sign of all 9 spatial trits simultaneously
```

The combination of the opponent encoding and the vector width creates a media processing primitive with no binary equivalent at any instruction count. Binary spatial inversion requires per-sample arithmetic across the entire buffer. Ternary spatial inversion of nine frames is one instruction.

### Zero-Cost Spatial Operations

**Channel swap (L ↔ R):** Run `TINV` on Trit 2. The physical sign flip is the operation. No arithmetic, no temporary registers, no loop.

**Stereo collapse to mono:** Set Trit 2 to zero. In a dual-rail physical implementation, zero is the unpowered state. Mono playback physically disables the spatial circuit — power consumption drops by half because the spatial component is not driven.

**Stereo widening:** Scale Trit 2 by a factor. Because the spatial differential is isolated from the mono pressure component, widening does not cause the phase cancellation or hollowing artifacts common in binary spatializers. The mono content is untouched.

---

## 7. Direct-to-Transistor Balanced Class-D Amplification

Class-D amplifiers convert audio to a high-frequency pulse stream for efficient output. Binary Class-D has only two physical states: High (+V) and Low (-V). Even during silence, the amplifier must continuously toggle between rails at megahertz frequencies to average to zero, producing continuous idle power dissipation, high-frequency EMI, and audible idle hiss requiring low-pass filtering.

### The Three-State H-Bridge Driver

A ternary Class-D driver uses three states, driving an H-bridge speaker circuit directly:

```
+1 : Driver pulled to positive reference voltage (+Vref)
 0 : Driver completely disconnected / shorted to ground (High-Z or Ground)
-1 : Driver pulled to negative reference voltage (-Vref)
```

```
Ternary Audio Stream → Direct-to-Transistor Driver
                              |
               +--------------+--------------+
               |              |              |
           +1 path        0 path         -1 path
           +Vref          Ground         -Vref
               |              |              |
               +--------------+--------------+
                              |
                       Speaker Diaphragm
```

During silence, the audio stream produces a steady stream of zero trits. The driver transistors remain fully off. Power consumption drops to zero — no switching, no EMI, no hiss. This is the physical consequence of ternary's genuine zero state rather than an optimization layered on top of binary switching logic.

**Important qualification:** This zero-power property requires a dual-rail physical implementation where zero is an open-circuit or ground state, not a driven voltage level. Standard CMOS ternary implementations drive zero as a specific voltage and do not have this property. The claim is contingent on the physical layer design.

---

## 8. Perfect Phase-Reversal Active Noise Cancellation

Active Noise Cancellation captures ambient noise and plays back an inverted waveform to acoustically cancel pressure waves.

### The Binary Negation Problem

In two's complement binary, negating a value is asymmetric:

```
-x = ~x + 1
```

Negating the maximum negative value (e.g., -32768 in 16-bit) causes arithmetic overflow, clipping the waveform and introducing distortion. The operation requires multiple gate levels: inversion followed by carry-propagating addition.

### Ternary Negation

In balanced ternary, every number is natively symmetric around zero. Negation is an exact physical operation:

```
ANC Wave = TINV(Noise Wave)
```

The cancellation wave is generated by physically routing dual-rail lines through a polarity swapper — swapping which wire carries the positive signal. This occurs at propagation delay speed, bypassing the ALU entirely.

Because the ternary range is perfectly symmetric (e.g., -13 to +13 for a 3-trit amplitude), there are no boundary asymmetries. The cancellation wave can mirror the noise wave up to absolute maximum amplitude without clipping.

**Important qualification:** The end-to-end ANC latency still includes ADC sampling, digital processing, and DAC output — typically under 1 millisecond for in-ear monitors. The ternary advantage is specifically in the negation step, eliminating ~5-10 gate delays of carry-propagating addition. The total system latency improvement is real but modest relative to the ADC/DAC pipeline. Patent claims should be scoped to the negation mechanism, not total system latency.

---

## 9. Patent Considerations

The architecture described above contains several potentially patentable claims. Prior art search against Soviet ternary computing literature (Setun, 1958, and subsequent work) is essential before filing — claims must be specifically about the combination with modern ISA design rather than ternary media processing in general.

### Claim 1 — Biologically-Aligned Ternary Color Processing

**Non-obviousness argument:** Binary display patents focus on making RGB faster. This is a paradigm shift: mapping Hering's Opponent Color Theory directly to balanced ternary's three states at the ISA level. The `TINV` instruction becomes a hardware-accelerated color space transformation — zero-cost inversion is a physical property of the encoding, not a software optimization.

**Key elements to protect:**
- The opponent-color format: T10 per channel (Luma, Red-Green, Blue-Yellow) packed into a T40 word
- `TINV` as a display-space color inversion primitive that operates on the opponent encoding
- `SYSCALL 10 (BLIT_OPPONENT)` as the standard kernel interface
- The claim that balanced ternary is the unique mathematical structure enabling Hering's theory as a hardware instruction

### Claim 2 — Dual-Array Delta Power Architecture

**Non-obviousness argument:** Binary has no zero state — zero is a value, not the absence of signal. A binary screen must maintain a voltage floor even for black pixels. The dual-rail ternary zero state makes power consumption physically proportional to scene entropy.

**Key elements to protect:**
- Separation of baseline (T20) and delta (T5) framebuffer arrays
- Power consumption proportional to delta array information density
- Zero-power static display in dual-rail physical implementation
- Physical layer requirement must be specified in the claim

### Claim 3 — Differential Phase Dithering

**Non-obviousness argument:** Binary sub-pixel dithering requires capacitive charge/discharge per state transition. Ternary sign-flip (+1 to -1) is a phase shift on differential wires, not a capacitive event.

**Key elements to protect:**
- Sub-pixel modulation via differential phase reversal
- Intermediate color states without capacitive charging cycles
- Power-per-frame reduction at 120Hz+ refresh rates

### Claim 4 — Three-State Class-D Audio with True Quiescence

**Key elements to protect:**
- H-bridge speaker driver with three physical states (+Vref, Ground, -Vref)
- Zero idle power during silence via transistor off-state (High-Z)
- Zero idle EMI and hiss as physical consequences of the zero state
- Physical implementation requirement: dual-rail open-circuit zero state

---

## 10. The Ternary Media Stack Roadmap

| Component | Status | Notes |
|-----------|--------|-------|
| **Audio ring buffer** | Possible now | Needs TMOD opcode + SYSCALL 8/9 timer |
| **Image framebuffer** | Possible now | Needs host-side window (Vulkan or SDL) |
| **Triple framebuffer** | Possible now | TLADD + TSEL state machine, three buffers |
| **Opponent-trit pixel format** | Needs definition | T10×3 in T40 word; define in ARCHITECTURE_CONTRACTS.md |
| **FFT/DCT kernel** | Possible with vector ISA | VMAC + VSUM opcodes, benefits from compiler |
| **Timer interrupt** | Needs SYSCALL 8/9 | Host thread coordination for deadlines |
| **DMA transfer** | Needs SYSCALL extension | FENCE already in ISA |
| **Video decoding** | Needs compiler | Codecs too complex for hand-written assembly |
| **Binaural opponent-trit audio** | Needs format definition | 9-trit spatial frame spec |
| **Three-state Class-D driver** | Hardware only | Requires dual-rail physical implementation |
| **Ternary ANC** | Hardware only | Polarity swapper circuit required |
| **Ternary native codec** | Long term | Full compiler + vector ops + codec design |
| **GPU acceleration** | In progress | Vulkan bridge; same infrastructure |

---

## Strategic Connection

The Lock ABI, MMU, and atomic `TSTR` together enable hardware-accelerated video decode: one core decodes a frame while the GPU renders the previous one, with `TSTR` signaling buffer readiness across the boundary. The ternary three-state atomic makes the producer-consumer handshake cleaner than binary — no separate done/error flags, no ABA problem.

The opponent-trit framebuffer and binaural opponent-trit audio together establish a **Biological Media Standard** — an OS that processes light and sound in the same opponent structure the human brain uses to receive them. This is not a cosmetic difference from binary media stacks. The zero-cost inversion, the dual-rail power scaling, the phase-reversal ANC, and the vector-width spatial audio operations are all consequences of the same underlying architectural decision: three states, not two.

The next concrete step is defining the opponent-trit T40 pixel format and the 9-trit binaural frame format in `TERNARY_ARCHITECTURE_CONTRACTS.md` as the `v1` Media Standard. Everything else in this document builds on those two format definitions.