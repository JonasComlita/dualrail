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

## Offline workflow

The importer never downloads files:

```text
python tools/import_external_benchmark_assets.py validate
python tools/import_external_benchmark_assets.py validate --require-present
python tools/import_external_benchmark_assets.py import --kind bitnet --stage sliced \
  --source C:/models/bitnet-slices --id bitnet-local-slices \
  --output-manifest build/bitnet-slices.inventory.json
```

`metadata` accepts JSON/YAML configuration slices, `sliced` accepts numbered
model shards (for example `model-00001-of-00003.safetensors`), and `full`
requires every declared file plus a valid container header.  Safetensors
offsets and GGUF fixed headers are checked without loading tensor payloads.
WAD validation checks the IWAD/PWAD header, lump directory bounds, lump names,
and lump data bounds.  Hashes are streamed with SHA-256.

Tests and release jobs should invoke `validate` against local files only.  A
missing external file is a deliberate, visible `not_present` state; it is not
silently replaced by a synthetic benchmark or reported as imported.
