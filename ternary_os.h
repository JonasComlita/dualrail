// =============================================================================
// ternary_os.h - Trit OS xv6-class platform boot substrate
// =============================================================================
//
// This header defines the first concrete Trit OS platform services beside the
// frozen Phase 6/7 VM, ABI, and compiler contracts. It intentionally keeps the
// existing syscall ids, executable header v1, task context layout, and PTE v1
// compatible while adding device-tree, block-device, tiny filesystem, heap, and
// process-lifecycle models that the assembly kernel can grow into.

#pragma once
#ifndef TERNARY_OS_H
#define TERNARY_OS_H

#include "ternary_compiler.h"
#include "ternary_vm.h"

#include <atomic>
#include <algorithm>
#include <array>
#include <cstdint>
#include <deque>
#include <fstream>
#include <map>
#include <set>
#include <sstream>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace sandbox {
namespace os {

// =============================================================================
// ABI/status constants
// =============================================================================

static constexpr int T1_ERROR = -1;
static constexpr int T1_PENDING = 0;
static constexpr int T1_SUCCESS = 1;

static constexpr int SYSCALL_OPEN = vm::SYSCALL_OPEN;
static constexpr int SYSCALL_CLOSE = vm::SYSCALL_CLOSE;
static constexpr int SYSCALL_READ = vm::SYSCALL_READ;
static constexpr int SYSCALL_WRITE = vm::SYSCALL_WRITE;
static constexpr int SYSCALL_STAT = vm::SYSCALL_STAT;
static constexpr int SYSCALL_READDIR = vm::SYSCALL_READDIR;
static constexpr int SYSCALL_BRK = vm::SYSCALL_BRK;
static constexpr int SYSCALL_SBRK = vm::SYSCALL_SBRK;
static constexpr int SYSCALL_FORK = vm::SYSCALL_FORK;
static constexpr int SYSCALL_EXEC = vm::SYSCALL_EXEC;
static constexpr int SYSCALL_FSYNC = vm::SYSCALL_FSYNC;
static constexpr int SYSCALL_KILL = vm::SYSCALL_KILL;
static constexpr int SYSCALL_SUSPEND = vm::SYSCALL_SUSPEND;
static constexpr int SYSCALL_RESUME = vm::SYSCALL_RESUME;
static constexpr int SYSCALL_GETPROC = vm::SYSCALL_GETPROC;
static constexpr int SYSCALL_FUTEX_WAIT = vm::SYSCALL_FUTEX_WAIT;
static constexpr int SYSCALL_FUTEX_WAKE = vm::SYSCALL_FUTEX_WAKE;
static constexpr int SYSCALL_IPC_RECV_BLOCKING = vm::SYSCALL_IPC_RECV_BLOCKING;
static constexpr int SYSCALL_WAIT_EVENT = vm::SYSCALL_WAIT_EVENT;
static constexpr int SYSCALL_SLEEP_MS = vm::SYSCALL_SLEEP_MS;
static constexpr int SYSCALL_APP_SPAWN = vm::SYSCALL_APP_SPAWN;

static constexpr int ERR_NONE = 0;
static constexpr int ERR_NOT_FOUND = 1;
static constexpr int ERR_EXISTS = 2;
static constexpr int ERR_NO_SPACE = 3;
static constexpr int ERR_BAD_FD = 4;
static constexpr int ERR_BAD_PTR = 5;
static constexpr int ERR_NOT_DIR = 6;
static constexpr int ERR_IS_DIR = 7;
static constexpr int ERR_INVALID = 8;
static constexpr int ERR_EOF = 9;
static constexpr int ERR_TIMEOUT = 10;
static constexpr int ERR_AGAIN = 11;
static constexpr int ERR_CANCELED = 12;
static constexpr int ERR_ACCESS = 13;
static constexpr int ERR_CORRUPT = 14;
static constexpr int ERR_SIGNATURE = 15;

static constexpr int BLOCK_WORDS = vm::MMU_PAGE_WORDS;
static constexpr int FS_MAGIC = 80808;
static constexpr int FS_VERSION = 1;
static constexpr int DEFAULT_INODE_COUNT = 32;
static constexpr int DIRECT_BLOCKS = 6;

static constexpr int NATIVE_VFS_MAGIC = 60606;
static constexpr int NATIVE_VFS_VERSION = 1;
static constexpr int NATIVE_KERNEL_MAGIC = 40404;
static constexpr int NATIVE_VFS_REQUIRED_BLOCKS = 7303;
static constexpr int NATIVE_VFS_MAX_INODES = 2048;
static constexpr int NATIVE_VFS_MAX_DIRENTS = 4096;
static constexpr int NATIVE_VFS_MAX_EXTENTS = 4096;
static constexpr int NATIVE_VFS_MAX_NAME_WORDS = 16;
static constexpr int NATIVE_VFS_PAYLOAD_WORDS = 65536;
static constexpr int NATIVE_KIND_FILE = 1;
static constexpr int NATIVE_KIND_DIR = 2;
static constexpr int NATIVE_KIND_EXEC = 3;
static constexpr int NATIVE_VFS_DISK_SUPER_BLOCK = 0;
static constexpr int NATIVE_VFS_DISK_INODE_BLOCK = 2;
static constexpr int NATIVE_VFS_DISK_INODE_BLOCKS = 607;
static constexpr int NATIVE_VFS_DISK_DIRENT_BLOCK = 609;
static constexpr int NATIVE_VFS_DISK_DIRENT_BLOCKS = 911;
static constexpr int NATIVE_VFS_DISK_DIRENT_NAME_BLOCK = 1520;
static constexpr int NATIVE_VFS_DISK_DIRENT_NAME_BLOCKS = 2428;
static constexpr int NATIVE_VFS_DISK_EXTENT_BLOCK = 3948;
static constexpr int NATIVE_VFS_DISK_EXTENT_BLOCKS = 911;
static constexpr int NATIVE_VFS_DISK_DATA_BLOCK = 4859;
static constexpr int NATIVE_VFS_DISK_DATA_BLOCKS = 2428;
static constexpr int NATIVE_WAL_DISK_META_BLOCK = 7287;
static constexpr int NATIVE_WAL_DISK_RECORD_BLOCK = 7288;
static constexpr int NATIVE_WAL_DISK_RECORD_BLOCKS = 15;
static constexpr int NATIVE_VFS_INODE_WORDS = 8;
static constexpr int NATIVE_VFS_DIRENT_WORDS = 6;
static constexpr int NATIVE_VFS_EXTENT_WORDS = 6;
static constexpr int NATIVE_VFS_DATA_BASE = 310000;
static constexpr int NATIVE_EXEC_DESC_WORDS = vm::EXEC_HEADER_WORDS + 1;
static constexpr int NATIVE_EXEC_DESC_V2_WORDS = vm::EXEC_HEADER_WORDS + 3;
static constexpr int OS_CLUSTER_WORDS = vm::OS_CLUSTER_WORDS;
static constexpr int PACKAGE_MAGIC = 90909;
static constexpr int PACKAGE_FORMAT_VERSION = 1;
static constexpr int RELEASE_IMAGE_MAGIC = 91919;
static constexpr int SIGNED_EXEC_METADATA_VERSION = 1;

static constexpr int CAP_FILE_READ = 1 << 0;
static constexpr int CAP_FILE_WRITE = 1 << 1;
static constexpr int CAP_WINDOW = 1 << 2;
static constexpr int CAP_IPC = 1 << 3;
static constexpr int CAP_PROCESS_CONTROL = 1 << 4;
static constexpr int CAP_ALL =
    CAP_FILE_READ | CAP_FILE_WRITE | CAP_WINDOW | CAP_IPC | CAP_PROCESS_CONTROL;

using ProductionProfile = vm::ProductionProfile;

struct StatusResult {
    int status = T1_ERROR;
    int payload = 0;
    int detail = ERR_INVALID;

    [[nodiscard]] static StatusResult success(int payload = 0) {
        return StatusResult{T1_SUCCESS, payload, ERR_NONE};
    }
    [[nodiscard]] static StatusResult pending(int payload = 0, int detail = ERR_NONE) {
        return StatusResult{T1_PENDING, payload, detail};
    }
    [[nodiscard]] static StatusResult error(int detail, int payload = 0) {
        return StatusResult{T1_ERROR, payload, detail};
    }
    [[nodiscard]] bool ok() const { return status == T1_SUCCESS; }
};

struct FsConsistencyReport {
    std::vector<std::string> errors;
    std::vector<std::string> warnings;
    int checked_inodes = 0;
    int files = 0;
    int directories = 0;
    int executables = 0;
    int referenced_blocks = 0;
    int free_blocks = 0;

    [[nodiscard]] bool ok() const { return errors.empty(); }
    [[nodiscard]] StatusResult status() const {
        return ok() ? StatusResult::success(checked_inodes)
                    : StatusResult::error(ERR_CORRUPT, static_cast<int>(errors.size()));
    }
    void fail(const std::string& message) { errors.push_back(message); }
    void warn(const std::string& message) { warnings.push_back(message); }
};

enum class BootMode : int {
    Normal = 0,
    Recovery = 1,
};

struct BootRecoveryReport {
    BootMode mode = BootMode::Normal;
    StatusResult status = StatusResult::error(ERR_INVALID);
    FsConsistencyReport fsck;
    std::string reason;
};

enum class JournalWritePhase : int {
    BeforeBegin = 0,
    AfterBegin = 1,
    AfterRecord = 2,
    AfterCommit = 3,
    AfterApply = 4,
    AfterCheckpoint = 5,
};

struct JournalReplayRecord {
    int address = -1;
    long long old_value = 0;
    long long new_value = 0;
    bool committed = false;
    bool applied = false;
};

class JournalReplayHarness {
public:
    explicit JournalReplayHarness(int words)
        : words_(static_cast<std::size_t>(std::max(1, words)), 0) {}

    [[nodiscard]] long long read(int address) const {
        if (address < 0 || address >= static_cast<int>(words_.size())) return 0;
        return words_[static_cast<std::size_t>(address)];
    }

    [[nodiscard]] StatusResult seed(int address, long long value) {
        if (!validAddress(address)) return StatusResult::error(ERR_INVALID);
        words_[static_cast<std::size_t>(address)] = value;
        return StatusResult::success(address);
    }

    [[nodiscard]] StatusResult simulateWriteCrash(
        int address,
        long long new_value,
        JournalWritePhase phase) {

        if (!validAddress(address)) return StatusResult::error(ERR_INVALID);
        const long long old_value = words_[static_cast<std::size_t>(address)];
        if (phase == JournalWritePhase::BeforeBegin ||
            phase == JournalWritePhase::AfterBegin) {
            return StatusResult::pending(address, ERR_CANCELED);
        }

        JournalReplayRecord record{address, old_value, new_value, false, false};
        if (phase == JournalWritePhase::AfterCommit ||
            phase == JournalWritePhase::AfterApply ||
            phase == JournalWritePhase::AfterCheckpoint) {
            record.committed = true;
        }
        if (phase == JournalWritePhase::AfterApply ||
            phase == JournalWritePhase::AfterCheckpoint) {
            record.applied = true;
            words_[static_cast<std::size_t>(address)] = new_value;
        }
        journal_.push_back(record);
        if (phase == JournalWritePhase::AfterCheckpoint) {
            journal_.clear();
        }
        return StatusResult::success(address);
    }

    [[nodiscard]] StatusResult recover() {
        for (const JournalReplayRecord& record : journal_) {
            if (!validAddress(record.address)) return StatusResult::error(ERR_CORRUPT);
            words_[static_cast<std::size_t>(record.address)] =
                record.committed ? record.new_value : record.old_value;
        }
        journal_.clear();
        return StatusResult::success(static_cast<int>(words_.size()));
    }

    [[nodiscard]] int pendingRecords() const {
        return static_cast<int>(journal_.size());
    }

private:
    std::vector<long long> words_;
    std::vector<JournalReplayRecord> journal_;

    [[nodiscard]] bool validAddress(int address) const {
        return address >= 0 && address < static_cast<int>(words_.size());
    }
};

struct SignedExecutableMetadata {
    int version = SIGNED_EXEC_METADATA_VERSION;
    std::string signer;
    long long content_hash = 0;
    long long header_hash = 0;
    long long signature = 0;
    int flags = 0;
    bool present = false;
};

struct PackageEntry {
    std::string path;
    std::vector<long long> words;
    bool executable = false;
    vm::ExecutableImageHeader header;
    SignedExecutableMetadata metadata;
};

struct PackageImage {
    int magic = PACKAGE_MAGIC;
    int version = PACKAGE_FORMAT_VERSION;
    std::string name;
    int update_epoch = 0;
    std::vector<PackageEntry> entries;
};

[[nodiscard]] inline long long stableWordHash(
    const std::vector<long long>& words,
    std::uint64_t seed = 1469598103934665603ULL) {

    std::uint64_t hash = seed;
    for (long long word : words) {
        hash ^= static_cast<std::uint64_t>(word) + 0x9e3779b97f4a7c15ULL +
                (hash << 6) + (hash >> 2);
        hash *= 1099511628211ULL;
    }
    return static_cast<long long>(hash & 0x3fffffffffffffffLL);
}

[[nodiscard]] inline std::vector<long long> executableHeaderWords(
    const vm::ExecutableImageHeader& header) {

    return {
        header.magic,
        header.version,
        header.abi_version,
        header.entry_virtual_pc,
        header.text_pages,
        header.data_pages,
        header.stack_words,
        header.syscall_abi_version,
        header.flags,
    };
}

inline void appendStringWords(std::vector<long long>& out, const std::string& text) {
    out.push_back(static_cast<long long>(text.size()));
    for (unsigned char c : text) out.push_back(static_cast<long long>(c));
}

[[nodiscard]] inline std::vector<long long> stringWords(const std::string& text) {
    std::vector<long long> out;
    appendStringWords(out, text);
    return out;
}

[[nodiscard]] inline SignedExecutableMetadata signExecutableMetadata(
    const std::vector<long long>& image,
    const vm::ExecutableImageHeader& header,
    const std::string& signer,
    const std::string& secret,
    int flags = 0) {

    SignedExecutableMetadata metadata;
    metadata.signer = signer;
    metadata.content_hash = stableWordHash(image);
    metadata.header_hash = stableWordHash(executableHeaderWords(header), 0xcbf29ce484222325ULL);
    metadata.flags = flags;
    std::vector<long long> signature_words = {
        metadata.content_hash,
        metadata.header_hash,
        static_cast<long long>(metadata.flags),
    };
    std::vector<long long> signer_words = stringWords(signer);
    std::vector<long long> secret_words = stringWords(secret);
    signature_words.insert(signature_words.end(), signer_words.begin(), signer_words.end());
    signature_words.insert(signature_words.end(), secret_words.begin(), secret_words.end());
    metadata.signature = stableWordHash(signature_words, 0x84222325cbf29ce4ULL);
    metadata.present = true;
    return metadata;
}

[[nodiscard]] inline bool executableMetadataMatches(
    const std::vector<long long>& image,
    const vm::ExecutableImageHeader& header,
    const SignedExecutableMetadata& metadata) {

    return metadata.present &&
           metadata.version == SIGNED_EXEC_METADATA_VERSION &&
           metadata.content_hash == stableWordHash(image) &&
           metadata.header_hash == stableWordHash(executableHeaderWords(header), 0xcbf29ce484222325ULL) &&
           metadata.signature != 0;
}

[[nodiscard]] inline bool verifySignedExecutableMetadata(
    const std::vector<long long>& image,
    const vm::ExecutableImageHeader& header,
    const SignedExecutableMetadata& metadata,
    const std::string& secret) {

    if (!executableMetadataMatches(image, header, metadata)) return false;
    SignedExecutableMetadata expected =
        signExecutableMetadata(image, header, metadata.signer, secret, metadata.flags);
    return expected.signature == metadata.signature;
}

[[nodiscard]] inline std::vector<long long> encodeSignedExecutableMetadata(
    const SignedExecutableMetadata& metadata) {

    std::vector<long long> out = {
        SIGNED_EXEC_METADATA_VERSION,
        metadata.present ? 1LL : 0LL,
        metadata.content_hash,
        metadata.header_hash,
        metadata.signature,
        metadata.flags,
    };
    appendStringWords(out, metadata.signer);
    return out;
}

[[nodiscard]] inline std::vector<long long> encodePackageManifest(const PackageImage& package) {
    std::vector<long long> out = {
        package.magic,
        package.version,
        package.update_epoch,
        static_cast<long long>(package.entries.size()),
    };
    appendStringWords(out, package.name);
    for (const PackageEntry& entry : package.entries) {
        out.push_back(entry.executable ? 1LL : 0LL);
        appendStringWords(out, entry.path);
        out.push_back(static_cast<long long>(entry.words.size()));
        out.push_back(stableWordHash(entry.words));
        out.push_back(entry.metadata.present ? entry.metadata.signature : 0);
    }
    return out;
}

class PackageImageBuilder {
public:
    PackageImageBuilder(std::string name, int update_epoch)
        : package_{PACKAGE_MAGIC, PACKAGE_FORMAT_VERSION, std::move(name), update_epoch, {}} {}

    [[nodiscard]] StatusResult addFile(
        const std::string& path,
        const std::vector<long long>& words) {

        if (!validPackagePath(path)) return StatusResult::error(ERR_INVALID);
        PackageEntry entry;
        entry.path = path;
        entry.words = words;
        package_.entries.push_back(std::move(entry));
        return StatusResult::success(static_cast<int>(package_.entries.size()));
    }

    [[nodiscard]] StatusResult addExecutable(
        const std::string& path,
        const std::vector<long long>& image,
        const vm::ExecutableImageHeader& header,
        const SignedExecutableMetadata& metadata) {

        if (!validPackagePath(path) ||
            !vm::validateExecutableHeader(header) ||
            !executableMetadataMatches(image, header, metadata)) {
            return StatusResult::error(ERR_INVALID);
        }
        PackageEntry entry;
        entry.path = path;
        entry.words = image;
        entry.executable = true;
        entry.header = header;
        entry.metadata = metadata;
        package_.entries.push_back(std::move(entry));
        return StatusResult::success(static_cast<int>(package_.entries.size()));
    }

    [[nodiscard]] PackageImage build() const { return package_; }

private:
    PackageImage package_;

    [[nodiscard]] static bool validPackagePath(const std::string& path) {
        return !path.empty() && path[0] == '/' && path.find("..") == std::string::npos;
    }
};

enum class UserPtrState : int8_t {
    Null = T1_ERROR,
    Unknown = T1_PENDING,
    Valid = T1_SUCCESS,
};

template<typename T>
struct UserPtr {
    int address = 0;
    UserPtrState state = UserPtrState::Unknown;

    [[nodiscard]] bool canDeref() const {
        return state == UserPtrState::Valid && address >= 0;
    }
};

enum class SharedOrder : int8_t {
    Relaxed = T1_ERROR,
    AcquireRelease = T1_PENDING,
    Sequential = T1_SUCCESS,
};

struct SharedWord {
    std::atomic<long long> value{0};
    SharedOrder order = SharedOrder::AcquireRelease;

    [[nodiscard]] long long tldr(SharedOrder required) const {
        return value.load(memoryOrderFor(required, false));
    }
    [[nodiscard]] StatusResult tstr(long long desired, long long expected, SharedOrder required) {
        long long observed = expected;
        if (!value.compare_exchange_strong(
                observed,
                desired,
                memoryOrderFor(required, true),
                std::memory_order_acquire)) {
            return StatusResult::pending(static_cast<int>(observed), ERR_INVALID);
        }
        return StatusResult::success(static_cast<int>(desired));
    }

private:
    [[nodiscard]] static std::memory_order memoryOrderFor(SharedOrder order, bool store) {
        switch (order) {
            case SharedOrder::Relaxed: return std::memory_order_relaxed;
            case SharedOrder::Sequential: return std::memory_order_seq_cst;
            case SharedOrder::AcquireRelease:
                return store ? std::memory_order_acq_rel : std::memory_order_acquire;
        }
        return std::memory_order_acquire;
    }
};

// =============================================================================
// Device tree and block storage
// =============================================================================

struct DeviceNode {
    std::string name;
    std::string compatible;
    std::map<std::string, int> properties;
};

struct DeviceTree {
    std::vector<DeviceNode> nodes;

    void add(DeviceNode node) {
        nodes.push_back(std::move(node));
    }

    [[nodiscard]] const DeviceNode* find(const std::string& name) const {
        for (const auto& node : nodes) {
            if (node.name == name) return &node;
        }
        return nullptr;
    }

    [[nodiscard]] StatusResult validate(std::vector<std::string>* errors = nullptr) const {
        std::set<std::string> seen;
        bool ok = true;
        for (const auto& node : nodes) {
            if (!seen.insert(node.name).second) {
                ok = false;
                if (errors) errors->push_back("duplicate device node '" + node.name + "'");
            }
        }
        for (const char* required : {"console", "timer", "block0"}) {
            if (!find(required)) {
                ok = false;
                if (errors) errors->push_back(std::string("missing required device '") + required + "'");
            }
        }
        const DeviceNode* block = find("block0");
        if (block) {
            auto words = block->properties.find("block_words");
            auto count = block->properties.find("block_count");
            if (words == block->properties.end() || words->second != BLOCK_WORDS) {
                ok = false;
                if (errors) errors->push_back("block0 must use 27-word blocks");
            }
            if (count == block->properties.end() || count->second <= 0) {
                ok = false;
                if (errors) errors->push_back("block0 must declare positive block_count");
            }
        }
        return ok ? StatusResult::success() : StatusResult::error(ERR_INVALID);
    }
};

[[nodiscard]] inline DeviceTree defaultDeviceTree(int block_count) {
    DeviceTree tree;
    tree.add(DeviceNode{"console", "trit,console-v1", {{"csr_out", isa::CSR_CONSOLE_OUT}}});
    tree.add(DeviceNode{"timer", "trit,timer-v1", {{"csr_counter", isa::CSR_TIMER_COUNTER}}});
    tree.add(DeviceNode{"block0", "trit,block-v1",
                        {{"block_words", BLOCK_WORDS}, {"block_count", block_count}}});
    return tree;
}

class BlockDevice {
public:
    explicit BlockDevice(int block_count = 128)
        : block_count_(std::max(1, block_count)) {}

    BlockDevice(int block_count, std::string backing_path)
        : block_count_(std::max(1, block_count)), backing_path_(std::move(backing_path)) {
        (void)loadCompactBacking();
        (void)rewriteCompactBacking();
    }

    [[nodiscard]] int blockCount() const { return block_count_; }
    [[nodiscard]] int blockWords() const { return BLOCK_WORDS; }
    [[nodiscard]] std::size_t allocatedBlocks() const { return blocks_.size(); }

    [[nodiscard]] StatusResult attachBackingFile(const std::string& path) {
        backing_path_ = path;
        return loadCompactBacking() && rewriteCompactBacking()
                   ? StatusResult::success(block_count_)
                   : StatusResult::error(ERR_INVALID);
    }

    [[nodiscard]] StatusResult readBlock(int index, std::vector<long long>& out) const {
        if (index < 0 || index >= blockCount()) return StatusResult::error(ERR_INVALID);
        out.assign(BLOCK_WORDS, 0);
        auto found = blocks_.find(index);
        if (found != blocks_.end()) {
            out = found->second;
            return StatusResult::success(BLOCK_WORDS);
        }
        return StatusResult::success(BLOCK_WORDS);
    }

    [[nodiscard]] StatusResult writeBlock(int index, const std::vector<long long>& data) {
        if (index < 0 || index >= blockCount()) return StatusResult::error(ERR_INVALID);
        if (static_cast<int>(data.size()) != BLOCK_WORDS) return StatusResult::error(ERR_INVALID);
        blocks_[index] = data;
        dirty_.insert(index);
        if (!backing_path_.empty() && !rewriteCompactBacking()) {
            return StatusResult::error(ERR_INVALID);
        }
        return StatusResult::success(BLOCK_WORDS);
    }

    [[nodiscard]] bool dirty(int index) const {
        return index >= 0 && index < blockCount() && dirty_.count(index) != 0;
    }

    [[nodiscard]] std::vector<long long> serialize() const {
        std::vector<long long> out;
        out.reserve(static_cast<std::size_t>(blockCount()) * static_cast<std::size_t>(BLOCK_WORDS));
        std::vector<long long> block;
        for (int index = 0; index < blockCount(); ++index) {
            (void)readBlock(index, block);
            out.insert(out.end(), block.begin(), block.end());
        }
        return out;
    }

    [[nodiscard]] StatusResult loadSerialized(const std::vector<long long>& image) {
        if (image.empty() || static_cast<int>(image.size()) % BLOCK_WORDS != 0) {
            return StatusResult::error(ERR_INVALID);
        }
        const int blocks = static_cast<int>(image.size()) / BLOCK_WORDS;
        block_count_ = std::max(1, blocks);
        blocks_.clear();
        dirty_.clear();
        for (int block = 0; block < blocks; ++block) {
            std::vector<long long> payload(BLOCK_WORDS, 0);
            for (int word = 0; word < BLOCK_WORDS; ++word) {
                payload[static_cast<std::size_t>(word)] =
                    image[static_cast<std::size_t>(block * BLOCK_WORDS + word)];
            }
            if (!isZeroBlock(payload)) blocks_[block] = std::move(payload);
        }
        return StatusResult::success(blocks);
    }

private:
    int block_count_ = 1;
    std::unordered_map<int, std::vector<long long>> blocks_;
    std::unordered_set<int> dirty_;
    std::string backing_path_;

    [[nodiscard]] static bool isZeroBlock(const std::vector<long long>& block) {
        for (long long word : block) {
            if (word != 0) return false;
        }
        return true;
    }

    [[nodiscard]] bool ensureBackingFile() const {
        if (backing_path_.empty()) return true;
        std::fstream file(backing_path_, std::ios::in | std::ios::out | std::ios::binary);
        if (file.good()) return true;
        std::ofstream create(backing_path_, std::ios::binary);
        create.close();
        return static_cast<bool>(std::fstream(backing_path_, std::ios::in | std::ios::out | std::ios::binary));
    }

    [[nodiscard]] bool loadCompactBacking() {
        if (backing_path_.empty()) return true;
        if (!ensureBackingFile()) return false;
        std::ifstream file(backing_path_, std::ios::binary);
        if (!file.good()) return false;
        long long magic = 0;
        int count = 0;
        file.read(reinterpret_cast<char*>(&magic), sizeof(magic));
        file.read(reinterpret_cast<char*>(&count), sizeof(count));
        if (!file.good()) return true;
        if (magic != kSparseDiskMagic || count < 0) return false;
        blocks_.clear();
        for (int i = 0; i < count; ++i) {
            int index = -1;
            std::vector<long long> payload(BLOCK_WORDS, 0);
            file.read(reinterpret_cast<char*>(&index), sizeof(index));
            for (int word = 0; word < BLOCK_WORDS; ++word) {
                file.read(reinterpret_cast<char*>(&payload[static_cast<std::size_t>(word)]),
                          sizeof(long long));
            }
            if (!file.good()) return false;
            if (index >= 0 && index < block_count_ && !isZeroBlock(payload)) {
                blocks_[index] = std::move(payload);
            }
        }
        return true;
    }

    [[nodiscard]] bool rewriteCompactBacking() const {
        if (backing_path_.empty()) return true;
        if (!ensureBackingFile()) return false;
        std::ofstream file(backing_path_, std::ios::binary | std::ios::trunc);
        if (!file.good()) return false;
        const long long magic = kSparseDiskMagic;
        const int count = static_cast<int>(blocks_.size());
        file.write(reinterpret_cast<const char*>(&magic), sizeof(magic));
        file.write(reinterpret_cast<const char*>(&count), sizeof(count));
        for (const auto& [index, payload] : blocks_) {
            file.write(reinterpret_cast<const char*>(&index), sizeof(index));
            for (long long word : payload) {
                file.write(reinterpret_cast<const char*>(&word), sizeof(word));
            }
        }
        file.flush();
        return static_cast<bool>(file);
    }

    static constexpr long long kSparseDiskMagic = 0x54524954535031LL;
};

// =============================================================================
// Tiny Unix-style filesystem
// =============================================================================

enum class InodeKind : int {
    Free = 0,
    File = 1,
    Directory = 2,
    Executable = 3,
};

struct DirectoryEntry {
    std::string name;
    int inode = -1;
};

struct FileStat {
    int inode = -1;
    InodeKind kind = InodeKind::Free;
    int size_words = 0;
    int direct_blocks = 0;
    int indirect_block = -1;
};

struct Inode {
    int id = -1;
    InodeKind kind = InodeKind::Free;
    int size_words = 0;
    std::array<int, DIRECT_BLOCKS> direct{};
    int indirect_block = -1;
    std::vector<DirectoryEntry> entries;
    std::vector<long long> data;
    std::vector<int> indirect_blocks;
    bool executable = false;
    vm::ExecutableImageHeader exec_header;

    Inode() {
        direct.fill(-1);
    }
};

class TinyFileSystem {
public:
    [[nodiscard]] StatusResult format(BlockDevice& device, int inode_count = DEFAULT_INODE_COUNT) {
        if (device.blockCount() < inode_count + 4) return StatusResult::error(ERR_NO_SPACE);
        device_ = &device;
        inode_count_ = std::max(4, inode_count);
        data_start_ = 2 + inode_count_;
        free_blocks_.assign(static_cast<std::size_t>(device.blockCount()), true);
        for (int i = 0; i < data_start_ && i < device.blockCount(); ++i) {
            free_blocks_[static_cast<std::size_t>(i)] = false;
        }
        next_alloc_block_ = data_start_;
        free_block_count_ = std::max(0, device.blockCount() - data_start_);
        inodes_.assign(static_cast<std::size_t>(inode_count_), Inode{});
        for (int i = 0; i < inode_count_; ++i) inodes_[static_cast<std::size_t>(i)].id = i;
        Inode& root = inodes_[0];
        root.kind = InodeKind::Directory;
        root.entries.push_back({".", 0});
        root.entries.push_back({"..", 0});
        root.size_words = static_cast<int>(root.entries.size());
        mounted_ = true;
        return sync();
    }

    [[nodiscard]] StatusResult mount(BlockDevice& device) {
        std::vector<long long> super;
        StatusResult read = device.readBlock(0, super);
        if (!read.ok()) return read;
        if (super.size() < 6 || super[0] != FS_MAGIC || super[1] != FS_VERSION ||
            super[2] != BLOCK_WORDS) {
            return StatusResult::error(ERR_INVALID);
        }
        device_ = &device;
        inode_count_ = static_cast<int>(super[3]);
        data_start_ = static_cast<int>(super[4]);
        if (inode_count_ <= 0 || data_start_ <= 1 || data_start_ >= device.blockCount()) {
            return StatusResult::error(ERR_INVALID);
        }

        // Read back the free blocks bitmap
        std::vector<long long> bitmap;
        if (!device.readBlock(1, bitmap).ok()) return StatusResult::error(ERR_INVALID);
        free_blocks_.resize(static_cast<std::size_t>(device.blockCount()), true);
        for (int i = 0; i < std::min(BLOCK_WORDS, static_cast<int>(free_blocks_.size())); ++i) {
            free_blocks_[static_cast<std::size_t>(i)] = (bitmap[static_cast<std::size_t>(i)] == 1);
        }
        for (int i = 0; i < data_start_ && i < device.blockCount(); ++i) {
            free_blocks_[static_cast<std::size_t>(i)] = false;
        }

        // Read back all inodes
        inodes_.assign(static_cast<std::size_t>(inode_count_), Inode{});
        for (int i = 0; i < inode_count_; ++i) {
            Inode& inode = inodes_[static_cast<std::size_t>(i)];
            inode.id = i;
            std::vector<long long> raw;
            if (!device.readBlock(2 + i, raw).ok()) return StatusResult::error(ERR_INVALID);
            inode.kind = static_cast<InodeKind>(raw[0]);
            inode.size_words = static_cast<int>(raw[1]);
            inode.executable = (raw[2] == 1);
            inode.indirect_block = static_cast<int>(raw[3]);
            for (int d = 0; d < DIRECT_BLOCKS; ++d) {
                inode.direct[static_cast<std::size_t>(d)] = static_cast<int>(raw[4 + d]);
            }
            inode.exec_header.entry_virtual_pc = static_cast<int>(raw[12]);
            inode.exec_header.text_pages = static_cast<int>(raw[13]);
            inode.exec_header.data_pages = static_cast<int>(raw[14]);
            inode.exec_header.stack_words = static_cast<int>(raw[15]);
            inode.indirect_blocks.clear();
            if (inode.indirect_block >= 0) {
                std::vector<long long> indirect;
                if (!device.readBlock(inode.indirect_block, indirect).ok()) {
                    return StatusResult::error(ERR_INVALID);
                }
                for (int w = 0; w < BLOCK_WORDS; ++w) {
                    if (indirect[static_cast<std::size_t>(w)] >= 0) {
                        inode.indirect_blocks.push_back(
                            static_cast<int>(indirect[static_cast<std::size_t>(w)]));
                    }
                }
            }
        }
        rebuildFreeBlocksFromInodes();

        // Read back file data and directory entries for all valid inodes
        for (int i = 0; i < inode_count_; ++i) {
            Inode& inode = inodes_[static_cast<std::size_t>(i)];
            if (inode.kind == InodeKind::Free) continue;

            if (inode.kind == InodeKind::Directory) {
                std::vector<long long> dir_words;
                for (int block : inode.direct) {
                    if (block < 0) continue;
                    std::vector<long long> raw;
                    if (device.readBlock(block, raw).ok()) {
                        dir_words.insert(dir_words.end(), raw.begin(), raw.end());
                    }
                }
                if (!dir_words.empty() && dir_words[0] > 0) {
                    inode.entries.clear();
                    int num_entries = static_cast<int>(dir_words[0]);
                    int idx = 1;
                    for (int e = 0; e < num_entries && idx < static_cast<int>(dir_words.size()); ++e) {
                        int entry_inode = static_cast<int>(dir_words[static_cast<std::size_t>(idx++)]);
                        int name_len = static_cast<int>(dir_words[static_cast<std::size_t>(idx++)]);
                        std::string name;
                        for (int c = 0; c < name_len && idx < static_cast<int>(dir_words.size()); ++c) {
                            name.push_back(static_cast<char>(dir_words[static_cast<std::size_t>(idx++)]));
                        }
                        inode.entries.push_back({name, entry_inode});
                    }
                }
            } else {
                inode.data.resize(static_cast<std::size_t>(inode.size_words), 0);
                std::vector<int> blocks = fileDataBlocks(inode);
                for (std::size_t d = 0; d < blocks.size(); ++d) {
                    int block = blocks[d];
                    if (block < 0) continue;
                    std::vector<long long> raw;
                    if (device.readBlock(block, raw).ok()) {
                        int start = static_cast<int>(d) * BLOCK_WORDS;
                        for (int w = 0; w < BLOCK_WORDS && start + w < inode.size_words; ++w) {
                            inode.data[static_cast<std::size_t>(start + w)] = raw[static_cast<std::size_t>(w)];
                        }
                    }
                }
            }
        }

        mounted_ = true;
        return StatusResult::success();
    }

    [[nodiscard]] bool mounted() const { return mounted_; }

    [[nodiscard]] FsConsistencyReport checkConsistency() const {
        FsConsistencyReport report;
        if (!mounted_) {
            report.fail("filesystem is not mounted");
            return report;
        }
        if (!device_) {
            report.fail("filesystem has no attached block device");
            return report;
        }
        if (inode_count_ <= 0 ||
            inode_count_ != static_cast<int>(inodes_.size())) {
            report.fail("inode table size does not match superblock metadata");
        }
        if (data_start_ <= 1 || data_start_ >= device_->blockCount()) {
            report.fail("data region starts outside the block device");
        }
        if (free_blocks_.size() != static_cast<std::size_t>(device_->blockCount())) {
            report.fail("free block bitmap size does not match block device");
        }
        if (!validInode(0) ||
            inodes_[0].kind != InodeKind::Directory) {
            report.fail("root inode is not a valid directory");
        }

        std::unordered_map<int, int> block_owner;
        for (int block = 0; block < data_start_ && block < device_->blockCount(); ++block) {
            if (block < static_cast<int>(free_blocks_.size()) &&
                free_blocks_[static_cast<std::size_t>(block)]) {
                report.fail("reserved block is marked free: " + std::to_string(block));
            }
        }

        auto checkBlock = [&](int block, int inode_id, const std::string& label) {
            if (block < 0) return;
            if (block < data_start_ || block >= device_->blockCount()) {
                report.fail(label + " references block outside data region: " + std::to_string(block));
                return;
            }
            auto existing = block_owner.find(block);
            if (existing != block_owner.end() && existing->second != inode_id) {
                report.fail(label + " duplicates block " + std::to_string(block));
                return;
            }
            block_owner[block] = inode_id;
            if (block < static_cast<int>(free_blocks_.size()) &&
                free_blocks_[static_cast<std::size_t>(block)]) {
                report.fail(label + " references a block marked free: " + std::to_string(block));
            }
            ++report.referenced_blocks;
        };

        for (std::size_t i = 0; i < inodes_.size(); ++i) {
            const Inode& inode = inodes_[i];
            if (inode.kind == InodeKind::Free) continue;
            ++report.checked_inodes;
            if (inode.id != static_cast<int>(i)) {
                report.fail("inode id does not match table slot: " + std::to_string(static_cast<int>(i)));
            }
            if (inode.size_words < 0) {
                report.fail("inode has negative size: " + std::to_string(inode.id));
            }
            if (inode.kind == InodeKind::Directory) {
                ++report.directories;
                bool saw_dot = false;
                bool saw_dotdot = false;
                for (const DirectoryEntry& entry : inode.entries) {
                    if (entry.name == ".") saw_dot = true;
                    if (entry.name == "..") saw_dotdot = true;
                    if (entry.name.empty()) {
                        report.fail("directory contains an empty entry name");
                    }
                    if (!validInode(entry.inode)) {
                        report.fail("directory entry points at an invalid inode: " + entry.name);
                    }
                }
                if (!saw_dot || !saw_dotdot) {
                    report.fail("directory missing dot entries: inode " + std::to_string(inode.id));
                }
            } else {
                if (inode.kind == InodeKind::Executable || inode.executable) {
                    ++report.executables;
                    if (!vm::validateExecutableHeader(inode.exec_header)) {
                        report.fail("executable inode has invalid header: " + std::to_string(inode.id));
                    }
                } else {
                    ++report.files;
                }
                if (inode.size_words != static_cast<int>(inode.data.size())) {
                    report.fail("file payload size does not match inode size: " +
                                std::to_string(inode.id));
                }
                const int expected_blocks = (inode.size_words + BLOCK_WORDS - 1) / BLOCK_WORDS;
                if (static_cast<int>(fileDataBlocks(inode).size()) < expected_blocks) {
                    report.fail("file inode is missing data blocks: " + std::to_string(inode.id));
                }
            }

            for (int block : inode.direct) {
                checkBlock(block, inode.id, "inode " + std::to_string(inode.id));
            }
            checkBlock(inode.indirect_block, inode.id,
                       "inode " + std::to_string(inode.id) + " indirect");
            for (int block : inode.indirect_blocks) {
                checkBlock(block, inode.id,
                           "inode " + std::to_string(inode.id) + " indirect data");
            }
        }

        for (int block = data_start_; block < static_cast<int>(free_blocks_.size()); ++block) {
            const bool referenced = block_owner.find(block) != block_owner.end();
            const bool marked_free = free_blocks_[static_cast<std::size_t>(block)];
            if (referenced && marked_free) {
                report.fail("referenced block is marked free: " + std::to_string(block));
            }
            if (!referenced && !marked_free) {
                report.warn("unreferenced block is marked allocated: " + std::to_string(block));
            }
            if (marked_free) ++report.free_blocks;
        }
        return report;
    }

    [[nodiscard]] StatusResult createFile(
        const std::string& path,
        InodeKind kind = InodeKind::File,
        bool executable = false) {

        if (!mounted_) return StatusResult::error(ERR_INVALID);
        if (path.empty() || path[0] != '/') return StatusResult::error(ERR_INVALID);
        if (lookup(path).ok()) return StatusResult::error(ERR_EXISTS);
        std::string parentPath;
        std::string name;
        splitPath(path, parentPath, name);
        int parent = lookup(parentPath.empty() ? "/" : parentPath).payload;
        if (!validInode(parent) || inodes_[static_cast<std::size_t>(parent)].kind != InodeKind::Directory) {
            return StatusResult::error(ERR_NOT_DIR);
        }
        int id = allocateInode();
        if (id < 0) return StatusResult::error(ERR_NO_SPACE);
        Inode& inode = inodes_[static_cast<std::size_t>(id)];
        inode.kind = kind;
        inode.executable = executable || kind == InodeKind::Executable;
        inode.size_words = 0;
        if (kind == InodeKind::Directory) {
            inode.entries.push_back({".", id});
            inode.entries.push_back({"..", parent});
            inode.size_words = static_cast<int>(inode.entries.size());
        }
        inodes_[static_cast<std::size_t>(parent)].entries.push_back({name, id});
        inodes_[static_cast<std::size_t>(parent)].size_words =
            static_cast<int>(inodes_[static_cast<std::size_t>(parent)].entries.size());
        StatusResult synced = sync();
        return synced.ok() ? StatusResult::success(id) : synced;
    }

    [[nodiscard]] StatusResult writeFile(const std::string& path, const std::vector<long long>& words) {
        StatusResult found = lookup(path);
        if (!found.ok()) return found;
        return writeInode(found.payload, words);
    }

    [[nodiscard]] StatusResult writeInode(int inode_id, const std::vector<long long>& words) {
        if (!validInode(inode_id)) return StatusResult::error(ERR_NOT_FOUND);
        Inode& inode = inodes_[static_cast<std::size_t>(inode_id)];
        if (inode.kind == InodeKind::Directory) return StatusResult::error(ERR_IS_DIR);
        const int needed = (static_cast<int>(words.size()) + BLOCK_WORDS - 1) / BLOCK_WORDS;
        if (needed > DIRECT_BLOCKS + BLOCK_WORDS) return StatusResult::error(ERR_NO_SPACE);
        const int required_blocks = needed + (needed > DIRECT_BLOCKS ? 1 : 0);
        if (freeBlockCount() + allocatedBlockCount(inode) < required_blocks) {
            return StatusResult::error(ERR_NO_SPACE);
        }
        releaseBlocks(inode);
        inode.data = words;
        inode.size_words = static_cast<int>(words.size());
        for (int i = 0; i < std::min(needed, DIRECT_BLOCKS); ++i) {
            int block = allocateBlock();
            if (block < 0) return StatusResult::error(ERR_NO_SPACE);
            inode.direct[static_cast<std::size_t>(i)] = block;
        }
        if (needed > DIRECT_BLOCKS) {
            inode.indirect_block = allocateBlock();
            if (inode.indirect_block < 0) return StatusResult::error(ERR_NO_SPACE);
            for (int i = DIRECT_BLOCKS; i < needed; ++i) {
                int block = allocateBlock();
                if (block < 0) return StatusResult::error(ERR_NO_SPACE);
                inode.indirect_blocks.push_back(block);
            }
        }
        StatusResult synced = sync();
        return synced.ok() ? StatusResult::success(inode.size_words) : synced;
    }

    [[nodiscard]] StatusResult readFile(const std::string& path, std::vector<long long>& out) const {
        StatusResult found = lookup(path);
        if (!found.ok()) return found;
        if (!validInode(found.payload)) return StatusResult::error(ERR_NOT_FOUND);
        const Inode& inode = inodes_[static_cast<std::size_t>(found.payload)];
        if (inode.kind == InodeKind::Directory) return StatusResult::error(ERR_IS_DIR);
        out = inode.data;
        return inode.data.empty() ? StatusResult::pending(0, ERR_EOF)
                                  : StatusResult::success(static_cast<int>(inode.data.size()));
    }

    [[nodiscard]] StatusResult lookup(const std::string& path) const {
        if (!mounted_) return StatusResult::error(ERR_INVALID);
        if (path == "/" || path.empty()) return StatusResult::success(0);
        if (path[0] != '/') return StatusResult::error(ERR_INVALID);
        int current = 0;
        for (const std::string& part : splitComponents(path)) {
            if (!validInode(current)) return StatusResult::error(ERR_NOT_FOUND);
            const Inode& dir = inodes_[static_cast<std::size_t>(current)];
            if (dir.kind != InodeKind::Directory) return StatusResult::error(ERR_NOT_DIR);
            int next = -1;
            for (const auto& entry : dir.entries) {
                if (entry.name == part) {
                    next = entry.inode;
                    break;
                }
            }
            if (next < 0) return StatusResult::error(ERR_NOT_FOUND);
            current = next;
        }
        return StatusResult::success(current);
    }

    [[nodiscard]] StatusResult readdir(const std::string& path, std::vector<DirectoryEntry>& out) const {
        StatusResult found = lookup(path);
        if (!found.ok()) return found;
        const Inode& inode = inodes_[static_cast<std::size_t>(found.payload)];
        if (inode.kind != InodeKind::Directory) return StatusResult::error(ERR_NOT_DIR);
        out = inode.entries;
        return StatusResult::success(static_cast<int>(out.size()));
    }

    [[nodiscard]] StatusResult stat(const std::string& path, FileStat& out) const {
        StatusResult found = lookup(path);
        if (!found.ok()) return found;
        const Inode& inode = inodes_[static_cast<std::size_t>(found.payload)];
        out.inode = inode.id;
        out.kind = inode.kind;
        out.size_words = inode.size_words;
        out.indirect_block = inode.indirect_block;
        out.direct_blocks = 0;
        for (int block : inode.direct) {
            if (block >= 0) ++out.direct_blocks;
        }
        return StatusResult::success(out.size_words);
    }

    [[nodiscard]] StatusResult markExecutable(
        const std::string& path,
        const vm::ExecutableImageHeader& header) {

        StatusResult found = lookup(path);
        if (!found.ok()) return found;
        Inode& inode = inodes_[static_cast<std::size_t>(found.payload)];
        if (inode.kind == InodeKind::Directory) return StatusResult::error(ERR_IS_DIR);
        if (!vm::validateExecutableHeader(header)) return StatusResult::error(ERR_INVALID);
        inode.kind = InodeKind::Executable;
        inode.executable = true;
        inode.exec_header = header;
        StatusResult synced = sync();
        return synced.ok() ? StatusResult::success(found.payload) : synced;
    }

    [[nodiscard]] const Inode* inode(int id) const {
        return validInode(id) ? &inodes_[static_cast<std::size_t>(id)] : nullptr;
    }

    [[nodiscard]] StatusResult sync() {
        if (!device_) return StatusResult::error(ERR_INVALID);
        std::vector<long long> super(BLOCK_WORDS, 0);
        super[0] = FS_MAGIC;
        super[1] = FS_VERSION;
        super[2] = BLOCK_WORDS;
        super[3] = inode_count_;
        super[4] = data_start_;
        super[5] = 0;
        StatusResult status = device_->writeBlock(0, super);
        if (!status.ok()) return status;

        std::vector<long long> bitmap(BLOCK_WORDS, 0);
        for (int i = 0; i < std::min(BLOCK_WORDS, static_cast<int>(free_blocks_.size())); ++i) {
            bitmap[static_cast<std::size_t>(i)] = free_blocks_[static_cast<std::size_t>(i)] ? 1 : -1;
        }
        status = device_->writeBlock(1, bitmap);
        if (!status.ok()) return status;

        // Serialize directory entries into directory data blocks
        for (Inode& inode : inodes_) {
            if (inode.kind != InodeKind::Directory) continue;
            std::vector<long long> dir_words;
            dir_words.push_back(static_cast<long long>(inode.entries.size()));
            for (const auto& entry : inode.entries) {
                dir_words.push_back(static_cast<long long>(entry.inode));
                dir_words.push_back(static_cast<long long>(entry.name.size()));
                for (char c : entry.name) {
                    dir_words.push_back(static_cast<long long>(c));
                }
            }
            int needed = (static_cast<int>(dir_words.size()) + BLOCK_WORDS - 1) / BLOCK_WORDS;
            for (int i = 0; i < std::min(needed, DIRECT_BLOCKS); ++i) {
                if (inode.direct[static_cast<std::size_t>(i)] == -1) {
                    inode.direct[static_cast<std::size_t>(i)] = allocateBlock();
                }
            }
            for (int i = 0; i < std::min(needed, DIRECT_BLOCKS); ++i) {
                int block = inode.direct[static_cast<std::size_t>(i)];
                if (block < 0) continue;
                std::vector<long long> raw(BLOCK_WORDS, 0);
                int start = i * BLOCK_WORDS;
                for (int w = 0; w < BLOCK_WORDS && start + w < static_cast<int>(dir_words.size()); ++w) {
                    raw[static_cast<std::size_t>(w)] = dir_words[static_cast<std::size_t>(start + w)];
                }
                StatusResult wrote = device_->writeBlock(block, raw);
                if (!wrote.ok()) return wrote;
            }
        }

        for (int i = 0; i < inode_count_ && 2 + i < device_->blockCount(); ++i) {
            const Inode& inode = inodes_[static_cast<std::size_t>(i)];
            std::vector<long long> raw(BLOCK_WORDS, 0);
            raw[0] = static_cast<int>(inode.kind);
            raw[1] = inode.size_words;
            raw[2] = inode.executable ? 1 : 0;
            raw[3] = inode.indirect_block;
            for (int d = 0; d < DIRECT_BLOCKS; ++d) raw[4 + d] = inode.direct[static_cast<std::size_t>(d)];
            raw[12] = inode.exec_header.entry_virtual_pc;
            raw[13] = inode.exec_header.text_pages;
            raw[14] = inode.exec_header.data_pages;
            raw[15] = inode.exec_header.stack_words;
            raw[16] = static_cast<int>(inode.entries.size());
            StatusResult wrote = device_->writeBlock(2 + i, raw);
            if (!wrote.ok()) return wrote;
        }

        for (const Inode& inode : inodes_) {
            if (inode.kind == InodeKind::Directory) continue;
            if (inode.indirect_block >= 0) {
                std::vector<long long> indirect(BLOCK_WORDS, -1);
                for (std::size_t i = 0; i < inode.indirect_blocks.size() &&
                                        i < static_cast<std::size_t>(BLOCK_WORDS); ++i) {
                    indirect[i] = inode.indirect_blocks[i];
                }
                StatusResult wrote = device_->writeBlock(inode.indirect_block, indirect);
                if (!wrote.ok()) return wrote;
            }
            std::vector<int> blocks = fileDataBlocks(inode);
            for (std::size_t i = 0; i < blocks.size(); ++i) {
                int block = blocks[i];
                if (block < 0) continue;
                std::vector<long long> raw(BLOCK_WORDS, 0);
                const int start = static_cast<int>(i) * BLOCK_WORDS;
                for (int w = 0; w < BLOCK_WORDS && start + w < inode.size_words; ++w) {
                    raw[static_cast<std::size_t>(w)] = inode.data[static_cast<std::size_t>(start + w)];
                }
                StatusResult wrote = device_->writeBlock(block, raw);
                if (!wrote.ok()) return wrote;
            }
        }
        return StatusResult::success();
    }

private:
    BlockDevice* device_ = nullptr;
    bool mounted_ = false;
    int inode_count_ = 0;
    int data_start_ = 0;
    int next_alloc_block_ = 0;
    int free_block_count_ = 0;
    std::vector<bool> free_blocks_;
    std::vector<Inode> inodes_;

    [[nodiscard]] bool validInode(int id) const {
        return id >= 0 && id < static_cast<int>(inodes_.size()) &&
               inodes_[static_cast<std::size_t>(id)].kind != InodeKind::Free;
    }

    [[nodiscard]] int allocateInode() {
        for (std::size_t i = 1; i < inodes_.size(); ++i) {
            if (inodes_[i].kind == InodeKind::Free) {
                inodes_[i] = Inode{};
                inodes_[i].id = static_cast<int>(i);
                return static_cast<int>(i);
            }
        }
        return -1;
    }

    [[nodiscard]] int allocateBlock() {
        if (free_block_count_ <= 0) return -1;
        const int total = static_cast<int>(free_blocks_.size());
        if (next_alloc_block_ < data_start_ || next_alloc_block_ >= total) {
            next_alloc_block_ = data_start_;
        }
        for (int scanned = 0; scanned < total - data_start_; ++scanned) {
            int i = next_alloc_block_ + scanned;
            if (i >= total) i = data_start_ + (i - total);
            if (free_blocks_[static_cast<std::size_t>(i)]) {
                free_blocks_[static_cast<std::size_t>(i)] = false;
                --free_block_count_;
                next_alloc_block_ = i + 1;
                return i;
            }
        }
        return -1;
    }

    [[nodiscard]] int freeBlockCount() const {
        return free_block_count_;
    }

    [[nodiscard]] static int allocatedBlockCount(const Inode& inode) {
        int count = 0;
        for (int block : inode.direct) {
            if (block >= 0) ++count;
        }
        if (inode.indirect_block >= 0) ++count;
        count += static_cast<int>(inode.indirect_blocks.size());
        return count;
    }

    [[nodiscard]] static std::vector<int> fileDataBlocks(const Inode& inode) {
        std::vector<int> blocks;
        for (int block : inode.direct) {
            if (block >= 0) blocks.push_back(block);
        }
        blocks.insert(blocks.end(), inode.indirect_blocks.begin(), inode.indirect_blocks.end());
        return blocks;
    }

    void rebuildFreeBlocksFromInodes() {
        free_blocks_.assign(static_cast<std::size_t>(device_ ? device_->blockCount() : 0), true);
        for (int i = 0; i < data_start_ && i < static_cast<int>(free_blocks_.size()); ++i) {
            free_blocks_[static_cast<std::size_t>(i)] = false;
        }
        free_block_count_ = std::max(0, static_cast<int>(free_blocks_.size()) - data_start_);
        next_alloc_block_ = data_start_;
        for (const Inode& inode : inodes_) {
            if (inode.kind == InodeKind::Free) continue;
            for (int block : inode.direct) {
                markBlockAllocated(block);
            }
            markBlockAllocated(inode.indirect_block);
            for (int block : inode.indirect_blocks) {
                markBlockAllocated(block);
            }
        }
    }

    void markBlockAllocated(int block) {
        if (block >= 0 && block < static_cast<int>(free_blocks_.size())) {
            if (free_blocks_[static_cast<std::size_t>(block)]) {
                free_blocks_[static_cast<std::size_t>(block)] = false;
                --free_block_count_;
            }
        }
    }

    void releaseBlocks(Inode& inode) {
        for (int& block : inode.direct) {
            if (block >= 0 && block < static_cast<int>(free_blocks_.size())) {
                if (!free_blocks_[static_cast<std::size_t>(block)]) {
                    free_blocks_[static_cast<std::size_t>(block)] = true;
                    ++free_block_count_;
                    next_alloc_block_ = std::min(next_alloc_block_, block);
                }
            }
            block = -1;
        }
        if (inode.indirect_block >= 0 &&
            inode.indirect_block < static_cast<int>(free_blocks_.size())) {
            if (!free_blocks_[static_cast<std::size_t>(inode.indirect_block)]) {
                free_blocks_[static_cast<std::size_t>(inode.indirect_block)] = true;
                ++free_block_count_;
                next_alloc_block_ = std::min(next_alloc_block_, inode.indirect_block);
            }
        }
        inode.indirect_block = -1;
        for (int block : inode.indirect_blocks) {
            if (block >= 0 && block < static_cast<int>(free_blocks_.size())) {
                if (!free_blocks_[static_cast<std::size_t>(block)]) {
                    free_blocks_[static_cast<std::size_t>(block)] = true;
                    ++free_block_count_;
                    next_alloc_block_ = std::min(next_alloc_block_, block);
                }
            }
        }
        inode.indirect_blocks.clear();
    }

    [[nodiscard]] static std::vector<std::string> splitComponents(const std::string& path) {
        std::vector<std::string> parts;
        std::string current;
        for (char c : path) {
            if (c == '/') {
                if (!current.empty()) {
                    parts.push_back(current);
                    current.clear();
                }
            } else {
                current.push_back(c);
            }
        }
        if (!current.empty()) parts.push_back(current);
        return parts;
    }

    static void splitPath(const std::string& path, std::string& parent, std::string& name) {
        const std::size_t slash = path.find_last_of('/');
        if (slash == std::string::npos || slash == 0) {
            parent = "/";
            name = slash == std::string::npos ? path : path.substr(1);
        } else {
            parent = path.substr(0, slash);
            name = path.substr(slash + 1);
        }
    }
};

// =============================================================================
// Root filesystem image builder
// =============================================================================

class RootFsImageBuilder {
public:
    explicit RootFsImageBuilder(int blocks = 128, int inode_count = DEFAULT_INODE_COUNT)
        : device_(blocks) {
        status_ = fs_.format(device_, inode_count);
    }

    [[nodiscard]] StatusResult status() const { return status_; }
    [[nodiscard]] TinyFileSystem& fs() { return fs_; }
    [[nodiscard]] const TinyFileSystem& fs() const { return fs_; }

    [[nodiscard]] StatusResult installBaseLayout() {
        for (const std::string& dir : {"/bin", "/apps", "/etc", "/home", "/tmp",
                                       "/var", "/dev", "/system", "/lib"}) {
            StatusResult made = mkdir(dir);
            if (!made.ok()) return made;
        }
        StatusResult log = mkdir("/var/log");
        if (!log.ok()) return log;
        StatusResult crash = mkdir("/var/crash");
        if (!crash.ok()) return crash;
        StatusResult packages = mkdir("/var/packages");
        if (!packages.ok()) return packages;
        StatusResult services = mkdir("/system/services");
        if (!services.ok()) return services;
        return fs_.sync();
    }

    [[nodiscard]] StatusResult mkdir(const std::string& path) {
        if (!status_.ok()) return status_;
        if (path.empty() || path == "/") return StatusResult::success(0);
        StatusResult found = fs_.lookup(path);
        if (found.ok()) return StatusResult::success(found.payload);
        StatusResult parents = ensureParentDirectories(path);
        if (!parents.ok()) return parents;
        return fs_.createFile(path, InodeKind::Directory);
    }

    [[nodiscard]] StatusResult addFile(const std::string& path, const std::vector<long long>& words) {
        if (!status_.ok()) return status_;
        StatusResult parents = ensureParentDirectories(path);
        if (!parents.ok()) return parents;
        StatusResult found = fs_.lookup(path);
        if (!found.ok()) {
            StatusResult created = fs_.createFile(path, InodeKind::File);
            if (!created.ok()) return created;
        }
        return fs_.writeFile(path, words);
    }

    [[nodiscard]] StatusResult addExecutable(
        const std::string& path,
        const std::vector<long long>& image,
        const vm::ExecutableImageHeader& header) {

        if (!status_.ok()) return status_;
        if (!vm::validateExecutableHeader(header)) return StatusResult::error(ERR_INVALID);
        StatusResult parents = ensureParentDirectories(path);
        if (!parents.ok()) return parents;
        StatusResult found = fs_.lookup(path);
        if (!found.ok()) {
            StatusResult created = fs_.createFile(path, InodeKind::Executable, true);
            if (!created.ok()) return created;
        }
        StatusResult wrote = fs_.writeFile(path, image);
        if (!wrote.ok()) return wrote;
        return fs_.markExecutable(path, header);
    }

    [[nodiscard]] StatusResult addUserRecord(
        const std::string& username,
        long long password_hash,
        const std::string& home,
        const std::string& shell) {

        if (!status_.ok()) return status_;
        if (username.empty() || home.empty() || shell.empty()) {
            return StatusResult::error(ERR_INVALID);
        }
        StatusResult layout = installBaseLayout();
        if (!layout.ok()) return layout;
        StatusResult homeDir = mkdir(home);
        if (!homeDir.ok()) return homeDir;

        std::vector<long long> users;
        std::vector<long long> existing;
        StatusResult read = fs_.readFile("/etc/users", existing);
        if (read.ok() || read.status == T1_PENDING) {
            users = existing;
        } else {
            StatusResult created = fs_.createFile("/etc/users", InodeKind::File);
            if (!created.ok() && created.detail != ERR_EXISTS) return created;
        }

        appendStringRecord(users, username);
        users.push_back(password_hash);
        appendStringRecord(users, home);
        appendStringRecord(users, shell);
        return fs_.writeFile("/etc/users", users);
    }

    [[nodiscard]] std::vector<long long> image() {
        (void)fs_.sync();
        return device_.serialize();
    }

private:
    BlockDevice device_;
    TinyFileSystem fs_;
    StatusResult status_ = StatusResult::error(ERR_INVALID);

    [[nodiscard]] StatusResult ensureParentDirectories(const std::string& path) {
        if (path.empty() || path[0] != '/') return StatusResult::error(ERR_INVALID);
        std::string current;
        std::vector<std::string> parts = splitComponents(path);
        if (parts.empty()) return StatusResult::success(0);
        for (std::size_t i = 0; i + 1 < parts.size(); ++i) {
            current += "/";
            current += parts[i];
            StatusResult found = fs_.lookup(current);
            if (!found.ok()) {
                StatusResult made = fs_.createFile(current, InodeKind::Directory);
                if (!made.ok()) return made;
            }
        }
        return StatusResult::success(0);
    }

    [[nodiscard]] static std::vector<std::string> splitComponents(const std::string& path) {
        std::vector<std::string> parts;
        std::string current;
        for (char c : path) {
            if (c == '/') {
                if (!current.empty()) {
                    parts.push_back(current);
                    current.clear();
                }
            } else {
                current.push_back(c);
            }
        }
        if (!current.empty()) parts.push_back(current);
        return parts;
    }

    static void appendStringRecord(std::vector<long long>& out, const std::string& text) {
        out.push_back(static_cast<long long>(text.size()));
        for (char c : text) {
            out.push_back(static_cast<long long>(c));
        }
    }
};

// =============================================================================
// Native kernel VFS image builder
// =============================================================================

class NativeVfsImageBuilder {
public:
    explicit NativeVfsImageBuilder(int blocks = NATIVE_VFS_REQUIRED_BLOCKS)
        : device_(std::max(blocks, NATIVE_VFS_REQUIRED_BLOCKS)),
          inodes_(NATIVE_VFS_MAX_INODES * NATIVE_VFS_INODE_WORDS, 0),
          dirents_(NATIVE_VFS_MAX_DIRENTS * NATIVE_VFS_DIRENT_WORDS, 0),
          names_(NATIVE_VFS_MAX_DIRENTS * NATIVE_VFS_MAX_NAME_WORDS, 0),
          extents_(NATIVE_VFS_MAX_EXTENTS * NATIVE_VFS_EXTENT_WORDS, 0),
          data_(NATIVE_VFS_PAYLOAD_WORDS, 0) {
        status_ = format();
    }

    [[nodiscard]] StatusResult status() const { return status_; }

    [[nodiscard]] StatusResult installBaseLayout() {
        for (const std::string& dir : {"/bin", "/apps", "/etc", "/home", "/tmp",
                                       "/var", "/dev", "/system", "/lib"}) {
            StatusResult made = mkdir(dir);
            if (!made.ok()) return made;
        }
        StatusResult log = mkdir("/var/log");
        if (!log.ok()) return log;
        StatusResult crash = mkdir("/var/crash");
        if (!crash.ok()) return crash;
        StatusResult packages = mkdir("/var/packages");
        if (!packages.ok()) return packages;
        return mkdir("/system/services");
    }

    [[nodiscard]] StatusResult mkdir(const std::string& path) {
        if (!status_.ok()) return status_;
        const std::string normalized = normalizePath(path);
        if (normalized.empty()) return StatusResult::error(ERR_INVALID);
        auto found = path_to_inode_.find(normalized);
        if (found != path_to_inode_.end()) {
            return inodeKind(found->second) == NATIVE_KIND_DIR
                       ? StatusResult::success(found->second)
                       : StatusResult::error(ERR_EXISTS);
        }
        StatusResult parents = ensureParentDirectories(normalized);
        if (!parents.ok()) return parents;
        return createNode(normalized, NATIVE_KIND_DIR);
    }

    [[nodiscard]] StatusResult addFile(
        const std::string& path,
        const std::vector<long long>& words) {

        if (!status_.ok()) return status_;
        StatusResult inode = createOrLookupFile(path, NATIVE_KIND_FILE);
        if (!inode.ok()) return inode;
        return writePayload(inode.payload, words);
    }

    [[nodiscard]] StatusResult addExecutableDescriptor(
        const std::string& path,
        const vm::ExecutableImageHeader& header,
        int text_ppn) {

        if (!status_.ok()) return status_;
        if (!vm::validateExecutableHeader(header) || text_ppn <= 0) {
            return StatusResult::error(ERR_INVALID);
        }
        StatusResult inode = createOrLookupFile(path, NATIVE_KIND_EXEC);
        if (!inode.ok()) return inode;
        return writePayload(inode.payload, executableDescriptor(header, text_ppn));
    }

    [[nodiscard]] StatusResult addExecutableImage(
        const std::string& path,
        const std::vector<isa::TritWord27>& program,
        const vm::ExecutableImageHeader& header,
        int text_ppn) {

        if (!status_.ok()) return status_;
        if (!vm::validateExecutableHeader(header) || text_ppn <= 0 || program.empty()) {
            return StatusResult::error(ERR_INVALID);
        }
        const int text_capacity = header.text_pages * BLOCK_WORDS;
        if (static_cast<int>(program.size()) > text_capacity) {
            return StatusResult::error(ERR_NO_SPACE);
        }
        const int blocks = static_cast<int>((program.size() + BLOCK_WORDS - 1) / BLOCK_WORDS);
        if (next_text_block_ + blocks > device_.blockCount()) {
            return StatusResult::error(ERR_NO_SPACE);
        }
        const int first_text_block = next_text_block_;
        for (int block = 0; block < blocks; ++block) {
            std::vector<long long> out(BLOCK_WORDS, 0);
            for (int word = 0; word < BLOCK_WORDS; ++word) {
                const int index = block * BLOCK_WORDS + word;
                if (index < static_cast<int>(program.size())) {
                    out[static_cast<std::size_t>(word)] =
                        static_cast<long long>(program[static_cast<std::size_t>(index)].bits);
                }
            }
            StatusResult wrote = device_.writeBlock(first_text_block + block, out);
            if (!wrote.ok()) return wrote;
        }
        next_text_block_ += blocks;
        StatusResult inode = createOrLookupFile(path, NATIVE_KIND_EXEC);
        if (!inode.ok()) return inode;
        return writePayload(inode.payload,
                            executableDescriptor(header,
                                                 text_ppn,
                                                 first_text_block,
                                                 static_cast<int>(program.size())));
    }

    [[nodiscard]] StatusResult addUserRecord(
        const std::string& username,
        long long password_hash,
        const std::string& home,
        const std::string& shell) {

        if (username.empty() || home.empty() || shell.empty()) {
            return StatusResult::error(ERR_INVALID);
        }
        StatusResult layout = installBaseLayout();
        if (!layout.ok()) return layout;
        StatusResult homeDir = mkdir(home);
        if (!homeDir.ok()) return homeDir;

        std::vector<long long> users = file_payloads_["/etc/users"];
        appendStringRecord(users, username);
        users.push_back(password_hash);
        appendStringRecord(users, home);
        appendStringRecord(users, shell);
        StatusResult wrote = addFile("/etc/users", users);
        if (wrote.ok()) file_payloads_["/etc/users"] = users;
        return wrote;
    }

    [[nodiscard]] std::vector<long long> image() {
        (void)sync();
        return device_.serialize();
    }

    [[nodiscard]] StatusResult sync() {
        if (!status_.ok()) return status_;
        StatusResult super = writeSuperBlock();
        if (!super.ok()) return super;
        StatusResult wrote = writeRange(NATIVE_VFS_DISK_INODE_BLOCK,
                                        NATIVE_VFS_DISK_INODE_BLOCKS,
                                        inodes_);
        if (!wrote.ok()) return wrote;
        wrote = writeRange(NATIVE_VFS_DISK_DIRENT_BLOCK,
                           NATIVE_VFS_DISK_DIRENT_BLOCKS,
                           dirents_);
        if (!wrote.ok()) return wrote;
        wrote = writeRange(NATIVE_VFS_DISK_DIRENT_NAME_BLOCK,
                           NATIVE_VFS_DISK_DIRENT_NAME_BLOCKS,
                           names_);
        if (!wrote.ok()) return wrote;
        wrote = writeRange(NATIVE_VFS_DISK_EXTENT_BLOCK,
                           NATIVE_VFS_DISK_EXTENT_BLOCKS,
                           extents_);
        if (!wrote.ok()) return wrote;
        wrote = writeRange(NATIVE_VFS_DISK_DATA_BLOCK,
                           NATIVE_VFS_DISK_DATA_BLOCKS,
                           data_);
        if (!wrote.ok()) return wrote;
        wrote = writeZeroBlocks(NATIVE_WAL_DISK_META_BLOCK, 1);
        if (!wrote.ok()) return wrote;
        return writeZeroBlocks(NATIVE_WAL_DISK_RECORD_BLOCK,
                               NATIVE_WAL_DISK_RECORD_BLOCKS);
    }

private:
    BlockDevice device_;
    std::vector<long long> inodes_;
    std::vector<long long> dirents_;
    std::vector<long long> names_;
    std::vector<long long> extents_;
    std::vector<long long> data_;
    std::map<std::string, int> path_to_inode_;
    std::map<std::string, std::vector<long long>> file_payloads_;
    int next_inode_ = 1;
    int next_dirent_ = 0;
    int next_extent_ = 0;
    int next_data_offset_ = 0;
    int next_text_block_ = NATIVE_VFS_REQUIRED_BLOCKS;
    StatusResult status_ = StatusResult::error(ERR_INVALID);

    [[nodiscard]] StatusResult format() {
        std::fill(inodes_.begin(), inodes_.end(), 0);
        std::fill(dirents_.begin(), dirents_.end(), 0);
        std::fill(names_.begin(), names_.end(), 0);
        std::fill(extents_.begin(), extents_.end(), 0);
        std::fill(data_.begin(), data_.end(), 0);
        path_to_inode_.clear();
        file_payloads_.clear();
        next_inode_ = 1;
        next_dirent_ = 0;
        next_extent_ = 0;
        next_data_offset_ = 0;
        next_text_block_ = NATIVE_VFS_REQUIRED_BLOCKS;
        path_to_inode_["/"] = 0;
        setInode(0, NATIVE_KIND_DIR, 0, 0);
        return StatusResult::success();
    }

    [[nodiscard]] int inodeBase(int inode) const {
        return inode * NATIVE_VFS_INODE_WORDS;
    }

    [[nodiscard]] int direntBase(int slot) const {
        return slot * NATIVE_VFS_DIRENT_WORDS;
    }

    [[nodiscard]] int nameBase(int slot) const {
        return slot * NATIVE_VFS_MAX_NAME_WORDS;
    }

    [[nodiscard]] int extentBase(int slot) const {
        return slot * NATIVE_VFS_EXTENT_WORDS;
    }

    [[nodiscard]] int inodeKind(int inode) const {
        if (inode < 0 || inode >= NATIVE_VFS_MAX_INODES) return 0;
        return static_cast<int>(inodes_[static_cast<std::size_t>(inodeBase(inode))]);
    }

    void setInode(int inode, int kind, int size_words, int parent) {
        const int base = inodeBase(inode);
        inodes_[static_cast<std::size_t>(base + 0)] = kind;
        inodes_[static_cast<std::size_t>(base + 1)] = 1;
        inodes_[static_cast<std::size_t>(base + 2)] = size_words;
        inodes_[static_cast<std::size_t>(base + 3)] = 1;
        inodes_[static_cast<std::size_t>(base + 4)] = 1;
        inodes_[static_cast<std::size_t>(base + 5)] = 0;
        inodes_[static_cast<std::size_t>(base + 6)] = 0;
        inodes_[static_cast<std::size_t>(base + 7)] = parent;
    }

    [[nodiscard]] StatusResult createNode(const std::string& path, int kind) {
        const std::string normalized = normalizePath(path);
        if (normalized.empty() || normalized == "/") return StatusResult::error(ERR_INVALID);
        if (path_to_inode_.find(normalized) != path_to_inode_.end()) {
            return StatusResult::error(ERR_EXISTS);
        }
        if (next_inode_ >= NATIVE_VFS_MAX_INODES ||
            next_dirent_ >= NATIVE_VFS_MAX_DIRENTS) {
            return StatusResult::error(ERR_NO_SPACE);
        }
        const std::string parent_path = parentPath(normalized);
        auto parent = path_to_inode_.find(parent_path);
        if (parent == path_to_inode_.end()) return StatusResult::error(ERR_NOT_DIR);
        if (inodeKind(parent->second) != NATIVE_KIND_DIR) {
            return StatusResult::error(ERR_NOT_DIR);
        }
        const std::string leaf = leafName(normalized);
        if (leaf.empty() || static_cast<int>(leaf.size()) > NATIVE_VFS_MAX_NAME_WORDS) {
            return StatusResult::error(ERR_INVALID);
        }

        const int inode = next_inode_++;
        setInode(inode, kind, 0, parent->second);
        writeDirent(next_dirent_++, parent->second, inode, leaf);
        path_to_inode_[normalized] = inode;
        return StatusResult::success(inode);
    }

    [[nodiscard]] StatusResult createOrLookupFile(const std::string& path, int kind) {
        const std::string normalized = normalizePath(path);
        if (normalized.empty() || normalized == "/") return StatusResult::error(ERR_INVALID);
        StatusResult parents = ensureParentDirectories(normalized);
        if (!parents.ok()) return parents;
        auto found = path_to_inode_.find(normalized);
        if (found != path_to_inode_.end()) {
            int existing_kind = inodeKind(found->second);
            if (existing_kind == NATIVE_KIND_DIR) return StatusResult::error(ERR_IS_DIR);
            if (existing_kind != kind) {
                setInode(found->second, kind, 0, parentInode(normalized));
            }
            return StatusResult::success(found->second);
        }
        return createNode(normalized, kind);
    }

    [[nodiscard]] StatusResult writePayload(int inode, const std::vector<long long>& words) {
        if (inode <= 0 || inode >= NATIVE_VFS_MAX_INODES) {
            return StatusResult::error(ERR_INVALID);
        }
        if (static_cast<int>(words.size()) > NATIVE_VFS_PAYLOAD_WORDS - next_data_offset_) {
            return StatusResult::error(ERR_NO_SPACE);
        }
        for (int slot = 0; slot < next_extent_; ++slot) {
            const int base = extentBase(slot);
            if (extents_[static_cast<std::size_t>(base + 1)] == inode &&
                extents_[static_cast<std::size_t>(base + 5)] > 0) {
                extents_[static_cast<std::size_t>(base + 3)] = 0;
                extents_[static_cast<std::size_t>(base + 5)] = 0;
            }
        }
        setInode(inode, inodeKind(inode), static_cast<int>(words.size()), parentOf(inode));
        if (words.empty()) return StatusResult::success(0);
        if (next_extent_ >= NATIVE_VFS_MAX_EXTENTS) {
            return StatusResult::error(ERR_NO_SPACE);
        }
        const int data_addr = NATIVE_VFS_DATA_BASE + next_data_offset_;
        const int extent = next_extent_++;
        const int base = extentBase(extent);
        extents_[static_cast<std::size_t>(base + 0)] = 0;
        extents_[static_cast<std::size_t>(base + 1)] = inode;
        extents_[static_cast<std::size_t>(base + 2)] = 0;
        extents_[static_cast<std::size_t>(base + 3)] = static_cast<long long>(words.size());
        extents_[static_cast<std::size_t>(base + 4)] = data_addr;
        extents_[static_cast<std::size_t>(base + 5)] = 1;
        for (std::size_t i = 0; i < words.size(); ++i) {
            data_[static_cast<std::size_t>(next_data_offset_) + i] = words[i];
        }
        next_data_offset_ += static_cast<int>(words.size());
        return StatusResult::success(static_cast<int>(words.size()));
    }

    void writeDirent(int slot, int parent, int child, const std::string& name) {
        const int base = direntBase(slot);
        dirents_[static_cast<std::size_t>(base + 0)] = 0;
        dirents_[static_cast<std::size_t>(base + 1)] = parent;
        dirents_[static_cast<std::size_t>(base + 2)] = componentHash(name);
        dirents_[static_cast<std::size_t>(base + 3)] = static_cast<long long>(name.size());
        dirents_[static_cast<std::size_t>(base + 4)] = child;
        dirents_[static_cast<std::size_t>(base + 5)] = 1;
        const int nbase = nameBase(slot);
        for (std::size_t i = 0; i < name.size(); ++i) {
            names_[static_cast<std::size_t>(nbase) + i] =
                static_cast<unsigned char>(name[i]);
        }
    }

    [[nodiscard]] StatusResult writeSuperBlock() {
        std::vector<long long> block(BLOCK_WORDS, 0);
        block[0] = NATIVE_VFS_MAGIC;
        block[1] = NATIVE_VFS_VERSION;
        block[2] = BLOCK_WORDS;
        block[3] = NATIVE_VFS_MAX_INODES;
        block[4] = NATIVE_VFS_MAX_DIRENTS;
        block[5] = NATIVE_VFS_MAX_EXTENTS;
        block[6] = NATIVE_VFS_PAYLOAD_WORDS;
        block[7] = NATIVE_KERNEL_MAGIC;
        block[8] = next_inode_;
        block[9] = next_dirent_;
        block[10] = next_extent_;
        block[11] = NATIVE_VFS_DATA_BASE + next_data_offset_;
        return device_.writeBlock(NATIVE_VFS_DISK_SUPER_BLOCK, block);
    }

    [[nodiscard]] StatusResult writeRange(
        int first_block,
        int block_count,
        const std::vector<long long>& words) {

        int copied = 0;
        for (int block = 0; block < block_count; ++block) {
            std::vector<long long> out(BLOCK_WORDS, 0);
            for (int word = 0; word < BLOCK_WORDS; ++word) {
                if (copied < static_cast<int>(words.size())) {
                    out[static_cast<std::size_t>(word)] =
                        words[static_cast<std::size_t>(copied++)];
                }
            }
            StatusResult wrote = device_.writeBlock(first_block + block, out);
            if (!wrote.ok()) return wrote;
        }
        return StatusResult::success(copied);
    }

    [[nodiscard]] StatusResult writeZeroBlocks(int first_block, int block_count) {
        const std::vector<long long> zero(BLOCK_WORDS, 0);
        for (int block = 0; block < block_count; ++block) {
            StatusResult wrote = device_.writeBlock(first_block + block, zero);
            if (!wrote.ok()) return wrote;
        }
        return StatusResult::success(block_count);
    }

    [[nodiscard]] StatusResult ensureParentDirectories(const std::string& path) {
        std::string current;
        const std::vector<std::string> parts = splitComponents(path);
        if (parts.empty()) return StatusResult::success(0);
        for (std::size_t i = 0; i + 1 < parts.size(); ++i) {
            current += "/";
            current += parts[i];
            auto found = path_to_inode_.find(current);
            if (found == path_to_inode_.end()) {
                StatusResult made = createNode(current, NATIVE_KIND_DIR);
                if (!made.ok()) return made;
            } else if (inodeKind(found->second) != NATIVE_KIND_DIR) {
                return StatusResult::error(ERR_NOT_DIR);
            }
        }
        return StatusResult::success(0);
    }

    [[nodiscard]] int parentInode(const std::string& path) const {
        auto found = path_to_inode_.find(parentPath(path));
        return found == path_to_inode_.end() ? 0 : found->second;
    }

    [[nodiscard]] int parentOf(int inode) const {
        if (inode < 0 || inode >= NATIVE_VFS_MAX_INODES) return 0;
        return static_cast<int>(inodes_[static_cast<std::size_t>(inodeBase(inode) + 7)]);
    }

    [[nodiscard]] static std::vector<long long> executableDescriptor(
        const vm::ExecutableImageHeader& header,
        int text_ppn) {

        return {
            vm::EXEC_MAGIC,
            header.version,
            header.abi_version,
            header.entry_virtual_pc,
            header.text_pages,
            header.data_pages,
            header.stack_words,
            header.syscall_abi_version,
            header.flags,
            text_ppn,
        };
    }

    [[nodiscard]] static std::vector<long long> executableDescriptor(
        const vm::ExecutableImageHeader& header,
        int text_ppn,
        int text_disk_block,
        int text_words) {

        std::vector<long long> descriptor = executableDescriptor(header, text_ppn);
        descriptor.push_back(text_disk_block);
        descriptor.push_back(text_words);
        return descriptor;
    }

    [[nodiscard]] static long long componentHash(const std::string& name) {
        vm::TernaryValue h = vm::ops::fromLong(17);
        const vm::TernaryValue base = vm::ops::fromLong(31);
        for (unsigned char c : name) {
            h = vm::exec::addValue(
                vm::exec::multiplyValue(h, base, TernaryMode::T40),
                vm::ops::fromLong(static_cast<long long>(c)),
                TernaryMode::T40);
        }
        return vm::ops::toLong(h);
    }

    [[nodiscard]] static std::string normalizePath(const std::string& path) {
        if (path.empty() || path[0] != '/') return {};
        std::string out;
        bool prev_slash = false;
        for (char c : path) {
            if (c == '/') {
                if (!prev_slash) out.push_back(c);
                prev_slash = true;
            } else {
                out.push_back(c);
                prev_slash = false;
            }
        }
        while (out.size() > 1 && out.back() == '/') out.pop_back();
        return out.empty() ? "/" : out;
    }

    [[nodiscard]] static std::string parentPath(const std::string& path) {
        const std::size_t slash = path.find_last_of('/');
        if (slash == std::string::npos || slash == 0) return "/";
        return path.substr(0, slash);
    }

    [[nodiscard]] static std::string leafName(const std::string& path) {
        const std::size_t slash = path.find_last_of('/');
        if (slash == std::string::npos) return path;
        return path.substr(slash + 1);
    }

    [[nodiscard]] static std::vector<std::string> splitComponents(const std::string& path) {
        std::vector<std::string> parts;
        std::string current;
        for (char c : path) {
            if (c == '/') {
                if (!current.empty()) {
                    parts.push_back(current);
                    current.clear();
                }
            } else {
                current.push_back(c);
            }
        }
        if (!current.empty()) parts.push_back(current);
        return parts;
    }

    static void appendStringRecord(std::vector<long long>& out, const std::string& text) {
        out.push_back(static_cast<long long>(text.size()));
        for (unsigned char c : text) {
            out.push_back(static_cast<long long>(c));
        }
    }
};

class ReleaseImageBuilder {
public:
    ReleaseImageBuilder(std::string name, int update_epoch, int blocks = 192)
        : builder_(blocks),
          release_name_(std::move(name)),
          update_epoch_(update_epoch),
          status_(builder_.status()) {}

    [[nodiscard]] StatusResult status() const { return status_; }

    [[nodiscard]] StatusResult installBaseLayout() {
        if (!status_.ok()) return status_;
        StatusResult layout = builder_.installBaseLayout();
        if (!layout.ok()) return layout;
        layout = builder_.mkdir("/var/packages");
        if (!layout.ok()) return layout;
        return builder_.addFile("/etc/release", encodeReleaseManifest());
    }

    [[nodiscard]] StatusResult addPackage(const PackageImage& package) {
        if (!status_.ok()) return status_;
        if (package.magic != PACKAGE_MAGIC ||
            package.version != PACKAGE_FORMAT_VERSION ||
            package.name.empty()) {
            return StatusResult::error(ERR_INVALID);
        }
        StatusResult layout = installBaseLayout();
        if (!layout.ok()) return layout;

        packages_.push_back(package);
        StatusResult manifest = builder_.addFile(
            "/var/packages/" + package.name + ".manifest",
            encodePackageManifest(package));
        if (!manifest.ok()) return manifest;

        for (const PackageEntry& entry : package.entries) {
            StatusResult wrote = builder_.addFile(entry.path, entry.words);
            if (!wrote.ok()) return wrote;
            if (entry.executable && entry.metadata.present) {
                wrote = builder_.addFile(entry.path + ".sig",
                                         encodeSignedExecutableMetadata(entry.metadata));
                if (!wrote.ok()) return wrote;
            }
        }
        return builder_.addFile("/etc/release", encodeReleaseManifest());
    }

    [[nodiscard]] std::vector<long long> image() {
        (void)builder_.addFile("/etc/release", encodeReleaseManifest());
        return builder_.image();
    }

private:
    NativeVfsImageBuilder builder_;
    std::string release_name_;
    int update_epoch_ = 0;
    std::vector<PackageImage> packages_;
    StatusResult status_ = StatusResult::error(ERR_INVALID);

    [[nodiscard]] std::vector<long long> encodeReleaseManifest() const {
        std::vector<long long> out = {
            RELEASE_IMAGE_MAGIC,
            PACKAGE_FORMAT_VERSION,
            update_epoch_,
            static_cast<long long>(packages_.size()),
        };
        appendStringWords(out, release_name_);
        for (const PackageImage& package : packages_) {
            appendStringWords(out, package.name);
            out.push_back(package.update_epoch);
            out.push_back(static_cast<long long>(package.entries.size()));
        }
        return out;
    }
};

// =============================================================================
// Process, heap, and syscall facade
// =============================================================================

struct OpenFile {
    int inode = -1;
    int offset = 0;
    bool writable = false;
};

struct Process {
    int pid = -1;
    int parent_pid = -1;
    int state = vm::PROC_STATE_FREE;
    int exit_status = 0;
    int pending_signals = 0;
    int capabilities = CAP_ALL;
    int heap_start = 0;
    int heap_break = 0;
    int heap_limit = 0;
    int fork_return_payload = -1;
    vm::ExecutableImageHeader exec_header;
    std::vector<long long> memory;
    std::map<int, OpenFile> fds;
};

struct ProcessInfo {
    int pid = -1;
    int state = vm::PROC_STATE_FREE;
    int parent_pid = -1;
    int exit_status = 0;
    int pending_signals = 0;
    int capabilities = CAP_ALL;
    int open_fds = 0;
    int memory_words = 0;
};

struct WindowRecord {
    int id = -1;
    int owner_pid = -1;
    int width = 0;
    int height = 0;
};

struct IpcMessage {
    int from_pid = -1;
    int to_pid = -1;
    long long payload = 0;
};

struct ProcessIsolationReport {
    std::vector<std::string> errors;
    int live_processes = 0;
    int open_fds = 0;
    int windows = 0;
    int ipc_messages = 0;

    [[nodiscard]] bool ok() const { return errors.empty(); }
    [[nodiscard]] StatusResult status() const {
        return ok() ? StatusResult::success(live_processes)
                    : StatusResult::error(ERR_CORRUPT, static_cast<int>(errors.size()));
    }
    void fail(const std::string& message) { errors.push_back(message); }
};

class OSKernel {
public:
    explicit OSKernel(int blocks = 128)
        : profile_(toyProfile(blocks)),
          device_tree_(defaultDeviceTree(blocks)),
          block_device_(blocks) {
        (void)fs_.format(block_device_);
        (void)createProcess(-1);
    }

    explicit OSKernel(const ProductionProfile& profile, const std::string& disk_path = {})
        : profile_(profile),
          device_tree_(defaultDeviceTree(profile.disk_blocks)),
          block_device_(profile.disk_blocks) {
        if (!disk_path.empty()) (void)block_device_.attachBackingFile(disk_path);
        (void)fs_.format(block_device_, std::max(DEFAULT_INODE_COUNT, profile_.max_files + 8));
        (void)createProcess(-1);
    }

    explicit OSKernel(const std::vector<long long>& disk_image)
        : profile_(toyProfile(blockCountFromImage(disk_image))),
          device_tree_(defaultDeviceTree(blockCountFromImage(disk_image))),
          block_device_(blockCountFromImage(disk_image)) {
        (void)block_device_.loadSerialized(disk_image);
        (void)createProcess(-1);
    }

    [[nodiscard]] DeviceTree& deviceTree() { return device_tree_; }
    [[nodiscard]] BlockDevice& blockDevice() { return block_device_; }
    [[nodiscard]] TinyFileSystem& fs() { return fs_; }
    [[nodiscard]] const TinyFileSystem& fs() const { return fs_; }
    [[nodiscard]] const ProductionProfile& productionProfile() const { return profile_; }
    [[nodiscard]] int processCount() const { return static_cast<int>(processes_.size()); }
    [[nodiscard]] int windowCount() const { return static_cast<int>(windows_.size()); }
    [[nodiscard]] int ipcMessageCount() const { return static_cast<int>(ipc_messages_.size()); }

    [[nodiscard]] std::vector<long long> diskImage() const {
        return block_device_.serialize();
    }

    [[nodiscard]] FsConsistencyReport checkFilesystemConsistency() const {
        return fs_.checkConsistency();
    }

    [[nodiscard]] StatusResult shutdownSync() {
        return fs_.sync();
    }

    [[nodiscard]] StatusResult boot() {
        std::vector<std::string> errors;
        StatusResult dt = device_tree_.validate(&errors);
        if (!dt.ok()) return dt;
        return fs_.mount(block_device_);
    }

    [[nodiscard]] BootRecoveryReport bootWithRecovery() {
        BootRecoveryReport report;
        StatusResult booted = boot();
        if (booted.ok()) {
            report.fsck = fs_.checkConsistency();
            if (report.fsck.ok()) {
                report.mode = BootMode::Normal;
                report.status = StatusResult::success();
                return report;
            }
            report.reason = "filesystem consistency check failed";
        } else {
            report.reason = "normal boot failed";
        }

        report.mode = BootMode::Recovery;
        StatusResult formatted = fs_.format(block_device_,
                                            std::max(DEFAULT_INODE_COUNT,
                                                     profile_.max_files + 8));
        if (!formatted.ok()) {
            report.status = formatted;
            return report;
        }
        (void)fs_.createFile("/var", InodeKind::Directory);
        (void)fs_.createFile("/var/log", InodeKind::Directory);
        StatusResult log = fs_.createFile("/var/log/recovery", InodeKind::File);
        if (!log.ok() && log.detail != ERR_EXISTS) {
            report.status = log;
            return report;
        }
        std::vector<long long> reason_words;
        appendStringWords(reason_words, report.reason);
        StatusResult wrote = fs_.writeFile("/var/log/recovery", reason_words);
        if (!wrote.ok()) {
            report.status = wrote;
            return report;
        }
        report.fsck = fs_.checkConsistency();
        report.status = report.fsck.status();
        return report;
    }

    [[nodiscard]] Process* process(int pid) {
        for (auto& proc : processes_) {
            if (proc.pid == pid) return &proc;
        }
        return nullptr;
    }

    [[nodiscard]] const Process* process(int pid) const {
        for (const auto& proc : processes_) {
            if (proc.pid == pid) return &proc;
        }
        return nullptr;
    }

    [[nodiscard]] StatusResult setProcessCapabilities(int pid, int capabilities) {
        Process* proc = process(pid);
        if (!proc) return StatusResult::error(ERR_INVALID);
        proc->capabilities = capabilities & CAP_ALL;
        return StatusResult::success(proc->capabilities);
    }

    [[nodiscard]] StatusResult capabilityCheck(int pid, int capability) const {
        const Process* proc = process(pid);
        if (!proc) return StatusResult::error(ERR_INVALID);
        return hasCapability(*proc, capability)
                   ? StatusResult::success(capability)
                   : StatusResult::error(ERR_ACCESS, capability);
    }

    [[nodiscard]] ProcessIsolationReport checkProcessIsolation() const {
        ProcessIsolationReport report;
        std::set<int> live_pids;
        for (const Process& proc : processes_) {
            if (proc.pid < 0 || proc.state == vm::PROC_STATE_FREE) continue;
            ++report.live_processes;
            if (!live_pids.insert(proc.pid).second) {
                report.fail("duplicate live pid: " + std::to_string(proc.pid));
            }
            if (proc.heap_break < proc.heap_start || proc.heap_break > proc.heap_limit) {
                report.fail("process heap bounds are inconsistent: " + std::to_string(proc.pid));
            }
            for (const auto& [fd, open] : proc.fds) {
                (void)fd;
                ++report.open_fds;
                if (!fs_.inode(open.inode)) {
                    report.fail("process fd points at an invalid inode: " + std::to_string(proc.pid));
                }
            }
        }
        for (const WindowRecord& window : windows_) {
            ++report.windows;
            if (live_pids.count(window.owner_pid) == 0) {
                report.fail("window is owned by a non-live process: " +
                            std::to_string(window.owner_pid));
            }
        }
        for (const IpcMessage& message : ipc_messages_) {
            ++report.ipc_messages;
            if (live_pids.count(message.from_pid) == 0 ||
                live_pids.count(message.to_pid) == 0) {
                report.fail("IPC message references a non-live process");
            }
        }
        return report;
    }

    [[nodiscard]] StatusResult sysOpen(int pid, const std::string& path, bool writable = false) {
        Process* proc = process(pid);
        if (!proc) return StatusResult::error(ERR_INVALID);
        if (!hasCapability(*proc, CAP_FILE_READ) ||
            (writable && !hasCapability(*proc, CAP_FILE_WRITE))) {
            return StatusResult::error(ERR_ACCESS);
        }
        StatusResult found = fs_.lookup(path);
        if (!found.ok()) return found;
        int fd = nextFd(*proc);
        proc->fds[fd] = OpenFile{found.payload, 0, writable};
        return StatusResult::success(fd);
    }

    [[nodiscard]] StatusResult sysClose(int pid, int fd) {
        Process* proc = process(pid);
        if (!proc) return StatusResult::error(ERR_INVALID);
        auto it = proc->fds.find(fd);
        if (it == proc->fds.end()) return StatusResult::error(ERR_BAD_FD);
        proc->fds.erase(it);
        return StatusResult::success();
    }

    [[nodiscard]] StatusResult sysRead(int pid, int fd, int count, std::vector<long long>& out) {
        Process* proc = process(pid);
        if (!proc) return StatusResult::error(ERR_INVALID);
        if (!hasCapability(*proc, CAP_FILE_READ)) return StatusResult::error(ERR_ACCESS);
        auto fdIt = proc->fds.find(fd);
        if (fdIt == proc->fds.end()) return StatusResult::error(ERR_BAD_FD);
        const Inode* inode = fs_.inode(fdIt->second.inode);
        if (!inode) return StatusResult::error(ERR_NOT_FOUND);
        if (inode->kind == InodeKind::Directory) return StatusResult::error(ERR_IS_DIR);
        if (fdIt->second.offset >= inode->size_words) {
            out.clear();
            return StatusResult::pending(0, ERR_EOF);
        }
        const int take = std::min(count, inode->size_words - fdIt->second.offset);
        out.assign(inode->data.begin() + fdIt->second.offset,
                   inode->data.begin() + fdIt->second.offset + take);
        fdIt->second.offset += take;
        return StatusResult::success(take);
    }

    [[nodiscard]] StatusResult sysWrite(int pid, int fd, const std::vector<long long>& words) {
        Process* proc = process(pid);
        if (!proc) return StatusResult::error(ERR_INVALID);
        if (!hasCapability(*proc, CAP_FILE_WRITE)) return StatusResult::error(ERR_ACCESS);
        auto fdIt = proc->fds.find(fd);
        if (fdIt == proc->fds.end()) return StatusResult::error(ERR_BAD_FD);
        if (!fdIt->second.writable) return StatusResult::error(ERR_INVALID);
        const Inode* inode = fs_.inode(fdIt->second.inode);
        if (!inode) return StatusResult::error(ERR_NOT_FOUND);
        std::vector<long long> merged = inode->data;
        if (fdIt->second.offset > static_cast<int>(merged.size())) {
            merged.resize(static_cast<std::size_t>(fdIt->second.offset), 0);
        }
        if (fdIt->second.offset + static_cast<int>(words.size()) >
            static_cast<int>(merged.size())) {
            merged.resize(static_cast<std::size_t>(fdIt->second.offset + words.size()), 0);
        }
        std::copy(words.begin(), words.end(), merged.begin() + fdIt->second.offset);
        StatusResult wrote = fs_.writeInode(fdIt->second.inode, merged);
        if (wrote.ok()) fdIt->second.offset += static_cast<int>(words.size());
        return wrote.ok() ? StatusResult::success(static_cast<int>(words.size())) : wrote;
    }

    [[nodiscard]] StatusResult sysStat(const std::string& path, FileStat& out) const {
        return fs_.stat(path, out);
    }

    [[nodiscard]] StatusResult sysReadDir(const std::string& path, std::vector<DirectoryEntry>& out) const {
        return fs_.readdir(path, out);
    }

    [[nodiscard]] StatusResult sysBrk(int pid, int new_break) {
        Process* proc = process(pid);
        if (!proc) return StatusResult::error(ERR_INVALID);
        if (new_break < proc->heap_start || new_break > proc->heap_limit) {
            return StatusResult::error(ERR_NO_SPACE, proc->heap_break);
        }
        proc->heap_break = new_break;
        if (static_cast<int>(proc->memory.size()) < new_break) {
            proc->memory.resize(static_cast<std::size_t>(new_break), 0);
        }
        return StatusResult::success(proc->heap_break);
    }

    [[nodiscard]] StatusResult sysSbrk(int pid, int delta) {
        Process* proc = process(pid);
        if (!proc) return StatusResult::error(ERR_INVALID);
        return sysBrk(pid, proc->heap_break + delta);
    }

    [[nodiscard]] StatusResult sysFork(int pid) {
        const Process* parent = process(pid);
        if (!parent) return StatusResult::error(ERR_INVALID);
        if (static_cast<int>(processes_.size()) >= profile_.max_processes) {
            return StatusResult::error(ERR_NO_SPACE);
        }
        Process child = *parent;
        child.pid = next_pid_++;
        child.parent_pid = parent->pid;
        child.fork_return_payload = 0;
        child.fds = parent->fds;
        child.state = vm::PROC_STATE_RUNNABLE;
        processes_.push_back(child);
        return StatusResult::success(child.pid);
    }

    [[nodiscard]] StatusResult sysExec(int pid, const std::string& path) {
        Process* proc = process(pid);
        if (!proc) return StatusResult::error(ERR_INVALID);
        StatusResult found = fs_.lookup(path);
        if (!found.ok()) return found;
        const Inode* inode = fs_.inode(found.payload);
        if (!inode || !inode->executable) return StatusResult::error(ERR_INVALID);
        proc->exec_header = inode->exec_header;
        proc->memory = inode->data;
        proc->heap_start = proc->exec_header.data_pages * vm::MMU_PAGE_WORDS;
        proc->heap_break = proc->heap_start;
        proc->heap_limit = proc->heap_start + proc->exec_header.stack_words + vm::MMU_PAGE_WORDS;
        proc->state = vm::PROC_STATE_RUNNABLE;
        return StatusResult::success(proc->exec_header.entry_virtual_pc);
    }

    [[nodiscard]] StatusResult sysExit(int pid, int status) {
        Process* proc = process(pid);
        if (!proc) return StatusResult::error(ERR_INVALID);
        return finishProcessExit(*proc, status, vm::PROC_STATE_ZOMBIE);
    }

    [[nodiscard]] StatusResult sysWaitPid(int parent_pid, int child_pid) {
        Process* child = process(child_pid);
        if (!child) return StatusResult::error(ERR_NOT_FOUND);
        if (child->parent_pid != parent_pid) return StatusResult::error(ERR_INVALID);
        if (!isTerminalState(child->state)) return StatusResult::pending(0);
        const int status = child->exit_status;
        reapProcess(*child);
        return StatusResult::success(status);
    }

    [[nodiscard]] StatusResult sysKill(int pid, int signal) {
        Process* proc = process(pid);
        if (!proc) return StatusResult::error(ERR_INVALID);
        if (!isSignal(signal)) return StatusResult::error(ERR_INVALID);
        addSignal(*proc, signal);
        if (signal == vm::SIGNAL_KILL) {
            proc->state = vm::PROC_STATE_KILLING;
            return finishProcessExit(*proc, vm::SIGNAL_KILL, vm::PROC_STATE_ZOMBIE);
        }
        if (signal == vm::SIGNAL_STOP) {
            proc->state = vm::PROC_STATE_STOPPED;
            return StatusResult::success(proc->state);
        }
        if (signal == vm::SIGNAL_CONT) {
            proc->pending_signals &= ~vm::SIGNAL_STOP;
            proc->pending_signals &= ~vm::SIGNAL_CONT;
            proc->state = vm::PROC_STATE_RUNNABLE;
            return StatusResult::success(proc->state);
        }
        addSignal(*proc, vm::SIGNAL_CLOSE_REQUEST);
        return StatusResult::success(proc->pending_signals);
    }

    [[nodiscard]] StatusResult sysKillFrom(int caller_pid, int target_pid, int signal) {
        const Process* caller = process(caller_pid);
        if (!caller) return StatusResult::error(ERR_INVALID);
        if (caller_pid != target_pid && !hasCapability(*caller, CAP_PROCESS_CONTROL)) {
            return StatusResult::error(ERR_ACCESS);
        }
        return sysKill(target_pid, signal);
    }

    [[nodiscard]] StatusResult sysSuspend(int pid) {
        return sysKill(pid, vm::SIGNAL_STOP);
    }

    [[nodiscard]] StatusResult sysResume(int pid) {
        return sysKill(pid, vm::SIGNAL_CONT);
    }

    [[nodiscard]] StatusResult sysSuspendFrom(int caller_pid, int target_pid) {
        return sysKillFrom(caller_pid, target_pid, vm::SIGNAL_STOP);
    }

    [[nodiscard]] StatusResult sysResumeFrom(int caller_pid, int target_pid) {
        return sysKillFrom(caller_pid, target_pid, vm::SIGNAL_CONT);
    }

    [[nodiscard]] StatusResult sysIpcSend(int from_pid, int to_pid, long long payload) {
        const Process* sender = process(from_pid);
        if (!sender || !process(to_pid)) return StatusResult::error(ERR_INVALID);
        if (!hasCapability(*sender, CAP_IPC)) return StatusResult::error(ERR_ACCESS);
        ipc_messages_.push_back(IpcMessage{from_pid, to_pid, payload});
        return StatusResult::success(static_cast<int>(ipc_messages_.size()));
    }

    [[nodiscard]] StatusResult sysIpcRecv(int pid, long long& payload) {
        const Process* receiver = process(pid);
        if (!receiver) return StatusResult::error(ERR_INVALID);
        if (!hasCapability(*receiver, CAP_IPC)) return StatusResult::error(ERR_ACCESS);
        for (auto it = ipc_messages_.begin(); it != ipc_messages_.end(); ++it) {
            if (it->to_pid != pid) continue;
            payload = it->payload;
            const int from = it->from_pid;
            ipc_messages_.erase(it);
            return StatusResult::success(from);
        }
        return StatusResult::pending(0, ERR_AGAIN);
    }

    [[nodiscard]] StatusResult sysGetProc(int pid, ProcessInfo& out) const {
        const Process* proc = process(pid);
        if (!proc) return StatusResult::error(ERR_INVALID);
        out.pid = proc->pid;
        out.state = proc->state;
        out.parent_pid = proc->parent_pid;
        out.exit_status = proc->exit_status;
        out.pending_signals = proc->pending_signals;
        out.capabilities = proc->capabilities;
        out.open_fds = static_cast<int>(proc->fds.size());
        out.memory_words = static_cast<int>(proc->memory.size());
        return StatusResult::success(8);
    }

    [[nodiscard]] StatusResult installExecutable(
        const std::string& path,
        const std::vector<long long>& image,
        const vm::ExecutableImageHeader& header) {

        if (!vm::validateExecutableHeader(header)) return StatusResult::error(ERR_INVALID);
        StatusResult found = fs_.lookup(path);
        if (!found.ok()) {
            StatusResult created = fs_.createFile(path, InodeKind::Executable, true);
            if (!created.ok()) return created;
        } else {
            const Inode* inode = fs_.inode(found.payload);
            if (!inode || inode->kind == InodeKind::Directory) {
                return StatusResult::error(ERR_IS_DIR);
            }
        }
        StatusResult wrote = fs_.writeFile(path, image);
        if (!wrote.ok()) return wrote;
        StatusResult marked = fs_.markExecutable(path, header);
        return marked.ok() ? StatusResult::success(static_cast<int>(image.size())) : marked;
    }

    [[nodiscard]] StatusResult mallocWords(int pid, int words, UserPtr<long long>& out) {
        if (words <= 0) {
            out = UserPtr<long long>{0, UserPtrState::Null};
            return StatusResult::error(ERR_INVALID);
        }
        Process* proc = process(pid);
        if (!proc) return StatusResult::error(ERR_INVALID);
        const int base = proc->heap_break;
        StatusResult grown = sysSbrk(pid, words);
        if (!grown.ok()) {
            out = UserPtr<long long>{0, UserPtrState::Null};
            return grown;
        }
        out = UserPtr<long long>{base, UserPtrState::Valid};
        return StatusResult::success(base);
    }

    [[nodiscard]] StatusResult createWindow(int pid, int width, int height) {
        const Process* owner = process(pid);
        if (!owner) return StatusResult::error(ERR_INVALID);
        if (!hasCapability(*owner, CAP_WINDOW)) return StatusResult::error(ERR_ACCESS);
        if (width <= 0 || height <= 0) return StatusResult::error(ERR_INVALID);
        if (static_cast<int>(windows_.size()) >= profile_.max_windows) {
            return StatusResult::error(ERR_NO_SPACE);
        }
        const long long words = static_cast<long long>(width) * static_cast<long long>(height);
        if (words > profile_.framebuffer_words) return StatusResult::error(ERR_NO_SPACE);
        const int id = next_window_id_++;
        windows_.push_back(WindowRecord{id, pid, width, height});
        return StatusResult::success(id);
    }

private:
    ProductionProfile profile_;
    DeviceTree device_tree_;
    BlockDevice block_device_;
    TinyFileSystem fs_;
    std::vector<Process> processes_;
    std::vector<WindowRecord> windows_;
    std::deque<IpcMessage> ipc_messages_;
    int next_pid_ = 1;
    int next_window_id_ = 1;

    [[nodiscard]] static ProductionProfile toyProfile(int blocks) {
        ProductionProfile profile;
        profile.cores = 1;
        profile.ram_words = vm::DEFAULT_DMEM_SIZE;
        profile.instruction_words = vm::DEFAULT_IMEM_SIZE;
        profile.disk_blocks = std::max(1, blocks);
        profile.framebuffer_words = 80 * 60;
        profile.framebuffer_width = 80;
        profile.framebuffer_height = 60;
        profile.max_processes = 128;
        profile.max_files = DEFAULT_INODE_COUNT;
        profile.max_windows = 8;
        return profile;
    }

    [[nodiscard]] static int blockCountFromImage(const std::vector<long long>& image) {
        if (image.empty() || static_cast<int>(image.size()) % BLOCK_WORDS != 0) {
            return 1;
        }
        return std::max(1, static_cast<int>(image.size()) / BLOCK_WORDS);
    }

    [[nodiscard]] static bool hasCapability(const Process& proc, int capability) {
        return (proc.capabilities & capability) == capability;
    }

    [[nodiscard]] int createProcess(int parent) {
        if (static_cast<int>(processes_.size()) >= profile_.max_processes) return -1;
        Process proc;
        proc.pid = next_pid_++;
        proc.parent_pid = parent;
        proc.state = vm::PROC_STATE_RUNNABLE;
        proc.capabilities = CAP_ALL;
        proc.heap_start = vm::MMU_PAGE_WORDS;
        proc.heap_break = proc.heap_start;
        proc.heap_limit = proc.heap_start + 9 * vm::MMU_PAGE_WORDS;
        proc.memory.resize(static_cast<std::size_t>(proc.heap_start), 0);
        processes_.push_back(proc);
        return proc.pid;
    }

    [[nodiscard]] static bool isTerminalState(int state) {
        return state == vm::PROC_STATE_EXITED ||
               state == vm::PROC_STATE_ZOMBIE ||
               state == vm::PROC_STATE_CRASHED;
    }

    [[nodiscard]] static bool isSignal(int signal) {
        return signal == vm::SIGNAL_TERM ||
               signal == vm::SIGNAL_KILL ||
               signal == vm::SIGNAL_STOP ||
               signal == vm::SIGNAL_CONT ||
               signal == vm::SIGNAL_CLOSE_REQUEST;
    }

    static void addSignal(Process& proc, int signal) {
        if ((proc.pending_signals & signal) == 0) {
            proc.pending_signals |= signal;
        }
    }

    void cleanupOrphans(int parent_pid) {
        for (auto& proc : processes_) {
            if (proc.parent_pid != parent_pid) continue;
            if (isTerminalState(proc.state)) {
                reapProcess(proc);
            } else {
                proc.parent_pid = -1;
            }
        }
    }

    void cleanupProcess(Process& proc) {
        proc.fds.clear();
        proc.memory.clear();
        proc.heap_break = proc.heap_start;
        ipc_messages_.erase(
            std::remove_if(ipc_messages_.begin(), ipc_messages_.end(),
                           [&](const IpcMessage& message) {
                               return message.from_pid == proc.pid || message.to_pid == proc.pid;
                           }),
            ipc_messages_.end());
        windows_.erase(
            std::remove_if(windows_.begin(), windows_.end(),
                           [&](const WindowRecord& window) { return window.owner_pid == proc.pid; }),
            windows_.end());
    }

    void reapProcess(Process& proc) {
        cleanupProcess(proc);
        proc.pid = -1;
        proc.parent_pid = -1;
        proc.state = vm::PROC_STATE_FREE;
        proc.exit_status = 0;
        proc.pending_signals = 0;
    }

    [[nodiscard]] StatusResult finishProcessExit(Process& proc, int status, int final_state) {
        const int pid = proc.pid;
        const int parent = proc.parent_pid;
        proc.exit_status = status;
        cleanupProcess(proc);
        cleanupOrphans(pid);
        if (parent > 0 && process(parent)) {
            proc.state = final_state;
            return StatusResult::success(final_state);
        }
        reapProcess(proc);
        return StatusResult::success(vm::PROC_STATE_FREE);
    }

    [[nodiscard]] static int nextFd(const Process& proc) {
        int fd = 3;
        while (proc.fds.count(fd)) ++fd;
        return fd;
    }
};

[[nodiscard]] inline std::vector<std::string> splitPackagePath(const std::string& path) {
    std::vector<std::string> parts;
    std::string current;
    for (char c : path) {
        if (c == '/') {
            if (!current.empty()) {
                parts.push_back(current);
                current.clear();
            }
        } else {
            current.push_back(c);
        }
    }
    if (!current.empty()) parts.push_back(current);
    return parts;
}

[[nodiscard]] inline StatusResult ensureKernelParentDirectories(
    OSKernel& kernel,
    const std::string& path) {

    if (path.empty() || path[0] != '/') return StatusResult::error(ERR_INVALID);
    std::string current;
    const std::vector<std::string> parts = splitPackagePath(path);
    for (std::size_t i = 0; i + 1 < parts.size(); ++i) {
        current += "/";
        current += parts[i];
        StatusResult found = kernel.fs().lookup(current);
        if (found.ok()) continue;
        StatusResult made = kernel.fs().createFile(current, InodeKind::Directory);
        if (!made.ok() && made.detail != ERR_EXISTS) return made;
    }
    return StatusResult::success();
}

[[nodiscard]] inline StatusResult writeKernelFile(
    OSKernel& kernel,
    const std::string& path,
    const std::vector<long long>& words) {

    StatusResult parents = ensureKernelParentDirectories(kernel, path);
    if (!parents.ok()) return parents;
    StatusResult found = kernel.fs().lookup(path);
    if (!found.ok()) {
        StatusResult created = kernel.fs().createFile(path, InodeKind::File);
        if (!created.ok()) return created;
    }
    return kernel.fs().writeFile(path, words);
}

[[nodiscard]] inline StatusResult installPackage(OSKernel& kernel, const PackageImage& package) {
    if (package.magic != PACKAGE_MAGIC ||
        package.version != PACKAGE_FORMAT_VERSION ||
        package.name.empty()) {
        return StatusResult::error(ERR_INVALID);
    }

    StatusResult manifest =
        writeKernelFile(kernel,
                        "/var/packages/" + package.name + ".manifest",
                        encodePackageManifest(package));
    if (!manifest.ok()) return manifest;

    for (const PackageEntry& entry : package.entries) {
        if (entry.path.empty() || entry.path[0] != '/') {
            return StatusResult::error(ERR_INVALID);
        }
        if (entry.executable) {
            if (!executableMetadataMatches(entry.words, entry.header, entry.metadata)) {
                return StatusResult::error(ERR_SIGNATURE);
            }
            StatusResult parents = ensureKernelParentDirectories(kernel, entry.path);
            if (!parents.ok()) return parents;
            StatusResult installed =
                kernel.installExecutable(entry.path, entry.words, entry.header);
            if (!installed.ok()) return installed;
            StatusResult sidecar =
                writeKernelFile(kernel,
                                entry.path + ".sig",
                                encodeSignedExecutableMetadata(entry.metadata));
            if (!sidecar.ok()) return sidecar;
        } else {
            StatusResult wrote = writeKernelFile(kernel, entry.path, entry.words);
            if (!wrote.ok()) return wrote;
        }
    }
    return kernel.shutdownSync();
}

} // namespace os
} // namespace sandbox

#endif // TERNARY_OS_H
