# Exact-width binary interoperability

`ternary_binary_io.h` is a small, header-only C++17 host/reference layer for
external binary data.  It supplies `sandbox::binary::Byte`, `ByteBuffer`,
exact-width unsigned aliases plus `I32`, endian codecs, bounded views, a
cursor reader, and an append/indexed writer.  Its purpose is to make a
byte-level format contract explicit before later code consumes or produces
external data.

## Scope and separation

`Byte` is an octet (`std::uint8_t`), and `ByteBuffer` is a sequence of octets.
They are not `T40` or `T50` numeric values, ternary lane encodings, or an
implicit conversion boundary.  The layer also has no meaning for L1/L50
storage, virtual-machine values, or guest ABI representations.  A caller must
perform any required format conversion deliberately at its own boundary.

This layer does not define or change an ABI, image format, encryption format,
device interface, or guest-facing API.  It intentionally omits MMIO, memory
ordering, DMA, bus addressing, interrupts, packed descriptors, checksums,
media formats, packets, and text codecs.  It does not discover or report host
byte order.

## Byte order

`Endian::little` and `Endian::big` describe the external byte sequence only.
`encode_u8`, `encode_u16`, `encode_u32`, `encode_u64`, and `encode_i32` write
a value at a specified byte offset; matching `decode_*` functions recover it.
Each takes an explicit endian argument, including the one-byte operation for
regular call sites.  Their output is constructed by shifts and masks, so it is
independent of the host architecture.  They do not use packed structs, unions,
pointer punning, or serialization through casts.

`I32` is exactly 32 storage bits. `encode_i32` writes its externally defined
32-bit two's-complement bit pattern, and `decode_i32` maps that pattern back to
an `I32` using representable signed intermediates; it does not rely on an
overflowing unsigned-to-signed conversion.  This signed representation is an
external binary rule, not a ternary numeric encoding.

```cpp
using namespace sandbox::binary;

ByteBuffer bytes(4);
encode_u32(ByteView(bytes), 0, Endian::big, 0x12345678U);
// bytes is {0x12, 0x34, 0x56, 0x78}

U32 value = 0;
decode_u32(ConstByteView(bytes), 0, Endian::big, value);
// value is 0x12345678U
```

## Views and checked operations

`ConstByteView` and `ByteView` are non-owning C++17 views.  A default view is
empty with a null data pointer.  Constructing either view with a null pointer
also produces an empty view, even if a non-zero size was passed; zero-size
views likewise have a null data pointer.  This makes empty and null inputs
safe to inspect without dereferencing a pointer.

All indexed codec calls return `false` when their complete fixed-width range is
not available.  On that failure, decoder output remains unchanged and encoder
output remains unchanged.  `Writer::write_u*` and `Writer::write_i32` use those
checked indexed calls.  `Writer::append_i32` appends the same explicit signed
encoding.
`narrow_u8`, `narrow_u16`, and `narrow_u32` similarly return `false` and leave
their output unchanged when a `U64` cannot be represented.

`Reader` owns a view and a cursor.  A failed `read_u*` or `read_i32` is failure-atomic: it
does not advance the cursor and does not modify the output argument.  Parse
failures therefore use return values rather than exceptions.  `Writer` appends
the requested externally ordered bytes; normal `ByteBuffer` allocation may
still throw according to the C++ standard library.

```cpp
ByteBuffer message;
Writer writer(message);
writer.append_u8(Endian::little, 0xAB);
writer.append_u16(Endian::big, 0x1234);

Reader reader{ConstByteView(message)};
U8 tag = 0;
U16 length = 0;
if (reader.read_u8(Endian::little, tag) &&
    reader.read_u16(Endian::big, length)) {
    // tag == 0xAB and length == 0x1234
}
```

## First adopted boundary: `.tboot` v3

The current `.tboot` v3 writer and reader are the first consumer of this layer.
They encode the existing 24-byte header and every v3 payload scalar with an
explicit `Endian::little` codec: unsigned fields use their fixed-width unsigned
representation and `int32` fields use the `I32` two's-complement rule above.
This preserves the already-declared `.tboot` v3 bytes, field order, version,
and validation behavior without deriving them from host object layout.  It does
not adopt, migrate, or make a completion claim for `.tdisk`, checkpoint files,
or any device/physical-driver boundary.

## Dependent packets

Further packets can adopt this layer at a specifically documented external
format boundary, with focused golden-byte tests. Candidate follow-ups are
additional host-tool format adapters, separately specified checksums or text
encodings where needed, and later storage/device contracts. Those packets must
define their own semantics and must not make this foundational layer a proxy
for ternary numeric, L1/L50, guest, or hardware behavior.
