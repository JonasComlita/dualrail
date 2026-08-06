# Doom-Class OS Benchmark

Doom is the interactive systems benchmark target. The goal is not just to draw
pixels; it is to prove that the OS can sustain a real-time app with input,
timers, asset loading, memory allocation, and diagnostics.

## Capability Surfaces

- framebuffer setup and present path
- palette or pixel conversion
- keyboard/input event routing
- fixed-timestep timer behavior
- filesystem reads for WAD-like assets
- app heap allocation and memory reuse
- scheduler behavior under a steady frame loop
- audio path when available
- crash diagnostics on app failure

## Milestones

1. **Implemented:** `benchmark_doom_os` runs a deterministic Doom-like kernel
   workload that writes one synthetic WAD-like asset, reads it back in 81-word
   chunks, renders 7 frames by default, routes a fixed two-event input trace,
   advances one timer tick per frame, and checks frozen asset/frame/input hashes.
2. **Implemented baseline:** emit machine-readable compile/run time, dynamic
   instructions, frame/timer/input counts, draw/present calls, chunked I/O
   counters, VFS words, memory high-water mark, and correctness hashes.
3. **Implemented sustained profile:** set `TRIT_BENCH_PROFILE=large-sustained`
   for 81 frames, 729 asset words, nine read chunks, and a three-event trace.
   This profile is intentionally opt-in so the bounded gate remains practical.
4. Port or adapt a minimal Doom renderer/game loop.
5. Package shareware-compatible or test WAD assets into `.tdisk`.
6. **Implemented:** the manual CMake/manifest target emits
   `build/benchmarks/doom-os.json`.

## Correctness Checks

- deterministic startup state
- reproducible frame hash for a fixed input trace
- no unexpected VM trap
- asset reads match expected counts or checksums
- benchmark exits with a clear status code

## Initial Metrics

- `time_to_first_frame_ms`
- `avg_frame_ms`
- `p95_frame_ms`
- `frames_presented`
- `late_frames`
- `input_to_present_ms`
- `asset_load_ms`
- `disk_read_words`
- `asset_read_operations`
- `io_operations`
- `memory_high_water_words`
- `draw_calls`
- `present_calls`
- `input_trace_hash`

The host harness runs one bounded probe first (one frame, no input events),
requires its guest pass to stay at or below 60 seconds, and starts the full
two-warmup/seven-sample CV gate only after that probe passes. The 81-frame,
729-word workload is explicit opt-in via `TRIT_BENCH_PROFILE=large-sustained`.

## Non-Goals For The First Slice

- perfect Doom compatibility
- audio synchronization
- multiplayer/network support
- host-specific frame-time budgets without an archived controlled-host baseline
