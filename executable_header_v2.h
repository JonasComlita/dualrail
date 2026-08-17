#pragma once

// Authoritative ISA v2 executable-header codec.

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

// The first fifteen words are shared by every executable envelope.  Keeping a
// value-only view lets versioned codecs validate the common contract without
// accepting a v3 extension as a v2 image (or vice versa).
struct ExecutableHeaderCommonView {
    int header_addr = -1;
    int magic = EXEC_MAGIC;
    int executable_version = 0;
    int function_abi_version = 0;
    int header_words = 0;
    int isa_version = 0;
    std::uint64_t required_features = 0;
    int entry_pc = 0;
    int text_words = 0;
    int data_words = 0;
    int stack_words = 0;
    int syscall_abi_version = 0;
    int scalar_word_trits = 0;
    int base_page_words = 0;
    int flags = 0;
};

struct ExecutableHeaderCommonContract {
    int executable_version = architecture::v2::EXECUTABLE_VERSION;
    int function_abi_version = architecture::v2::FUNCTION_ABI_VERSION;
    int header_words = EXEC_V2_HEADER_WORDS;
    int isa_version = architecture::v2::ISA_VERSION;
    std::uint64_t supported_features =
        (std::uint64_t{1} <<
         static_cast<unsigned>(architecture::v2::FEATURE_WIDE_T50 + 1)) -
        1;
    std::uint64_t required_features =
        featureBit(architecture::v2::FEATURE_BASE_V2);
    int syscall_abi_version = architecture::v2::SYSCALL_ABI_VERSION;
    int scalar_word_trits = architecture::v2::SCALAR_WORD_TRITS;
    int base_page_words = architecture::v2::BASE_PAGE_WORDS;
};

[[nodiscard]] inline long long executableFeatureWordNumeric(
    std::uint64_t features,
    int last_feature_trit) {
    if (last_feature_trit < 0 || last_feature_trit >= 62) return -1;
    long long value = 0;
    long long place = 1;
    for (int trit = 0; trit <= last_feature_trit; ++trit) {
        if ((features & featureBit(trit)) != 0) value += place;
        place *= 3;
    }
    return value;
}

[[nodiscard]] inline bool executableFeatureMaskFromNumeric(
    long long numeric,
    int last_feature_trit,
    std::uint64_t& features) {
    if (last_feature_trit < 0 || last_feature_trit >= 62) return false;
    features = 0;
    for (int trit = 0; trit <= last_feature_trit; ++trit) {
        long long remainder = (numeric + 1) % 3;
        if (remainder < 0) remainder += 3;
        const int value = static_cast<int>(remainder - 1);
        numeric = (numeric - value) / 3;
        if (value < 0) return false;
        if (value > 0) features |= featureBit(trit);
    }
    return numeric == 0;
}

[[nodiscard]] inline bool validateExecutableHeaderCommon(
    const ExecutableHeaderCommonView& view,
    const ExecutableHeaderCommonContract& contract = {}) {
    const std::uint64_t required = contract.required_features;
    return view.magic == EXEC_MAGIC &&
           view.executable_version == contract.executable_version &&
           view.function_abi_version == contract.function_abi_version &&
           view.header_words == contract.header_words &&
           view.isa_version == contract.isa_version &&
           (view.required_features & required) == required &&
           (view.required_features & ~contract.supported_features) == 0 &&
           view.entry_pc >= 0 &&
           view.text_words > 0 &&
           view.data_words >= 0 &&
           view.stack_words > 0 &&
           view.stack_words % architecture::v2::STACK_ALIGNMENT_WORDS == 0 &&
           view.syscall_abi_version == contract.syscall_abi_version &&
           view.scalar_word_trits == contract.scalar_word_trits &&
           view.base_page_words == contract.base_page_words;
}

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

[[nodiscard]] inline ExecutableHeaderCommonView executableHeaderCommonView(
    const ExecutableImageHeaderV2& header) {
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

[[nodiscard]] inline int executableTextPages(
    const ExecutableImageHeaderV2& header) {
    return std::max(
        1,
        (header.text_words + architecture::v2::BASE_PAGE_WORDS - 1) /
            architecture::v2::BASE_PAGE_WORDS);
}

[[nodiscard]] inline int executableDataPages(
    const ExecutableImageHeaderV2& header) {
    const int writable_words =
        std::max(header.data_words, header.stack_words);
    return std::max(
        1,
        (writable_words + architecture::v2::BASE_PAGE_WORDS - 1) /
            architecture::v2::BASE_PAGE_WORDS);
}

struct ExecutableArchitectureIdentity {
    int executable_version = architecture::v2::EXECUTABLE_VERSION;
    int function_abi_version = architecture::v2::FUNCTION_ABI_VERSION;
    int syscall_abi_version = architecture::v2::SYSCALL_ABI_VERSION;
    IsaEncodingVersion isa_version = IsaEncodingVersion::V2;
    std::uint64_t required_features =
        featureBit(architecture::v2::FEATURE_BASE_V2);
    int vector_abi_version = 0;
};

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
    return executableFeatureMaskFromNumeric(
        numeric, architecture::v2::FEATURE_WIDE_T50, features);
}

[[nodiscard]] inline long long executableHeaderV2Checksum(
    const ExecutableImageHeaderV2& header) {
    return -(static_cast<long long>(header.magic) +
             header.executable_version +
             header.function_abi_version +
             header.header_words +
             header.isa_version +
             executableFeatureWordNumeric(
                 header.required_features,
                 architecture::v2::FEATURE_WIDE_T50) +
             header.entry_pc +
             header.text_words +
             header.data_words +
             header.stack_words +
             header.syscall_abi_version +
             header.scalar_word_trits +
             header.base_page_words +
             header.flags);
}

[[nodiscard]] inline ExecutableImageHeaderV2 makeExecutableHeaderV2(
    int entry_pc,
    int text_words,
    int data_words = 0,
    int stack_words = architecture::v2::STACK_ALIGNMENT_WORDS,
    std::uint64_t required_features =
        featureBit(architecture::v2::FEATURE_BASE_V2),
    int flags = 0) {
    ExecutableImageHeaderV2 header;
    header.entry_pc = entry_pc;
    header.text_words = text_words;
    header.data_words = data_words;
    header.stack_words = stack_words;
    header.required_features = required_features;
    header.flags = flags;
    header.header_checksum = executableHeaderV2Checksum(header);
    return header;
}

[[nodiscard]] inline bool validateExecutableHeaderV2(
    const ExecutableImageHeaderV2& header) {
    ExecutableHeaderCommonContract contract;
    const bool common = validateExecutableHeaderCommon(
        executableHeaderCommonView(header), contract);
    return common &&
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
        ops::fromLong(executableFeatureWordNumeric(
            header.required_features, architecture::v2::FEATURE_WIDE_T50)),
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
        header_addr > static_cast<int>(image.size()) ||
        EXEC_V2_HEADER_WORDS >
            static_cast<int>(image.size()) - header_addr) {
        return false;
    }
    long long words[EXEC_V2_HEADER_WORDS]{};
    for (int index = 0; index < EXEC_V2_HEADER_WORDS; ++index) {
        const TernaryValue& word =
            image[static_cast<std::size_t>(header_addr + index)];
        if (!isNumericMode(word.mode) || word.isInvalid()) return false;
        words[index] = ops::toLong(word);
    }

    const auto fitsInt = [](long long value) {
        return value >= static_cast<long long>(std::numeric_limits<int>::min()) &&
               value <= static_cast<long long>(std::numeric_limits<int>::max());
    };
    for (int index = 0; index < EXEC_V2_HEADER_WORDS; ++index) {
        if (index == EXEC_V2_REQUIRED_FEATURES ||
            index == EXEC_V2_CHECKSUM) {
            continue;
        }
        if (!fitsInt(words[index])) return false;
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
