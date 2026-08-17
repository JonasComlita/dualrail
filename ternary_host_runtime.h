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
inline constexpr std::uint32_t TOS_BOOT_FORMAT_VERSION =
    architecture::v2::TBOOT_WRITE_VERSION;
inline constexpr std::uint64_t TOS_SPARSE_DISK_MAGIC =
    0x54524954535032ULL; // "TRITSP2"
inline constexpr std::uint64_t TOS_LEGACY_SPARSE_DISK_MAGIC =
    0x54524954535031ULL; // "TRITSP1" (offline migration input only)
inline constexpr std::uint32_t TOS_SPARSE_DISK_VERSION =
    architecture::v2::TDISK_WRITE_VERSION;
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
    int isa_version = architecture::v2::ISA_VERSION;
    std::uint64_t required_features =
        isa::featureBit(architecture::v2::FEATURE_BASE_V2);
    int function_abi_version = architecture::v2::FUNCTION_ABI_VERSION;
    int syscall_abi_version = architecture::v2::SYSCALL_ABI_VERSION;
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
    int isa_version = architecture::v2::ISA_VERSION;
    std::uint64_t required_features =
        isa::featureBit(architecture::v2::FEATURE_BASE_V2);
    int scalar_word_trits = architecture::v2::SCALAR_WORD_TRITS;
    int base_page_words = architecture::v2::BASE_PAGE_WORDS;
    int function_abi_version = architecture::v2::FUNCTION_ABI_VERSION;
    int syscall_abi_version = architecture::v2::SYSCALL_ABI_VERSION;
    std::vector<TosImageSection> sections;
    std::vector<TosAppManifestEntry> apps;
};

struct TosBootImage {
    TosImageManifest manifest;
    std::vector<isa::TritWord27> program;
    std::vector<long long> data_words;
    std::vector<std::uint64_t> data_words_raw;
    std::vector<long long> rootfs_words;
};

struct TosRuntimeConfig {
    std::string boot_image_path;
    std::string disk_path;
    std::string profile_name = "minimum";
    bool start_paused = false;
    bool debug_overlay = false;
    bool record_syscall_trace = false;
#if defined(_M_X64) || defined(__x86_64__)
    // The accepted NativeX64Jit path is the host-runtime default on x86-64.
    // Callers on any host can still select a portable backend explicitly.
    vm::VMExecutionBackend execution_backend =
        vm::VMExecutionBackend::NativeX64Jit;
#else
    vm::VMExecutionBackend execution_backend =
        vm::VMExecutionBackend::CachedBlockInterpreter;
#endif
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
    std::uint64_t boot_generation = 0;
    std::uint64_t guest_reboot_count = 0;
};

enum class TosInputEventKind : std::uint8_t {
    KeyboardWord = 1,
    Text = 2,
    Mouse = 3,
};

struct TosInputJournalEvent {
    std::uint64_t sequence = 0;
    std::uint64_t cycle = 0;
    TosInputEventKind kind = TosInputEventKind::KeyboardWord;
    long long value0 = 0;
    long long value1 = 0;
    long long value2 = 0;
    std::string text;
    // Provenance is host-side metadata.  It is intentionally separate from
    // the guest payload so replay remains deterministic while diagnostics can
    // identify which external ingress path supplied an event.
    std::string source = "host.api";
    std::string channel;
};

struct TosRuntimeCheckpoint {
    static constexpr const char* kSchema = "trit.runtime_checkpoint.v1";

    std::uint64_t sequence = 0;
    std::size_t input_event_count = 0;
    vm::VMCheckpoint vm;
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

// Checkpoint state files are deliberately a small, versioned binary format.
// They contain architectural values and materialized memory only; decoded
// instruction/native-code caches are rebuilt after restore.  Keeping this
// format separate from tBoot/tDisk lets a replay bundle restore a live VM
// without pretending that a host C++ object layout is portable.
struct CheckpointWriter {
    std::ofstream file;
    bool ok = false;

    explicit CheckpointWriter(const std::filesystem::path& path)
        : file(path, std::ios::binary | std::ios::trunc), ok(file.good()) {}

    template <typename T>
    void pod(const T& value) {
        if (!ok) return;
        file.write(reinterpret_cast<const char*>(&value), sizeof(T));
        ok = file.good();
    }

    void bytes(const char* data, std::size_t size) {
        if (!ok) return;
        if (size > static_cast<std::size_t>(std::numeric_limits<std::streamsize>::max())) {
            ok = false;
            return;
        }
        file.write(data, static_cast<std::streamsize>(size));
        ok = file.good();
    }

    void string(const std::string& value) {
        const std::uint64_t size = static_cast<std::uint64_t>(value.size());
        pod(size);
        bytes(value.data(), value.size());
    }

    void finish() {
        if (!ok) return;
        file.flush();
        ok = file.good();
    }
};

struct CheckpointReader {
    std::ifstream file;
    bool ok = false;

    explicit CheckpointReader(const std::filesystem::path& path)
        : file(path, std::ios::binary), ok(file.good()) {}

    template <typename T>
    bool pod(T& value) {
        if (!ok) return false;
        file.read(reinterpret_cast<char*>(&value), sizeof(T));
        ok = file.good();
        return ok;
    }

    bool bytes(char* data, std::size_t size) {
        if (!ok || size > static_cast<std::size_t>(std::numeric_limits<std::streamsize>::max())) {
            ok = false;
            return false;
        }
        file.read(data, static_cast<std::streamsize>(size));
        ok = file.good();
        return ok;
    }

    bool string(std::string& value) {
        std::uint64_t size = 0;
        if (!pod(size) || size > (64ULL << 20)) return false;
        value.resize(static_cast<std::size_t>(size));
        return bytes(value.data(), value.size());
    }
};

inline constexpr std::uint64_t kCheckpointStateMagic =
    0x3156535043545254ULL; // "TRTCPV S1" (little-endian marker)
inline constexpr std::uint32_t kCheckpointStateVersion = 1;

inline void writeCheckpointValue(CheckpointWriter& writer,
                                 const vm::TernaryValue& value) {
    writer.pod(static_cast<std::uint8_t>(value.mode));
    writer.pod(value.bits.lo);
    writer.pod(value.bits.hi);
}

inline bool readCheckpointValue(CheckpointReader& reader,
                                vm::TernaryValue& value) {
    std::uint8_t mode = 0;
    if (!reader.pod(mode) || mode > static_cast<std::uint8_t>(TernaryMode::L50)) {
        return false;
    }
    value.mode = static_cast<TernaryMode>(mode);
    return reader.pod(value.bits.lo) && reader.pod(value.bits.hi);
}

inline void writeCheckpointProfile(CheckpointWriter& writer,
                                   const vm::ProductionProfile& profile) {
    writer.pod(profile.cores);
    writer.pod(profile.ram_words);
    writer.pod(profile.instruction_words);
    writer.pod(profile.disk_blocks);
    writer.pod(profile.framebuffer_words);
    writer.pod(profile.framebuffer_width);
    writer.pod(profile.framebuffer_height);
    writer.pod(profile.max_processes);
    writer.pod(profile.max_files);
    writer.pod(profile.max_windows);
    writer.pod(profile.hardware_page_words);
    writer.pod(profile.cluster_words);
    writer.pod(profile.ram_bytes);
    writer.pod(profile.disk_bytes);
}

inline bool readCheckpointProfile(CheckpointReader& reader,
                                  vm::ProductionProfile& profile) {
    return reader.pod(profile.cores) && reader.pod(profile.ram_words) &&
           reader.pod(profile.instruction_words) && reader.pod(profile.disk_blocks) &&
           reader.pod(profile.framebuffer_words) && reader.pod(profile.framebuffer_width) &&
           reader.pod(profile.framebuffer_height) && reader.pod(profile.max_processes) &&
           reader.pod(profile.max_files) && reader.pod(profile.max_windows) &&
           reader.pod(profile.hardware_page_words) && reader.pod(profile.cluster_words) &&
           reader.pod(profile.ram_bytes) && reader.pod(profile.disk_bytes);
}

inline bool checkpointProfileSane(const vm::ProductionProfile& profile) {
    // Checkpoint bundles may be supplied by diagnostics tooling, so reject
    // dimensions that could allocate an unbounded host object before restore
    // has validated any architectural state.  The largest shipped profile is
    // below these limits (4 GiB of ternary RAM and a 64 GiB sparse disk).
    constexpr int kMaxWords = 1 << 29;
    constexpr int kMaxBlocks = 1 << 29;
    constexpr int kMaxFramebufferDimension = 16384;
    return profile.cores >= 1 && profile.cores <= 256 &&
           profile.ram_words > 0 && profile.ram_words <= kMaxWords &&
           profile.instruction_words > 0 &&
           profile.instruction_words <= kMaxWords &&
           profile.disk_blocks > 0 && profile.disk_blocks <= kMaxBlocks &&
           profile.framebuffer_words > 0 &&
           profile.framebuffer_width > 0 &&
           profile.framebuffer_width <= kMaxFramebufferDimension &&
           profile.framebuffer_height > 0 &&
           profile.framebuffer_height <= kMaxFramebufferDimension &&
           profile.max_processes > 0 && profile.max_processes <= 1'000'000 &&
           profile.max_files > 0 && profile.max_files <= 1'000'000 &&
           profile.max_windows > 0 && profile.max_windows <= 1'000'000 &&
           profile.hardware_page_words > 0 &&
           profile.hardware_page_words <= (1 << 20) &&
           profile.cluster_words > 0 && profile.cluster_words <= (1 << 20) &&
           profile.ram_bytes > 0 && profile.disk_bytes > 0;
}

inline void writeCheckpointTlbEntry(CheckpointWriter& writer,
                                    const vm::TlbEntry& entry) {
    writer.pod(static_cast<std::uint8_t>(entry.valid));
    writer.pod(static_cast<std::uint8_t>(entry.global));
    writer.pod(entry.asid);
    writer.pod(entry.translation_root);
    writer.pod(static_cast<std::uint8_t>(entry.instruction_space));
    writer.pod(entry.vpn);
    writer.pod(entry.page_words);
    writer.pod(entry.ppn);
    writer.pod(static_cast<std::uint8_t>(entry.user));
    writer.pod(static_cast<std::uint8_t>(entry.read));
    writer.pod(static_cast<std::uint8_t>(entry.write));
    writer.pod(static_cast<std::uint8_t>(entry.execute));
    writer.pod(static_cast<std::uint8_t>(entry.accessed));
    writer.pod(static_cast<std::uint8_t>(entry.dirty));
    writer.pod(entry.replacement_stamp);
}

inline bool readCheckpointTlbEntry(CheckpointReader& reader,
                                   vm::TlbEntry& entry) {
    std::uint8_t valid = 0;
    std::uint8_t global = 0;
    std::uint8_t instruction = 0;
    std::uint8_t user = 0;
    std::uint8_t read = 0;
    std::uint8_t write = 0;
    std::uint8_t execute = 0;
    std::uint8_t accessed = 0;
    std::uint8_t dirty = 0;
    if (!reader.pod(valid) || !reader.pod(global) || !reader.pod(entry.asid) ||
        !reader.pod(entry.translation_root) || !reader.pod(instruction) ||
        !reader.pod(entry.vpn) || !reader.pod(entry.page_words) ||
        !reader.pod(entry.ppn) || !reader.pod(user) || !reader.pod(read) ||
        !reader.pod(write) || !reader.pod(execute) || !reader.pod(accessed) ||
        !reader.pod(dirty) || !reader.pod(entry.replacement_stamp)) {
        return false;
    }
    entry.valid = valid != 0;
    entry.global = global != 0;
    entry.instruction_space = instruction != 0;
    entry.user = user != 0;
    entry.read = read != 0;
    entry.write = write != 0;
    entry.execute = execute != 0;
    entry.accessed = accessed != 0;
    entry.dirty = dirty != 0;
    return true;
}

inline void writeCheckpointTlbStats(CheckpointWriter& writer,
                                    const vm::VMTlbStats& stats) {
    writer.pod(stats.instruction_l1_hits);
    writer.pod(stats.data_l1_hits);
    writer.pod(stats.l2_hits);
    writer.pod(stats.misses);
    writer.pod(stats.walks);
    writer.pod(stats.evictions);
    writer.pod(stats.superpage_hits);
    writer.pod(stats.shootdowns);
}

inline bool readCheckpointTlbStats(CheckpointReader& reader,
                                   vm::VMTlbStats& stats) {
    return reader.pod(stats.instruction_l1_hits) &&
           reader.pod(stats.data_l1_hits) && reader.pod(stats.l2_hits) &&
           reader.pod(stats.misses) && reader.pod(stats.walks) &&
           reader.pod(stats.evictions) && reader.pod(stats.superpage_hits) &&
           reader.pod(stats.shootdowns);
}

inline bool writeCheckpointState(const std::filesystem::path& path,
                                 const vm::VMState& state,
                                 std::string* error = nullptr) {
    CheckpointWriter writer(path);
    if (!writer.ok) {
        setError(error, "failed to open checkpoint state: " + path.string());
        return false;
    }
    writer.pod(kCheckpointStateMagic);
    writer.pod(kCheckpointStateVersion);
    writer.pod(static_cast<std::int32_t>(state.imem.size()));
    writer.pod(static_cast<std::int32_t>(state.dmem.size()));
    writer.pod(static_cast<std::uint8_t>(state.imem.isSparse()));
    writer.pod(static_cast<std::uint8_t>(state.dmem.isSparse()));
    writer.pod(static_cast<std::int32_t>(state.block_device.blockCount()));
    writeCheckpointProfile(writer, state.profile);

    std::uint64_t imem_count = 0;
    state.imem.forEachNonZero([&](int, const isa::TritWord27&) { ++imem_count; });
    writer.pod(imem_count);
    state.imem.forEachNonZero([&](int address, const isa::TritWord27& word) {
        writer.pod(static_cast<std::int32_t>(address));
        writer.pod(word.bits);
    });
    std::uint64_t dmem_count = 0;
    state.dmem.forEachNonZero([&](int, const vm::TernaryValue&) { ++dmem_count; });
    writer.pod(dmem_count);
    state.dmem.forEachNonZero([&](int address, const vm::TernaryValue& value) {
        writer.pod(static_cast<std::int32_t>(address));
        writeCheckpointValue(writer, value);
    });

    for (int index = 0; index < isa::REG_COUNT; ++index) {
        writeCheckpointValue(writer, state.regfile.reg[static_cast<std::size_t>(index)]);
        writer.pod(static_cast<std::uint8_t>(state.regfile.view_mode[static_cast<std::size_t>(index)]));
    }
    writer.pod(static_cast<std::int32_t>(state.pc));
    writer.pod(static_cast<std::uint8_t>(state.status));
    writeCheckpointValue(writer, state.trap_reg);
    writer.pod(state.required_features);
    writer.pod(state.supported_features);
    writer.pod(state.asid);
    for (const auto& entry : state.instruction_tlb) writeCheckpointTlbEntry(writer, entry);
    for (const auto& entry : state.data_tlb) writeCheckpointTlbEntry(writer, entry);
    for (const auto& entry : state.unified_l2_tlb) writeCheckpointTlbEntry(writer, entry);
    writer.pod(state.tlb_stats.instruction_l1_hits);
    writer.pod(state.tlb_stats.data_l1_hits);
    writer.pod(state.tlb_stats.l2_hits);
    writer.pod(state.tlb_stats.misses);
    writer.pod(state.tlb_stats.walks);
    writer.pod(state.tlb_stats.evictions);
    writer.pod(state.tlb_stats.superpage_hits);
    writer.pod(state.tlb_stats.shootdowns);
    writer.pod(state.tlb_replacement_clock);
    writer.pod(state.vector_length);
    for (const auto& reg : state.vregfile.reg) {
        const std::uint64_t lane_count = static_cast<std::uint64_t>(reg.lane.size());
        writer.pod(lane_count);
        for (const auto& value : reg.lane) writeCheckpointValue(writer, value);
    }
    const std::uint64_t fault_count = static_cast<std::uint64_t>(state.vector_faults.fault_valid.size());
    writer.pod(fault_count);
    for (std::size_t index = 0; index < state.vector_faults.fault_valid.size(); ++index) {
        writer.pod(state.vector_faults.fault_valid[index]);
        const std::uint8_t trap = index < state.vector_faults.fault_class.size()
            ? static_cast<std::uint8_t>(state.vector_faults.fault_class[index]) : 0;
        writer.pod(trap);
    }
    writer.pod(state.vector_faults.first_failing_lane);
    writeCheckpointValue(writer, state.accumulator);
    writer.string(state.syscall_buffer);
    const std::uint64_t input_count = static_cast<std::uint64_t>(state.console_input.size());
    writer.pod(input_count);
    for (long long value : state.console_input) writer.pod(value);

    writer.pod(static_cast<std::uint8_t>(state.privilege));
    writer.pod(static_cast<std::uint8_t>(state.previous_privilege));
    writer.pod(static_cast<std::uint8_t>(state.interrupt_enable));
    writer.pod(static_cast<std::uint8_t>(state.previous_interrupt_enable));
    writer.pod(static_cast<std::uint8_t>(state.trap_routing_enabled));
    writer.pod(state.epc); writer.pod(state.cause); writer.pod(state.tvec); writer.pod(state.scratch);
    writer.pod(state.cycle_count); writer.pod(state.branch_instructions_count);
    writer.pod(state.decode_instructions_count); writer.pod(state.timer_reload);
    writer.pod(state.timer_counter);
    writer.pod(static_cast<std::uint8_t>(state.timer_enable));
    writer.pod(static_cast<std::uint8_t>(state.timer_pending));
    writer.pod(state.user_imem_base); writer.pod(state.user_imem_limit);
    writer.pod(state.user_dmem_base); writer.pod(state.user_dmem_limit);
    writer.pod(state.syscall_id);
    writer.pod(static_cast<std::uint8_t>(state.console_char_mode));
    writer.pod(static_cast<std::uint8_t>(state.mmu_enable));
    writer.pod(state.mouse_x); writer.pod(state.mouse_y); writer.pod(state.mouse_btn);
    writer.pod(state.gpu_x1); writer.pod(state.gpu_y1); writer.pod(state.gpu_x2);
    writer.pod(state.gpu_y2); writer.pod(state.gpu_color); writer.pod(state.gpu_page);
    writer.pod(state.gpu_mode); writer.pod(state.sprite_x); writer.pod(state.sprite_y);
    writer.pod(state.sprite_attr); writer.pod(state.block_index); writer.pod(state.block_addr);
    writer.pod(state.block_status); writer.pod(state.power_control);
    writer.pod(state.user_imem_ptbr); writer.pod(state.user_imem_pages);
    writer.pod(state.user_dmem_ptbr); writer.pod(state.user_dmem_pages);
    writer.pod(state.page_fault_addr); writer.pod(state.page_fault_access);
    writer.pod(static_cast<std::uint8_t>(state.atomic_reservation_valid));
    writer.pod(state.atomic_reservation_addr); writer.pod(state.standalone_heap_break);
    writer.pod(state.active_core);
    const std::uint64_t core_count = static_cast<std::uint64_t>(state.coreCount());
    writer.pod(core_count);
    for (const auto& core : state.cores) writer.pod(core.current_process);
    const std::uint64_t queue_count = static_cast<std::uint64_t>(state.core_run_queues.size());
    writer.pod(queue_count);
    for (const auto& queue : state.core_run_queues) {
        writer.pod(static_cast<std::uint64_t>(queue.size()));
        for (int pid : queue) writer.pod(pid);
    }
    writer.pod(static_cast<std::uint64_t>(state.global_run_queue.size()));
    for (int pid : state.global_run_queue) writer.pod(pid);
    writer.pod(static_cast<std::uint8_t>(state.execution_backend));
    writer.pod(static_cast<std::uint8_t>(state.block_cache_enabled));
    writer.pod(state.trace_jit_hot_threshold);
    writer.pod(state.executable_mapping_generation);
    writer.pod(state.mmu_generation);
    writer.finish();
    if (!writer.ok) {
        setError(error, "failed to write checkpoint state: " + path.string());
        return false;
    }
    return true;
}

inline bool readCheckpointState(const std::filesystem::path& path,
                                std::unique_ptr<vm::VMState>& output,
                                std::string* error = nullptr) {
    try {
    CheckpointReader reader(path);
    if (!reader.ok) {
        setError(error, "failed to open checkpoint state: " + path.string());
        return false;
    }
    std::uint64_t magic = 0;
    std::uint32_t version = 0;
    std::int32_t imem_size = 0;
    std::int32_t dmem_size = 0;
    std::uint8_t imem_sparse = 0;
    std::uint8_t dmem_sparse = 0;
    std::int32_t block_count = 0;
    vm::ProductionProfile profile;
    if (!reader.pod(magic) || !reader.pod(version) || magic != kCheckpointStateMagic ||
        version != kCheckpointStateVersion || !reader.pod(imem_size) ||
        !reader.pod(dmem_size) || !reader.pod(imem_sparse) || !reader.pod(dmem_sparse) ||
        !reader.pod(block_count) || imem_size <= 0 || dmem_size <= 0 ||
        block_count <= 0 || !readCheckpointProfile(reader, profile) ||
        !checkpointProfileSane(profile) ||
        imem_size > (1 << 29) || dmem_size > (1 << 29) ||
        block_count > (1 << 29) || imem_sparse > 1 || dmem_sparse > 1) {
        setError(error, "checkpoint state header is invalid");
        return false;
    }
    auto machine = std::make_unique<vm::VMState>(profile);
    machine->imem.resize(imem_size, imem_sparse ? vm::MemoryBacking::Sparse : vm::MemoryBacking::Dense);
    machine->dmem.resize(dmem_size, dmem_sparse ? vm::MemoryBacking::Sparse : vm::MemoryBacking::Dense);
    machine->resetBlockDevice(block_count);
    std::uint64_t count = 0;
    if (!reader.pod(count) || count > 100000000ULL) {
        setError(error, "checkpoint instruction record count is invalid");
        return false;
    }
    for (std::uint64_t index = 0; index < count; ++index) {
        std::int32_t address = 0;
        std::uint64_t bits = 0;
        if (!reader.pod(address) || !reader.pod(bits) ||
            machine->imem.write(address, isa::TritWord27{bits}) != vm::MemFaultCode::OK) {
            setError(error, "checkpoint instruction memory is invalid");
            return false;
        }
    }
    if (!reader.pod(count) || count > 100000000ULL) {
        setError(error, "checkpoint data record count is invalid");
        return false;
    }
    for (std::uint64_t index = 0; index < count; ++index) {
        std::int32_t address = 0;
        vm::TernaryValue value;
        if (!reader.pod(address) || !readCheckpointValue(reader, value) ||
            machine->dmem.store(address, value) != vm::MemFaultCode::OK) {
            setError(error, "checkpoint data memory is invalid");
            return false;
        }
    }
    for (int index = 0; index < isa::REG_COUNT; ++index) {
        vm::TernaryValue value;
        std::uint8_t mode = 0;
        if (!readCheckpointValue(reader, value) || !reader.pod(mode) ||
            mode > static_cast<std::uint8_t>(TernaryMode::L50)) {
            setError(error, "checkpoint register file is invalid");
            return false;
        }
        machine->regfile.reg[static_cast<std::size_t>(index)] = value;
        machine->regfile.view_mode[static_cast<std::size_t>(index)] =
            static_cast<TernaryMode>(mode);
    }
    std::uint8_t status = 0;
    if (!reader.pod(machine->pc) || !reader.pod(status) ||
        status > static_cast<std::uint8_t>(vm::VMStatus::WAITING) ||
        !readCheckpointValue(reader, machine->trap_reg) ||
        !reader.pod(machine->required_features) || !reader.pod(machine->supported_features) ||
        !reader.pod(machine->asid)) {
        setError(error, "checkpoint control state is invalid");
        return false;
    }
    machine->status = static_cast<vm::VMStatus>(status);
    for (auto& entry : machine->instruction_tlb) if (!readCheckpointTlbEntry(reader, entry)) return false;
    for (auto& entry : machine->data_tlb) if (!readCheckpointTlbEntry(reader, entry)) return false;
    for (auto& entry : machine->unified_l2_tlb) if (!readCheckpointTlbEntry(reader, entry)) return false;
    if (!reader.pod(machine->tlb_stats.instruction_l1_hits) ||
        !reader.pod(machine->tlb_stats.data_l1_hits) || !reader.pod(machine->tlb_stats.l2_hits) ||
        !reader.pod(machine->tlb_stats.misses) || !reader.pod(machine->tlb_stats.walks) ||
        !reader.pod(machine->tlb_stats.evictions) || !reader.pod(machine->tlb_stats.superpage_hits) ||
        !reader.pod(machine->tlb_stats.shootdowns) || !reader.pod(machine->tlb_replacement_clock) ||
        !reader.pod(machine->vector_length)) {
        setError(error, "checkpoint MMU/vector header is invalid");
        return false;
    }
    machine->vregfile.reset(machine->vector_length);
    for (auto& reg : machine->vregfile.reg) {
        std::uint64_t lane_count = 0;
        if (!reader.pod(lane_count) || lane_count > 4096ULL) return false;
        reg.lane.assign(static_cast<std::size_t>(lane_count), vm::TernaryValue::zero());
        for (auto& value : reg.lane) if (!readCheckpointValue(reader, value)) return false;
    }
    std::uint64_t fault_count = 0;
    if (!reader.pod(fault_count) || fault_count > 4096ULL) return false;
    machine->vector_faults.fault_valid.assign(static_cast<std::size_t>(fault_count), 0);
    machine->vector_faults.fault_class.assign(static_cast<std::size_t>(fault_count), isa::TrapCode::TRAP_MEM_FAULT);
    for (std::size_t index = 0; index < static_cast<std::size_t>(fault_count); ++index) {
        std::uint8_t trap = 0;
        if (!reader.pod(machine->vector_faults.fault_valid[index]) || !reader.pod(trap)) return false;
        machine->vector_faults.fault_class[index] = static_cast<isa::TrapCode>(trap);
    }
    if (!reader.pod(machine->vector_faults.first_failing_lane) ||
        !readCheckpointValue(reader, machine->accumulator) ||
        !reader.string(machine->syscall_buffer)) return false;
    if (!reader.pod(count) || count > 100000000ULL) return false;
    machine->console_input.resize(static_cast<std::size_t>(count));
    for (long long& value : machine->console_input) if (!reader.pod(value)) return false;

    std::uint8_t privilege = 0, previous_privilege = 0, interrupt = 0,
                 previous_interrupt = 0, trap_routing = 0, timer_enable = 0,
                 timer_pending = 0, char_mode = 0, mmu_enable = 0,
                 atomic_valid = 0, backend = 0, cache_enabled = 0;
    if (!reader.pod(privilege) || !reader.pod(previous_privilege) ||
        !reader.pod(interrupt) || !reader.pod(previous_interrupt) ||
        !reader.pod(trap_routing) || !reader.pod(machine->epc) || !reader.pod(machine->cause) ||
        !reader.pod(machine->tvec) || !reader.pod(machine->scratch) ||
        !reader.pod(machine->cycle_count) || !reader.pod(machine->branch_instructions_count) ||
        !reader.pod(machine->decode_instructions_count) || !reader.pod(machine->timer_reload) ||
        !reader.pod(machine->timer_counter) || !reader.pod(timer_enable) ||
        !reader.pod(timer_pending) || !reader.pod(machine->user_imem_base) ||
        !reader.pod(machine->user_imem_limit) || !reader.pod(machine->user_dmem_base) ||
        !reader.pod(machine->user_dmem_limit) || !reader.pod(machine->syscall_id) ||
        !reader.pod(char_mode) || !reader.pod(mmu_enable) || !reader.pod(machine->mouse_x) ||
        !reader.pod(machine->mouse_y) || !reader.pod(machine->mouse_btn) || !reader.pod(machine->gpu_x1) ||
        !reader.pod(machine->gpu_y1) || !reader.pod(machine->gpu_x2) || !reader.pod(machine->gpu_y2) ||
        !reader.pod(machine->gpu_color) || !reader.pod(machine->gpu_page) || !reader.pod(machine->gpu_mode) ||
        !reader.pod(machine->sprite_x) || !reader.pod(machine->sprite_y) || !reader.pod(machine->sprite_attr) ||
        !reader.pod(machine->block_index) || !reader.pod(machine->block_addr) || !reader.pod(machine->block_status) ||
        !reader.pod(machine->power_control) || !reader.pod(machine->user_imem_ptbr) || !reader.pod(machine->user_imem_pages) ||
        !reader.pod(machine->user_dmem_ptbr) || !reader.pod(machine->user_dmem_pages) || !reader.pod(machine->page_fault_addr) ||
        !reader.pod(machine->page_fault_access) || !reader.pod(atomic_valid) || !reader.pod(machine->atomic_reservation_addr) ||
        !reader.pod(machine->standalone_heap_break) || !reader.pod(machine->active_core)) return false;
    machine->privilege = static_cast<isa::PrivilegeMode>(privilege);
    machine->previous_privilege = static_cast<isa::PrivilegeMode>(previous_privilege);
    machine->interrupt_enable = interrupt != 0;
    machine->previous_interrupt_enable = previous_interrupt != 0;
    machine->trap_routing_enabled = trap_routing != 0;
    machine->timer_enable = timer_enable != 0;
    machine->timer_pending = timer_pending != 0;
    machine->console_char_mode = char_mode != 0;
    machine->mmu_enable = mmu_enable != 0;
    machine->atomic_reservation_valid = atomic_valid != 0;
    if (!reader.pod(count) || count > 256ULL) return false;
    machine->configureCores(static_cast<int>(std::max<std::uint64_t>(1, count)));
    for (std::uint64_t index = 0; index < count; ++index) {
        if (!reader.pod(machine->cores[static_cast<std::size_t>(index)].current_process)) return false;
    }
    std::uint64_t queue_count = 0;
    if (!reader.pod(queue_count) || queue_count > 256ULL) return false;
    machine->core_run_queues.assign(static_cast<std::size_t>(machine->coreCount()), {});
    for (std::uint64_t index = 0; index < queue_count && index < static_cast<std::uint64_t>(machine->coreCount()); ++index) {
        std::uint64_t depth = 0;
        if (!reader.pod(depth) || depth > 1000000ULL) return false;
        for (std::uint64_t item = 0; item < depth; ++item) { int pid = 0; if (!reader.pod(pid)) return false; machine->core_run_queues[static_cast<std::size_t>(index)].push_back(pid); }
    }
    if (!reader.pod(count) || count > 1000000ULL) return false;
    machine->global_run_queue.clear();
    for (std::uint64_t item = 0; item < count; ++item) { int pid = 0; if (!reader.pod(pid)) return false; machine->global_run_queue.push_back(pid); }
    if (!reader.pod(backend) || !reader.pod(cache_enabled) || !reader.pod(machine->trace_jit_hot_threshold) ||
        !reader.pod(machine->executable_mapping_generation) || !reader.pod(machine->mmu_generation) ||
        backend > static_cast<std::uint8_t>(vm::VMExecutionBackend::NativeX64Jit)) return false;
    machine->execution_backend = static_cast<vm::VMExecutionBackend>(backend);
    machine->block_cache_enabled = cache_enabled != 0;
    machine->invalidateBlockCache();
    machine->invalidateTraceJit();
    output = std::move(machine);
    if (!reader.ok) {
        setError(error, "checkpoint state is truncated");
        return false;
    }
    return true;
    } catch (const std::exception& exc) {
        setError(error, std::string("checkpoint state restore failed: ") + exc.what());
        output.reset();
        return false;
    } catch (...) {
        setError(error, "checkpoint state restore failed with an unknown exception");
        output.reset();
        return false;
    }
}

inline constexpr std::uint64_t kCheckpointJournalMagic =
    0x314c4e52504a5254ULL; // "TRR PNRL1"
inline constexpr std::uint32_t kCheckpointJournalVersion = 2;

inline const char* inputEventKindName(TosInputEventKind kind) {
    switch (kind) {
        case TosInputEventKind::KeyboardWord: return "keyboard";
        case TosInputEventKind::Text: return "text";
        case TosInputEventKind::Mouse: return "mouse";
        default: return "unknown";
    }
}

inline bool inputProvenanceFieldSane(const std::string& value) {
    // Provenance is diagnostic metadata, not guest data.  Keep it bounded and
    // reject control characters so JSONL and the binary journal have one
    // canonical representation across host platforms.
    if (value.empty() || value.size() > 256) return false;
    for (const unsigned char ch : value) {
        if (ch < 0x20 || ch == 0x7f) return false;
    }
    return true;
}

inline std::string inputProvenanceOrDefault(const std::string& value) {
    return inputProvenanceFieldSane(value) ? value : "host.api";
}

inline std::string inputChannelOrDefault(const TosInputJournalEvent& event) {
    if (inputProvenanceFieldSane(event.channel)) return event.channel;
    return inputEventKindName(event.kind);
}

// Defined below with the other host-runtime JSON helpers; the declaration is
// kept here so journal writers can live next to the binary format contract.
inline void writeJsonString(std::ostream& out, const std::string& value);

inline bool writeInputJournalJson(
    const std::filesystem::path& path,
    const std::vector<TosInputJournalEvent>& events,
    std::string* error = nullptr) {
    std::ofstream out(path, std::ios::trunc);
    if (!out.good()) {
        setError(error, "failed to write input journal: " + path.string());
        return false;
    }
    for (std::size_t index = 0; index < events.size(); ++index) {
        const TosInputJournalEvent& event = events[index];
        if (event.sequence != index ||
            event.kind < TosInputEventKind::KeyboardWord ||
            event.kind > TosInputEventKind::Mouse ||
            !inputProvenanceFieldSane(event.source)) {
            setError(error, "input journal ordering or provenance is invalid");
            return false;
        }
        out << "{\"schema\":\"trit.input_journal.v1\",";
        out << "\"sequence\":" << event.sequence << ",";
        out << "\"cycle\":" << event.cycle << ",";
        out << "\"kind\":" << static_cast<int>(event.kind) << ",";
        out << "\"value0\":" << event.value0 << ",";
        out << "\"value1\":" << event.value1 << ",";
        out << "\"value2\":" << event.value2 << ",";
        out << "\"text\":";
        writeJsonString(out, event.text);
        out << ",\"provenance\":{\"source\":";
        writeJsonString(out, event.source);
        out << ",\"channel\":";
        writeJsonString(out, inputChannelOrDefault(event));
        out << ",\"external\":true}}\n";
    }
    if (!out.good()) {
        setError(error, "failed to write input journal: " + path.string());
        return false;
    }
    return true;
}

inline bool writeCheckpointJournal(
    const std::filesystem::path& path,
    const std::vector<TosInputJournalEvent>& events,
    std::string* error = nullptr) {
    CheckpointWriter writer(path);
    if (!writer.ok) {
        setError(error, "failed to open checkpoint journal: " + path.string());
        return false;
    }
    writer.pod(kCheckpointJournalMagic);
    writer.pod(kCheckpointJournalVersion);
    writer.pod(static_cast<std::uint64_t>(events.size()));
    for (std::size_t index = 0; index < events.size(); ++index) {
        const auto& event = events[index];
        if (event.sequence != index ||
            event.kind < TosInputEventKind::KeyboardWord ||
            event.kind > TosInputEventKind::Mouse ||
            !inputProvenanceFieldSane(event.source) ||
            !inputProvenanceFieldSane(inputChannelOrDefault(event))) {
            setError(error, "input journal ordering or provenance is invalid");
            return false;
        }
        writer.pod(event.sequence);
        writer.pod(event.cycle);
        writer.pod(static_cast<std::uint8_t>(event.kind));
        writer.pod(event.value0);
        writer.pod(event.value1);
        writer.pod(event.value2);
        writer.string(event.text);
        writer.string(event.source);
        writer.string(inputChannelOrDefault(event));
    }
    writer.finish();
    if (!writer.ok) {
        setError(error, "failed to write checkpoint journal: " + path.string());
        return false;
    }
    return true;
}

inline bool readCheckpointJournal(
    const std::filesystem::path& path,
    std::vector<TosInputJournalEvent>& events,
    std::string* error = nullptr) {
    CheckpointReader reader(path);
    std::uint64_t magic = 0;
    std::uint32_t version = 0;
    std::uint64_t count = 0;
    if (!reader.ok || !reader.pod(magic) || !reader.pod(version) ||
        !reader.pod(count) || magic != kCheckpointJournalMagic ||
        (version != kCheckpointStateVersion && version != kCheckpointJournalVersion) ||
        count > 100000000ULL) {
        setError(error, "checkpoint journal header is invalid");
        return false;
    }
    events.clear();
    events.reserve(static_cast<std::size_t>(count));
    for (std::uint64_t index = 0; index < count; ++index) {
        TosInputJournalEvent event;
        std::uint8_t kind = 0;
        if (!reader.pod(event.sequence) || !reader.pod(event.cycle) ||
            !reader.pod(kind) || kind < static_cast<std::uint8_t>(TosInputEventKind::KeyboardWord) ||
            kind > static_cast<std::uint8_t>(TosInputEventKind::Mouse) ||
            !reader.pod(event.value0) || !reader.pod(event.value1) ||
            !reader.pod(event.value2) || !reader.string(event.text)) {
            setError(error, "checkpoint journal is truncated or invalid");
            return false;
        }
        event.kind = static_cast<TosInputEventKind>(kind);
        if (version == kCheckpointJournalVersion) {
            if (!reader.string(event.source) || !reader.string(event.channel) ||
                !inputProvenanceFieldSane(event.source) ||
                !inputProvenanceFieldSane(event.channel)) {
                setError(error, "checkpoint journal provenance is invalid");
                return false;
            }
        } else {
            event.source = "legacy.unknown";
            event.channel = inputEventKindName(event.kind);
        }
        if (event.sequence != index) {
            setError(error, "checkpoint journal sequence is not contiguous");
            return false;
        }
        events.push_back(std::move(event));
    }
    return reader.ok;
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
    appendPod<std::int32_t>(out, entry.isa_version);
    appendPod<std::uint64_t>(out, entry.required_features);
    appendPod<std::int32_t>(out, entry.function_abi_version);
    appendPod<std::int32_t>(out, entry.syscall_abi_version);
    return true;
}

inline bool readManifestEntry(const std::vector<std::uint8_t>& in,
                              std::size_t& offset,
                              TosAppManifestEntry& entry) {
    if (!readString(in, offset, entry.name) ||
        !readString(in, offset, entry.path) ||
        !readPod(in, offset, entry.text_ppn) ||
        !readPod(in, offset, entry.entry_pc) ||
        !readPod(in, offset, entry.text_pages) ||
        !readPod(in, offset, entry.data_pages) ||
        !readPod(in, offset, entry.stack_words)) {
        return false;
    }
    return readPod(in, offset, entry.isa_version) &&
           readPod(in, offset, entry.required_features) &&
           readPod(in, offset, entry.function_abi_version) &&
           readPod(in, offset, entry.syscall_abi_version);
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
    appendPod<std::int32_t>(out, image.manifest.isa_version);
    appendPod<std::uint64_t>(out, image.manifest.required_features);
    appendPod<std::int32_t>(out, image.manifest.scalar_word_trits);
    appendPod<std::int32_t>(out, image.manifest.base_page_words);
    appendPod<std::int32_t>(out, image.manifest.function_abi_version);
    appendPod<std::int32_t>(out, image.manifest.syscall_abi_version);

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
    for (std::size_t index = 0; index < image.data_words.size(); ++index) {
        const std::uint64_t raw =
            index < image.data_words_raw.size()
                ? image.data_words_raw[index]
                : vm::convertValue(
                      vm::ops::fromLong(image.data_words[index]),
                      TernaryMode::T40).asTriple().data;
        appendPod<std::uint64_t>(out, raw);
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
        setError(
            error,
            "boot image format v" + std::to_string(version) +
                " is not supported by the v2 runtime; use "
                "migrate_tos_artifacts with a fresh v2 template");
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
    if (!readPod(payload, offset, decoded.manifest.isa_version) ||
        !readPod(payload, offset, decoded.manifest.required_features) ||
        !readPod(payload, offset, decoded.manifest.scalar_word_trits) ||
        !readPod(payload, offset, decoded.manifest.base_page_words) ||
        !readPod(payload, offset,
                 decoded.manifest.function_abi_version) ||
        !readPod(payload, offset,
                 decoded.manifest.syscall_abi_version)) {
        setError(error, "boot image architecture metadata is truncated");
        return false;
    }

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
    decoded.data_words_raw.assign(data_words, 0);
    for (std::uint32_t i = 0; i < data_words; ++i) {
        std::uint64_t raw = 0;
        if (!readPod(payload, offset, raw) ||
            raw > 12157665459056928801ULL) {
            setError(error, "boot image raw T40 data is invalid or truncated");
            return false;
        }
        decoded.data_words_raw[static_cast<std::size_t>(i)] = raw;
        decoded.data_words[static_cast<std::size_t>(i)] =
            vm::ops::toLong(vm::TernaryValue::fromTriple(Triple{raw}));
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
    const int base = block * vm::STORAGE_BLOCK_WORDS;
    for (int i = 0; i < vm::STORAGE_BLOCK_WORDS; ++i) {
        if (image[static_cast<std::size_t>(base + i)] != 0) return false;
    }
    return true;
}

inline std::vector<int> nonZeroBlocks(const std::vector<long long>& image) {
    std::vector<int> blocks;
    if (image.empty() ||
        static_cast<int>(image.size()) % vm::STORAGE_BLOCK_WORDS != 0) {
        return blocks;
    }
    const int block_count =
        static_cast<int>(image.size()) / vm::STORAGE_BLOCK_WORDS;
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

// Diagnostics intentionally use a tiny, dependency-free PNG encoder.  The
// framebuffer is already materialized as RGBA pixels, so a deterministic PNG
// with unfiltered scanlines and DEFLATE stored blocks is sufficient here and
// avoids making zlib/SDL image libraries part of the host-runtime ABI.
inline void appendPngU32(std::vector<std::uint8_t>& out, std::uint32_t value) {
    out.push_back(static_cast<std::uint8_t>((value >> 24) & 0xff));
    out.push_back(static_cast<std::uint8_t>((value >> 16) & 0xff));
    out.push_back(static_cast<std::uint8_t>((value >> 8) & 0xff));
    out.push_back(static_cast<std::uint8_t>(value & 0xff));
}

inline std::uint32_t pngCrc32(const std::uint8_t* bytes, std::size_t size) {
    std::uint32_t crc = 0xffffffffU;
    for (std::size_t i = 0; i < size; ++i) {
        crc ^= bytes[i];
        for (int bit = 0; bit < 8; ++bit) {
            crc = (crc >> 1) ^ (0xedb88320U &
                                static_cast<std::uint32_t>(-
                                    static_cast<std::int32_t>(crc & 1U)));
        }
    }
    return ~crc;
}

inline std::uint32_t pngAdler32(const std::vector<std::uint8_t>& bytes) {
    constexpr std::uint32_t kModulus = 65521U;
    std::uint32_t a = 1U;
    std::uint32_t b = 0U;
    for (const std::uint8_t byte : bytes) {
        a += byte;
        if (a >= kModulus) a -= kModulus;
        b += a;
        if (b >= kModulus) b -= kModulus;
    }
    return (b << 16) | a;
}

inline bool appendPngChunk(std::vector<std::uint8_t>& out,
                           const char type[4],
                           const std::vector<std::uint8_t>& data) {
    if (data.size() > static_cast<std::size_t>(std::numeric_limits<std::uint32_t>::max()) ||
        data.size() > std::numeric_limits<std::size_t>::max() - 4) {
        return false;
    }
    appendPngU32(out, static_cast<std::uint32_t>(data.size()));
    const std::size_t type_offset = out.size();
    out.insert(out.end(), reinterpret_cast<const std::uint8_t*>(type),
               reinterpret_cast<const std::uint8_t*>(type) + 4);
    out.insert(out.end(), data.begin(), data.end());
    appendPngU32(out, pngCrc32(out.data() + type_offset, 4 + data.size()));
    return true;
}

inline bool writeFramebufferPng(const std::filesystem::path& path,
                                const TosFramebufferSnapshot& framebuffer,
                                std::string* error = nullptr) {
    if (framebuffer.width <= 0 || framebuffer.height <= 0) {
        setError(error, "framebuffer PNG geometry is invalid");
        return false;
    }

    const std::size_t width = static_cast<std::size_t>(framebuffer.width);
    const std::size_t height = static_cast<std::size_t>(framebuffer.height);
    if (height > std::numeric_limits<std::size_t>::max() / width) {
        setError(error, "framebuffer PNG geometry overflows host size");
        return false;
    }
    const std::size_t pixel_count = width * height;
    if (framebuffer.rgba.size() != pixel_count) {
        setError(error, "framebuffer PNG pixel count does not match geometry");
        return false;
    }
    if (pixel_count >
        (std::numeric_limits<std::size_t>::max() - height) / 4) {
        setError(error, "framebuffer PNG scanline size overflows host size");
        return false;
    }

    const std::size_t scanline_size = pixel_count * 4 + height;
    std::vector<std::uint8_t> scanlines;
    scanlines.reserve(scanline_size);
    for (std::size_t y = 0; y < height; ++y) {
        scanlines.push_back(0); // PNG filter type: none.
        for (std::size_t x = 0; x < width; ++x) {
            const std::uint32_t pixel = framebuffer.rgba[y * width + x];
            scanlines.push_back(static_cast<std::uint8_t>((pixel >> 24) & 0xff));
            scanlines.push_back(static_cast<std::uint8_t>((pixel >> 16) & 0xff));
            scanlines.push_back(static_cast<std::uint8_t>((pixel >> 8) & 0xff));
            scanlines.push_back(static_cast<std::uint8_t>(pixel & 0xff));
        }
    }

    // Use zlib's no-compression stream and stored DEFLATE blocks.  Splitting
    // at 65535 bytes keeps the encoder valid for larger diagnostic frames.
    std::vector<std::uint8_t> compressed;
    compressed.reserve(scanlines.size() + 16);
    compressed.push_back(0x78); // CMF: deflate, 32K window.
    compressed.push_back(0x01); // FLG: no compression, valid FCHECK.
    std::size_t offset = 0;
    do {
        const std::size_t remaining = scanlines.size() - offset;
        const std::size_t block_size = std::min<std::size_t>(remaining, 65535);
        const bool final_block = offset + block_size == scanlines.size();
        compressed.push_back(static_cast<std::uint8_t>(final_block ? 0x01 : 0x00));
        const auto length = static_cast<std::uint16_t>(block_size);
        const auto inverse_length = static_cast<std::uint16_t>(0xffffU ^ length);
        compressed.push_back(static_cast<std::uint8_t>(length & 0xff));
        compressed.push_back(static_cast<std::uint8_t>((length >> 8) & 0xff));
        compressed.push_back(static_cast<std::uint8_t>(inverse_length & 0xff));
        compressed.push_back(static_cast<std::uint8_t>((inverse_length >> 8) & 0xff));
        compressed.insert(compressed.end(),
                          scanlines.begin() + static_cast<std::ptrdiff_t>(offset),
                          scanlines.begin() + static_cast<std::ptrdiff_t>(offset + block_size));
        offset += block_size;
    } while (offset < scanlines.size());
    appendPngU32(compressed, pngAdler32(scanlines));

    std::vector<std::uint8_t> ihdr;
    appendPngU32(ihdr, static_cast<std::uint32_t>(framebuffer.width));
    appendPngU32(ihdr, static_cast<std::uint32_t>(framebuffer.height));
    ihdr.push_back(8); // bit depth
    ihdr.push_back(6); // color type RGBA
    ihdr.push_back(0); // compression method
    ihdr.push_back(0); // filter method
    ihdr.push_back(0); // no interlace

    std::vector<std::uint8_t> png = {
        0x89, 0x50, 0x4e, 0x47, 0x0d, 0x0a, 0x1a, 0x0a,
    };
    if (!appendPngChunk(png, "IHDR", ihdr) ||
        !appendPngChunk(png, "IDAT", compressed) ||
        !appendPngChunk(png, "IEND", {})) {
        setError(error, "framebuffer PNG chunk is too large");
        return false;
    }

    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    if (!out.good()) {
        setError(error, "failed to write framebuffer_snapshot.png");
        return false;
    }
    out.write(reinterpret_cast<const char*>(png.data()),
              static_cast<std::streamsize>(png.size()));
    if (!out.good()) {
        setError(error, "failed to write framebuffer_snapshot.png");
        return false;
    }
    return true;
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
constexpr int kProcessRowWords = 9;
constexpr int kProcPid = 0;
constexpr int kProcNamespace = 1;
constexpr int kProcState = 2;
constexpr int kProcPriority = 3;
constexpr int kProcQuota = 4;
constexpr int kProcContext = 5;
constexpr int kProcWaitChannel = 6;
constexpr int kProcVersion = 7;
constexpr int kProcVectorContext = 8;
constexpr int kProcRunnable = 1;
constexpr int kProcRunning = 2;
constexpr int kProcBlocked = 3;
constexpr int kProcSleeping = 4;
constexpr int kProcessBase = 390000;
constexpr int kQuotaRowWords = 4;
constexpr int kQuotaBase = 391000;
constexpr int kQuotaLimit = 1;
constexpr int kQuotaUsed = 2;
constexpr int kTaskContextEpc = 0;
constexpr int kTaskContextStatus = 1;
constexpr int kTaskContextImemPtbr = 2;
constexpr int kTaskContextImemPages = 3;
constexpr int kTaskContextDmemPtbr = 4;
constexpr int kTaskContextDmemPages = 5;
constexpr int kTaskContextRegBase = 6;
constexpr int kTaskContextSp = 31;
constexpr int kProcParentPidBase = 130000;
constexpr int kProcExitStatusBase = 130100;
constexpr int kProcSignalPendingBase = 130200;
constexpr int kProcCapsBase = 131350;
constexpr int kWaitKindBase = 138600;
constexpr int kWaitDeadlineBase = 138700;
constexpr int kAppMax = 256;
constexpr int kAppRowWords = 10;
constexpr int kAppRegistryBase = 665000;
constexpr int kAppNamespace = 0;
constexpr int kAppPathHash = 1;
constexpr int kAppInode = 2;
constexpr int kAppCaps = 3;
constexpr int kAppQuota = 4;
constexpr int kAppTrusted = 5;
constexpr int kAppVersion = 6;
constexpr int kAppLaunchCount = 7;
constexpr int kAppMaxImage = 8;
constexpr int kAppFlags = 9;

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
    if (image.manifest.format_version != TOS_BOOT_FORMAT_VERSION) {
        detail::setError(
            error,
            "boot image format v" +
                std::to_string(image.manifest.format_version) +
                " is not supported by the v2 runtime; use "
                "migrate_tos_artifacts with a fresh v2 template");
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
        static_cast<int>(image.rootfs_words.size()) %
                vm::STORAGE_BLOCK_WORDS !=
            0) {
        detail::setError(error, "boot image root filesystem seed is not block aligned");
        return false;
    }
    if (image.manifest.sections.empty()) {
        detail::setError(error, "boot image has no section table entries");
        return false;
    }
    const bool v2_image =
        image.manifest.isa_version == architecture::v2::ISA_VERSION &&
        image.manifest.scalar_word_trits ==
            architecture::v2::SCALAR_WORD_TRITS &&
        image.manifest.base_page_words ==
            architecture::v2::BASE_PAGE_WORDS &&
        image.manifest.function_abi_version ==
            architecture::v2::FUNCTION_ABI_VERSION &&
        image.manifest.syscall_abi_version ==
            architecture::v2::SYSCALL_ABI_VERSION &&
        (image.manifest.required_features &
         isa::featureBit(architecture::v2::FEATURE_BASE_V2)) != 0;
    if (!v2_image) {
        detail::setError(
            error, "boot image v3 architecture metadata is incompatible");
        return false;
    }
    if (!image.data_words_raw.empty() &&
        image.data_words_raw.size() != image.data_words.size()) {
        detail::setError(error,
            "boot image raw T40 data count does not match data words");
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
    image.manifest.isa_version =
        static_cast<int>(assembled.isa_version);
    image.manifest.required_features = assembled.required_features;
    image.program = assembled.program;
    image.rootfs_words = rootfs_words;
    image.data_words.reserve(assembled.data.size());
    image.data_words_raw.reserve(assembled.data.size());
    for (const vm::TernaryValue& value : assembled.data) {
        image.data_words.push_back(vm::ops::toLong(value));
        image.data_words_raw.push_back(
            vm::convertValue(value, TernaryMode::T40).asTriple().data);
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
        static_cast<int>(block_image.size()) %
                vm::STORAGE_BLOCK_WORDS !=
            0) {
        detail::setError(error, "disk seed image is empty or not block aligned");
        return false;
    }
    if (!overwrite && std::filesystem::exists(path)) return true;
    if (!detail::parentDirectoryExistsOrCreate(path, error)) return false;

    const std::vector<int> blocks = detail::nonZeroBlocks(block_image);
    std::vector<std::uint8_t> records;
    for (int block : blocks) {
        detail::appendPod<std::int32_t>(records, block);
        const int base = block * vm::STORAGE_BLOCK_WORDS;
        for (int word = 0; word < vm::STORAGE_BLOCK_WORDS; ++word) {
            const long long value =
                block_image[static_cast<std::size_t>(base + word)];
            constexpr std::uint64_t kTritWord27Mask =
                (std::uint64_t{1} << 54) - 1;
            const bool executable_text =
                block >= vm::TDISK_EXECUTABLE_TEXT_FIRST_BLOCK;
            if (executable_text &&
                (value < 0 || static_cast<std::uint64_t>(value) > kTritWord27Mask)) {
                detail::setError(error, "executable text block contains invalid TritWord27 bits");
                return false;
            }
            const std::uint64_t raw = executable_text
                ? static_cast<std::uint64_t>(value)
                : vm::convertValue(
                      vm::ops::fromLong(value),
                      TernaryMode::T40).asTriple().data;
            detail::appendPod<std::uint64_t>(records, raw);
        }
    }
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    if (!out.good()) {
        detail::setError(error, "failed to open sparse disk for writing: " + path);
        return false;
    }
    const std::uint64_t magic = TOS_SPARSE_DISK_MAGIC;
    const std::uint32_t version = TOS_SPARSE_DISK_VERSION;
    const std::uint32_t block_words = vm::STORAGE_BLOCK_WORDS;
    const std::uint64_t generation = 1;
    const std::uint64_t checksum = detail::fnv1a(records);
    const int count = static_cast<int>(blocks.size());
    out.write(reinterpret_cast<const char*>(&magic), sizeof(magic));
    out.write(reinterpret_cast<const char*>(&version), sizeof(version));
    out.write(reinterpret_cast<const char*>(&block_words),
              sizeof(block_words));
    out.write(reinterpret_cast<const char*>(&generation),
              sizeof(generation));
    out.write(reinterpret_cast<const char*>(&checksum), sizeof(checksum));
    out.write(reinterpret_cast<const char*>(&count), sizeof(count));
    out.write(reinterpret_cast<const char*>(records.data()),
              static_cast<std::streamsize>(records.size()));
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
    if (!machine.configureArchitecture(
            isa::IsaEncodingVersion::V2,
            image.manifest.required_features)) {
        detail::setError(
            error, "boot image requires unsupported architecture features");
        return false;
    }
    if (!machine.imem.loadProgram(image.program, 0)) {
        detail::setError(error, "failed to load boot image text segment");
        return false;
    }
    for (int i = 0; i < static_cast<int>(image.data_words.size()); ++i) {
        const vm::TernaryValue data_word =
            i < static_cast<int>(image.data_words_raw.size())
                ? vm::TernaryValue::fromTriple(
                      Triple{image.data_words_raw[static_cast<std::size_t>(i)]})
                : vm::ops::fromLong(
                      image.data_words[static_cast<std::size_t>(i)]);
        if (machine.dmem.store(i, data_word) !=
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
            // Keep the production rejection actionable without teaching the
            // live VM how to mount historical formats. The standalone
            // migrator owns those readers and emits a fresh tDisk v2 image.
            std::uint64_t magic = 0;
            std::uint32_t version = 0;
            std::ifstream disk(disk_path, std::ios::binary);
            disk.read(reinterpret_cast<char*>(&magic), sizeof(magic));
            if (magic == TOS_LEGACY_SPARSE_DISK_MAGIC) {
                detail::setError(
                    error,
                    "legacy tDisk v1 is rejected by the v2 runtime; use "
                    "migrate_tos_artifacts for offline conversion: " +
                        disk_path);
            } else if (magic == TOS_SPARSE_DISK_MAGIC) {
                disk.read(reinterpret_cast<char*>(&version), sizeof(version));
                detail::setError(
                    error,
                    "tDisk version " + std::to_string(version) +
                        " is not supported by the v2 runtime; use "
                        "migrate_tos_artifacts for offline conversion: " +
                        disk_path);
            } else {
                detail::setError(
                    error,
                    "unsupported or corrupt tDisk backing; production runtime "
                    "accepts only checksummed tDisk v2 (use "
                    "migrate_tos_artifacts for legacy inputs): " +
                        disk_path);
            }
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
        machine->setExecutionBackend(config_.execution_backend);
        if (!loadBootImageIntoVm(*machine, image, config_.disk_path, error)) return false;

        std::lock_guard<std::mutex> lock(mutex_);
        image_ = std::move(image);
        machine_ = std::move(machine);
        paused_ = config_.start_paused;
        boot_generation_ = 1;
        guest_reboot_count_ = 0;
        syscall_trace_.clear();
        syscall_trace_sequence_ = 0;
        checkpoint_.reset();
        next_checkpoint_sequence_ = 0;
        input_journal_.clear();
        next_input_sequence_ = 0;
        return true;
    }

    [[nodiscard]] bool loadImage(const TosBootImage& image, std::string* error = nullptr) {
        shutdownWorkerOnly();
        compactLoadedDiskIfNeeded();
        if (!validateBootImage(image, error)) return false;
        vm::ProductionProfile profile = profileForManifest(image.manifest);
        auto machine = std::make_unique<vm::VMState>(profile);
        machine->setExecutionBackend(config_.execution_backend);
        if (!loadBootImageIntoVm(*machine, image, config_.disk_path, error)) return false;

        std::lock_guard<std::mutex> lock(mutex_);
        image_ = image;
        machine_ = std::move(machine);
        paused_ = config_.start_paused;
        boot_generation_ = 1;
        guest_reboot_count_ = 0;
        syscall_trace_.clear();
        syscall_trace_sequence_ = 0;
        checkpoint_.reset();
        next_checkpoint_sequence_ = 0;
        input_journal_.clear();
        next_input_sequence_ = 0;
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

    void setSyscallTraceEnabled(bool enabled) {
        std::lock_guard<std::mutex> lock(mutex_);
        config_.record_syscall_trace = enabled;
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
        return resetMachineLocked(false, error);
    }

    [[nodiscard]] vm::RunResult runForSteps(int steps) {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!machine_) {
            return {vm::VMStatus::HALTED, 0, 0, isa::TrapCode::TRAP_ILLEGAL_OP, "VM not loaded"};
        }
        vm::VMHooks hooks;
        if (config_.record_syscall_trace) {
            hooks.onInstruction = [this](const vm::VMState&,
                                          const vm::VMExecutionRecord& record) {
                recordSyscall(record);
            };
        }
        vm::RunResult result = vm::run(
            *machine_, steps,
            config_.record_syscall_trace ? &hooks : nullptr);
        if (machine_->power_control == 1) {
            std::string error;
            if (!resetMachineLocked(true, &error)) {
                machine_->status = vm::VMStatus::TRAPPED;
                result.status = vm::VMStatus::TRAPPED;
                result.description = "guest reboot failed: " + error;
                return result;
            }
            result.status = machine_->status;
            result.final_pc = machine_->pc;
            result.description = "guest reboot completed after " +
                                 std::to_string(result.steps) + " steps";
        }
        return result;
    }

    [[nodiscard]] vm::VMStatus stepOnce() {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!machine_) return vm::VMStatus::HALTED;
        vm::VMExecutionRecord record;
        vm::VMStatus status = vm::step(
            *machine_,
            config_.record_syscall_trace ? &record : nullptr);
        if (config_.record_syscall_trace) recordSyscall(record);
        if (machine_->power_control == 1) {
            std::string error;
            if (!resetMachineLocked(true, &error)) {
                machine_->status = vm::VMStatus::TRAPPED;
                return vm::VMStatus::TRAPPED;
            }
            status = machine_->status;
        }
        return status;
    }

    void pushKeyboardInput(long long word, const std::string& source = "host.api") {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!machine_) return;
        machine_->enqueueConsoleInput(word);
        machine_->resumeFromEvent();
        input_journal_.push_back(TosInputJournalEvent{
            next_input_sequence_++,
            static_cast<std::uint64_t>(std::max<long long>(
                0, machine_->cycle_count)),
            TosInputEventKind::KeyboardWord,
            word,
            0,
            0,
            {},
            detail::inputProvenanceOrDefault(source),
            "keyboard"});
    }

    void pushTextInput(const std::string& text, const std::string& source = "host.api") {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!machine_) return;
        machine_->enqueueConsoleAscii(text);
        machine_->resumeFromEvent();
        input_journal_.push_back(TosInputJournalEvent{
            next_input_sequence_++,
            static_cast<std::uint64_t>(std::max<long long>(
                0, machine_->cycle_count)),
            TosInputEventKind::Text,
            0,
            0,
            0,
            text,
            detail::inputProvenanceOrDefault(source),
            "text"});
    }

    void updateMouseState(long long x,
                          long long y,
                          long long buttons,
                          const std::string& source = "host.api") {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!machine_) return;
        machine_->mouse_x = x;
        machine_->mouse_y = y;
        machine_->mouse_btn = buttons;
        machine_->resumeFromEvent();
        input_journal_.push_back(TosInputJournalEvent{
            next_input_sequence_++,
            static_cast<std::uint64_t>(std::max<long long>(
                0, machine_->cycle_count)),
            TosInputEventKind::Mouse,
            x,
            y,
            buttons,
            {},
            detail::inputProvenanceOrDefault(source),
            "mouse"});
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
        out.boot_generation = boot_generation_;
        out.guest_reboot_count = guest_reboot_count_;
        return out;
    }

    // Capture the complete machine at an instruction boundary.  The copy is
    // independent of the live runtime and can be restored after a crash or
    // used as the starting point for deterministic input replay.
    [[nodiscard]] bool captureCheckpoint(std::string* error = nullptr) {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!machine_) {
            detail::setError(error, "runtime has no loaded VM image");
            return false;
        }
        auto checkpoint = std::make_shared<TosRuntimeCheckpoint>();
        checkpoint->sequence = next_checkpoint_sequence_++;
        checkpoint->input_event_count = input_journal_.size();
        checkpoint->vm = vm::captureCheckpoint(*machine_);
        checkpoint_ = std::move(checkpoint);
        return true;
    }

    [[nodiscard]] bool hasCheckpoint() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return checkpoint_ != nullptr;
    }

    [[nodiscard]] TosRuntimeCheckpoint checkpointMetadata() const {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!checkpoint_) return TosRuntimeCheckpoint{};
        TosRuntimeCheckpoint metadata;
        metadata.sequence = checkpoint_->sequence;
        metadata.input_event_count = checkpoint_->input_event_count;
        metadata.vm.cycle = checkpoint_->vm.cycle;
        metadata.vm.pc = checkpoint_->vm.pc;
        return metadata;
    }

    [[nodiscard]] bool restoreCheckpoint(std::string* error = nullptr) {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!checkpoint_) {
            detail::setError(error, "runtime has no checkpoint");
            return false;
        }
        if (!machine_ ||
            !vm::restoreCheckpoint(*machine_, checkpoint_->vm)) {
            detail::setError(error, "checkpoint state failed architectural validation");
            return false;
        }
        syscall_trace_.clear();
        syscall_trace_sequence_ = 0;
        return true;
    }

    // Restore the last checkpoint and re-execute while injecting host input at
    // the cycle at which it was originally observed.  Events already present
    // in the checkpoint's input queue are part of the copied VM state and are
    // therefore not duplicated.
    [[nodiscard]] vm::RunResult replayFromCheckpoint(
        int max_steps,
        std::string* error = nullptr) {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!checkpoint_) {
            detail::setError(error, "runtime has no checkpoint");
            return {vm::VMStatus::HALTED, 0, 0,
                    isa::TrapCode::TRAP_ILLEGAL_OP,
                    "runtime has no checkpoint"};
        }
        if (!machine_ ||
            !vm::restoreCheckpoint(*machine_, checkpoint_->vm)) {
            detail::setError(error, "checkpoint state failed architectural validation");
            return {vm::VMStatus::TRAPPED, 0, machine_ ? machine_->pc : 0,
                    isa::TrapCode::TRAP_ILLEGAL_OP,
                    "checkpoint state failed architectural validation"};
        }
        syscall_trace_.clear();
        syscall_trace_sequence_ = 0;

        std::size_t next_event = checkpoint_->input_event_count;
        int steps = 0;
        while (machine_->isRunning() && machine_->power_control == 0 &&
               (max_steps < 0 || steps < max_steps)) {
            while (next_event < input_journal_.size() &&
                   input_journal_[next_event].cycle <=
                       static_cast<std::uint64_t>(std::max<long long>(
                           0, machine_->cycle_count))) {
                applyInputEventLocked(input_journal_[next_event]);
                ++next_event;
            }
            vm::VMExecutionRecord record;
            const vm::VMStatus status = vm::step(
                *machine_, config_.record_syscall_trace ? &record : nullptr);
            if (config_.record_syscall_trace) recordSyscall(record);
            ++steps;
            if (status != vm::VMStatus::RUNNING) break;
        }
        vm::RunResult result;
        result.status = machine_->status;
        result.steps = steps;
        result.final_pc = machine_->pc;
        result.description = "checkpoint replay";
        return result;
    }

    // Persist a complete architectural checkpoint and its input journal.
    // The resulting directory is self-contained and can be opened by a fresh
    // TosRuntime instance (or another process) without access to the original
    // mutable disk backing file.
    [[nodiscard]] bool exportCheckpointBundle(
        const std::string& directory,
        std::string* error = nullptr) const {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!machine_ || !checkpoint_) {
            detail::setError(error, "runtime has no checkpoint to export");
            return false;
        }
        std::error_code ec;
        const std::filesystem::path base(directory);
        std::filesystem::create_directories(base, ec);
        if (ec) {
            detail::setError(error, "failed to create checkpoint bundle: " + ec.message());
            return false;
        }
        if (!writeBootImageFile((base / "boot.tboot").string(), image_, error) ||
            !detail::writeCheckpointState(base / "vm_state.bin",
                                           checkpoint_->vm.state, error) ||
            !checkpoint_->vm.state.block_device.writeSnapshotFile(
                (base / "disk.tdisk").string()) ||
            !detail::writeCheckpointJournal(base / "input_journal.bin",
                                             input_journal_, error)) {
            if (error && error->empty()) {
                detail::setError(error, "failed to write checkpoint bundle payload");
            }
            return false;
        }
        if (!detail::writeInputJournalJson(base / "input_journal.jsonl",
                                            input_journal_, error)) {
            return false;
        }
        {
            std::ofstream trace(base / "syscall_trace.jsonl", std::ios::trunc);
            if (!trace.good()) {
                detail::setError(error, "failed to write checkpoint syscall trace");
                return false;
            }
            if (syscall_trace_.empty()) {
                trace << "{\"schema\":\"trit.syscall_trace.v1\","
                      << "\"event\":\"trace_empty\",\"cycles\":"
                      << machine_->cycle_count << "}\n";
            } else {
                for (const auto& event : syscall_trace_) {
                    trace << "{\"schema\":\"trit.syscall_trace.v1\","
                          << "\"sequence\":" << event.sequence
                          << ",\"pc\":" << event.record.pc
                          << ",\"physical_pc\":" << event.record.physical_pc
                          << ",\"syscall_id\":" << event.record.syscall_id
                          << ",\"process_id\":" << event.record.process_id
                          << ",\"before_privilege\":"
                          << static_cast<int>(event.record.before_privilege)
                          << ",\"after_privilege\":"
                          << static_cast<int>(event.record.after_privilege)
                          << ",\"args\":[" << event.record.syscall_arg0 << ","
                          << event.record.syscall_arg1 << "," << event.record.syscall_arg2
                          << "," << event.record.syscall_arg3 << "],\"results\":["
                          << event.record.syscall_result0 << "," << event.record.syscall_result1
                          << "," << event.record.syscall_result2 << "],\"before_status\":"
                          << static_cast<int>(event.record.before_status)
                          << ",\"after_status\":"
                          << static_cast<int>(event.record.after_status)
                          << ",\"trap\":" << (event.record.trap_observed ? 1 : 0)
                          << ",\"trap_code\":"
                          << static_cast<int>(event.record.trap_code)
                          << ",\"trap_cause\":" << event.record.trap_cause
                          << ",\"cycle_before\":" << event.record.cycle_before
                          << ",\"cycle_after\":" << event.record.cycle_after << "}\n";
                }
            }
            if (!trace.good()) {
                detail::setError(error, "failed to write checkpoint syscall trace");
                return false;
            }
        }
        {
            detail::CheckpointWriter metadata(base / "checkpoint.bin");
            if (!metadata.ok) {
                detail::setError(error, "failed to open checkpoint metadata");
                return false;
            }
            metadata.pod(detail::kCheckpointStateMagic);
            metadata.pod(detail::kCheckpointStateVersion);
            metadata.pod(checkpoint_->sequence);
            metadata.pod(static_cast<std::uint64_t>(checkpoint_->input_event_count));
            metadata.pod(checkpoint_->vm.cycle);
            metadata.pod(static_cast<std::int32_t>(checkpoint_->vm.pc));
            metadata.pod(boot_generation_);
            metadata.pod(guest_reboot_count_);
            metadata.finish();
            if (!metadata.ok) {
                detail::setError(error, "failed to write checkpoint metadata");
                return false;
            }
        }
        {
            std::ofstream metadata(base / "checkpoint.json", std::ios::trunc);
            if (!metadata.good()) {
                detail::setError(error, "failed to write checkpoint.json");
                return false;
            }
            metadata << "{\n"
                      << "  \"schema\":\"trit.runtime_checkpoint.v2\",\n"
                      << "  \"available\":true,\n"
                      << "  \"sequence\":" << checkpoint_->sequence << ",\n"
                      << "  \"input_event_count\":" << checkpoint_->input_event_count << ",\n"
                      << "  \"cycle\":" << checkpoint_->vm.cycle << ",\n"
                      << "  \"pc\":" << checkpoint_->vm.pc << ",\n"
                      << "  \"state_file\":\"vm_state.bin\",\n"
                      << "  \"disk_file\":\"disk.tdisk\",\n"
                      << "  \"journal_file\":\"input_journal.bin\",\n"
                      << "  \"boot_image\":\"boot.tboot\"\n"
                      << "}\n";
            if (!metadata.good()) {
                detail::setError(error, "failed to write checkpoint.json");
                return false;
            }
        }
        return true;
    }

    // Load a checkpoint bundle produced by exportCheckpointBundle.  This is
    // the file-backed/separate-process restore path: no live VM or original
    // disk backing path is consulted.  After restore, replayFromCheckpoint()
    // can continue with journaled events that occurred after the checkpoint.
    [[nodiscard]] bool restoreCheckpointBundle(
        const std::string& directory,
        std::string* error = nullptr) {
        shutdownWorkerOnly();
        const std::filesystem::path base(directory);
        TosBootImage image;
        if (!readBootImageFile((base / "boot.tboot").string(), image, error)) return false;
        std::vector<TosInputJournalEvent> journal;
        if (!detail::readCheckpointJournal(base / "input_journal.bin", journal, error)) return false;
        detail::CheckpointReader metadata(base / "checkpoint.bin");
        std::uint64_t magic = 0, sequence = 0, input_count = 0,
                      cycle = 0, boot_generation = 0, reboot_count = 0;
        std::uint32_t version = 0;
        std::int32_t pc = 0;
        if (!metadata.ok || !metadata.pod(magic) || !metadata.pod(version) ||
            magic != detail::kCheckpointStateMagic || version != detail::kCheckpointStateVersion ||
            !metadata.pod(sequence) || !metadata.pod(input_count) || !metadata.pod(cycle) ||
            !metadata.pod(pc) || !metadata.pod(boot_generation) || !metadata.pod(reboot_count) ||
            input_count > journal.size()) {
            detail::setError(error, "checkpoint metadata is invalid");
            return false;
        }
        std::unique_ptr<vm::VMState> machine;
        if (!detail::readCheckpointState(base / "vm_state.bin", machine, error) || !machine) return false;
        if (machine->pc != pc || machine->cycle_count < 0 ||
            static_cast<std::uint64_t>(machine->cycle_count) != cycle) {
            detail::setError(error, "checkpoint state does not match metadata");
            return false;
        }
        if (!machine->attachBlockBackingFile((base / "disk.tdisk").string())) {
            detail::setError(error, "checkpoint disk snapshot failed validation");
            return false;
        }
        std::lock_guard<std::mutex> lock(mutex_);
        image_ = std::move(image);
        config_.boot_image_path = (base / "boot.tboot").string();
        config_.disk_path = (base / "disk.tdisk").string();
        machine_ = std::move(machine);
        paused_ = config_.start_paused;
        boot_generation_ = boot_generation;
        guest_reboot_count_ = reboot_count;
        input_journal_ = std::move(journal);
        next_input_sequence_ = input_journal_.size();
        syscall_trace_.clear();
        syscall_trace_sequence_ = 0;
        auto checkpoint = std::make_shared<TosRuntimeCheckpoint>();
        checkpoint->sequence = sequence;
        checkpoint->input_event_count = static_cast<std::size_t>(input_count);
        checkpoint->vm = vm::captureCheckpoint(*machine_);
        checkpoint_ = std::move(checkpoint);
        next_checkpoint_sequence_ = sequence + 1;
        return true;
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
        // Every diagnostic capture gets a self-contained, restorable
        // instruction-boundary snapshot.  An explicitly captured checkpoint
        // retains its sequence/input boundary; otherwise capture the live VM
        // locally without mutating runtime replay state.
        std::shared_ptr<TosRuntimeCheckpoint> diagnostics_checkpoint = checkpoint_;
        if (!diagnostics_checkpoint) {
            diagnostics_checkpoint = std::make_shared<TosRuntimeCheckpoint>();
            diagnostics_checkpoint->sequence = 0;
            diagnostics_checkpoint->input_event_count = input_journal_.size();
            diagnostics_checkpoint->vm = vm::captureCheckpoint(*machine_);
        }
        const std::filesystem::path checkpoint_base = base / "checkpoint";
        std::filesystem::create_directories(checkpoint_base, ec);
        if (ec) {
            detail::setError(error,
                             "failed to create diagnostics checkpoint: " + ec.message());
            return false;
        }
        if (!writeBootImageFile((checkpoint_base / "boot.tboot").string(),
                                image_, error) ||
            !detail::writeCheckpointState(checkpoint_base / "vm_state.bin",
                                           diagnostics_checkpoint->vm.state,
                                           error) ||
            !diagnostics_checkpoint->vm.state.block_device.writeSnapshotFile(
                (checkpoint_base / "disk.tdisk").string()) ||
            !detail::writeCheckpointJournal(checkpoint_base / "input_journal.bin",
                                             input_journal_, error) ||
            !detail::writeInputJournalJson(checkpoint_base / "input_journal.jsonl",
                                            input_journal_, error) ||
            !writeSyscallTraceJsonLocked(checkpoint_base / "syscall_trace.jsonl",
                                         error)) {
            if (error && error->empty()) {
                detail::setError(error, "failed to write diagnostics checkpoint payload");
            }
            return false;
        }
        {
            detail::CheckpointWriter metadata(checkpoint_base / "checkpoint.bin");
            if (!metadata.ok) {
                detail::setError(error, "failed to open diagnostics checkpoint metadata");
                return false;
            }
            metadata.pod(detail::kCheckpointStateMagic);
            metadata.pod(detail::kCheckpointStateVersion);
            metadata.pod(diagnostics_checkpoint->sequence);
            metadata.pod(static_cast<std::uint64_t>(
                diagnostics_checkpoint->input_event_count));
            metadata.pod(diagnostics_checkpoint->vm.cycle);
            metadata.pod(static_cast<std::int32_t>(diagnostics_checkpoint->vm.pc));
            metadata.pod(boot_generation_);
            metadata.pod(guest_reboot_count_);
            metadata.finish();
            if (!metadata.ok) {
                detail::setError(error, "failed to write diagnostics checkpoint metadata");
                return false;
            }
        }
        {
            std::ofstream metadata(checkpoint_base / "checkpoint.json", std::ios::trunc);
            if (!metadata.good()) {
                detail::setError(error, "failed to write diagnostics checkpoint.json");
                return false;
            }
            metadata << "{\n"
                      << "  \"schema\":\"trit.runtime_checkpoint.v2\",\n"
                      << "  \"compatibility_schema\":\"trit.runtime_checkpoint.v1\",\n"
                      << "  \"available\":true,\n"
                      << "  \"sequence\":" << diagnostics_checkpoint->sequence << ",\n"
                      << "  \"input_event_count\":"
                      << diagnostics_checkpoint->input_event_count << ",\n"
                      << "  \"cycle\":" << diagnostics_checkpoint->vm.cycle << ",\n"
                      << "  \"pc\":" << diagnostics_checkpoint->vm.pc << ",\n"
                      << "  \"state_file\":\"vm_state.bin\",\n"
                      << "  \"disk_file\":\"disk.tdisk\",\n"
                      << "  \"journal_file\":\"input_journal.bin\",\n"
                      << "  \"boot_image\":\"boot.tboot\",\n"
                      << "  \"trace_file\":\"syscall_trace.jsonl\"\n"
                      << "}\n";
            if (!metadata.good()) {
                detail::setError(error, "failed to write diagnostics checkpoint.json");
                return false;
            }
        }
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
            out << "epc=" << machine_->epc << "\n";
            out << "scratch=" << machine_->scratch << "\n";
            out << "sp=" << vm::ops::toLong(machine_->regfile.read(isa::R26_SP)) << "\n";
            out << "r1=" << vm::ops::toLong(machine_->regfile.read(1)) << "\n";
            out << "r13=" << vm::ops::toLong(machine_->regfile.read(13)) << "\n";
            out << "r20=" << vm::ops::toLong(machine_->regfile.read(20)) << "\n";
            out << "r25=" << vm::ops::toLong(machine_->regfile.read(isa::R25_LR)) << "\n";
            out << "interrupt_enable=" << (machine_->interrupt_enable ? 1 : 0) << "\n";
            out << "previous_interrupt_enable="
                << (machine_->previous_interrupt_enable ? 1 : 0) << "\n";
            out << "timer_reload=" << machine_->timer_reload << "\n";
            out << "timer_counter=" << machine_->timer_counter << "\n";
            out << "timer_enable=" << (machine_->timer_enable ? 1 : 0) << "\n";
            out << "timer_pending=" << (machine_->timer_pending ? 1 : 0) << "\n";
            out << "console_input_available=" << machine_->consoleInputAvailable() << "\n";
            out << "console_input_front=" << machine_->peekConsoleInput() << "\n";
            out << "gpu_mode=" << machine_->gpu_mode << "\n";
            out << "trap=" << vm::ops::toLong(machine_->trap_reg) << "\n";
            out << "cause=" << machine_->cause << "\n";
            out << "disk_path=" << config_.disk_path << "\n";
            out << "boot_generation=" << boot_generation_ << "\n";
            out << "guest_reboot_count=" << guest_reboot_count_ << "\n";
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
            out << "    \"boot_generation\": " << boot_generation_ << ",\n";
            out << "    \"guest_reboot_count\": " << guest_reboot_count_ << ",\n";
            out << "    \"privilege\": ";
            detail::writeJsonString(out, privilegeName(machine_->privilege));
            out << ",\n";
            out << "    \"console_input_available\": " << machine_->consoleInputAvailable() << ",\n";
            out << "    \"console_input_front\": " << machine_->peekConsoleInput() << ",\n";
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
            out << "  ],\n";
            out << "  \"checkpoint\": {\n";
            out << "    \"schema\": \"trit.runtime_checkpoint.v2\",\n";
            out << "    \"compatibility_schema\": \"trit.runtime_checkpoint.v1\",\n";
            out << "    \"available\": true,\n";
            out << "    \"metadata_file\": \"checkpoint/checkpoint.json\",\n";
            out << "    \"state_file\": \"checkpoint/vm_state.bin\",\n";
            out << "    \"disk_file\": \"checkpoint/disk.tdisk\",\n";
            out << "    \"journal_file\": \"checkpoint/input_journal.bin\",\n";
            out << "    \"journal_jsonl\": \"checkpoint/input_journal.jsonl\",\n";
            out << "    \"trace_file\": \"checkpoint/syscall_trace.jsonl\",\n";
            out << "    \"boot_image\": \"checkpoint/boot.tboot\"\n";
            out << "  }\n";
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
                const long long quota_id =
                    detail::dmemWord(*machine_, row + detail::kProcQuota);
                const int quota_row =
                    detail::kQuotaBase + static_cast<int>(quota_id) * detail::kQuotaRowWords;
                const long long quota_remaining =
                    detail::dmemWord(*machine_, quota_row + detail::kQuotaLimit) -
                    detail::dmemWord(*machine_, quota_row + detail::kQuotaUsed);
                out << "    {\"slot\": " << slot
                    << ", \"pid\": " << detail::dmemWord(*machine_, row + detail::kProcPid)
                    << ", \"namespace\": " << detail::dmemWord(*machine_, row + detail::kProcNamespace)
                    << ", \"state\": " << state
                    << ", \"state_name\": ";
                detail::writeJsonString(out, detail::processStateName(state));
                out << ", \"priority\": " << detail::dmemWord(*machine_, row + detail::kProcPriority)
                    << ", \"quota\": " << quota_id
                    << ", \"quota_remaining\": " << quota_remaining
                    << ", \"context\": " << context
                    << ", \"context_epc\": " << detail::dmemWord(*machine_, static_cast<int>(context) + detail::kTaskContextEpc)
                    << ", \"context_status\": " << detail::dmemWord(*machine_, static_cast<int>(context) + detail::kTaskContextStatus)
                    << ", \"context_imem_ptbr\": " << detail::dmemWord(*machine_, static_cast<int>(context) + detail::kTaskContextImemPtbr)
                    << ", \"context_imem_pages\": " << detail::dmemWord(*machine_, static_cast<int>(context) + detail::kTaskContextImemPages)
                    << ", \"context_dmem_ptbr\": " << detail::dmemWord(*machine_, static_cast<int>(context) + detail::kTaskContextDmemPtbr)
                    << ", \"context_dmem_pages\": " << detail::dmemWord(*machine_, static_cast<int>(context) + detail::kTaskContextDmemPages)
                    << ", \"context_r1\": " << detail::dmemWord(*machine_, static_cast<int>(context) + detail::kTaskContextRegBase)
                    << ", \"context_r13\": " << detail::dmemWord(*machine_, static_cast<int>(context) + detail::kTaskContextRegBase + 12)
                    << ", \"context_r20\": " << detail::dmemWord(*machine_, static_cast<int>(context) + detail::kTaskContextRegBase + 19)
                    << ", \"context_r25\": " << detail::dmemWord(*machine_, static_cast<int>(context) + detail::kTaskContextRegBase + 24)
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
            std::ofstream out(base / "app_registry.json", std::ios::trunc);
            if (!out.good()) {
                detail::setError(error, "failed to write app_registry.json");
                return false;
            }
            out << "{\n";
            out << "  \"format_version\": 1,\n";
            out << "  \"entries\": [\n";
            bool first = true;
            for (int slot = 0; slot < detail::kAppMax; ++slot) {
                const int row = detail::kAppRegistryBase + slot * detail::kAppRowWords;
                const long long version =
                    detail::dmemWord(*machine_, row + detail::kAppVersion);
                if (version <= 0) continue;
                if (!first) out << ",\n";
                first = false;
                out << "    {\"slot\": " << slot
                    << ", \"namespace\": "
                    << detail::dmemWord(*machine_, row + detail::kAppNamespace)
                    << ", \"path_hash\": "
                    << detail::dmemWord(*machine_, row + detail::kAppPathHash)
                    << ", \"inode\": "
                    << detail::dmemWord(*machine_, row + detail::kAppInode)
                    << ", \"caps\": "
                    << detail::dmemWord(*machine_, row + detail::kAppCaps)
                    << ", \"quota\": "
                    << detail::dmemWord(*machine_, row + detail::kAppQuota)
                    << ", \"trusted\": "
                    << detail::dmemWord(*machine_, row + detail::kAppTrusted)
                    << ", \"version\": " << version
                    << ", \"launch_count\": "
                    << detail::dmemWord(*machine_, row + detail::kAppLaunchCount)
                    << ", \"max_image\": "
                    << detail::dmemWord(*machine_, row + detail::kAppMaxImage)
                    << ", \"flags\": "
                    << detail::dmemWord(*machine_, row + detail::kAppFlags)
                    << "}";
            }
            out << "\n  ]\n";
            out << "}\n";
        }
        {
            std::ofstream out(base / "syscall_trace.jsonl", std::ios::trunc);
            if (!out.good()) {
                detail::setError(error, "failed to write syscall_trace.jsonl");
                return false;
            }
            if (!config_.record_syscall_trace) {
                out << "{\"schema\":\"trit.syscall_trace.v1\","
                    << "\"event\":\"trace_disabled\","
                    << "\"cycles\":" << machine_->cycle_count << "}\n";
            } else if (syscall_trace_.empty()) {
                out << "{\"schema\":\"trit.syscall_trace.v1\","
                    << "\"event\":\"trace_empty\","
                    << "\"cycles\":" << machine_->cycle_count << "}\n";
            } else {
                for (const auto& event : syscall_trace_) {
                    out << "{\"schema\":\"trit.syscall_trace.v1\","
                        << "\"sequence\":" << event.sequence << ","
                        << "\"pc\":" << event.record.pc << ","
                        << "\"physical_pc\":" << event.record.physical_pc << ","
                        << "\"syscall_id\":" << event.record.syscall_id << ","
                        << "\"process_id\":" << event.record.process_id << ","
                        << "\"before_privilege\":"
                        << static_cast<int>(event.record.before_privilege) << ","
                        << "\"after_privilege\":"
                        << static_cast<int>(event.record.after_privilege) << ","
                        << "\"args\":[" << event.record.syscall_arg0 << ","
                        << event.record.syscall_arg1 << ","
                        << event.record.syscall_arg2 << ","
                        << event.record.syscall_arg3 << "],"
                        << "\"results\":[" << event.record.syscall_result0 << ","
                        << event.record.syscall_result1 << ","
                        << event.record.syscall_result2 << "],"
                        << "\"before_status\":"
                        << static_cast<int>(event.record.before_status) << ","
                        << "\"after_status\":"
                        << static_cast<int>(event.record.after_status) << ","
                        << "\"trap\":" << (event.record.trap_observed ? 1 : 0) << ","
                        << "\"trap_code\":"
                        << static_cast<int>(event.record.trap_code) << ","
                        << "\"trap_cause\":" << event.record.trap_cause << ","
                        << "\"cycle_before\":" << event.record.cycle_before << ","
                        << "\"cycle_after\":" << event.record.cycle_after << "}\n";
                }
            }
        }
        if (!detail::writeInputJournalJson(base / "input_journal.jsonl",
                                            input_journal_, error)) {
            return false;
        }
        {
            std::ofstream out(base / "checkpoint.json", std::ios::trunc);
            if (!out.good()) {
                detail::setError(error, "failed to write checkpoint.json");
                return false;
            }
            out << "{\n"
                << "  \"schema\":\"trit.runtime_checkpoint.v2\",\n"
                << "  \"compatibility_schema\":\"trit.runtime_checkpoint.v1\",\n"
                << "  \"available\":true,\n"
                << "  \"sequence\":" << diagnostics_checkpoint->sequence << ",\n"
                << "  \"input_event_count\":"
                << diagnostics_checkpoint->input_event_count << ",\n"
                << "  \"cycle\":" << diagnostics_checkpoint->vm.cycle << ",\n"
                << "  \"pc\":" << diagnostics_checkpoint->vm.pc << ",\n"
                << "  \"state_file\":\"checkpoint/vm_state.bin\",\n"
                << "  \"disk_file\":\"checkpoint/disk.tdisk\",\n"
                << "  \"journal_file\":\"checkpoint/input_journal.bin\",\n"
                << "  \"boot_image\":\"checkpoint/boot.tboot\",\n"
                << "  \"trace_file\":\"checkpoint/syscall_trace.jsonl\"\n"
                << "}\n";
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
            out << "epc=" << machine_->epc << "\n";
            out << "cause=" << machine_->cause << "\n";
            out << "page_fault_addr=" << machine_->page_fault_addr << "\n";
            out << "page_fault_access=" << machine_->page_fault_access << "\n";
            out << "boot_generation=" << boot_generation_ << "\n";
            out << "guest_reboot_count=" << guest_reboot_count_ << "\n";
            out << "current_pid=" << detail::dmemWord(*machine_, detail::kCurrentPidAddr) << "\n";
            out << "syscall_status=" << detail::dmemWord(*machine_, detail::kSysStatusAddr) << "\n";
            out << "syscall_payload=" << detail::dmemWord(*machine_, detail::kSysPayloadAddr) << "\n";
            out << "syscall_detail=" << detail::dmemWord(*machine_, detail::kSysDetailAddr) << "\n";
            long long crashed_slot = -1;
            long long crashed_pid = 0;
            long long crashed_parent_pid = 0;
            long long crashed_exit_status = 0;
            for (int slot = 0; slot < detail::kProcessMax; ++slot) {
                const int row = detail::kProcessBase + slot * detail::kProcessRowWords;
                if (detail::dmemWord(*machine_, row + detail::kProcState) == 9) {
                    crashed_slot = slot;
                    crashed_pid = detail::dmemWord(*machine_, row + detail::kProcPid);
                    crashed_parent_pid =
                        detail::dmemWord(*machine_, detail::kProcParentPidBase + slot);
                    crashed_exit_status =
                        detail::dmemWord(*machine_, detail::kProcExitStatusBase + slot);
                    break;
                }
            }
            out << "crashed_slot=" << crashed_slot << "\n";
            out << "crashed_pid=" << crashed_pid << "\n";
            out << "crashed_parent_pid=" << crashed_parent_pid << "\n";
            out << "crashed_exit_status=" << crashed_exit_status << "\n";
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
            if (!detail::writeFramebufferPng(base / "framebuffer_snapshot.png",
                                             framebuffer,
                                             error)) {
                return false;
            }
        }
        return true;
    }

private:
    struct SyscallTraceEvent {
        std::uint64_t sequence = 0;
        vm::VMExecutionRecord record;
    };

    TosRuntimeConfig config_;
    TosBootImage image_;
    std::unique_ptr<vm::VMState> machine_;
    mutable std::mutex mutex_;
    std::thread worker_;
    std::atomic<bool> worker_running_{false};
    std::atomic<bool> paused_{false};
    std::uint64_t boot_generation_ = 0;
    std::uint64_t guest_reboot_count_ = 0;
    std::vector<SyscallTraceEvent> syscall_trace_;
    std::uint64_t syscall_trace_sequence_ = 0;
    std::shared_ptr<TosRuntimeCheckpoint> checkpoint_;
    std::uint64_t next_checkpoint_sequence_ = 0;
    std::vector<TosInputJournalEvent> input_journal_;
    std::uint64_t next_input_sequence_ = 0;

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

    void applyInputEventLocked(const TosInputJournalEvent& event) {
        if (!machine_) return;
        switch (event.kind) {
            case TosInputEventKind::KeyboardWord:
                machine_->enqueueConsoleInput(event.value0);
                machine_->resumeFromEvent();
                break;
            case TosInputEventKind::Text:
                machine_->enqueueConsoleAscii(event.text);
                machine_->resumeFromEvent();
                break;
            case TosInputEventKind::Mouse:
                machine_->mouse_x = event.value0;
                machine_->mouse_y = event.value1;
                machine_->mouse_btn = event.value2;
                machine_->resumeFromEvent();
                break;
        }
    }

    bool writeSyscallTraceJsonLocked(const std::filesystem::path& path,
                                     std::string* error) const {
        std::ofstream trace(path, std::ios::trunc);
        if (!trace.good()) {
            detail::setError(error, "failed to write syscall trace: " + path.string());
            return false;
        }
        if (syscall_trace_.empty()) {
            trace << "{\"schema\":\"trit.syscall_trace.v1\","
                  << "\"event\":\"trace_empty\",\"cycles\":"
                  << machine_->cycle_count << "}\n";
        } else {
            for (const auto& event : syscall_trace_) {
                trace << "{\"schema\":\"trit.syscall_trace.v1\","
                      << "\"sequence\":" << event.sequence
                      << ",\"pc\":" << event.record.pc
                      << ",\"physical_pc\":" << event.record.physical_pc
                      << ",\"syscall_id\":" << event.record.syscall_id
                      << ",\"process_id\":" << event.record.process_id
                      << ",\"before_privilege\":"
                      << static_cast<int>(event.record.before_privilege)
                      << ",\"after_privilege\":"
                      << static_cast<int>(event.record.after_privilege)
                      << ",\"args\":[" << event.record.syscall_arg0 << ","
                      << event.record.syscall_arg1 << "," << event.record.syscall_arg2
                      << "," << event.record.syscall_arg3 << "],\"results\":["
                      << event.record.syscall_result0 << "," << event.record.syscall_result1
                      << "," << event.record.syscall_result2 << "],\"before_status\":"
                      << static_cast<int>(event.record.before_status)
                      << ",\"after_status\":"
                      << static_cast<int>(event.record.after_status)
                      << ",\"trap\":" << (event.record.trap_observed ? 1 : 0)
                      << ",\"trap_code\":"
                      << static_cast<int>(event.record.trap_code)
                      << ",\"trap_cause\":" << event.record.trap_cause
                      << ",\"cycle_before\":" << event.record.cycle_before
                      << ",\"cycle_after\":" << event.record.cycle_after << "}\n";
            }
        }
        if (!trace.good()) {
            detail::setError(error, "failed to write syscall trace: " + path.string());
            return false;
        }
        return true;
    }

    bool resetMachineLocked(bool guest_reboot, std::string* error) {
        if (!machine_) {
            detail::setError(error, "runtime has no loaded VM image");
            return false;
        }
        if (!machine_->compactBlockBackingFile(false)) {
            detail::setError(error, "failed to flush mutable disk before guest reset");
            return false;
        }
        vm::ProductionProfile profile = profileForManifest(image_.manifest);
        auto machine = std::make_unique<vm::VMState>(profile);
        if (!loadBootImageIntoVm(*machine, image_, config_.disk_path, error)) return false;
        machine_ = std::move(machine);
        paused_ = config_.start_paused;
        syscall_trace_.clear();
        syscall_trace_sequence_ = 0;
        checkpoint_.reset();
        next_checkpoint_sequence_ = 0;
        input_journal_.clear();
        next_input_sequence_ = 0;
        ++boot_generation_;
        if (guest_reboot) ++guest_reboot_count_;
        return true;
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
                            vm::VMExecutionRecord record;
                            const vm::VMStatus status = vm::step(
                                *machine_,
                                config_.record_syscall_trace ? &record : nullptr);
                            if (config_.record_syscall_trace) recordSyscall(record);
                            ++chunk_steps;
                            if (machine_->power_control == 1) {
                                std::string error;
                                if (!resetMachineLocked(true, &error)) {
                                    machine_->status = vm::VMStatus::TRAPPED;
                                }
                                stopped_or_idle = true;
                                break;
                            }
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

    void recordSyscall(const vm::VMExecutionRecord& record) {
        if (!record.attempted || !record.has_instruction ||
            record.instruction.opcode != isa::Opcode::SYSCALL) {
            return;
        }
        syscall_trace_.push_back(SyscallTraceEvent{
            syscall_trace_sequence_++, record});
    }
};

} // namespace host
} // namespace sandbox

#endif // TERNARY_HOST_RUNTIME_H
