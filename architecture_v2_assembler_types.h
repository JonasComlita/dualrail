#pragma once

struct AssemblyOptions {
    IsaEncodingVersion default_isa = IsaEncodingVersion::V2;
    bool require_isa_directive = false;
};

struct ArchitectureDirectives {
    IsaEncodingVersion isa = IsaEncodingVersion::V2;
    bool has_isa = false;
    std::uint64_t required_features = 0;
};
