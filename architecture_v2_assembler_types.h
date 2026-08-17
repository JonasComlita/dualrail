#pragma once

struct AssemblyOptions {
    IsaEncodingVersion default_isa = IsaEncodingVersion::V2;
    bool require_isa_directive = false;
    // Executable envelope selection is separate from instruction encoding.
    // v2 remains the default; v3 emission is explicit at the assembler/linker
    // boundary and never inferred from a vector instruction alone.
    int executable_version = architecture::v2::EXECUTABLE_VERSION;
};

struct ArchitectureDirectives {
    IsaEncodingVersion isa = IsaEncodingVersion::V2;
    bool has_isa = false;
    std::uint64_t required_features = 0;
};
