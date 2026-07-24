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

1. Add a small Doom-like benchmark app that renders deterministic frames from
   packaged assets.
2. Record time-to-first-frame, average frame time, p95 frame time, frame count,
   input event latency, asset load time, and memory high-water mark.
3. Port or adapt a minimal Doom renderer/game loop.
4. Package shareware-compatible or test WAD assets into `.tdisk`.
5. Add `benchmark_doom_os` as a manual CMake target once it emits stable JSON.

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
- `memory_high_water_words`

## Non-Goals For The First Slice

- perfect Doom compatibility
- audio synchronization
- multiplayer/network support
- CI gating
