#pragma once

struct AssemblyOptions {
    IsaEncodingVersion default_isa = IsaEncodingVersion::V2;
    bool require_isa_directive = false;
    // Executable envelope selection is separate from ISA instruction encoding.
    // The current platform always emits the v3 executable envelope.
    int executable_version = architecture::v3::EXECUTABLE_VERSION;
};

struct ArchitectureDirectives {
    IsaEncodingVersion isa = IsaEncodingVersion::V2;
    bool has_isa = false;
    std::uint64_t required_features = 0;
};
