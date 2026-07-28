#pragma once
#ifndef TERNARY_REDO_WAL_H
#define TERNARY_REDO_WAL_H

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <map>
#include <set>
#include <utility>
#include <vector>

namespace sandbox {
namespace os {

// Host-side executable model of the v2 storage contract.  The kernel uses the
// same constants and record layout; this model is intentionally independent of
// a host database or logging library so it can also serve as a recovery oracle.
class RedoWalV2 {
public:
    static constexpr int kBlockWords = 27;
    static constexpr int kHeaderWords = 12;
    static constexpr int kPayloadWords = 15;
    static constexpr int kRingBlocks = 729;
    static constexpr int kSuperblocks = 2;
    static constexpr long long kMagic = 170171;
    static constexpr int kVersion = 2;

    enum class RecordType : int {
        Data = 1,
        Commit = 2,
        Checkpoint = 3,
    };

    enum class TargetKind : int {
        WordArray = 1,
        Metadata = 2,
        Data = 3,
    };

    enum class CrashPoint : int {
        None = 0,
        BeforeWalAppend,
        AfterDataAppend,
        AfterCommitAppend,
        AfterWalBarrier,
        AfterPageWrite,
        AfterDataBarrier,
        AfterCheckpointAppend,
        AfterSuperblockSwitch,
    };

    struct Counters {
        long long appended_blocks = 0;
        long long stable_barriers = 0;
        long long page_writes = 0;
        long long commits = 0;
        long long group_flushes = 0;
        long long checkpoints = 0;
        long long recovery_replays = 0;
        long long torn_blocks = 0;
    };

    using Block = std::array<long long, kBlockWords>;

    struct Snapshot {
        std::array<Block, kSuperblocks> superblocks{};
        std::array<Block, kRingBlocks> ring{};
        std::vector<long long> durable_words;
    };

    explicit RedoWalV2(int word_count = 1)
        : cache_words_(static_cast<std::size_t>(std::max(1, word_count)), 0),
          durable_words_(cache_words_),
          dirty_lsn_(cache_words_.size(), 0) {
        format();
    }

    void format() {
        for (Block& block : volatile_ring_) block.fill(0);
        for (Block& block : durable_ring_) block.fill(0);
        for (Block& block : durable_superblocks_) block.fill(0);
        transactions_.clear();
        appended_.clear();
        head_ = 0;
        tail_ = 0;
        next_lsn_ = 1;
        durable_lsn_ = 0;
        checkpoint_lsn_ = 0;
        generation_ = 1;
        active_superblock_ = 0;
        next_txid_ = 1;
        pending_transactions_ = 0;
        pending_blocks_ = 0;
        first_pending_tick_ = -1;
        tick_ = 0;
        counters_ = Counters{};
        writeDurableSuperblock(0, generation_);
    }

    [[nodiscard]] long long read(int address) const {
        return validAddress(address)
            ? cache_words_[static_cast<std::size_t>(address)]
            : 0;
    }

    [[nodiscard]] bool seed(int address, long long value) {
        if (!validAddress(address)) return false;
        cache_words_[static_cast<std::size_t>(address)] = value;
        durable_words_[static_cast<std::size_t>(address)] = value;
        dirty_lsn_[static_cast<std::size_t>(address)] = 0;
        return true;
    }

    [[nodiscard]] long long begin() {
        const long long txid = next_txid_++;
        transactions_[txid] = Transaction{};
        return txid;
    }

    [[nodiscard]] bool write(long long txid,
                             TargetKind kind,
                             int target_id,
                             int offset,
                             const std::vector<long long>& after_image) {
        const auto found = transactions_.find(txid);
        if (found == transactions_.end() || after_image.empty() ||
            offset < 0 ||
            offset + static_cast<int>(after_image.size()) >
                static_cast<int>(cache_words_.size())) {
            return false;
        }
        found->second.records.push_back(
            PendingRecord{kind, target_id, offset, after_image});
        return true;
    }

    [[nodiscard]] bool writeWord(long long txid,
                                 int address,
                                 long long value) {
        return write(txid, TargetKind::WordArray, 0, address, {value});
    }

    [[nodiscard]] bool abort(long long txid) {
        return transactions_.erase(txid) != 0;
    }

    // Logical commit: records are appended and their after-images become
    // visible in cache, but no stable-storage barrier is implied.
    [[nodiscard]] bool commit(long long txid,
                              CrashPoint crash = CrashPoint::None) {
        const auto found = transactions_.find(txid);
        if (found == transactions_.end()) return false;
        const Transaction tx = found->second;
        if (crash == CrashPoint::BeforeWalAppend) {
            transactions_.erase(found);
            return false;
        }

        int needed = 1;
        for (const PendingRecord& record : tx.records) {
            needed += static_cast<int>(
                (record.words.size() + kPayloadWords - 1) / kPayloadWords);
        }
        if (needed > freeBlocks()) return false;

        long long previous = 0;
        int data_blocks = 0;
        for (const PendingRecord& record : tx.records) {
            std::size_t consumed = 0;
            while (consumed < record.words.size()) {
                const int count = std::min<int>(
                    kPayloadWords,
                    static_cast<int>(record.words.size() - consumed));
                Block block = makeBlock(
                    next_lsn_++,
                    txid,
                    previous,
                    RecordType::Data,
                    record.kind,
                    record.target_id,
                    record.offset + static_cast<int>(consumed),
                    record.words.data() + consumed,
                    count);
                previous = block[3];
                append(block);
                consumed += static_cast<std::size_t>(count);
                ++data_blocks;
                if (crash == CrashPoint::AfterDataAppend && data_blocks == 1) {
                    transactions_.erase(found);
                    return false;
                }
            }
        }

        Block commit_block = makeBlock(
            next_lsn_++,
            txid,
            previous,
            RecordType::Commit,
            TargetKind::WordArray,
            0,
            0,
            nullptr,
            0);
        const long long commit_lsn = commit_block[3];
        append(commit_block);
        transactions_.erase(found);
        if (crash == CrashPoint::AfterCommitAppend) return false;

        // Steal is forbidden: cache pages remember the commit LSN and cannot
        // become durable before the WAL reaches it.
        for (const PendingRecord& record : tx.records) {
            for (std::size_t i = 0; i < record.words.size(); ++i) {
                const std::size_t address =
                    static_cast<std::size_t>(record.offset) + i;
                cache_words_[address] = record.words[i];
                dirty_lsn_[address] = commit_lsn;
            }
        }
        ++pending_transactions_;
        ++counters_.commits;
        if (first_pending_tick_ < 0) first_pending_tick_ = tick_;
        if (pending_transactions_ >= 27 || pending_blocks_ >= 27) {
            return flushWal(crash);
        }
        return true;
    }

    [[nodiscard]] bool flushWal(CrashPoint crash = CrashPoint::None) {
        if (appended_.empty()) return true;
        for (int index : appended_) {
            durable_ring_[static_cast<std::size_t>(index)] =
                volatile_ring_[static_cast<std::size_t>(index)];
        }
        if (crash == CrashPoint::AfterCommitAppend) return false;

        durable_lsn_ = lastAppendedLsn();
        ++generation_;
        active_superblock_ = 1 - active_superblock_;
        writeDurableSuperblock(active_superblock_, generation_);
        ++counters_.stable_barriers;
        ++counters_.group_flushes;
        if (crash == CrashPoint::AfterWalBarrier ||
            crash == CrashPoint::AfterSuperblockSwitch) {
            return false;
        }
        appended_.clear();
        pending_transactions_ = 0;
        pending_blocks_ = 0;
        first_pending_tick_ = -1;
        return true;
    }

    // Flushes the WAL through every dirty word in [offset, offset+count), then
    // writes only those words and issues one data barrier.
    [[nodiscard]] bool fsyncRange(int offset,
                                  int count,
                                  CrashPoint crash = CrashPoint::None) {
        if (offset < 0 || count < 0 ||
            offset + count > static_cast<int>(cache_words_.size())) {
            return false;
        }
        long long required_lsn = 0;
        for (int i = 0; i < count; ++i) {
            required_lsn = std::max(
                required_lsn,
                dirty_lsn_[static_cast<std::size_t>(offset + i)]);
        }
        if (required_lsn > durable_lsn_ && !flushWal(crash)) return false;
        bool wrote = false;
        for (int i = 0; i < count; ++i) {
            const std::size_t address =
                static_cast<std::size_t>(offset + i);
            if (dirty_lsn_[address] == 0) continue;
            if (dirty_lsn_[address] > durable_lsn_) return false;
            durable_words_[address] = cache_words_[address];
            dirty_lsn_[address] = 0;
            wrote = true;
            ++counters_.page_writes;
            if (crash == CrashPoint::AfterPageWrite) return false;
        }
        if (wrote) {
            ++counters_.stable_barriers;
            if (crash == CrashPoint::AfterDataBarrier) return false;
        }
        return true;
    }

    [[nodiscard]] bool syncAll(CrashPoint crash = CrashPoint::None) {
        return fsyncRange(
            0, static_cast<int>(cache_words_.size()), crash);
    }

    void timerTick() {
        ++tick_;
        if (first_pending_tick_ >= 0 && tick_ > first_pending_tick_) {
            (void)flushWal();
        }
    }

    [[nodiscard]] bool checkpoint(CrashPoint crash = CrashPoint::None) {
        if (!flushWal(crash) || !syncAll(crash)) return false;
        const long long chosen_lsn = durable_lsn_;
        Block checkpoint = makeBlock(
            next_lsn_++,
            0,
            chosen_lsn,
            RecordType::Checkpoint,
            TargetKind::Metadata,
            0,
            0,
            nullptr,
            0);
        append(checkpoint);
        if (crash == CrashPoint::AfterCheckpointAppend) return false;
        if (!flushWal(crash)) return false;
        checkpoint_lsn_ = chosen_lsn;
        tail_ = head_;
        ++generation_;
        active_superblock_ = 1 - active_superblock_;
        writeDurableSuperblock(active_superblock_, generation_);
        ++counters_.stable_barriers;
        ++counters_.checkpoints;
        return crash != CrashPoint::AfterSuperblockSwitch;
    }

    [[nodiscard]] Snapshot snapshot() const {
        return Snapshot{
            durable_superblocks_, durable_ring_, durable_words_};
    }

    [[nodiscard]] bool recover(const Snapshot& image) {
        durable_superblocks_ = image.superblocks;
        durable_ring_ = image.ring;
        durable_words_ = image.durable_words;
        cache_words_ = durable_words_;
        dirty_lsn_.assign(cache_words_.size(), 0);
        transactions_.clear();
        appended_.clear();

        int chosen = -1;
        long long chosen_generation = -1;
        for (int i = 0; i < kSuperblocks; ++i) {
            if (!validSuperblock(durable_superblocks_[i])) continue;
            if (durable_superblocks_[i][2] > chosen_generation) {
                chosen = i;
                chosen_generation = durable_superblocks_[i][2];
            }
        }
        if (chosen < 0) return false;
        const Block& super = durable_superblocks_[chosen];
        generation_ = super[2];
        head_ = static_cast<int>(super[3]);
        tail_ = static_cast<int>(super[4]);
        durable_lsn_ = super[5];
        checkpoint_lsn_ = super[6];
        active_superblock_ = chosen;

        std::map<long long, std::vector<Block>> data_by_tx;
        std::set<long long> committed;
        int cursor = tail_;
        long long previous_lsn = checkpoint_lsn_;
        while (cursor != head_) {
            const Block& block = durable_ring_[static_cast<std::size_t>(cursor)];
            if (!validRecord(block) || block[3] <= previous_lsn ||
                block[3] > durable_lsn_) {
                if (block[0] != 0) ++counters_.torn_blocks;
                break;
            }
            previous_lsn = block[3];
            const auto type = static_cast<RecordType>(block[6]);
            if (type == RecordType::Data) {
                data_by_tx[block[4]].push_back(block);
            } else if (type == RecordType::Commit) {
                committed.insert(block[4]);
            }
            cursor = (cursor + 1) % kRingBlocks;
        }

        std::vector<Block> replay;
        for (long long txid : committed) {
            const auto found = data_by_tx.find(txid);
            if (found == data_by_tx.end()) continue;
            replay.insert(replay.end(), found->second.begin(), found->second.end());
        }
        std::sort(replay.begin(), replay.end(),
                  [](const Block& a, const Block& b) { return a[3] < b[3]; });
        for (const Block& block : replay) {
            const int offset = static_cast<int>(block[9]);
            const int count = static_cast<int>(block[10]);
            if (offset < 0 || count < 0 ||
                offset + count > static_cast<int>(cache_words_.size())) {
                return false;
            }
            for (int i = 0; i < count; ++i) {
                cache_words_[static_cast<std::size_t>(offset + i)] =
                    block[static_cast<std::size_t>(kHeaderWords + i)];
                dirty_lsn_[static_cast<std::size_t>(offset + i)] = block[3];
            }
            ++counters_.recovery_replays;
        }
        return true;
    }

    [[nodiscard]] long long durableLsn() const { return durable_lsn_; }
    [[nodiscard]] int pendingTransactions() const {
        return pending_transactions_;
    }
    [[nodiscard]] int pendingBlocks() const { return pending_blocks_; }
    [[nodiscard]] int openTransactions() const {
        return static_cast<int>(transactions_.size());
    }
    [[nodiscard]] const Counters& counters() const { return counters_; }

    static void corruptRecordChecksum(Snapshot& image, int ring_index) {
        if (ring_index < 0 || ring_index >= kRingBlocks) return;
        image.ring[static_cast<std::size_t>(ring_index)][11] += 1;
    }

    static void corruptSuperblockChecksum(Snapshot& image, int index) {
        if (index < 0 || index >= kSuperblocks) return;
        image.superblocks[static_cast<std::size_t>(index)][11] += 1;
    }

private:
    struct PendingRecord {
        TargetKind kind = TargetKind::WordArray;
        int target_id = 0;
        int offset = 0;
        std::vector<long long> words;
    };
    struct Transaction {
        std::vector<PendingRecord> records;
    };

    std::vector<long long> cache_words_;
    std::vector<long long> durable_words_;
    std::vector<long long> dirty_lsn_;
    std::array<Block, kRingBlocks> volatile_ring_{};
    std::array<Block, kRingBlocks> durable_ring_{};
    std::array<Block, kSuperblocks> durable_superblocks_{};
    std::map<long long, Transaction> transactions_;
    std::vector<int> appended_;
    int head_ = 0;
    int tail_ = 0;
    long long next_lsn_ = 1;
    long long durable_lsn_ = 0;
    long long checkpoint_lsn_ = 0;
    long long generation_ = 1;
    int active_superblock_ = 0;
    long long next_txid_ = 1;
    int pending_transactions_ = 0;
    int pending_blocks_ = 0;
    long long first_pending_tick_ = -1;
    long long tick_ = 0;
    Counters counters_;

    [[nodiscard]] bool validAddress(int address) const {
        return address >= 0 &&
               address < static_cast<int>(cache_words_.size());
    }

    [[nodiscard]] int usedBlocks() const {
        return head_ >= tail_
            ? head_ - tail_
            : kRingBlocks - tail_ + head_;
    }
    [[nodiscard]] int freeBlocks() const {
        return kRingBlocks - usedBlocks() - 1;
    }

    static long long checksum(const Block& block) {
        constexpr long long modulus = 1000003;
        long long value = 17;
        for (int i = 0; i < kBlockWords; ++i) {
            if (i == 11) continue;
            long long word = block[static_cast<std::size_t>(i)] % modulus;
            if (word < 0) word += modulus;
            value = (value * 257 + word + i) % modulus;
        }
        return value;
    }

    static Block makeBlock(long long lsn,
                           long long txid,
                           long long previous_lsn,
                           RecordType type,
                           TargetKind kind,
                           int target_id,
                           int offset,
                           const long long* payload,
                           int count) {
        Block block{};
        block[0] = kMagic;
        block[1] = kVersion;
        block[2] = 1;
        block[3] = lsn;
        block[4] = txid;
        block[5] = previous_lsn;
        block[6] = static_cast<int>(type);
        block[7] = static_cast<int>(kind);
        block[8] = target_id;
        block[9] = offset;
        block[10] = count;
        for (int i = 0; i < count; ++i) {
            block[static_cast<std::size_t>(kHeaderWords + i)] = payload[i];
        }
        block[11] = checksum(block);
        return block;
    }

    void append(const Block& block) {
        volatile_ring_[static_cast<std::size_t>(head_)] = block;
        appended_.push_back(head_);
        head_ = (head_ + 1) % kRingBlocks;
        ++pending_blocks_;
        ++counters_.appended_blocks;
    }

    [[nodiscard]] long long lastAppendedLsn() const {
        if (appended_.empty()) return durable_lsn_;
        const int index = appended_.back();
        return volatile_ring_[static_cast<std::size_t>(index)][3];
    }

    void writeDurableSuperblock(int index, long long generation) {
        Block block{};
        block[0] = kMagic;
        block[1] = kVersion;
        block[2] = generation;
        block[3] = head_;
        block[4] = tail_;
        block[5] = durable_lsn_;
        block[6] = checkpoint_lsn_;
        block[7] = kRingBlocks;
        block[8] = kBlockWords;
        block[9] = active_superblock_;
        block[10] = 0;
        block[11] = checksum(block);
        durable_superblocks_[static_cast<std::size_t>(index)] = block;
    }

    static bool validRecord(const Block& block) {
        if (block[0] != kMagic || block[1] != kVersion ||
            block[10] < 0 || block[10] > kPayloadWords) {
            return false;
        }
        const int type = static_cast<int>(block[6]);
        if (type < static_cast<int>(RecordType::Data) ||
            type > static_cast<int>(RecordType::Checkpoint)) {
            return false;
        }
        return block[11] == checksum(block);
    }

    static bool validSuperblock(const Block& block) {
        return block[0] == kMagic &&
               block[1] == kVersion &&
               block[7] == kRingBlocks &&
               block[8] == kBlockWords &&
               block[3] >= 0 && block[3] < kRingBlocks &&
               block[4] >= 0 && block[4] < kRingBlocks &&
               block[11] == checksum(block);
    }
};

} // namespace os
} // namespace sandbox

#endif // TERNARY_REDO_WAL_H
