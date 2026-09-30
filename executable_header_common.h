#pragma once

// Shared validation primitives for the current executable ABI v3 envelope.
//
// This header deliberately contains no alternate executable codec.  It only
// provides the value-only view, current contract, feature-word conversion,
// and architecture identity used by the v3 loader and VM.

#include <cstdint>

struct ExecutableHeaderCommonView {
    int header_addr = -1;
    int magic = 40404;
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
    int executable_version =
        sandbox::architecture::v3::EXECUTABLE_VERSION;
    int function_abi_version =
        sandbox::architecture::v3::FUNCTION_ABI_VERSION;
    int header_words =
        sandbox::architecture::v2::CURRENT_EXECUTABLE_HEADER_WORDS;
    int isa_version = sandbox::architecture::v3::ISA_VERSION;
    std::uint64_t supported_features =
        sandbox::architecture::v3::SUPPORTED_FEATURES;
    std::uint64_t required_features =
        sandbox::architecture::v3::REQUIRED_FEATURES;
    int syscall_abi_version =
        sandbox::architecture::v3::SYSCALL_ABI_VERSION;
    int scalar_word_trits = sandbox::architecture::v2::SCALAR_WORD_TRITS;
    int base_page_words = sandbox::architecture::v2::BASE_PAGE_WORDS;
};

[[nodiscard]] inline long long executableFeatureWordNumeric(
    std::uint64_t features,
    int last_feature_trit) {
    if (last_feature_trit < 0 || last_feature_trit >= 62) return -1;
    long long value = 0;
    long long place = 1;
    for (int trit = 0; trit <= last_feature_trit; ++trit) {
        if ((features & sandbox::isa::featureBit(trit)) != 0) value += place;
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
        if (value > 0) features |= sandbox::isa::featureBit(trit);
    }
    return numeric == 0;
}

[[nodiscard]] inline bool validateExecutableHeaderCommon(
    const ExecutableHeaderCommonView& view,
    const ExecutableHeaderCommonContract& contract = {}) {
    const std::uint64_t required = contract.required_features;
    return view.magic == 40404 &&
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
           view.stack_words % sandbox::architecture::v2::STACK_ALIGNMENT_WORDS ==
               0 &&
           view.syscall_abi_version == contract.syscall_abi_version &&
           view.scalar_word_trits == contract.scalar_word_trits &&
           view.base_page_words == contract.base_page_words;
}

struct ExecutableArchitectureIdentity {
    int executable_version =
        sandbox::architecture::v3::EXECUTABLE_VERSION;
    int function_abi_version =
        sandbox::architecture::v3::FUNCTION_ABI_VERSION;
    int syscall_abi_version =
        sandbox::architecture::v3::SYSCALL_ABI_VERSION;
    sandbox::isa::IsaEncodingVersion isa_version =
        sandbox::isa::IsaEncodingVersion::V2;
    std::uint64_t required_features =
        sandbox::architecture::v3::REQUIRED_FEATURES;
    int vector_abi_version = sandbox::architecture::v3::VECTOR_ABI_VERSION;
};
