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

## External BitNet profiles

`synthetic` remains the normal offline profile. `official-slice` and
`official-full` require the provenance-locked local cache and never fall back
to synthetic tensors. Safetensors and GGUF validation is metadata-first;
conversion is tensor-by-tensor, resumable, hash-checked, and atomically
promoted into separate guest VFS files. A completed conversion also publishes
`guest_package.v1.json`, an immutable local package index that locks every
control and tensor file by exact size and SHA-256 before the guest executor can
open it. The conversion manifest records source hashes, tensor geometry,
packing/scales, converter version, output hashes, and completion state.

The official profiles validate the exact Microsoft artifact before doing
anything else. The importer now checks the safetensors tensor geometry and the
GGUF model identity (`bitnet-b1.58` / `bitnet2b`, 30 blocks, 2560 hidden,
6912 intermediate, 20/5 heads), and the converter decodes the official packed
U8 planes rather than treating the payload as a byte sample. The benchmark
performs an ABI-v3 VM configure/context round-trip. It still skips unless the
conversion manifest, two-run official reference evidence, and primary-owned
guest executor are all present; timing cannot waive a token mismatch.

Official conversion is explicit and writes only to the ignored build cache:

```text
python bitnet_weights/extract_bitnet.py \
  build/external-assets/bitnet-b1.58-2B-4T-safetensors/payload/model.safetensors \
  build/external-assets/bitnet-b1.58-2B-4T-safetensors/converted-official-slice \
  --expected-sha256 8143ae115ed6babe5e5ada8fb8c5b769d8f417802b2db042ad98b4f7ed73975b \
  --expected-size 1178623988 \
  --source-revision 04c3b9ad9361b824064a1f25ea60a8be9599b127 \
  --model-id bitnet-b1.58-2B-4T-safetensors \
  --profile official-slice --token-ids 128000,791,7438,315,2324,374

python bitnet_weights/extract_bitnet.py \
  build/external-assets/bitnet-b1.58-2B-4T-safetensors/payload/model.safetensors \
  build/external-assets/bitnet-b1.58-2B-4T-safetensors/converted-official-full \
  --expected-sha256 8143ae115ed6babe5e5ada8fb8c5b769d8f417802b2db042ad98b4f7ed73975b \
  --expected-size 1178623988 \
  --source-revision 04c3b9ad9361b824064a1f25ea60a8be9599b127 \
  --model-id bitnet-b1.58-2B-4T-safetensors \
  --profile official-full --token-ids 1,2
```

The pinned official GGUF runner must emit a `token_ids:` line. Two identical
runs are required before writing `reference.v1.json`:

```text
python bitnet_weights/official_reference.py \
  --runner C:/path/to/pinned/llama-cli.exe \
  --runner-revision <immutable-bitnet.cpp-revision> \
  --model build/external-assets/bitnet-b1.58-2B-4T-gguf/payload/ggml-model-i2_s.gguf \
  --expected-model-sha256 4221b252fdd5fd25e15847adfeb5ee88886506ba50b8a34548374492884c2162 \
  --prompt "The meaning of life is" --input-token-ids 128000,42 \
  --length 32 --threads 4 \
  --output build/external-assets/bitnet-b1.58-2B-4T-gguf/reference.v1.json
```

The external benchmark wiring is `TRIT_BITNET_CONVERTED_DIR`,
`TRIT_BITNET_REFERENCE_EVIDENCE`, and `TRIT_BITNET_GUEST_EXECUTOR`. The
repository includes the fixed-protocol `bitnet_guest_executor` target. It
validates the read-only guest package, loads the complete official
safetensors-derived conversion, probes the ABI-v3 vector context, runs the
fixed prompt IDs, and writes
`trit.bitnet_guest_execution.v1` only after the generated IDs exactly match the
two-run reference. `official-slice` remains a layer-zero conversion and
embedding-row probe; the full fixed-prompt parity contract is `official-full`.

Example wiring after the full conversion and reference evidence exist:

```powershell
$env:TRIT_BITNET_CONVERTED_DIR = "build/external-assets/bitnet-b1.58-2B-4T-safetensors/converted-official-full"
$env:TRIT_BITNET_REFERENCE_EVIDENCE = "build/external-assets/bitnet-b1.58-2B-4T-safetensors/derived-reference/reference.v1.json"
$env:TRIT_BITNET_GUEST_EXECUTOR = "build/bitnet_guest_executor.exe"
$env:TRIT_BITNET_PROFILE = "official-full"
$env:TRIT_BITNET_FORMAT = "safetensors"
build/benchmark_bitnet_os.exe build/external-bitnet-full.json
```

The external suite is manual (`external_system_benchmarks`). Missing official
weights are an explicit skip/non-pass. Full acceptance additionally requires
two matching official `bitnet.cpp` reference runs and exact token/hash
agreement from the Trit safetensors-derived representation.

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
