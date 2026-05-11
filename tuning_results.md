## SYCL Geometry Tuning Results
- **Device:** `AMD Ryzen 9 3900X 12-Core Processor            `
- **Backend Status:** CPU OpenCL/default SYCL path. AMD GPU SYCL via Codeplay AMD plugin has not been installed or validated; Codeplay's AMD plugin path targets Linux/ROCm rather than this Windows host.
- **Payload Count:** `10000000` lanes
- **Operation:** `addLane64 (20 trits)`

| Local Size | Execution Time (us) |
| ---------- | ------------------- |
| Auto | 841258 |
| 32 | 1718362 |
| 64 | 1697517 |
| 128 | 1697609 |
| 256 | 1691751 |
| 512 | 1696047 |
| 1024 | 1699355 |


## SYCL Geometry Tuning Results (Post-UInt128 Optimizations)
- **Device:** `AMD Ryzen 9 3900X 12-Core Processor            `
- **Backend Status:** CPU OpenCL/default SYCL path. This is still not an AMD GPU SYCL run.
- **Payload Count:** `10000000` lanes
- **Operation:** `addLane64 (20 trits)`

| Local Size | Execution Time (us) |
| ---------- | ------------------- |
| Auto | 895109 |
| 32 | 1761061 |
| 64 | 2025556 |
| 128 | 1799758 |
| 256 | 1813226 |
| 512 | 1791342 |
| 1024 | 1726797 |

### SYCL CPU Geometry Comparison

The UInt128/native hot-path optimization did not improve the CPU OpenCL lane
kernel timing. `Auto` remains the best local-size choice on this host, but the
post-optimization CPU OpenCL run was slightly slower. Treat this as CPU backend
noise/regression data, not AMD GPU behavior.

| Local Size | Old Time (us) | New Time (us) | Speedup |
| ---------- | ------------- | ------------- | ------- |
| Auto | 841258 | 895109 | 0.94x |
| 32 | 1718362 | 1761061 | 0.98x |
| 64 | 1697517 | 2025556 | 0.84x |
| 128 | 1697609 | 1799758 | 0.94x |
| 256 | 1691751 | 1813226 | 0.93x |
| 512 | 1696047 | 1791342 | 0.95x |
| 1024 | 1699355 | 1726797 | 0.98x |

## CPU UInt128 / Native Hot-Path Benchmark

- **Device:** `AMD Ryzen 9 3900X 12-Core Processor`
- **Compiler:** `g++.exe (Rev8, Built by MSYS2 project) 15.2.0`
- **Build:** `g++ -std=c++20 -O3 -DNDEBUG benchmark.cpp -o benchmark.exe`
- **Run:** `.\benchmark.exe`
- **Benchmark scale:** `1`
- **Sink:** `0xa2f99029460268a3`

### UInt128 Microbenchmarks

These rows are the new low-level baselines for the stable `{lo, hi}` ABI plus
optional internal fast paths.

| Benchmark | ns/unit | Units/sec | Checksum |
| --------- | ------: | --------: | -------- |
| `add` | 1.81 | 552158249 | `0x2020972d42c700ca` |
| `sub` | 1.64 | 610948192 | `0x5f909cc4f7d72171` |
| `mul` | 2.77 | 361581921 | `0x15ba73253da6b5d2` |
| `div` | 11.35 | 88079536 | `0xfb56cb521c1d4cac` |
| `mod` | 11.37 | 87937020 | `0xc2c8942edcf89142` |
| `divSmall` | 6.44 | 155174086 | `0x6b32bccd0291376e` |
| `modSmall` | 6.27 | 159464200 | `0x266e0dd74bb4b23f` |
| `divModSmall` | 5.61 | 178377125 | `0x6b32bccd0291376e` |
| `pow3UInt128` | 15.15 | 66024578 | `0x837f75d29a988ff4` |
| `LongTriple pack` | 55.88 | 17895170 | `0x241a089e3077cdcb` |
| `LongTriple unpack` | 267.09 | 3744098 | `0xd32d0cd2f6cc9e6a` |

### High-Level Comparison To Phase 4 Baseline

Baseline values are from `optimization_baseline.md`, before the UInt128 hot-path
tranche. Speedup is old `ns/unit` divided by new `ns/unit`.

| Benchmark | Old ns/unit | New ns/unit | Speedup | New checksum |
| --------- | ----------: | ----------: | ------: | ------------ |
| `T50 add` | 39571.10 | 1101.61 | 35.92x | `0x6214e4db184ca64c` |
| `T50 multiply` | 61955.05 | 2645.54 | 23.42x | `0xf11546d81be2c3e9` |
| `T50 divide` | 46244.78 | 705.41 | 65.56x | `0xced7cef435b93466` |
| `T50 sqrt` | 425363.20 | 12977.87 | 32.78x | `0x2a2789558647a8b7` |
| `T50 roundtrip` | 15310.54 | 520.27 | 29.43x | `0x797650f53a003252` |
| `T50 tritwiseAdd` | 279.70 | 279.69 | 1.00x | `0xb1921de298a6a811` |
| `TritLane50 add raw` | 221.04 | 226.79 | 0.97x | `0x870b8b7b786f2cbc` |
| `VM dispatch loop` | 244.95 | 72.78 | 3.37x | `0x9a0812482c2094fd` |

Notes:

- The big wins come from keeping `UInt128 { lo, hi }` stable while using
  internal host-native fast paths and fused divide/remainder-by-3 helpers.
- Lane raw kernels are mostly unchanged because they are lane/wire operations,
  not positional UInt128 arithmetic.
- The production header no longer emits the earlier `__int128` pedantic
  warnings; remaining `__int128` warnings are test-reference code only.

## Phase 5D Tiny Transformer VM Runtime Fixture

| Runtime | DMEM Words | Scalar Ops | DMEM Loads | DMEM Stores | Runtime (us) | Max Error |
| ------- | ---------: | ---------: | ---------: | ----------: | -----------: | --------: |
| VM tensor runtime | 138 | 745 | 144 | 36 | 3243 | 3.49523e-10 |

## Phase 5A CUDA Raw Lane Conformance Results

| Backend | Device | Compiler | Operation | Width | Count | Block/Local Size | Checksum | Runtime (us) | Execution | Result |
| ------- | ------ | -------- | --------- | ----- | -----: | ---------------- | -------- | -----------: | --------- | ------ |
| CUDA | `none` | `nvcc 13.2` | `all` | `all` | 0 | `n/a` | `n/a` | 0 | not available | skipped: no CUDA device |

## Phase 5A SYCL Raw Lane Conformance Results

| Backend | Device | Compiler | Operation | Width | Count | Block/Local Size | Checksum | Runtime (us) | Execution | Result |
| ------- | ------ | -------- | --------- | ----- | -----: | ---------------- | -------- | -----------: | --------- | ------ |
| SYCL | `AMD Ryzen 9 3900X 12-Core Processor            ` | `IntelLLVM 20250101` | `neg` | `L1` | 0 | `auto` | `0x14650fb0739d0383` | 32 | CPU fallback | pass |
| SYCL | `AMD Ryzen 9 3900X 12-Core Processor            ` | `IntelLLVM 20250101` | `neg` | `L1` | 0 | `64` | `0x14650fb0739d0383` | 17 | CPU fallback | pass |
| SYCL | `AMD Ryzen 9 3900X 12-Core Processor            ` | `IntelLLVM 20250101` | `neg` | `L1` | 0 | `256` | `0x14650fb0739d0383` | 21 | CPU fallback | pass |
| SYCL | `AMD Ryzen 9 3900X 12-Core Processor            ` | `IntelLLVM 20250101` | `neg` | `L1` | 1 | `auto` | `0x0a087f6c99afbda1` | 104 | CPU fallback | pass |
| SYCL | `AMD Ryzen 9 3900X 12-Core Processor            ` | `IntelLLVM 20250101` | `neg` | `L1` | 1 | `64` | `0x0a087f6c99afbda1` | 97 | CPU fallback | pass |
| SYCL | `AMD Ryzen 9 3900X 12-Core Processor            ` | `IntelLLVM 20250101` | `neg` | `L1` | 1 | `256` | `0x0a087f6c99afbda1` | 103 | CPU fallback | pass |
| SYCL | `AMD Ryzen 9 3900X 12-Core Processor            ` | `IntelLLVM 20250101` | `neg` | `L1` | 127 | `auto` | `0x08287cb69c3a3960` | 116 | CPU fallback | pass |
| SYCL | `AMD Ryzen 9 3900X 12-Core Processor            ` | `IntelLLVM 20250101` | `neg` | `L1` | 127 | `64` | `0x08287cb69c3a3960` | 47 | CPU fallback | pass |
| SYCL | `AMD Ryzen 9 3900X 12-Core Processor            ` | `IntelLLVM 20250101` | `neg` | `L1` | 127 | `256` | `0x08287cb69c3a3960` | 35 | CPU fallback | pass |
| SYCL | `AMD Ryzen 9 3900X 12-Core Processor            ` | `IntelLLVM 20250101` | `neg` | `L1` | 256 | `auto` | `0x35e4e590e9545623` | 81 | CPU fallback | pass |
| SYCL | `AMD Ryzen 9 3900X 12-Core Processor            ` | `IntelLLVM 20250101` | `neg` | `L1` | 256 | `64` | `0x35e4e590e9545623` | 42 | CPU fallback | pass |
| SYCL | `AMD Ryzen 9 3900X 12-Core Processor            ` | `IntelLLVM 20250101` | `neg` | `L1` | 256 | `256` | `0x35e4e590e9545623` | 29 | CPU fallback | pass |
| SYCL | `AMD Ryzen 9 3900X 12-Core Processor            ` | `IntelLLVM 20250101` | `neg` | `L1` | 1009 | `auto` | `0x9e0a489baf2fefe0` | 116 | CPU fallback | pass |
| SYCL | `AMD Ryzen 9 3900X 12-Core Processor            ` | `IntelLLVM 20250101` | `neg` | `L1` | 1009 | `64` | `0x9e0a489baf2fefe0` | 59 | CPU fallback | pass |
| SYCL | `AMD Ryzen 9 3900X 12-Core Processor            ` | `IntelLLVM 20250101` | `neg` | `L1` | 1009 | `256` | `0x9e0a489baf2fefe0` | 59 | CPU fallback | pass |
| SYCL | `AMD Ryzen 9 3900X 12-Core Processor            ` | `IntelLLVM 20250101` | `neg` | `L1` | 65536 | `auto` | `0x16cc9d5b5ca266a0` | 84 | CPU fallback | pass |
| SYCL | `AMD Ryzen 9 3900X 12-Core Processor            ` | `IntelLLVM 20250101` | `neg` | `L1` | 65536 | `64` | `0x16cc9d5b5ca266a0` | 96 | CPU fallback | pass |
| SYCL | `AMD Ryzen 9 3900X 12-Core Processor            ` | `IntelLLVM 20250101` | `neg` | `L1` | 65536 | `256` | `0x16cc9d5b5ca266a0` | 96 | CPU fallback | pass |
| SYCL | `AMD Ryzen 9 3900X 12-Core Processor            ` | `IntelLLVM 20250101` | `add` | `L1` | 0 | `auto` | `0x14650fb0739d0383` | 96 | CPU fallback | pass |
| SYCL | `AMD Ryzen 9 3900X 12-Core Processor            ` | `IntelLLVM 20250101` | `add` | `L1` | 0 | `64` | `0x14650fb0739d0383` | 30 | CPU fallback | pass |
| SYCL | `AMD Ryzen 9 3900X 12-Core Processor            ` | `IntelLLVM 20250101` | `add` | `L1` | 0 | `256` | `0x14650fb0739d0383` | 30 | CPU fallback | pass |
| SYCL | `AMD Ryzen 9 3900X 12-Core Processor            ` | `IntelLLVM 20250101` | `add` | `L1` | 1 | `auto` | `0x29034675a49f07c2` | 71 | CPU fallback | pass |
| SYCL | `AMD Ryzen 9 3900X 12-Core Processor            ` | `IntelLLVM 20250101` | `add` | `L1` | 1 | `64` | `0x29034675a49f07c2` | 54 | CPU fallback | pass |
| SYCL | `AMD Ryzen 9 3900X 12-Core Processor            ` | `IntelLLVM 20250101` | `add` | `L1` | 1 | `256` | `0x29034675a49f07c2` | 41 | CPU fallback | pass |
| SYCL | `AMD Ryzen 9 3900X 12-Core Processor            ` | `IntelLLVM 20250101` | `add` | `L1` | 127 | `auto` | `0x3156b7f9f5c3c643` | 56 | CPU fallback | pass |
| SYCL | `AMD Ryzen 9 3900X 12-Core Processor            ` | `IntelLLVM 20250101` | `add` | `L1` | 127 | `64` | `0x3156b7f9f5c3c643` | 119 | CPU fallback | pass |
| SYCL | `AMD Ryzen 9 3900X 12-Core Processor            ` | `IntelLLVM 20250101` | `add` | `L1` | 127 | `256` | `0x3156b7f9f5c3c643` | 46 | CPU fallback | pass |
| SYCL | `AMD Ryzen 9 3900X 12-Core Processor            ` | `IntelLLVM 20250101` | `add` | `L1` | 256 | `auto` | `0xc830b3b217a09123` | 55 | CPU fallback | pass |
| SYCL | `AMD Ryzen 9 3900X 12-Core Processor            ` | `IntelLLVM 20250101` | `add` | `L1` | 256 | `64` | `0xc830b3b217a09123` | 56 | CPU fallback | pass |
| SYCL | `AMD Ryzen 9 3900X 12-Core Processor            ` | `IntelLLVM 20250101` | `add` | `L1` | 256 | `256` | `0xc830b3b217a09123` | 40 | CPU fallback | pass |
| SYCL | `AMD Ryzen 9 3900X 12-Core Processor            ` | `IntelLLVM 20250101` | `add` | `L1` | 1009 | `auto` | `0xb236018413ef8ae1` | 64 | CPU fallback | pass |
| SYCL | `AMD Ryzen 9 3900X 12-Core Processor            ` | `IntelLLVM 20250101` | `add` | `L1` | 1009 | `64` | `0xb236018413ef8ae1` | 35 | CPU fallback | pass |
| SYCL | `AMD Ryzen 9 3900X 12-Core Processor            ` | `IntelLLVM 20250101` | `add` | `L1` | 1009 | `256` | `0xb236018413ef8ae1` | 50 | CPU fallback | pass |
| SYCL | `AMD Ryzen 9 3900X 12-Core Processor            ` | `IntelLLVM 20250101` | `add` | `L1` | 65536 | `auto` | `0xac20c9c8ab640c42` | 173 | CPU fallback | pass |
| SYCL | `AMD Ryzen 9 3900X 12-Core Processor            ` | `IntelLLVM 20250101` | `add` | `L1` | 65536 | `64` | `0xac20c9c8ab640c42` | 151 | CPU fallback | pass |
| SYCL | `AMD Ryzen 9 3900X 12-Core Processor            ` | `IntelLLVM 20250101` | `add` | `L1` | 65536 | `256` | `0xac20c9c8ab640c42` | 131 | CPU fallback | pass |
| SYCL | `AMD Ryzen 9 3900X 12-Core Processor            ` | `IntelLLVM 20250101` | `sub` | `L1` | 0 | `auto` | `0x14650fb0739d0383` | 36 | CPU fallback | pass |
| SYCL | `AMD Ryzen 9 3900X 12-Core Processor            ` | `IntelLLVM 20250101` | `sub` | `L1` | 0 | `64` | `0x14650fb0739d0383` | 45 | CPU fallback | pass |
| SYCL | `AMD Ryzen 9 3900X 12-Core Processor            ` | `IntelLLVM 20250101` | `sub` | `L1` | 0 | `256` | `0x14650fb0739d0383` | 46 | CPU fallback | pass |
| SYCL | `AMD Ryzen 9 3900X 12-Core Processor            ` | `IntelLLVM 20250101` | `sub` | `L1` | 1 | `auto` | `0xeb0db8638ec07380` | 45 | CPU fallback | pass |
| SYCL | `AMD Ryzen 9 3900X 12-Core Processor            ` | `IntelLLVM 20250101` | `sub` | `L1` | 1 | `64` | `0xeb0db8638ec07380` | 45 | CPU fallback | pass |
| SYCL | `AMD Ryzen 9 3900X 12-Core Processor            ` | `IntelLLVM 20250101` | `sub` | `L1` | 1 | `256` | `0xeb0db8638ec07380` | 24 | CPU fallback | pass |
| SYCL | `AMD Ryzen 9 3900X 12-Core Processor            ` | `IntelLLVM 20250101` | `sub` | `L1` | 127 | `auto` | `0xaecfa220f3009f83` | 62 | CPU fallback | pass |
| SYCL | `AMD Ryzen 9 3900X 12-Core Processor            ` | `IntelLLVM 20250101` | `sub` | `L1` | 127 | `64` | `0xaecfa220f3009f83` | 52 | CPU fallback | pass |
| SYCL | `AMD Ryzen 9 3900X 12-Core Processor            ` | `IntelLLVM 20250101` | `sub` | `L1` | 127 | `256` | `0xaecfa220f3009f83` | 40 | CPU fallback | pass |
| SYCL | `AMD Ryzen 9 3900X 12-Core Processor            ` | `IntelLLVM 20250101` | `sub` | `L1` | 256 | `auto` | `0xe4dc349a2fd81a21` | 52 | CPU fallback | pass |
| SYCL | `AMD Ryzen 9 3900X 12-Core Processor            ` | `IntelLLVM 20250101` | `sub` | `L1` | 256 | `64` | `0xe4dc349a2fd81a21` | 39 | CPU fallback | pass |
| SYCL | `AMD Ryzen 9 3900X 12-Core Processor            ` | `IntelLLVM 20250101` | `sub` | `L1` | 256 | `256` | `0xe4dc349a2fd81a21` | 46 | CPU fallback | pass |
| SYCL | `AMD Ryzen 9 3900X 12-Core Processor            ` | `IntelLLVM 20250101` | `sub` | `L1` | 1009 | `auto` | `0x9d996ef1b6469f23` | 70 | CPU fallback | pass |
| SYCL | `AMD Ryzen 9 3900X 12-Core Processor            ` | `IntelLLVM 20250101` | `sub` | `L1` | 1009 | `64` | `0x9d996ef1b6469f23` | 47 | CPU fallback | pass |
| SYCL | `AMD Ryzen 9 3900X 12-Core Processor            ` | `IntelLLVM 20250101` | `sub` | `L1` | 1009 | `256` | `0x9d996ef1b6469f23` | 45 | CPU fallback | pass |
| SYCL | `AMD Ryzen 9 3900X 12-Core Processor            ` | `IntelLLVM 20250101` | `sub` | `L1` | 65536 | `auto` | `0xfb5da996c6c53d82` | 185 | CPU fallback | pass |
| SYCL | `AMD Ryzen 9 3900X 12-Core Processor            ` | `IntelLLVM 20250101` | `sub` | `L1` | 65536 | `64` | `0xfb5da996c6c53d82` | 248 | CPU fallback | pass |
| SYCL | `AMD Ryzen 9 3900X 12-Core Processor            ` | `IntelLLVM 20250101` | `sub` | `L1` | 65536 | `256` | `0xfb5da996c6c53d82` | 141 | CPU fallback | pass |
| SYCL | `AMD Ryzen 9 3900X 12-Core Processor            ` | `IntelLLVM 20250101` | `compare` | `L1` | 0 | `auto` | `0x14650fb0739d0383` | 32 | CPU fallback | pass |
| SYCL | `AMD Ryzen 9 3900X 12-Core Processor            ` | `IntelLLVM 20250101` | `compare` | `L1` | 0 | `64` | `0x14650fb0739d0383` | 21 | CPU fallback | pass |
| SYCL | `AMD Ryzen 9 3900X 12-Core Processor            ` | `IntelLLVM 20250101` | `compare` | `L1` | 0 | `256` | `0x14650fb0739d0383` | 24 | CPU fallback | pass |
| SYCL | `AMD Ryzen 9 3900X 12-Core Processor            ` | `IntelLLVM 20250101` | `compare` | `L1` | 1 | `auto` | `0x44bd2bd473ccf799` | 38 | CPU fallback | pass |
| SYCL | `AMD Ryzen 9 3900X 12-Core Processor            ` | `IntelLLVM 20250101` | `compare` | `L1` | 1 | `64` | `0x44bd2bd473ccf799` | 26 | CPU fallback | pass |
| SYCL | `AMD Ryzen 9 3900X 12-Core Processor            ` | `IntelLLVM 20250101` | `compare` | `L1` | 1 | `256` | `0x44bd2bd473ccf799` | 59 | CPU fallback | pass |
| SYCL | `AMD Ryzen 9 3900X 12-Core Processor            ` | `IntelLLVM 20250101` | `compare` | `L1` | 127 | `auto` | `0xa897d53224189902` | 56 | CPU fallback | pass |
| SYCL | `AMD Ryzen 9 3900X 12-Core Processor            ` | `IntelLLVM 20250101` | `compare` | `L1` | 127 | `64` | `0xa897d53224189902` | 31 | CPU fallback | pass |
| SYCL | `AMD Ryzen 9 3900X 12-Core Processor            ` | `IntelLLVM 20250101` | `compare` | `L1` | 127 | `256` | `0xa897d53224189902` | 35 | CPU fallback | pass |
| SYCL | `AMD Ryzen 9 3900X 12-Core Processor            ` | `IntelLLVM 20250101` | `compare` | `L1` | 256 | `auto` | `0x502d278fb3a13aa8` | 56 | CPU fallback | pass |
| SYCL | `AMD Ryzen 9 3900X 12-Core Processor            ` | `IntelLLVM 20250101` | `compare` | `L1` | 256 | `64` | `0x502d278fb3a13aa8` | 38 | CPU fallback | pass |
| SYCL | `AMD Ryzen 9 3900X 12-Core Processor            ` | `IntelLLVM 20250101` | `compare` | `L1` | 256 | `256` | `0x502d278fb3a13aa8` | 56 | CPU fallback | pass |
| SYCL | `AMD Ryzen 9 3900X 12-Core Processor            ` | `IntelLLVM 20250101` | `compare` | `L1` | 1009 | `auto` | `0xa018d1ace885029c` | 67 | CPU fallback | pass |
| SYCL | `AMD Ryzen 9 3900X 12-Core Processor            ` | `IntelLLVM 20250101` | `compare` | `L1` | 1009 | `64` | `0xa018d1ace885029c` | 40 | CPU fallback | pass |
| SYCL | `AMD Ryzen 9 3900X 12-Core Processor            ` | `IntelLLVM 20250101` | `compare` | `L1` | 1009 | `256` | `0xa018d1ace885029c` | 39 | CPU fallback | pass |
| SYCL | `AMD Ryzen 9 3900X 12-Core Processor            ` | `IntelLLVM 20250101` | `compare` | `L1` | 65536 | `auto` | `0x88bd943cd0eafa01` | 163 | CPU fallback | pass |
| SYCL | `AMD Ryzen 9 3900X 12-Core Processor            ` | `IntelLLVM 20250101` | `compare` | `L1` | 65536 | `64` | `0x88bd943cd0eafa01` | 143 | CPU fallback | pass |
| SYCL | `AMD Ryzen 9 3900X 12-Core Processor            ` | `IntelLLVM 20250101` | `compare` | `L1` | 65536 | `256` | `0x88bd943cd0eafa01` | 151 | CPU fallback | pass |
| SYCL | `AMD Ryzen 9 3900X 12-Core Processor            ` | `IntelLLVM 20250101` | `min` | `L1` | 0 | `auto` | `0x14650fb0739d0383` | 40 | CPU fallback | pass |
| SYCL | `AMD Ryzen 9 3900X 12-Core Processor            ` | `IntelLLVM 20250101` | `min` | `L1` | 0 | `64` | `0x14650fb0739d0383` | 44 | CPU fallback | pass |
| SYCL | `AMD Ryzen 9 3900X 12-Core Processor            ` | `IntelLLVM 20250101` | `min` | `L1` | 0 | `256` | `0x14650fb0739d0383` | 47 | CPU fallback | pass |
| SYCL | `AMD Ryzen 9 3900X 12-Core Processor            ` | `IntelLLVM 20250101` | `min` | `L1` | 1 | `auto` | `0x47fe0d7eaf8e51e3` | 58 | CPU fallback | pass |
| SYCL | `AMD Ryzen 9 3900X 12-Core Processor            ` | `IntelLLVM 20250101` | `min` | `L1` | 1 | `64` | `0x47fe0d7eaf8e51e3` | 42 | CPU fallback | pass |
| SYCL | `AMD Ryzen 9 3900X 12-Core Processor            ` | `IntelLLVM 20250101` | `min` | `L1` | 1 | `256` | `0x47fe0d7eaf8e51e3` | 26 | CPU fallback | pass |
| SYCL | `AMD Ryzen 9 3900X 12-Core Processor            ` | `IntelLLVM 20250101` | `min` | `L1` | 127 | `auto` | `0x8f1814d799e65a42` | 47 | CPU fallback | pass |
| SYCL | `AMD Ryzen 9 3900X 12-Core Processor            ` | `IntelLLVM 20250101` | `min` | `L1` | 127 | `64` | `0x8f1814d799e65a42` | 27 | CPU fallback | pass |
| SYCL | `AMD Ryzen 9 3900X 12-Core Processor            ` | `IntelLLVM 20250101` | `min` | `L1` | 127 | `256` | `0x8f1814d799e65a42` | 33 | CPU fallback | pass |
| SYCL | `AMD Ryzen 9 3900X 12-Core Processor            ` | `IntelLLVM 20250101` | `min` | `L1` | 256 | `auto` | `0x0740e81631846080` | 42 | CPU fallback | pass |
| SYCL | `AMD Ryzen 9 3900X 12-Core Processor            ` | `IntelLLVM 20250101` | `min` | `L1` | 256 | `64` | `0x0740e81631846080` | 33 | CPU fallback | pass |
| SYCL | `AMD Ryzen 9 3900X 12-Core Processor            ` | `IntelLLVM 20250101` | `min` | `L1` | 256 | `256` | `0x0740e81631846080` | 26 | CPU fallback | pass |
| SYCL | `AMD Ryzen 9 3900X 12-Core Processor            ` | `IntelLLVM 20250101` | `min` | `L1` | 1009 | `auto` | `0x05df1bf847c39303` | 65 | CPU fallback | pass |
| SYCL | `AMD Ryzen 9 3900X 12-Core Processor            ` | `IntelLLVM 20250101` | `min` | `L1` | 1009 | `64` | `0x05df1bf847c39303` | 37 | CPU fallback | pass |
| SYCL | `AMD Ryzen 9 3900X 12-Core Processor            ` | `IntelLLVM 20250101` | `min` | `L1` | 1009 | `256` | `0x05df1bf847c39303` | 42 | CPU fallback | pass |
| SYCL | `AMD Ryzen 9 3900X 12-Core Processor            ` | `IntelLLVM 20250101` | `min` | `L1` | 65536 | `auto` | `0x4e198e66162ee9e1` | 2422 | CPU fallback | pass |
| SYCL | `AMD Ryzen 9 3900X 12-Core Processor            ` | `IntelLLVM 20250101` | `min` | `L1` | 65536 | `64` | `0x4e198e66162ee9e1` | 169 | CPU fallback | pass |
| SYCL | `AMD Ryzen 9 3900X 12-Core Processor            ` | `IntelLLVM 20250101` | `min` | `L1` | 65536 | `256` | `0x4e198e66162ee9e1` | 142 | CPU fallback | pass |
| SYCL | `AMD Ryzen 9 3900X 12-Core Processor            ` | `IntelLLVM 20250101` | `max` | `L1` | 0 | `auto` | `0x14650fb0739d0383` | 48 | CPU fallback | pass |
| SYCL | `AMD Ryzen 9 3900X 12-Core Processor            ` | `IntelLLVM 20250101` | `max` | `L1` | 0 | `64` | `0x14650fb0739d0383` | 65 | CPU fallback | pass |
| SYCL | `AMD Ryzen 9 3900X 12-Core Processor            ` | `IntelLLVM 20250101` | `max` | `L1` | 0 | `256` | `0x14650fb0739d0383` | 35 | CPU fallback | pass |
| SYCL | `AMD Ryzen 9 3900X 12-Core Processor            ` | `IntelLLVM 20250101` | `max` | `L1` | 1 | `auto` | `0x0a087f6c99afbda1` | 59 | CPU fallback | pass |
| SYCL | `AMD Ryzen 9 3900X 12-Core Processor            ` | `IntelLLVM 20250101` | `max` | `L1` | 1 | `64` | `0x0a087f6c99afbda1` | 40 | CPU fallback | pass |
| SYCL | `AMD Ryzen 9 3900X 12-Core Processor            ` | `IntelLLVM 20250101` | `max` | `L1` | 1 | `256` | `0x0a087f6c99afbda1` | 40 | CPU fallback | pass |
| SYCL | `AMD Ryzen 9 3900X 12-Core Processor            ` | `IntelLLVM 20250101` | `max` | `L1` | 127 | `auto` | `0x2dee89751c520121` | 58 | CPU fallback | pass |
| SYCL | `AMD Ryzen 9 3900X 12-Core Processor            ` | `IntelLLVM 20250101` | `max` | `L1` | 127 | `64` | `0x2dee89751c520121` | 36 | CPU fallback | pass |
| SYCL | `AMD Ryzen 9 3900X 12-Core Processor            ` | `IntelLLVM 20250101` | `max` | `L1` | 127 | `256` | `0x2dee89751c520121` | 30 | CPU fallback | pass |
| SYCL | `AMD Ryzen 9 3900X 12-Core Processor            ` | `IntelLLVM 20250101` | `max` | `L1` | 256 | `auto` | `0x7bca155fb50abd62` | 71 | CPU fallback | pass |
| SYCL | `AMD Ryzen 9 3900X 12-Core Processor            ` | `IntelLLVM 20250101` | `max` | `L1` | 256 | `64` | `0x7bca155fb50abd62` | 63 | CPU fallback | pass |
| SYCL | `AMD Ryzen 9 3900X 12-Core Processor            ` | `IntelLLVM 20250101` | `max` | `L1` | 256 | `256` | `0x7bca155fb50abd62` | 39 | CPU fallback | pass |
| SYCL | `AMD Ryzen 9 3900X 12-Core Processor            ` | `IntelLLVM 20250101` | `max` | `L1` | 1009 | `auto` | `0x1c75f7531629d9e2` | 81 | CPU fallback | pass |
| SYCL | `AMD Ryzen 9 3900X 12-Core Processor            ` | `IntelLLVM 20250101` | `max` | `L1` | 1009 | `64` | `0x1c75f7531629d9e2` | 54 | CPU fallback | pass |
| SYCL | `AMD Ryzen 9 3900X 12-Core Processor            ` | `IntelLLVM 20250101` | `max` | `L1` | 1009 | `256` | `0x1c75f7531629d9e2` | 47 | CPU fallback | pass |
| SYCL | `AMD Ryzen 9 3900X 12-Core Processor            ` | `IntelLLVM 20250101` | `max` | `L1` | 65536 | `auto` | `0xc64a49641634f2e2` | 224 | CPU fallback | pass |
| SYCL | `AMD Ryzen 9 3900X 12-Core Processor            ` | `IntelLLVM 20250101` | `max` | `L1` | 65536 | `64` | `0xc64a49641634f2e2` | 156 | CPU fallback | pass |
| SYCL | `AMD Ryzen 9 3900X 12-Core Processor            ` | `IntelLLVM 20250101` | `max` | `L1` | 65536 | `256` | `0xc64a49641634f2e2` | 172 | CPU fallback | pass |
| SYCL | `AMD Ryzen 9 3900X 12-Core Processor            ` | `IntelLLVM 20250101` | `neg` | `L5` | 0 | `auto` | `0x14650fb0739d0383` | 63 | CPU fallback | pass |
| SYCL | `AMD Ryzen 9 3900X 12-Core Processor            ` | `IntelLLVM 20250101` | `neg` | `L5` | 0 | `64` | `0x14650fb0739d0383` | 34 | CPU fallback | pass |
| SYCL | `AMD Ryzen 9 3900X 12-Core Processor            ` | `IntelLLVM 20250101` | `neg` | `L5` | 0 | `256` | `0x14650fb0739d0383` | 64 | CPU fallback | pass |
| SYCL | `AMD Ryzen 9 3900X 12-Core Processor            ` | `IntelLLVM 20250101` | `neg` | `L5` | 1 | `auto` | `0xf1ac74632d57a97b` | 53 | CPU fallback | pass |
| SYCL | `AMD Ryzen 9 3900X 12-Core Processor            ` | `IntelLLVM 20250101` | `neg` | `L5` | 1 | `64` | `0xf1ac74632d57a97b` | 38 | CPU fallback | pass |
| SYCL | `AMD Ryzen 9 3900X 12-Core Processor            ` | `IntelLLVM 20250101` | `neg` | `L5` | 1 | `256` | `0xf1ac74632d57a97b` | 56 | CPU fallback | pass |
| SYCL | `AMD Ryzen 9 3900X 12-Core Processor            ` | `IntelLLVM 20250101` | `neg` | `L5` | 127 | `auto` | `0x5ba965e9d3fe091c` | 62 | CPU fallback | pass |
| SYCL | `AMD Ryzen 9 3900X 12-Core Processor            ` | `IntelLLVM 20250101` | `neg` | `L5` | 127 | `64` | `0x5ba965e9d3fe091c` | 490 | CPU fallback | pass |
| SYCL | `AMD Ryzen 9 3900X 12-Core Processor            ` | `IntelLLVM 20250101` | `neg` | `L5` | 127 | `256` | `0x5ba965e9d3fe091c` | 62 | CPU fallback | pass |
| SYCL | `AMD Ryzen 9 3900X 12-Core Processor            ` | `IntelLLVM 20250101` | `neg` | `L5` | 256 | `auto` | `0x3e5631504635ae1b` | 39 | CPU fallback | pass |
| SYCL | `AMD Ryzen 9 3900X 12-Core Processor            ` | `IntelLLVM 20250101` | `neg` | `L5` | 256 | `64` | `0x3e5631504635ae1b` | 31 | CPU fallback | pass |
| SYCL | `AMD Ryzen 9 3900X 12-Core Processor            ` | `IntelLLVM 20250101` | `neg` | `L5` | 256 | `256` | `0x3e5631504635ae1b` | 88 | CPU fallback | pass |
| SYCL | `AMD Ryzen 9 3900X 12-Core Processor            ` | `IntelLLVM 20250101` | `neg` | `L5` | 1009 | `auto` | `0x966ba8f15fe0e062` | 73 | CPU fallback | pass |
| SYCL | `AMD Ryzen 9 3900X 12-Core Processor            ` | `IntelLLVM 20250101` | `neg` | `L5` | 1009 | `64` | `0x966ba8f15fe0e062` | 45 | CPU fallback | pass |
| SYCL | `AMD Ryzen 9 3900X 12-Core Processor            ` | `IntelLLVM 20250101` | `neg` | `L5` | 1009 | `256` | `0x966ba8f15fe0e062` | 28 | CPU fallback | pass |
| SYCL | `AMD Ryzen 9 3900X 12-Core Processor            ` | `IntelLLVM 20250101` | `neg` | `L5` | 65536 | `auto` | `0x52d360cfa66da818` | 142 | CPU fallback | pass |
| SYCL | `AMD Ryzen 9 3900X 12-Core Processor            ` | `IntelLLVM 20250101` | `neg` | `L5` | 65536 | `64` | `0x52d360cfa66da818` | 193 | CPU fallback | pass |
| SYCL | `AMD Ryzen 9 3900X 12-Core Processor            ` | `IntelLLVM 20250101` | `neg` | `L5` | 65536 | `256` | `0x52d360cfa66da818` | 102 | CPU fallback | pass |
| SYCL | `AMD Ryzen 9 3900X 12-Core Processor            ` | `IntelLLVM 20250101` | `add` | `L5` | 0 | `auto` | `0x14650fb0739d0383` | 36 | CPU fallback | pass |
| SYCL | `AMD Ryzen 9 3900X 12-Core Processor            ` | `IntelLLVM 20250101` | `add` | `L5` | 0 | `64` | `0x14650fb0739d0383` | 34 | CPU fallback | pass |
| SYCL | `AMD Ryzen 9 3900X 12-Core Processor            ` | `IntelLLVM 20250101` | `add` | `L5` | 0 | `256` | `0x14650fb0739d0383` | 27 | CPU fallback | pass |
| SYCL | `AMD Ryzen 9 3900X 12-Core Processor            ` | `IntelLLVM 20250101` | `add` | `L5` | 1 | `auto` | `0x8c606564102ea6fd` | 54 | CPU fallback | pass |
| SYCL | `AMD Ryzen 9 3900X 12-Core Processor            ` | `IntelLLVM 20250101` | `add` | `L5` | 1 | `64` | `0x8c606564102ea6fd` | 41 | CPU fallback | pass |
| SYCL | `AMD Ryzen 9 3900X 12-Core Processor            ` | `IntelLLVM 20250101` | `add` | `L5` | 1 | `256` | `0x8c606564102ea6fd` | 50 | CPU fallback | pass |
| SYCL | `AMD Ryzen 9 3900X 12-Core Processor            ` | `IntelLLVM 20250101` | `add` | `L5` | 127 | `auto` | `0x278bc828c703c355` | 57 | CPU fallback | pass |
| SYCL | `AMD Ryzen 9 3900X 12-Core Processor            ` | `IntelLLVM 20250101` | `add` | `L5` | 127 | `64` | `0x278bc828c703c355` | 53 | CPU fallback | pass |
| SYCL | `AMD Ryzen 9 3900X 12-Core Processor            ` | `IntelLLVM 20250101` | `add` | `L5` | 127 | `256` | `0x278bc828c703c355` | 62 | CPU fallback | pass |
| SYCL | `AMD Ryzen 9 3900X 12-Core Processor            ` | `IntelLLVM 20250101` | `add` | `L5` | 256 | `auto` | `0xba32362089c95034` | 52 | CPU fallback | pass |
| SYCL | `AMD Ryzen 9 3900X 12-Core Processor            ` | `IntelLLVM 20250101` | `add` | `L5` | 256 | `64` | `0xba32362089c95034` | 34 | CPU fallback | pass |
| SYCL | `AMD Ryzen 9 3900X 12-Core Processor            ` | `IntelLLVM 20250101` | `add` | `L5` | 256 | `256` | `0xba32362089c95034` | 39 | CPU fallback | pass |
| SYCL | `AMD Ryzen 9 3900X 12-Core Processor            ` | `IntelLLVM 20250101` | `add` | `L5` | 1009 | `auto` | `0x513ccef1f74fb93a` | 63 | CPU fallback | pass |
| SYCL | `AMD Ryzen 9 3900X 12-Core Processor            ` | `IntelLLVM 20250101` | `add` | `L5` | 1009 | `64` | `0x513ccef1f74fb93a` | 44 | CPU fallback | pass |
| SYCL | `AMD Ryzen 9 3900X 12-Core Processor            ` | `IntelLLVM 20250101` | `add` | `L5` | 1009 | `256` | `0x513ccef1f74fb93a` | 40 | CPU fallback | pass |
| SYCL | `AMD Ryzen 9 3900X 12-Core Processor            ` | `IntelLLVM 20250101` | `add` | `L5` | 65536 | `auto` | `0x3e630ad7b5736248` | 1244 | CPU fallback | pass |
| SYCL | `AMD Ryzen 9 3900X 12-Core Processor            ` | `IntelLLVM 20250101` | `add` | `L5` | 65536 | `64` | `0x3e630ad7b5736248` | 630 | CPU fallback | pass |
| SYCL | `AMD Ryzen 9 3900X 12-Core Processor            ` | `IntelLLVM 20250101` | `add` | `L5` | 65536 | `256` | `0x3e630ad7b5736248` | 340 | CPU fallback | pass |
| SYCL | `AMD Ryzen 9 3900X 12-Core Processor            ` | `IntelLLVM 20250101` | `sub` | `L5` | 0 | `auto` | `0x14650fb0739d0383` | 38 | CPU fallback | pass |
| SYCL | `AMD Ryzen 9 3900X 12-Core Processor            ` | `IntelLLVM 20250101` | `sub` | `L5` | 0 | `64` | `0x14650fb0739d0383` | 32 | CPU fallback | pass |
| SYCL | `AMD Ryzen 9 3900X 12-Core Processor            ` | `IntelLLVM 20250101` | `sub` | `L5` | 0 | `256` | `0x14650fb0739d0383` | 64 | CPU fallback | pass |
| SYCL | `AMD Ryzen 9 3900X 12-Core Processor            ` | `IntelLLVM 20250101` | `sub` | `L5` | 1 | `auto` | `0xbf1a0ede587ec38d` | 81 | CPU fallback | pass |
| SYCL | `AMD Ryzen 9 3900X 12-Core Processor            ` | `IntelLLVM 20250101` | `sub` | `L5` | 1 | `64` | `0xbf1a0ede587ec38d` | 38 | CPU fallback | pass |
| SYCL | `AMD Ryzen 9 3900X 12-Core Processor            ` | `IntelLLVM 20250101` | `sub` | `L5` | 1 | `256` | `0xbf1a0ede587ec38d` | 31 | CPU fallback | pass |
| SYCL | `AMD Ryzen 9 3900X 12-Core Processor            ` | `IntelLLVM 20250101` | `sub` | `L5` | 127 | `auto` | `0x145a7449ec4ba4b6` | 56 | CPU fallback | pass |
| SYCL | `AMD Ryzen 9 3900X 12-Core Processor            ` | `IntelLLVM 20250101` | `sub` | `L5` | 127 | `64` | `0x145a7449ec4ba4b6` | 42 | CPU fallback | pass |
| SYCL | `AMD Ryzen 9 3900X 12-Core Processor            ` | `IntelLLVM 20250101` | `sub` | `L5` | 127 | `256` | `0x145a7449ec4ba4b6` | 36 | CPU fallback | pass |
| SYCL | `AMD Ryzen 9 3900X 12-Core Processor            ` | `IntelLLVM 20250101` | `sub` | `L5` | 256 | `auto` | `0xf538d7b954ba12bb` | 47 | CPU fallback | pass |
| SYCL | `AMD Ryzen 9 3900X 12-Core Processor            ` | `IntelLLVM 20250101` | `sub` | `L5` | 256 | `64` | `0xf538d7b954ba12bb` | 51 | CPU fallback | pass |
| SYCL | `AMD Ryzen 9 3900X 12-Core Processor            ` | `IntelLLVM 20250101` | `sub` | `L5` | 256 | `256` | `0xf538d7b954ba12bb` | 54 | CPU fallback | pass |
| SYCL | `AMD Ryzen 9 3900X 12-Core Processor            ` | `IntelLLVM 20250101` | `sub` | `L5` | 1009 | `auto` | `0xd7f80e2d2f817210` | 65 | CPU fallback | pass |
| SYCL | `AMD Ryzen 9 3900X 12-Core Processor            ` | `IntelLLVM 20250101` | `sub` | `L5` | 1009 | `64` | `0xd7f80e2d2f817210` | 74 | CPU fallback | pass |
| SYCL | `AMD Ryzen 9 3900X 12-Core Processor            ` | `IntelLLVM 20250101` | `sub` | `L5` | 1009 | `256` | `0xd7f80e2d2f817210` | 59 | CPU fallback | pass |
| SYCL | `AMD Ryzen 9 3900X 12-Core Processor            ` | `IntelLLVM 20250101` | `sub` | `L5` | 65536 | `auto` | `0x2ddabea16a77e54a` | 1739 | CPU fallback | pass |
| SYCL | `AMD Ryzen 9 3900X 12-Core Processor            ` | `IntelLLVM 20250101` | `sub` | `L5` | 65536 | `64` | `0x2ddabea16a77e54a` | 1250 | CPU fallback | pass |
| SYCL | `AMD Ryzen 9 3900X 12-Core Processor            ` | `IntelLLVM 20250101` | `sub` | `L5` | 65536 | `256` | `0x2ddabea16a77e54a` | 889 | CPU fallback | pass |
| SYCL | `AMD Ryzen 9 3900X 12-Core Processor            ` | `IntelLLVM 20250101` | `compare` | `L5` | 0 | `auto` | `0x14650fb0739d0383` | 75 | CPU fallback | pass |
| SYCL | `AMD Ryzen 9 3900X 12-Core Processor            ` | `IntelLLVM 20250101` | `compare` | `L5` | 0 | `64` | `0x14650fb0739d0383` | 45 | CPU fallback | pass |
| SYCL | `AMD Ryzen 9 3900X 12-Core Processor            ` | `IntelLLVM 20250101` | `compare` | `L5` | 0 | `256` | `0x14650fb0739d0383` | 32 | CPU fallback | pass |
| SYCL | `AMD Ryzen 9 3900X 12-Core Processor            ` | `IntelLLVM 20250101` | `compare` | `L5` | 1 | `auto` | `0x44bd2bd473ccf799` | 334 | CPU fallback | pass |
| SYCL | `AMD Ryzen 9 3900X 12-Core Processor            ` | `IntelLLVM 20250101` | `compare` | `L5` | 1 | `64` | `0x44bd2bd473ccf799` | 45 | CPU fallback | pass |
| SYCL | `AMD Ryzen 9 3900X 12-Core Processor            ` | `IntelLLVM 20250101` | `compare` | `L5` | 1 | `256` | `0x44bd2bd473ccf799` | 39 | CPU fallback | pass |
| SYCL | `AMD Ryzen 9 3900X 12-Core Processor            ` | `IntelLLVM 20250101` | `compare` | `L5` | 127 | `auto` | `0x14e58de27287f88b` | 279 | CPU fallback | pass |
| SYCL | `AMD Ryzen 9 3900X 12-Core Processor            ` | `IntelLLVM 20250101` | `compare` | `L5` | 127 | `64` | `0x14e58de27287f88b` | 39 | CPU fallback | pass |
| SYCL | `AMD Ryzen 9 3900X 12-Core Processor            ` | `IntelLLVM 20250101` | `compare` | `L5` | 127 | `256` | `0x14e58de27287f88b` | 48 | CPU fallback | pass |
| SYCL | `AMD Ryzen 9 3900X 12-Core Processor            ` | `IntelLLVM 20250101` | `compare` | `L5` | 256 | `auto` | `0xdc3ad6e17fce5eed` | 77 | CPU fallback | pass |
| SYCL | `AMD Ryzen 9 3900X 12-Core Processor            ` | `IntelLLVM 20250101` | `compare` | `L5` | 256 | `64` | `0xdc3ad6e17fce5eed` | 42 | CPU fallback | pass |
| SYCL | `AMD Ryzen 9 3900X 12-Core Processor            ` | `IntelLLVM 20250101` | `compare` | `L5` | 256 | `256` | `0xdc3ad6e17fce5eed` | 71 | CPU fallback | pass |
| SYCL | `AMD Ryzen 9 3900X 12-Core Processor            ` | `IntelLLVM 20250101` | `compare` | `L5` | 1009 | `auto` | `0xa8e897644d1faeb1` | 89 | CPU fallback | pass |
| SYCL | `AMD Ryzen 9 3900X 12-Core Processor            ` | `IntelLLVM 20250101` | `compare` | `L5` | 1009 | `64` | `0xa8e897644d1faeb1` | 86 | CPU fallback | pass |
| SYCL | `AMD Ryzen 9 3900X 12-Core Processor            ` | `IntelLLVM 20250101` | `compare` | `L5` | 1009 | `256` | `0xa8e897644d1faeb1` | 55 | CPU fallback | pass |
| SYCL | `AMD Ryzen 9 3900X 12-Core Processor            ` | `IntelLLVM 20250101` | `compare` | `L5` | 65536 | `auto` | `0x06db7657b04196fb` | 520 | CPU fallback | pass |
| SYCL | `AMD Ryzen 9 3900X 12-Core Processor            ` | `IntelLLVM 20250101` | `compare` | `L5` | 65536 | `64` | `0x06db7657b04196fb` | 174 | CPU fallback | pass |
| SYCL | `AMD Ryzen 9 3900X 12-Core Processor            ` | `IntelLLVM 20250101` | `compare` | `L5` | 65536 | `256` | `0x06db7657b04196fb` | 154 | CPU fallback | pass |
| SYCL | `AMD Ryzen 9 3900X 12-Core Processor            ` | `IntelLLVM 20250101` | `min` | `L5` | 0 | `auto` | `0x14650fb0739d0383` | 57 | CPU fallback | pass |
| SYCL | `AMD Ryzen 9 3900X 12-Core Processor            ` | `IntelLLVM 20250101` | `min` | `L5` | 0 | `64` | `0x14650fb0739d0383` | 30 | CPU fallback | pass |
| SYCL | `AMD Ryzen 9 3900X 12-Core Processor            ` | `IntelLLVM 20250101` | `min` | `L5` | 0 | `256` | `0x14650fb0739d0383` | 36 | CPU fallback | pass |
| SYCL | `AMD Ryzen 9 3900X 12-Core Processor            ` | `IntelLLVM 20250101` | `min` | `L5` | 1 | `auto` | `0x2f80b657b5fd44fb` | 51 | CPU fallback | pass |
| SYCL | `AMD Ryzen 9 3900X 12-Core Processor            ` | `IntelLLVM 20250101` | `min` | `L5` | 1 | `64` | `0x2f80b657b5fd44fb` | 29 | CPU fallback | pass |
| SYCL | `AMD Ryzen 9 3900X 12-Core Processor            ` | `IntelLLVM 20250101` | `min` | `L5` | 1 | `256` | `0x2f80b657b5fd44fb` | 43 | CPU fallback | pass |
| SYCL | `AMD Ryzen 9 3900X 12-Core Processor            ` | `IntelLLVM 20250101` | `min` | `L5` | 127 | `auto` | `0xb385070407909141` | 49 | CPU fallback | pass |
| SYCL | `AMD Ryzen 9 3900X 12-Core Processor            ` | `IntelLLVM 20250101` | `min` | `L5` | 127 | `64` | `0xb385070407909141` | 32 | CPU fallback | pass |
| SYCL | `AMD Ryzen 9 3900X 12-Core Processor            ` | `IntelLLVM 20250101` | `min` | `L5` | 127 | `256` | `0xb385070407909141` | 39 | CPU fallback | pass |
| SYCL | `AMD Ryzen 9 3900X 12-Core Processor            ` | `IntelLLVM 20250101` | `min` | `L5` | 256 | `auto` | `0x9ac55e172dc1d665` | 42 | CPU fallback | pass |
| SYCL | `AMD Ryzen 9 3900X 12-Core Processor            ` | `IntelLLVM 20250101` | `min` | `L5` | 256 | `64` | `0x9ac55e172dc1d665` | 37 | CPU fallback | pass |
| SYCL | `AMD Ryzen 9 3900X 12-Core Processor            ` | `IntelLLVM 20250101` | `min` | `L5` | 256 | `256` | `0x9ac55e172dc1d665` | 39 | CPU fallback | pass |
| SYCL | `AMD Ryzen 9 3900X 12-Core Processor            ` | `IntelLLVM 20250101` | `min` | `L5` | 1009 | `auto` | `0x92b3f7c6b7535be6` | 68 | CPU fallback | pass |
| SYCL | `AMD Ryzen 9 3900X 12-Core Processor            ` | `IntelLLVM 20250101` | `min` | `L5` | 1009 | `64` | `0x92b3f7c6b7535be6` | 44 | CPU fallback | pass |
| SYCL | `AMD Ryzen 9 3900X 12-Core Processor            ` | `IntelLLVM 20250101` | `min` | `L5` | 1009 | `256` | `0x92b3f7c6b7535be6` | 40 | CPU fallback | pass |
| SYCL | `AMD Ryzen 9 3900X 12-Core Processor            ` | `IntelLLVM 20250101` | `min` | `L5` | 65536 | `auto` | `0x5dca1c48b0be8f56` | 185 | CPU fallback | pass |
| SYCL | `AMD Ryzen 9 3900X 12-Core Processor            ` | `IntelLLVM 20250101` | `min` | `L5` | 65536 | `64` | `0x5dca1c48b0be8f56` | 204 | CPU fallback | pass |
| SYCL | `AMD Ryzen 9 3900X 12-Core Processor            ` | `IntelLLVM 20250101` | `min` | `L5` | 65536 | `256` | `0x5dca1c48b0be8f56` | 270 | CPU fallback | pass |
| SYCL | `AMD Ryzen 9 3900X 12-Core Processor            ` | `IntelLLVM 20250101` | `max` | `L5` | 0 | `auto` | `0x14650fb0739d0383` | 31 | CPU fallback | pass |
| SYCL | `AMD Ryzen 9 3900X 12-Core Processor            ` | `IntelLLVM 20250101` | `max` | `L5` | 0 | `64` | `0x14650fb0739d0383` | 27 | CPU fallback | pass |
| SYCL | `AMD Ryzen 9 3900X 12-Core Processor            ` | `IntelLLVM 20250101` | `max` | `L5` | 0 | `256` | `0x14650fb0739d0383` | 34 | CPU fallback | pass |
| SYCL | `AMD Ryzen 9 3900X 12-Core Processor            ` | `IntelLLVM 20250101` | `max` | `L5` | 1 | `auto` | `0xe982acab84d1fa83` | 39 | CPU fallback | pass |
| SYCL | `AMD Ryzen 9 3900X 12-Core Processor            ` | `IntelLLVM 20250101` | `max` | `L5` | 1 | `64` | `0xe982acab84d1fa83` | 38 | CPU fallback | pass |
| SYCL | `AMD Ryzen 9 3900X 12-Core Processor            ` | `IntelLLVM 20250101` | `max` | `L5` | 1 | `256` | `0xe982acab84d1fa83` | 35 | CPU fallback | pass |
| SYCL | `AMD Ryzen 9 3900X 12-Core Processor            ` | `IntelLLVM 20250101` | `max` | `L5` | 127 | `auto` | `0x67a964ca0eb56120` | 64 | CPU fallback | pass |
| SYCL | `AMD Ryzen 9 3900X 12-Core Processor            ` | `IntelLLVM 20250101` | `max` | `L5` | 127 | `64` | `0x67a964ca0eb56120` | 56 | CPU fallback | pass |
| SYCL | `AMD Ryzen 9 3900X 12-Core Processor            ` | `IntelLLVM 20250101` | `max` | `L5` | 127 | `256` | `0x67a964ca0eb56120` | 53 | CPU fallback | pass |
| SYCL | `AMD Ryzen 9 3900X 12-Core Processor            ` | `IntelLLVM 20250101` | `max` | `L5` | 256 | `auto` | `0xf23d6ae57b6038db` | 61 | CPU fallback | pass |
| SYCL | `AMD Ryzen 9 3900X 12-Core Processor            ` | `IntelLLVM 20250101` | `max` | `L5` | 256 | `64` | `0xf23d6ae57b6038db` | 50 | CPU fallback | pass |
| SYCL | `AMD Ryzen 9 3900X 12-Core Processor            ` | `IntelLLVM 20250101` | `max` | `L5` | 256 | `256` | `0xf23d6ae57b6038db` | 47 | CPU fallback | pass |
| SYCL | `AMD Ryzen 9 3900X 12-Core Processor            ` | `IntelLLVM 20250101` | `max` | `L5` | 1009 | `auto` | `0xa6faa7b125d2744b` | 70 | CPU fallback | pass |
| SYCL | `AMD Ryzen 9 3900X 12-Core Processor            ` | `IntelLLVM 20250101` | `max` | `L5` | 1009 | `64` | `0xa6faa7b125d2744b` | 55 | CPU fallback | pass |
| SYCL | `AMD Ryzen 9 3900X 12-Core Processor            ` | `IntelLLVM 20250101` | `max` | `L5` | 1009 | `256` | `0xa6faa7b125d2744b` | 49 | CPU fallback | pass |
| SYCL | `AMD Ryzen 9 3900X 12-Core Processor            ` | `IntelLLVM 20250101` | `max` | `L5` | 65536 | `auto` | `0xa9e5d673221eef8f` | 366 | CPU fallback | pass |
| SYCL | `AMD Ryzen 9 3900X 12-Core Processor            ` | `IntelLLVM 20250101` | `max` | `L5` | 65536 | `64` | `0xa9e5d673221eef8f` | 347 | CPU fallback | pass |
| SYCL | `AMD Ryzen 9 3900X 12-Core Processor            ` | `IntelLLVM 20250101` | `max` | `L5` | 65536 | `256` | `0xa9e5d673221eef8f` | 238 | CPU fallback | pass |
| SYCL | `AMD Ryzen 9 3900X 12-Core Processor            ` | `IntelLLVM 20250101` | `neg` | `L10` | 0 | `auto` | `0x14650fb0739d0383` | 41 | CPU fallback | pass |
| SYCL | `AMD Ryzen 9 3900X 12-Core Processor            ` | `IntelLLVM 20250101` | `neg` | `L10` | 0 | `64` | `0x14650fb0739d0383` | 45 | CPU fallback | pass |
| SYCL | `AMD Ryzen 9 3900X 12-Core Processor            ` | `IntelLLVM 20250101` | `neg` | `L10` | 0 | `256` | `0x14650fb0739d0383` | 35 | CPU fallback | pass |
| SYCL | `AMD Ryzen 9 3900X 12-Core Processor            ` | `IntelLLVM 20250101` | `neg` | `L10` | 1 | `auto` | `0xe2ee4c89d77357cb` | 68 | CPU fallback | pass |
| SYCL | `AMD Ryzen 9 3900X 12-Core Processor            ` | `IntelLLVM 20250101` | `neg` | `L10` | 1 | `64` | `0xe2ee4c89d77357cb` | 33 | CPU fallback | pass |
| SYCL | `AMD Ryzen 9 3900X 12-Core Processor            ` | `IntelLLVM 20250101` | `neg` | `L10` | 1 | `256` | `0xe2ee4c89d77357cb` | 41 | CPU fallback | pass |
| SYCL | `AMD Ryzen 9 3900X 12-Core Processor            ` | `IntelLLVM 20250101` | `neg` | `L10` | 127 | `auto` | `0xd9efd4218792ea82` | 48 | CPU fallback | pass |
| SYCL | `AMD Ryzen 9 3900X 12-Core Processor            ` | `IntelLLVM 20250101` | `neg` | `L10` | 127 | `64` | `0xd9efd4218792ea82` | 39 | CPU fallback | pass |
| SYCL | `AMD Ryzen 9 3900X 12-Core Processor            ` | `IntelLLVM 20250101` | `neg` | `L10` | 127 | `256` | `0xd9efd4218792ea82` | 33 | CPU fallback | pass |
| SYCL | `AMD Ryzen 9 3900X 12-Core Processor            ` | `IntelLLVM 20250101` | `neg` | `L10` | 256 | `auto` | `0x25dd3103e6511096` | 68 | CPU fallback | pass |
| SYCL | `AMD Ryzen 9 3900X 12-Core Processor            ` | `IntelLLVM 20250101` | `neg` | `L10` | 256 | `64` | `0x25dd3103e6511096` | 53 | CPU fallback | pass |
| SYCL | `AMD Ryzen 9 3900X 12-Core Processor            ` | `IntelLLVM 20250101` | `neg` | `L10` | 256 | `256` | `0x25dd3103e6511096` | 35 | CPU fallback | pass |
| SYCL | `AMD Ryzen 9 3900X 12-Core Processor            ` | `IntelLLVM 20250101` | `neg` | `L10` | 1009 | `auto` | `0x2da8b28be8d2cf3a` | 87 | CPU fallback | pass |
| SYCL | `AMD Ryzen 9 3900X 12-Core Processor            ` | `IntelLLVM 20250101` | `neg` | `L10` | 1009 | `64` | `0x2da8b28be8d2cf3a` | 36 | CPU fallback | pass |
| SYCL | `AMD Ryzen 9 3900X 12-Core Processor            ` | `IntelLLVM 20250101` | `neg` | `L10` | 1009 | `256` | `0x2da8b28be8d2cf3a` | 53 | CPU fallback | pass |
| SYCL | `AMD Ryzen 9 3900X 12-Core Processor            ` | `IntelLLVM 20250101` | `neg` | `L10` | 65536 | `auto` | `0x10072dee1b37455c` | 172 | CPU fallback | pass |
| SYCL | `AMD Ryzen 9 3900X 12-Core Processor            ` | `IntelLLVM 20250101` | `neg` | `L10` | 65536 | `64` | `0x10072dee1b37455c` | 171 | CPU fallback | pass |
| SYCL | `AMD Ryzen 9 3900X 12-Core Processor            ` | `IntelLLVM 20250101` | `neg` | `L10` | 65536 | `256` | `0x10072dee1b37455c` | 189 | CPU fallback | pass |
| SYCL | `AMD Ryzen 9 3900X 12-Core Processor            ` | `IntelLLVM 20250101` | `add` | `L10` | 0 | `auto` | `0x14650fb0739d0383` | 41 | CPU fallback | pass |
| SYCL | `AMD Ryzen 9 3900X 12-Core Processor            ` | `IntelLLVM 20250101` | `add` | `L10` | 0 | `64` | `0x14650fb0739d0383` | 26 | CPU fallback | pass |
| SYCL | `AMD Ryzen 9 3900X 12-Core Processor            ` | `IntelLLVM 20250101` | `add` | `L10` | 0 | `256` | `0x14650fb0739d0383` | 35 | CPU fallback | pass |
| SYCL | `AMD Ryzen 9 3900X 12-Core Processor            ` | `IntelLLVM 20250101` | `add` | `L10` | 1 | `auto` | `0x84e2223519b79ec3` | 34 | CPU fallback | pass |
| SYCL | `AMD Ryzen 9 3900X 12-Core Processor            ` | `IntelLLVM 20250101` | `add` | `L10` | 1 | `64` | `0x84e2223519b79ec3` | 35 | CPU fallback | pass |
| SYCL | `AMD Ryzen 9 3900X 12-Core Processor            ` | `IntelLLVM 20250101` | `add` | `L10` | 1 | `256` | `0x84e2223519b79ec3` | 35 | CPU fallback | pass |
| SYCL | `AMD Ryzen 9 3900X 12-Core Processor            ` | `IntelLLVM 20250101` | `add` | `L10` | 127 | `auto` | `0x5e30910cbdb756e9` | 50 | CPU fallback | pass |
| SYCL | `AMD Ryzen 9 3900X 12-Core Processor            ` | `IntelLLVM 20250101` | `add` | `L10` | 127 | `64` | `0x5e30910cbdb756e9` | 42 | CPU fallback | pass |
| SYCL | `AMD Ryzen 9 3900X 12-Core Processor            ` | `IntelLLVM 20250101` | `add` | `L10` | 127 | `256` | `0x5e30910cbdb756e9` | 40 | CPU fallback | pass |
| SYCL | `AMD Ryzen 9 3900X 12-Core Processor            ` | `IntelLLVM 20250101` | `add` | `L10` | 256 | `auto` | `0x9babd73ab35c8213` | 50 | CPU fallback | pass |
| SYCL | `AMD Ryzen 9 3900X 12-Core Processor            ` | `IntelLLVM 20250101` | `add` | `L10` | 256 | `64` | `0x9babd73ab35c8213` | 67 | CPU fallback | pass |
| SYCL | `AMD Ryzen 9 3900X 12-Core Processor            ` | `IntelLLVM 20250101` | `add` | `L10` | 256 | `256` | `0x9babd73ab35c8213` | 61 | CPU fallback | pass |
| SYCL | `AMD Ryzen 9 3900X 12-Core Processor            ` | `IntelLLVM 20250101` | `add` | `L10` | 1009 | `auto` | `0x7ce9c8e2e841f24a` | 83 | CPU fallback | pass |
| SYCL | `AMD Ryzen 9 3900X 12-Core Processor            ` | `IntelLLVM 20250101` | `add` | `L10` | 1009 | `64` | `0x7ce9c8e2e841f24a` | 56 | CPU fallback | pass |
| SYCL | `AMD Ryzen 9 3900X 12-Core Processor            ` | `IntelLLVM 20250101` | `add` | `L10` | 1009 | `256` | `0x7ce9c8e2e841f24a` | 57 | CPU fallback | pass |
| SYCL | `AMD Ryzen 9 3900X 12-Core Processor            ` | `IntelLLVM 20250101` | `add` | `L10` | 65536 | `auto` | `0xbbc75f94f9c4f05c` | 440 | CPU fallback | pass |
| SYCL | `AMD Ryzen 9 3900X 12-Core Processor            ` | `IntelLLVM 20250101` | `add` | `L10` | 65536 | `64` | `0xbbc75f94f9c4f05c` | 520 | CPU fallback | pass |
| SYCL | `AMD Ryzen 9 3900X 12-Core Processor            ` | `IntelLLVM 20250101` | `add` | `L10` | 65536 | `256` | `0xbbc75f94f9c4f05c` | 2819 | CPU fallback | pass |
| SYCL | `AMD Ryzen 9 3900X 12-Core Processor            ` | `IntelLLVM 20250101` | `sub` | `L10` | 0 | `auto` | `0x14650fb0739d0383` | 56 | CPU fallback | pass |
| SYCL | `AMD Ryzen 9 3900X 12-Core Processor            ` | `IntelLLVM 20250101` | `sub` | `L10` | 0 | `64` | `0x14650fb0739d0383` | 54 | CPU fallback | pass |
| SYCL | `AMD Ryzen 9 3900X 12-Core Processor            ` | `IntelLLVM 20250101` | `sub` | `L10` | 0 | `256` | `0x14650fb0739d0383` | 36 | CPU fallback | pass |
| SYCL | `AMD Ryzen 9 3900X 12-Core Processor            ` | `IntelLLVM 20250101` | `sub` | `L10` | 1 | `auto` | `0x69de508800adcdd0` | 45 | CPU fallback | pass |
| SYCL | `AMD Ryzen 9 3900X 12-Core Processor            ` | `IntelLLVM 20250101` | `sub` | `L10` | 1 | `64` | `0x69de508800adcdd0` | 51 | CPU fallback | pass |
| SYCL | `AMD Ryzen 9 3900X 12-Core Processor            ` | `IntelLLVM 20250101` | `sub` | `L10` | 1 | `256` | `0x69de508800adcdd0` | 43 | CPU fallback | pass |
| SYCL | `AMD Ryzen 9 3900X 12-Core Processor            ` | `IntelLLVM 20250101` | `sub` | `L10` | 127 | `auto` | `0xbc29f9d0ab2f358b` | 61 | CPU fallback | pass |
| SYCL | `AMD Ryzen 9 3900X 12-Core Processor            ` | `IntelLLVM 20250101` | `sub` | `L10` | 127 | `64` | `0xbc29f9d0ab2f358b` | 50 | CPU fallback | pass |
| SYCL | `AMD Ryzen 9 3900X 12-Core Processor            ` | `IntelLLVM 20250101` | `sub` | `L10` | 127 | `256` | `0xbc29f9d0ab2f358b` | 42 | CPU fallback | pass |
| SYCL | `AMD Ryzen 9 3900X 12-Core Processor            ` | `IntelLLVM 20250101` | `sub` | `L10` | 256 | `auto` | `0x2bab5af8e5562ade` | 59 | CPU fallback | pass |
| SYCL | `AMD Ryzen 9 3900X 12-Core Processor            ` | `IntelLLVM 20250101` | `sub` | `L10` | 256 | `64` | `0x2bab5af8e5562ade` | 69 | CPU fallback | pass |
| SYCL | `AMD Ryzen 9 3900X 12-Core Processor            ` | `IntelLLVM 20250101` | `sub` | `L10` | 256 | `256` | `0x2bab5af8e5562ade` | 54 | CPU fallback | pass |
| SYCL | `AMD Ryzen 9 3900X 12-Core Processor            ` | `IntelLLVM 20250101` | `sub` | `L10` | 1009 | `auto` | `0x6d9d27b607076943` | 160 | CPU fallback | pass |
| SYCL | `AMD Ryzen 9 3900X 12-Core Processor            ` | `IntelLLVM 20250101` | `sub` | `L10` | 1009 | `64` | `0x6d9d27b607076943` | 50 | CPU fallback | pass |
| SYCL | `AMD Ryzen 9 3900X 12-Core Processor            ` | `IntelLLVM 20250101` | `sub` | `L10` | 1009 | `256` | `0x6d9d27b607076943` | 76 | CPU fallback | pass |
| SYCL | `AMD Ryzen 9 3900X 12-Core Processor            ` | `IntelLLVM 20250101` | `sub` | `L10` | 65536 | `auto` | `0xd3c14b6e077b572e` | 662 | CPU fallback | pass |
| SYCL | `AMD Ryzen 9 3900X 12-Core Processor            ` | `IntelLLVM 20250101` | `sub` | `L10` | 65536 | `64` | `0xd3c14b6e077b572e` | 462 | CPU fallback | pass |
| SYCL | `AMD Ryzen 9 3900X 12-Core Processor            ` | `IntelLLVM 20250101` | `sub` | `L10` | 65536 | `256` | `0xd3c14b6e077b572e` | 406 | CPU fallback | pass |
| SYCL | `AMD Ryzen 9 3900X 12-Core Processor            ` | `IntelLLVM 20250101` | `compare` | `L10` | 0 | `auto` | `0x14650fb0739d0383` | 37 | CPU fallback | pass |
| SYCL | `AMD Ryzen 9 3900X 12-Core Processor            ` | `IntelLLVM 20250101` | `compare` | `L10` | 0 | `64` | `0x14650fb0739d0383` | 31 | CPU fallback | pass |
| SYCL | `AMD Ryzen 9 3900X 12-Core Processor            ` | `IntelLLVM 20250101` | `compare` | `L10` | 0 | `256` | `0x14650fb0739d0383` | 26 | CPU fallback | pass |
| SYCL | `AMD Ryzen 9 3900X 12-Core Processor            ` | `IntelLLVM 20250101` | `compare` | `L10` | 1 | `auto` | `0x44bd2bd473ccf799` | 45 | CPU fallback | pass |
| SYCL | `AMD Ryzen 9 3900X 12-Core Processor            ` | `IntelLLVM 20250101` | `compare` | `L10` | 1 | `64` | `0x44bd2bd473ccf799` | 36 | CPU fallback | pass |
| SYCL | `AMD Ryzen 9 3900X 12-Core Processor            ` | `IntelLLVM 20250101` | `compare` | `L10` | 1 | `256` | `0x44bd2bd473ccf799` | 44 | CPU fallback | pass |
| SYCL | `AMD Ryzen 9 3900X 12-Core Processor            ` | `IntelLLVM 20250101` | `compare` | `L10` | 127 | `auto` | `0xa178bf1ff6e1ca11` | 45 | CPU fallback | pass |
| SYCL | `AMD Ryzen 9 3900X 12-Core Processor            ` | `IntelLLVM 20250101` | `compare` | `L10` | 127 | `64` | `0xa178bf1ff6e1ca11` | 54 | CPU fallback | pass |
| SYCL | `AMD Ryzen 9 3900X 12-Core Processor            ` | `IntelLLVM 20250101` | `compare` | `L10` | 127 | `256` | `0xa178bf1ff6e1ca11` | 51 | CPU fallback | pass |
| SYCL | `AMD Ryzen 9 3900X 12-Core Processor            ` | `IntelLLVM 20250101` | `compare` | `L10` | 256 | `auto` | `0x4e8eb0a1edddf25f` | 70 | CPU fallback | pass |
| SYCL | `AMD Ryzen 9 3900X 12-Core Processor            ` | `IntelLLVM 20250101` | `compare` | `L10` | 256 | `64` | `0x4e8eb0a1edddf25f` | 49 | CPU fallback | pass |
| SYCL | `AMD Ryzen 9 3900X 12-Core Processor            ` | `IntelLLVM 20250101` | `compare` | `L10` | 256 | `256` | `0x4e8eb0a1edddf25f` | 90 | CPU fallback | pass |
| SYCL | `AMD Ryzen 9 3900X 12-Core Processor            ` | `IntelLLVM 20250101` | `compare` | `L10` | 1009 | `auto` | `0xdd6a189897c960b3` | 110 | CPU fallback | pass |
| SYCL | `AMD Ryzen 9 3900X 12-Core Processor            ` | `IntelLLVM 20250101` | `compare` | `L10` | 1009 | `64` | `0xdd6a189897c960b3` | 82 | CPU fallback | pass |
| SYCL | `AMD Ryzen 9 3900X 12-Core Processor            ` | `IntelLLVM 20250101` | `compare` | `L10` | 1009 | `256` | `0xdd6a189897c960b3` | 87 | CPU fallback | pass |
| SYCL | `AMD Ryzen 9 3900X 12-Core Processor            ` | `IntelLLVM 20250101` | `compare` | `L10` | 65536 | `auto` | `0x6d398767d93edad9` | 293 | CPU fallback | pass |
| SYCL | `AMD Ryzen 9 3900X 12-Core Processor            ` | `IntelLLVM 20250101` | `compare` | `L10` | 65536 | `64` | `0x6d398767d93edad9` | 528 | CPU fallback | pass |
| SYCL | `AMD Ryzen 9 3900X 12-Core Processor            ` | `IntelLLVM 20250101` | `compare` | `L10` | 65536 | `256` | `0x6d398767d93edad9` | 584 | CPU fallback | pass |
| SYCL | `AMD Ryzen 9 3900X 12-Core Processor            ` | `IntelLLVM 20250101` | `min` | `L10` | 0 | `auto` | `0x14650fb0739d0383` | 47 | CPU fallback | pass |
| SYCL | `AMD Ryzen 9 3900X 12-Core Processor            ` | `IntelLLVM 20250101` | `min` | `L10` | 0 | `64` | `0x14650fb0739d0383` | 21 | CPU fallback | pass |
| SYCL | `AMD Ryzen 9 3900X 12-Core Processor            ` | `IntelLLVM 20250101` | `min` | `L10` | 0 | `256` | `0x14650fb0739d0383` | 64 | CPU fallback | pass |
| SYCL | `AMD Ryzen 9 3900X 12-Core Processor            ` | `IntelLLVM 20250101` | `min` | `L10` | 1 | `auto` | `0x77659a132aaaa669` | 49 | CPU fallback | pass |
| SYCL | `AMD Ryzen 9 3900X 12-Core Processor            ` | `IntelLLVM 20250101` | `min` | `L10` | 1 | `64` | `0x77659a132aaaa669` | 48 | CPU fallback | pass |
| SYCL | `AMD Ryzen 9 3900X 12-Core Processor            ` | `IntelLLVM 20250101` | `min` | `L10` | 1 | `256` | `0x77659a132aaaa669` | 48 | CPU fallback | pass |
| SYCL | `AMD Ryzen 9 3900X 12-Core Processor            ` | `IntelLLVM 20250101` | `min` | `L10` | 127 | `auto` | `0x7794b808f8cec2b1` | 58 | CPU fallback | pass |
| SYCL | `AMD Ryzen 9 3900X 12-Core Processor            ` | `IntelLLVM 20250101` | `min` | `L10` | 127 | `64` | `0x7794b808f8cec2b1` | 44 | CPU fallback | pass |
| SYCL | `AMD Ryzen 9 3900X 12-Core Processor            ` | `IntelLLVM 20250101` | `min` | `L10` | 127 | `256` | `0x7794b808f8cec2b1` | 45 | CPU fallback | pass |
| SYCL | `AMD Ryzen 9 3900X 12-Core Processor            ` | `IntelLLVM 20250101` | `min` | `L10` | 256 | `auto` | `0x990fb86a83fec479` | 51 | CPU fallback | pass |
| SYCL | `AMD Ryzen 9 3900X 12-Core Processor            ` | `IntelLLVM 20250101` | `min` | `L10` | 256 | `64` | `0x990fb86a83fec479` | 53 | CPU fallback | pass |
| SYCL | `AMD Ryzen 9 3900X 12-Core Processor            ` | `IntelLLVM 20250101` | `min` | `L10` | 256 | `256` | `0x990fb86a83fec479` | 45 | CPU fallback | pass |
| SYCL | `AMD Ryzen 9 3900X 12-Core Processor            ` | `IntelLLVM 20250101` | `min` | `L10` | 1009 | `auto` | `0x8f045824a7d0cb8d` | 138 | CPU fallback | pass |
| SYCL | `AMD Ryzen 9 3900X 12-Core Processor            ` | `IntelLLVM 20250101` | `min` | `L10` | 1009 | `64` | `0x8f045824a7d0cb8d` | 46 | CPU fallback | pass |
| SYCL | `AMD Ryzen 9 3900X 12-Core Processor            ` | `IntelLLVM 20250101` | `min` | `L10` | 1009 | `256` | `0x8f045824a7d0cb8d` | 52 | CPU fallback | pass |
| SYCL | `AMD Ryzen 9 3900X 12-Core Processor            ` | `IntelLLVM 20250101` | `min` | `L10` | 65536 | `auto` | `0x8fc419cf978f7542` | 707 | CPU fallback | pass |
| SYCL | `AMD Ryzen 9 3900X 12-Core Processor            ` | `IntelLLVM 20250101` | `min` | `L10` | 65536 | `64` | `0x8fc419cf978f7542` | 416 | CPU fallback | pass |
| SYCL | `AMD Ryzen 9 3900X 12-Core Processor            ` | `IntelLLVM 20250101` | `min` | `L10` | 65536 | `256` | `0x8fc419cf978f7542` | 297 | CPU fallback | pass |
| SYCL | `AMD Ryzen 9 3900X 12-Core Processor            ` | `IntelLLVM 20250101` | `max` | `L10` | 0 | `auto` | `0x14650fb0739d0383` | 26 | CPU fallback | pass |
| SYCL | `AMD Ryzen 9 3900X 12-Core Processor            ` | `IntelLLVM 20250101` | `max` | `L10` | 0 | `64` | `0x14650fb0739d0383` | 23 | CPU fallback | pass |
| SYCL | `AMD Ryzen 9 3900X 12-Core Processor            ` | `IntelLLVM 20250101` | `max` | `L10` | 0 | `256` | `0x14650fb0739d0383` | 46 | CPU fallback | pass |
| SYCL | `AMD Ryzen 9 3900X 12-Core Processor            ` | `IntelLLVM 20250101` | `max` | `L10` | 1 | `auto` | `0xaa628ba7858e5af6` | 36 | CPU fallback | pass |
| SYCL | `AMD Ryzen 9 3900X 12-Core Processor            ` | `IntelLLVM 20250101` | `max` | `L10` | 1 | `64` | `0xaa628ba7858e5af6` | 42 | CPU fallback | pass |
| SYCL | `AMD Ryzen 9 3900X 12-Core Processor            ` | `IntelLLVM 20250101` | `max` | `L10` | 1 | `256` | `0xaa628ba7858e5af6` | 33 | CPU fallback | pass |
| SYCL | `AMD Ryzen 9 3900X 12-Core Processor            ` | `IntelLLVM 20250101` | `max` | `L10` | 127 | `auto` | `0x3e0d5ecc74c2bbfa` | 93 | CPU fallback | pass |
| SYCL | `AMD Ryzen 9 3900X 12-Core Processor            ` | `IntelLLVM 20250101` | `max` | `L10` | 127 | `64` | `0x3e0d5ecc74c2bbfa` | 45 | CPU fallback | pass |
| SYCL | `AMD Ryzen 9 3900X 12-Core Processor            ` | `IntelLLVM 20250101` | `max` | `L10` | 127 | `256` | `0x3e0d5ecc74c2bbfa` | 42 | CPU fallback | pass |
| SYCL | `AMD Ryzen 9 3900X 12-Core Processor            ` | `IntelLLVM 20250101` | `max` | `L10` | 256 | `auto` | `0x4c32569d6a02c965` | 70 | CPU fallback | pass |
| SYCL | `AMD Ryzen 9 3900X 12-Core Processor            ` | `IntelLLVM 20250101` | `max` | `L10` | 256 | `64` | `0x4c32569d6a02c965` | 44 | CPU fallback | pass |
| SYCL | `AMD Ryzen 9 3900X 12-Core Processor            ` | `IntelLLVM 20250101` | `max` | `L10` | 256 | `256` | `0x4c32569d6a02c965` | 42 | CPU fallback | pass |
| SYCL | `AMD Ryzen 9 3900X 12-Core Processor            ` | `IntelLLVM 20250101` | `max` | `L10` | 1009 | `auto` | `0x46c2073ab23a768d` | 64 | CPU fallback | pass |
| SYCL | `AMD Ryzen 9 3900X 12-Core Processor            ` | `IntelLLVM 20250101` | `max` | `L10` | 1009 | `64` | `0x46c2073ab23a768d` | 66 | CPU fallback | pass |
| SYCL | `AMD Ryzen 9 3900X 12-Core Processor            ` | `IntelLLVM 20250101` | `max` | `L10` | 1009 | `256` | `0x46c2073ab23a768d` | 74 | CPU fallback | pass |
| SYCL | `AMD Ryzen 9 3900X 12-Core Processor            ` | `IntelLLVM 20250101` | `max` | `L10` | 65536 | `auto` | `0xf9cd3a4f52e88421` | 243 | CPU fallback | pass |
| SYCL | `AMD Ryzen 9 3900X 12-Core Processor            ` | `IntelLLVM 20250101` | `max` | `L10` | 65536 | `64` | `0xf9cd3a4f52e88421` | 243 | CPU fallback | pass |
| SYCL | `AMD Ryzen 9 3900X 12-Core Processor            ` | `IntelLLVM 20250101` | `max` | `L10` | 65536 | `256` | `0xf9cd3a4f52e88421` | 263 | CPU fallback | pass |
| SYCL | `AMD Ryzen 9 3900X 12-Core Processor            ` | `IntelLLVM 20250101` | `neg` | `L20` | 0 | `auto` | `0x14650fb0739d0383` | 58 | CPU fallback | pass |
| SYCL | `AMD Ryzen 9 3900X 12-Core Processor            ` | `IntelLLVM 20250101` | `neg` | `L20` | 0 | `64` | `0x14650fb0739d0383` | 36 | CPU fallback | pass |
| SYCL | `AMD Ryzen 9 3900X 12-Core Processor            ` | `IntelLLVM 20250101` | `neg` | `L20` | 0 | `256` | `0x14650fb0739d0383` | 32 | CPU fallback | pass |
| SYCL | `AMD Ryzen 9 3900X 12-Core Processor            ` | `IntelLLVM 20250101` | `neg` | `L20` | 1 | `auto` | `0x3a757c5626368a4d` | 35 | CPU fallback | pass |
| SYCL | `AMD Ryzen 9 3900X 12-Core Processor            ` | `IntelLLVM 20250101` | `neg` | `L20` | 1 | `64` | `0x3a757c5626368a4d` | 47 | CPU fallback | pass |
| SYCL | `AMD Ryzen 9 3900X 12-Core Processor            ` | `IntelLLVM 20250101` | `neg` | `L20` | 1 | `256` | `0x3a757c5626368a4d` | 35 | CPU fallback | pass |
| SYCL | `AMD Ryzen 9 3900X 12-Core Processor            ` | `IntelLLVM 20250101` | `neg` | `L20` | 127 | `auto` | `0x54050dbdf7fe0884` | 49 | CPU fallback | pass |
| SYCL | `AMD Ryzen 9 3900X 12-Core Processor            ` | `IntelLLVM 20250101` | `neg` | `L20` | 127 | `64` | `0x54050dbdf7fe0884` | 62 | CPU fallback | pass |
| SYCL | `AMD Ryzen 9 3900X 12-Core Processor            ` | `IntelLLVM 20250101` | `neg` | `L20` | 127 | `256` | `0x54050dbdf7fe0884` | 39 | CPU fallback | pass |
| SYCL | `AMD Ryzen 9 3900X 12-Core Processor            ` | `IntelLLVM 20250101` | `neg` | `L20` | 256 | `auto` | `0x9601b1701024930d` | 99 | CPU fallback | pass |
| SYCL | `AMD Ryzen 9 3900X 12-Core Processor            ` | `IntelLLVM 20250101` | `neg` | `L20` | 256 | `64` | `0x9601b1701024930d` | 39 | CPU fallback | pass |
| SYCL | `AMD Ryzen 9 3900X 12-Core Processor            ` | `IntelLLVM 20250101` | `neg` | `L20` | 256 | `256` | `0x9601b1701024930d` | 37 | CPU fallback | pass |
| SYCL | `AMD Ryzen 9 3900X 12-Core Processor            ` | `IntelLLVM 20250101` | `neg` | `L20` | 1009 | `auto` | `0xa9bb068ebeb07edd` | 53 | CPU fallback | pass |
| SYCL | `AMD Ryzen 9 3900X 12-Core Processor            ` | `IntelLLVM 20250101` | `neg` | `L20` | 1009 | `64` | `0xa9bb068ebeb07edd` | 50 | CPU fallback | pass |
| SYCL | `AMD Ryzen 9 3900X 12-Core Processor            ` | `IntelLLVM 20250101` | `neg` | `L20` | 1009 | `256` | `0xa9bb068ebeb07edd` | 54 | CPU fallback | pass |
| SYCL | `AMD Ryzen 9 3900X 12-Core Processor            ` | `IntelLLVM 20250101` | `neg` | `L20` | 65536 | `auto` | `0x99038cfb1cd7469f` | 322 | CPU fallback | pass |
| SYCL | `AMD Ryzen 9 3900X 12-Core Processor            ` | `IntelLLVM 20250101` | `neg` | `L20` | 65536 | `64` | `0x99038cfb1cd7469f` | 210 | CPU fallback | pass |
| SYCL | `AMD Ryzen 9 3900X 12-Core Processor            ` | `IntelLLVM 20250101` | `neg` | `L20` | 65536 | `256` | `0x99038cfb1cd7469f` | 280 | CPU fallback | pass |
| SYCL | `AMD Ryzen 9 3900X 12-Core Processor            ` | `IntelLLVM 20250101` | `add` | `L20` | 0 | `auto` | `0x14650fb0739d0383` | 44 | CPU fallback | pass |
| SYCL | `AMD Ryzen 9 3900X 12-Core Processor            ` | `IntelLLVM 20250101` | `add` | `L20` | 0 | `64` | `0x14650fb0739d0383` | 38 | CPU fallback | pass |
| SYCL | `AMD Ryzen 9 3900X 12-Core Processor            ` | `IntelLLVM 20250101` | `add` | `L20` | 0 | `256` | `0x14650fb0739d0383` | 20 | CPU fallback | pass |
| SYCL | `AMD Ryzen 9 3900X 12-Core Processor            ` | `IntelLLVM 20250101` | `add` | `L20` | 1 | `auto` | `0x4ae89020cad63223` | 48 | CPU fallback | pass |
| SYCL | `AMD Ryzen 9 3900X 12-Core Processor            ` | `IntelLLVM 20250101` | `add` | `L20` | 1 | `64` | `0x4ae89020cad63223` | 52 | CPU fallback | pass |
| SYCL | `AMD Ryzen 9 3900X 12-Core Processor            ` | `IntelLLVM 20250101` | `add` | `L20` | 1 | `256` | `0x4ae89020cad63223` | 42 | CPU fallback | pass |
| SYCL | `AMD Ryzen 9 3900X 12-Core Processor            ` | `IntelLLVM 20250101` | `add` | `L20` | 127 | `auto` | `0xa85eb7215d0858dd` | 46 | CPU fallback | pass |
| SYCL | `AMD Ryzen 9 3900X 12-Core Processor            ` | `IntelLLVM 20250101` | `add` | `L20` | 127 | `64` | `0xa85eb7215d0858dd` | 99 | CPU fallback | pass |
| SYCL | `AMD Ryzen 9 3900X 12-Core Processor            ` | `IntelLLVM 20250101` | `add` | `L20` | 127 | `256` | `0xa85eb7215d0858dd` | 64 | CPU fallback | pass |
| SYCL | `AMD Ryzen 9 3900X 12-Core Processor            ` | `IntelLLVM 20250101` | `add` | `L20` | 256 | `auto` | `0xf3c3ee621a62bfbd` | 68 | CPU fallback | pass |
| SYCL | `AMD Ryzen 9 3900X 12-Core Processor            ` | `IntelLLVM 20250101` | `add` | `L20` | 256 | `64` | `0xf3c3ee621a62bfbd` | 126 | CPU fallback | pass |
| SYCL | `AMD Ryzen 9 3900X 12-Core Processor            ` | `IntelLLVM 20250101` | `add` | `L20` | 256 | `256` | `0xf3c3ee621a62bfbd` | 96 | CPU fallback | pass |
| SYCL | `AMD Ryzen 9 3900X 12-Core Processor            ` | `IntelLLVM 20250101` | `add` | `L20` | 1009 | `auto` | `0xd0bd46a160928676` | 119 | CPU fallback | pass |
| SYCL | `AMD Ryzen 9 3900X 12-Core Processor            ` | `IntelLLVM 20250101` | `add` | `L20` | 1009 | `64` | `0xd0bd46a160928676` | 70 | CPU fallback | pass |
| SYCL | `AMD Ryzen 9 3900X 12-Core Processor            ` | `IntelLLVM 20250101` | `add` | `L20` | 1009 | `256` | `0xd0bd46a160928676` | 77 | CPU fallback | pass |
| SYCL | `AMD Ryzen 9 3900X 12-Core Processor            ` | `IntelLLVM 20250101` | `add` | `L20` | 65536 | `auto` | `0x7f2beabcfdf86991` | 885 | CPU fallback | pass |
| SYCL | `AMD Ryzen 9 3900X 12-Core Processor            ` | `IntelLLVM 20250101` | `add` | `L20` | 65536 | `64` | `0x7f2beabcfdf86991` | 645 | CPU fallback | pass |
| SYCL | `AMD Ryzen 9 3900X 12-Core Processor            ` | `IntelLLVM 20250101` | `add` | `L20` | 65536 | `256` | `0x7f2beabcfdf86991` | 677 | CPU fallback | pass |
| SYCL | `AMD Ryzen 9 3900X 12-Core Processor            ` | `IntelLLVM 20250101` | `sub` | `L20` | 0 | `auto` | `0x14650fb0739d0383` | 45 | CPU fallback | pass |
| SYCL | `AMD Ryzen 9 3900X 12-Core Processor            ` | `IntelLLVM 20250101` | `sub` | `L20` | 0 | `64` | `0x14650fb0739d0383` | 46 | CPU fallback | pass |
| SYCL | `AMD Ryzen 9 3900X 12-Core Processor            ` | `IntelLLVM 20250101` | `sub` | `L20` | 0 | `256` | `0x14650fb0739d0383` | 37 | CPU fallback | pass |
| SYCL | `AMD Ryzen 9 3900X 12-Core Processor            ` | `IntelLLVM 20250101` | `sub` | `L20` | 1 | `auto` | `0x6b156808b734e570` | 47 | CPU fallback | pass |
| SYCL | `AMD Ryzen 9 3900X 12-Core Processor            ` | `IntelLLVM 20250101` | `sub` | `L20` | 1 | `64` | `0x6b156808b734e570` | 138 | CPU fallback | pass |
| SYCL | `AMD Ryzen 9 3900X 12-Core Processor            ` | `IntelLLVM 20250101` | `sub` | `L20` | 1 | `256` | `0x6b156808b734e570` | 102 | CPU fallback | pass |
| SYCL | `AMD Ryzen 9 3900X 12-Core Processor            ` | `IntelLLVM 20250101` | `sub` | `L20` | 127 | `auto` | `0xcc4524b120fb6a77` | 96 | CPU fallback | pass |
| SYCL | `AMD Ryzen 9 3900X 12-Core Processor            ` | `IntelLLVM 20250101` | `sub` | `L20` | 127 | `64` | `0xcc4524b120fb6a77` | 60 | CPU fallback | pass |
| SYCL | `AMD Ryzen 9 3900X 12-Core Processor            ` | `IntelLLVM 20250101` | `sub` | `L20` | 127 | `256` | `0xcc4524b120fb6a77` | 51 | CPU fallback | pass |
| SYCL | `AMD Ryzen 9 3900X 12-Core Processor            ` | `IntelLLVM 20250101` | `sub` | `L20` | 256 | `auto` | `0x40a640346d5a3e46` | 87 | CPU fallback | pass |
| SYCL | `AMD Ryzen 9 3900X 12-Core Processor            ` | `IntelLLVM 20250101` | `sub` | `L20` | 256 | `64` | `0x40a640346d5a3e46` | 208 | CPU fallback | pass |
| SYCL | `AMD Ryzen 9 3900X 12-Core Processor            ` | `IntelLLVM 20250101` | `sub` | `L20` | 256 | `256` | `0x40a640346d5a3e46` | 60 | CPU fallback | pass |
| SYCL | `AMD Ryzen 9 3900X 12-Core Processor            ` | `IntelLLVM 20250101` | `sub` | `L20` | 1009 | `auto` | `0xc2f4a7f94dd8a96d` | 100 | CPU fallback | pass |
| SYCL | `AMD Ryzen 9 3900X 12-Core Processor            ` | `IntelLLVM 20250101` | `sub` | `L20` | 1009 | `64` | `0xc2f4a7f94dd8a96d` | 106 | CPU fallback | pass |
| SYCL | `AMD Ryzen 9 3900X 12-Core Processor            ` | `IntelLLVM 20250101` | `sub` | `L20` | 1009 | `256` | `0xc2f4a7f94dd8a96d` | 93 | CPU fallback | pass |
| SYCL | `AMD Ryzen 9 3900X 12-Core Processor            ` | `IntelLLVM 20250101` | `sub` | `L20` | 65536 | `auto` | `0x601bd12b2d67a2eb` | 1238 | CPU fallback | pass |
| SYCL | `AMD Ryzen 9 3900X 12-Core Processor            ` | `IntelLLVM 20250101` | `sub` | `L20` | 65536 | `64` | `0x601bd12b2d67a2eb` | 1550 | CPU fallback | pass |
| SYCL | `AMD Ryzen 9 3900X 12-Core Processor            ` | `IntelLLVM 20250101` | `sub` | `L20` | 65536 | `256` | `0x601bd12b2d67a2eb` | 1245 | CPU fallback | pass |
| SYCL | `AMD Ryzen 9 3900X 12-Core Processor            ` | `IntelLLVM 20250101` | `compare` | `L20` | 0 | `auto` | `0x14650fb0739d0383` | 40 | CPU fallback | pass |
| SYCL | `AMD Ryzen 9 3900X 12-Core Processor            ` | `IntelLLVM 20250101` | `compare` | `L20` | 0 | `64` | `0x14650fb0739d0383` | 33 | CPU fallback | pass |
| SYCL | `AMD Ryzen 9 3900X 12-Core Processor            ` | `IntelLLVM 20250101` | `compare` | `L20` | 0 | `256` | `0x14650fb0739d0383` | 40 | CPU fallback | pass |
| SYCL | `AMD Ryzen 9 3900X 12-Core Processor            ` | `IntelLLVM 20250101` | `compare` | `L20` | 1 | `auto` | `0x44bd2bd473ccf799` | 33 | CPU fallback | pass |
| SYCL | `AMD Ryzen 9 3900X 12-Core Processor            ` | `IntelLLVM 20250101` | `compare` | `L20` | 1 | `64` | `0x44bd2bd473ccf799` | 42 | CPU fallback | pass |
| SYCL | `AMD Ryzen 9 3900X 12-Core Processor            ` | `IntelLLVM 20250101` | `compare` | `L20` | 1 | `256` | `0x44bd2bd473ccf799` | 36 | CPU fallback | pass |
| SYCL | `AMD Ryzen 9 3900X 12-Core Processor            ` | `IntelLLVM 20250101` | `compare` | `L20` | 127 | `auto` | `0x66d1e3627a854f9b` | 52 | CPU fallback | pass |
| SYCL | `AMD Ryzen 9 3900X 12-Core Processor            ` | `IntelLLVM 20250101` | `compare` | `L20` | 127 | `64` | `0x66d1e3627a854f9b` | 53 | CPU fallback | pass |
| SYCL | `AMD Ryzen 9 3900X 12-Core Processor            ` | `IntelLLVM 20250101` | `compare` | `L20` | 127 | `256` | `0x66d1e3627a854f9b` | 60 | CPU fallback | pass |
| SYCL | `AMD Ryzen 9 3900X 12-Core Processor            ` | `IntelLLVM 20250101` | `compare` | `L20` | 256 | `auto` | `0xf1a8c2f31d3382c1` | 56 | CPU fallback | pass |
| SYCL | `AMD Ryzen 9 3900X 12-Core Processor            ` | `IntelLLVM 20250101` | `compare` | `L20` | 256 | `64` | `0xf1a8c2f31d3382c1` | 65 | CPU fallback | pass |
| SYCL | `AMD Ryzen 9 3900X 12-Core Processor            ` | `IntelLLVM 20250101` | `compare` | `L20` | 256 | `256` | `0xf1a8c2f31d3382c1` | 62 | CPU fallback | pass |
| SYCL | `AMD Ryzen 9 3900X 12-Core Processor            ` | `IntelLLVM 20250101` | `compare` | `L20` | 1009 | `auto` | `0xef309cdf0d086abd` | 101 | CPU fallback | pass |
| SYCL | `AMD Ryzen 9 3900X 12-Core Processor            ` | `IntelLLVM 20250101` | `compare` | `L20` | 1009 | `64` | `0xef309cdf0d086abd` | 84 | CPU fallback | pass |
| SYCL | `AMD Ryzen 9 3900X 12-Core Processor            ` | `IntelLLVM 20250101` | `compare` | `L20` | 1009 | `256` | `0xef309cdf0d086abd` | 66 | CPU fallback | pass |
| SYCL | `AMD Ryzen 9 3900X 12-Core Processor            ` | `IntelLLVM 20250101` | `compare` | `L20` | 65536 | `auto` | `0x8eee36930ac021e9` | 409 | CPU fallback | pass |
| SYCL | `AMD Ryzen 9 3900X 12-Core Processor            ` | `IntelLLVM 20250101` | `compare` | `L20` | 65536 | `64` | `0x8eee36930ac021e9` | 1969 | CPU fallback | pass |
| SYCL | `AMD Ryzen 9 3900X 12-Core Processor            ` | `IntelLLVM 20250101` | `compare` | `L20` | 65536 | `256` | `0x8eee36930ac021e9` | 302 | CPU fallback | pass |
| SYCL | `AMD Ryzen 9 3900X 12-Core Processor            ` | `IntelLLVM 20250101` | `min` | `L20` | 0 | `auto` | `0x14650fb0739d0383` | 65 | CPU fallback | pass |
| SYCL | `AMD Ryzen 9 3900X 12-Core Processor            ` | `IntelLLVM 20250101` | `min` | `L20` | 0 | `64` | `0x14650fb0739d0383` | 27 | CPU fallback | pass |
| SYCL | `AMD Ryzen 9 3900X 12-Core Processor            ` | `IntelLLVM 20250101` | `min` | `L20` | 0 | `256` | `0x14650fb0739d0383` | 30 | CPU fallback | pass |
| SYCL | `AMD Ryzen 9 3900X 12-Core Processor            ` | `IntelLLVM 20250101` | `min` | `L20` | 1 | `auto` | `0xb60274812cd61b43` | 58 | CPU fallback | pass |
| SYCL | `AMD Ryzen 9 3900X 12-Core Processor            ` | `IntelLLVM 20250101` | `min` | `L20` | 1 | `64` | `0xb60274812cd61b43` | 39 | CPU fallback | pass |
| SYCL | `AMD Ryzen 9 3900X 12-Core Processor            ` | `IntelLLVM 20250101` | `min` | `L20` | 1 | `256` | `0xb60274812cd61b43` | 23 | CPU fallback | pass |
| SYCL | `AMD Ryzen 9 3900X 12-Core Processor            ` | `IntelLLVM 20250101` | `min` | `L20` | 127 | `auto` | `0xfc4238b6f756afd5` | 53 | CPU fallback | pass |
| SYCL | `AMD Ryzen 9 3900X 12-Core Processor            ` | `IntelLLVM 20250101` | `min` | `L20` | 127 | `64` | `0xfc4238b6f756afd5` | 58 | CPU fallback | pass |
| SYCL | `AMD Ryzen 9 3900X 12-Core Processor            ` | `IntelLLVM 20250101` | `min` | `L20` | 127 | `256` | `0xfc4238b6f756afd5` | 47 | CPU fallback | pass |
| SYCL | `AMD Ryzen 9 3900X 12-Core Processor            ` | `IntelLLVM 20250101` | `min` | `L20` | 256 | `auto` | `0xf26b167b8d17a850` | 49 | CPU fallback | pass |
| SYCL | `AMD Ryzen 9 3900X 12-Core Processor            ` | `IntelLLVM 20250101` | `min` | `L20` | 256 | `64` | `0xf26b167b8d17a850` | 42 | CPU fallback | pass |
| SYCL | `AMD Ryzen 9 3900X 12-Core Processor            ` | `IntelLLVM 20250101` | `min` | `L20` | 256 | `256` | `0xf26b167b8d17a850` | 41 | CPU fallback | pass |
| SYCL | `AMD Ryzen 9 3900X 12-Core Processor            ` | `IntelLLVM 20250101` | `min` | `L20` | 1009 | `auto` | `0x2bcbdb88de65386c` | 77 | CPU fallback | pass |
| SYCL | `AMD Ryzen 9 3900X 12-Core Processor            ` | `IntelLLVM 20250101` | `min` | `L20` | 1009 | `64` | `0x2bcbdb88de65386c` | 66 | CPU fallback | pass |
| SYCL | `AMD Ryzen 9 3900X 12-Core Processor            ` | `IntelLLVM 20250101` | `min` | `L20` | 1009 | `256` | `0x2bcbdb88de65386c` | 63 | CPU fallback | pass |
| SYCL | `AMD Ryzen 9 3900X 12-Core Processor            ` | `IntelLLVM 20250101` | `min` | `L20` | 65536 | `auto` | `0xe7d2f01de0d36c7a` | 602 | CPU fallback | pass |
| SYCL | `AMD Ryzen 9 3900X 12-Core Processor            ` | `IntelLLVM 20250101` | `min` | `L20` | 65536 | `64` | `0xe7d2f01de0d36c7a` | 519 | CPU fallback | pass |
| SYCL | `AMD Ryzen 9 3900X 12-Core Processor            ` | `IntelLLVM 20250101` | `min` | `L20` | 65536 | `256` | `0xe7d2f01de0d36c7a` | 476 | CPU fallback | pass |
| SYCL | `AMD Ryzen 9 3900X 12-Core Processor            ` | `IntelLLVM 20250101` | `max` | `L20` | 0 | `auto` | `0x14650fb0739d0383` | 23 | CPU fallback | pass |
| SYCL | `AMD Ryzen 9 3900X 12-Core Processor            ` | `IntelLLVM 20250101` | `max` | `L20` | 0 | `64` | `0x14650fb0739d0383` | 27 | CPU fallback | pass |
| SYCL | `AMD Ryzen 9 3900X 12-Core Processor            ` | `IntelLLVM 20250101` | `max` | `L20` | 0 | `256` | `0x14650fb0739d0383` | 39 | CPU fallback | pass |
| SYCL | `AMD Ryzen 9 3900X 12-Core Processor            ` | `IntelLLVM 20250101` | `max` | `L20` | 1 | `auto` | `0xb5b98756b4f79166` | 79 | CPU fallback | pass |
| SYCL | `AMD Ryzen 9 3900X 12-Core Processor            ` | `IntelLLVM 20250101` | `max` | `L20` | 1 | `64` | `0xb5b98756b4f79166` | 419 | CPU fallback | pass |
| SYCL | `AMD Ryzen 9 3900X 12-Core Processor            ` | `IntelLLVM 20250101` | `max` | `L20` | 1 | `256` | `0xb5b98756b4f79166` | 44 | CPU fallback | pass |
| SYCL | `AMD Ryzen 9 3900X 12-Core Processor            ` | `IntelLLVM 20250101` | `max` | `L20` | 127 | `auto` | `0x947a906c4a24d632` | 111 | CPU fallback | pass |
| SYCL | `AMD Ryzen 9 3900X 12-Core Processor            ` | `IntelLLVM 20250101` | `max` | `L20` | 127 | `64` | `0x947a906c4a24d632` | 49 | CPU fallback | pass |
| SYCL | `AMD Ryzen 9 3900X 12-Core Processor            ` | `IntelLLVM 20250101` | `max` | `L20` | 127 | `256` | `0x947a906c4a24d632` | 71 | CPU fallback | pass |
| SYCL | `AMD Ryzen 9 3900X 12-Core Processor            ` | `IntelLLVM 20250101` | `max` | `L20` | 256 | `auto` | `0x1e548a70f2c5af61` | 81 | CPU fallback | pass |
| SYCL | `AMD Ryzen 9 3900X 12-Core Processor            ` | `IntelLLVM 20250101` | `max` | `L20` | 256 | `64` | `0x1e548a70f2c5af61` | 73 | CPU fallback | pass |
| SYCL | `AMD Ryzen 9 3900X 12-Core Processor            ` | `IntelLLVM 20250101` | `max` | `L20` | 256 | `256` | `0x1e548a70f2c5af61` | 62 | CPU fallback | pass |
| SYCL | `AMD Ryzen 9 3900X 12-Core Processor            ` | `IntelLLVM 20250101` | `max` | `L20` | 1009 | `auto` | `0xb1525e861e7b6729` | 74 | CPU fallback | pass |
| SYCL | `AMD Ryzen 9 3900X 12-Core Processor            ` | `IntelLLVM 20250101` | `max` | `L20` | 1009 | `64` | `0xb1525e861e7b6729` | 61 | CPU fallback | pass |
| SYCL | `AMD Ryzen 9 3900X 12-Core Processor            ` | `IntelLLVM 20250101` | `max` | `L20` | 1009 | `256` | `0xb1525e861e7b6729` | 69 | CPU fallback | pass |
| SYCL | `AMD Ryzen 9 3900X 12-Core Processor            ` | `IntelLLVM 20250101` | `max` | `L20` | 65536 | `auto` | `0xf968fdb3cd8567f4` | 410 | CPU fallback | pass |
| SYCL | `AMD Ryzen 9 3900X 12-Core Processor            ` | `IntelLLVM 20250101` | `max` | `L20` | 65536 | `64` | `0xf968fdb3cd8567f4` | 459 | CPU fallback | pass |
| SYCL | `AMD Ryzen 9 3900X 12-Core Processor            ` | `IntelLLVM 20250101` | `max` | `L20` | 65536 | `256` | `0xf968fdb3cd8567f4` | 552 | CPU fallback | pass |
| SYCL | `AMD Ryzen 9 3900X 12-Core Processor            ` | `IntelLLVM 20250101` | `neg` | `L40` | 0 | `auto` | `0x14650fb0739d0383` | 60 | CPU fallback | pass |
| SYCL | `AMD Ryzen 9 3900X 12-Core Processor            ` | `IntelLLVM 20250101` | `neg` | `L40` | 0 | `64` | `0x14650fb0739d0383` | 54 | CPU fallback | pass |
| SYCL | `AMD Ryzen 9 3900X 12-Core Processor            ` | `IntelLLVM 20250101` | `neg` | `L40` | 0 | `256` | `0x14650fb0739d0383` | 30 | CPU fallback | pass |
| SYCL | `AMD Ryzen 9 3900X 12-Core Processor            ` | `IntelLLVM 20250101` | `neg` | `L40` | 1 | `auto` | `0x28b72b238faffffe` | 34 | CPU fallback | pass |
| SYCL | `AMD Ryzen 9 3900X 12-Core Processor            ` | `IntelLLVM 20250101` | `neg` | `L40` | 1 | `64` | `0x28b72b238faffffe` | 53 | CPU fallback | pass |
| SYCL | `AMD Ryzen 9 3900X 12-Core Processor            ` | `IntelLLVM 20250101` | `neg` | `L40` | 1 | `256` | `0x28b72b238faffffe` | 54 | CPU fallback | pass |
| SYCL | `AMD Ryzen 9 3900X 12-Core Processor            ` | `IntelLLVM 20250101` | `neg` | `L40` | 127 | `auto` | `0x536cd8941fcdb362` | 54 | CPU fallback | pass |
| SYCL | `AMD Ryzen 9 3900X 12-Core Processor            ` | `IntelLLVM 20250101` | `neg` | `L40` | 127 | `64` | `0x536cd8941fcdb362` | 43 | CPU fallback | pass |
| SYCL | `AMD Ryzen 9 3900X 12-Core Processor            ` | `IntelLLVM 20250101` | `neg` | `L40` | 127 | `256` | `0x536cd8941fcdb362` | 50 | CPU fallback | pass |
| SYCL | `AMD Ryzen 9 3900X 12-Core Processor            ` | `IntelLLVM 20250101` | `neg` | `L40` | 256 | `auto` | `0x217234c346568952` | 55 | CPU fallback | pass |
| SYCL | `AMD Ryzen 9 3900X 12-Core Processor            ` | `IntelLLVM 20250101` | `neg` | `L40` | 256 | `64` | `0x217234c346568952` | 38 | CPU fallback | pass |
| SYCL | `AMD Ryzen 9 3900X 12-Core Processor            ` | `IntelLLVM 20250101` | `neg` | `L40` | 256 | `256` | `0x217234c346568952` | 41 | CPU fallback | pass |
| SYCL | `AMD Ryzen 9 3900X 12-Core Processor            ` | `IntelLLVM 20250101` | `neg` | `L40` | 1009 | `auto` | `0xd03597cfb107c6b0` | 143 | CPU fallback | pass |
| SYCL | `AMD Ryzen 9 3900X 12-Core Processor            ` | `IntelLLVM 20250101` | `neg` | `L40` | 1009 | `64` | `0xd03597cfb107c6b0` | 54 | CPU fallback | pass |
| SYCL | `AMD Ryzen 9 3900X 12-Core Processor            ` | `IntelLLVM 20250101` | `neg` | `L40` | 1009 | `256` | `0xd03597cfb107c6b0` | 59 | CPU fallback | pass |
| SYCL | `AMD Ryzen 9 3900X 12-Core Processor            ` | `IntelLLVM 20250101` | `neg` | `L40` | 65536 | `auto` | `0x94677bc36ca212a3` | 409 | CPU fallback | pass |
| SYCL | `AMD Ryzen 9 3900X 12-Core Processor            ` | `IntelLLVM 20250101` | `neg` | `L40` | 65536 | `64` | `0x94677bc36ca212a3` | 906 | CPU fallback | pass |
| SYCL | `AMD Ryzen 9 3900X 12-Core Processor            ` | `IntelLLVM 20250101` | `neg` | `L40` | 65536 | `256` | `0x94677bc36ca212a3` | 358 | CPU fallback | pass |
| SYCL | `AMD Ryzen 9 3900X 12-Core Processor            ` | `IntelLLVM 20250101` | `add` | `L40` | 0 | `auto` | `0x14650fb0739d0383` | 40 | CPU fallback | pass |
| SYCL | `AMD Ryzen 9 3900X 12-Core Processor            ` | `IntelLLVM 20250101` | `add` | `L40` | 0 | `64` | `0x14650fb0739d0383` | 35 | CPU fallback | pass |
| SYCL | `AMD Ryzen 9 3900X 12-Core Processor            ` | `IntelLLVM 20250101` | `add` | `L40` | 0 | `256` | `0x14650fb0739d0383` | 25 | CPU fallback | pass |
| SYCL | `AMD Ryzen 9 3900X 12-Core Processor            ` | `IntelLLVM 20250101` | `add` | `L40` | 1 | `auto` | `0x619d92eb9c301301` | 44 | CPU fallback | pass |
| SYCL | `AMD Ryzen 9 3900X 12-Core Processor            ` | `IntelLLVM 20250101` | `add` | `L40` | 1 | `64` | `0x619d92eb9c301301` | 38 | CPU fallback | pass |
| SYCL | `AMD Ryzen 9 3900X 12-Core Processor            ` | `IntelLLVM 20250101` | `add` | `L40` | 1 | `256` | `0x619d92eb9c301301` | 31 | CPU fallback | pass |
| SYCL | `AMD Ryzen 9 3900X 12-Core Processor            ` | `IntelLLVM 20250101` | `add` | `L40` | 127 | `auto` | `0x24e145dd9c798721` | 61 | CPU fallback | pass |
| SYCL | `AMD Ryzen 9 3900X 12-Core Processor            ` | `IntelLLVM 20250101` | `add` | `L40` | 127 | `64` | `0x24e145dd9c798721` | 67 | CPU fallback | pass |
| SYCL | `AMD Ryzen 9 3900X 12-Core Processor            ` | `IntelLLVM 20250101` | `add` | `L40` | 127 | `256` | `0x24e145dd9c798721` | 71 | CPU fallback | pass |
| SYCL | `AMD Ryzen 9 3900X 12-Core Processor            ` | `IntelLLVM 20250101` | `add` | `L40` | 256 | `auto` | `0x7029378b693f1115` | 51 | CPU fallback | pass |
| SYCL | `AMD Ryzen 9 3900X 12-Core Processor            ` | `IntelLLVM 20250101` | `add` | `L40` | 256 | `64` | `0x7029378b693f1115` | 54 | CPU fallback | pass |
| SYCL | `AMD Ryzen 9 3900X 12-Core Processor            ` | `IntelLLVM 20250101` | `add` | `L40` | 256 | `256` | `0x7029378b693f1115` | 103 | CPU fallback | pass |
| SYCL | `AMD Ryzen 9 3900X 12-Core Processor            ` | `IntelLLVM 20250101` | `add` | `L40` | 1009 | `auto` | `0xa8cb9a73026ee030` | 94 | CPU fallback | pass |
| SYCL | `AMD Ryzen 9 3900X 12-Core Processor            ` | `IntelLLVM 20250101` | `add` | `L40` | 1009 | `64` | `0xa8cb9a73026ee030` | 80 | CPU fallback | pass |
| SYCL | `AMD Ryzen 9 3900X 12-Core Processor            ` | `IntelLLVM 20250101` | `add` | `L40` | 1009 | `256` | `0xa8cb9a73026ee030` | 137 | CPU fallback | pass |
| SYCL | `AMD Ryzen 9 3900X 12-Core Processor            ` | `IntelLLVM 20250101` | `add` | `L40` | 65536 | `auto` | `0xdc3b47db5456d32c` | 1158 | CPU fallback | pass |
| SYCL | `AMD Ryzen 9 3900X 12-Core Processor            ` | `IntelLLVM 20250101` | `add` | `L40` | 65536 | `64` | `0xdc3b47db5456d32c` | 1339 | CPU fallback | pass |
| SYCL | `AMD Ryzen 9 3900X 12-Core Processor            ` | `IntelLLVM 20250101` | `add` | `L40` | 65536 | `256` | `0xdc3b47db5456d32c` | 1203 | CPU fallback | pass |
| SYCL | `AMD Ryzen 9 3900X 12-Core Processor            ` | `IntelLLVM 20250101` | `sub` | `L40` | 0 | `auto` | `0x14650fb0739d0383` | 59 | CPU fallback | pass |
| SYCL | `AMD Ryzen 9 3900X 12-Core Processor            ` | `IntelLLVM 20250101` | `sub` | `L40` | 0 | `64` | `0x14650fb0739d0383` | 39 | CPU fallback | pass |
| SYCL | `AMD Ryzen 9 3900X 12-Core Processor            ` | `IntelLLVM 20250101` | `sub` | `L40` | 0 | `256` | `0x14650fb0739d0383` | 50 | CPU fallback | pass |
| SYCL | `AMD Ryzen 9 3900X 12-Core Processor            ` | `IntelLLVM 20250101` | `sub` | `L40` | 1 | `auto` | `0x17f26ea9b40b837b` | 45 | CPU fallback | pass |
| SYCL | `AMD Ryzen 9 3900X 12-Core Processor            ` | `IntelLLVM 20250101` | `sub` | `L40` | 1 | `64` | `0x17f26ea9b40b837b` | 49 | CPU fallback | pass |
| SYCL | `AMD Ryzen 9 3900X 12-Core Processor            ` | `IntelLLVM 20250101` | `sub` | `L40` | 1 | `256` | `0x17f26ea9b40b837b` | 54 | CPU fallback | pass |
| SYCL | `AMD Ryzen 9 3900X 12-Core Processor            ` | `IntelLLVM 20250101` | `sub` | `L40` | 127 | `auto` | `0xb5782575fb7bc89d` | 58 | CPU fallback | pass |
| SYCL | `AMD Ryzen 9 3900X 12-Core Processor            ` | `IntelLLVM 20250101` | `sub` | `L40` | 127 | `64` | `0xb5782575fb7bc89d` | 63 | CPU fallback | pass |
| SYCL | `AMD Ryzen 9 3900X 12-Core Processor            ` | `IntelLLVM 20250101` | `sub` | `L40` | 127 | `256` | `0xb5782575fb7bc89d` | 77 | CPU fallback | pass |
| SYCL | `AMD Ryzen 9 3900X 12-Core Processor            ` | `IntelLLVM 20250101` | `sub` | `L40` | 256 | `auto` | `0xe5e7f8428591f200` | 75 | CPU fallback | pass |
| SYCL | `AMD Ryzen 9 3900X 12-Core Processor            ` | `IntelLLVM 20250101` | `sub` | `L40` | 256 | `64` | `0xe5e7f8428591f200` | 68 | CPU fallback | pass |
| SYCL | `AMD Ryzen 9 3900X 12-Core Processor            ` | `IntelLLVM 20250101` | `sub` | `L40` | 256 | `256` | `0xe5e7f8428591f200` | 115 | CPU fallback | pass |
| SYCL | `AMD Ryzen 9 3900X 12-Core Processor            ` | `IntelLLVM 20250101` | `sub` | `L40` | 1009 | `auto` | `0xed1685b346449d25` | 88 | CPU fallback | pass |
| SYCL | `AMD Ryzen 9 3900X 12-Core Processor            ` | `IntelLLVM 20250101` | `sub` | `L40` | 1009 | `64` | `0xed1685b346449d25` | 87 | CPU fallback | pass |
| SYCL | `AMD Ryzen 9 3900X 12-Core Processor            ` | `IntelLLVM 20250101` | `sub` | `L40` | 1009 | `256` | `0xed1685b346449d25` | 126 | CPU fallback | pass |
| SYCL | `AMD Ryzen 9 3900X 12-Core Processor            ` | `IntelLLVM 20250101` | `sub` | `L40` | 65536 | `auto` | `0x35974fd5cc59c144` | 1135 | CPU fallback | pass |
| SYCL | `AMD Ryzen 9 3900X 12-Core Processor            ` | `IntelLLVM 20250101` | `sub` | `L40` | 65536 | `64` | `0x35974fd5cc59c144` | 1098 | CPU fallback | pass |
| SYCL | `AMD Ryzen 9 3900X 12-Core Processor            ` | `IntelLLVM 20250101` | `sub` | `L40` | 65536 | `256` | `0x35974fd5cc59c144` | 1118 | CPU fallback | pass |
| SYCL | `AMD Ryzen 9 3900X 12-Core Processor            ` | `IntelLLVM 20250101` | `compare` | `L40` | 0 | `auto` | `0x14650fb0739d0383` | 49 | CPU fallback | pass |
| SYCL | `AMD Ryzen 9 3900X 12-Core Processor            ` | `IntelLLVM 20250101` | `compare` | `L40` | 0 | `64` | `0x14650fb0739d0383` | 81 | CPU fallback | pass |
| SYCL | `AMD Ryzen 9 3900X 12-Core Processor            ` | `IntelLLVM 20250101` | `compare` | `L40` | 0 | `256` | `0x14650fb0739d0383` | 27 | CPU fallback | pass |
| SYCL | `AMD Ryzen 9 3900X 12-Core Processor            ` | `IntelLLVM 20250101` | `compare` | `L40` | 1 | `auto` | `0x44bd2bd473ccf799` | 72 | CPU fallback | pass |
| SYCL | `AMD Ryzen 9 3900X 12-Core Processor            ` | `IntelLLVM 20250101` | `compare` | `L40` | 1 | `64` | `0x44bd2bd473ccf799` | 60 | CPU fallback | pass |
| SYCL | `AMD Ryzen 9 3900X 12-Core Processor            ` | `IntelLLVM 20250101` | `compare` | `L40` | 1 | `256` | `0x44bd2bd473ccf799` | 42 | CPU fallback | pass |
| SYCL | `AMD Ryzen 9 3900X 12-Core Processor            ` | `IntelLLVM 20250101` | `compare` | `L40` | 127 | `auto` | `0xcd24fa151b7cfae3` | 56 | CPU fallback | pass |
| SYCL | `AMD Ryzen 9 3900X 12-Core Processor            ` | `IntelLLVM 20250101` | `compare` | `L40` | 127 | `64` | `0xcd24fa151b7cfae3` | 49 | CPU fallback | pass |
| SYCL | `AMD Ryzen 9 3900X 12-Core Processor            ` | `IntelLLVM 20250101` | `compare` | `L40` | 127 | `256` | `0xcd24fa151b7cfae3` | 41 | CPU fallback | pass |
| SYCL | `AMD Ryzen 9 3900X 12-Core Processor            ` | `IntelLLVM 20250101` | `compare` | `L40` | 256 | `auto` | `0x7c65925d6b98aa6b` | 62 | CPU fallback | pass |
| SYCL | `AMD Ryzen 9 3900X 12-Core Processor            ` | `IntelLLVM 20250101` | `compare` | `L40` | 256 | `64` | `0x7c65925d6b98aa6b` | 53 | CPU fallback | pass |
| SYCL | `AMD Ryzen 9 3900X 12-Core Processor            ` | `IntelLLVM 20250101` | `compare` | `L40` | 256 | `256` | `0x7c65925d6b98aa6b` | 53 | CPU fallback | pass |
| SYCL | `AMD Ryzen 9 3900X 12-Core Processor            ` | `IntelLLVM 20250101` | `compare` | `L40` | 1009 | `auto` | `0xe26216ddcf6bd461` | 68 | CPU fallback | pass |
| SYCL | `AMD Ryzen 9 3900X 12-Core Processor            ` | `IntelLLVM 20250101` | `compare` | `L40` | 1009 | `64` | `0xe26216ddcf6bd461` | 69 | CPU fallback | pass |
| SYCL | `AMD Ryzen 9 3900X 12-Core Processor            ` | `IntelLLVM 20250101` | `compare` | `L40` | 1009 | `256` | `0xe26216ddcf6bd461` | 60 | CPU fallback | pass |
| SYCL | `AMD Ryzen 9 3900X 12-Core Processor            ` | `IntelLLVM 20250101` | `compare` | `L40` | 65536 | `auto` | `0xbbef362dcfd20513` | 335 | CPU fallback | pass |
| SYCL | `AMD Ryzen 9 3900X 12-Core Processor            ` | `IntelLLVM 20250101` | `compare` | `L40` | 65536 | `64` | `0xbbef362dcfd20513` | 496 | CPU fallback | pass |
| SYCL | `AMD Ryzen 9 3900X 12-Core Processor            ` | `IntelLLVM 20250101` | `compare` | `L40` | 65536 | `256` | `0xbbef362dcfd20513` | 327 | CPU fallback | pass |
| SYCL | `AMD Ryzen 9 3900X 12-Core Processor            ` | `IntelLLVM 20250101` | `min` | `L40` | 0 | `auto` | `0x14650fb0739d0383` | 48 | CPU fallback | pass |
| SYCL | `AMD Ryzen 9 3900X 12-Core Processor            ` | `IntelLLVM 20250101` | `min` | `L40` | 0 | `64` | `0x14650fb0739d0383` | 48 | CPU fallback | pass |
| SYCL | `AMD Ryzen 9 3900X 12-Core Processor            ` | `IntelLLVM 20250101` | `min` | `L40` | 0 | `256` | `0x14650fb0739d0383` | 27 | CPU fallback | pass |
| SYCL | `AMD Ryzen 9 3900X 12-Core Processor            ` | `IntelLLVM 20250101` | `min` | `L40` | 1 | `auto` | `0x7c9b1a1c9148ac2c` | 43 | CPU fallback | pass |
| SYCL | `AMD Ryzen 9 3900X 12-Core Processor            ` | `IntelLLVM 20250101` | `min` | `L40` | 1 | `64` | `0x7c9b1a1c9148ac2c` | 37 | CPU fallback | pass |
| SYCL | `AMD Ryzen 9 3900X 12-Core Processor            ` | `IntelLLVM 20250101` | `min` | `L40` | 1 | `256` | `0x7c9b1a1c9148ac2c` | 33 | CPU fallback | pass |
| SYCL | `AMD Ryzen 9 3900X 12-Core Processor            ` | `IntelLLVM 20250101` | `min` | `L40` | 127 | `auto` | `0x376811760c0e7159` | 39 | CPU fallback | pass |
| SYCL | `AMD Ryzen 9 3900X 12-Core Processor            ` | `IntelLLVM 20250101` | `min` | `L40` | 127 | `64` | `0x376811760c0e7159` | 39 | CPU fallback | pass |
| SYCL | `AMD Ryzen 9 3900X 12-Core Processor            ` | `IntelLLVM 20250101` | `min` | `L40` | 127 | `256` | `0x376811760c0e7159` | 34 | CPU fallback | pass |
| SYCL | `AMD Ryzen 9 3900X 12-Core Processor            ` | `IntelLLVM 20250101` | `min` | `L40` | 256 | `auto` | `0xa3835b24a898d8ab` | 54 | CPU fallback | pass |
| SYCL | `AMD Ryzen 9 3900X 12-Core Processor            ` | `IntelLLVM 20250101` | `min` | `L40` | 256 | `64` | `0xa3835b24a898d8ab` | 45 | CPU fallback | pass |
| SYCL | `AMD Ryzen 9 3900X 12-Core Processor            ` | `IntelLLVM 20250101` | `min` | `L40` | 256 | `256` | `0xa3835b24a898d8ab` | 63 | CPU fallback | pass |
| SYCL | `AMD Ryzen 9 3900X 12-Core Processor            ` | `IntelLLVM 20250101` | `min` | `L40` | 1009 | `auto` | `0x7530471bdefff5ba` | 80 | CPU fallback | pass |
| SYCL | `AMD Ryzen 9 3900X 12-Core Processor            ` | `IntelLLVM 20250101` | `min` | `L40` | 1009 | `64` | `0x7530471bdefff5ba` | 80 | CPU fallback | pass |
| SYCL | `AMD Ryzen 9 3900X 12-Core Processor            ` | `IntelLLVM 20250101` | `min` | `L40` | 1009 | `256` | `0x7530471bdefff5ba` | 135 | CPU fallback | pass |
| SYCL | `AMD Ryzen 9 3900X 12-Core Processor            ` | `IntelLLVM 20250101` | `min` | `L40` | 65536 | `auto` | `0x5cad45e7204fff1a` | 516 | CPU fallback | pass |
| SYCL | `AMD Ryzen 9 3900X 12-Core Processor            ` | `IntelLLVM 20250101` | `min` | `L40` | 65536 | `64` | `0x5cad45e7204fff1a` | 563 | CPU fallback | pass |
| SYCL | `AMD Ryzen 9 3900X 12-Core Processor            ` | `IntelLLVM 20250101` | `min` | `L40` | 65536 | `256` | `0x5cad45e7204fff1a` | 519 | CPU fallback | pass |
| SYCL | `AMD Ryzen 9 3900X 12-Core Processor            ` | `IntelLLVM 20250101` | `max` | `L40` | 0 | `auto` | `0x14650fb0739d0383` | 46 | CPU fallback | pass |
| SYCL | `AMD Ryzen 9 3900X 12-Core Processor            ` | `IntelLLVM 20250101` | `max` | `L40` | 0 | `64` | `0x14650fb0739d0383` | 34 | CPU fallback | pass |
| SYCL | `AMD Ryzen 9 3900X 12-Core Processor            ` | `IntelLLVM 20250101` | `max` | `L40` | 0 | `256` | `0x14650fb0739d0383` | 30 | CPU fallback | pass |
| SYCL | `AMD Ryzen 9 3900X 12-Core Processor            ` | `IntelLLVM 20250101` | `max` | `L40` | 1 | `auto` | `0x77a5e1bbd2363d54` | 42 | CPU fallback | pass |
| SYCL | `AMD Ryzen 9 3900X 12-Core Processor            ` | `IntelLLVM 20250101` | `max` | `L40` | 1 | `64` | `0x77a5e1bbd2363d54` | 46 | CPU fallback | pass |
| SYCL | `AMD Ryzen 9 3900X 12-Core Processor            ` | `IntelLLVM 20250101` | `max` | `L40` | 1 | `256` | `0x77a5e1bbd2363d54` | 36 | CPU fallback | pass |
| SYCL | `AMD Ryzen 9 3900X 12-Core Processor            ` | `IntelLLVM 20250101` | `max` | `L40` | 127 | `auto` | `0x5acd9a687f024840` | 46 | CPU fallback | pass |
| SYCL | `AMD Ryzen 9 3900X 12-Core Processor            ` | `IntelLLVM 20250101` | `max` | `L40` | 127 | `64` | `0x5acd9a687f024840` | 48 | CPU fallback | pass |
| SYCL | `AMD Ryzen 9 3900X 12-Core Processor            ` | `IntelLLVM 20250101` | `max` | `L40` | 127 | `256` | `0x5acd9a687f024840` | 78 | CPU fallback | pass |
| SYCL | `AMD Ryzen 9 3900X 12-Core Processor            ` | `IntelLLVM 20250101` | `max` | `L40` | 256 | `auto` | `0x47a97f771c124024` | 78 | CPU fallback | pass |
| SYCL | `AMD Ryzen 9 3900X 12-Core Processor            ` | `IntelLLVM 20250101` | `max` | `L40` | 256 | `64` | `0x47a97f771c124024` | 56 | CPU fallback | pass |
| SYCL | `AMD Ryzen 9 3900X 12-Core Processor            ` | `IntelLLVM 20250101` | `max` | `L40` | 256 | `256` | `0x47a97f771c124024` | 83 | CPU fallback | pass |
| SYCL | `AMD Ryzen 9 3900X 12-Core Processor            ` | `IntelLLVM 20250101` | `max` | `L40` | 1009 | `auto` | `0xa604d757794af19f` | 73 | CPU fallback | pass |
| SYCL | `AMD Ryzen 9 3900X 12-Core Processor            ` | `IntelLLVM 20250101` | `max` | `L40` | 1009 | `64` | `0xa604d757794af19f` | 73 | CPU fallback | pass |
| SYCL | `AMD Ryzen 9 3900X 12-Core Processor            ` | `IntelLLVM 20250101` | `max` | `L40` | 1009 | `256` | `0xa604d757794af19f` | 90 | CPU fallback | pass |
| SYCL | `AMD Ryzen 9 3900X 12-Core Processor            ` | `IntelLLVM 20250101` | `max` | `L40` | 65536 | `auto` | `0x78622c0f94126423` | 675 | CPU fallback | pass |
| SYCL | `AMD Ryzen 9 3900X 12-Core Processor            ` | `IntelLLVM 20250101` | `max` | `L40` | 65536 | `64` | `0x78622c0f94126423` | 549 | CPU fallback | pass |
| SYCL | `AMD Ryzen 9 3900X 12-Core Processor            ` | `IntelLLVM 20250101` | `max` | `L40` | 65536 | `256` | `0x78622c0f94126423` | 572 | CPU fallback | pass |
| SYCL | `AMD Ryzen 9 3900X 12-Core Processor            ` | `IntelLLVM 20250101` | `neg` | `L50` | 0 | `auto` | `0x14650fb0739d0383` | 33 | CPU fallback | pass |
| SYCL | `AMD Ryzen 9 3900X 12-Core Processor            ` | `IntelLLVM 20250101` | `neg` | `L50` | 0 | `64` | `0x14650fb0739d0383` | 47 | CPU fallback | pass |
| SYCL | `AMD Ryzen 9 3900X 12-Core Processor            ` | `IntelLLVM 20250101` | `neg` | `L50` | 0 | `256` | `0x14650fb0739d0383` | 36 | CPU fallback | pass |
| SYCL | `AMD Ryzen 9 3900X 12-Core Processor            ` | `IntelLLVM 20250101` | `neg` | `L50` | 1 | `auto` | `0x5ab7de04d60aedfd` | 38 | CPU fallback | pass |
| SYCL | `AMD Ryzen 9 3900X 12-Core Processor            ` | `IntelLLVM 20250101` | `neg` | `L50` | 1 | `64` | `0x5ab7de04d60aedfd` | 38 | CPU fallback | pass |
| SYCL | `AMD Ryzen 9 3900X 12-Core Processor            ` | `IntelLLVM 20250101` | `neg` | `L50` | 1 | `256` | `0x5ab7de04d60aedfd` | 42 | CPU fallback | pass |
| SYCL | `AMD Ryzen 9 3900X 12-Core Processor            ` | `IntelLLVM 20250101` | `neg` | `L50` | 127 | `auto` | `0x0846eb6b90bebf0c` | 51 | CPU fallback | pass |
| SYCL | `AMD Ryzen 9 3900X 12-Core Processor            ` | `IntelLLVM 20250101` | `neg` | `L50` | 127 | `64` | `0x0846eb6b90bebf0c` | 36 | CPU fallback | pass |
| SYCL | `AMD Ryzen 9 3900X 12-Core Processor            ` | `IntelLLVM 20250101` | `neg` | `L50` | 127 | `256` | `0x0846eb6b90bebf0c` | 52 | CPU fallback | pass |
| SYCL | `AMD Ryzen 9 3900X 12-Core Processor            ` | `IntelLLVM 20250101` | `neg` | `L50` | 256 | `auto` | `0x36c4fbd0f4501302` | 49 | CPU fallback | pass |
| SYCL | `AMD Ryzen 9 3900X 12-Core Processor            ` | `IntelLLVM 20250101` | `neg` | `L50` | 256 | `64` | `0x36c4fbd0f4501302` | 43 | CPU fallback | pass |
| SYCL | `AMD Ryzen 9 3900X 12-Core Processor            ` | `IntelLLVM 20250101` | `neg` | `L50` | 256 | `256` | `0x36c4fbd0f4501302` | 54 | CPU fallback | pass |
| SYCL | `AMD Ryzen 9 3900X 12-Core Processor            ` | `IntelLLVM 20250101` | `neg` | `L50` | 1009 | `auto` | `0x17bb63dfd6ef9a79` | 55 | CPU fallback | pass |
| SYCL | `AMD Ryzen 9 3900X 12-Core Processor            ` | `IntelLLVM 20250101` | `neg` | `L50` | 1009 | `64` | `0x17bb63dfd6ef9a79` | 51 | CPU fallback | pass |
| SYCL | `AMD Ryzen 9 3900X 12-Core Processor            ` | `IntelLLVM 20250101` | `neg` | `L50` | 1009 | `256` | `0x17bb63dfd6ef9a79` | 55 | CPU fallback | pass |
| SYCL | `AMD Ryzen 9 3900X 12-Core Processor            ` | `IntelLLVM 20250101` | `neg` | `L50` | 65536 | `auto` | `0xfc8113c09d4fec10` | 369 | CPU fallback | pass |
| SYCL | `AMD Ryzen 9 3900X 12-Core Processor            ` | `IntelLLVM 20250101` | `neg` | `L50` | 65536 | `64` | `0xfc8113c09d4fec10` | 406 | CPU fallback | pass |
| SYCL | `AMD Ryzen 9 3900X 12-Core Processor            ` | `IntelLLVM 20250101` | `neg` | `L50` | 65536 | `256` | `0xfc8113c09d4fec10` | 396 | CPU fallback | pass |
| SYCL | `AMD Ryzen 9 3900X 12-Core Processor            ` | `IntelLLVM 20250101` | `add` | `L50` | 0 | `auto` | `0x14650fb0739d0383` | 25 | CPU fallback | pass |
| SYCL | `AMD Ryzen 9 3900X 12-Core Processor            ` | `IntelLLVM 20250101` | `add` | `L50` | 0 | `64` | `0x14650fb0739d0383` | 23 | CPU fallback | pass |
| SYCL | `AMD Ryzen 9 3900X 12-Core Processor            ` | `IntelLLVM 20250101` | `add` | `L50` | 0 | `256` | `0x14650fb0739d0383` | 34 | CPU fallback | pass |
| SYCL | `AMD Ryzen 9 3900X 12-Core Processor            ` | `IntelLLVM 20250101` | `add` | `L50` | 1 | `auto` | `0x58c0405fd60eda83` | 61 | CPU fallback | pass |
| SYCL | `AMD Ryzen 9 3900X 12-Core Processor            ` | `IntelLLVM 20250101` | `add` | `L50` | 1 | `64` | `0x58c0405fd60eda83` | 46 | CPU fallback | pass |
| SYCL | `AMD Ryzen 9 3900X 12-Core Processor            ` | `IntelLLVM 20250101` | `add` | `L50` | 1 | `256` | `0x58c0405fd60eda83` | 37 | CPU fallback | pass |
| SYCL | `AMD Ryzen 9 3900X 12-Core Processor            ` | `IntelLLVM 20250101` | `add` | `L50` | 127 | `auto` | `0x912619c637f64a1e` | 52 | CPU fallback | pass |
| SYCL | `AMD Ryzen 9 3900X 12-Core Processor            ` | `IntelLLVM 20250101` | `add` | `L50` | 127 | `64` | `0x912619c637f64a1e` | 63 | CPU fallback | pass |
| SYCL | `AMD Ryzen 9 3900X 12-Core Processor            ` | `IntelLLVM 20250101` | `add` | `L50` | 127 | `256` | `0x912619c637f64a1e` | 78 | CPU fallback | pass |
| SYCL | `AMD Ryzen 9 3900X 12-Core Processor            ` | `IntelLLVM 20250101` | `add` | `L50` | 256 | `auto` | `0x0260fc945bf25e68` | 51 | CPU fallback | pass |
| SYCL | `AMD Ryzen 9 3900X 12-Core Processor            ` | `IntelLLVM 20250101` | `add` | `L50` | 256 | `64` | `0x0260fc945bf25e68` | 79 | CPU fallback | pass |
| SYCL | `AMD Ryzen 9 3900X 12-Core Processor            ` | `IntelLLVM 20250101` | `add` | `L50` | 256 | `256` | `0x0260fc945bf25e68` | 130 | CPU fallback | pass |
| SYCL | `AMD Ryzen 9 3900X 12-Core Processor            ` | `IntelLLVM 20250101` | `add` | `L50` | 1009 | `auto` | `0xa76636eb82eee928` | 91 | CPU fallback | pass |
| SYCL | `AMD Ryzen 9 3900X 12-Core Processor            ` | `IntelLLVM 20250101` | `add` | `L50` | 1009 | `64` | `0xa76636eb82eee928` | 92 | CPU fallback | pass |
| SYCL | `AMD Ryzen 9 3900X 12-Core Processor            ` | `IntelLLVM 20250101` | `add` | `L50` | 1009 | `256` | `0xa76636eb82eee928` | 145 | CPU fallback | pass |
| SYCL | `AMD Ryzen 9 3900X 12-Core Processor            ` | `IntelLLVM 20250101` | `add` | `L50` | 65536 | `auto` | `0xbfb7d4442d8342b1` | 1462 | CPU fallback | pass |
| SYCL | `AMD Ryzen 9 3900X 12-Core Processor            ` | `IntelLLVM 20250101` | `add` | `L50` | 65536 | `64` | `0xbfb7d4442d8342b1` | 1353 | CPU fallback | pass |
| SYCL | `AMD Ryzen 9 3900X 12-Core Processor            ` | `IntelLLVM 20250101` | `add` | `L50` | 65536 | `256` | `0xbfb7d4442d8342b1` | 1474 | CPU fallback | pass |
| SYCL | `AMD Ryzen 9 3900X 12-Core Processor            ` | `IntelLLVM 20250101` | `sub` | `L50` | 0 | `auto` | `0x14650fb0739d0383` | 42 | CPU fallback | pass |
| SYCL | `AMD Ryzen 9 3900X 12-Core Processor            ` | `IntelLLVM 20250101` | `sub` | `L50` | 0 | `64` | `0x14650fb0739d0383` | 31 | CPU fallback | pass |
| SYCL | `AMD Ryzen 9 3900X 12-Core Processor            ` | `IntelLLVM 20250101` | `sub` | `L50` | 0 | `256` | `0x14650fb0739d0383` | 30 | CPU fallback | pass |
| SYCL | `AMD Ryzen 9 3900X 12-Core Processor            ` | `IntelLLVM 20250101` | `sub` | `L50` | 1 | `auto` | `0xa84bbcc31817a168` | 75 | CPU fallback | pass |
| SYCL | `AMD Ryzen 9 3900X 12-Core Processor            ` | `IntelLLVM 20250101` | `sub` | `L50` | 1 | `64` | `0xa84bbcc31817a168` | 63 | CPU fallback | pass |
| SYCL | `AMD Ryzen 9 3900X 12-Core Processor            ` | `IntelLLVM 20250101` | `sub` | `L50` | 1 | `256` | `0xa84bbcc31817a168` | 61 | CPU fallback | pass |
| SYCL | `AMD Ryzen 9 3900X 12-Core Processor            ` | `IntelLLVM 20250101` | `sub` | `L50` | 127 | `auto` | `0x6bf420a74370c412` | 76 | CPU fallback | pass |
| SYCL | `AMD Ryzen 9 3900X 12-Core Processor            ` | `IntelLLVM 20250101` | `sub` | `L50` | 127 | `64` | `0x6bf420a74370c412` | 84 | CPU fallback | pass |
| SYCL | `AMD Ryzen 9 3900X 12-Core Processor            ` | `IntelLLVM 20250101` | `sub` | `L50` | 127 | `256` | `0x6bf420a74370c412` | 91 | CPU fallback | pass |
| SYCL | `AMD Ryzen 9 3900X 12-Core Processor            ` | `IntelLLVM 20250101` | `sub` | `L50` | 256 | `auto` | `0x5ed05f4de82ab901` | 67 | CPU fallback | pass |
| SYCL | `AMD Ryzen 9 3900X 12-Core Processor            ` | `IntelLLVM 20250101` | `sub` | `L50` | 256 | `64` | `0x5ed05f4de82ab901` | 81 | CPU fallback | pass |
| SYCL | `AMD Ryzen 9 3900X 12-Core Processor            ` | `IntelLLVM 20250101` | `sub` | `L50` | 256 | `256` | `0x5ed05f4de82ab901` | 131 | CPU fallback | pass |
| SYCL | `AMD Ryzen 9 3900X 12-Core Processor            ` | `IntelLLVM 20250101` | `sub` | `L50` | 1009 | `auto` | `0x677590ab4a9a8ea9` | 108 | CPU fallback | pass |
| SYCL | `AMD Ryzen 9 3900X 12-Core Processor            ` | `IntelLLVM 20250101` | `sub` | `L50` | 1009 | `64` | `0x677590ab4a9a8ea9` | 85 | CPU fallback | pass |
| SYCL | `AMD Ryzen 9 3900X 12-Core Processor            ` | `IntelLLVM 20250101` | `sub` | `L50` | 1009 | `256` | `0x677590ab4a9a8ea9` | 139 | CPU fallback | pass |
| SYCL | `AMD Ryzen 9 3900X 12-Core Processor            ` | `IntelLLVM 20250101` | `sub` | `L50` | 65536 | `auto` | `0x25c41d7939451262` | 1266 | CPU fallback | pass |
| SYCL | `AMD Ryzen 9 3900X 12-Core Processor            ` | `IntelLLVM 20250101` | `sub` | `L50` | 65536 | `64` | `0x25c41d7939451262` | 4341 | CPU fallback | pass |
| SYCL | `AMD Ryzen 9 3900X 12-Core Processor            ` | `IntelLLVM 20250101` | `sub` | `L50` | 65536 | `256` | `0x25c41d7939451262` | 2648 | CPU fallback | pass |
| SYCL | `AMD Ryzen 9 3900X 12-Core Processor            ` | `IntelLLVM 20250101` | `compare` | `L50` | 0 | `auto` | `0x14650fb0739d0383` | 35 | CPU fallback | pass |
| SYCL | `AMD Ryzen 9 3900X 12-Core Processor            ` | `IntelLLVM 20250101` | `compare` | `L50` | 0 | `64` | `0x14650fb0739d0383` | 32 | CPU fallback | pass |
| SYCL | `AMD Ryzen 9 3900X 12-Core Processor            ` | `IntelLLVM 20250101` | `compare` | `L50` | 0 | `256` | `0x14650fb0739d0383` | 47 | CPU fallback | pass |
| SYCL | `AMD Ryzen 9 3900X 12-Core Processor            ` | `IntelLLVM 20250101` | `compare` | `L50` | 1 | `auto` | `0x44bd2bd473ccf799` | 39 | CPU fallback | pass |
| SYCL | `AMD Ryzen 9 3900X 12-Core Processor            ` | `IntelLLVM 20250101` | `compare` | `L50` | 1 | `64` | `0x44bd2bd473ccf799` | 46 | CPU fallback | pass |
| SYCL | `AMD Ryzen 9 3900X 12-Core Processor            ` | `IntelLLVM 20250101` | `compare` | `L50` | 1 | `256` | `0x44bd2bd473ccf799` | 38 | CPU fallback | pass |
| SYCL | `AMD Ryzen 9 3900X 12-Core Processor            ` | `IntelLLVM 20250101` | `compare` | `L50` | 127 | `auto` | `0xaf7ae26f6269abab` | 57 | CPU fallback | pass |
| SYCL | `AMD Ryzen 9 3900X 12-Core Processor            ` | `IntelLLVM 20250101` | `compare` | `L50` | 127 | `64` | `0xaf7ae26f6269abab` | 52 | CPU fallback | pass |
| SYCL | `AMD Ryzen 9 3900X 12-Core Processor            ` | `IntelLLVM 20250101` | `compare` | `L50` | 127 | `256` | `0xaf7ae26f6269abab` | 42 | CPU fallback | pass |
| SYCL | `AMD Ryzen 9 3900X 12-Core Processor            ` | `IntelLLVM 20250101` | `compare` | `L50` | 256 | `auto` | `0x67d36b9d21480dbf` | 51 | CPU fallback | pass |
| SYCL | `AMD Ryzen 9 3900X 12-Core Processor            ` | `IntelLLVM 20250101` | `compare` | `L50` | 256 | `64` | `0x67d36b9d21480dbf` | 57 | CPU fallback | pass |
| SYCL | `AMD Ryzen 9 3900X 12-Core Processor            ` | `IntelLLVM 20250101` | `compare` | `L50` | 256 | `256` | `0x67d36b9d21480dbf` | 38 | CPU fallback | pass |
| SYCL | `AMD Ryzen 9 3900X 12-Core Processor            ` | `IntelLLVM 20250101` | `compare` | `L50` | 1009 | `auto` | `0xb41c8296cf2c4b7f` | 70 | CPU fallback | pass |
| SYCL | `AMD Ryzen 9 3900X 12-Core Processor            ` | `IntelLLVM 20250101` | `compare` | `L50` | 1009 | `64` | `0xb41c8296cf2c4b7f` | 55 | CPU fallback | pass |
| SYCL | `AMD Ryzen 9 3900X 12-Core Processor            ` | `IntelLLVM 20250101` | `compare` | `L50` | 1009 | `256` | `0xb41c8296cf2c4b7f` | 55 | CPU fallback | pass |
| SYCL | `AMD Ryzen 9 3900X 12-Core Processor            ` | `IntelLLVM 20250101` | `compare` | `L50` | 65536 | `auto` | `0x545581db097652e7` | 351 | CPU fallback | pass |
| SYCL | `AMD Ryzen 9 3900X 12-Core Processor            ` | `IntelLLVM 20250101` | `compare` | `L50` | 65536 | `64` | `0x545581db097652e7` | 489 | CPU fallback | pass |
| SYCL | `AMD Ryzen 9 3900X 12-Core Processor            ` | `IntelLLVM 20250101` | `compare` | `L50` | 65536 | `256` | `0x545581db097652e7` | 388 | CPU fallback | pass |
| SYCL | `AMD Ryzen 9 3900X 12-Core Processor            ` | `IntelLLVM 20250101` | `min` | `L50` | 0 | `auto` | `0x14650fb0739d0383` | 47 | CPU fallback | pass |
| SYCL | `AMD Ryzen 9 3900X 12-Core Processor            ` | `IntelLLVM 20250101` | `min` | `L50` | 0 | `64` | `0x14650fb0739d0383` | 35 | CPU fallback | pass |
| SYCL | `AMD Ryzen 9 3900X 12-Core Processor            ` | `IntelLLVM 20250101` | `min` | `L50` | 0 | `256` | `0x14650fb0739d0383` | 27 | CPU fallback | pass |
| SYCL | `AMD Ryzen 9 3900X 12-Core Processor            ` | `IntelLLVM 20250101` | `min` | `L50` | 1 | `auto` | `0x7489b21005cd153b` | 56 | CPU fallback | pass |
| SYCL | `AMD Ryzen 9 3900X 12-Core Processor            ` | `IntelLLVM 20250101` | `min` | `L50` | 1 | `64` | `0x7489b21005cd153b` | 49 | CPU fallback | pass |
| SYCL | `AMD Ryzen 9 3900X 12-Core Processor            ` | `IntelLLVM 20250101` | `min` | `L50` | 1 | `256` | `0x7489b21005cd153b` | 38 | CPU fallback | pass |
| SYCL | `AMD Ryzen 9 3900X 12-Core Processor            ` | `IntelLLVM 20250101` | `min` | `L50` | 127 | `auto` | `0x59b75635925265af` | 55 | CPU fallback | pass |
| SYCL | `AMD Ryzen 9 3900X 12-Core Processor            ` | `IntelLLVM 20250101` | `min` | `L50` | 127 | `64` | `0x59b75635925265af` | 47 | CPU fallback | pass |
| SYCL | `AMD Ryzen 9 3900X 12-Core Processor            ` | `IntelLLVM 20250101` | `min` | `L50` | 127 | `256` | `0x59b75635925265af` | 51 | CPU fallback | pass |
| SYCL | `AMD Ryzen 9 3900X 12-Core Processor            ` | `IntelLLVM 20250101` | `min` | `L50` | 256 | `auto` | `0x3d322485f3f6aa2b` | 37 | CPU fallback | pass |
| SYCL | `AMD Ryzen 9 3900X 12-Core Processor            ` | `IntelLLVM 20250101` | `min` | `L50` | 256 | `64` | `0x3d322485f3f6aa2b` | 47 | CPU fallback | pass |
| SYCL | `AMD Ryzen 9 3900X 12-Core Processor            ` | `IntelLLVM 20250101` | `min` | `L50` | 256 | `256` | `0x3d322485f3f6aa2b` | 71 | CPU fallback | pass |
| SYCL | `AMD Ryzen 9 3900X 12-Core Processor            ` | `IntelLLVM 20250101` | `min` | `L50` | 1009 | `auto` | `0x99bf35c81f0fa003` | 64 | CPU fallback | pass |
| SYCL | `AMD Ryzen 9 3900X 12-Core Processor            ` | `IntelLLVM 20250101` | `min` | `L50` | 1009 | `64` | `0x99bf35c81f0fa003` | 61 | CPU fallback | pass |
| SYCL | `AMD Ryzen 9 3900X 12-Core Processor            ` | `IntelLLVM 20250101` | `min` | `L50` | 1009 | `256` | `0x99bf35c81f0fa003` | 92 | CPU fallback | pass |
| SYCL | `AMD Ryzen 9 3900X 12-Core Processor            ` | `IntelLLVM 20250101` | `min` | `L50` | 65536 | `auto` | `0x0d25f2c9400404ce` | 534 | CPU fallback | pass |
| SYCL | `AMD Ryzen 9 3900X 12-Core Processor            ` | `IntelLLVM 20250101` | `min` | `L50` | 65536 | `64` | `0x0d25f2c9400404ce` | 551 | CPU fallback | pass |
| SYCL | `AMD Ryzen 9 3900X 12-Core Processor            ` | `IntelLLVM 20250101` | `min` | `L50` | 65536 | `256` | `0x0d25f2c9400404ce` | 608 | CPU fallback | pass |
| SYCL | `AMD Ryzen 9 3900X 12-Core Processor            ` | `IntelLLVM 20250101` | `max` | `L50` | 0 | `auto` | `0x14650fb0739d0383` | 34 | CPU fallback | pass |
| SYCL | `AMD Ryzen 9 3900X 12-Core Processor            ` | `IntelLLVM 20250101` | `max` | `L50` | 0 | `64` | `0x14650fb0739d0383` | 36 | CPU fallback | pass |
| SYCL | `AMD Ryzen 9 3900X 12-Core Processor            ` | `IntelLLVM 20250101` | `max` | `L50` | 0 | `256` | `0x14650fb0739d0383` | 27 | CPU fallback | pass |
| SYCL | `AMD Ryzen 9 3900X 12-Core Processor            ` | `IntelLLVM 20250101` | `max` | `L50` | 1 | `auto` | `0x228ddb79e7cb791a` | 61 | CPU fallback | pass |
| SYCL | `AMD Ryzen 9 3900X 12-Core Processor            ` | `IntelLLVM 20250101` | `max` | `L50` | 1 | `64` | `0x228ddb79e7cb791a` | 41 | CPU fallback | pass |
| SYCL | `AMD Ryzen 9 3900X 12-Core Processor            ` | `IntelLLVM 20250101` | `max` | `L50` | 1 | `256` | `0x228ddb79e7cb791a` | 49 | CPU fallback | pass |
| SYCL | `AMD Ryzen 9 3900X 12-Core Processor            ` | `IntelLLVM 20250101` | `max` | `L50` | 127 | `auto` | `0xd5a377ad8a957b26` | 58 | CPU fallback | pass |
| SYCL | `AMD Ryzen 9 3900X 12-Core Processor            ` | `IntelLLVM 20250101` | `max` | `L50` | 127 | `64` | `0xd5a377ad8a957b26` | 53 | CPU fallback | pass |
| SYCL | `AMD Ryzen 9 3900X 12-Core Processor            ` | `IntelLLVM 20250101` | `max` | `L50` | 127 | `256` | `0xd5a377ad8a957b26` | 51 | CPU fallback | pass |
| SYCL | `AMD Ryzen 9 3900X 12-Core Processor            ` | `IntelLLVM 20250101` | `max` | `L50` | 256 | `auto` | `0xbbf87d80d2a23ded` | 52 | CPU fallback | pass |
| SYCL | `AMD Ryzen 9 3900X 12-Core Processor            ` | `IntelLLVM 20250101` | `max` | `L50` | 256 | `64` | `0xbbf87d80d2a23ded` | 51 | CPU fallback | pass |
| SYCL | `AMD Ryzen 9 3900X 12-Core Processor            ` | `IntelLLVM 20250101` | `max` | `L50` | 256 | `256` | `0xbbf87d80d2a23ded` | 71 | CPU fallback | pass |
| SYCL | `AMD Ryzen 9 3900X 12-Core Processor            ` | `IntelLLVM 20250101` | `max` | `L50` | 1009 | `auto` | `0xfb179fe5205c8474` | 69 | CPU fallback | pass |
| SYCL | `AMD Ryzen 9 3900X 12-Core Processor            ` | `IntelLLVM 20250101` | `max` | `L50` | 1009 | `64` | `0xfb179fe5205c8474` | 54 | CPU fallback | pass |
| SYCL | `AMD Ryzen 9 3900X 12-Core Processor            ` | `IntelLLVM 20250101` | `max` | `L50` | 1009 | `256` | `0xfb179fe5205c8474` | 88 | CPU fallback | pass |
| SYCL | `AMD Ryzen 9 3900X 12-Core Processor            ` | `IntelLLVM 20250101` | `max` | `L50` | 65536 | `auto` | `0xf973ba4e15385def` | 623 | CPU fallback | pass |
| SYCL | `AMD Ryzen 9 3900X 12-Core Processor            ` | `IntelLLVM 20250101` | `max` | `L50` | 65536 | `64` | `0xf973ba4e15385def` | 543 | CPU fallback | pass |
| SYCL | `AMD Ryzen 9 3900X 12-Core Processor            ` | `IntelLLVM 20250101` | `max` | `L50` | 65536 | `256` | `0xf973ba4e15385def` | 604 | CPU fallback | pass |

## Phase 5D Tiny Transformer VM Runtime Fixture

| Runtime | DMEM Words | Scalar Ops | DMEM Loads | DMEM Stores | Runtime (us) | Max Error |
| ------- | ---------: | ---------: | ---------: | ----------: | -----------: | --------: |
| VM tensor runtime | 138 | 745 | 144 | 36 | 3256 | 3.49523e-10 |
