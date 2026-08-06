#include "ternary_host_runtime.h"

#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <string>

#ifndef TRIT_SOURCE_DIR
#error "TRIT_SOURCE_DIR must name the repository root"
#endif
#ifndef TRIT_BUILD_TOS_IMAGE_PATH
#error "TRIT_BUILD_TOS_IMAGE_PATH must name build_tos_image"
#endif
#ifndef TRIT_MIGRATE_TOS_ARTIFACTS_PATH
#error "TRIT_MIGRATE_TOS_ARTIFACTS_PATH must name migrate_tos_artifacts"
#endif

namespace {

int failures = 0;

void expect(bool condition, const std::string& message) {
    if (condition) return;
    ++failures;
    std::cerr << "FAIL: " << message << '\n';
}

std::string quote(const std::filesystem::path& path) {
    return "\"" + path.string() + "\"";
}

bool runCommand(const std::string& command, const std::string& description) {
    const int result = std::system(command.c_str());
    expect(result == 0, description);
    return result == 0;
}

void testTemplateMigration() {
    namespace fs = std::filesystem;
    const fs::path source = TRIT_SOURCE_DIR;
    const fs::path output = source / "build" / "test-migration-v2";
    std::error_code ec;
    fs::remove_all(output, ec);
    fs::create_directories(output, ec);
    expect(!ec, "migration test output directory is available");

    const fs::path template_boot = output / "template.tboot";
    const fs::path template_disk = output / "template.tdisk";
    const fs::path migrated_boot = output / "migrated.tboot";
    const fs::path migrated_disk = output / "migrated.tdisk";
    const fs::path legacy_boot =
        source / "tests" / "fixtures" / "v1" / "v1-release.tboot";
    const fs::path legacy_disk =
        source / "tests" / "fixtures" / "v1" / "v1-release.tdisk";

    const std::string build =
        std::string(TRIT_BUILD_TOS_IMAGE_PATH) + " " +
        quote(template_boot) + " " + quote(template_disk) +
        " migration-test";
    if (!runCommand(build, "v2 migration template builds")) return;

    const std::string migrate =
        std::string(TRIT_MIGRATE_TOS_ARTIFACTS_PATH) +
        " --legacy-boot " + quote(legacy_boot) +
        " --v2-boot-template " + quote(template_boot) +
        " --out-boot " + quote(migrated_boot) +
        " --legacy-disk " + quote(legacy_disk) +
        " --v2-disk-template " + quote(template_disk) +
        " --out-disk " + quote(migrated_disk);
    if (!runCommand(migrate, "v1 release migrates through v2 templates")) {
        return;
    }

    sandbox::host::TosBootImage v2_template;
    sandbox::host::TosBootImage migrated;
    std::string error;
    expect(
        !sandbox::host::readBootImageFile(
            legacy_boot.string(), migrated, &error) &&
            error.find("migrate_tos_artifacts") != std::string::npos,
        "production runtime rejects legacy boot with migration guidance");
    sandbox::host::TosRuntimeConfig legacy_disk_config;
    legacy_disk_config.boot_image_path = template_boot.string();
    legacy_disk_config.disk_path = legacy_disk.string();
    sandbox::host::TosRuntime legacy_disk_runtime(legacy_disk_config);
    error.clear();
    expect(
        !legacy_disk_runtime.loadImage(&error) &&
            error.find("legacy tDisk v1") != std::string::npos &&
            error.find("migrate_tos_artifacts") != std::string::npos,
        "production runtime rejects a live legacy disk mount with migration guidance");
    error.clear();
    expect(
        sandbox::host::readBootImageFile(
            template_boot.string(), v2_template, &error),
        "v2 template reads");
    expect(
        sandbox::host::readBootImageFile(
            migrated_boot.string(), migrated, &error),
        "migrated tboot reads");
    expect(
        migrated.manifest.format_version ==
            sandbox::host::TOS_BOOT_FORMAT_VERSION,
        "migrated boot is tboot v3");
    expect(
        migrated.manifest.isa_version ==
            sandbox::architecture::v2::ISA_VERSION,
        "migrated boot advertises ISA v2");
    expect(
        migrated.program == v2_template.program,
        "migrated boot text comes exactly from the v2 template");

    sandbox::host::TosRuntimeConfig config;
    config.boot_image_path = migrated_boot.string();
    config.disk_path = migrated_disk.string();
    config.profile_name = "minimum";
    sandbox::host::TosRuntime runtime(config);
    const bool loaded = runtime.loadImage(&error);
    expect(
        loaded,
        "migrated v2 boot and disk load together: " + error);
    if (loaded) {
        const auto result = runtime.runForSteps(250000);
        expect(
            !result.trapped(),
            "migrated release begins execution without a VM trap");
    }

    const fs::path embedded_boot =
        source / "tests" / "fixtures" / "v1" /
        "v1-embedded-root.tboot";
    const fs::path embedded_output_boot =
        output / "embedded-migrated.tboot";
    const fs::path embedded_output_disk =
        output / "embedded-migrated.tdisk";
    const std::string migrate_embedded =
        std::string(TRIT_MIGRATE_TOS_ARTIFACTS_PATH) +
        " --legacy-boot " + quote(embedded_boot) +
        " --v2-boot-template " + quote(template_boot) +
        " --out-boot " + quote(embedded_output_boot) +
        " --v2-disk-template " + quote(template_disk) +
        " --out-disk " + quote(embedded_output_disk);
    if (!runCommand(
            migrate_embedded,
            "tboot v1 embedded root migrates without a live legacy mount")) {
        return;
    }

    const std::string reject_v2_source =
        std::string(TRIT_MIGRATE_TOS_ARTIFACTS_PATH) +
        " --legacy-boot " + quote(template_boot) +
        " --v2-boot-template " + quote(template_boot) +
        " --out-boot " + quote(output / "must-not-exist.tboot");
    // This invocation is expected to fail: the standalone migrator must not
    // treat an already-v2 source as a legacy input.  Keep the production gate
    // console clean while preserving the non-zero exit assertion.
#if defined(_WIN32)
    const std::string null_stderr = " 2>NUL";
#else
    const std::string null_stderr = " 2>/dev/null";
#endif
    expect(
        std::system((reject_v2_source + null_stderr).c_str()) != 0,
        "migrator rejects a non-legacy source and aliased template input");
}

}  // namespace

int main() {
    sandbox::LongTriple::initPowTable();
    testTemplateMigration();
    if (failures != 0) {
        std::cerr << failures << " migration-v2 assertion(s) failed\n";
        return 1;
    }
    std::cout << "Migration v2 tests passed\n";
    return 0;
}
