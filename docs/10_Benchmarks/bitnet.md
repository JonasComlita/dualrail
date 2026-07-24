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
2. Define a small deterministic inference workload using a tiny model or a
   sliced layer.
3. Package model shards and prompt data into OS-visible files.
4. Add a guest-visible runner or hosted OS benchmark path.
5. Record model load time, first-token latency, tokens per second, memory
   high-water mark, disk throughput, and accelerator/kernel counters.
6. Add `benchmark_bitnet_os` as a manual CMake target once it emits stable JSON.

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
