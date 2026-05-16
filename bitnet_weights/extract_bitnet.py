import torch
from safetensors.torch import load_file
import os
import struct
import math
import numpy as np
from tqdm import tqdm

def quantize_to_ternary(tensor):
    """
    BitNet b1.58 quantization for projection layers.
    W_q = round(clip(W / gamma, -1, 1))
    where gamma = max(abs(W))
    """
    gamma = tensor.abs().mean()
    if gamma < 1e-9:
        return tensor.to(torch.int8), 0.0
    
    scaled = tensor / gamma
    quantized = torch.round(torch.clamp(scaled, -1, 1)).to(torch.int8)
    
    return quantized, gamma.item()

def encode_t40_batch(values):
    """
    Encodes a batch of floats as Trit VM T40 (Triple).
    Uses positional base-3: 33 trits mantissa, 7 trits exponent.
    Fits in a single uint64.
    """
    if len(values) == 0:
        return np.array([], dtype=np.uint64)

    m = values.astype(np.float64)
    e = np.zeros(len(values), dtype=np.int32)
    
    # Normalization
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
    
    # Exponent: 7 trits, indices 33-39
    temp_exp = e.copy()
    for i in range(7):
        r = ((temp_exp + 1) % 3)
        trit = (r - 1).astype(np.int8)
        trits[:, 33 + i] = trit
        temp_exp = (temp_exp - trit.astype(np.int32)) // 3

    # Mantissa: 33 trits, indices 0-32
    m_temp = m.copy()
    for i in range(32, -1, -1):
        trit = np.zeros(len(values), dtype=np.int8)
        trit[m_temp >= 0.5] = 1
        trit[m_temp <= -0.5] = -1
        trits[:, i] = trit
        m_temp -= trit.astype(np.float64)
        m_temp *= 3.0

    # Pack into uint64
    p3_powers = np.array([3**i for i in range(40)], dtype=np.uint64)
    result = np.dot(
        (trits.astype(np.uint64) + 1),
        p3_powers
    )

    return result

def convert_bitnet_to_trit(model_path, output_dir):
    if not os.path.exists(output_dir):
        os.makedirs(output_dir)
        
    print(f"Loading weights from {model_path}...")
    weights = load_file(model_path)
    
    manifest = []

    for name, param in weights.items():
        # Determine if this layer should be ternary or high-precision
        is_ternary = "proj.weight" in name.lower()
        
        if is_ternary:
            # Quantize to {-1, 0, 1}
            print(f"Quantizing {name} to T40 (packed)...")
            q_weights, scale = quantize_to_ternary(param)
            values = q_weights.flatten().numpy()
            
            # TERNARY PACKING (T40)
            trits = (values + 1).clip(0, 2).astype(np.uint64)
            
            # Calculate number of T40 blocks
            num_weights = len(trits)
            
            # Pad trits to multiple of 40
            if num_weights % 40 != 0:
                trits = np.pad(trits, (0, 40 - (num_weights % 40)), constant_values=1) # Pad with 0-trit (encoded as 1)
            
            trits = trits.reshape(-1, 40)
            
            # Pack using base-3 positional encoding
            p3_powers = np.array([3**i for i in range(40)], dtype=np.uint64)
            packed_vals = np.dot(trits, p3_powers)
            
            weight_file = f"{name.replace('.', '_')}.t40"
            with open(os.path.join(output_dir, weight_file), "wb") as f:
                f.write(packed_vals.tobytes())
            
            manifest.append(f"{name},{weight_file},{scale},T40\n")
        else:
            # SKIP: Large high-precision tensors are read directly from safetensors by the host
            if param.numel() > 1000000:
                print(f"Skipping T40 encoding for large tensor {name} (will read from safetensors)...")
                manifest.append(f"{name},SKIP,1.0,T40\n")
                continue
 
            weight_file = f"{name.replace('.', '_')}.t40"
            target_path = os.path.join(output_dir, weight_file)
            
            if os.path.exists(target_path) and os.path.getsize(target_path) > 0:
                print(f"Skipping {name} (already exists)...")
                manifest.append(f"{name},{weight_file},1.0,T40\n")
                continue
 
            print(f"Encoding {name} to T40 (batch)...")
            # FIX: BFloat16 is not supported by numpy natively, cast to float32
            flat_param = param.to(torch.float32).flatten().numpy()
            
            # Process in batches to manage memory
            batch_size = 1000000
            with open(target_path, "wb") as f:
                for i in tqdm(range(0, len(flat_param), batch_size)):
                    batch = flat_param[i:i+batch_size]
                    packed = encode_t40_batch(batch)
                    f.write(packed.tobytes())
                    
            manifest.append(f"{name},{weight_file},1.0,T40\n")
        
    # Write manifest
    manifest_path = os.path.join(output_dir, "manifest.csv")
    print(f"Writing manifest to {manifest_path}...")
    with open(manifest_path, "w") as f:
        f.write("parameter_name,filename,scale,mode\n")
        f.writelines(manifest)
        
    print(f"Conversion complete. Weights and manifest saved to {output_dir}")

if __name__ == "__main__":
    import sys
    if len(sys.argv) < 2:
        print("Usage: python extract_bitnet.py <path_to_safetensors> [output_dir]")
        sys.exit(1)
    
    model_path = sys.argv[1]
    output_dir = sys.argv[2] if len(sys.argv) > 2 else "converted"
    
    convert_bitnet_to_trit(model_path, output_dir)
