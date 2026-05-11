# BitNet b1.58 Loader for Trit VM

This folder contains the bridge between Microsoft's BitNet b1.58 PyTorch models and the Trit Ternary VM.

## Files
1. `extract_bitnet.py`: A Python script to extract and quantize `bf16` BitNet weights into raw ternary integers (`-1`, `0`, `1`).
2. `bitnet_loader.h`: A C++ header-only utility to load the exported weights into the VM's `DMEM` using `L1` (Lane 1) mode.

## Usage Workflow

### Step 1: Export from Python
Run the extraction script on a downloaded `safetensors` file.
```bash
python extract_bitnet.py path/to/model.safetensors
```
This will generate many `.bin` files and a `manifest.csv`.

### Step 2: Load in C++
Include the loader in your C++ project to populate the VM state.

```cpp
#include "bitnet_weights/bitnet_loader.h"

// 1. Initialize loader
sandbox::bitnet::BitNetLoader loader("bitnet_weights/");
loader.loadManifest();

// 2. Load weights into a specific address in DMEM
int baseAddr = 10000;
loader.loadLayerToVM(state, "model.layers.0.self_attn.q_proj.weight", baseAddr);

// 3. Get the scaling factor
double scale = loader.getScale("model.layers.0.self_attn.q_proj.weight");
```

### Step 3: Execute
Now you can use `matmulT1Dot` on that `baseAddr` to perform 1.58-bit matrix multiplication. Remember to multiply the final output by the `scale` value retrieved from the loader to restore the original magnitude.
