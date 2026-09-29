#include "ternary_binary_io.h"

#include <climits>
#include <cstdio>
#include <initializer_list>
#include <limits>

namespace {

using sandbox::binary::Byte;
using sandbox::binary::ByteBuffer;
using sandbox::binary::ByteView;
using sandbox::binary::ConstByteView;
using sandbox::binary::Endian;
using sandbox::binary::I32;
using sandbox::binary::Reader;
using sandbox::binary::U8;
using sandbox::binary::U16;
using sandbox::binary::U32;
using sandbox::binary::U64;
using sandbox::binary::Writer;

static_assert(std::numeric_limits<U8>::digits == 8, "U8 width");
static_assert(std::numeric_limits<U16>::digits == 16, "U16 width");
static_assert(std::numeric_limits<U32>::digits == 32, "U32 width");
static_assert(std::numeric_limits<U64>::digits == 64, "U64 width");
static_assert(sizeof(I32) * CHAR_BIT == 32, "I32 width");

bool fail(const char* expression, int line) {
    std::fprintf(stderr, "FAILED line %d: %s\n", line, expression);
    return false;
}

#define REQUIRE(expression) \
    do { \
        if (!(expression)) { \
            return fail(#expression, __LINE__); \
        } \
    } while (false)

bool equals(const ByteBuffer& actual, std::initializer_list<Byte> expected) {
    if (actual.size() != expected.size()) {
        return false;
    }
    std::size_t index = 0;
    for (Byte byte : expected) {
        if (actual[index++] != byte) {
            return false;
        }
    }
    return true;
}

template <typename UInt, typename Read>
bool check_truncation(std::size_t width, UInt sentinel, Read read) {
    for (Endian endian : {Endian::little, Endian::big}) {
        for (std::size_t size = 0; size < width; ++size) {
            ByteBuffer bytes(size, static_cast<Byte>(0xA5));
            Reader reader{ConstByteView(bytes)};
            UInt output = sentinel;
            REQUIRE(!read(reader, endian, output));
            REQUIRE(reader.position() == 0);
            REQUIRE(reader.remaining() == size);
            REQUIRE(output == sentinel);
        }
    }
    return true;
}

bool test_golden_layouts() {
    ByteBuffer bytes(8, 0);

    REQUIRE(sandbox::binary::encode_u8(ByteView(bytes), 0, Endian::little, 0xA5));
    REQUIRE(equals(ByteBuffer{bytes[0]}, {0xA5}));
    REQUIRE(sandbox::binary::encode_u8(ByteView(bytes), 0, Endian::big, 0xA5));
    REQUIRE(equals(ByteBuffer{bytes[0]}, {0xA5}));

    REQUIRE(sandbox::binary::encode_u16(ByteView(bytes), 0, Endian::little, 0x1234));
    REQUIRE(equals(ByteBuffer{bytes[0], bytes[1]}, {0x34, 0x12}));
    REQUIRE(sandbox::binary::encode_u16(ByteView(bytes), 0, Endian::big, 0x1234));
    REQUIRE(equals(ByteBuffer{bytes[0], bytes[1]}, {0x12, 0x34}));

    REQUIRE(sandbox::binary::encode_u32(ByteView(bytes), 0, Endian::little, 0x12345678U));
    REQUIRE(equals(ByteBuffer{bytes[0], bytes[1], bytes[2], bytes[3]},
                   {0x78, 0x56, 0x34, 0x12}));
    REQUIRE(sandbox::binary::encode_u32(ByteView(bytes), 0, Endian::big, 0x12345678U));
    REQUIRE(equals(ByteBuffer{bytes[0], bytes[1], bytes[2], bytes[3]},
                   {0x12, 0x34, 0x56, 0x78}));

    REQUIRE(sandbox::binary::encode_i32(ByteView(bytes), 0, Endian::little,
                                         static_cast<I32>(-123456789)));
    REQUIRE(equals(ByteBuffer{bytes[0], bytes[1], bytes[2], bytes[3]},
                   {0xEB, 0x32, 0xA4, 0xF8}));
    REQUIRE(sandbox::binary::encode_i32(ByteView(bytes), 0, Endian::big,
                                         static_cast<I32>(-123456789)));
    REQUIRE(equals(ByteBuffer{bytes[0], bytes[1], bytes[2], bytes[3]},
                   {0xF8, 0xA4, 0x32, 0xEB}));

    REQUIRE(sandbox::binary::encode_u64(ByteView(bytes), 0, Endian::little,
                                         0x0123456789ABCDEFULL));
    REQUIRE(equals(bytes, {0xEF, 0xCD, 0xAB, 0x89, 0x67, 0x45, 0x23, 0x01}));
    REQUIRE(sandbox::binary::encode_u64(ByteView(bytes), 0, Endian::big,
                                         0x0123456789ABCDEFULL));
    REQUIRE(equals(bytes, {0x01, 0x23, 0x45, 0x67, 0x89, 0xAB, 0xCD, 0xEF}));
    return true;
}

bool test_round_trips() {
    for (Endian endian : {Endian::little, Endian::big}) {
        for (U8 value : {static_cast<U8>(0), std::numeric_limits<U8>::max(),
                         static_cast<U8>(0xA5)}) {
            ByteBuffer bytes(sizeof(U8));
            U8 decoded = 0;
            REQUIRE(sandbox::binary::encode_u8(ByteView(bytes), 0, endian, value));
            REQUIRE(sandbox::binary::decode_u8(ConstByteView(bytes), 0, endian, decoded));
            REQUIRE(decoded == value);
        }
        for (U16 value : {static_cast<U16>(0), std::numeric_limits<U16>::max(),
                          static_cast<U16>(0xA5C3)}) {
            ByteBuffer bytes(sizeof(U16));
            U16 decoded = 0;
            REQUIRE(sandbox::binary::encode_u16(ByteView(bytes), 0, endian, value));
            REQUIRE(sandbox::binary::decode_u16(ConstByteView(bytes), 0, endian, decoded));
            REQUIRE(decoded == value);
        }
        for (U32 value : {static_cast<U32>(0), std::numeric_limits<U32>::max(),
                          static_cast<U32>(0xA5C30F5AU)}) {
            ByteBuffer bytes(sizeof(U32));
            U32 decoded = 0;
            REQUIRE(sandbox::binary::encode_u32(ByteView(bytes), 0, endian, value));
            REQUIRE(sandbox::binary::decode_u32(ConstByteView(bytes), 0, endian, decoded));
            REQUIRE(decoded == value);
        }
        for (I32 value : {static_cast<I32>(0), std::numeric_limits<I32>::min(),
                          std::numeric_limits<I32>::max(), static_cast<I32>(-1),
                          static_cast<I32>(-123456789)}) {
            ByteBuffer bytes(sizeof(I32));
            I32 decoded = 0;
            REQUIRE(sandbox::binary::encode_i32(ByteView(bytes), 0, endian, value));
            REQUIRE(sandbox::binary::decode_i32(ConstByteView(bytes), 0, endian, decoded));
            REQUIRE(decoded == value);
        }
        for (U64 value : {static_cast<U64>(0), std::numeric_limits<U64>::max(),
                          static_cast<U64>(0xA5C30F5A6996F00DULL)}) {
            ByteBuffer bytes(sizeof(U64));
            U64 decoded = 0;
            REQUIRE(sandbox::binary::encode_u64(ByteView(bytes), 0, endian, value));
            REQUIRE(sandbox::binary::decode_u64(ConstByteView(bytes), 0, endian, decoded));
            REQUIRE(decoded == value);
        }
    }
    return true;
}

bool test_sequential_reader_and_writer() {
    ByteBuffer bytes;
    Writer writer(bytes);
    writer.append_u8(Endian::little, 0xAB);
    writer.append_u16(Endian::big, 0x1234);
    writer.append_u32(Endian::little, 0x10203040U);
    writer.append_i32(Endian::big, static_cast<I32>(-2));
    writer.append_u64(Endian::big, 0x0123456789ABCDEFULL);
    REQUIRE(equals(bytes, {0xAB, 0x12, 0x34, 0x40, 0x30, 0x20, 0x10,
                           0xFF, 0xFF, 0xFF, 0xFE,
                           0x01, 0x23, 0x45, 0x67, 0x89, 0xAB, 0xCD, 0xEF}));

    Reader reader{ConstByteView(bytes)};
    U8 first = 0;
    U16 second = 0;
    U32 third = 0;
    I32 signed_value = 0;
    U64 fourth = 0;
    REQUIRE(reader.read_u8(Endian::big, first));
    REQUIRE(reader.read_u16(Endian::big, second));
    REQUIRE(reader.read_u32(Endian::little, third));
    REQUIRE(reader.read_i32(Endian::big, signed_value));
    REQUIRE(reader.read_u64(Endian::big, fourth));
    REQUIRE(first == 0xAB);
    REQUIRE(second == 0x1234);
    REQUIRE(third == 0x10203040U);
    REQUIRE(signed_value == -2);
    REQUIRE(fourth == 0x0123456789ABCDEFULL);
    REQUIRE(reader.position() == bytes.size());
    REQUIRE(reader.remaining() == 0);
    return true;
}

bool test_truncation_atomicity() {
    REQUIRE(check_truncation<U8>(sizeof(U8), 0x7E,
        [](Reader& reader, Endian endian, U8& output) {
            return reader.read_u8(endian, output);
        }));
    REQUIRE(check_truncation<U16>(sizeof(U16), 0x7E7E,
        [](Reader& reader, Endian endian, U16& output) {
            return reader.read_u16(endian, output);
        }));
    REQUIRE(check_truncation<U32>(sizeof(U32), 0x7E7E7E7EU,
        [](Reader& reader, Endian endian, U32& output) {
            return reader.read_u32(endian, output);
        }));
    REQUIRE(check_truncation<I32>(sizeof(I32), static_cast<I32>(-123456789),
        [](Reader& reader, Endian endian, I32& output) {
            return reader.read_i32(endian, output);
        }));
    REQUIRE(check_truncation<U64>(sizeof(U64), 0x7E7E7E7E7E7E7E7EULL,
        [](Reader& reader, Endian endian, U64& output) {
            return reader.read_u64(endian, output);
        }));

    ByteBuffer partial{0xAB, 0x12, 0x34};
    Reader partially_consumed{ConstByteView(partial)};
    U8 tag = 0;
    U32 unchanged = 0x7E7E7E7EU;
    REQUIRE(partially_consumed.read_u8(Endian::little, tag));
    REQUIRE(!partially_consumed.read_u32(Endian::big, unchanged));
    REQUIRE(partially_consumed.position() == 1);
    REQUIRE(unchanged == 0x7E7E7E7EU);
    return true;
}

bool test_failed_indexed_writes_are_atomic() {
    ByteBuffer bytes(8, static_cast<Byte>(0x7E));
    const ByteBuffer original = bytes;
    Writer writer(bytes);
    REQUIRE(!writer.write_u8(8, Endian::little, 0x01));
    REQUIRE(bytes == original);
    REQUIRE(!writer.write_u16(7, Endian::big, 0x0102));
    REQUIRE(bytes == original);
    REQUIRE(!writer.write_u32(6, Endian::little, 0x01020304U));
    REQUIRE(bytes == original);
    REQUIRE(!writer.write_u64(1, Endian::big, 0x0102030405060708ULL));
    REQUIRE(bytes == original);
    REQUIRE(!writer.write_i32(6, Endian::little, static_cast<I32>(-2)));
    REQUIRE(bytes == original);
    REQUIRE(!writer.write_u64(std::numeric_limits<std::size_t>::max(), Endian::big,
                              0x0102030405060708ULL));
    REQUIRE(bytes == original);
    return true;
}

bool test_checked_narrowing() {
    U8 u8 = 0;
    U16 u16 = 0;
    U32 u32 = 0;
    REQUIRE(sandbox::binary::narrow_u8(0xA5, u8));
    REQUIRE(u8 == 0xA5);
    u8 = 0x7E;
    REQUIRE(!sandbox::binary::narrow_u8(0x100, u8));
    REQUIRE(u8 == 0x7E);

    REQUIRE(sandbox::binary::narrow_u16(0xA5C3, u16));
    REQUIRE(u16 == 0xA5C3);
    u16 = 0x7E7E;
    REQUIRE(!sandbox::binary::narrow_u16(0x10000, u16));
    REQUIRE(u16 == 0x7E7E);

    REQUIRE(sandbox::binary::narrow_u32(0xA5C30F5AU, u32));
    REQUIRE(u32 == 0xA5C30F5AU);
    u32 = 0x7E7E7E7EU;
    REQUIRE(!sandbox::binary::narrow_u32(0x100000000ULL, u32));
    REQUIRE(u32 == 0x7E7E7E7EU);
    return true;
}

bool test_empty_views() {
    const ByteBuffer empty_buffer;
    ConstByteView empty;
    ConstByteView null_with_size(nullptr, 8);
    ConstByteView from_empty(empty_buffer);
    ByteView mutable_empty;
    ByteView mutable_null_with_size(nullptr, 8);
    REQUIRE(empty.empty() && empty.data() == nullptr);
    REQUIRE(null_with_size.empty() && null_with_size.data() == nullptr);
    REQUIRE(from_empty.empty() && from_empty.data() == nullptr);
    REQUIRE(mutable_empty.empty() && mutable_empty.data() == nullptr);
    REQUIRE(mutable_null_with_size.empty() && mutable_null_with_size.data() == nullptr);

    U16 output = 0x7E7E;
    Reader reader(empty);
    REQUIRE(!reader.read_u16(Endian::little, output));
    REQUIRE(reader.position() == 0);
    REQUIRE(output == 0x7E7E);
    REQUIRE(!sandbox::binary::encode_u8(mutable_empty, 0, Endian::big, 0x01));
    return true;
}

}  // namespace

int main() {
    if (!test_golden_layouts() || !test_round_trips() ||
        !test_sequential_reader_and_writer() || !test_truncation_atomicity() ||
        !test_failed_indexed_writes_are_atomic() || !test_checked_narrowing() ||
        !test_empty_views()) {
        return 1;
    }
    std::puts("test_binary_interop: all checks passed");
    return 0;
}
