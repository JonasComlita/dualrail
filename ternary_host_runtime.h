#pragma once
#ifndef TERNARY_HOST_RUNTIME_H
#define TERNARY_HOST_RUNTIME_H

#include "ternary_asm.h"
#include "ternary_vm.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <limits>
#include <memory>
#include <mutex>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

namespace sandbox {
namespace host {

inline constexpr std::uint64_t TOS_BOOT_MAGIC = 0x31544f4f424f5354ULL; // "TSOBOOT1"
inline constexpr std::uint32_t TOS_BOOT_FORMAT_VERSION = 1;
inline constexpr std::uint64_t TOS_SPARSE_DISK_MAGIC = 0x54524954535031ULL; // "TRITSP1"

struct TosAppManifestEntry {
    std::string name;
    std::string path;
    int text_ppn = 0;
    int entry_pc = 0;
    int text_pages = 0;
    int data_pages = 0;
    int stack_words = 0;
};

struct TosImageManifest {
    std::string image_version = "dev";
    std::string profile_name = "minimum";
    int boot_entry = 0;
    int framebuffer_width = 80;
    int framebuffer_height = 60;
    std::vector<TosAppManifestEntry> apps;
};

struct TosBootImage {
    TosImageManifest manifest;
    std::vector<isa::TritWord27> program;
    std::vector<long long> data_words;
    std::vector<long long> rootfs_words;
};

struct TosRuntimeConfig {
    std::string boot_image_path;
    std::string disk_path;
    std::string profile_name = "minimum";
    bool start_paused = false;
    bool debug_overlay = false;
};

enum class TosFramebufferMode {
    Text80x25,
    Graphics80x60,
};

struct TosFramebufferSnapshot {
    TosFramebufferMode mode = TosFramebufferMode::Text80x25;
    int width = 80;
    int height = 25;
    std::vector<char> glyphs;
    std::vector<std::uint32_t> rgba;
    long long sprite_x = 0;
    long long sprite_y = 0;
    long long sprite_attr = 0;
};

struct TosRuntimeSnapshot {
    int pc = 0;
    vm::VMStatus status = vm::VMStatus::HALTED;
    long long cycles = 0;
    isa::PrivilegeMode privilege = isa::PrivilegeMode::Kernel;
    long long gpu_mode = 0;
    std::string disk_path;
    std::string image_version;
    std::size_t allocated_disk_blocks = 0;
};

namespace detail {

inline void setError(std::string* error, const std::string& message) {
    if (error) *error = message;
}

template <typename T>
inline void appendPod(std::vector<std::uint8_t>& out, T value) {
    const auto* raw = reinterpret_cast<const std::uint8_t*>(&value);
    out.insert(out.end(), raw, raw + sizeof(T));
}

template <typename T>
inline bool readPod(const std::vector<std::uint8_t>& in, std::size_t& offset, T& out) {
    if (offset + sizeof(T) > in.size()) return false;
    std::copy(in.begin() + static_cast<std::ptrdiff_t>(offset),
              in.begin() + static_cast<std::ptrdiff_t>(offset + sizeof(T)),
              reinterpret_cast<std::uint8_t*>(&out));
    offset += sizeof(T);
    return true;
}

inline bool checkedSize(std::size_t size) {
    return size <= static_cast<std::size_t>(std::numeric_limits<std::uint32_t>::max());
}

inline bool appendString(std::vector<std::uint8_t>& out, const std::string& value) {
    if (!checkedSize(value.size())) return false;
    appendPod<std::uint32_t>(out, static_cast<std::uint32_t>(value.size()));
    out.insert(out.end(), value.begin(), value.end());
    return true;
}

inline bool readString(const std::vector<std::uint8_t>& in,
                       std::size_t& offset,
                       std::string& out) {
    std::uint32_t size = 0;
    if (!readPod(in, offset, size)) return false;
    if (offset + size > in.size()) return false;
    out.assign(reinterpret_cast<const char*>(in.data() + offset),
               reinterpret_cast<const char*>(in.data() + offset + size));
    offset += size;
    return true;
}

inline std::uint64_t fnv1a(const std::vector<std::uint8_t>& data) {
    std::uint64_t hash = 1469598103934665603ULL;
    for (std::uint8_t byte : data) {
        hash ^= byte;
        hash *= 1099511628211ULL;
    }
    return hash;
}

inline bool appendManifestEntry(std::vector<std::uint8_t>& out,
                                const TosAppManifestEntry& entry) {
    if (!appendString(out, entry.name) || !appendString(out, entry.path)) return false;
    appendPod<std::int32_t>(out, entry.text_ppn);
    appendPod<std::int32_t>(out, entry.entry_pc);
    appendPod<std::int32_t>(out, entry.text_pages);
    appendPod<std::int32_t>(out, entry.data_pages);
    appendPod<std::int32_t>(out, entry.stack_words);
    return true;
}

inline bool readManifestEntry(const std::vector<std::uint8_t>& in,
                              std::size_t& offset,
                              TosAppManifestEntry& entry) {
    return readString(in, offset, entry.name) &&
           readString(in, offset, entry.path) &&
           readPod(in, offset, entry.text_ppn) &&
           readPod(in, offset, entry.entry_pc) &&
           readPod(in, offset, entry.text_pages) &&
           readPod(in, offset, entry.data_pages) &&
           readPod(in, offset, entry.stack_words);
}

inline std::vector<std::uint8_t> serializePayload(const TosBootImage& image) {
    std::vector<std::uint8_t> out;
    appendPod<std::uint32_t>(out, TOS_BOOT_FORMAT_VERSION);
    appendPod<std::int32_t>(out, image.manifest.boot_entry);
    appendPod<std::int32_t>(out, image.manifest.framebuffer_width);
    appendPod<std::int32_t>(out, image.manifest.framebuffer_height);
    if (!appendString(out, image.manifest.profile_name) ||
        !appendString(out, image.manifest.image_version)) {
        return {};
    }

    if (!checkedSize(image.manifest.apps.size()) ||
        !checkedSize(image.program.size()) ||
        !checkedSize(image.data_words.size()) ||
        !checkedSize(image.rootfs_words.size())) {
        return {};
    }

    appendPod<std::uint32_t>(out, static_cast<std::uint32_t>(image.manifest.apps.size()));
    for (const TosAppManifestEntry& entry : image.manifest.apps) {
        if (!appendManifestEntry(out, entry)) return {};
    }

    appendPod<std::uint32_t>(out, static_cast<std::uint32_t>(image.program.size()));
    for (const isa::TritWord27& word : image.program) {
        appendPod<std::uint64_t>(out, word.bits);
    }

    appendPod<std::uint32_t>(out, static_cast<std::uint32_t>(image.data_words.size()));
    for (long long word : image.data_words) {
        appendPod<std::int64_t>(out, static_cast<std::int64_t>(word));
    }

    appendPod<std::uint32_t>(out, static_cast<std::uint32_t>(image.rootfs_words.size()));
    for (long long word : image.rootfs_words) {
        appendPod<std::int64_t>(out, static_cast<std::int64_t>(word));
    }
    return out;
}

inline bool deserializePayload(const std::vector<std::uint8_t>& payload,
                               TosBootImage& image,
                               std::string* error) {
    std::size_t offset = 0;
    std::uint32_t version = 0;
    if (!readPod(payload, offset, version)) {
        setError(error, "boot image payload is truncated");
        return false;
    }
    if (version != TOS_BOOT_FORMAT_VERSION) {
        setError(error, "unsupported boot image format version " + std::to_string(version));
        return false;
    }

    if (!readPod(payload, offset, image.manifest.boot_entry) ||
        !readPod(payload, offset, image.manifest.framebuffer_width) ||
        !readPod(payload, offset, image.manifest.framebuffer_height) ||
        !readString(payload, offset, image.manifest.profile_name) ||
        !readString(payload, offset, image.manifest.image_version)) {
        setError(error, "boot image manifest is truncated");
        return false;
    }

    std::uint32_t app_count = 0;
    if (!readPod(payload, offset, app_count)) {
        setError(error, "boot image app registry is missing");
        return false;
    }
    image.manifest.apps.clear();
    image.manifest.apps.reserve(app_count);
    for (std::uint32_t i = 0; i < app_count; ++i) {
        TosAppManifestEntry entry;
        if (!readManifestEntry(payload, offset, entry)) {
            setError(error, "boot image app registry is truncated");
            return false;
        }
        image.manifest.apps.push_back(std::move(entry));
    }

    std::uint32_t program_words = 0;
    if (!readPod(payload, offset, program_words)) {
        setError(error, "boot image text segment is missing");
        return false;
    }
    image.program.assign(program_words, isa::TritWord27{});
    for (std::uint32_t i = 0; i < program_words; ++i) {
        std::uint64_t bits = 0;
        if (!readPod(payload, offset, bits)) {
            setError(error, "boot image text segment is truncated");
            return false;
        }
        image.program[static_cast<std::size_t>(i)].bits = bits;
    }

    std::uint32_t data_words = 0;
    if (!readPod(payload, offset, data_words)) {
        setError(error, "boot image data segment is missing");
        return false;
    }
    image.data_words.assign(data_words, 0);
    for (std::uint32_t i = 0; i < data_words; ++i) {
        std::int64_t word = 0;
        if (!readPod(payload, offset, word)) {
            setError(error, "boot image data segment is truncated");
            return false;
        }
        image.data_words[static_cast<std::size_t>(i)] = static_cast<long long>(word);
    }

    std::uint32_t rootfs_words = 0;
    if (!readPod(payload, offset, rootfs_words)) {
        setError(error, "boot image root filesystem seed is missing");
        return false;
    }
    image.rootfs_words.assign(rootfs_words, 0);
    for (std::uint32_t i = 0; i < rootfs_words; ++i) {
        std::int64_t word = 0;
        if (!readPod(payload, offset, word)) {
            setError(error, "boot image root filesystem seed is truncated");
            return false;
        }
        image.rootfs_words[static_cast<std::size_t>(i)] = static_cast<long long>(word);
    }

    if (offset != payload.size()) {
        setError(error, "boot image has trailing payload bytes");
        return false;
    }
    return true;
}

inline bool parentDirectoryExistsOrCreate(const std::string& path, std::string* error) {
    const std::filesystem::path fs_path(path);
    const std::filesystem::path parent = fs_path.parent_path();
    if (parent.empty()) return true;
    std::error_code ec;
    std::filesystem::create_directories(parent, ec);
    if (ec) {
        setError(error, "failed to create directory '" + parent.string() + "': " + ec.message());
        return false;
    }
    return true;
}

inline bool isZeroBlock(const std::vector<long long>& image, int block) {
    const int base = block * vm::MMU_PAGE_WORDS;
    for (int i = 0; i < vm::MMU_PAGE_WORDS; ++i) {
        if (image[static_cast<std::size_t>(base + i)] != 0) return false;
    }
    return true;
}

inline std::vector<int> nonZeroBlocks(const std::vector<long long>& image) {
    std::vector<int> blocks;
    if (image.empty() || static_cast<int>(image.size()) % vm::MMU_PAGE_WORDS != 0) {
        return blocks;
    }
    const int block_count = static_cast<int>(image.size()) / vm::MMU_PAGE_WORDS;
    for (int block = 0; block < block_count; ++block) {
        if (!isZeroBlock(image, block)) blocks.push_back(block);
    }
    return blocks;
}

inline std::uint32_t rgba(int r, int g, int b, int a = 255) {
    return (static_cast<std::uint32_t>(r & 0xff) << 24) |
           (static_cast<std::uint32_t>(g & 0xff) << 16) |
           (static_cast<std::uint32_t>(b & 0xff) << 8) |
           static_cast<std::uint32_t>(a & 0xff);
}

inline std::uint32_t paletteColor(int index) {
    switch (index & 0x0f) {
        case 0:  return rgba(5, 8, 20);      // Midnight blue-black (background)
        case 1:  return rgba(0, 220, 85);    // Warm phosphor green (primary text)
        case 2:  return rgba(0, 100, 30);    // Shadow green
        case 3:  return rgba(255, 165, 30);  // Gold amber (status)
        case 4:  return rgba(100, 50, 0);    // Shadow amber
        case 5:  return rgba(0, 240, 255);   // Neon cyan (accent)
        case 6:  return rgba(255, 0, 128);   // Hot pink
        case 7:  return rgba(215, 230, 255); // Cool white (backlit feel)
        case 8:  return rgba(40, 55, 80);    // Dim blue-grey (muted/borders)
        case 9:  return rgba(255, 80, 80);   // Neon red (danger)
        case 10: return rgba(0, 220, 85);    // Warm phosphor (desktop menu)
        case 11: return rgba(0, 240, 255);   // Neon cyan (title)
        case 12: return rgba(255, 0, 128);   // Hot pink
        case 13: return rgba(255, 165, 30);  // Gold amber
        case 14: return rgba(255, 255, 0);   // Bright yellow
        case 15: return rgba(215, 230, 255); // Cool white
        default: return rgba(0, 220, 85);
    }
}

inline long long dmemWord(const vm::VMState& machine, int addr) {
    auto [word, fault] = machine.dmem.load(addr);
    if (fault != vm::MemFaultCode::OK) return 0;
    return vm::ops::toLong(word);
}

constexpr int kKernelBootedAddr = 3000;
constexpr int kInputLastMouseBtnAddr = 3031;
constexpr int kInputLastMouseXAddr = 3034;
constexpr int kInputLastMouseYAddr = 3035;
constexpr int kProcessMax = 8;
constexpr int kProcessRowWords = 8;
constexpr int kProcState = 2;
constexpr int kProcRunnable = 1;
constexpr int kProcRunning = 2;
constexpr int kProcBlocked = 3;
constexpr int kProcSleeping = 4;
constexpr int kProcessBase = 9864;
constexpr int kWaitDeadlineBase = 27580;

inline bool inputPendingForWaiter(const vm::VMState& machine) {
    if (!machine.console_input.empty()) return true;
    if (machine.mouse_x != dmemWord(machine, kInputLastMouseXAddr)) return true;
    if (machine.mouse_y != dmemWord(machine, kInputLastMouseYAddr)) return true;
    return machine.mouse_btn != dmemWord(machine, kInputLastMouseBtnAddr);
}

inline bool canIdleWithoutStepping(const vm::VMState& machine) {
    if (dmemWord(machine, kKernelBootedAddr) <= 0) return false;
    if (inputPendingForWaiter(machine)) return false;

    bool saw_waiting = false;
    for (int slot = 0; slot < kProcessMax; ++slot) {
        const long long state =
            dmemWord(machine, kProcessBase + slot * kProcessRowWords + kProcState);
        if (state == kProcRunnable || state == kProcRunning) return false;
        if (state == kProcBlocked || state == kProcSleeping) {
            if (dmemWord(machine, kWaitDeadlineBase + slot) >= 0) return false;
            saw_waiting = true;
        }
    }
    return saw_waiting;
}

} // namespace detail

inline bool validateBootImage(const TosBootImage& image, std::string* error = nullptr) {
    if (image.program.empty()) {
        detail::setError(error, "boot image has no text segment");
        return false;
    }
    if (image.manifest.boot_entry < 0 ||
        image.manifest.boot_entry >= static_cast<int>(image.program.size())) {
        detail::setError(error, "boot image entry point is outside the text segment");
        return false;
    }
    if (image.manifest.profile_name.empty()) {
        detail::setError(error, "boot image profile name is empty");
        return false;
    }
    if (image.manifest.profile_name != "minimum" &&
        image.manifest.profile_name != "compact") {
        detail::setError(error, "unsupported VM profile '" + image.manifest.profile_name + "'");
        return false;
    }
    if (image.manifest.framebuffer_width <= 0 ||
        image.manifest.framebuffer_height <= 0) {
        detail::setError(error, "boot image framebuffer geometry is invalid");
        return false;
    }
    if (!image.rootfs_words.empty() &&
        static_cast<int>(image.rootfs_words.size()) % vm::MMU_PAGE_WORDS != 0) {
        detail::setError(error, "boot image root filesystem seed is not block aligned");
        return false;
    }
    return true;
}

inline bool writeBootImageFile(const std::string& path,
                               const TosBootImage& image,
                               std::string* error = nullptr) {
    if (!validateBootImage(image, error)) return false;
    if (!detail::parentDirectoryExistsOrCreate(path, error)) return false;

    std::vector<std::uint8_t> payload = detail::serializePayload(image);
    if (payload.empty()) {
        detail::setError(error, "boot image payload is too large to serialize");
        return false;
    }
    const std::uint64_t checksum = detail::fnv1a(payload);
    const std::uint64_t payload_size = static_cast<std::uint64_t>(payload.size());

    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    if (!out.good()) {
        detail::setError(error, "failed to open boot image for writing: " + path);
        return false;
    }
    out.write(reinterpret_cast<const char*>(&TOS_BOOT_MAGIC), sizeof(TOS_BOOT_MAGIC));
    out.write(reinterpret_cast<const char*>(&checksum), sizeof(checksum));
    out.write(reinterpret_cast<const char*>(&payload_size), sizeof(payload_size));
    out.write(reinterpret_cast<const char*>(payload.data()),
              static_cast<std::streamsize>(payload.size()));
    out.flush();
    if (!out.good()) {
        detail::setError(error, "failed to write boot image: " + path);
        return false;
    }
    return true;
}

inline bool readBootImageFile(const std::string& path,
                              TosBootImage& image,
                              std::string* error = nullptr) {
    std::ifstream in(path, std::ios::binary);
    if (!in.good()) {
        detail::setError(error, "failed to open boot image: " + path);
        return false;
    }

    std::uint64_t magic = 0;
    std::uint64_t checksum = 0;
    std::uint64_t payload_size = 0;
    in.read(reinterpret_cast<char*>(&magic), sizeof(magic));
    in.read(reinterpret_cast<char*>(&checksum), sizeof(checksum));
    in.read(reinterpret_cast<char*>(&payload_size), sizeof(payload_size));
    if (!in.good()) {
        detail::setError(error, "boot image header is truncated");
        return false;
    }
    if (magic != TOS_BOOT_MAGIC) {
        detail::setError(error, "boot image magic is invalid");
        return false;
    }
    if (payload_size > (1ULL << 34)) {
        detail::setError(error, "boot image payload is unreasonably large");
        return false;
    }

    std::vector<std::uint8_t> payload(static_cast<std::size_t>(payload_size));
    in.read(reinterpret_cast<char*>(payload.data()), static_cast<std::streamsize>(payload.size()));
    if (!in.good()) {
        detail::setError(error, "boot image payload is truncated");
        return false;
    }
    if (detail::fnv1a(payload) != checksum) {
        detail::setError(error, "boot image checksum validation failed");
        return false;
    }
    if (!detail::deserializePayload(payload, image, error)) return false;
    return validateBootImage(image, error);
}

inline TosBootImage bootImageFromAssembly(
    const vm::assembler::AssemblyResult& assembled,
    TosImageManifest manifest,
    const std::vector<long long>& rootfs_words = {}) {

    TosBootImage image;
    image.manifest = std::move(manifest);
    image.program = assembled.program;
    image.rootfs_words = rootfs_words;
    image.data_words.reserve(assembled.data.size());
    for (const vm::TernaryValue& value : assembled.data) {
        image.data_words.push_back(vm::ops::toLong(value));
    }
    return image;
}

inline vm::ProductionProfile profileForManifest(const TosImageManifest& manifest) {
    vm::ProductionProfile profile = vm::ProductionProfile::minimum();
    if (manifest.profile_name == "compact") {
        profile.ram_words = std::min(profile.ram_words, 1 << 20);
        profile.instruction_words = std::min(profile.instruction_words, 1 << 18);
        profile.disk_blocks = std::min(profile.disk_blocks, 16384);
    }
    profile.framebuffer_width = manifest.framebuffer_width;
    profile.framebuffer_height = manifest.framebuffer_height;
    profile.framebuffer_words = profile.framebuffer_width * profile.framebuffer_height;
    return profile;
}

inline bool writeSparseDiskFile(const std::string& path,
                                const std::vector<long long>& block_image,
                                bool overwrite = false,
                                std::string* error = nullptr) {
    if (block_image.empty() ||
        static_cast<int>(block_image.size()) % vm::MMU_PAGE_WORDS != 0) {
        detail::setError(error, "disk seed image is empty or not block aligned");
        return false;
    }
    if (!overwrite && std::filesystem::exists(path)) return true;
    if (!detail::parentDirectoryExistsOrCreate(path, error)) return false;

    const std::vector<int> blocks = detail::nonZeroBlocks(block_image);
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    if (!out.good()) {
        detail::setError(error, "failed to open sparse disk for writing: " + path);
        return false;
    }
    const std::uint64_t magic = TOS_SPARSE_DISK_MAGIC;
    const int count = static_cast<int>(blocks.size());
    out.write(reinterpret_cast<const char*>(&magic), sizeof(magic));
    out.write(reinterpret_cast<const char*>(&count), sizeof(count));
    for (int block : blocks) {
        out.write(reinterpret_cast<const char*>(&block), sizeof(block));
        const int base = block * vm::MMU_PAGE_WORDS;
        for (int word = 0; word < vm::MMU_PAGE_WORDS; ++word) {
            const long long value = block_image[static_cast<std::size_t>(base + word)];
            out.write(reinterpret_cast<const char*>(&value), sizeof(value));
        }
    }
    out.flush();
    if (!out.good()) {
        detail::setError(error, "failed to write sparse disk: " + path);
        return false;
    }
    return true;
}

inline bool loadBootImageIntoVm(vm::VMState& machine,
                                const TosBootImage& image,
                                const std::string& disk_path,
                                std::string* error = nullptr) {
    if (!validateBootImage(image, error)) return false;
    if (static_cast<int>(image.program.size()) > machine.imem.size()) {
        detail::setError(error, "boot image text segment exceeds VM instruction memory");
        return false;
    }
    if (static_cast<int>(image.data_words.size()) > machine.dmem.size()) {
        detail::setError(error, "boot image data segment exceeds VM data memory");
        return false;
    }

    machine.coldReset();
    if (!machine.imem.loadProgram(image.program, 0)) {
        detail::setError(error, "failed to load boot image text segment");
        return false;
    }
    for (int i = 0; i < static_cast<int>(image.data_words.size()); ++i) {
        if (machine.dmem.store(i, vm::ops::fromLong(image.data_words[static_cast<std::size_t>(i)])) !=
            vm::MemFaultCode::OK) {
            detail::setError(error, "failed to load boot image data segment");
            return false;
        }
    }
    machine.pc = image.manifest.boot_entry;
    machine.standalone_heap_break =
        std::max<long long>(machine.standalone_heap_break,
                            static_cast<long long>(image.data_words.size()) + 16);

    if (!disk_path.empty()) {
        if (!image.rootfs_words.empty() && !std::filesystem::exists(disk_path)) {
            if (!writeSparseDiskFile(disk_path, image.rootfs_words, false, error)) {
                return false;
            }
        }
        if (!machine.attachBlockBackingFile(disk_path)) {
            detail::setError(error, "failed to attach sparse disk backing: " + disk_path);
            return false;
        }
    } else if (!image.rootfs_words.empty()) {
        if (!machine.loadBlockImage(image.rootfs_words)) {
            detail::setError(error, "failed to load root filesystem seed into VM block device");
            return false;
        }
    }

    return true;
}

inline TosFramebufferSnapshot decodeFramebuffer(const vm::VMState& machine) {
    TosFramebufferSnapshot snapshot;
    snapshot.sprite_x = machine.sprite_x;
    snapshot.sprite_y = machine.sprite_y;
    snapshot.sprite_attr = machine.sprite_attr;

    if (machine.gpu_mode == 0) {
        snapshot.mode = TosFramebufferMode::Text80x25;
        snapshot.width = 80;
        snapshot.height = 25;
        snapshot.glyphs.assign(80 * 25, ' ');
        snapshot.rgba.assign(80 * 25, detail::paletteColor(0));
        for (int i = 0; i < 80 * 25; ++i) {
            const long long value = detail::dmemWord(machine, 60000 + i);
            const char ch = static_cast<char>(value & 0xff);
            const int color = static_cast<int>((value >> 8) & 0x0f);
            snapshot.glyphs[static_cast<std::size_t>(i)] =
                (ch >= 32 && ch <= 126) ? ch : ' ';
            snapshot.rgba[static_cast<std::size_t>(i)] = detail::paletteColor(color);
        }
        return snapshot;
    }

    snapshot.mode = TosFramebufferMode::Graphics80x60;
    snapshot.width = 80;
    snapshot.height = 60;
    snapshot.rgba.assign(80 * 60, detail::paletteColor(0));
    const int base = (machine.gpu_page == 0) ? 50000 : 55000;
    for (int i = 0; i < 80 * 60; ++i) {
        const long long value = detail::dmemWord(machine, base + i);
        snapshot.rgba[static_cast<std::size_t>(i)] =
            value == 0 ? detail::paletteColor(0)
                       : detail::paletteColor(static_cast<int>(value & 0x0f));
    }
    return snapshot;
}

class TosRuntime {
public:
    TosRuntime() = default;
    explicit TosRuntime(TosRuntimeConfig config)
        : config_(std::move(config)),
          paused_(config_.start_paused) {}

    ~TosRuntime() {
        shutdown();
    }

    TosRuntime(const TosRuntime&) = delete;
    TosRuntime& operator=(const TosRuntime&) = delete;

    [[nodiscard]] bool loadImage(std::string* error = nullptr) {
        shutdownWorkerOnly();
        compactLoadedDiskIfNeeded();
        TosBootImage image;
        if (!readBootImageFile(config_.boot_image_path, image, error)) return false;
        if (!config_.profile_name.empty()) image.manifest.profile_name = config_.profile_name;
        vm::ProductionProfile profile = profileForManifest(image.manifest);
        auto machine = std::make_unique<vm::VMState>(profile);
        if (!loadBootImageIntoVm(*machine, image, config_.disk_path, error)) return false;

        std::lock_guard<std::mutex> lock(mutex_);
        image_ = std::move(image);
        machine_ = std::move(machine);
        paused_ = config_.start_paused;
        return true;
    }

    [[nodiscard]] bool loadImage(const TosBootImage& image, std::string* error = nullptr) {
        shutdownWorkerOnly();
        compactLoadedDiskIfNeeded();
        if (!validateBootImage(image, error)) return false;
        vm::ProductionProfile profile = profileForManifest(image.manifest);
        auto machine = std::make_unique<vm::VMState>(profile);
        if (!loadBootImageIntoVm(*machine, image, config_.disk_path, error)) return false;

        std::lock_guard<std::mutex> lock(mutex_);
        image_ = image;
        machine_ = std::move(machine);
        paused_ = config_.start_paused;
        return true;
    }

    [[nodiscard]] bool start() {
        if (worker_running_) return true;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (!machine_) return false;
        }
        worker_running_ = true;
        worker_ = std::thread([this]() { runLoop(); });
        return true;
    }

    void pause() {
        paused_ = true;
    }

    void resume() {
        paused_ = false;
    }

    [[nodiscard]] bool paused() const {
        return paused_;
    }

    void shutdown() {
        shutdownWorkerOnly();
        std::lock_guard<std::mutex> lock(mutex_);
        if (machine_) {
            (void)machine_->compactBlockBackingFile(false);
            machine_->status = vm::VMStatus::HALTED;
        }
    }

    [[nodiscard]] bool reset(std::string* error = nullptr) {
        shutdownWorkerOnly();
        std::lock_guard<std::mutex> lock(mutex_);
        if (!machine_) {
            detail::setError(error, "runtime has no loaded VM image");
            return false;
        }
        (void)machine_->compactBlockBackingFile(false);
        vm::ProductionProfile profile = profileForManifest(image_.manifest);
        auto machine = std::make_unique<vm::VMState>(profile);
        if (!loadBootImageIntoVm(*machine, image_, config_.disk_path, error)) return false;
        machine_ = std::move(machine);
        paused_ = config_.start_paused;
        return true;
    }

    [[nodiscard]] vm::RunResult runForSteps(int steps) {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!machine_) {
            return {vm::VMStatus::HALTED, 0, 0, isa::TrapCode::TRAP_ILLEGAL_OP, "VM not loaded"};
        }
        return vm::run(*machine_, steps);
    }

    [[nodiscard]] vm::VMStatus stepOnce() {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!machine_) return vm::VMStatus::HALTED;
        return vm::step(*machine_);
    }

    void pushKeyboardInput(long long word) {
        std::lock_guard<std::mutex> lock(mutex_);
        if (machine_) machine_->enqueueConsoleInput(word);
    }

    void pushTextInput(const std::string& text) {
        std::lock_guard<std::mutex> lock(mutex_);
        if (machine_) machine_->enqueueConsoleAscii(text);
    }

    void updateMouseState(long long x, long long y, long long buttons) {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!machine_) return;
        machine_->mouse_x = x;
        machine_->mouse_y = y;
        machine_->mouse_btn = buttons;
    }

    [[nodiscard]] TosFramebufferSnapshot readFramebuffer() const {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!machine_) return TosFramebufferSnapshot{};
        return decodeFramebuffer(*machine_);
    }

    [[nodiscard]] TosRuntimeSnapshot snapshot() const {
        std::lock_guard<std::mutex> lock(mutex_);
        TosRuntimeSnapshot out;
        if (!machine_) return out;
        out.pc = machine_->pc;
        out.status = machine_->status;
        out.cycles = machine_->cycle_count;
        out.privilege = machine_->privilege;
        out.gpu_mode = machine_->gpu_mode;
        out.disk_path = config_.disk_path;
        out.image_version = image_.manifest.image_version;
        out.allocated_disk_blocks = machine_->allocatedDiskBlocks();
        return out;
    }

    [[nodiscard]] bool exportDiagnostics(const std::string& directory,
                                         std::string* error = nullptr) const {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!machine_) {
            detail::setError(error, "runtime has no loaded VM image");
            return false;
        }
        std::error_code ec;
        std::filesystem::create_directories(directory, ec);
        if (ec) {
            detail::setError(error, "failed to create diagnostics directory: " + ec.message());
            return false;
        }

        const std::filesystem::path base(directory);
        {
            std::ofstream out(base / "vm_state.txt", std::ios::trunc);
            if (!out.good()) {
                detail::setError(error, "failed to write vm_state.txt");
                return false;
            }
            out << "pc=" << machine_->pc << "\n";
            out << "status=" << vm::vmStatusToString(machine_->status) << "\n";
            out << "cycles=" << machine_->cycle_count << "\n";
            out << "privilege=" << privilegeName(machine_->privilege) << "\n";
            out << "gpu_mode=" << machine_->gpu_mode << "\n";
            out << "trap=" << vm::ops::toLong(machine_->trap_reg) << "\n";
            out << "cause=" << machine_->cause << "\n";
            out << "disk_path=" << config_.disk_path << "\n";
        }
        {
            std::ofstream out(base / "guest.log", std::ios::trunc);
            if (!out.good()) {
                detail::setError(error, "failed to write guest.log");
                return false;
            }
            out << machine_->syscall_buffer;
        }
        {
            std::ofstream out(base / "manifest.txt", std::ios::trunc);
            if (!out.good()) {
                detail::setError(error, "failed to write manifest.txt");
                return false;
            }
            out << "image_version=" << image_.manifest.image_version << "\n";
            out << "profile=" << image_.manifest.profile_name << "\n";
            out << "boot_entry=" << image_.manifest.boot_entry << "\n";
            out << "apps=" << image_.manifest.apps.size() << "\n";
            for (const TosAppManifestEntry& app : image_.manifest.apps) {
                out << app.name << " " << app.path << " ppn=" << app.text_ppn << "\n";
            }
        }
        {
            TosFramebufferSnapshot framebuffer = decodeFramebuffer(*machine_);
            std::ofstream out(base / "framebuffer_snapshot.txt", std::ios::trunc);
            if (!out.good()) {
                detail::setError(error, "failed to write framebuffer_snapshot.txt");
                return false;
            }
            out << "mode=" << (framebuffer.mode == TosFramebufferMode::Text80x25 ? "text" : "graphics") << "\n";
            out << "width=" << framebuffer.width << "\n";
            out << "height=" << framebuffer.height << "\n";
            for (std::size_t i = 0; i < framebuffer.rgba.size(); ++i) {
                out << std::hex << std::setw(8) << std::setfill('0') << framebuffer.rgba[i] << "\n";
            }
        }
        return true;
    }

private:
    TosRuntimeConfig config_;
    TosBootImage image_;
    std::unique_ptr<vm::VMState> machine_;
    mutable std::mutex mutex_;
    std::thread worker_;
    std::atomic<bool> worker_running_{false};
    std::atomic<bool> paused_{false};

    static const char* privilegeName(isa::PrivilegeMode mode) {
        switch (mode) {
            case isa::PrivilegeMode::Kernel: return "kernel";
            case isa::PrivilegeMode::Supervisor: return "supervisor";
            case isa::PrivilegeMode::User: return "user";
            default: return "unknown";
        }
    }

    void shutdownWorkerOnly() {
        worker_running_ = false;
        if (worker_.joinable()) worker_.join();
    }

    void compactLoadedDiskIfNeeded() {
        std::lock_guard<std::mutex> lock(mutex_);
        if (machine_) (void)machine_->compactBlockBackingFile(false);
    }

    void runLoop() {
        using namespace std::chrono;
        auto last_time = steady_clock::now();
        while (worker_running_) {
            if (paused_) {
                std::this_thread::sleep_for(milliseconds(5));
                last_time = steady_clock::now();
                continue;
            }

            auto now = steady_clock::now();
            const auto elapsed = duration_cast<microseconds>(now - last_time).count();
            if (elapsed <= 0) {
                std::this_thread::yield();
                continue;
            }
            last_time = now;

            long long target_steps = elapsed * 1000;
            if (target_steps > 1000000) target_steps = 1000000;

            long long run_steps = 0;
            bool stopped_or_idle = false;
            while (run_steps < target_steps && worker_running_) {
                long long chunk_steps = 0;
                {
                    std::lock_guard<std::mutex> lock(mutex_);
                    if (!machine_ || !machine_->isRunning()) {
                        run_steps = target_steps;
                        stopped_or_idle = true;
                    } else if (detail::canIdleWithoutStepping(*machine_)) {
                        stopped_or_idle = true;
                    } else {
                        const long long chunk =
                            std::min<long long>(8192, target_steps - run_steps);
                        for (long long i = 0; i < chunk && worker_running_; ++i) {
                            const vm::VMStatus status = vm::step(*machine_);
                            ++chunk_steps;
                            if (status == vm::VMStatus::HALTED ||
                                status == vm::VMStatus::TRAPPED) {
                                stopped_or_idle = true;
                                break;
                            }
                        }
                    }
                }
                run_steps += chunk_steps;
                if (stopped_or_idle) break;
                std::this_thread::yield();
            }

            if (run_steps == 0) {
                std::this_thread::sleep_for(milliseconds(5));
            } else if (run_steps < target_steps) {
                std::this_thread::sleep_for(milliseconds(1));
            } else {
                std::this_thread::yield();
            }
        }
    }
};

} // namespace host
} // namespace sandbox

#endif // TERNARY_HOST_RUNTIME_H
