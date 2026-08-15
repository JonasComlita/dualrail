# Reproducible performance baselines

`benchmarks/performance_baseline.py` records a small, repeatable trace for the
five OS surfaces that are otherwise difficult to compare across hosts:

| Surface | Functional (authoritative) evidence | Wall-clock observation |
| --- | --- | --- |
| Boot | six fixed reset, loader, kernel, VFS, scheduler, and desktop events | `boot_time_ms` |
| App launch | five fixed lookup, read, verify, and handoff events | `app_launch_time_ms` |
| Frame loop | number of deterministic frame presents | mean `frame_time_ms` |
| Disk I/O | read/write bytes and operation counts for a fixed payload | `disk_io_time_ms` |
| Context switches | two boot/app handoffs plus one handoff per frame | optional host counter, when the OS exposes one |

The report contract is [`performance_baseline_schema.json`](../../benchmarks/performance_baseline_schema.json), with a host-neutral functional
fixture at [`performance-baseline.v1.json`](../../benchmarks/reference/performance-baseline.v1.json).

## Collecting a run

The collector needs only the Python standard library and does not start SDL or
compile guest sources.  The default trace uses two warmups and seven measured
iterations to match the existing benchmark protocol:

```powershell
python benchmarks/performance_baseline.py collect `
  --output build/benchmarks/performance-baseline.json
```

For a fast local check, reduce the iteration count.  This does not change the
functional trace, and timing variation is intentionally not a gate:

```powershell
python benchmarks/performance_baseline.py collect --warmups 0 --iterations 1
python benchmarks/performance_baseline.py validate `
  build/benchmarks/performance-baseline.json `
  --reference benchmarks/reference/performance-baseline.v1.json
```

`--evidence` can be repeated to retain counters from an existing
`benchmark_doom_os`/`benchmark_bitnet_os` report or a runtime diagnostic JSON:

```powershell
python benchmarks/performance_baseline.py collect `
  --evidence build/benchmarks/doom-os.json `
  --evidence build/diagnostics/latest/checkpoint.json
```

External values are copied to `external_evidence`; they never overwrite the
fixed-trace counters.  This keeps a release-run measurement comparable with
the synthetic reference without claiming that two different workloads are
identical.

## Reading a report

Every item under `metrics` declares its authority.  Functional counters have
`deterministic: true`, `authority: "functional"`, and a single integer
`value`.  Timing metrics have `deterministic: false`,
`authority: "advisory"`, and a sample distribution (`median`, `p95`, mean,
standard deviation, and coefficient of variation).  The host metadata records
the platform, Python implementation, CPU count/affinity, control profile, and
a fingerprint so timing observations can be grouped by controlled host.

The protocol explicitly sets `timing_threshold_enforced: false`.  A high CV,
different scheduler policy, filesystem cache state, or an uncontrolled host
must be reported and investigated, but cannot turn a passing functional trace
into a false functional failure.  The `host_context_switches` metric follows
the same rule: POSIX `getrusage` values are advisory and Windows reports the
counter as unavailable when no standard-library source exists.

The `reproducibility` object hashes the fixed input and every measured trace.
`functional_match` and `trace_repeatable` must remain true.  The reference
command writes an intentionally host-neutral artifact with zero-valued,
`reference_unmeasured` timing samples; those values are placeholders, not a
performance claim:

```powershell
python benchmarks/performance_baseline.py reference
```

Use the existing Doom/BitNet system gate for guest workload correctness and
the baseline collector for cross-host surface evidence.  Do not compare raw
milliseconds from different host fingerprints without first controlling CPU
affinity, power policy, filesystem cache state, and background load.
