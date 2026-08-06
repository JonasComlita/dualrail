#include "ternary_redo_wal.h"

#include <iostream>
#include <vector>

namespace {

int failures = 0;

void expect(bool condition, const char* message) {
    if (condition) return;
    ++failures;
    std::cout << "FAIL: " << message << "\n";
}

void testLogicalCommitAndExplicitFsync() {
    using Wal = sandbox::os::RedoWalV2;
    Wal wal(128);
    for (int i = 0; i < 78; ++i) expect(wal.seed(i, i), "seed succeeds");

    const long long tx = wal.begin();
    std::vector<long long> after(78, 0);
    for (int i = 0; i < 78; ++i) after[static_cast<std::size_t>(i)] = 1000 + i;
    expect(wal.write(tx, Wal::TargetKind::Data, 7, 0, after),
           "78-word after-image buffers");
    expect(wal.commit(tx), "logical commit succeeds");
    expect(wal.read(77) == 1077, "logical commit is cache-visible");
    expect(wal.counters().stable_barriers == 0,
           "ordinary write issues no stable barrier");
    expect(wal.pendingBlocks() == 7,
           "78 words use six data records and one commit record");

    Wal crashed_before_fsync(128);
    expect(crashed_before_fsync.recover(wal.snapshot()),
           "pre-fsync recovery accepts the old durable image");
    expect(crashed_before_fsync.read(77) == 77,
           "non-durable logical commit is lost as a complete old state");

    expect(wal.fsyncRange(0, 78), "targeted fsync succeeds");
    expect(wal.counters().stable_barriers == 2,
           "78-word write plus fsync uses one WAL and one data barrier");
    expect(wal.counters().appended_blocks == 7,
           "fsync does not rewrite the entire WAL ring");

    Wal recovered(128);
    expect(recovered.recover(wal.snapshot()), "durable image recovers");
    expect(recovered.read(0) == 1000 && recovered.read(77) == 1077,
           "fsynced transaction recovers as a complete new state");
}

void testRedoRecoveryAndTornCommit() {
    using Wal = sandbox::os::RedoWalV2;
    Wal wal(16);
    expect(wal.seed(3, 10), "redo seed succeeds");
    const long long tx = wal.begin();
    expect(wal.writeWord(tx, 3, 99), "redo word buffers");
    expect(wal.commit(tx), "redo commit appends");
    expect(wal.flushWal(), "redo WAL becomes durable");
    expect(wal.counters().stable_barriers == 1,
           "WAL-only durability uses one barrier");

    Wal replay(16);
    expect(replay.recover(wal.snapshot()), "redo-only image recovers");
    expect(replay.read(3) == 99, "committed after-image replays");
    expect(replay.counters().recovery_replays == 1,
           "recovery reports one replayed data block");

    auto torn = wal.snapshot();
    Wal::corruptRecordChecksum(torn, 1); // commit block
    Wal ignored(16);
    expect(ignored.recover(torn), "torn record terminates recovery safely");
    expect(ignored.read(3) == 10,
           "transaction with torn commit remains entirely old");
}

void testAbortGroupingTimerAndSuperblocks() {
    using Wal = sandbox::os::RedoWalV2;
    Wal wal(64);
    expect(wal.seed(0, 4), "group seed succeeds");
    const long long aborted = wal.begin();
    expect(wal.writeWord(aborted, 0, 5), "abort after-image buffers");
    expect(wal.abort(aborted), "abort discards unpublished transaction");
    expect(wal.read(0) == 4 && wal.pendingBlocks() == 0,
           "abort needs neither undo nor a WAL record");

    for (int i = 0; i < 27; ++i) {
        const long long tx = wal.begin();
        expect(wal.writeWord(tx, i, 200 + i), "group record buffers");
        expect(wal.commit(tx), "group transaction commits");
    }
    expect(wal.pendingTransactions() < 27,
           "27 transactions cannot remain in one unflushed group");
    expect(wal.counters().group_flushes >= 1,
           "group flush counter advances");

    Wal timer(8);
    const long long tx = timer.begin();
    expect(timer.writeWord(tx, 1, 7) && timer.commit(tx),
           "timer transaction logically commits");
    timer.timerTick();
    expect(timer.pendingTransactions() == 0,
           "one tick after the first pending tick flushes the group");

    auto image = wal.snapshot();
    Wal::corruptSuperblockChecksum(image, 1);
    Wal fallback(64);
    expect(fallback.recover(image),
           "alternating superblock permits fallback after corruption");
}

bool isEntireState(const sandbox::os::RedoWalV2& wal,
                   const std::vector<long long>& expected) {
    for (std::size_t i = 0; i < expected.size(); ++i) {
        if (wal.read(static_cast<int>(i)) != expected[i]) return false;
    }
    return true;
}

void expectOldOrNew(const sandbox::os::RedoWalV2::Snapshot& image,
                    const std::vector<long long>& old_state,
                    const std::vector<long long>& new_state,
                    const char* message) {
    sandbox::os::RedoWalV2 recovered(
        static_cast<int>(old_state.size()));
    const bool valid = recovered.recover(image);
    expect(valid, "crash image has a valid recovery boundary");
    expect(valid &&
               (isEntireState(recovered, old_state) ||
                isEntireState(recovered, new_state)),
           message);
}

void testCrashInjectionMatrix() {
    using Wal = sandbox::os::RedoWalV2;
    constexpr int words = 31; // three data records plus commit
    std::vector<long long> old_state(words);
    std::vector<long long> new_state(words);
    for (int i = 0; i < words; ++i) {
        old_state[static_cast<std::size_t>(i)] = 10 + i;
        new_state[static_cast<std::size_t>(i)] = 1000 + i;
    }

    // A crash after any individual append but before the WAL barrier must
    // recover the complete old transaction.
    for (Wal::CrashPoint point : {
             Wal::CrashPoint::BeforeWalAppend,
             Wal::CrashPoint::AfterDataAppend,
             Wal::CrashPoint::AfterCommitAppend}) {
        Wal wal(words);
        for (int i = 0; i < words; ++i)
            expect(wal.seed(i, old_state[static_cast<std::size_t>(i)]),
                   "crash matrix seed succeeds");
        const long long tx = wal.begin();
        expect(wal.write(
                   tx, Wal::TargetKind::Data, 3, 0, new_state),
               "crash matrix transaction buffers");
        (void)wal.commit(tx, point);
        expectOldOrNew(
            wal.snapshot(), old_state, new_state,
            "append crash recovers complete old or new state");
    }

    // A power loss while a WAL record is being copied can leave a durable
    // record prefix behind, but the old superblock is still authoritative.
    {
        Wal wal(words);
        for (int i = 0; i < words; ++i)
            expect(wal.seed(i, old_state[static_cast<std::size_t>(i)]),
                   "record-write crash seed succeeds");
        const long long tx = wal.begin();
        expect(wal.write(tx, Wal::TargetKind::Data, 9, 0, new_state) &&
                   wal.commit(tx),
               "record-write crash transaction commits logically");
        (void)wal.flushWal(Wal::CrashPoint::AfterWalRecordWrite);
        expectOldOrNew(wal.snapshot(), old_state, old_state,
                       "record-write crash keeps the old superblock state");
    }

    Wal durable_log(words);
    for (int i = 0; i < words; ++i)
        expect(durable_log.seed(
                   i, old_state[static_cast<std::size_t>(i)]),
               "durable crash matrix seed succeeds");
    const long long tx = durable_log.begin();
    expect(durable_log.write(
               tx, Wal::TargetKind::Data, 3, 0, new_state) &&
               durable_log.commit(tx) &&
               durable_log.flushWal(),
           "crash matrix WAL reaches durable commit");

    // Torn checksum at each of the three data records or the commit record
    // must never expose a hybrid transaction.
    for (int ring_index = 0; ring_index < 4; ++ring_index) {
        auto torn = durable_log.snapshot();
        Wal::corruptRecordChecksum(torn, ring_index);
        expectOldOrNew(
            torn, old_state, new_state,
            "every torn WAL record recovers atomically");
    }

    // Model a crash after every possible home-page write prefix. Recovery
    // must replay the durable committed transaction to completion.
    const auto durable_snapshot = durable_log.snapshot();
    for (int persisted = 0; persisted <= words; ++persisted) {
        auto page_prefix = durable_snapshot;
        page_prefix.durable_words = old_state;
        for (int i = 0; i < persisted; ++i) {
            page_prefix.durable_words[static_cast<std::size_t>(i)] =
                new_state[static_cast<std::size_t>(i)];
        }
        expectOldOrNew(
            page_prefix, old_state, new_state,
            "every page-write crash prefix recovers atomically");
    }

    for (Wal::CrashPoint point : {
             Wal::CrashPoint::AfterWalRecordWrite,
             Wal::CrashPoint::AfterWalBarrier,
             Wal::CrashPoint::AfterPageWrite,
             Wal::CrashPoint::AfterDataBarrier,
             Wal::CrashPoint::AfterCheckpointAppend,
             Wal::CrashPoint::AfterSuperblockSwitch}) {
        Wal wal(words);
        for (int i = 0; i < words; ++i)
            expect(wal.seed(i, old_state[static_cast<std::size_t>(i)]),
                   "barrier crash matrix seed succeeds");
        const long long stage_tx = wal.begin();
        expect(wal.write(
                   stage_tx, Wal::TargetKind::Data, 5, 0, new_state) &&
                   wal.commit(stage_tx),
               "barrier crash transaction commits logically");
        if (point == Wal::CrashPoint::AfterWalRecordWrite ||
            point == Wal::CrashPoint::AfterWalBarrier ||
            point == Wal::CrashPoint::AfterSuperblockSwitch) {
            (void)wal.flushWal(point);
        } else if (point == Wal::CrashPoint::AfterPageWrite ||
                   point == Wal::CrashPoint::AfterDataBarrier) {
            (void)wal.fsyncRange(0, words, point);
        } else {
            (void)wal.checkpoint(point);
        }
        expectOldOrNew(
            wal.snapshot(), old_state, new_state,
            "barrier/checkpoint crash recovers atomically");
    }
}

void testCommitRequiresCompleteDataChain() {
    using Wal = sandbox::os::RedoWalV2;
    constexpr int words = 31;
    std::vector<long long> old_state(words);
    std::vector<long long> new_state(words);
    for (int i = 0; i < words; ++i) {
        old_state[static_cast<std::size_t>(i)] = 10 + i;
        new_state[static_cast<std::size_t>(i)] = 1000 + i;
    }

    // Build an older valid record that can stand in for a torn data slot.
    Wal stale_source(words);
    for (int i = 0; i < words; ++i)
        expect(stale_source.seed(i, old_state[static_cast<std::size_t>(i)]),
               "chain baseline seed succeeds");
    (void)stale_source.begin(); // reserve txid 1 so stale_tx differs from tx
    const long long stale_tx = stale_source.begin();
    expect(stale_source.writeWord(stale_tx, 30, 777) &&
               stale_source.commit(stale_tx) && stale_source.flushWal(),
           "chain baseline transaction is durable");

    Wal durable(words);
    for (int i = 0; i < words; ++i)
        expect(durable.seed(i, old_state[static_cast<std::size_t>(i)]),
               "chain new-state seed succeeds");
    const long long tx = durable.begin();
    expect(durable.write(tx, Wal::TargetKind::Data, 3, 0, new_state) &&
               durable.commit(tx) && durable.flushWal(),
           "chain new-state transaction is durable");

    // Keep the new transaction's second data record and commit, but replace
    // its first data record with an older valid record.  A checksum-only
    // recovery would replay the suffix and expose a hybrid state.
    auto hybrid = durable.snapshot();
    hybrid.ring[0] = stale_source.snapshot().ring[0];
    Wal recovered(words);
    expect(recovered.recover(hybrid),
           "stale-valid WAL slot still has a recoverable boundary");
    expect(isEntireState(recovered, old_state),
           "incomplete committed chain is discarded as one old state");
}

} // namespace

int main() {
    std::cout << std::unitbuf;
    std::cout << "[1] v2 redo WAL logical commit and fsync\n";
    testLogicalCommitAndExplicitFsync();
    std::cout << "[2] v2 redo recovery and torn commit\n";
    testRedoRecoveryAndTornCommit();
    std::cout << "[3] v2 redo abort, grouping, timer, and superblocks\n";
    testAbortGroupingTimerAndSuperblocks();
    std::cout << "[4] v2 redo crash-injection matrix\n";
    testCrashInjectionMatrix();
    std::cout << "[5] v2 redo WAL complete-transaction chain\n";
    testCommitRequiresCompleteDataChain();
    if (failures == 0) {
        std::cout << "Redo WAL v2 tests passed\n";
        return 0;
    }
    std::cout << failures << " failure(s)\n";
    return 1;
}
