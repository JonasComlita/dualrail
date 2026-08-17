#pragma once

// Staged executable ABI v3 envelope.  The common prefix is byte/word
// compatible with v2, but the exact header size, ABI version, feature mask,
// and vector geometry are validated as one versioned contract.  The v3
// envelope deliberately keeps ISA encoding v2 until the instruction codec and
// loader owners add a corresponding ISA version.

static constexpr int EXEC_V3_HEADER_WORDS = 20;
static constexpr int EXEC_V3_MAGIC = EXEC_MAGIC;

static constexpr int EXEC_V3_MAGIC_INDEX = EXEC_V2_MAGIC_INDEX;
static constexpr int EXEC_V3_EXECUTABLE_VERSION = EXEC_V2_EXECUTABLE_VERSION;
static constexpr int EXEC_V3_FUNCTION_ABI_VERSION = EXEC_V2_FUNCTION_ABI_VERSION;
static constexpr int EXEC_V3_HEADER_SIZE = EXEC_V2_HEADER_SIZE;
static constexpr int EXEC_V3_ISA_VERSION = EXEC_V2_ISA_VERSION;
static constexpr int EXEC_V3_REQUIRED_FEATURES = EXEC_V2_REQUIRED_FEATURES;
static constexpr int EXEC_V3_ENTRY_PC = EXEC_V2_ENTRY_PC;
static constexpr int EXEC_V3_TEXT_WORDS = EXEC_V2_TEXT_WORDS;
static constexpr int EXEC_V3_DATA_WORDS = EXEC_V2_DATA_WORDS;
static constexpr int EXEC_V3_STACK_WORDS = EXEC_V2_STACK_WORDS;
static constexpr int EXEC_V3_SYSCALL_ABI_VERSION = EXEC_V2_SYSCALL_ABI_VERSION;
static constexpr int EXEC_V3_SCALAR_WIDTH = EXEC_V2_SCALAR_WIDTH;
static constexpr int EXEC_V3_BASE_PAGE_WORDS = EXEC_V2_BASE_PAGE_WORDS;
static constexpr int EXEC_V3_FLAGS = EXEC_V2_FLAGS;
static constexpr int EXEC_V3_CHECKSUM = EXEC_V2_CHECKSUM;

// Extension fields occupy the final five words of the 20-word envelope.
static constexpr int EXEC_V3_VECTOR_REGISTER_COUNT_INDEX = 15;
static constexpr int EXEC_V3_VECTOR_LANE_COUNT_INDEX = 16;
static constexpr int EXEC_V3_VECTOR_LANE_WORD_TRITS_INDEX = 17;
static constexpr int EXEC_V3_VECTOR_CONTEXT_WORDS_INDEX = 18;
static constexpr int EXEC_V3_VECTOR_SPILL_WORDS_INDEX = 19;

// Authoritative v3 geometry values.
static constexpr int EXEC_V3_VECTOR_REGISTER_COUNT_VALUE =
    architecture::v3::VECTOR_REGISTER_COUNT;
static constexpr int EXEC_V3_VECTOR_LANE_COUNT_VALUE =
    architecture::v3::VECTOR_LANE_COUNT;
static constexpr int EXEC_V3_VECTOR_LANE_WORD_TRITS_VALUE =
    architecture::v3::VECTOR_LANE_WORD_TRITS;
static constexpr int EXEC_V3_VECTOR_CONTEXT_WORDS_VALUE =
    architecture::v3::VECTOR_CONTEXT_WORDS;
static constexpr int EXEC_V3_VECTOR_SPILL_WORDS_VALUE =
    architecture::v3::VECTOR_SPILL_WORDS;

struct ExecutableImageHeaderV3 {
    int header_addr = -1;
    int magic = EXEC_V3_MAGIC;
    int executable_version = architecture::v3::EXECUTABLE_VERSION;
    int function_abi_version = architecture::v3::FUNCTION_ABI_VERSION;
    int header_words = EXEC_V3_HEADER_WORDS;
    // v3 is an executable/function envelope profile over the v2 ISA codec.
    int isa_version = architecture::v3::ISA_VERSION;
    std::uint64_t required_features = architecture::v3::REQUIRED_FEATURES;
    int entry_pc = 0;
    int text_words = 0;
    int data_words = 0;
    int stack_words = architecture::v2::STACK_ALIGNMENT_WORDS;
    int syscall_abi_version = architecture::v3::SYSCALL_ABI_VERSION;
    int scalar_word_trits = architecture::v2::SCALAR_WORD_TRITS;
    int base_page_words = architecture::v2::BASE_PAGE_WORDS;
    int flags = 0;
    int vector_register_count = architecture::v3::VECTOR_REGISTER_COUNT;
    int vector_lane_count = architecture::v3::VECTOR_LANE_COUNT;
    int vector_lane_word_trits = architecture::v3::VECTOR_LANE_WORD_TRITS;
    int vector_context_words = architecture::v3::VECTOR_CONTEXT_WORDS;
    int vector_spill_words = architecture::v3::VECTOR_SPILL_WORDS;
    long long header_checksum = 0;
};

enum class ExecutableHeaderVersion : int {
    V2 = architecture::v2::EXECUTABLE_VERSION,
    V3 = architecture::v3::EXECUTABLE_VERSION,
};

// One version-dispatched view used by loaders and image inspectors.  The
// concrete headers remain available so v2 callers never need to reinterpret a
// v3 extension as a v2 record.
struct ExecutableHeaderVariant {
    ExecutableHeaderVersion version = ExecutableHeaderVersion::V2;
    bool is_v3 = false;
    ExecutableHeaderCommonView common{};
    ExecutableImageHeaderV2 v2{};
    ExecutableImageHeaderV3 v3{};
};

[[nodiscard]] inline ExecutableHeaderCommonView executableHeaderCommonView(
    const ExecutableImageHeaderV3& header) {
    return {
        header.header_addr,
        header.magic,
        header.executable_version,
        header.function_abi_version,
        header.header_words,
        header.isa_version,
        header.required_features,
        header.entry_pc,
        header.text_words,
        header.data_words,
        header.stack_words,
        header.syscall_abi_version,
        header.scalar_word_trits,
        header.base_page_words,
        header.flags,
    };
}

[[nodiscard]] inline ExecutableHeaderCommonContract executableHeaderV3Contract() {
    ExecutableHeaderCommonContract contract;
    contract.executable_version = architecture::v3::EXECUTABLE_VERSION;
    contract.function_abi_version = architecture::v3::FUNCTION_ABI_VERSION;
    contract.header_words = EXEC_V3_HEADER_WORDS;
    contract.isa_version = architecture::v3::ISA_VERSION;
    contract.supported_features = architecture::v3::SUPPORTED_FEATURES;
    contract.required_features = architecture::v3::REQUIRED_FEATURES;
    contract.syscall_abi_version = architecture::v3::SYSCALL_ABI_VERSION;
    contract.scalar_word_trits = architecture::v2::SCALAR_WORD_TRITS;
    contract.base_page_words = architecture::v2::BASE_PAGE_WORDS;
    return contract;
}

[[nodiscard]] inline int executableTextPages(
    const ExecutableImageHeaderV3& header) {
    return std::max(
        1,
        (header.text_words + architecture::v2::BASE_PAGE_WORDS - 1) /
            architecture::v2::BASE_PAGE_WORDS);
}

[[nodiscard]] inline int executableDataPages(
    const ExecutableImageHeaderV3& header) {
    const int writable_words =
        std::max(header.data_words, header.stack_words);
    return std::max(
        1,
        (writable_words + architecture::v2::BASE_PAGE_WORDS - 1) /
            architecture::v2::BASE_PAGE_WORDS);
}

[[nodiscard]] inline ExecutableArchitectureIdentity architectureIdentity(
    const ExecutableImageHeaderV3& header) {
    return {
        header.executable_version,
        header.function_abi_version,
        header.syscall_abi_version,
        IsaEncodingVersion::V2,
        header.required_features,
        architecture::v3::VECTOR_ABI_VERSION,
    };
}

[[nodiscard]] inline long long executableHeaderV3Checksum(
    const ExecutableImageHeaderV3& header) {
    return -(static_cast<long long>(header.magic) +
             header.executable_version +
             header.function_abi_version +
             header.header_words +
             header.isa_version +
             executableFeatureWordNumeric(
                 header.required_features,
                 architecture::v3::FEATURE_V3_LAST) +
             header.entry_pc +
             header.text_words +
             header.data_words +
             header.stack_words +
             header.syscall_abi_version +
             header.scalar_word_trits +
             header.base_page_words +
             header.flags +
             header.vector_register_count +
             header.vector_lane_count +
             header.vector_lane_word_trits +
             header.vector_context_words +
             header.vector_spill_words);
}

[[nodiscard]] inline bool validateExecutableHeaderV3(
    const ExecutableImageHeaderV3& header) {
    const ExecutableHeaderCommonContract contract =
        executableHeaderV3Contract();
    return validateExecutableHeaderCommon(
               executableHeaderCommonView(header), contract) &&
           header.vector_register_count ==
               architecture::v3::VECTOR_REGISTER_COUNT &&
           header.vector_lane_count == architecture::v3::VECTOR_LANE_COUNT &&
           header.vector_lane_word_trits ==
               architecture::v3::VECTOR_LANE_WORD_TRITS &&
           header.vector_context_words == architecture::v3::VECTOR_CONTEXT_WORDS &&
           header.vector_spill_words == architecture::v3::VECTOR_SPILL_WORDS &&
           header.header_checksum == executableHeaderV3Checksum(header);
}

[[nodiscard]] inline ExecutableImageHeaderV3 makeExecutableHeaderV3(
    int entry_pc,
    int text_words,
    int data_words = 0,
    int stack_words = architecture::v2::STACK_ALIGNMENT_WORDS,
    std::uint64_t required_features = architecture::v3::REQUIRED_FEATURES,
    int flags = 0) {
    ExecutableImageHeaderV3 header;
    header.entry_pc = entry_pc;
    header.text_words = text_words;
    header.data_words = data_words;
    header.stack_words = stack_words;
    header.required_features = required_features;
    header.flags = flags;
    header.header_checksum = executableHeaderV3Checksum(header);
    return header;
}

[[nodiscard]] inline std::vector<TernaryValue> encodeExecutableHeaderV3(
    ExecutableImageHeaderV3 header) {
    header.header_checksum = executableHeaderV3Checksum(header);
    if (!validateExecutableHeaderV3(header)) {
        throw std::invalid_argument("invalid executable header v3");
    }
    return {
        ops::fromLong(header.magic),
        ops::fromLong(header.executable_version),
        ops::fromLong(header.function_abi_version),
        ops::fromLong(header.header_words),
        ops::fromLong(header.isa_version),
        ops::fromLong(executableFeatureWordNumeric(
            header.required_features, architecture::v3::FEATURE_V3_LAST)),
        ops::fromLong(header.entry_pc),
        ops::fromLong(header.text_words),
        ops::fromLong(header.data_words),
        ops::fromLong(header.stack_words),
        ops::fromLong(header.syscall_abi_version),
        ops::fromLong(header.scalar_word_trits),
        ops::fromLong(header.base_page_words),
        ops::fromLong(header.flags),
        ops::fromLong(header.header_checksum),
        ops::fromLong(header.vector_register_count),
        ops::fromLong(header.vector_lane_count),
        ops::fromLong(header.vector_lane_word_trits),
        ops::fromLong(header.vector_context_words),
        ops::fromLong(header.vector_spill_words),
    };
}

[[nodiscard]] inline bool decodeExecutableHeaderV3(
    const std::vector<TernaryValue>& image,
    int header_addr,
    ExecutableImageHeaderV3& out) {
    if (header_addr < 0 ||
        header_addr > static_cast<int>(image.size()) ||
        EXEC_V3_HEADER_WORDS >
            static_cast<int>(image.size()) - header_addr) {
        return false;
    }
    long long words[EXEC_V3_HEADER_WORDS]{};
    for (int index = 0; index < EXEC_V3_HEADER_WORDS; ++index) {
        const TernaryValue& word =
            image[static_cast<std::size_t>(header_addr + index)];
        if (!isNumericMode(word.mode) || word.isInvalid()) return false;
        words[index] = ops::toLong(word);
    }
    const auto fitsInt = [](long long value) {
        return value >= static_cast<long long>(std::numeric_limits<int>::min()) &&
               value <= static_cast<long long>(std::numeric_limits<int>::max());
    };
    for (int index = 0; index < EXEC_V3_HEADER_WORDS; ++index) {
        if (index == EXEC_V3_REQUIRED_FEATURES ||
            index == EXEC_V3_CHECKSUM) {
            continue;
        }
        if (!fitsInt(words[index])) return false;
    }

    ExecutableImageHeaderV3 decoded;
    decoded.header_addr = header_addr;
    decoded.magic = static_cast<int>(words[EXEC_V3_MAGIC_INDEX]);
    decoded.executable_version =
        static_cast<int>(words[EXEC_V3_EXECUTABLE_VERSION]);
    decoded.function_abi_version =
        static_cast<int>(words[EXEC_V3_FUNCTION_ABI_VERSION]);
    decoded.header_words = static_cast<int>(words[EXEC_V3_HEADER_SIZE]);
    decoded.isa_version = static_cast<int>(words[EXEC_V3_ISA_VERSION]);
    if (!executableFeatureMaskFromNumeric(
            words[EXEC_V3_REQUIRED_FEATURES],
            architecture::v3::FEATURE_V3_LAST,
            decoded.required_features)) {
        return false;
    }
    decoded.entry_pc = static_cast<int>(words[EXEC_V3_ENTRY_PC]);
    decoded.text_words = static_cast<int>(words[EXEC_V3_TEXT_WORDS]);
    decoded.data_words = static_cast<int>(words[EXEC_V3_DATA_WORDS]);
    decoded.stack_words = static_cast<int>(words[EXEC_V3_STACK_WORDS]);
    decoded.syscall_abi_version =
        static_cast<int>(words[EXEC_V3_SYSCALL_ABI_VERSION]);
    decoded.scalar_word_trits =
        static_cast<int>(words[EXEC_V3_SCALAR_WIDTH]);
    decoded.base_page_words =
        static_cast<int>(words[EXEC_V3_BASE_PAGE_WORDS]);
    decoded.flags = static_cast<int>(words[EXEC_V3_FLAGS]);
    decoded.header_checksum = words[EXEC_V3_CHECKSUM];
    decoded.vector_register_count =
        static_cast<int>(words[EXEC_V3_VECTOR_REGISTER_COUNT_INDEX]);
    decoded.vector_lane_count =
        static_cast<int>(words[EXEC_V3_VECTOR_LANE_COUNT_INDEX]);
    decoded.vector_lane_word_trits =
        static_cast<int>(words[EXEC_V3_VECTOR_LANE_WORD_TRITS_INDEX]);
    decoded.vector_context_words =
        static_cast<int>(words[EXEC_V3_VECTOR_CONTEXT_WORDS_INDEX]);
    decoded.vector_spill_words =
        static_cast<int>(words[EXEC_V3_VECTOR_SPILL_WORDS_INDEX]);
    if (!validateExecutableHeaderV3(decoded)) return false;
    out = decoded;
    return true;
}

[[nodiscard]] inline bool decodeExecutableHeaderVersioned(
    const std::vector<TernaryValue>& image,
    int header_addr,
    ExecutableHeaderVariant& out) {
    if (header_addr < 0 ||
        header_addr > static_cast<int>(image.size()) ||
        2 > static_cast<int>(image.size()) - header_addr) {
        return false;
    }
    const TernaryValue& version_word = image[
        static_cast<std::size_t>(header_addr + EXEC_V2_EXECUTABLE_VERSION)];
    if (!isNumericMode(version_word.mode) || version_word.isInvalid()) {
        return false;
    }
    const long long version = ops::toLong(version_word);
    ExecutableHeaderVariant decoded;
    if (version == architecture::v2::EXECUTABLE_VERSION) {
        if (!decodeExecutableHeaderV2(image, header_addr, decoded.v2)) {
            return false;
        }
        decoded.version = ExecutableHeaderVersion::V2;
        decoded.is_v3 = false;
        decoded.common = executableHeaderCommonView(decoded.v2);
    } else if (version == architecture::v3::EXECUTABLE_VERSION) {
        if (!decodeExecutableHeaderV3(image, header_addr, decoded.v3)) {
            return false;
        }
        decoded.version = ExecutableHeaderVersion::V3;
        decoded.is_v3 = true;
        decoded.common = executableHeaderCommonView(decoded.v3);
    } else {
        // Unknown envelope versions are never migrated or guessed.
        return false;
    }
    out = decoded;
    return true;
}

[[nodiscard]] inline std::vector<TernaryValue> encodeExecutableHeaderVersioned(
    const ExecutableHeaderVariant& header) {
    if (header.is_v3 || header.version == ExecutableHeaderVersion::V3) {
        return encodeExecutableHeaderV3(header.v3);
    }
    return encodeExecutableHeaderV2(header.v2);
}

[[nodiscard]] inline bool validateExecutableHeaderVersioned(
    const ExecutableHeaderVariant& header) {
    if (header.is_v3 != (header.version == ExecutableHeaderVersion::V3)) {
        return false;
    }
    if (header.is_v3) {
        return validateExecutableHeaderV3(header.v3) &&
               header.v3.function_abi_version ==
                   architecture::v3::FUNCTION_ABI_VERSION;
    }
    return validateExecutableHeaderV2(header.v2) &&
           header.v2.function_abi_version ==
               architecture::v2::FUNCTION_ABI_VERSION;
}
