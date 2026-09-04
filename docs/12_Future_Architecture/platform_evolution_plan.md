# Future Platform Architecture

Status: **accepted planning scope; implementation remains open unless cited otherwise**

This plan records cross-layer requirements that were easy to overlook while the
project was being built vertically. The project boundary is the complete Trit
stack in the repository root: logic representation, ISA, VM, compiler, language,
runtime, kernel, applications, host runtime, and eventual physical hardware.
TreatCode is a consumer and collaboration surface inside that project; it is not
the architectural boundary used by this plan.

The items below are real future platform work. They are not claims that the
corresponding implementations already exist.

## Decisions and boundaries

1. **External standards remain compatible.** ASCII, UTF-8, executable and file
   formats, PCIe, NVMe, USB, Ethernet, display, and audio protocols retain their
   specified binary layouts. Trit adds explicit codecs, exact-width binary data,
   drivers, and translation boundaries rather than incompatible replacements.
2. **Ternary-native semantics remain authoritative.** Host AVX, future ACE,
   CUDA/SYCL, FPGA, and ASIC paths are implementation backends. They must not
   define guest-visible arithmetic semantics.
3. **The vector/matrix ISA is capability based, not vendor branded.** Trit may
   need native vector, dot-product, matrix-accumulate, conversion, and saturation
   operations, but it does not need a proprietary clone of AVX or ACE.
4. **A Qt-class native application framework is a desktop requirement.** Qt
   itself is optional compatibility software. `apps/libwidget.trit` is the seed
   of a modular Trit-native framework for third-party desktop applications.
5. **The monolithic kernel may produce multiple system profiles.** Desktop,
   server, appliance, embedded, and single-application/unikernel images should
   share contracts and source where practical, selecting capabilities at build
   and image-composition time.
6. **A hosted device is not a native driver.** Host file, socket, SDL, and audio
   adapters are valuable virtual devices, but eventual native hardware also
   requires MMIO, DMA, interrupts, discovery, and physical controller drivers.

## Track A: vector, matrix, and low-precision acceleration

### Current foundation

- `ternary_simd.h` contains optional AVX2 batch acceleration.
- The guest ISA already has vector semantics and an architectural vector length.
- CUDA/SYCL and transformer runtime work provide additional host-side execution
  evidence.

### Required direction

The reference interpreter and scalar/native ternary operations define results.
Every accelerator backend must be differential-tested against them. Backend
selection is runtime- or build-time policy and must retain a correct fallback.

The guest-visible accelerator contract should eventually cover:

- capability discovery and versioning;
- vector length independent of AVX2, AVX-512, or a particular matrix tile;
- ternary dot product and matrix multiply-accumulate;
- explicit accumulator width and rounding;
- conversions at named representation boundaries;
- saturation, invalid-value, exception, and tail-lane behavior;
- save/restore and ABI rules for accelerator state.

AVX and ACE are host lowering targets. A future compiler may map the same Trit
operation to AVX2, AVX-512, ACE, a GPU kernel, FPGA logic, or an ASIC unit.
Vendor capabilities must never leak into portable guest binaries without an
explicit optional feature contract.

NVFP4 is likewise an interoperability and backend format, not a replacement for
the native ternary scalar model. If supported, it should enter through explicit
tensor load/store and conversion operations, with accuracy, range, scale/block
metadata, and deterministic reference tests. Benchmarks must distinguish native
ternary computation from conversion-plus-NVFP4 execution.

### Acceptance gates

- One semantic operation produces equivalent results across the reference,
  portable batch, and every enabled accelerator backend.
- Unsupported hardware falls back or rejects the feature before execution.
- Benchmarks report conversion cost separately from kernel cost.
- No new branded opcode is accepted without a portable semantic justification.

## Track B: modular system profiles and unikernel derivation

The existing monolithic kernel is not an obstacle to modular products. The
important distinction is between a monolithic runtime address space and a
monolithic, inseparable build.

Introduce a versioned **system profile manifest** that selects:

- kernel subsystems and drivers;
- process, memory, and framebuffer limits;
- filesystems and network services;
- GUI/window services;
- bundled applications and PID 1;
- debug, tracing, and recovery facilities;
- required ISA and device capabilities.

Initial profiles should be:

| Profile | Intended surface |
|---|---|
| `desktop` | Window system, TritUI, shell, networking, storage, bundled apps |
| `server` | Process/VFS/network services without desktop dependencies |
| `embedded` | Static devices and a small application/service set |
| `appliance` | One product workflow with selected services |
| `unikernel` | One application linked with the minimum runtime and kernel surface |

The first unikernel should be a derived build/profile, not a permanent fork. A
fork becomes reasonable only when its execution or protection model must diverge
from the shared contracts. Static linking, dead-code elimination, capability
closure, and generated syscall/device tables should remove unused components.

### Acceptance gates

- Profiles are reproducible and recorded in `.tboot` metadata.
- A capability-closure check rejects missing dependencies before image creation.
- Desktop and minimal profiles pass the same ABI tests for shared facilities.
- A minimal single-application image boots without desktop, shell, or unrelated
  drivers and reports exactly which capabilities were included.

## Track C: TritUI, the native desktop application framework

A window manager is insufficient as an application-development platform.
General-purpose desktop status requires a supported framework so applications do
not independently implement layout, input, text, focus, themes, and accessibility.

`apps/libwidget.trit` already supplies widgets, focus, selection, themes, dirty
regions, and text/pixel drawing. It should evolve into a modular framework,
provisionally called **TritUI**:

| Module | Responsibility |
|---|---|
| `tritui-core` | Application object, event loop, timers, ownership, callbacks/signals |
| `tritui-gui` | Windows, input, clipboard, drawing, images, fonts, scaling |
| `tritui-layout` | Horizontal, vertical, grid, stack, constraints and measurement |
| `tritui-widgets` | Standard controls, menus, dialogs, tabs, trees and tables |
| `tritui-models` | List/tree/table models, selection and data binding |
| `tritui-accessibility` | Roles, names, navigation, contrast and assistive hooks |
| `tritui-tools` | Resource compiler, UI markup compiler, inspector and later designer |

The kernel and window ABI must remain toolkit neutral. TritUI belongs in user
space and should be reducible so appliance and unikernel images include only the
used modules.

Qt may later be ported as a compatibility workload after a sufficient C++ ABI,
runtime, and platform plugin exist. Tkinter is not a native foundation: after a
Python port, a Python binding to TritUI is preferable to requiring CPython,
Tcl, Tk, and a second window backend.

### Acceptance gates

- A third-party application can create a responsive resizable window without
  drawing raw framebuffer cells.
- Layout, focus, keyboard navigation, themes, accessibility metadata, clipboard,
  and standard dialogs have stable SDK contracts.
- Framework modules can be omitted from non-desktop profiles.
- Bundled applications migrate incrementally; direct framebuffer access remains
  available for specialized software but is not the normal application API.

## Track D: binary interoperability and physical device software

Conventional interfaces pose the same category of problem as UTF-8 and ASCII:
their external representation remains exact, while Trit requires explicit
software to encode, decode, transport, and expose native objects.

### Binary interoperability contract

Define compiler and ABI support for:

- exact-width `u8`, `u16`, `u32`, and `u64` bit patterns;
- endian-qualified values and network byte order;
- packed structures, masks, shifts, and checked conversions;
- byte-addressed binary buffers separate from ordinary T40 numeric semantics;
- volatile MMIO loads/stores and device memory fences;
- binary bus/physical addresses;
- CRC, checksum, UTF, pixel, PCM, packet, and descriptor codecs;
- invalid dual-rail and padding behavior at every bridge.

These primitives should form a shared `libbinary`/`libinterop` surface rather
than private helpers duplicated in every driver.

### Native device architecture

The kernel/HAL work must then add:

1. a hierarchical, versioned device graph populated from the Trit DTB, standard
   Device Tree, ACPI, and enumerable buses as appropriate;
2. a driver binding/lifecycle model with probe, start, suspend, resume, reset,
   removal, and error reporting;
3. a general interrupt controller abstraction, including masking, priorities,
   shared IRQs, and message-signaled interrupts;
4. DMA-safe allocation, pinned pages, scatter/gather, cache-coherency rules,
   bounce buffers, and IOMMU hooks;
5. class interfaces for block, network, display, input, sound, serial buses, and
   GPIO;
6. virtual devices whose VM and FPGA implementations obey the same contract.

Recommended implementation order:

1. UART, timer, interrupt controller, binary MMIO, and binary buffers;
2. virtual block, network, input, framebuffer, and audio devices;
3. DMA, IOMMU hooks, fault injection, and deterministic device replay;
4. GPIO, I2C/SMBus, SPI, and I2S for embedded bring-up;
5. PCIe enumeration, BARs, MSI/MSI-X, bridges, and hotplug;
6. NVMe and one documented Ethernet controller;
7. USB xHCI, hubs, and keyboard/mouse/storage/audio class drivers;
8. platform display/audio controllers and power management;
9. SATA/AHCI where required;
10. Wi-Fi, Bluetooth, SAS, Thunderbolt/USB4, and complex GPU support after the
    foundational bus, security, firmware, and hotplug models are proven.

Hosted execution may continue translating virtual devices into host files,
sockets, SDL, and audio APIs. Bare-metal host runtimes and native ternary
hardware require separate lower adapters but should expose the same guest device
contract.

### Acceptance gates

- Binary protocol fixtures round-trip without numeric reinterpretation.
- A simulated device and a hardware/FPGA device pass the same driver contract.
- DMA cannot address memory outside its granted mapping.
- Driver timeout, reset, malformed descriptor, dropped interrupt, and hot-remove
  paths are deterministic and testable.
- Socket, VFS, GUI, and audio APIs are tested above real class drivers rather
  than only in-memory facades.

## Dependency order

The tracks are related but should not become one unbounded implementation:

1. Freeze exact binary data, MMIO, DMA, interrupt, capability, and profile
   contracts.
2. Build virtual devices and profile-aware images so work remains testable on
   ordinary hosts.
3. Stabilize TritUI Core, layout, drawing/text, and standard controls above the
   existing window ABI.
4. Add embedded buses and then PCIe/NVMe/Ethernet/USB physical drivers.
5. Add a minimal single-application profile and prove capability pruning.
6. Extend matrix/low-precision backends only behind differential reference
   tests; add ACE/NVFP4 adapters when accessible hardware and toolchains exist.
7. Consider Qt and Python compatibility ports only after the native ABI and
   platform services they consume are stable.

## Definition of completion

This document establishes scope; it does not close any gap. A track or item is
complete only when its source contract, manifests, implementation, focused
tests, negative tests, diagnostics, and relevant release profile all agree.
`ROADMAP_STATUS.json`, `KNOWN_GAPS.md`, and test results remain authoritative.
