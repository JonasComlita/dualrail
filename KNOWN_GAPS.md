# Known Gaps

These are intentionally visible so agents can pick useful work without asking for direction.

## Highest Priority

- TreatCode Intelligence v3.1 has the repository executor, privileged grading
  contracts, disposable pilot evidence, calibration/freeze/subject aggregators,
  and twenty atomic five-task authoring shards. Each completed shard is bound to
  the assignment plan, contribution bytes, canonical task array, and five task
  hashes in an immutable private checkpoint, so an interrupted author loses at
  most the active five-task shard. The corpus remains intentionally non-official:
  all twenty genuine non-subject author checkpoints, two independent reviews per
  task, executable package qualification, 1,800 weak/medium/frontier calibration
  observations, verified infrastructure attestation, a frozen holdout, and 200
  one-shot Luna/Sol attempts with provider-signed execution identity are still
  missing. Earlier scalar suites remain
  diagnostics and must not be presented as model-ranking scores.

- Define the exact-width binary interoperability contract required for physical
  devices: endian-qualified values, packed structures, byte buffers, volatile
  MMIO, binary bus addresses, DMA/coherency, IOMMU hooks, and a general interrupt
  controller. Existing CSR block/framebuffer/input facades are not evidence of
  native PCIe, NVMe, USB, Ethernet, display, or audio driver support.
- Promote `apps/libwidget.trit` into a specified, modular TritUI application
  framework with an event loop, layout, drawing/text, standard widgets,
  clipboard, accessibility, themes, resources, models, and development tools.
  A window manager plus direct framebuffer helpers is not yet a complete
  third-party desktop SDK.
- Specify a versioned system-profile and capability-closure format for desktop,
  server, embedded, appliance, and single-application/unikernel images. The
  current monolithic kernel may remain monolithic at runtime while becoming
  selectively composed at build and image time.

## Medium Priority

- Extend the current small DTB/HAL into a hierarchical device graph and driver
  lifecycle. Provide shared virtual device contracts first, followed by UART,
  GPIO/I2C/SPI/I2S, PCIe, NVMe, Ethernet, USB, display, and audio drivers in the
  dependency order recorded in
  `docs/12_Future_Architecture/platform_evolution_plan.md`.
- Formalize accelerator lowering so the portable ternary vector/matrix semantics
  remain authoritative while AVX2, future ACE, GPU, FPGA, and ASIC paths are
  optional equivalent backends. Define dot/matrix accumulation, rounding,
  conversion, invalid-state, feature-discovery, and state/ABI contracts before
  adding branded or hardware-specific operations.
- Treat NVFP4 as an explicit tensor interoperability/backend format with scale
  metadata, conversion boundaries, accuracy tests, and separately reported
  conversion cost; it is not selected as the native ternary scalar format.
- External payload acceptance is complete in the ignored, provenance-locked
  local cache: Freedoom slice/full frame hashes and official BitNet slice/full
  token/reference hashes pass. Payloads, converted tensors, and inventories
  remain intentionally untracked; a fresh machine must acquire the cache, and
  the normal offline benchmark profile remains synthetic.
- Complete the independent cryptographic review for the production host-only
  `TRITENC1` lifecycle. The strict key provider, OpenSSL 3 AES-256-GCM path,
  parser, CLI, managed plaintext lifecycle, atomic replacement, and focused
  tamper/cleanup tests are implemented. The review gate still has unresolved
  high-severity nonce-uniqueness, replay/rollback, crash-cleanup, key-file
  race, provider-policy, and fuzz-bound findings. Any format-affecting
  remediation requires a new envelope version; the ternary-native proposal
  remains explicitly non-selected.

## Lower Priority

- Consider a Qt compatibility port only after the C++ ABI/runtime and toolkit-
  neutral window/platform services are sufficient. After any Python port,
  prefer Python bindings to TritUI before importing Tcl/Tk solely for Tkinter.
- Replay schema adapters, POSIX wrappers, source-derived manifest checks, and
  canvas/Graphify freshness warnings have focused coverage and are not the
  blockers for the future platform tracks above.
