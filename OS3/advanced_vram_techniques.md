# Advanced VRAM & Virtual GPU Architectures for Ternary VM

Our current VRAM implementation maps a flat $80 \times 60$ pixel array directly to virtual memory space in `dmem`. While simple and functional, a flat framebuffer suffers from two key bottlenecks:
1. **CPU Computation Overhead**: Drawing shapes, lines, or clearing the canvas requires the VM CPU to perform multiplication loops (`y * width + x`) for every single pixel.
2. **Synchronous Rendering Tearing**: Writing directly to the active frame memory while the host Win32 thread is reading it creates intermediate states and screen tearing.

Here are four advanced, production-grade VRAM and graphics rendering architectures we can implement in our Ternary VM:

---

## 1. Hardware 2D Blitter Engine (Virtual GPU Coprocessor) - *Highly Recommended*

Instead of the CPU plotting pixels one-by-one, we delegate high-frequency drawing tasks to a virtual **Coprocessor / GPU Blitter**.

```
+------------------+                    +-----------------------+
|  Ternary CPU     |                    |  Virtual GPU          |
|  * Write X1, Y1  |                    |  * Native GDI/Vulkan  |
|  * Write X2, Y2  | === MMIO/CSRs ===> |  * Solid Fill         |
|  * Write CMD     |                    |  * Bresenham Line     |
+------------------+                    +-----------------------+
```

### How it Works
We map dedicated drawing registers to the VM's CSR or memory-mapped space:
* `CSR_GPU_X1`, `CSR_GPU_Y1`, `CSR_GPU_X2`, `CSR_GPU_Y2`
* `CSR_GPU_COLOR`
* `CSR_GPU_CMD`

When the CPU writes to `CSR_GPU_CMD`, the host C++ simulator catches the command and executes it instantly using native host-side code:
* `CMD_CLEAR_SCREEN` (0) $\rightarrow$ Instantly zeros VRAM array.
* `CMD_FILL_RECT` (1) $\rightarrow$ Fills bounding box $(X_1, Y_1) \rightarrow (X_2, Y_2)$ with color.
* `CMD_DRAW_LINE` (2) $\rightarrow$ Draws a vector line using the host's optimized C++ Bresenham routine.

### Why it's Better
* **Speed**: A full canvas clear is reduced from a $4,800$-instruction assembly loop down to **2 instructions**!
* **Low-Power**: Offloads compute burden from the emulated CPU to the host system.

---

## 2. Page-Flipped (Double-Buffered) VRAM

To completely eliminate flickering and half-drawn frames, we split our VRAM allocation into two distinct memory buffers.

```
       Front Buffer (Page 0) -------------> Painted on Screen
                 ^
             [ FLIP! ] (Swap Pointers)
                 v
       Back Buffer  (Page 1) <------------- VM Writes Here
```

### How it Works
1. We allocate two VRAM segments in `dmem`:
   * **VRAM Page 0**: Address `50000` to `54799`.
   * **VRAM Page 1**: Address `55000` to `59799`.
2. We map a new control register: `CSR_VRAM_PAGE`.
3. The VM draws all of its graphical components onto the inactive Back Buffer (e.g. Page 1).
4. Once the frame is fully assembled, the VM writes the target page index to the CSR (`csrw vram_page, 0`).
5. The host instantly flips the active presentation pointer.

### Why it's Better
* **Tear-Free**: The user never sees half-drawn buttons, intermediate scanlines, or flicker.
* **Smooth 60 FPS**: Animation transitions look incredibly professional and fluid.

---

## 3. Hardware Sprite Engine (OAM - Object Attribute Memory)

Standard in retro gaming hardware (NES, Sega Genesis), this technique renders independent overlay items without altering the background VRAM canvas.

```
+-------------------------------------------------------------+
| Active Screen Canvas                                        |
|                                                             |
|   [Background VRAM Grid]                                    |
|                                                             |
|          +--------------------+                             |
|          | Sprite #0 (Cursor) |                             |
|          | Pos: (X, Y)        |                             |
|          +--------------------+                             |
+-------------------------------------------------------------+
```

### How it Works
* We declare a dedicated block of memory (e.g., 64 words) called **Object Attribute Memory (OAM)**.
* Each sprite struct in OAM contains:
  * Word 0: $X$ screen offset.
  * Word 1: $Y$ screen offset.
  * Word 2: Tile/Glyph index & palette attributes.
* To move the mouse cursor or a window panel, the VM CPU simply changes the coordinate registers in OAM. The host composites the cursor overlay on the fly during `WM_PAINT`.

### Why it's Better
* **Zero Canvas Cleanup**: Moving an object doesn't require saving the background underneath, drawing, and cleaning it up later. 
* **Ultra-Lightweight**: Requires negligible CPU memory writes.

---

## 4. Tile Grid & Attribute Memory (Text Mode)

Used in early DOS PCs and classic terminal interfaces.

### How it Works
* The VRAM is not mapped as individual pixels. Instead, it is a $40 \times 30$ grid of **Text Tiles** (1,200 words).
* **Tile Memory**: Holds the ASCII or custom character glyph index (0-255).
* **Attribute Memory**: Holds foreground and background color combinations.
* The host renders the matching glyph images at those coordinates.

### Why it's Better
* **Minimal Memory**: Reduces visual memory usage by over $75\%$.
* **Fast GUI**: Instantly prints complete text layouts, borders, and menus using standard characters.

---

### 🌌 We can absolutely integrate them all! 

In fact, combining these techniques creates a **highly advanced, production-grade graphics engine** where they work in perfect harmony. They do not conflict; they solve completely different parts of the visual pipeline:

```
+---------------------------------------------------------------------------------+
| GPU Coprocessor (Blitter) ---> Renders shapes/lines into Back Buffer (Page 1)   |
|                                                                    |            |
|                                                                 [FLIP!]         |
|                                                                    v            |
| Presenter (Win32 Host GDI) <--- Composites Sprites + reads Front Buffer (Page 0)|
+---------------------------------------------------------------------------------+
```

Here is how we can implement and combine them together right now:

---

### 1. Page-Flipped Double-Buffering + The Blitter
Right now, the VM draws directly to VRAM starting at `50000` while the host GDI thread is presenting it. If the program starts rendering a complex frame, the user might see half-drawn items.
* **The Fusion**: We allocate a second VRAM buffer (e.g. Page 0 at `50000`, Page 1 at `55000`) and add `CSR_GPU_PAGE` (CSR 35) to toggle which page is currently presented by the host and which page is painted to by the GPU Blitter.
* **Result**: Perfectly flicker-free, tear-free, high-speed 60 FPS presenting!

### 2. Hardware Sprite Engine (OAM) + The Framebuffer
Right now, if we want to draw a mouse cursor or a floating window panel, we have to manually composite it, which is complex.
* **The Fusion**: We define a dedicated segment in memory or registers called **Object Attribute Memory (OAM)** (e.g., tracking up to 8 sprites, each with $X, Y$, character graphic, and color). 
* **Result**: To draw or move the mouse cursor or custom window controls, the VM simply writes a single register coordinate: `csrw sprite0_x, r10`. The host presentation layer blends the sprite overlay on top of the VRAM background natively.

### 3. Text Mode Tile Grid Toggle
* **The Fusion**: We introduce a display mode CSR `CSR_GPU_MODE`. If set to `0`, it renders a high-speed terminal character tile grid (for BIOS, shell consoles, and system logs). If set to `1`, it unlocks the 2D graphics framebuffer.

---

### 🛠️ Would you like us to integrate them?
If you'd like to elevate our graphics substrate even further, we can implement **Page-Flipped VRAM** and a **Hardware Sprite Engine** right now in a single, unified sweep! 

Let me know which ones you want to add, and I'll sketch the next upgrade!