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
- `python tools/trit_tool.py symbolic dump <literal>` prints host hex and all
  three ternary-native forms.

The alphabets and the complete 81-entry text table are defined once in
`ternary_symbolic_encoding.h` and mirrored by `tools/trit_symbolic.py`.
Existing decimal and `0x` hexadecimal syntax retains its original meaning.

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
