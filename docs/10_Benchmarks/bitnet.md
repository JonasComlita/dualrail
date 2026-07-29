# BitNet 1.58B-Class OS Benchmark

BitNet is the heavy compute and data movement benchmark target. The goal is to
prove that Ternary OS can load large model artifacts, keep long-running compute
stable, and report useful performance telemetry.

## Capability Surfaces

- large file packaging in `.tdisk`
- model metadata loading
- sequential and chunked disk reads
- memory pressure and high-water tracking
- vector and matrix kernels
- accelerator integration when configured
- long-running process stability
- diagnostics for model or runtime failure

## Milestones

1. Keep the host BitNet runner as the reference result.
2. **Implemented:** `benchmark_bitnet_os` uses a deterministic 27x27 ternary
   model and a frozen 27-token output checksum.
3. **Implemented:** package the model in the OS VFS, reboot, and validate exact
   readback before inference.
4. **Implemented baseline:** hosted OS packaging plus a v2 VM vector-kernel
   portfolio and allocation-pressure probe.
5. **Implemented baseline:** record package/load/kernel times, vector dynamic
   instructions, tokens per second, high-water words, disk words, and checksums.
6. **Implemented:** the manual CMake/manifest target emits
   `build/benchmarks/bitnet-os.json`.

## Correctness Checks

- deterministic prompt and token count
- output checksum or token sequence match for the reference workload
- no unexpected VM trap
- model artifact checksum validation
- benchmark exits with a clear status code

## Initial Metrics

- `model_load_ms`
- `first_token_ms`
- `tokens_per_second`
- `tokens_generated`
- `memory_high_water_words`
- `disk_read_words`
- `disk_read_ms`
- `kernel_compute_ms`
- `accelerator_compute_ms`

## Non-Goals For The First Slice

- full 1.58B model inside the guest before smaller slices work
- CI gating
- host/GPU parity claims without reproducible baselines
