#include "ternary_host_runtime.h"
#include "ternary_os.h"

#include <algorithm>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <map>
#include <set>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

namespace {

using sandbox::host::TosBootImage;

constexpr int kBlockWords = sandbox::vm::STORAGE_BLOCK_WORDS;
constexpr std::uint32_t kLegacyBootV1 = 1;
constexpr std::uint32_t kLegacyBootV2 = 2;
constexpr int kLegacySyscallAbiV1 = 1;
constexpr std::uint64_t kLegacySparseDiskMagic =
    0x54524954535031ULL; // "TRITSP1"

struct Options {
    std::filesystem::path legacy_boot;
    std::filesystem::path v2_boot_template;
    std::filesystem::path output_boot;
    std::filesystem::path legacy_disk;
    std::filesystem::path v2_disk_template;
    std::filesystem::path output_disk;
};

struct NativeEntry {
    std::string path;
    int inode = -1;
    int kind = 0;
    std::vector<long long> payload;
};

struct NativeSnapshot {
    std::map<std::string, NativeEntry> entries;
};

struct TemplateExecutable {
    std::string path;
    int source_inode = -1;
    int text_ppn = 0;
    sandbox::vm::ExecutableImageHeaderV2 header_v2;
    std::vector<sandbox::isa::TritWord27> program;
};

void printUsage(const char* executable) {
    std::cerr
        << "usage: " << executable
        << " --legacy-boot old.tboot"
           " --v2-boot-template fresh-v2.tboot"
           " --out-boot migrated.tboot"
           " [--legacy-disk old.tdisk"
           " --v2-disk-template fresh-v2.tdisk"
           " --out-disk migrated.tdisk]\n";
}

bool parseOptions(int argc, char** argv, Options& options, std::string& error) {
    if (argc == 2 && std::string(argv[1]) == "--help") return false;
    if (argc < 7 || (argc % 2) == 0) {
        error = "migration arguments must be option/value pairs";
        return false;
    }
    std::map<std::string, std::filesystem::path*> destinations = {
        {"--legacy-boot", &options.legacy_boot},
        {"--v2-boot-template", &options.v2_boot_template},
        {"--out-boot", &options.output_boot},
        {"--legacy-disk", &options.legacy_disk},
        {"--v2-disk-template", &options.v2_disk_template},
        {"--out-disk", &options.output_disk},
    };
    std::set<std::string> seen;
    for (int index = 1; index + 1 < argc; index += 2) {
        const std::string key = argv[index];
        const auto found = destinations.find(key);
        if (found == destinations.end()) {
            error = "unknown migration option: " + key;
            return false;
        }
        if (!seen.insert(key).second) {
            error = "duplicate migration option: " + key;
            return false;
        }
        *found->second = argv[index + 1];
    }
    if (options.legacy_boot.empty() ||
        options.v2_boot_template.empty() ||
        options.output_boot.empty()) {
        error = "legacy boot, v2 boot template, and output boot are required";
        return false;
    }
    const bool any_disk =
        !options.legacy_disk.empty() ||
        !options.v2_disk_template.empty() ||
        !options.output_disk.empty();
    if (any_disk &&
        (options.v2_disk_template.empty() || options.output_disk.empty())) {
        error = "disk migration requires a v2 disk template and output disk";
        return false;
    }
    const auto equivalent = [](const std::filesystem::path& a,
                               const std::filesystem::path& b) {
        if (a.empty() || b.empty()) return false;
        std::error_code ec;
        const auto left = std::filesystem::weakly_canonical(a, ec);
        ec.clear();
        const auto right = std::filesystem::weakly_canonical(b, ec);
        return !ec && left == right;
    };
    if (equivalent(options.output_boot, options.legacy_boot) ||
        equivalent(options.output_boot, options.v2_boot_template) ||
        equivalent(options.output_disk, options.legacy_disk) ||
        equivalent(options.output_disk, options.v2_disk_template)) {
        error = "outputs must be distinct from legacy inputs and v2 templates";
        return false;
    }
    return true;
}

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

    if (had_destination) std::filesystem::remove(backup, ec);
    return true;
}

bool readLegacyBootPayload(
    const std::vector<std::uint8_t>& payload,
    TosBootImage& image,
    std::string& error) {

    using sandbox::host::detail::readPod;
    using sandbox::host::detail::readString;

    std::size_t offset = 0;
    std::uint32_t version = 0;
    if (!readPod(payload, offset, version) ||
        (version != kLegacyBootV1 && version != kLegacyBootV2)) {
        error = "legacy boot input must be tboot v1 or v2";
        return false;
    }

    TosBootImage decoded;
    decoded.manifest.format_version = version;
    decoded.manifest.isa_version = 1;
    decoded.manifest.required_features = 0;
    decoded.manifest.scalar_word_trits = 50;
    decoded.manifest.base_page_words = 27;
    decoded.manifest.function_abi_version = 1;
    decoded.manifest.syscall_abi_version =
        kLegacySyscallAbiV1;
    if (!readPod(payload, offset, decoded.manifest.boot_entry) ||
        !readPod(payload, offset, decoded.manifest.framebuffer_width) ||
        !readPod(payload, offset, decoded.manifest.framebuffer_height) ||
        !readString(payload, offset, decoded.manifest.profile_name) ||
        !readString(payload, offset, decoded.manifest.image_version)) {
        error = "legacy boot manifest is truncated";
        return false;
    }

    if (version == kLegacyBootV2) {
        std::uint32_t section_count = 0;
        if (!readPod(payload, offset, section_count)) {
            error = "legacy boot section table is missing";
            return false;
        }
        decoded.manifest.sections.reserve(section_count);
        for (std::uint32_t index = 0; index < section_count; ++index) {
            sandbox::host::TosImageSection section;
            if (!readString(payload, offset, section.name) ||
                !readString(payload, offset, section.path) ||
                !readString(payload, offset, section.kind) ||
                !readPod(payload, offset, section.load_address) ||
                !readPod(payload, offset, section.entry_pc) ||
                !readPod(payload, offset, section.word_count) ||
                !readPod(payload, offset, section.page_count) ||
                !readPod(payload, offset, section.flags)) {
                error = "legacy boot section table is truncated";
                return false;
            }
            decoded.manifest.sections.push_back(std::move(section));
        }
    }

    std::uint32_t app_count = 0;
    if (!readPod(payload, offset, app_count)) {
        error = "legacy boot app registry is missing";
        return false;
    }
    decoded.manifest.apps.reserve(app_count);
    for (std::uint32_t index = 0; index < app_count; ++index) {
        sandbox::host::TosAppManifestEntry entry;
        if (!readString(payload, offset, entry.name) ||
            !readString(payload, offset, entry.path) ||
            !readPod(payload, offset, entry.text_ppn) ||
            !readPod(payload, offset, entry.entry_pc) ||
            !readPod(payload, offset, entry.text_pages) ||
            !readPod(payload, offset, entry.data_pages) ||
            !readPod(payload, offset, entry.stack_words)) {
            error = "legacy boot app registry is truncated";
            return false;
        }
        entry.isa_version = 1;
        entry.required_features = 0;
        entry.function_abi_version = 1;
        entry.syscall_abi_version =
            kLegacySyscallAbiV1;
        decoded.manifest.apps.push_back(std::move(entry));
    }

    std::uint32_t program_words = 0;
    if (!readPod(payload, offset, program_words)) {
        error = "legacy boot text segment is missing";
        return false;
    }
    decoded.program.assign(program_words, sandbox::isa::TritWord27{});
    for (std::uint32_t index = 0; index < program_words; ++index) {
        if (!readPod(
                payload, offset,
                decoded.program[static_cast<std::size_t>(index)].bits)) {
            error = "legacy boot text segment is truncated";
            return false;
        }
    }

    std::uint32_t data_words = 0;
    if (!readPod(payload, offset, data_words)) {
        error = "legacy boot data segment is missing";
        return false;
    }
    decoded.data_words.assign(data_words, 0);
    for (std::uint32_t index = 0; index < data_words; ++index) {
        std::int64_t word = 0;
        if (!readPod(payload, offset, word)) {
            error = "legacy boot data segment is truncated";
            return false;
        }
        decoded.data_words[static_cast<std::size_t>(index)] =
            static_cast<long long>(word);
    }

    if (version == kLegacyBootV1) {
        std::uint32_t rootfs_words = 0;
        if (!readPod(payload, offset, rootfs_words)) {
            error = "legacy boot root filesystem seed is missing";
            return false;
        }
        decoded.rootfs_words.assign(rootfs_words, 0);
        for (std::uint32_t index = 0; index < rootfs_words; ++index) {
            std::int64_t word = 0;
            if (!readPod(payload, offset, word)) {
                error = "legacy boot root filesystem seed is truncated";
                return false;
            }
            decoded.rootfs_words[static_cast<std::size_t>(index)] =
                static_cast<long long>(word);
        }
    }

    if (offset != payload.size()) {
        error = "legacy boot has trailing payload bytes";
        return false;
    }
    image = std::move(decoded);
    return true;
}

bool readLegacyBootImageFile(
    const std::filesystem::path& source,
    TosBootImage& image,
    std::string& error) {

    std::ifstream file(source, std::ios::binary);
    if (!file.good()) {
        error = "failed to open legacy boot image: " + source.string();
        return false;
    }
    std::uint64_t magic = 0;
    std::uint64_t checksum = 0;
    std::uint64_t payload_size = 0;
    file.read(reinterpret_cast<char*>(&magic), sizeof(magic));
    file.read(reinterpret_cast<char*>(&checksum), sizeof(checksum));
    file.read(reinterpret_cast<char*>(&payload_size), sizeof(payload_size));
    if (!file.good() || magic != sandbox::host::TOS_BOOT_MAGIC ||
        payload_size > (1ULL << 34)) {
        error = "legacy boot container header is invalid";
        return false;
    }
    std::vector<std::uint8_t> payload(
        static_cast<std::size_t>(payload_size));
    file.read(
        reinterpret_cast<char*>(payload.data()),
        static_cast<std::streamsize>(payload.size()));
    if (!file.good() ||
        sandbox::host::detail::fnv1a(payload) != checksum) {
        error = "legacy boot payload is truncated or checksum-invalid";
        return false;
    }
    return readLegacyBootPayload(payload, image, error);
}

bool migrateBoot(
    const std::filesystem::path& legacy_source,
    const std::filesystem::path& v2_template_source,
    const std::filesystem::path& destination,
    TosBootImage& legacy,
    TosBootImage& migrated,
    std::string& error) {

    if (!readLegacyBootImageFile(legacy_source, legacy, error)) {
        return false;
    }
    if (legacy.manifest.format_version >=
        sandbox::host::TOS_BOOT_FORMAT_VERSION) {
        error = "legacy boot input is already tboot v3; no migration is needed";
        return false;
    }
    if (!sandbox::host::readBootImageFile(
            v2_template_source.string(), migrated, &error)) {
        return false;
    }
    if (migrated.manifest.format_version !=
            sandbox::host::TOS_BOOT_FORMAT_VERSION ||
        migrated.manifest.isa_version !=
            sandbox::architecture::v2::ISA_VERSION ||
        migrated.manifest.function_abi_version !=
            sandbox::architecture::v2::FUNCTION_ABI_VERSION ||
        migrated.manifest.syscall_abi_version !=
            sandbox::architecture::v2::SYSCALL_ABI_VERSION ||
        migrated.manifest.scalar_word_trits !=
            sandbox::architecture::v2::SCALAR_WORD_TRITS ||
        migrated.manifest.base_page_words !=
            sandbox::architecture::v2::BASE_PAGE_WORDS) {
        error = "boot template is not a complete v2/tboot-v3 release image";
        return false;
    }

    // The output deliberately remains the v2 template.  In particular, no
    // instruction or executable section from the legacy container crosses
    // this boundary.
    const auto template_program = migrated.program;
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
        validation.manifest.isa_version !=
            sandbox::architecture::v2::ISA_VERSION ||
        validation.program != template_program) {
        std::filesystem::remove(temporary, ec);
        if (error.empty()) {
            error = "migrated boot image failed v2 template validation";
        }
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
    std::uint64_t expected_checksum = 0;
    if (magic == kLegacySparseDiskMagic) {
        file.read(reinterpret_cast<char*>(&count), sizeof(count));
    } else if (magic == sandbox::host::TOS_SPARSE_DISK_MAGIC) {
        std::uint32_t version = 0;
        std::uint32_t block_words = 0;
        std::uint64_t generation = 0;
        file.read(reinterpret_cast<char*>(&version), sizeof(version));
        file.read(reinterpret_cast<char*>(&block_words), sizeof(block_words));
        file.read(reinterpret_cast<char*>(&generation), sizeof(generation));
        file.read(
            reinterpret_cast<char*>(&expected_checksum),
            sizeof(expected_checksum));
        file.read(reinterpret_cast<char*>(&count), sizeof(count));
        (void)generation;
        if (version != sandbox::host::TOS_SPARSE_DISK_VERSION ||
            block_words != kBlockWords) {
            error = "disk v2 architecture metadata is invalid";
            return false;
        }
        raw_v2 = true;
    } else {
        std::ostringstream message;
        message << "unsupported disk image magic 0x" << std::hex << magic;
        error = message.str();
        return false;
    }
    if (!file.good() || count < 0) {
        error = "disk image record count is invalid";
        return false;
    }

    if (raw_v2) {
        const std::streampos records_begin = file.tellg();
        file.seekg(0, std::ios::end);
        const std::streampos records_end = file.tellg();
        const auto bytes = static_cast<std::size_t>(
            records_end - records_begin);
        const std::size_t expected_bytes =
            static_cast<std::size_t>(count) *
            (sizeof(std::int32_t) +
             static_cast<std::size_t>(kBlockWords) *
                 sizeof(std::uint64_t));
        if (bytes != expected_bytes) {
            error = "disk v2 record payload length is invalid";
            return false;
        }
        std::vector<std::uint8_t> records(bytes);
        file.seekg(records_begin);
        file.read(
            reinterpret_cast<char*>(records.data()),
            static_cast<std::streamsize>(records.size()));
        if (!file.good() ||
            sandbox::host::detail::fnv1a(records) != expected_checksum) {
            error = "disk v2 record checksum is invalid";
            return false;
        }
        file.clear();
        file.seekg(records_begin);
    }

    struct Record {
        int block = 0;
        std::vector<long long> words;
    };
    std::vector<Record> records;
    records.reserve(static_cast<std::size_t>(count));
    int maximum_block = 0;
    std::set<int> occupied;
    for (int record_index = 0; record_index < count; ++record_index) {
        Record record;
        record.words.assign(kBlockWords, 0);
        file.read(reinterpret_cast<char*>(&record.block), sizeof(record.block));
        for (int word = 0; word < kBlockWords; ++word) {
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
                file.read(
                    reinterpret_cast<char*>(
                        &record.words[static_cast<std::size_t>(word)]),
                    sizeof(long long));
            }
        }
        if (!file.good() || record.block < 0 ||
            !occupied.insert(record.block).second) {
            error = "disk image record is truncated, invalid, or duplicated";
            return false;
        }
        maximum_block = std::max(maximum_block, record.block);
        records.push_back(std::move(record));
    }

    dense.assign(
        static_cast<std::size_t>(maximum_block + 1) * kBlockWords,
        0);
    for (const auto& record : records) {
        std::copy(
            record.words.begin(), record.words.end(),
            dense.begin() +
                static_cast<std::ptrdiff_t>(record.block * kBlockWords));
    }
    return true;
}

bool readDenseRange(
    const std::vector<long long>& dense,
    int first_block,
    int block_count,
    std::vector<long long>& out,
    std::string& error) {

    const std::size_t begin =
        static_cast<std::size_t>(first_block) * kBlockWords;
    const std::size_t count =
        static_cast<std::size_t>(block_count) * kBlockWords;
    if (begin > dense.size() || count > dense.size() - begin) {
        error = "disk image is shorter than its native VFS geometry";
        return false;
    }
    out.assign(
        dense.begin() + static_cast<std::ptrdiff_t>(begin),
        dense.begin() + static_cast<std::ptrdiff_t>(begin + count));
    return true;
}

bool parseNativeSnapshot(
    const std::vector<long long>& dense,
    NativeSnapshot& snapshot,
    std::string& error) {

    if (dense.size() <
        static_cast<std::size_t>(
            sandbox::os::NATIVE_VFS_DISK_DATA_BLOCK +
            sandbox::os::NATIVE_VFS_DISK_DATA_BLOCKS) *
            kBlockWords) {
        error = "disk is too small for the native VFS";
        return false;
    }
    const auto wordAt = [&](int block, int word) {
        return dense[static_cast<std::size_t>(block * kBlockWords + word)];
    };
    const long long native_vfs_version =
        wordAt(sandbox::os::NATIVE_VFS_DISK_SUPER_BLOCK, 1);
    if (wordAt(sandbox::os::NATIVE_VFS_DISK_SUPER_BLOCK, 0) !=
            sandbox::os::NATIVE_VFS_MAGIC ||
        (native_vfs_version != sandbox::os::NATIVE_VFS_LEGACY_VERSION &&
         native_vfs_version != sandbox::os::NATIVE_VFS_VERSION) ||
        wordAt(sandbox::os::NATIVE_VFS_DISK_SUPER_BLOCK, 2) != kBlockWords ||
        wordAt(sandbox::os::NATIVE_VFS_DISK_SUPER_BLOCK, 3) !=
            sandbox::os::NATIVE_VFS_MAX_INODES ||
        wordAt(sandbox::os::NATIVE_VFS_DISK_SUPER_BLOCK, 4) !=
            sandbox::os::NATIVE_VFS_MAX_DIRENTS ||
        wordAt(sandbox::os::NATIVE_VFS_DISK_SUPER_BLOCK, 5) !=
            sandbox::os::NATIVE_VFS_MAX_EXTENTS ||
        wordAt(sandbox::os::NATIVE_VFS_DISK_SUPER_BLOCK, 6) !=
            sandbox::os::NATIVE_VFS_PAYLOAD_WORDS) {
        error = "legacy disk native VFS superblock is incompatible";
        return false;
    }
    const int next_inode =
        static_cast<int>(wordAt(
            sandbox::os::NATIVE_VFS_DISK_SUPER_BLOCK, 8));
    const int next_dirent =
        static_cast<int>(wordAt(
            sandbox::os::NATIVE_VFS_DISK_SUPER_BLOCK, 9));
    const int next_extent =
        static_cast<int>(wordAt(
            sandbox::os::NATIVE_VFS_DISK_SUPER_BLOCK, 10));
    if (next_inode < 1 ||
        next_inode > sandbox::os::NATIVE_VFS_MAX_INODES ||
        next_dirent < 0 ||
        next_dirent > sandbox::os::NATIVE_VFS_MAX_DIRENTS ||
        next_extent < 0 ||
        next_extent > sandbox::os::NATIVE_VFS_MAX_EXTENTS) {
        error = "native VFS allocation cursors are invalid";
        return false;
    }

    std::vector<long long> inodes;
    std::vector<long long> dirents;
    std::vector<long long> names;
    std::vector<long long> extents;
    std::vector<long long> data;
    if (!readDenseRange(
            dense, sandbox::os::NATIVE_VFS_DISK_INODE_BLOCK,
            sandbox::os::NATIVE_VFS_DISK_INODE_BLOCKS, inodes, error) ||
        !readDenseRange(
            dense, sandbox::os::NATIVE_VFS_DISK_DIRENT_BLOCK,
            sandbox::os::NATIVE_VFS_DISK_DIRENT_BLOCKS, dirents, error) ||
        !readDenseRange(
            dense, sandbox::os::NATIVE_VFS_DISK_DIRENT_NAME_BLOCK,
            sandbox::os::NATIVE_VFS_DISK_DIRENT_NAME_BLOCKS, names, error) ||
        !readDenseRange(
            dense, sandbox::os::NATIVE_VFS_DISK_EXTENT_BLOCK,
            sandbox::os::NATIVE_VFS_DISK_EXTENT_BLOCKS, extents, error) ||
        !readDenseRange(
            dense, sandbox::os::NATIVE_VFS_DISK_DATA_BLOCK,
            sandbox::os::NATIVE_VFS_DISK_DATA_BLOCKS, data, error)) {
        return false;
    }

    std::map<int, std::string> paths = {{0, "/"}};
    std::set<int> unresolved;
    for (int slot = 0; slot < next_dirent; ++slot) unresolved.insert(slot);
    bool progress = true;
    while (progress && !unresolved.empty()) {
        progress = false;
        for (auto it = unresolved.begin(); it != unresolved.end();) {
            const int slot = *it;
            const int base =
                slot * sandbox::os::NATIVE_VFS_DIRENT_WORDS;
            const int parent =
                static_cast<int>(dirents[static_cast<std::size_t>(base + 1)]);
            const int length =
                static_cast<int>(dirents[static_cast<std::size_t>(base + 3)]);
            const int child =
                static_cast<int>(dirents[static_cast<std::size_t>(base + 4)]);
            const long long version =
                dirents[static_cast<std::size_t>(base + 5)];
            const auto parent_path = paths.find(parent);
            if (version <= 0) {
                it = unresolved.erase(it);
                continue;
            }
            if (parent_path == paths.end()) {
                ++it;
                continue;
            }
            if (child <= 0 || child >= next_inode ||
                length <= 0 ||
                length > sandbox::os::NATIVE_VFS_MAX_NAME_WORDS) {
                error = "native VFS directory entry is invalid";
                return false;
            }
            std::string name;
            for (int index = 0; index < length; ++index) {
                const long long value =
                    names[static_cast<std::size_t>(
                        slot * sandbox::os::NATIVE_VFS_MAX_NAME_WORDS +
                        index)];
                if (value <= 0 || value > 255) {
                    error = "native VFS filename contains an invalid byte";
                    return false;
                }
                name.push_back(static_cast<char>(value));
            }
            paths[child] =
                parent_path->second == "/"
                    ? "/" + name
                    : parent_path->second + "/" + name;
            it = unresolved.erase(it);
            progress = true;
        }
    }
    if (!unresolved.empty()) {
        error = "native VFS directory graph is disconnected or cyclic";
        return false;
    }

    snapshot.entries.clear();
    for (int inode = 1; inode < next_inode; ++inode) {
        const int base = inode * sandbox::os::NATIVE_VFS_INODE_WORDS;
        const int kind =
            static_cast<int>(inodes[static_cast<std::size_t>(base)]);
        if (kind == 0) continue;
        const auto path = paths.find(inode);
        if (path == paths.end()) {
            error = "native VFS contains an unreachable inode";
            return false;
        }
        if (kind != sandbox::os::NATIVE_KIND_FILE &&
            kind != sandbox::os::NATIVE_KIND_DIR &&
            kind != sandbox::os::NATIVE_KIND_EXEC) {
            error = "native VFS inode has an unsupported kind";
            return false;
        }
        const int size =
            static_cast<int>(inodes[static_cast<std::size_t>(base + 2)]);
        if (size < 0 || size > sandbox::os::NATIVE_VFS_PAYLOAD_WORDS) {
            error = "native VFS inode size is invalid";
            return false;
        }
        NativeEntry entry;
        entry.path = path->second;
        entry.inode = inode;
        entry.kind = kind;
        entry.payload.assign(static_cast<std::size_t>(size), 0);
        std::vector<bool> covered(static_cast<std::size_t>(size), false);
        for (int slot = 0; slot < next_extent; ++slot) {
            const int extent_base =
                slot * sandbox::os::NATIVE_VFS_EXTENT_WORDS;
            if (extents[static_cast<std::size_t>(extent_base + 5)] <= 0 ||
                extents[static_cast<std::size_t>(extent_base + 1)] != inode) {
                continue;
            }
            const int logical = static_cast<int>(
                extents[static_cast<std::size_t>(extent_base + 2)]);
            const int length = static_cast<int>(
                extents[static_cast<std::size_t>(extent_base + 3)]);
            const int data_address = static_cast<int>(
                extents[static_cast<std::size_t>(extent_base + 4)]);
            const int data_offset =
                data_address - sandbox::os::NATIVE_VFS_DATA_BASE;
            if (logical < 0 || length < 0 || logical + length > size ||
                data_offset < 0 ||
                data_offset + length >
                    sandbox::os::NATIVE_VFS_PAYLOAD_WORDS) {
                error = "native VFS extent is outside its inode or data area";
                return false;
            }
            for (int index = 0; index < length; ++index) {
                entry.payload[static_cast<std::size_t>(logical + index)] =
                    data[static_cast<std::size_t>(data_offset + index)];
                covered[static_cast<std::size_t>(logical + index)] = true;
            }
        }
        if (kind != sandbox::os::NATIVE_KIND_DIR &&
            std::find(covered.begin(), covered.end(), false) != covered.end()) {
            error = "native VFS file payload is not fully covered by extents";
            return false;
        }
        snapshot.entries.emplace(entry.path, std::move(entry));
    }
    return true;
}

bool decodeTemplateExecutable(
    const NativeEntry& entry,
    const std::vector<long long>& template_dense,
    TemplateExecutable& executable,
    std::string& error) {

    if (entry.payload.size() <
        static_cast<std::size_t>(
            sandbox::vm::EXEC_V2_HEADER_WORDS + 3)) {
        error = "v2 template executable descriptor is truncated: " +
                entry.path;
        return false;
    }
    std::vector<sandbox::vm::TernaryValue> encoded_v2;
    encoded_v2.reserve(sandbox::vm::EXEC_V2_HEADER_WORDS);
    for (int index = 0; index < sandbox::vm::EXEC_V2_HEADER_WORDS; ++index) {
        encoded_v2.push_back(
            sandbox::vm::ops::fromLong(
                entry.payload[static_cast<std::size_t>(index)]));
    }
    sandbox::vm::ExecutableImageHeaderV2 header_v2;
    if (!sandbox::vm::decodeExecutableHeaderV2(
            encoded_v2, 0, header_v2)) {
        error = "v2 template architecture header is invalid: " + entry.path;
        return false;
    }

    const int text_ppn = static_cast<int>(
        entry.payload[sandbox::vm::EXEC_V2_HEADER_WORDS]);
    const int disk_block = static_cast<int>(
        entry.payload[sandbox::vm::EXEC_V2_HEADER_WORDS + 1]);
    const int text_words = static_cast<int>(
        entry.payload[sandbox::vm::EXEC_V2_HEADER_WORDS + 2]);
    if (text_ppn <= 0 || disk_block < sandbox::os::NATIVE_VFS_REQUIRED_BLOCKS ||
        text_words <= 0 || text_words != header_v2.text_words) {
        error = "v2 template executable placement is invalid: " + entry.path;
        return false;
    }
    const std::size_t first =
        static_cast<std::size_t>(disk_block) * kBlockWords;
    if (first > template_dense.size() ||
        static_cast<std::size_t>(text_words) >
            template_dense.size() - first) {
        error = "v2 template executable text is outside the disk: " +
                entry.path;
        return false;
    }

    executable.path = entry.path;
    executable.source_inode = entry.inode;
    executable.text_ppn = text_ppn;
    executable.header_v2 = header_v2;
    executable.program.reserve(static_cast<std::size_t>(text_words));
    for (int index = 0; index < text_words; ++index) {
        executable.program.push_back({
            static_cast<std::uint64_t>(
                template_dense[first + static_cast<std::size_t>(index)])});
    }
    return true;
}

int pathDepth(const std::string& path) {
    return static_cast<int>(
        std::count(path.begin(), path.end(), '/'));
}

bool rebuildMigratedDisk(
    const std::vector<long long>& legacy_dense,
    const std::vector<long long>& template_dense,
    std::vector<long long>& migrated_dense,
    std::string& error) {

    NativeSnapshot legacy;
    NativeSnapshot v2_template;
    if (!parseNativeSnapshot(legacy_dense, legacy, error) ||
        !parseNativeSnapshot(template_dense, v2_template, error)) {
        return false;
    }

    std::map<std::string, NativeEntry> merged = v2_template.entries;
    std::vector<std::string> custom_executables;
    for (const auto& [path, entry] : legacy.entries) {
        if (entry.kind == sandbox::os::NATIVE_KIND_EXEC) {
            const auto replacement = v2_template.entries.find(path);
            if (replacement == v2_template.entries.end() ||
                replacement->second.kind != sandbox::os::NATIVE_KIND_EXEC) {
                custom_executables.push_back(path);
            }
            continue;
        }
        const auto replacement = v2_template.entries.find(path);
        if (replacement != v2_template.entries.end() &&
            replacement->second.kind == sandbox::os::NATIVE_KIND_EXEC) {
            error = "legacy non-executable conflicts with a v2 executable: " +
                    path;
            return false;
        }
        merged[path] = entry;
    }
    if (!custom_executables.empty()) {
        std::ostringstream message;
        message
            << "legacy disk contains executables that require source rebuild:";
        for (const auto& path : custom_executables) message << " " << path;
        error = message.str();
        return false;
    }

    std::vector<TemplateExecutable> executables;
    for (const auto& [path, entry] : v2_template.entries) {
        if (entry.kind != sandbox::os::NATIVE_KIND_EXEC) continue;
        TemplateExecutable executable;
        if (!decodeTemplateExecutable(
                entry, template_dense, executable, error)) {
            return false;
        }
        executables.push_back(std::move(executable));
    }
    std::sort(
        executables.begin(), executables.end(),
        [](const TemplateExecutable& left,
           const TemplateExecutable& right) {
            return left.source_inode < right.source_inode;
        });

    const int template_blocks = static_cast<int>(
        template_dense.size() / kBlockWords);
    sandbox::os::NativeVfsImageBuilder builder(template_blocks);
    if (!builder.status().ok()) {
        error = "failed to initialize the v2 native VFS builder";
        return false;
    }

    std::vector<NativeEntry> directories;
    std::vector<NativeEntry> files;
    for (const auto& [path, entry] : merged) {
        if (entry.kind == sandbox::os::NATIVE_KIND_DIR) {
            directories.push_back(entry);
        } else if (entry.kind == sandbox::os::NATIVE_KIND_FILE) {
            files.push_back(entry);
        }
    }
    const auto pathOrder = [](const NativeEntry& left,
                              const NativeEntry& right) {
        const int left_depth = pathDepth(left.path);
        const int right_depth = pathDepth(right.path);
        return left_depth == right_depth
                   ? left.path < right.path
                   : left_depth < right_depth;
    };
    std::sort(directories.begin(), directories.end(), pathOrder);
    std::sort(files.begin(), files.end(), pathOrder);
    for (const auto& directory : directories) {
        const auto made = builder.mkdir(directory.path);
        if (!made.ok()) {
            error = "failed to migrate directory: " + directory.path;
            return false;
        }
    }
    for (const auto& file : files) {
        const auto added = builder.addFile(file.path, file.payload);
        if (!added.ok()) {
            error = "failed to migrate file: " + file.path;
            return false;
        }
    }
    for (const auto& executable : executables) {
        const auto added = builder.addExecutableImage(
            executable.path,
            executable.program,
            executable.header_v2,
            executable.text_ppn);
        if (!added.ok()) {
            error = "failed to install rebuilt v2 executable: " +
                    executable.path;
            return false;
        }
    }

    migrated_dense = builder.image();
    NativeSnapshot validation;
    if (!parseNativeSnapshot(migrated_dense, validation, error)) return false;
    for (const auto& [path, entry] : legacy.entries) {
        if (entry.kind == sandbox::os::NATIVE_KIND_EXEC) continue;
        const auto migrated = validation.entries.find(path);
        if (migrated == validation.entries.end() ||
            migrated->second.kind != entry.kind ||
            migrated->second.payload != entry.payload) {
            error = "migrated VFS content mismatch: " + path;
            return false;
        }
    }
    return true;
}

bool migrateDisk(
    const std::filesystem::path& legacy_source,
    const std::filesystem::path& v2_template_source,
    const std::filesystem::path& destination,
    const std::vector<long long>& embedded_seed,
    std::string& error) {

    std::vector<long long> legacy_dense = embedded_seed;
    if (!legacy_source.empty()) {
        if (!readDiskDense(legacy_source, legacy_dense, error)) return false;
    }
    if (legacy_dense.empty()) {
        error =
            "no legacy disk or embedded root filesystem seed is available";
        return false;
    }
    std::vector<long long> template_dense;
    if (!readDiskDense(v2_template_source, template_dense, error)) {
        return false;
    }

    std::vector<long long> migrated_dense;
    if (!rebuildMigratedDisk(
            legacy_dense, template_dense, migrated_dense, error)) {
        return false;
    }
    // Builders expose host integers, while tDisk v2 stores architectural T40
    // words.  Normalize before validation so host-only sentinel values and
    // wide hashes are compared in the same representation the guest will
    // actually read.
    for (long long& word : migrated_dense) {
        word = sandbox::vm::ops::toLong(
            sandbox::vm::convertValue(
                sandbox::vm::ops::fromLong(word),
                sandbox::TernaryMode::T40));
    }
    // The sparse tDisk container intentionally omits trailing zero blocks and
    // carries no logical block-count field.  Canonicalize the dense image to
    // its last nonzero block before round-trip comparison.
    while (migrated_dense.size() > static_cast<std::size_t>(kBlockWords)) {
        const auto block_begin =
            migrated_dense.end() - kBlockWords;
        if (std::find_if(
                block_begin, migrated_dense.end(),
                [](long long word) { return word != 0; }) !=
            migrated_dense.end()) {
            break;
        }
        migrated_dense.erase(block_begin, migrated_dense.end());
    }

    std::filesystem::path temporary = destination;
    temporary += ".migrating";
    std::error_code ec;
    std::filesystem::remove(temporary, ec);
    if (!sandbox::host::writeSparseDiskFile(
            temporary.string(), migrated_dense, true, &error)) {
        return false;
    }

    std::vector<long long> validation;
    if (!readDiskDense(temporary, validation, error) ||
        validation != migrated_dense) {
        std::filesystem::remove(temporary, ec);
        if (error.empty()) {
            std::ostringstream message;
            message << "migrated disk validation failed"
                    << " (expected_words=" << migrated_dense.size()
                    << ", actual_words=" << validation.size();
            const std::size_t common =
                std::min(validation.size(), migrated_dense.size());
            const auto mismatch = std::mismatch(
                migrated_dense.begin(),
                migrated_dense.begin() +
                    static_cast<std::ptrdiff_t>(common),
                validation.begin());
            if (mismatch.first !=
                migrated_dense.begin() +
                    static_cast<std::ptrdiff_t>(common)) {
                message << ", first_mismatch="
                        << std::distance(
                               migrated_dense.begin(), mismatch.first)
                        << ", expected=" << *mismatch.first
                        << ", actual=" << *mismatch.second;
            }
            message << ")";
            error = message.str();
        }
        return false;
    }
    NativeSnapshot snapshot;
    if (!parseNativeSnapshot(validation, snapshot, error)) {
        std::filesystem::remove(temporary, ec);
        return false;
    }
    return replaceValidatedFile(temporary, destination, error);
}

}  // namespace

int main(int argc, char** argv) {
    sandbox::LongTriple::initPowTable();
    Options options;
    std::string error;
    if (!parseOptions(argc, argv, options, error)) {
        if (!error.empty()) std::cerr << error << '\n';
        printUsage(argv[0]);
        return error.empty() ? 0 : 2;
    }

    TosBootImage legacy;
    TosBootImage migrated;
    if (!migrateBoot(
            options.legacy_boot,
            options.v2_boot_template,
            options.output_boot,
            legacy,
            migrated,
            error)) {
        std::cerr << error << '\n';
        return 1;
    }

    if (!options.output_disk.empty() &&
        !migrateDisk(
            options.legacy_disk,
            options.v2_disk_template,
            options.output_disk,
            legacy.rootfs_words,
            error)) {
        std::cerr << error << '\n';
        return 1;
    }

    std::cout << "migrated boot image to "
              << options.output_boot.string() << '\n';
    if (!options.output_disk.empty()) {
        std::cout << "migrated disk image to "
                  << options.output_disk.string() << '\n';
    }
    return 0;
}
