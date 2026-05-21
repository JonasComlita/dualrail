# Optimization Baseline Log

This file records pre-optimization reference points. Add a new dated section
after each optimization tranche and compare against the previous baseline using
the same commands where possible.

## 2026-05-10 - Phase 4 Reference VM, Before UInt128/Int128 Hot-Path Optimization

### Context

- Git base: `bef4072` plus local Phase 4 reference VM changes.
- OS: `Microsoft Windows NT 10.0.22000.0`
- Shell: Windows PowerShell `5.1.22000.2538`
- CPU identifier: `AMD64 Family 23 Model 113 Stepping 0, AuthenticAMD`
- CPU name from existing tuning notes: `AMD Ryzen 9 3900X 12-Core Processor`
- Compiler: `g++.exe (Rev8, Built by MSYS2 project) 15.2.0`
- Benchmark build command: `g++ -std=c++20 -O3 -DNDEBUG benchmark.cpp -o benchmark.exe`
- Benchmark run command: `.\benchmark.exe`
- Benchmark scale: `1`
- Benchmark sink: `0x67861d4c6575a518`

### Storage Baseline

Measured with the current compiler and ABI.

| Type | Size |
|---|---:|
| `UInt128` | 16 bytes |
| `Int128` | 24 bytes |
| `T1` | 1 byte |
| `T5` | 1 byte |
| `T10` | 2 bytes |
| `T20` | 4 bytes |
| `Triple` / `T40` | 8 bytes |
| `LongTriple` / `T50` | 16 bytes |
| `TritLane1` | 1 byte |
| `TritLane5` | 2 bytes |
| `TritLane10` | 4 bytes |
| `TritLane20` | 8 bytes |
| `TritLane40` | 16 bytes |
| `TritLane50` | 16 bytes |
| `vm::TernaryValue` | 24 bytes |
| `vm::TernaryVectorRegister` | 24 bytes |
| `vm::VectorFaultState` | 48 bytes |
| `vm::VMState` | 1000 bytes |

### Speed Baseline - Native Numeric Hot Paths

| Benchmark | ns/unit | Units/sec | Checksum |
|---|---:|---:|---|
| `T10 add` | 1832.74 | 545631 | `0xf210797c11990ba2` |
| `T20 add` | 3660.95 | 273153 | `0xce6de2b8a749984c` |
| `T40 add` | 9039.85 | 110621 | `0xe92a2c1d8d4760cd` |
| `T50 add` | 39571.10 | 25271 | `0x6214e4db184ca64c` |
| `T10 multiply` | 3228.02 | 309787 | `0x6c967af4cb26c6c1` |
| `T20 multiply` | 7719.81 | 129537 | `0xe39aa7b11a8a1c41` |
| `T40 multiply` | 24701.22 | 40484 | `0xe4ed4172b1b1be35` |
| `T50 multiply` | 61955.05 | 16141 | `0xf11546d81be2c3e9` |
| `T10 divide` | 1779.52 | 561948 | `0x7bb7eef7d8d7bba4` |
| `T20 divide` | 3059.97 | 326800 | `0x7f366214d7942710` |
| `T40 divide` | 7952.08 | 125753 | `0x835698ad0a5f0ff0` |
| `T50 divide` | 46244.78 | 21624 | `0xad87b27edac0e62b` |
| `T10 sqrt` | 16833.10 | 59407 | `0xba54b6516e2dc220` |
| `T20 sqrt` | 38071.74 | 26266 | `0x6018d850d7a125a5` |
| `T40 sqrt` | 105524.24 | 9476 | `0xe976f43e07b4b6db` |
| `T50 sqrt` | 425363.20 | 2351 | `0x2a2789558647a8b7` |

### Speed Baseline - Lane and Kernel Hot Paths

| Benchmark | ns/unit | Units/sec | Checksum |
|---|---:|---:|---|
| `T20 roundtrip` | 63.47 | 15754656 | `0x66dee8331f544cc0` |
| `T40 roundtrip` | 549.43 | 1820065 | `0xc44e053b40d0611e` |
| `T50 roundtrip` | 15310.54 | 65314 | `0x797650f53a003252` |
| `T20 tritwiseAdd` | 83.47 | 11980158 | `0xd6ffffdbd9c3c1fb` |
| `T40 tritwiseAdd` | 223.39 | 4476506 | `0x7a1fda552522c797` |
| `T50 tritwiseAdd` | 279.70 | 3575214 | `0xb1921de298a6a811` |
| `TritLane20 add auto(avx2)` | 23.41 | 42708842 | `0xffac27f44e6e1caa` |
| `TritLane40 add auto(scalar)` | 240.83 | 4152339 | `0xe0882af0b43e4774` |
| `TritLane50 add auto(scalar)` | 254.28 | 3932678 | `0x473dec292997a2c0` |
| `TritLane20 add raw` | 62.42 | 16020564 | `0xa1cab75e95b41664` |
| `TritLane40 add raw` | 145.87 | 6855512 | `0x5f77fe0e78ca2bb3` |
| `TritLane50 add raw` | 221.04 | 4524046 | `0x870b8b7b786f2cbc` |
| `VM dispatch loop` | 244.95 | 4082435 | `0x9a0812482c2094fd` |

### Verification Baseline

- `test_multiwidth_vm.cpp`: passed
- `test_ternary_lanes.cpp`: passed
- `test_native_ops.cpp`: passed
- `test_numeric_workloads.cpp`: passed
- `benchmark.cpp`: built with `-O3 -DNDEBUG` and ran successfully
- `git diff --check`: no whitespace errors

### Warning Baseline

- Phase 4 lane-mode `-Wswitch` warnings are quiet.
- Remaining `-Wpedantic` warnings are the known `__int128` interop/reference
  warnings in `ternary_uint128.h` and test reference code.

### Comparison Rules For Future Optimizations

- Keep `UInt128` and `LongTriple` public storage layout stable unless a separate
  ABI migration is explicitly planned.
- Re-run the exact benchmark command above before and after each optimization.
- Preserve checksums unless the benchmark intentionally changes semantics or
  corpus generation.
- Report speedup as `old ns/unit / new ns/unit` for hot paths.
- Record any storage-size changes in the storage table.
- Run the full regression suite before accepting an optimization.

### Post-Optimization Results

#### UInt128 Hot-Path Tranche

Implemented without changing the public ABI:

- `UInt128` remains `{ uint64_t lo; uint64_t hi; }`.
- Added internal host-native `NativeUInt128` fast paths where available.
- Added `divModSmall()` / `divMod3()` and used fused quotient+remainder paths
  in `roundedDiv3`, mantissa encoding, `LongTriple::unpack`, and `toString`.
- Added guarded MSVC carry/borrow support.
- Added direct `UInt128 * uint32_t` support for portable fallback paths.

Benchmark command:

`g++ -std=c++20 -O3 -DNDEBUG benchmark.cpp -o benchmark.exe`

Benchmark run:

`.\benchmark.exe`

Benchmark sink:

`0xa2f99029460268a3`

#### UInt128 Microbenchmarks

| Metric | ns/unit | ops/sec | Checksum |
| ------ | ------: | ------: | -------- |
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

#### High-Level Impact

| Metric | Baseline ns/unit | New ns/unit | Speedup | New checksum |
| ------ | ---------------: | ----------: | ------: | ------------ |
| `T50 add` | 39571.10 | 1101.61 | 35.92x | `0x6214e4db184ca64c` |
| `T50 multiply` | 61955.05 | 2645.54 | 23.42x | `0xf11546d81be2c3e9` |
| `T50 divide` | 46244.78 | 705.41 | 65.56x | `0xced7cef435b93466` |
| `T50 sqrt` | 425363.20 | 12977.87 | 32.78x | `0x2a2789558647a8b7` |
| `T50 roundtrip` | 15310.54 | 520.27 | 29.43x | `0x797650f53a003252` |
| `T50 tritwiseAdd` | 279.70 | 279.69 | 1.00x | `0xb1921de298a6a811` |
| `TritLane50 add raw` | 221.04 | 226.79 | 0.97x | `0x870b8b7b786f2cbc` |
| `VM dispatch loop` | 244.95 | 72.78 | 3.37x | `0x9a0812482c2094fd` |

#### Verification

- `test_multiwidth_vm.cpp`: passed
- `test_ternary_lanes.cpp`: passed
- `test_native_ops.cpp`: passed
- `test_numeric_workloads.cpp`: passed
- `benchmark.cpp`: built with `-O3 -DNDEBUG` and ran successfully

## Phase 5D Tiny Transformer VM Runtime Fixture

| Runtime | Shape | Assembly Words | VM Steps | Kernel Runs | DMEM Words | DMEM Loads | DMEM Stores | Runtime (us) | Checksum | Max Error | Notes |
| ------- | ----- | -------------: | -------: | ----------: | ---------: | ---------: | ----------: | -----------: | -------- | --------: | ----- |
| Tiny character transformer | vocab=4, seq=2, width=3, heads=1 | 2298 | 2298 | 6 | 138 | 156 | 48 | 49232 | 0xd317ad9abb2cdb68 | 4.65543e-10 | IR-generated VM kernels, no transformer opcodes |
