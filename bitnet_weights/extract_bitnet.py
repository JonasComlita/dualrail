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

def encode_t50_batch(values):
    """
    Encodes a batch of floats as Trit VM T50 (LongTriple).
    Returns two numpy arrays (lo, hi).
    """
    if len(values) == 0:
        return np.array([], dtype=np.uint64), np.array([], dtype=np.uint64)

    # 1. Normalize mantissa and extract exponent
    # This is tricky to vectorize perfectly like the C++ version,
    # but we can approximate and then fix up.
    m = values.astype(np.float64)
    e = np.zeros_like(m, dtype=np.int32)
    
    # Simple iterative normalization
    for _ in range(100): # Should be enough for any reasonable float
        mask_high = np.abs(m) > 1.5
        if not np.any(mask_high): break
        m[mask_high] /= 3.0
        e[mask_high] += 1
        
    for _ in range(100):
        mask_low = (np.abs(m) < 0.5) & (m != 0)
        if not np.any(mask_low): break
        m[mask_low] *= 3.0
        e[mask_low] -= 1

    # 2. Encode Exponent (9 trits, indices 41-49)
    trits = np.zeros((len(values), 50), dtype=np.int8)
    temp_exp = e.copy()
    for i in range(9):
        r = (temp_exp + 1) % 3
        trit = r - 1
        trits[:, 41 + i] = trit
        temp_exp = (temp_exp - trit) // 3

    # 3. Encode Mantissa (41 trits, indices 0-40)
    m_temp = m.copy()
    for i in range(40, -1, -1):
        trit = np.zeros_like(m_temp, dtype=np.int8)
        trit[m_temp >= 0.5] = 1
        trit[m_temp <= -0.5] = -1
        trits[:, i] = trit
        
        m_temp -= trit
        m_temp *= 3.0

    # 4. Pack into 128-bit integer
    # result = sum((trit + 1) * 3^i)
    # Using np.dot on object arrays allows vectorizing large integer arithmetic in C.
    def encode_t40_batch(values):
        m = values.astype(np.float64)
        e = np.zeros(len(values), dtype=np.int32)
        
        # Normalization — same as before
        for _ in range(60):
            mask_high = np.abs(m) > 1.5
            if not np.any(mask_high): break
            m[mask_high] /= 3.0
            e[mask_high] += 1
        for _ in range(60):
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

        # Pack into uint64 — NO object arrays, pure numpy
        p3_powers = np.array([3**i for i in range(40)], dtype=np.uint64)
        result = np.dot(
            (trits.astype(np.uint64) + 1),
            p3_powers
        )  # result is uint64, fits natively, no hi word needed

        return result  # single uint64 array, not lo/hi pair

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
            print(f"Quantizing {name} to L50...")
            q_weights, scale = quantize_to_ternary(param)
            values = q_weights.flatten().numpy()
            
            # TERNARY PACKING (L50)
            trits = (values + 1).clip(0, 2).astype(np.int8)
            
            # Calculate number of L50 blocks
            num_weights = len(trits)
            
            # Pad trits to multiple of 50
            if num_weights % 50 != 0:
                trits = np.pad(trits, (0, 50 - (num_weights % 50)), constant_values=1) # Pad with 0-trit (encoded as 1)
            
            trits = trits.reshape(-1, 50)
            
            # Pack using 2-bit-per-trit (Base-4) to match TritLane50
            packed_vals = np.zeros(len(trits), dtype=object)
            for i in range(50):
                # t_i + 1 maps {-1, 0, 1} to {0, 1, 2} (fits in 2 bits)
                packed_vals |= (trits[:, i].astype(object) << (2 * i))
            
            lo = np.array([val & 0xFFFFFFFFFFFFFFFF for val in packed_vals], dtype=np.uint64)
            hi = np.array([(val >> 64) & 0xFFFFFFFFFFFFFFFF for val in packed_vals], dtype=np.uint64)
            
            weight_file = f"{name.replace('.', '_')}.l50"
            with open(os.path.join(output_dir, weight_file), "wb") as f:
                for i in range(len(lo)):
                    f.write(lo[i].tobytes())
                    f.write(hi[i].tobytes())
            
            manifest.append(f"{name},{weight_file},{scale},L50\n")
        else:
            # SKIP: Large high-precision tensors are read directly from safetensors by the host
            if param.numel() > 1000000:
                print(f"Skipping T50 encoding for large tensor {name} (will read from safetensors)...")
                manifest.append(f"{name},SKIP,1.0,T50\n")
                continue

            weight_file = f"{name.replace('.', '_')}.t50"
            target_path = os.path.join(output_dir, weight_file)
            
            if os.path.exists(target_path) and os.path.getsize(target_path) > 0:
                print(f"Skipping {name} (already exists)...")
                manifest.append(f"{name},{weight_file},1.0,T50\n")
                continue

            print(f"Encoding {name} to T50 (batch)...")
            # FIX: BFloat16 is not supported by numpy natively, cast to float32
            flat_param = param.to(torch.float32).flatten().numpy()
            
            # Process in batches to manage memory
            batch_size = 1000000
            with open(target_path, "wb") as f:
                for i in tqdm(range(0, len(flat_param), batch_size)):
                    batch = flat_param[i:i+batch_size]
                    lo, hi = encode_t50_batch(batch)
                    
                    # Interleave lo, hi for binary storage (Little-Endian 128-bit)
                    packed = np.stack([lo, hi], axis=1).flatten()
                    f.write(packed.tobytes())
                    
            manifest.append(f"{name},{weight_file},1.0,T50\n")
        
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
