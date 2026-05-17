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

#include <algorithm>
#include <array>
#include <map>
#include <set>
#include <sstream>
#include <string>
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

static constexpr int BLOCK_WORDS = vm::MMU_PAGE_WORDS;
static constexpr int FS_MAGIC = 80808;
static constexpr int FS_VERSION = 1;
static constexpr int DEFAULT_INODE_COUNT = 32;
static constexpr int DIRECT_BLOCKS = 6;

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
    long long value = 0;
    SharedOrder order = SharedOrder::AcquireRelease;

    [[nodiscard]] long long tldr(SharedOrder required) const {
        (void)required;
        return value;
    }
    [[nodiscard]] StatusResult tstr(long long desired, long long expected, SharedOrder required) {
        (void)required;
        if (value != expected) return StatusResult::pending(value, ERR_INVALID);
        value = desired;
        return StatusResult::success(desired);
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
        : blocks_(static_cast<std::size_t>(std::max(1, block_count)),
                  std::vector<long long>(BLOCK_WORDS, 0)),
          dirty_(static_cast<std::size_t>(std::max(1, block_count)), false) {}

    [[nodiscard]] int blockCount() const { return static_cast<int>(blocks_.size()); }
    [[nodiscard]] int blockWords() const { return BLOCK_WORDS; }

    [[nodiscard]] StatusResult readBlock(int index, std::vector<long long>& out) const {
        if (index < 0 || index >= blockCount()) return StatusResult::error(ERR_INVALID);
        out = blocks_[static_cast<std::size_t>(index)];
        return StatusResult::success(BLOCK_WORDS);
    }

    [[nodiscard]] StatusResult writeBlock(int index, const std::vector<long long>& data) {
        if (index < 0 || index >= blockCount()) return StatusResult::error(ERR_INVALID);
        if (static_cast<int>(data.size()) != BLOCK_WORDS) return StatusResult::error(ERR_INVALID);
        blocks_[static_cast<std::size_t>(index)] = data;
        dirty_[static_cast<std::size_t>(index)] = true;
        return StatusResult::success(BLOCK_WORDS);
    }

    [[nodiscard]] bool dirty(int index) const {
        return index >= 0 && index < blockCount() && dirty_[static_cast<std::size_t>(index)];
    }

    [[nodiscard]] std::vector<long long> serialize() const {
        std::vector<long long> out;
        out.reserve(static_cast<std::size_t>(blockCount() * BLOCK_WORDS));
        for (const auto& block : blocks_) {
            out.insert(out.end(), block.begin(), block.end());
        }
        return out;
    }

private:
    std::vector<std::vector<long long>> blocks_;
    std::vector<bool> dirty_;
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
        }

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
                for (std::size_t d = 0; d < inode.direct.size(); ++d) {
                    int block = inode.direct[d];
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
        releaseBlocks(inode);
        inode.data = words;
        inode.size_words = static_cast<int>(words.size());
        const int needed = (inode.size_words + BLOCK_WORDS - 1) / BLOCK_WORDS;
        if (needed > DIRECT_BLOCKS + BLOCK_WORDS) return StatusResult::error(ERR_NO_SPACE);
        for (int i = 0; i < std::min(needed, DIRECT_BLOCKS); ++i) {
            int block = allocateBlock();
            if (block < 0) return StatusResult::error(ERR_NO_SPACE);
            inode.direct[static_cast<std::size_t>(i)] = block;
        }
        if (needed > DIRECT_BLOCKS) {
            inode.indirect_block = allocateBlock();
            if (inode.indirect_block < 0) return StatusResult::error(ERR_NO_SPACE);
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
            for (std::size_t i = 0; i < inode.direct.size(); ++i) {
                int block = inode.direct[i];
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
        for (int i = data_start_; i < static_cast<int>(free_blocks_.size()); ++i) {
            if (free_blocks_[static_cast<std::size_t>(i)]) {
                free_blocks_[static_cast<std::size_t>(i)] = false;
                return i;
            }
        }
        return -1;
    }

    void releaseBlocks(Inode& inode) {
        for (int& block : inode.direct) {
            if (block >= 0 && block < static_cast<int>(free_blocks_.size())) {
                free_blocks_[static_cast<std::size_t>(block)] = true;
            }
            block = -1;
        }
        if (inode.indirect_block >= 0 &&
            inode.indirect_block < static_cast<int>(free_blocks_.size())) {
            free_blocks_[static_cast<std::size_t>(inode.indirect_block)] = true;
        }
        inode.indirect_block = -1;
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
    int heap_start = 0;
    int heap_break = 0;
    int heap_limit = 0;
    int fork_return_payload = -1;
    vm::ExecutableImageHeader exec_header;
    std::vector<long long> memory;
    std::map<int, OpenFile> fds;
};

class OSKernel {
public:
    explicit OSKernel(int blocks = 128)
        : device_tree_(defaultDeviceTree(blocks)), block_device_(blocks) {
        (void)fs_.format(block_device_);
        (void)createProcess(-1);
    }

    [[nodiscard]] DeviceTree& deviceTree() { return device_tree_; }
    [[nodiscard]] BlockDevice& blockDevice() { return block_device_; }
    [[nodiscard]] TinyFileSystem& fs() { return fs_; }
    [[nodiscard]] const TinyFileSystem& fs() const { return fs_; }

    [[nodiscard]] StatusResult boot() {
        std::vector<std::string> errors;
        StatusResult dt = device_tree_.validate(&errors);
        if (!dt.ok()) return dt;
        return fs_.mount(block_device_);
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

    [[nodiscard]] StatusResult sysOpen(int pid, const std::string& path, bool writable = false) {
        Process* proc = process(pid);
        if (!proc) return StatusResult::error(ERR_INVALID);
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

private:
    DeviceTree device_tree_;
    BlockDevice block_device_;
    TinyFileSystem fs_;
    std::vector<Process> processes_;
    int next_pid_ = 1;

    [[nodiscard]] int createProcess(int parent) {
        Process proc;
        proc.pid = next_pid_++;
        proc.parent_pid = parent;
        proc.state = vm::PROC_STATE_RUNNABLE;
        proc.heap_start = vm::MMU_PAGE_WORDS;
        proc.heap_break = proc.heap_start;
        proc.heap_limit = proc.heap_start + 9 * vm::MMU_PAGE_WORDS;
        proc.memory.resize(static_cast<std::size_t>(proc.heap_start), 0);
        processes_.push_back(proc);
        return proc.pid;
    }

    [[nodiscard]] static int nextFd(const Process& proc) {
        int fd = 3;
        while (proc.fds.count(fd)) ++fd;
        return fd;
    }
};

} // namespace os
} // namespace sandbox

#endif // TERNARY_OS_H
