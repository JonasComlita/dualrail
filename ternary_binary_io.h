#pragma once

#include <array>
#include <climits>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <vector>

// This header models external binary octets.  It deliberately does not model
// Trit numeric values or ternary lane encodings.
namespace sandbox {
namespace binary {

using U8 = std::uint8_t;
using U16 = std::uint16_t;
using U32 = std::uint32_t;
using U64 = std::uint64_t;
using I32 = std::int32_t;

static_assert(std::numeric_limits<U8>::digits == 8, "U8 must have 8 value bits");
static_assert(std::numeric_limits<U16>::digits == 16, "U16 must have 16 value bits");
static_assert(std::numeric_limits<U32>::digits == 32, "U32 must have 32 value bits");
static_assert(std::numeric_limits<U64>::digits == 64, "U64 must have 64 value bits");
static_assert(sizeof(I32) * CHAR_BIT == 32, "I32 must have 32 storage bits");
static_assert(std::numeric_limits<I32>::digits == 31, "I32 must have 31 magnitude bits");
static_assert(std::numeric_limits<I32>::min() == (-2147483647 - 1),
              "I32 must represent the two's-complement minimum");

using Byte = U8;
using ByteBuffer = std::vector<Byte>;

enum class Endian {
    little,
    big,
};

class ByteView;

class ConstByteView {
public:
    constexpr ConstByteView() noexcept = default;

    // A null pointer produces an empty view, even if size is non-zero.  Empty
    // views canonicalize their data pointer to null.
    constexpr ConstByteView(const Byte* data, std::size_t size) noexcept
        : data_(data == nullptr || size == 0 ? nullptr : data),
          size_(data == nullptr ? 0 : size) {}

    explicit ConstByteView(const ByteBuffer& buffer) noexcept
        : ConstByteView(buffer.data(), buffer.size()) {}

    ConstByteView(const ByteView& view) noexcept;

    constexpr const Byte* data() const noexcept { return data_; }
    constexpr std::size_t size() const noexcept { return size_; }
    constexpr bool empty() const noexcept { return size_ == 0; }

private:
    const Byte* data_ = nullptr;
    std::size_t size_ = 0;
};

class ByteView {
public:
    constexpr ByteView() noexcept = default;

    // A null pointer produces an empty view, even if size is non-zero.  Empty
    // views canonicalize their data pointer to null.
    constexpr ByteView(Byte* data, std::size_t size) noexcept
        : data_(data == nullptr || size == 0 ? nullptr : data),
          size_(data == nullptr ? 0 : size) {}

    explicit ByteView(ByteBuffer& buffer) noexcept
        : ByteView(buffer.data(), buffer.size()) {}

    constexpr Byte* data() const noexcept { return data_; }
    constexpr std::size_t size() const noexcept { return size_; }
    constexpr bool empty() const noexcept { return size_ == 0; }

private:
    Byte* data_ = nullptr;
    std::size_t size_ = 0;
};

inline ConstByteView::ConstByteView(const ByteView& view) noexcept
    : ConstByteView(view.data(), view.size()) {}

namespace detail {

inline bool has_range(std::size_t size, std::size_t offset,
                      std::size_t width) noexcept {
    return offset <= size && width <= size - offset;
}

template <typename UInt>
inline void encode_unchecked(Byte* output, Endian endian, UInt value) noexcept {
    for (std::size_t index = 0; index < sizeof(UInt); ++index) {
        const std::size_t destination = endian == Endian::little
            ? index
            : sizeof(UInt) - 1 - index;
        output[destination] = static_cast<Byte>(value >> (index * 8));
    }
}

template <typename UInt>
inline UInt decode_unchecked(const Byte* input, Endian endian) noexcept {
    UInt value = 0;
    for (std::size_t index = 0; index < sizeof(UInt); ++index) {
        const std::size_t source = endian == Endian::little
            ? index
            : sizeof(UInt) - 1 - index;
        value |= static_cast<UInt>(static_cast<UInt>(input[source]) << (index * 8));
    }
    return value;
}

template <typename UInt>
inline bool encode(ByteView output, std::size_t offset, Endian endian,
                   UInt value) noexcept {
    if (!has_range(output.size(), offset, sizeof(UInt))) {
        return false;
    }
    encode_unchecked(output.data() + offset, endian, value);
    return true;
}

template <typename UInt>
inline bool decode(ConstByteView input, std::size_t offset, Endian endian,
                   UInt& output) noexcept {
    if (!has_range(input.size(), offset, sizeof(UInt))) {
        return false;
    }
    const UInt decoded = decode_unchecked<UInt>(input.data() + offset, endian);
    output = decoded;
    return true;
}

template <typename UInt>
inline void append(ByteBuffer& output, Endian endian, UInt value) {
    std::array<Byte, sizeof(UInt)> encoded{};
    encode_unchecked(encoded.data(), endian, value);
    output.insert(output.end(), encoded.begin(), encoded.end());
}

inline U32 i32_to_twos_complement(I32 value) noexcept {
    if (value >= 0) {
        return static_cast<U32>(value);
    }
    const U32 sign_bit = static_cast<U32>(std::numeric_limits<I32>::max()) + U32{1};
    return sign_bit + static_cast<U32>(value - std::numeric_limits<I32>::min());
}

inline I32 i32_from_twos_complement(U32 bits) noexcept {
    const U32 signed_max = static_cast<U32>(std::numeric_limits<I32>::max());
    if (bits <= signed_max) {
        return static_cast<I32>(bits);
    }
    const U32 sign_bit = signed_max + U32{1};
    const I32 offset = static_cast<I32>(bits - sign_bit);
    return std::numeric_limits<I32>::min() + offset;
}

}  // namespace detail

inline bool encode_u8(ByteView output, std::size_t offset, Endian endian,
                      U8 value) noexcept {
    return detail::encode(output, offset, endian, value);
}

inline bool encode_u16(ByteView output, std::size_t offset, Endian endian,
                       U16 value) noexcept {
    return detail::encode(output, offset, endian, value);
}

inline bool encode_u32(ByteView output, std::size_t offset, Endian endian,
                       U32 value) noexcept {
    return detail::encode(output, offset, endian, value);
}

inline bool encode_u64(ByteView output, std::size_t offset, Endian endian,
                       U64 value) noexcept {
    return detail::encode(output, offset, endian, value);
}

inline bool encode_i32(ByteView output, std::size_t offset, Endian endian,
                       I32 value) noexcept {
    return detail::encode(output, offset, endian,
                          detail::i32_to_twos_complement(value));
}

inline bool decode_u8(ConstByteView input, std::size_t offset, Endian endian,
                      U8& output) noexcept {
    return detail::decode(input, offset, endian, output);
}

inline bool decode_u16(ConstByteView input, std::size_t offset, Endian endian,
                       U16& output) noexcept {
    return detail::decode(input, offset, endian, output);
}

inline bool decode_u32(ConstByteView input, std::size_t offset, Endian endian,
                       U32& output) noexcept {
    return detail::decode(input, offset, endian, output);
}

inline bool decode_u64(ConstByteView input, std::size_t offset, Endian endian,
                       U64& output) noexcept {
    return detail::decode(input, offset, endian, output);
}

inline bool decode_i32(ConstByteView input, std::size_t offset, Endian endian,
                       I32& output) noexcept {
    U32 bits = 0;
    if (!detail::decode(input, offset, endian, bits)) {
        return false;
    }
    output = detail::i32_from_twos_complement(bits);
    return true;
}

inline bool narrow_u8(U64 value, U8& output) noexcept {
    if (value > static_cast<U64>(std::numeric_limits<U8>::max())) {
        return false;
    }
    output = static_cast<U8>(value);
    return true;
}

inline bool narrow_u16(U64 value, U16& output) noexcept {
    if (value > static_cast<U64>(std::numeric_limits<U16>::max())) {
        return false;
    }
    output = static_cast<U16>(value);
    return true;
}

inline bool narrow_u32(U64 value, U32& output) noexcept {
    if (value > static_cast<U64>(std::numeric_limits<U32>::max())) {
        return false;
    }
    output = static_cast<U32>(value);
    return true;
}

class Reader {
public:
    constexpr Reader() noexcept = default;
    explicit constexpr Reader(ConstByteView input) noexcept : input_(input) {}

    constexpr std::size_t position() const noexcept { return position_; }
    constexpr std::size_t remaining() const noexcept {
        return input_.size() - position_;
    }

    bool read_u8(Endian endian, U8& output) noexcept {
        return read(output, endian, decode_u8, sizeof(U8));
    }

    bool read_u16(Endian endian, U16& output) noexcept {
        return read(output, endian, decode_u16, sizeof(U16));
    }

    bool read_u32(Endian endian, U32& output) noexcept {
        return read(output, endian, decode_u32, sizeof(U32));
    }

    bool read_u64(Endian endian, U64& output) noexcept {
        return read(output, endian, decode_u64, sizeof(U64));
    }

    bool read_i32(Endian endian, I32& output) noexcept {
        return read(output, endian, decode_i32, sizeof(I32));
    }

private:
    template <typename UInt>
    bool read(UInt& output, Endian endian,
              bool (*decode)(ConstByteView, std::size_t, Endian, UInt&) noexcept,
              std::size_t width) noexcept {
        UInt decoded{};
        if (!decode(input_, position_, endian, decoded)) {
            return false;
        }
        position_ += width;
        output = decoded;
        return true;
    }

    ConstByteView input_;
    std::size_t position_ = 0;
};

class Writer {
public:
    explicit Writer(ByteBuffer& output) noexcept : output_(output) {}

    std::size_t size() const noexcept { return output_.size(); }

    void append_u8(Endian endian, U8 value) {
        detail::append(output_, endian, value);
    }

    void append_u16(Endian endian, U16 value) {
        detail::append(output_, endian, value);
    }

    void append_u32(Endian endian, U32 value) {
        detail::append(output_, endian, value);
    }

    void append_u64(Endian endian, U64 value) {
        detail::append(output_, endian, value);
    }

    void append_i32(Endian endian, I32 value) {
        detail::append(output_, endian, detail::i32_to_twos_complement(value));
    }

    bool write_u8(std::size_t offset, Endian endian, U8 value) noexcept {
        return encode_u8(ByteView(output_), offset, endian, value);
    }

    bool write_u16(std::size_t offset, Endian endian, U16 value) noexcept {
        return encode_u16(ByteView(output_), offset, endian, value);
    }

    bool write_u32(std::size_t offset, Endian endian, U32 value) noexcept {
        return encode_u32(ByteView(output_), offset, endian, value);
    }

    bool write_u64(std::size_t offset, Endian endian, U64 value) noexcept {
        return encode_u64(ByteView(output_), offset, endian, value);
    }

    bool write_i32(std::size_t offset, Endian endian, I32 value) noexcept {
        return encode_i32(ByteView(output_), offset, endian, value);
    }

private:
    ByteBuffer& output_;
};

}  // namespace binary
}  // namespace sandbox
