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
#include <utility>
#include <vector>

namespace sandbox {
namespace host {

inline constexpr std::uint64_t TOS_BOOT_MAGIC = 0x31544f4f424f5354ULL; // "TSOBOOT1"
inline constexpr std::uint32_t TOS_BOOT_LEGACY_FORMAT_VERSION = 1;
inline constexpr std::uint32_t TOS_BOOT_FORMAT_VERSION = 2;
inline constexpr std::uint64_t TOS_SPARSE_DISK_MAGIC = 0x54524954535031ULL; // "TRITSP1"
inline constexpr int TOS_IMAGE_SECTION_EXECUTABLE = 1 << 0;
inline constexpr int TOS_IMAGE_SECTION_KERNEL = 1 << 1;
inline constexpr int TOS_IMAGE_SECTION_APP = 1 << 2;

struct TosAppManifestEntry {
    std::string name;
    std::string path;
    int text_ppn = 0;
    int entry_pc = 0;
    int text_pages = 0;
    int data_pages = 0;
    int stack_words = 0;
};

struct TosImageSection {
    std::string name;
    std::string path;
    std::string kind;
    int load_address = 0;
    int entry_pc = 0;
    int word_count = 0;
    int page_count = 0;
    int flags = 0;
};

struct TosImageManifest {
    std::uint32_t format_version = TOS_BOOT_FORMAT_VERSION;
    std::string image_version = "dev";
    std::string profile_name = "minimum";
    int boot_entry = 0;
    int framebuffer_width = 80;
    int framebuffer_height = 60;
    std::vector<TosImageSection> sections;
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

struct TosFramebufferMemorySnapshot {
    TosFramebufferMode mode = TosFramebufferMode::Text80x25;
    int width = 80;
    int height = 25;
    std::vector<long long> words;
    long long sprite_x = 0;
    long long sprite_y = 0;
    long long sprite_attr = 0;
    std::uint64_t revision = 0;
    bool changed = true;
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
    std::size_t pending_disk_writes = 0;
    int sparse_disk_records = 0;
    vm::VMBlockDeviceStats block_device_stats;
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

inline bool appendImageSection(std::vector<std::uint8_t>& out,
                               const TosImageSection& section) {
    if (!appendString(out, section.name) ||
        !appendString(out, section.path) ||
        !appendString(out, section.kind)) {
        return false;
    }
    appendPod<std::int32_t>(out, section.load_address);
    appendPod<std::int32_t>(out, section.entry_pc);
    appendPod<std::int32_t>(out, section.word_count);
    appendPod<std::int32_t>(out, section.page_count);
    appendPod<std::int32_t>(out, section.flags);
    return true;
}

inline bool readImageSection(const std::vector<std::uint8_t>& in,
                             std::size_t& offset,
                             TosImageSection& section) {
    return readString(in, offset, section.name) &&
           readString(in, offset, section.path) &&
           readString(in, offset, section.kind) &&
           readPod(in, offset, section.load_address) &&
           readPod(in, offset, section.entry_pc) &&
           readPod(in, offset, section.word_count) &&
           readPod(in, offset, section.page_count) &&
           readPod(in, offset, section.flags);
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
        !checkedSize(image.manifest.sections.size()) ||
        !checkedSize(image.program.size()) ||
        !checkedSize(image.data_words.size())) {
        return {};
    }

    appendPod<std::uint32_t>(out,
                             static_cast<std::uint32_t>(image.manifest.sections.size()));
    for (const TosImageSection& section : image.manifest.sections) {
        if (!appendImageSection(out, section)) return {};
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
    if (version != TOS_BOOT_LEGACY_FORMAT_VERSION &&
        version != TOS_BOOT_FORMAT_VERSION) {
        setError(error, "unsupported boot image format version " + std::to_string(version));
        return false;
    }

    TosBootImage decoded;
    decoded.manifest.format_version = version;
    if (!readPod(payload, offset, decoded.manifest.boot_entry) ||
        !readPod(payload, offset, decoded.manifest.framebuffer_width) ||
        !readPod(payload, offset, decoded.manifest.framebuffer_height) ||
        !readString(payload, offset, decoded.manifest.profile_name) ||
        !readString(payload, offset, decoded.manifest.image_version)) {
        setError(error, "boot image manifest is truncated");
        return false;
    }

    if (version == TOS_BOOT_FORMAT_VERSION) {
        std::uint32_t section_count = 0;
        if (!readPod(payload, offset, section_count)) {
            setError(error, "boot image section table is missing");
            return false;
        }
        decoded.manifest.sections.reserve(section_count);
        for (std::uint32_t i = 0; i < section_count; ++i) {
            TosImageSection section;
            if (!readImageSection(payload, offset, section)) {
                setError(error, "boot image section table is truncated");
                return false;
            }
            decoded.manifest.sections.push_back(std::move(section));
        }
    }

    std::uint32_t app_count = 0;
    if (!readPod(payload, offset, app_count)) {
        setError(error, "boot image app registry is missing");
        return false;
    }
    decoded.manifest.apps.reserve(app_count);
    for (std::uint32_t i = 0; i < app_count; ++i) {
        TosAppManifestEntry entry;
        if (!readManifestEntry(payload, offset, entry)) {
            setError(error, "boot image app registry is truncated");
            return false;
        }
        decoded.manifest.apps.push_back(std::move(entry));
    }

    std::uint32_t program_words = 0;
    if (!readPod(payload, offset, program_words)) {
        setError(error, "boot image text segment is missing");
        return false;
    }
    decoded.program.assign(program_words, isa::TritWord27{});
    for (std::uint32_t i = 0; i < program_words; ++i) {
        std::uint64_t bits = 0;
        if (!readPod(payload, offset, bits)) {
            setError(error, "boot image text segment is truncated");
            return false;
        }
        decoded.program[static_cast<std::size_t>(i)].bits = bits;
    }

    std::uint32_t data_words = 0;
    if (!readPod(payload, offset, data_words)) {
        setError(error, "boot image data segment is missing");
        return false;
    }
    decoded.data_words.assign(data_words, 0);
    for (std::uint32_t i = 0; i < data_words; ++i) {
        std::int64_t word = 0;
        if (!readPod(payload, offset, word)) {
            setError(error, "boot image data segment is truncated");
            return false;
        }
        decoded.data_words[static_cast<std::size_t>(i)] = static_cast<long long>(word);
    }

    if (version == TOS_BOOT_LEGACY_FORMAT_VERSION) {
        std::uint32_t rootfs_words = 0;
        if (!readPod(payload, offset, rootfs_words)) {
            setError(error, "boot image root filesystem seed is missing");
            return false;
        }
        decoded.rootfs_words.assign(rootfs_words, 0);
        for (std::uint32_t i = 0; i < rootfs_words; ++i) {
            std::int64_t word = 0;
            if (!readPod(payload, offset, word)) {
                setError(error, "boot image root filesystem seed is truncated");
                return false;
            }
            decoded.rootfs_words[static_cast<std::size_t>(i)] = static_cast<long long>(word);
        }
    }

    if (offset != payload.size()) {
        setError(error, "boot image has trailing payload bytes");
        return false;
    }
    image = std::move(decoded);
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

inline std::string jsonEscape(const std::string& value) {
    std::ostringstream out;
    for (unsigned char ch : value) {
        switch (ch) {
            case '"': out << "\\\""; break;
            case '\\': out << "\\\\"; break;
            case '\b': out << "\\b"; break;
            case '\f': out << "\\f"; break;
            case '\n': out << "\\n"; break;
            case '\r': out << "\\r"; break;
            case '\t': out << "\\t"; break;
            default:
                if (ch < 0x20) {
                    out << "\\u"
                        << std::hex << std::setw(4) << std::setfill('0')
                        << static_cast<int>(ch)
                        << std::dec << std::setfill(' ');
                } else {
                    out << static_cast<char>(ch);
                }
                break;
        }
    }
    return out.str();
}

inline void writeJsonString(std::ostream& out, const std::string& value) {
    out << '"' << jsonEscape(value) << '"';
}

inline const char* processStateName(long long state) {
    switch (state) {
        case 0: return "free";
        case 1: return "runnable";
        case 2: return "running";
        case 3: return "blocked";
        case 4: return "sleeping";
        case 5: return "exited";
        case 6: return "stopped";
        case 7: return "zombie";
        case 8: return "killing";
        case 9: return "crashed";
        default: return "unknown";
    }
}

inline long long dmemWord(const vm::VMState& machine, int addr) {
    auto [word, fault] = machine.dmem.load(addr);
    if (fault != vm::MemFaultCode::OK) return 0;
    return vm::ops::toLong(word);
}

constexpr int kKernelBootedAddr = 3000;
constexpr int kCurrentPidAddr = 3020;
constexpr int kSysStatusAddr = 3021;
constexpr int kSysPayloadAddr = 3022;
constexpr int kSysDetailAddr = 3023;
constexpr int kInputLastMouseBtnAddr = 3031;
constexpr int kInputLastMouseXAddr = 3034;
constexpr int kInputLastMouseYAddr = 3035;
constexpr int kProcessMax = 100;
constexpr int kProcessRowWords = 8;
constexpr int kProcPid = 0;
constexpr int kProcNamespace = 1;
constexpr int kProcState = 2;
constexpr int kProcPriority = 3;
constexpr int kProcQuota = 4;
constexpr int kProcContext = 5;
constexpr int kProcWaitChannel = 6;
constexpr int kProcVersion = 7;
constexpr int kProcRunnable = 1;
constexpr int kProcRunning = 2;
constexpr int kProcBlocked = 3;
constexpr int kProcSleeping = 4;
constexpr int kProcessBase = 390000;
constexpr int kTaskContextEpc = 0;
constexpr int kTaskContextStatus = 1;
constexpr int kTaskContextImemPtbr = 2;
constexpr int kTaskContextImemPages = 3;
constexpr int kTaskContextDmemPtbr = 4;
constexpr int kTaskContextDmemPages = 5;
constexpr int kTaskContextSp = 31;
constexpr int kProcParentPidBase = 130000;
constexpr int kProcExitStatusBase = 130100;
constexpr int kProcSignalPendingBase = 130200;
constexpr int kProcCapsBase = 131350;
constexpr int kWaitKindBase = 138600;
constexpr int kWaitDeadlineBase = 138700;

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
    if (image.manifest.format_version != TOS_BOOT_LEGACY_FORMAT_VERSION &&
        image.manifest.format_version != TOS_BOOT_FORMAT_VERSION) {
        detail::setError(error, "unsupported boot image format version " +
                                    std::to_string(image.manifest.format_version));
        return false;
    }
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
    if (image.manifest.format_version == TOS_BOOT_FORMAT_VERSION &&
        image.manifest.sections.empty()) {
        detail::setError(error, "boot image has no section table entries");
        return false;
    }
    for (const TosImageSection& section : image.manifest.sections) {
        if (section.name.empty() || section.kind.empty()) {
            detail::setError(error, "boot image section has empty name or kind");
            return false;
        }
        if (section.word_count < 0 || section.page_count < 0 || section.load_address < 0) {
            detail::setError(error, "boot image section has invalid geometry");
            return false;
        }
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
    image.manifest.format_version = TOS_BOOT_FORMAT_VERSION;
    image.program = assembled.program;
    image.rootfs_words = rootfs_words;
    image.data_words.reserve(assembled.data.size());
    for (const vm::TernaryValue& value : assembled.data) {
        image.data_words.push_back(vm::ops::toLong(value));
    }
    if (image.manifest.sections.empty()) {
        image.manifest.sections.push_back({
            "kernel",
            "/kernel",
            "kernel",
            0,
            image.manifest.boot_entry,
            static_cast<int>(image.program.size()),
            static_cast<int>((image.program.size() + vm::MMU_PAGE_WORDS - 1) /
                             vm::MMU_PAGE_WORDS),
            TOS_IMAGE_SECTION_EXECUTABLE | TOS_IMAGE_SECTION_KERNEL,
        });
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
        if (!std::filesystem::exists(disk_path)) {
            if (!image.rootfs_words.empty()) {
                if (!writeSparseDiskFile(disk_path, image.rootfs_words, false, error)) {
                    return false;
                }
            } else {
                detail::setError(error, "disk image is required for this boot image: " +
                                            disk_path);
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

struct TosFramebufferReadPlan {
    TosFramebufferMode mode = TosFramebufferMode::Text80x25;
    int width = 80;
    int height = 25;
    int base = 60000;
    int word_count = 80 * 25;
};

inline void mixFramebufferRevision(std::uint64_t& revision, std::uint64_t value) {
    revision ^= value + 0x9e3779b97f4a7c15ULL + (revision << 6) + (revision >> 2);
}

inline TosFramebufferReadPlan framebufferReadPlan(const vm::VMState& machine) {
    TosFramebufferReadPlan plan;
    if (machine.gpu_mode == 0) {
        return plan;
    }
    plan.mode = TosFramebufferMode::Graphics80x60;
    plan.width = 80;
    plan.height = 60;
    plan.base = (machine.gpu_page == 0) ? 50000 : 55000;
    plan.word_count = 80 * 60;
    return plan;
}

inline std::uint64_t framebufferRevision(const vm::VMState& machine,
                                         const TosFramebufferReadPlan& plan) {
    std::uint64_t revision = 0xcbf29ce484222325ULL;
    mixFramebufferRevision(revision, static_cast<std::uint64_t>(plan.mode));
    mixFramebufferRevision(revision, static_cast<std::uint64_t>(plan.width));
    mixFramebufferRevision(revision, static_cast<std::uint64_t>(plan.height));
    mixFramebufferRevision(revision, static_cast<std::uint64_t>(plan.base));
    mixFramebufferRevision(revision, static_cast<std::uint64_t>(plan.word_count));
    mixFramebufferRevision(revision, static_cast<std::uint64_t>(machine.sprite_x));
    mixFramebufferRevision(revision, static_cast<std::uint64_t>(machine.sprite_y));
    mixFramebufferRevision(revision, static_cast<std::uint64_t>(machine.sprite_attr));
    mixFramebufferRevision(revision, machine.dmem.rangeGeneration(plan.base, plan.word_count));
    return revision == 0 ? 1 : revision;
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

    [[nodiscard]] TosFramebufferMemorySnapshot readFramebufferMemory(
        std::uint64_t previous_revision = 0) const {
        std::lock_guard<std::mutex> lock(mutex_);
        TosFramebufferMemorySnapshot snapshot;
        if (!machine_) return snapshot;

        const TosFramebufferReadPlan plan = framebufferReadPlan(*machine_);
        snapshot.mode = plan.mode;
        snapshot.width = plan.width;
        snapshot.height = plan.height;
        snapshot.sprite_x = machine_->sprite_x;
        snapshot.sprite_y = machine_->sprite_y;
        snapshot.sprite_attr = machine_->sprite_attr;
        snapshot.revision = framebufferRevision(*machine_, plan);
        snapshot.changed =
            previous_revision == 0 || snapshot.revision != previous_revision;
        if (!snapshot.changed) return snapshot;

        snapshot.words.assign(static_cast<std::size_t>(plan.word_count), 0);
        for (int i = 0; i < plan.word_count; ++i) {
            snapshot.words[static_cast<std::size_t>(i)] =
                detail::dmemWord(*machine_, plan.base + i);
        }
        return snapshot;
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
        out.pending_disk_writes = machine_->pendingDiskWrites();
        out.sparse_disk_records = machine_->sparseDiskRecordCount();
        out.block_device_stats = machine_->blockDeviceStats();
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
            const vm::VMBlockDeviceStats block_stats = machine_->blockDeviceStats();
            out << "disk_allocated_blocks=" << machine_->allocatedDiskBlocks() << "\n";
            out << "disk_pending_writes=" << machine_->pendingDiskWrites() << "\n";
            out << "disk_sparse_records=" << machine_->sparseDiskRecordCount() << "\n";
            out << "disk_cache_reads=" << block_stats.reads << "\n";
            out << "disk_cache_writes=" << block_stats.writes << "\n";
            out << "disk_cache_hits=" << block_stats.hits << "\n";
            out << "disk_cache_misses=" << block_stats.misses << "\n";
            out << "disk_dirty_flushes=" << block_stats.dirty_flushes << "\n";
        }
        {
            std::ofstream out(base / "manifest.json", std::ios::trunc);
            if (!out.good()) {
                detail::setError(error, "failed to write manifest.json");
                return false;
            }
            out << "{\n";
            out << "  \"format_version\": 1,\n";
            out << "  \"image\": {\n";
            out << "    \"format_version\": " << image_.manifest.format_version << ",\n";
            out << "    \"version\": ";
            detail::writeJsonString(out, image_.manifest.image_version);
            out << ",\n";
            out << "    \"profile\": ";
            detail::writeJsonString(out, image_.manifest.profile_name);
            out << ",\n";
            out << "    \"boot_entry\": " << image_.manifest.boot_entry << ",\n";
            out << "    \"framebuffer_width\": " << image_.manifest.framebuffer_width << ",\n";
            out << "    \"framebuffer_height\": " << image_.manifest.framebuffer_height << ",\n";
            out << "    \"program_words\": " << image_.program.size() << ",\n";
            out << "    \"data_words\": " << image_.data_words.size() << ",\n";
            out << "    \"rootfs_words\": " << image_.rootfs_words.size() << "\n";
            out << "  },\n";
            out << "  \"sections\": [\n";
            for (std::size_t i = 0; i < image_.manifest.sections.size(); ++i) {
                const TosImageSection& section = image_.manifest.sections[i];
                out << "    {\"name\": ";
                detail::writeJsonString(out, section.name);
                out << ", \"path\": ";
                detail::writeJsonString(out, section.path);
                out << ", \"kind\": ";
                detail::writeJsonString(out, section.kind);
                out << ", \"load_address\": " << section.load_address
                    << ", \"entry_pc\": " << section.entry_pc
                    << ", \"word_count\": " << section.word_count
                    << ", \"page_count\": " << section.page_count
                    << ", \"flags\": " << section.flags << "}";
                if (i + 1 < image_.manifest.sections.size()) out << ",";
                out << "\n";
            }
            out << "  ],\n";
            out << "  \"runtime\": {\n";
            out << "    \"pc\": " << machine_->pc << ",\n";
            out << "    \"status\": ";
            detail::writeJsonString(out, vm::vmStatusToString(machine_->status));
            out << ",\n";
            out << "    \"cycles\": " << machine_->cycle_count << ",\n";
            out << "    \"privilege\": ";
            detail::writeJsonString(out, privilegeName(machine_->privilege));
            out << ",\n";
            out << "    \"gpu_mode\": " << machine_->gpu_mode << ",\n";
            out << "    \"current_pid\": " << detail::dmemWord(*machine_, detail::kCurrentPidAddr) << ",\n";
            out << "    \"syscall_status\": " << detail::dmemWord(*machine_, detail::kSysStatusAddr) << ",\n";
            out << "    \"syscall_payload\": " << detail::dmemWord(*machine_, detail::kSysPayloadAddr) << ",\n";
            out << "    \"syscall_detail\": " << detail::dmemWord(*machine_, detail::kSysDetailAddr) << ",\n";
            out << "    \"trap\": " << vm::ops::toLong(machine_->trap_reg) << ",\n";
            out << "    \"cause\": " << machine_->cause << "\n";
            out << "  },\n";
            out << "  \"disk\": {\n";
            out << "    \"path\": ";
            detail::writeJsonString(out, config_.disk_path);
            out << ",\n";
            const vm::VMBlockDeviceStats block_stats = machine_->blockDeviceStats();
            out << "    \"allocated_blocks\": " << machine_->allocatedDiskBlocks() << ",\n";
            out << "    \"pending_writes\": " << machine_->pendingDiskWrites() << ",\n";
            out << "    \"sparse_records\": " << machine_->sparseDiskRecordCount() << ",\n";
            out << "    \"block_cache\": {\n";
            out << "      \"reads\": " << block_stats.reads << ",\n";
            out << "      \"writes\": " << block_stats.writes << ",\n";
            out << "      \"hits\": " << block_stats.hits << ",\n";
            out << "      \"misses\": " << block_stats.misses << ",\n";
            out << "      \"dirty_flushes\": " << block_stats.dirty_flushes << ",\n";
            out << "      \"flushes\": " << block_stats.flushes << ",\n";
            out << "      \"compactions\": " << block_stats.compactions << ",\n";
            out << "      \"read_ahead\": " << block_stats.read_ahead << "\n";
            out << "    }\n";
            out << "  },\n";
            out << "  \"apps\": [\n";
            for (std::size_t i = 0; i < image_.manifest.apps.size(); ++i) {
                const TosAppManifestEntry& app = image_.manifest.apps[i];
                out << "    {\"name\": ";
                detail::writeJsonString(out, app.name);
                out << ", \"path\": ";
                detail::writeJsonString(out, app.path);
                out << ", \"text_ppn\": " << app.text_ppn
                    << ", \"entry_pc\": " << app.entry_pc
                    << ", \"text_pages\": " << app.text_pages
                    << ", \"data_pages\": " << app.data_pages
                    << ", \"stack_words\": " << app.stack_words << "}";
                if (i + 1 < image_.manifest.apps.size()) out << ",";
                out << "\n";
            }
            out << "  ]\n";
            out << "}\n";
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
            std::ofstream out(base / "kernel_log.txt", std::ios::trunc);
            if (!out.good()) {
                detail::setError(error, "failed to write kernel_log.txt");
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
            out << "format_version=" << image_.manifest.format_version << "\n";
            out << "image_version=" << image_.manifest.image_version << "\n";
            out << "profile=" << image_.manifest.profile_name << "\n";
            out << "boot_entry=" << image_.manifest.boot_entry << "\n";
            out << "sections=" << image_.manifest.sections.size() << "\n";
            for (const TosImageSection& section : image_.manifest.sections) {
                out << section.kind << " " << section.name << " " << section.path
                    << " words=" << section.word_count
                    << " load=" << section.load_address << "\n";
            }
            out << "apps=" << image_.manifest.apps.size() << "\n";
            for (const TosAppManifestEntry& app : image_.manifest.apps) {
                out << app.name << " " << app.path << " ppn=" << app.text_ppn << "\n";
            }
        }
        {
            std::ofstream out(base / "process_table.json", std::ios::trunc);
            if (!out.good()) {
                detail::setError(error, "failed to write process_table.json");
                return false;
            }
            out << "{\n";
            out << "  \"format_version\": 1,\n";
            out << "  \"current_pid\": " << detail::dmemWord(*machine_, detail::kCurrentPidAddr) << ",\n";
            out << "  \"slots\": [\n";
            for (int slot = 0; slot < detail::kProcessMax; ++slot) {
                const int row = detail::kProcessBase + slot * detail::kProcessRowWords;
                const long long state = detail::dmemWord(*machine_, row + detail::kProcState);
                const long long context = detail::dmemWord(*machine_, row + detail::kProcContext);
                out << "    {\"slot\": " << slot
                    << ", \"pid\": " << detail::dmemWord(*machine_, row + detail::kProcPid)
                    << ", \"namespace\": " << detail::dmemWord(*machine_, row + detail::kProcNamespace)
                    << ", \"state\": " << state
                    << ", \"state_name\": ";
                detail::writeJsonString(out, detail::processStateName(state));
                out << ", \"priority\": " << detail::dmemWord(*machine_, row + detail::kProcPriority)
                    << ", \"quota\": " << detail::dmemWord(*machine_, row + detail::kProcQuota)
                    << ", \"context\": " << context
                    << ", \"context_epc\": " << detail::dmemWord(*machine_, static_cast<int>(context) + detail::kTaskContextEpc)
                    << ", \"context_status\": " << detail::dmemWord(*machine_, static_cast<int>(context) + detail::kTaskContextStatus)
                    << ", \"context_imem_ptbr\": " << detail::dmemWord(*machine_, static_cast<int>(context) + detail::kTaskContextImemPtbr)
                    << ", \"context_imem_pages\": " << detail::dmemWord(*machine_, static_cast<int>(context) + detail::kTaskContextImemPages)
                    << ", \"context_dmem_ptbr\": " << detail::dmemWord(*machine_, static_cast<int>(context) + detail::kTaskContextDmemPtbr)
                    << ", \"context_dmem_pages\": " << detail::dmemWord(*machine_, static_cast<int>(context) + detail::kTaskContextDmemPages)
                    << ", \"context_sp\": " << detail::dmemWord(*machine_, static_cast<int>(context) + detail::kTaskContextSp)
                    << ", \"wait_channel\": " << detail::dmemWord(*machine_, row + detail::kProcWaitChannel)
                    << ", \"version\": " << detail::dmemWord(*machine_, row + detail::kProcVersion)
                    << ", \"parent_pid\": " << detail::dmemWord(*machine_, detail::kProcParentPidBase + slot)
                    << ", \"exit_status\": " << detail::dmemWord(*machine_, detail::kProcExitStatusBase + slot)
                    << ", \"pending_signals\": " << detail::dmemWord(*machine_, detail::kProcSignalPendingBase + slot)
                    << ", \"caps\": " << detail::dmemWord(*machine_, detail::kProcCapsBase + slot)
                    << ", \"wait_kind\": " << detail::dmemWord(*machine_, detail::kWaitKindBase + slot)
                    << ", \"wait_deadline\": " << detail::dmemWord(*machine_, detail::kWaitDeadlineBase + slot)
                    << "}";
                if (slot + 1 < detail::kProcessMax) out << ",";
                out << "\n";
            }
            out << "  ]\n";
            out << "}\n";
        }
        {
            std::ofstream out(base / "syscall_trace.jsonl", std::ios::trunc);
            if (!out.good()) {
                detail::setError(error, "failed to write syscall_trace.jsonl");
                return false;
            }
            out << "{\"event\":\"trace_unavailable\","
                << "\"reason\":\"runtime does not yet record per-syscall trace events\","
                << "\"cycles\":" << machine_->cycle_count << "}\n";
        }
        {
            std::ofstream out(base / "crash_report.txt", std::ios::trunc);
            if (!out.good()) {
                detail::setError(error, "failed to write crash_report.txt");
                return false;
            }
            out << "status=" << vm::vmStatusToString(machine_->status) << "\n";
            out << "pc=" << machine_->pc << "\n";
            out << "cycles=" << machine_->cycle_count << "\n";
            out << "privilege=" << privilegeName(machine_->privilege) << "\n";
            out << "trap=" << vm::ops::toLong(machine_->trap_reg) << "\n";
            out << "cause=" << machine_->cause << "\n";
            out << "current_pid=" << detail::dmemWord(*machine_, detail::kCurrentPidAddr) << "\n";
            out << "syscall_status=" << detail::dmemWord(*machine_, detail::kSysStatusAddr) << "\n";
            out << "syscall_payload=" << detail::dmemWord(*machine_, detail::kSysPayloadAddr) << "\n";
            out << "syscall_detail=" << detail::dmemWord(*machine_, detail::kSysDetailAddr) << "\n";
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
