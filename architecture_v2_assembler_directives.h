#pragma once

[[nodiscard]] inline std::optional<int> architectureFeatureTrit(
    std::string name) {
    name = toLower(name);
    std::replace(name.begin(), name.end(), '-', '_');
    if (name == "base" || name == "base_v2")
        return architecture::v2::FEATURE_BASE_V2;
    if (name == "scalar" || name == "scalar_advanced")
        return architecture::v2::FEATURE_SCALAR_ADVANCED;
    if (name == "lane")
        return architecture::v2::FEATURE_LANE;
    if (name == "vector")
        return architecture::v2::FEATURE_VECTOR;
    if (name == "accumulator" || name == "ai" ||
        name == "accumulator_ai")
        return architecture::v2::FEATURE_ACCUMULATOR_AI;
    if (name == "atomics")
        return architecture::v2::FEATURE_ATOMICS;
    if (name == "mmu")
        return architecture::v2::FEATURE_MMU;
    if (name == "wait")
        return architecture::v2::FEATURE_WAIT;
    if (name == "wide" || name == "wide_t50" || name == "t50")
        return architecture::v2::FEATURE_WIDE_T50;
    return std::nullopt;
}

[[nodiscard]] inline ArchitectureDirectives parseArchitectureDirectives(
    const std::string& source,
    const AssemblyOptions& options,
    std::vector<AssemblyError>& errors) {
    ArchitectureDirectives directives;
    directives.isa = options.default_isa;

    std::istringstream stream(source);
    std::string raw;
    int line_number = 0;
    bool saw_body = false;
    while (std::getline(stream, raw)) {
        ++line_number;
        const std::string line = trim(stripComment(raw));
        if (line.empty()) continue;
        const auto tokens = tokenize(line);
        if (tokens.empty()) continue;
        const std::string directive = toLower(tokens.front());

        if (directive != ".isa" && directive != ".require") {
            saw_body = true;
            continue;
        }
        if (saw_body) {
            errors.push_back(
                {line_number,
                 directive + " must precede sections, labels, and instructions"});
            continue;
        }

        if (directive == ".isa") {
            if (directives.has_isa) {
                errors.push_back(
                    {line_number, "Only one .isa directive is allowed"});
                continue;
            }
            if (tokens.size() != 2 || tokens[1] != "2") {
                errors.push_back(
                    {line_number,
                     "Only .isa 2 is supported; migrate v1 source and "
                     "artifacts with the offline migration tools"});
                continue;
            }
            directives.has_isa = true;
            directives.isa = IsaEncodingVersion::V2;
            continue;
        }

        if (tokens.size() < 2) {
            errors.push_back(
                {line_number, ".require needs at least one feature name"});
            continue;
        }
        for (std::size_t index = 1; index < tokens.size(); ++index) {
            const auto feature = architectureFeatureTrit(tokens[index]);
            if (!feature.has_value()) {
                errors.push_back(
                    {line_number,
                     "Unknown architecture feature '" + tokens[index] + "'"});
                continue;
            }
            directives.required_features |= featureBit(*feature);
        }
    }

    if (options.require_isa_directive && !directives.has_isa) {
        errors.push_back(
            {0, "Source must begin with an explicit .isa 2"});
    }
    directives.required_features |=
        featureBit(architecture::v2::FEATURE_BASE_V2);
    return directives;
}
