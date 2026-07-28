#include "ternary_host_runtime.h"

#include <algorithm>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <vector>

namespace {

using sandbox::host::TosBootImage;

bool replaceValidatedFile(
    const std::filesystem::path& temporary,
    const std::filesystem::path& destination,
    std::string& error) {

    std::error_code ec;
    std::filesystem::path backup = destination;
    backup += ".migration-backup";
    std::filesystem::remove(backup, ec);
    ec.clear();

    const bool had_destination = std::filesystem::exists(destination);
    if (had_destination) {
        std::filesystem::rename(destination, backup, ec);
        if (ec) {
            error = "failed to stage existing destination: " + ec.message();
            return false;
        }
    }

    std::filesystem::rename(temporary, destination, ec);
    if (ec) {
        if (had_destination) {
            std::error_code restore_ec;
            std::filesystem::rename(backup, destination, restore_ec);
        }
        error = "failed to atomically install migrated artifact: " +
                ec.message();
        return false;
    }

    if (had_destination) {
        std::filesystem::remove(backup, ec);
    }
    return true;
}

bool migrateBoot(
    const std::filesystem::path& source,
    const std::filesystem::path& destination,
    TosBootImage& migrated,
    std::string& error) {

    if (!sandbox::host::readBootImageFile(source.string(), migrated, &error))
        return false;

    migrated.manifest.format_version =
        sandbox::host::TOS_BOOT_FORMAT_VERSION;
    if (migrated.manifest.sections.empty()) {
        migrated.manifest.sections.push_back({
            "kernel",
            "/kernel",
            "kernel",
            0,
            migrated.manifest.boot_entry,
            static_cast<int>(migrated.program.size()),
            static_cast<int>(
                (migrated.program.size() + sandbox::vm::MMU_PAGE_WORDS - 1) /
                sandbox::vm::MMU_PAGE_WORDS),
            sandbox::host::TOS_IMAGE_SECTION_EXECUTABLE |
                sandbox::host::TOS_IMAGE_SECTION_KERNEL,
        });
    }

    std::filesystem::path temporary = destination;
    temporary += ".migrating";
    std::error_code ec;
    std::filesystem::remove(temporary, ec);
    if (!sandbox::host::writeBootImageFile(
            temporary.string(), migrated, &error)) {
        return false;
    }

    TosBootImage validation;
    if (!sandbox::host::readBootImageFile(
            temporary.string(), validation, &error) ||
        validation.manifest.format_version !=
            sandbox::host::TOS_BOOT_FORMAT_VERSION ||
        validation.program != migrated.program) {
        std::filesystem::remove(temporary, ec);
        if (error.empty()) error = "migrated boot image validation failed";
        return false;
    }
    return replaceValidatedFile(temporary, destination, error);
}

bool readDiskDense(
    const std::filesystem::path& source,
    std::vector<long long>& dense,
    std::string& error) {

    std::ifstream file(source, std::ios::binary);
    if (!file.good()) {
        error = "failed to open disk image: " + source.string();
        return false;
    }
    std::uint64_t magic = 0;
    file.read(reinterpret_cast<char*>(&magic), sizeof(magic));
    if (!file.good()) {
        error = "disk image header is truncated";
        return false;
    }

    int count = 0;
    bool raw_v2 = false;
    if (magic == sandbox::host::TOS_SPARSE_DISK_LEGACY_MAGIC) {
        file.read(reinterpret_cast<char*>(&count), sizeof(count));
    } else if (magic == sandbox::host::TOS_SPARSE_DISK_MAGIC) {
        std::uint32_t version = 0;
        std::uint32_t block_words = 0;
        std::uint64_t generation = 0;
        std::uint64_t checksum = 0;
        file.read(reinterpret_cast<char*>(&version), sizeof(version));
        file.read(reinterpret_cast<char*>(&block_words), sizeof(block_words));
        file.read(reinterpret_cast<char*>(&generation), sizeof(generation));
        file.read(reinterpret_cast<char*>(&checksum), sizeof(checksum));
        file.read(reinterpret_cast<char*>(&count), sizeof(count));
        (void)generation;
        (void)checksum;
        if (version != sandbox::host::TOS_SPARSE_DISK_VERSION ||
            block_words != sandbox::vm::STORAGE_BLOCK_WORDS) {
            error = "disk v2 architecture metadata is invalid";
            return false;
        }
        raw_v2 = true;
    } else {
        error = "unsupported disk image magic";
        return false;
    }
    if (!file.good() || count < 0) {
        error = "disk image record count is invalid";
        return false;
    }

    struct Record {
        int block = 0;
        std::vector<long long> words;
    };
    std::vector<Record> records;
    records.reserve(static_cast<std::size_t>(count));
    int maximum_block = 0;
    for (int record_index = 0; record_index < count; ++record_index) {
        Record record;
        record.words.assign(sandbox::vm::STORAGE_BLOCK_WORDS, 0);
        file.read(reinterpret_cast<char*>(&record.block), sizeof(record.block));
        for (int word = 0;
             word < sandbox::vm::STORAGE_BLOCK_WORDS;
             ++word) {
            if (raw_v2) {
                std::uint64_t raw = 0;
                file.read(reinterpret_cast<char*>(&raw), sizeof(raw));
                if (raw > 12157665459056928801ULL) {
                    error = "disk contains an invalid raw T40 word";
                    return false;
                }
                record.words[static_cast<std::size_t>(word)] =
                    sandbox::vm::ops::toLong(
                        sandbox::vm::TernaryValue::fromTriple(
                            sandbox::Triple{raw}));
            } else {
                file.read(reinterpret_cast<char*>(
                              &record.words[static_cast<std::size_t>(word)]),
                          sizeof(long long));
            }
        }
        if (!file.good() || record.block < 0) {
            error = "disk image record is truncated or invalid";
            return false;
        }
        maximum_block = std::max(maximum_block, record.block);
        records.push_back(std::move(record));
    }

    dense.assign(
        static_cast<std::size_t>(maximum_block + 1) *
            sandbox::vm::STORAGE_BLOCK_WORDS,
        0);
    for (const auto& record : records) {
        std::copy(
            record.words.begin(), record.words.end(),
            dense.begin() +
                static_cast<std::ptrdiff_t>(
                    record.block * sandbox::vm::STORAGE_BLOCK_WORDS));
    }
    return true;
}

bool migrateDisk(
    const std::filesystem::path& source,
    const std::filesystem::path& destination,
    const std::vector<long long>& embedded_seed,
    std::string& error) {

    std::vector<long long> dense = embedded_seed;
    if (!source.empty() && std::filesystem::exists(source)) {
        if (!readDiskDense(source, dense, error)) return false;
    }
    if (dense.empty()) {
        error = "no legacy disk or embedded root filesystem seed is available";
        return false;
    }

    std::filesystem::path temporary = destination;
    temporary += ".migrating";
    std::error_code ec;
    std::filesystem::remove(temporary, ec);
    if (!sandbox::host::writeSparseDiskFile(
            temporary.string(), dense, true, &error)) {
        return false;
    }

    sandbox::vm::VMState validation(64, 64);
    validation.resetBlockDevice(
        static_cast<int>(dense.size()) /
            sandbox::vm::STORAGE_BLOCK_WORDS);
    if (!validation.attachBlockBackingFile(temporary.string()) ||
        validation.blockImage() != dense) {
        std::filesystem::remove(temporary, ec);
        error = "migrated disk validation failed";
        return false;
    }
    return replaceValidatedFile(temporary, destination, error);
}

}  // namespace

int main(int argc, char** argv) {
    sandbox::LongTriple::initPowTable();
    if (argc != 3 && argc != 5) {
        std::cerr
            << "usage: " << argv[0]
            << " source.tboot destination.tboot "
               "[source.tdisk destination.tdisk]\n";
        return 2;
    }

    std::string error;
    TosBootImage migrated;
    if (!migrateBoot(argv[1], argv[2], migrated, error)) {
        std::cerr << error << '\n';
        return 1;
    }
    if (argc == 5 &&
        !migrateDisk(argv[3], argv[4], migrated.rootfs_words, error)) {
        std::cerr << error << '\n';
        return 1;
    }
    std::cout << "migrated boot image to " << argv[2] << '\n';
    if (argc == 5) std::cout << "migrated disk image to " << argv[4] << '\n';
    return 0;
}
