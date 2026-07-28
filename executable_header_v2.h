#pragma once

// Included by ternary_vm_state.h after the legacy executable-header codec.

static constexpr int EXEC_V2_HEADER_WORDS =
    architecture::v2::EXECUTABLE_HEADER_WORDS;
static constexpr int EXEC_V2_MAGIC = EXEC_MAGIC;

static constexpr int EXEC_V2_MAGIC_INDEX = 0;
static constexpr int EXEC_V2_EXECUTABLE_VERSION = 1;
static constexpr int EXEC_V2_FUNCTION_ABI_VERSION = 2;
static constexpr int EXEC_V2_HEADER_SIZE = 3;
static constexpr int EXEC_V2_ISA_VERSION = 4;
static constexpr int EXEC_V2_REQUIRED_FEATURES = 5;
static constexpr int EXEC_V2_ENTRY_PC = 6;
static constexpr int EXEC_V2_TEXT_WORDS = 7;
static constexpr int EXEC_V2_DATA_WORDS = 8;
static constexpr int EXEC_V2_STACK_WORDS = 9;
static constexpr int EXEC_V2_SYSCALL_ABI_VERSION = 10;
static constexpr int EXEC_V2_SCALAR_WIDTH = 11;
static constexpr int EXEC_V2_BASE_PAGE_WORDS = 12;
static constexpr int EXEC_V2_FLAGS = 13;
static constexpr int EXEC_V2_CHECKSUM = 14;

struct ExecutableImageHeaderV2 {
    int header_addr = -1;
    int magic = EXEC_V2_MAGIC;
    int executable_version = architecture::v2::EXECUTABLE_VERSION;
    int function_abi_version = architecture::v2::FUNCTION_ABI_VERSION;
    int header_words = EXEC_V2_HEADER_WORDS;
    int isa_version = architecture::v2::ISA_VERSION;
    std::uint64_t required_features =
        featureBit(architecture::v2::FEATURE_BASE_V2);
    int entry_pc = 0;
    int text_words = 0;
    int data_words = 0;
    int stack_words = architecture::v2::STACK_ALIGNMENT_WORDS;
    int syscall_abi_version = architecture::v2::SYSCALL_ABI_VERSION;
    int scalar_word_trits = architecture::v2::SCALAR_WORD_TRITS;
    int base_page_words = architecture::v2::BASE_PAGE_WORDS;
    int flags = 0;
    long long header_checksum = 0;
};

struct ExecutableArchitectureIdentity {
    int executable_version = 1;
    int function_abi_version = 1;
    int syscall_abi_version = EXEC_SYSCALL_ABI_VERSION_V1;
    IsaEncodingVersion isa_version = IsaEncodingVersion::V1;
    std::uint64_t required_features = 0;
};

[[nodiscard]] inline ExecutableArchitectureIdentity
architectureIdentity(const ExecutableImageHeader&) {
    return {};
}

[[nodiscard]] inline ExecutableArchitectureIdentity architectureIdentity(
    const ExecutableImageHeaderV2& header) {
    return {
        header.executable_version,
        header.function_abi_version,
        header.syscall_abi_version,
        IsaEncodingVersion::V2,
        header.required_features,
    };
}

[[nodiscard]] inline bool featureMaskFromNumeric(
    long long numeric,
    std::uint64_t& features) {
    features = 0;
    for (int trit = 0; trit <= architecture::v2::FEATURE_WIDE_T50;
         ++trit) {
        long long remainder = (numeric + 1) % 3;
        if (remainder < 0) remainder += 3;
        const int value = static_cast<int>(remainder - 1);
        numeric = (numeric - value) / 3;
        if (value < 0) return false;
        if (value > 0) features |= featureBit(trit);
    }
    return numeric == 0;
}

[[nodiscard]] inline long long executableHeaderV2Checksum(
    const ExecutableImageHeaderV2& header) {
    return -(static_cast<long long>(header.magic) +
             header.executable_version +
             header.function_abi_version +
             header.header_words +
             header.isa_version +
             featureWordNumeric(header.required_features) +
             header.entry_pc +
             header.text_words +
             header.data_words +
             header.stack_words +
             header.syscall_abi_version +
             header.scalar_word_trits +
             header.base_page_words +
             header.flags);
}

[[nodiscard]] inline bool validateExecutableHeaderV2(
    const ExecutableImageHeaderV2& header) {
    const std::uint64_t base =
        featureBit(architecture::v2::FEATURE_BASE_V2);
    return header.magic == EXEC_V2_MAGIC &&
           header.executable_version ==
               architecture::v2::EXECUTABLE_VERSION &&
           header.function_abi_version ==
               architecture::v2::FUNCTION_ABI_VERSION &&
           header.header_words == EXEC_V2_HEADER_WORDS &&
           header.isa_version == architecture::v2::ISA_VERSION &&
           (header.required_features & base) != 0 &&
           header.entry_pc >= 0 &&
           header.text_words > 0 &&
           header.data_words >= 0 &&
           header.stack_words > 0 &&
           header.stack_words %
                   architecture::v2::STACK_ALIGNMENT_WORDS ==
               0 &&
           header.syscall_abi_version ==
               architecture::v2::SYSCALL_ABI_VERSION &&
           header.scalar_word_trits ==
               architecture::v2::SCALAR_WORD_TRITS &&
           header.base_page_words == architecture::v2::BASE_PAGE_WORDS &&
           header.header_checksum ==
               executableHeaderV2Checksum(header);
}

[[nodiscard]] inline std::vector<TernaryValue> encodeExecutableHeaderV2(
    ExecutableImageHeaderV2 header) {
    header.header_checksum = executableHeaderV2Checksum(header);
    if (!validateExecutableHeaderV2(header)) {
        throw std::invalid_argument("invalid executable header v2");
    }
    return {
        ops::fromLong(header.magic),
        ops::fromLong(header.executable_version),
        ops::fromLong(header.function_abi_version),
        ops::fromLong(header.header_words),
        ops::fromLong(header.isa_version),
        ops::fromLong(featureWordNumeric(header.required_features)),
        ops::fromLong(header.entry_pc),
        ops::fromLong(header.text_words),
        ops::fromLong(header.data_words),
        ops::fromLong(header.stack_words),
        ops::fromLong(header.syscall_abi_version),
        ops::fromLong(header.scalar_word_trits),
        ops::fromLong(header.base_page_words),
        ops::fromLong(header.flags),
        ops::fromLong(header.header_checksum),
    };
}

[[nodiscard]] inline bool decodeExecutableHeaderV2(
    const std::vector<TernaryValue>& image,
    int header_addr,
    ExecutableImageHeaderV2& out) {
    if (header_addr < 0 ||
        header_addr + EXEC_V2_HEADER_WORDS >
            static_cast<int>(image.size())) {
        return false;
    }
    long long words[EXEC_V2_HEADER_WORDS]{};
    for (int index = 0; index < EXEC_V2_HEADER_WORDS; ++index) {
        const TernaryValue& word =
            image[static_cast<std::size_t>(header_addr + index)];
        if (!isNumericMode(word.mode) || word.isInvalid()) return false;
        words[index] = ops::toLong(word);
    }

    ExecutableImageHeaderV2 decoded;
    decoded.header_addr = header_addr;
    decoded.magic = static_cast<int>(words[EXEC_V2_MAGIC_INDEX]);
    decoded.executable_version =
        static_cast<int>(words[EXEC_V2_EXECUTABLE_VERSION]);
    decoded.function_abi_version =
        static_cast<int>(words[EXEC_V2_FUNCTION_ABI_VERSION]);
    decoded.header_words = static_cast<int>(words[EXEC_V2_HEADER_SIZE]);
    decoded.isa_version = static_cast<int>(words[EXEC_V2_ISA_VERSION]);
    if (!featureMaskFromNumeric(
            words[EXEC_V2_REQUIRED_FEATURES],
            decoded.required_features)) {
        return false;
    }
    decoded.entry_pc = static_cast<int>(words[EXEC_V2_ENTRY_PC]);
    decoded.text_words = static_cast<int>(words[EXEC_V2_TEXT_WORDS]);
    decoded.data_words = static_cast<int>(words[EXEC_V2_DATA_WORDS]);
    decoded.stack_words = static_cast<int>(words[EXEC_V2_STACK_WORDS]);
    decoded.syscall_abi_version =
        static_cast<int>(words[EXEC_V2_SYSCALL_ABI_VERSION]);
    decoded.scalar_word_trits =
        static_cast<int>(words[EXEC_V2_SCALAR_WIDTH]);
    decoded.base_page_words =
        static_cast<int>(words[EXEC_V2_BASE_PAGE_WORDS]);
    decoded.flags = static_cast<int>(words[EXEC_V2_FLAGS]);
    decoded.header_checksum = words[EXEC_V2_CHECKSUM];
    if (!validateExecutableHeaderV2(decoded)) return false;
    out = decoded;
    return true;
}
