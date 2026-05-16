import torch
from safetensors.torch import load_file
import os
import struct
import math
import numpy as np
from tqdm import tqdm
import json
import sys

# =============================================================================
# Qwen 3.6 27B Ternary Quantizer (Memory-Efficient Shard Processor)
# =============================================================================

def quantize_weight_bitnet_a(tensor):
    if tensor.numel() == 0: return tensor.to(torch.int8), 1.0
    gamma = tensor.abs().mean().item()
    if gamma < 1e-9: return torch.zeros_like(tensor, dtype=torch.int8), 0.0
    scaled = tensor / gamma
    quantized = torch.round(torch.clamp(scaled, -1, 1)).to(torch.int8)
    return quantized, gamma

def pack_t2_ternary(q_weights):
    values = q_weights.flatten().numpy()
    trits = np.zeros_like(values, dtype=np.uint8)
    trits[values == 1] = 1
    trits[values == -1] = 3
    num_weights = len(trits)
    if num_weights % 4 != 0:
        padding = 4 - (num_weights % 4)
        trits = np.pad(trits, (0, padding), constant_values=0)
    trits = trits.reshape(-1, 4)
    packed = (trits[:, 0] | (trits[:, 1] << 2) | (trits[:, 2] << 4) | (trits[:, 3] << 6)).astype(np.uint8)
    return packed

def encode_t40_float_batch(values):
    if len(values) == 0: return np.array([], dtype=np.uint64)
    m = values.astype(np.float64)
    e = np.zeros(len(values), dtype=np.int32)
    for _ in range(100):
        mask_high = np.abs(m) > 1.5
        if not np.any(mask_high): break
        m[mask_high] /= 3.0
        e[mask_high] += 1
    for _ in range(100):
        mask_low = (np.abs(m) < 0.5) & (m != 0)
        if not np.any(mask_low): break
        m[mask_low] *= 3.0
        e[mask_low] -= 1
    trits = np.zeros((len(values), 40), dtype=np.int8)
    temp_exp = e.copy()
    for i in range(7):
        r = ((temp_exp + 1) % 3)
        trit = (r - 1).astype(np.int8)
        trits[:, 33 + i] = trit
        temp_exp = (temp_exp - trit.astype(np.int32)) // 3
    m_temp = m.copy()
    for i in range(32, -1, -1):
        trit = np.zeros(len(values), dtype=np.int8)
        trit[m_temp >= 0.5] = 1
        trit[m_temp <= -0.5] = -1
        trits[:, i] = trit
        m_temp -= trit.astype(np.float64)
        m_temp *= 3.0
    p3_powers = np.array([3**i for i in range(40)], dtype=np.uint64)
    return np.dot((trits.astype(np.uint64) + 1), p3_powers)

def process_tensor(name, param, output_dir):
    # Projections (Linear layers)
    is_proj = ".weight" in name and ("proj" in name or "mlp" in name)
    # Embeddings and Head
    is_embedding = "embed" in name or "lm_head" in name
    # Normalization layers
    is_norm = "norm" in name
    # Mamba state vectors and convolutions
    is_mamba = any(x in name for x in ["A_log", "conv1d", "dt_bias", "D"])
    
    if is_proj:
        weight_file = f"{name.replace('.', '_')}.t2"
        target_path = os.path.join(output_dir, weight_file)
        if not os.path.exists(target_path):
            q_weights, scale = quantize_weight_bitnet_a(param)
            packed = pack_t2_ternary(q_weights)
            with open(target_path, "wb") as f:
                f.write(packed.tobytes())
        else:
            scale = 0.0 # Or read it, but for manifest it's printed. Wait, scale is printed.
            # We must compute scale or we break manifest. But wait, manifest is recreated!
            q_weights, scale = quantize_weight_bitnet_a(param) # Have to compute scale unfortunately, or save it alongside.
        return f"{name},{weight_file},{scale},T2\n"
    
    elif is_norm or is_embedding or is_mamba:
        # Save all norms, embeddings, and Mamba state vectors as raw BF16 for the engine to load directly
        weight_file = f"{name.replace('.', '_')}.bf16"
        target_path = os.path.join(output_dir, weight_file)
        
        if not os.path.exists(target_path):
            # Cast to BF16 and save raw bytes (using view(int16) because numpy doesn't support bf16)
            with open(target_path, "wb") as f:
                f.write(param.to(torch.bfloat16).view(torch.int16).numpy().tobytes())
        return f"{name},{weight_file},1.0,BF16\n"
    else:
        return f"{name},SKIP,1.0,T40\n"

def convert_qwen(model_path, output_dir):
    if not os.path.exists(output_dir): os.makedirs(output_dir)
    if not os.path.exists(model_path):
        print(f"Error: Path not found: {model_path}"); sys.exit(1)
        
    manifest = []
    
    if os.path.isdir(model_path):
        shards = []
        for root, dirs, files in os.walk(model_path):
            for f in files:
                if f.endswith(".safetensors") and not f.endswith(".incomplete"):
                    shards.append(os.path.join(root, f))
        shards.sort()
        
        print(f"Processing {len(shards)} shards from {model_path}...")
        for shard_path in shards:
            shard_name = os.path.basename(shard_path)
            print(f"Loading shard {shard_name}...")
            weights = load_file(shard_path)
            for name, param in tqdm(weights.items(), desc=f"Shard {shard_name}"):
                entry = process_tensor(name, param, output_dir)
                manifest.append(entry)
            del weights # Free memory immediately
            torch.cuda.empty_cache() if torch.cuda.is_available() else None
    else:
        weights = load_file(model_path)
        for name, param in tqdm(weights.items()):
            manifest.append(process_tensor(name, param, output_dir))

    with open(os.path.join(output_dir, "manifest.csv"), "w") as f:
        f.write("parameter_name,filename,scale,mode\n")
        f.writelines(manifest)
    print(f"\n[Success] Quantization Complete.")

if __name__ == "__main__":
    model_path = sys.argv[1] if len(sys.argv) > 1 else "qwen3.627b_weights/raw"
    output_dir = sys.argv[2] if len(sys.argv) > 2 else "qwen3.627b_weights/converted_qwen"
    convert_qwen(model_path, output_dir)
