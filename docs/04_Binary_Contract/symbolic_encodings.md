# Symbolic Encodings

This project uses binary-world encodings at host and interchange boundaries,
but the ternary stack should eventually have native symbolic encodings that
align with trit group sizes.

## Current Policy

ASCII, UTF-8, and hexadecimal are compatibility encodings. They are useful for
source files, manifests, diagnostics, terminal text, host tools, and binary
inspection, but they are not the native semantic model of the VM.

The native model remains:

- balanced trits: `-1`, `0`, `+1`
- typed TCL values
- VM words and lanes
- `.tboot` and `.tdisk` binary fields

Use ASCII, UTF-8, decimal, and hexadecimal as projections of those values when
humans or host tooling need stable text.

## ASCII And UTF-8

ASCII should remain the guaranteed minimum text subset for:

- source files and assembler files
- shell text and diagnostics
- path names while the filesystem contract is still young
- test golden output
- manifest strings

UTF-8 should be the external interchange direction for richer text once the OS
and app SDK need non-ASCII symbols. Until then, non-ASCII should be explicit in
docs and tests rather than accidental.

Implemented native work:

- `TASCII-81`: fixed-width 4-trit basic text encoding using the authoritative
  table in `ternary_symbolic_encoding.h`.
- `TUTF`: future variable-width ternary Unicode adapter, analogous in purpose to
  UTF-8 but grouped around trit ranges instead of bytes.

`TASCII-81` should cover control essentials, digits, Latin letters, path
punctuation, shell punctuation, assembler punctuation, and TCL syntax. It should
not block the current ASCII/UTF-8 host boundary.

## Hexadecimal

Hexadecimal is a host/debug notation. It is appropriate for:

- file magics
- checksums
- packed instruction words
- memory dumps
- register dumps
- host-side golden tests
- manifests that describe binary wire fields

Hexadecimal is not the natural compact notation for ternary values. It groups
bits, not trits.

Implemented notation:

- exact balanced trits: `0t+-0++--` (MSB first; `-`, `0`, `+` only)
- three-trit groups: `0z27:<digits>`
- four-trit groups: `0z81:<digits>`
- The current conformance executable exercises the supported host projections;
  no standalone symbolic-dump wrapper is part of the platform tool surface.

The alphabets and the complete 81-entry text table are defined once in
`ternary_symbolic_encoding.h`. Existing decimal and `0x` hexadecimal syntax
retains its original meaning.

## Guest SDK and dump selectors

The allocation-free guest surface is executable in `apps/os_sdk.trit`:

- `os_tascii81_encode_char(ascii)` and `os_tascii81_decode_char(index)` return
  the table index or `-1`.
- `os_symbolic_parse(addr, length, out_trits, capacity)` writes MSB-first
  balanced trits and returns the trit count or a negative error. `0y` remains
  parse-only; formatters emit `0t`.
- `os_symbolic_format(value, format, out_addr, capacity)` selects decimal,
  hexadecimal, `0t`, `0z27:`, or `0z81:` output and never writes partial output
  on a buffer error.

The guest API remains source-level functionality; the consolidated current
validation surface is `tests/current_only_conformance.cpp` plus the CMake
production gate. The retired symbolic dump and image-inspection subcommands
are not part of the current tooling contract.

Useful grouping:

| Group | States | Role |
|-------|--------|------|
| 1 trit | 3 | exact logic |
| 3 trits | 27 | compact digit candidate |
| 4 trits | 81 | fixed basic character candidate |
| 5 trits | 243 | byte-adjacent storage bridge |

## Implementation Direction

1. Keep existing ASCII/UTF-8 and hex behavior stable at host boundaries.
2. Add tests around current decimal, hex, ASCII, and UTF-8 assumptions before
   changing parsers or file formats.
3. Specify `TASCII-81` as a table before using it in guest-visible ABI.
4. Add explicit conversion functions in the app SDK and host tools.
5. Add dump modes to image, memory, and graph tooling that show ternary-native
   notation alongside hex.
6. Only then consider TCL literal syntax for ternary-native text or compact
   ternary numeric notation.

## Non-Goals

- Replacing UTF-8 on host interfaces.
- Replacing hexadecimal in binary inspectors.
- Making current production gates depend on future ternary-native text.
- Treating a compact dump notation as the source of truth.

The source of truth remains the typed value or binary field being displayed.
