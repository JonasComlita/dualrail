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
2. **Implemented:** `benchmark_bitnet_os` builds a deterministic sliced model:
   three 729-word ternary shards by default (nine in the opt-in sustained
   profile), with a frozen model checksum and per-shard token output checksum.
3. **Implemented:** package every shard as a separate VFS file, reboot, read
   every shard back, and validate exact ordering and checksums before inference.
4. **Implemented baseline:** hosted OS packaging plus a v2 VM vector-kernel
   portfolio, vector invocation/lane counters, and a 4-page allocation-pressure
   probe (96 pages in the sustained profile).
5. **Implemented baseline:** record package/load/kernel times, shard read counts,
   vector dynamic instructions, sustained token throughput, high-water words,
   disk words, and checksums.
6. **Implemented:** the manual CMake/manifest target emits
   `build/benchmarks/bitnet-os.json`.

The bounded portfolio performs 243 repetitions across three shards (729
vector-kernel invocations per measured sample). Set
`TRIT_BENCH_PROFILE=large-sustained` for nine shards and 729 repetitions. Both
profiles keep the total sustained kernel work deterministic while exercising
shard loading and memory pressure; timing is accepted only when the seven
measured samples have CV <3%.

The host harness runs one bounded probe first. The probe uses one shard and one
repetition, must complete in at most 60 seconds per sample, and is not treated
as evidence for the two-warmup/seven-sample CV gate.

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
- `model_shards_read`
- `read_shards`
- `disk_read_ms`
- `kernel_compute_ms`
- `vector_kernel_invocations`
- `vector_lanes_processed`
- `sustained_tokens`
- `accelerator_compute_ms`

## Non-Goals For The First Slice

- full 1.58B model inside the guest before smaller slices work
- host-specific throughput budgets without an archived controlled-host baseline
- host/GPU parity claims without reproducible baselines
