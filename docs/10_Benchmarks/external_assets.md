# External Benchmark Assets

The deterministic system benchmarks remain the release-gate workloads.  This
page records how optional Doom-class and BitNet 1.58B-class assets can be
introduced without making an unverified download or license claim.

## Provenance inventory

`benchmarks/assets/external_assets.v1.json` is the source-of-truth inventory for
external benchmark payloads.  It records the official URL, revision, license,
expected size/hash, and whether the payload is actually present locally.

- Freedoom 0.13.0 is the recommended freely redistributable WAD source.  The
  official release checksum is recorded, and the archive's BSD-3-Clause notice
  must travel with any copied WAD.
- Microsoft's `microsoft/bitnet-b1.58-2B-4T` model card identifies packed
  safetensors, BF16, and GGUF variants and links the official `microsoft/BitNet`
  inference implementation.  The model and GGUF files are MIT-licensed but
  are roughly 1.18 GB each, so no weight payload is checked in.
- ViZDoom is recorded only as an optional external renderer reference.  Its
  README says original ViZDoom code is MIT while ZDoom-derived code has mixed
  licenses; no renderer is vendored by this repository.

The checked-in BitNet fixture is metadata-only and explicitly sets
`weights_included: false`.  It must not be used as evidence that a full model
has been imported.

## Validation and explicit acquisition

`validate`, `cache`, and `import` are offline. They never follow a URL or
replace a missing payload with synthetic data. Network access is available only
through the explicit, provenance-locked `acquire` command:

```text
python tools/import_external_benchmark_assets.py validate
python tools/import_external_benchmark_assets.py cache \
  --asset-id doom-freedoom-0.13.0
python tools/import_external_benchmark_assets.py cache \
  --asset-id bitnet-b1.58-2B-4T-safetensors
python tools/import_external_benchmark_assets.py cache \
  --asset-id bitnet-b1.58-2B-4T-gguf
python tools/import_external_benchmark_assets.py cache \
  --asset-id doom-freedoom-0.13.0
python tools/import_external_benchmark_assets.py acquire \
  --id doom-freedoom-0.13.0 --kind doom
python tools/import_external_benchmark_assets.py acquire \
  --id bitnet-b1.58-2B-4T-safetensors --kind bitnet
python tools/import_external_benchmark_assets.py acquire \
  --id bitnet-b1.58-2B-4T-gguf --kind bitnet
python tools/import_external_benchmark_assets.py import --kind bitnet --stage sliced \
  --source C:/models/bitnet-slices --id bitnet-local-slices \
  --output-manifest build/bitnet-slices.inventory.json
```

`acquire` derives the revision-pinned URL from the checked-in lock, leaves a
`.part` file for resumable retries, verifies the locked archive/file size and
SHA-256, and atomically promotes only validated payloads under the ignored
`build/external-assets/<asset-id>/` directory. `--force` is required to replace
an existing mismatched file. The checked-in provenance manifest is not edited.

`metadata` accepts JSON/YAML configuration slices, `sliced` accepts numbered
model shards (for example `model-00001-of-00003.safetensors`), and `full`
requires every declared file plus a valid container header.  Safetensors
offsets and GGUF fixed headers are checked without loading tensor payloads.
WAD validation checks the IWAD/PWAD header, lump directory bounds, lump names,
and lump data bounds.  Hashes are streamed with SHA-256.

Tests and release jobs should invoke `validate` against local files only. A
The checked-in lock remains `not_present` until a payload is intentionally
acquired; it is never rewritten to describe local cache state. Once an
operator has acquired a payload, the `cache --asset-id` command validates its
ignored `inventory.v1.json` and returns `skip` when that inventory is absent.
A missing external file is therefore a deliberate, visible `not_present` or
`skip` state; it is not silently replaced by a synthetic benchmark or
reported as imported.
