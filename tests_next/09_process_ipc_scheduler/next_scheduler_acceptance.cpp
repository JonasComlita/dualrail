#include "tests_next/00_harness/next_test_harness.h"
#include "ternary_compiler.h"
#include "ternary_vm.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <map>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace {

using tests_next::TestCase;
using tests_next::TestContext;
using sandbox::compiler::CompileResult;
using sandbox::compiler::LinkResult;

struct JsonValue {
    enum class Kind { null_value, boolean, number, string, array, object };

    Kind kind = Kind::null_value;
    bool boolean = false;
    double number = 0.0;
    std::string string_value;
    std::vector<JsonValue> array;
    std::map<std::string, JsonValue> object;
};

class JsonParser {
public:
    explicit JsonParser(const std::string& text) : text_(text) {}

    bool parse(JsonValue& value, std::string& error) {
        if (!parseValue(value)) {
            error = error_;
            return false;
        }
        skipWhitespace();
        if (pos_ != text_.size()) {
            error = "trailing JSON data at offset " + std::to_string(pos_);
            return false;
        }
        return true;
    }

private:
    const std::string& text_;
    std::size_t pos_ = 0;
    std::string error_;

    void skipWhitespace() {
        while (pos_ < text_.size() &&
               std::isspace(static_cast<unsigned char>(text_[pos_])) != 0) {
            ++pos_;
        }
    }

    bool fail(const std::string& message) {
        error_ = message + " at offset " + std::to_string(pos_);
        return false;
    }

    bool consume(char expected) {
        skipWhitespace();
        if (pos_ >= text_.size() || text_[pos_] != expected) return false;
        ++pos_;
        return true;
    }

    bool parseValue(JsonValue& value) {
        skipWhitespace();
        if (pos_ >= text_.size()) return fail("expected JSON value");
        switch (text_[pos_]) {
        case '{': return parseObject(value);
        case '[': return parseArray(value);
        case '"':
            value.kind = JsonValue::Kind::string;
            return parseString(value.string_value);
        case 't':
            if (text_.compare(pos_, 4, "true") != 0) return fail("invalid literal");
            pos_ += 4;
            value.kind = JsonValue::Kind::boolean;
            value.boolean = true;
            return true;
        case 'f':
            if (text_.compare(pos_, 5, "false") != 0) return fail("invalid literal");
            pos_ += 5;
            value.kind = JsonValue::Kind::boolean;
            value.boolean = false;
            return true;
        case 'n':
            if (text_.compare(pos_, 4, "null") != 0) return fail("invalid literal");
            pos_ += 4;
            value.kind = JsonValue::Kind::null_value;
            return true;
        default:
            value.kind = JsonValue::Kind::number;
            return parseNumber(value.number);
        }
    }

    bool parseString(std::string& value) {
        skipWhitespace();
        if (pos_ >= text_.size() || text_[pos_] != '"') {
            return fail("expected JSON string");
        }
        ++pos_;
        value.clear();
        while (pos_ < text_.size()) {
            const char ch = text_[pos_++];
            if (ch == '"') return true;
            if (static_cast<unsigned char>(ch) < 0x20) {
                return fail("control character in JSON string");
            }
            if (ch != '\\') {
                value.push_back(ch);
                continue;
            }
            if (pos_ >= text_.size()) return fail("unterminated escape");
            const char escaped = text_[pos_++];
            switch (escaped) {
            case '"': value.push_back('"'); break;
            case '\\': value.push_back('\\'); break;
            case '/': value.push_back('/'); break;
            case 'b': value.push_back('\b'); break;
            case 'f': value.push_back('\f'); break;
            case 'n': value.push_back('\n'); break;
            case 'r': value.push_back('\r'); break;
            case 't': value.push_back('\t'); break;
            default: return fail("unsupported JSON escape");
            }
        }
        return fail("unterminated JSON string");
    }

    bool parseNumber(double& value) {
        skipWhitespace();
        const std::size_t start = pos_;
        char* end = nullptr;
        value = std::strtod(text_.c_str() + start, &end);
        if (end == text_.c_str() + start) return fail("invalid JSON number");
        pos_ = static_cast<std::size_t>(end - text_.c_str());
        return true;
    }

    bool parseArray(JsonValue& value) {
        if (!consume('[')) return fail("expected JSON array");
        value.kind = JsonValue::Kind::array;
        value.array.clear();
        skipWhitespace();
        if (consume(']')) return true;
        while (true) {
            JsonValue item;
            if (!parseValue(item)) return false;
            value.array.push_back(std::move(item));
            skipWhitespace();
            if (consume(']')) return true;
            if (!consume(',')) return fail("expected array separator");
        }
    }

    bool parseObject(JsonValue& value) {
        if (!consume('{')) return fail("expected JSON object");
        value.kind = JsonValue::Kind::object;
        value.object.clear();
        skipWhitespace();
        if (consume('}')) return true;
        while (true) {
            std::string key;
            if (!parseString(key)) return false;
            if (!consume(':')) return fail("expected object separator");
            JsonValue item;
            if (!parseValue(item)) return false;
            const auto inserted = value.object.emplace(std::move(key), std::move(item));
            if (!inserted.second) return fail("duplicate object key");
            skipWhitespace();
            if (consume('}')) return true;
            if (!consume(',')) return fail("expected object separator");
        }
    }
};

const JsonValue* jsonMember(const JsonValue& value, const std::string& name) {
    if (value.kind != JsonValue::Kind::object) return nullptr;
    const auto it = value.object.find(name);
    return it == value.object.end() ? nullptr : &it->second;
}

bool jsonString(const JsonValue& value, const std::string& expected) {
    return value.kind == JsonValue::Kind::string && value.string_value == expected;
}

bool jsonNumber(const JsonValue& value, double expected) {
    return value.kind == JsonValue::Kind::number &&
           std::isfinite(value.number) && std::fabs(value.number - expected) < 1e-9;
}

bool jsonBoolean(const JsonValue& value, bool expected) {
    return value.kind == JsonValue::Kind::boolean && value.boolean == expected;
}

bool parseJsonFile(const std::string& path, JsonValue& value, std::string& error) {
    const std::string text = tests_next::readText(path);
    if (text.empty()) {
        error = "file is empty or unavailable: " + path;
        return false;
    }
    JsonParser parser(text);
    if (!parser.parse(value, error)) {
        error = path + ": " + error;
        return false;
    }
    return true;
}

bool arrayContainsString(const JsonValue& value, const std::string& expected) {
    if (value.kind != JsonValue::Kind::array) return false;
    for (const JsonValue& item : value.array) {
        if (jsonString(item, expected)) return true;
    }
    return false;
}

void checkJsonStringField(TestContext& ctx, const JsonValue& object,
                          const std::string& field, const std::string& expected,
                          const std::string& message) {
    const JsonValue* value = jsonMember(object, field);
    ctx.check(value != nullptr && jsonString(*value, expected), message);
}

void checkJsonNumberField(TestContext& ctx, const JsonValue& object,
                          const std::string& field, double expected,
                          const std::string& message) {
    const JsonValue* value = jsonMember(object, field);
    ctx.check(value != nullptr && jsonNumber(*value, expected), message);
}

void checkJsonBooleanField(TestContext& ctx, const JsonValue& object,
                           const std::string& field, bool expected,
                           const std::string& message) {
    const JsonValue* value = jsonMember(object, field);
    ctx.check(value != nullptr && jsonBoolean(*value, expected), message);
}

std::optional<long long> tritConstant(const std::string& source,
                                      const std::string& name) {
    const std::string prefix = "const " + name + ": t40 = ";
    const std::size_t begin = source.find(prefix);
    if (begin == std::string::npos) return std::nullopt;
    const std::size_t value_begin = begin + prefix.size();
    const std::size_t end = source.find(';', value_begin);
    if (end == std::string::npos) return std::nullopt;
    const std::string text = source.substr(value_begin, end - value_begin);
    char* parsed_end = nullptr;
    const long long value = std::strtoll(text.c_str(), &parsed_end, 10);
    if (parsed_end == text.c_str()) return std::nullopt;
    while (*parsed_end != '\0' &&
           std::isspace(static_cast<unsigned char>(*parsed_end)) != 0) {
        ++parsed_end;
    }
    if (*parsed_end != '\0') return std::nullopt;
    return value;
}

std::string tritFunction(const std::string& source, const std::string& signature) {
    const std::size_t begin = source.find(signature);
    if (begin == std::string::npos) return {};
    const std::size_t body = source.find('{', begin + signature.size());
    if (body == std::string::npos) return {};
    int depth = 0;
    for (std::size_t i = body; i < source.size(); ++i) {
        if (source[i] == '{') ++depth;
        if (source[i] == '}') {
            --depth;
            if (depth == 0) return source.substr(begin, i - begin + 1);
        }
    }
    return {};
}

constexpr int kFailureAddr = 36000;
constexpr int kFailureActualAddr = 36001;
constexpr int kFailureExpectedAddr = 36002;
constexpr int kFailureIndexAddr = 36003;
constexpr int kOneSamplesAddr = 36100;
constexpr int kManySamplesAddr = 36120;
constexpr int kFairnessTraceAddr = 36140;
constexpr int kFifoOrderAddr = 37150;
constexpr int kWakeResultsAddr = 37160;
constexpr int kIdleResultsAddr = 37200;

const char* const kCoreDriver = R"TRIT(
const SCHED_TEST_FAILURE: t40 = 36000;
const SCHED_TEST_FAILURE_ACTUAL: t40 = 36001;
const SCHED_TEST_FAILURE_EXPECTED: t40 = 36002;
const SCHED_TEST_FAILURE_INDEX: t40 = 36003;
const SCHED_TEST_ONE_SAMPLES: t40 = 36100;
const SCHED_TEST_MANY_SAMPLES: t40 = 36120;
const SCHED_TEST_FAIRNESS_TRACE: t40 = 36140;
const SCHED_TEST_FIFO_ORDER: t40 = 37150;
const SCHED_TEST_WAKE_RESULTS: t40 = 37160;

fn scheduler_test_cycle() -> t40 {
    var cycles: t40 = 0;
    unsafe { cycles = csr_read(cycle); }
    return cycles;
}

fn scheduler_test_equal(actual: t40, expected: t40,
                        assertion: t40, ok: t40) -> t40 {
    if actual - expected == 0 {
        return ok;
    }
    if kload(SCHED_TEST_FAILURE) == 0 {
        kstore(SCHED_TEST_FAILURE, 1);
        kstore(SCHED_TEST_FAILURE_ACTUAL, actual);
        kstore(SCHED_TEST_FAILURE_EXPECTED, expected);
        kstore(SCHED_TEST_FAILURE_INDEX, assertion);
    }
    return 0;
}

fn scheduler_test_positive(value: t40) -> t40 {
    if value > 0 { return 1; }
    return 0;
}

fn scheduler_test_nonnegative(value: t40) -> t40 {
    if value >= 0 { return 1; }
    return 0;
}

fn scheduler_test_reset() -> t40 {
    scheduler_init();
    process_table_init();
    return 1;
}

fn scheduler_test_seed_processes(count: t40) -> t40 {
    var slot: t40 = 0;
    var ok: t40 = 1;
    while count - slot > 0 {
        var pid: t40 = slot + 1;
        var created: t40 = process_create(slot, pid, 0, 1, 0, 0);
        ok = scheduler_test_equal(created, pid, 10 + slot, ok);
        slot = slot + 1;
    }
    return ok;
}

fn scheduler_test_drain(count: t40) -> t40 {
    var slot: t40 = 0;
    var ok: t40 = 1;
    while count - slot > 0 {
        var selected: t40 = tier1_dequeue();
        ok = scheduler_test_equal(selected, slot, 100 + slot, ok);
        slot = slot + 1;
    }
    return ok;
}

fn scheduler_test_stale_membership(ok: t40) -> t40 {
    ok = scheduler_test_reset() * ok;
    ok = scheduler_test_seed_processes(3) * ok;

    // Slot one remains physically present in the ring after blocking, but its
    // generation/membership metadata must make that entry stale.
    ok = scheduler_test_equal(process_set_state(1, PROC_BLOCKED),
                              PROC_BLOCKED, 150, ok);
    ok = scheduler_test_equal(tier1_enqueue(0), 1, 151, ok);
    ok = scheduler_test_equal(tier1_dequeue(), 0, 152, ok);
    ok = scheduler_test_equal(tier1_dequeue(), 2, 153, ok);
    ok = scheduler_test_equal(tier1_dequeue(), ERR_NOT_FOUND, 154, ok);

    // Unblock publishes once; an explicit second enqueue is idempotent.
    ok = scheduler_test_equal(process_set_state(1, PROC_RUNNABLE),
                              PROC_RUNNABLE, 155, ok);
    ok = scheduler_test_equal(tier1_enqueue(1), 1, 156, ok);
    ok = scheduler_test_equal(tier1_dequeue(), 1, 157, ok);
    ok = scheduler_test_equal(tier1_dequeue(), ERR_NOT_FOUND, 158, ok);

    // A terminal transition invalidates an already queued generation.
    ok = scheduler_test_equal(tier1_enqueue(1), 1, 159, ok);
    ok = scheduler_test_equal(process_set_state(1, PROC_EXITED),
                              PROC_EXITED, 160, ok);
    ok = scheduler_test_equal(tier1_dequeue(), ERR_NOT_FOUND, 161, ok);
    return ok;
}

fn scheduler_test_measure(count: t40, out_addr: t40, ok: t40) -> t40 {
    var warmup: t40 = 0;
    while 2 - warmup > 0 {
        var before: t40 = scheduler_test_cycle();
        var selected: t40 = scheduler_pick_next();
        var after: t40 = scheduler_test_cycle();
        ok = scheduler_test_equal(scheduler_test_nonnegative(selected), 1,
                                  200 + warmup, ok);
        if selected >= 0 {
            tier1_enqueue(selected);
        }
        ok = scheduler_test_equal(scheduler_test_positive(after - before), 1,
                                  210 + warmup, ok);
        warmup = warmup + 1;
    }

    var sample: t40 = 0;
    while 7 - sample > 0 {
        var before: t40 = scheduler_test_cycle();
        var selected: t40 = scheduler_pick_next();
        var after: t40 = scheduler_test_cycle();
        ok = scheduler_test_equal(scheduler_test_nonnegative(selected), 1,
                                  220 + sample, ok);
        kstore(out_addr + sample, after - before);
        if selected >= 0 {
            tier1_enqueue(selected);
        }
        ok = scheduler_test_equal(scheduler_test_positive(after - before), 1,
                                  230 + sample, ok);
        sample = sample + 1;
    }
    return ok;
}

fn scheduler_test_sustained_fairness(count: t40, rounds: t40, out_addr: t40,
                                     ok: t40) -> t40 {
    var i: t40 = 0;
    while rounds - i > 0 {
        var selected: t40 = scheduler_pick_next();
        kstore(out_addr + i, selected);
        ok = scheduler_test_equal(scheduler_test_nonnegative(selected), 1,
                                  300 + i, ok);
        if selected >= 0 {
            tier1_enqueue(selected);
        }
        i = i + 1;
    }
    return ok;
}

fn scheduler_test_map_user(slot: t40) -> t40 {
    var process_row: t40 = process_addr(slot);
    var ctx: t40 = kload(process_row + PROC_CONTEXT);
    var ptbr: t40 = exec_hw_dmem_ptbr(slot);
    var ppn: t40 = exec_process_dmem_ppn(slot);
    kstore(ptbr + USER_SCRATCH_VPN_BASE,
           exec_encode_pte(ppn, 1, 1, 1, 0));
    kstore(ctx + TASK_CONTEXT_DMEM_PTBR, ptbr);
    kstore(ctx + TASK_CONTEXT_DMEM_PAGES,
           USER_SCRATCH_VPN_BASE + USER_SCRATCH_PAGES);
    return 1;
}

fn scheduler_test_prepare_waiters() -> t40 {
    var ok: t40 = scheduler_test_reset();
    ok = scheduler_test_seed_processes(5) * ok;
    var slot: t40 = 0;
    while 5 - slot > 0 {
        scheduler_test_map_user(slot);
        slot = slot + 1;
    }
    ok = scheduler_test_drain(5) * ok;
    return ok;
}

fn scheduler_test_capture_wake(slot: t40, result_addr: t40,
                               wake_result: t40, assertion: t40,
                               ok: t40) -> t40 {
    var first: t40 = tier1_dequeue();
    var second: t40 = tier1_dequeue();
    kstore(result_addr, wake_result);
    kstore(result_addr + 1, first);
    kstore(result_addr + 2, second);
    ok = scheduler_test_equal(wake_result, 1, assertion, ok);
    ok = scheduler_test_equal(first, slot, assertion + 1, ok);
    ok = scheduler_test_equal(second, ERR_NOT_FOUND, assertion + 2, ok);
    return ok;
}

fn scheduler_test_fifo(ok: t40) -> t40 {
    ok = scheduler_test_reset() * ok;
    ok = scheduler_test_seed_processes(4) * ok;
    ok = scheduler_test_drain(4) * ok;
    var slot: t40 = 0;
    while 4 - slot > 0 {
        var blocked: t40 = process_block_on(
            slot, wait_channel_ipc(77), WAIT_KIND_IPC, 0,
            77, USER_MEM_BASE, 0, PROC_BLOCKED);
        ok = scheduler_test_equal(blocked, PROC_BLOCKED, 400 + slot, ok);
        slot = slot + 1;
    }
    ok = scheduler_test_equal(process_wake_channel(wait_channel_ipc(77)),
                              4, 409, ok);
    var pop: t40 = 0;
    while 4 - pop > 0 {
        var selected: t40 = tier1_dequeue();
        kstore(SCHED_TEST_FIFO_ORDER + pop, selected);
        ok = scheduler_test_equal(selected, pop, 410 + pop, ok);
        pop = pop + 1;
    }
    ok = scheduler_test_equal(tier1_dequeue(), ERR_NOT_FOUND, 414, ok);
    ok = scheduler_test_equal(process_wake_channel(wait_channel_ipc(77)),
                              0, 415, ok);
    return ok;
}

fn scheduler_test_unblock_paths(ok: t40) -> t40 {
    // Sleep timeout path.
    ok = scheduler_test_prepare_waiters() * ok;
    var blocked: t40 = process_block_on(
        0, wait_channel_sleep(1), WAIT_KIND_SLEEP, 2,
        0, 0, 0, PROC_SLEEPING);
    ok = scheduler_test_equal(blocked, PROC_SLEEPING, 500, ok);
    kstore(KERNEL_TICK_ADDR, 2);
    var expired: t40 = wait_expire_timeouts();
    ok = scheduler_test_equal(expired, 1, 501, ok);
    ok = scheduler_test_capture_wake(0, SCHED_TEST_WAKE_RESULTS, expired, 502, ok);

    // Futex wake path.
    ok = scheduler_test_prepare_waiters() * ok;
    blocked = process_block_on(
        1, wait_channel_futex_key(77), WAIT_KIND_FUTEX, 0,
        77, 0, 0, PROC_BLOCKED);
    ok = scheduler_test_equal(blocked, PROC_BLOCKED, 510, ok);
    var woke: t40 = wait_wake_futex_key(77, 1);
    ok = scheduler_test_capture_wake(1, SCHED_TEST_WAKE_RESULTS + 3, woke, 511, ok);

    // Blocking IPC wake path, including the user-copy success branch.
    ok = scheduler_test_prepare_waiters() * ok;
    ipc_init();
    ipc_open(7, 0, 0);
    blocked = process_block_on(
        2, wait_channel_ipc(7), WAIT_KIND_IPC, 0,
        7, USER_MEM_BASE, 0, PROC_BLOCKED);
    ok = scheduler_test_equal(blocked, PROC_BLOCKED, 520, ok);
    var sent: t40 = ipc_send(7, 0, 777);
    ok = scheduler_test_capture_wake(2, SCHED_TEST_WAKE_RESULTS + 6, sent, 521, ok);

    // Window event wake path, including event delivery into mapped user memory.
    ok = scheduler_test_prepare_waiters() * ok;
    window_table_init();
    var window_id: t40 = window_create(4, 0, 0, 2, 2);
    var window_slot: t40 = window_find(window_id);
    blocked = process_block_on(
        3, wait_channel_window(window_id), WAIT_KIND_WINDOW, 0,
        window_id, USER_MEM_BASE, 0, PROC_BLOCKED);
    ok = scheduler_test_equal(blocked, PROC_BLOCKED, 530, ok);
    var queued: t40 = window_queue_event(
        window_slot, EVENT_KIND_KEY, 65, 0, 0);
    ok = scheduler_test_equal(queued, 1, 531, ok);
    ok = scheduler_test_capture_wake(3, SCHED_TEST_WAKE_RESULTS + 9, queued, 532, ok);

    // Generic timer expiry path (non-sleep wait kind).
    ok = scheduler_test_prepare_waiters() * ok;
    blocked = process_block_on(
        4, wait_channel_waitpid(999), WAIT_KIND_LEGACY, 1,
        0, 0, 0, PROC_BLOCKED);
    ok = scheduler_test_equal(blocked, PROC_BLOCKED, 540, ok);
    kstore(KERNEL_TICK_ADDR, 1);
    expired = wait_expire_timeouts();
    ok = scheduler_test_equal(expired, 1, 541, ok);
    ok = scheduler_test_capture_wake(4, SCHED_TEST_WAKE_RESULTS + 12, expired, 542, ok);

    // Waitpid completion path.
    ok = scheduler_test_prepare_waiters() * ok;
    blocked = process_block_on(
        0, wait_channel_waitpid(999), WAIT_KIND_LEGACY, 0,
        0, 0, 0, PROC_BLOCKED);
    ok = scheduler_test_equal(blocked, PROC_BLOCKED, 550, ok);
    woke = wait_wake_waitpid(999, 27);
    ok = scheduler_test_capture_wake(0, SCHED_TEST_WAKE_RESULTS + 15, woke, 551, ok);
    return ok;
}

fn main() -> t40 {
    var ok: t40 = 1;

    ok = scheduler_test_reset() * ok;
    ok = scheduler_test_seed_processes(1) * ok;
    ok = scheduler_test_measure(1, SCHED_TEST_ONE_SAMPLES, ok);

    ok = scheduler_test_reset() * ok;
    ok = scheduler_test_seed_processes(100) * ok;
    ok = scheduler_test_measure(100, SCHED_TEST_MANY_SAMPLES, ok);

    ok = scheduler_test_reset() * ok;
    ok = scheduler_test_seed_processes(100) * ok;
    ok = scheduler_test_sustained_fairness(
        100, 1000, SCHED_TEST_FAIRNESS_TRACE, ok);

    ok = scheduler_test_stale_membership(ok);
    ok = scheduler_test_fifo(ok);
    ok = scheduler_test_unblock_paths(ok);
    return ok;
}
)TRIT";

const char* const kIdleDriver = R"TRIT(
const SCHED_IDLE_RESULTS: t40 = 37200;

fn main() -> t40 {
    scheduler_init();
    process_table_init();
    process_create(0, 1, 0, 1, 0, 0);
    tier1_dequeue();
    process_block_on(0, wait_channel_sleep(1), WAIT_KIND_SLEEP, 1,
                     0, 0, 0, PROC_SLEEPING);

    // With no runnable slots, kernel_schedule_next must reconcile once and
    // retire architectural WAIT. The C++ harness resumes this VM only via
    // resumeFromEvent(), after which the timer wheel makes slot zero runnable.
    var selected_pid: t40 = kernel_schedule_next();
    kstore(SCHED_IDLE_RESULTS, selected_pid);
    kstore(SCHED_IDLE_RESULTS + 1, tier1_dequeue());
    kstore(SCHED_IDLE_RESULTS + 2, tier1_dequeue());
    return 1;
}
)TRIT";

std::string diagnostics(const std::vector<sandbox::compiler::Diagnostic>& values) {
    if (values.empty()) return "<none>";
    std::ostringstream out;
    for (const auto& value : values) out << value.format() << "\n";
    return out.str();
}

long long wordAt(sandbox::vm::VMState& vm, int address) {
    auto [value, fault] = vm.dmem.load(address);
    if (fault != sandbox::vm::MemFaultCode::OK) return -999999;
    return sandbox::vm::ops::toLong(value);
}

struct LinkedImage {
    CompileResult compiled;
    LinkResult linked;
};

LinkedImage buildImage(const std::string& driver, const std::string& name) {
    LinkedImage image;
    const std::string kernel = tests_next::readText("kernel.trit");
    image.compiled = sandbox::compiler::compileSource(name, kernel + "\n" + driver);
    if (image.compiled.success) {
        image.linked = sandbox::compiler::linkModules({image.compiled.object});
    }
    return image;
}

bool loadImage(TestContext& ctx, const LinkedImage& image,
               sandbox::vm::VMState& vm, const std::string& label) {
    ctx.check(image.compiled.success, label + " compiles");
    if (!image.compiled.success) {
        ctx.fail(label + " diagnostics:\n" + diagnostics(image.compiled.diagnostics));
        return false;
    }
    ctx.check(image.linked.success, label + " links");
    if (!image.linked.success) {
        ctx.fail(label + " link diagnostics:\n" + diagnostics(image.linked.diagnostics));
        return false;
    }
    vm.resetBlockDevice(192);
    ctx.check(sandbox::vm::assembler::loadAndReset(vm, image.linked.assembled),
              label + " loads");
    return true;
}

long long median(std::array<long long, 7> values) {
    std::sort(values.begin(), values.end());
    return values[values.size() / 2];
}

double coefficientOfVariation(const std::array<long long, 7>& values) {
    double mean = 0.0;
    for (long long value : values) mean += static_cast<double>(value);
    mean /= static_cast<double>(values.size());
    if (mean <= 0.0) return 1.0;
    double variance = 0.0;
    for (long long value : values) {
        const double delta = static_cast<double>(value) - mean;
        variance += delta * delta;
    }
    variance /= static_cast<double>(values.size());
    return std::sqrt(variance) / mean;
}

void schedulerScalingFairnessAndWakePaths(TestContext& ctx) {
    const LinkedImage image = buildImage(kCoreDriver, "next_scheduler_acceptance.trit");
    sandbox::vm::VMState vm(sandbox::vm::ProductionProfile::minimum());
    if (!loadImage(ctx, image, vm, "compiled scheduler acceptance driver")) return;

    const auto result = sandbox::vm::run(vm, 50000000);
    if (!result.halted()) {
        std::ostringstream out;
        out << "scheduler acceptance driver did not halt; status="
            << static_cast<int>(result.status) << " pc=" << vm.pc
            << " trap=" << sandbox::vm::ops::toLong(vm.trap_reg);
        ctx.fail(out.str());
        return;
    }

    ctx.equal(sandbox::vm::ops::toLong(vm.regfile.read(13)), 1LL,
              "compiled scheduler acceptance driver returns success");
    if (wordAt(vm, kFailureAddr) != 0) {
        std::ostringstream out;
        out << "first compiled-kernel assertion failed at " << wordAt(vm, kFailureIndexAddr)
            << ": actual=" << wordAt(vm, kFailureActualAddr)
            << " expected=" << wordAt(vm, kFailureExpectedAddr);
        ctx.fail(out.str());
    }

    std::array<long long, 7> one{};
    std::array<long long, 7> many{};
    for (int i = 0; i < 7; ++i) {
        one[static_cast<std::size_t>(i)] = wordAt(vm, kOneSamplesAddr + i);
        many[static_cast<std::size_t>(i)] = wordAt(vm, kManySamplesAddr + i);
        ctx.check(one[static_cast<std::size_t>(i)] > 0,
                  "one-runnable context-switch sample is positive");
        ctx.check(many[static_cast<std::size_t>(i)] > 0,
                  "100-runnable context-switch sample is positive");
    }
    const long long one_median = median(one);
    const long long many_median = median(many);
    std::cout << "scheduler_acceptance one_median_cycles=" << one_median
              << " many_median_cycles=" << many_median
              << " one_cv=" << coefficientOfVariation(one)
              << " many_cv=" << coefficientOfVariation(many) << "\n";
    ctx.check(coefficientOfVariation(one) < 0.03,
              "one-runnable seven-sample cycle distribution has CV below 3%");
    ctx.check(coefficientOfVariation(many) < 0.03,
              "100-runnable seven-sample cycle distribution has CV below 3%");
    ctx.check(many_median * 5 <= one_median * 6,
              "100-runnable median context-switch cost is at most 20% above one-runnable cost");
    ctx.check(many_median * 5 >= one_median * 4,
              "100-runnable median context-switch cost is at most 20% below one-runnable cost");

    for (int i = 0; i < 1000; ++i) {
        const long long selected = wordAt(vm, kFairnessTraceAddr + i);
        ctx.equal(selected, static_cast<long long>(i % 100),
                  "sustained tier-one scheduling remains FIFO and starvation-free");
    }

    for (int i = 0; i < 4; ++i) {
        ctx.equal(wordAt(vm, kFifoOrderAddr + i), static_cast<long long>(i),
                  "wait queue wakes blocked processes FIFO");
    }

    for (int path = 0; path < 6; ++path) {
        const int base = kWakeResultsAddr + path * 3;
        ctx.equal(wordAt(vm, base), 1LL,
                  "wait unblock path reports one completion");
        ctx.equal(wordAt(vm, base + 2), static_cast<long long>(-1),
                  "wait unblock path leaves no duplicate runnable queue entry");
    }
}

void schedulerMemoryLayoutContract(TestContext& ctx) {
    const std::string source = tests_next::readText("kernel.trit");
    ctx.check(!source.empty(), "kernel source is available for scheduler layout proof");
    ctx.contains(source, "kclear(WAIT_QUEUE_TAIL_BASE, WAIT_QUEUE_MAX);",
                 "wait queue tail range is initialized independently");

    const auto wait_queue_base = tritConstant(source, "WAIT_QUEUE_BASE");
    const auto wait_queue_words = tritConstant(source, "WAIT_QUEUE_WORDS");
    const auto wait_queue_max = tritConstant(source, "WAIT_QUEUE_MAX");
    const auto wait_proc_next_base = tritConstant(source, "WAIT_PROC_NEXT_BASE");
    const auto wait_tail_base = tritConstant(source, "WAIT_QUEUE_TAIL_BASE");
    const auto process_max = tritConstant(source, "PROCESS_MAX");
    const auto timer_buckets = tritConstant(source, "TIMER_WHEEL_BUCKETS");
    const auto timer_head_base = tritConstant(source, "TIMER_WHEEL_HEAD_BASE");
    const auto timer_next_base = tritConstant(source, "TIMER_WHEEL_NEXT_BASE");
    const auto wal_capacity = tritConstant(source, "WAL_TX_TRACKER_CAPACITY");
    const auto wal_key_base = tritConstant(source, "WAL_TX_TRACKER_KEY_BASE");
    const auto wal_lsn_base = tritConstant(source, "WAL_TX_TRACKER_LSN_BASE");
    const bool constants_present = wait_queue_base && wait_queue_words &&
        wait_queue_max && wait_proc_next_base && wait_tail_base && process_max &&
        timer_buckets && timer_head_base && timer_next_base && wal_capacity &&
        wal_key_base && wal_lsn_base;
    ctx.check(constants_present, "scheduler layout constants parse from kernel source");
    if (!constants_present) return;

    ctx.equal(*wait_queue_words, 3LL, "wait queue row ABI remains three words");
    ctx.equal(*wait_tail_base, 678000LL,
              "wait queue FIFO tails use the dedicated high-memory range");
    ctx.check(*wait_queue_base + *wait_queue_words * *wait_queue_max <=
                  *wait_proc_next_base,
              "wait queue rows do not overlap per-process next links");
    ctx.check(*timer_head_base + *timer_buckets <= *timer_next_base,
              "timer-wheel head buckets do not overlap process next links");
    ctx.check(*timer_next_base + *process_max <= *wait_tail_base,
              "wait queue tails begin after all timer-wheel metadata");
    ctx.check(*wait_tail_base + *wait_queue_max <= *wal_key_base,
              "wait queue tails do not overlap WAL transaction trackers");
    ctx.check(*wal_key_base + *wal_capacity <= *wal_lsn_base,
              "WAL tracker key and last-LSN ranges do not overlap");

    const std::string pick = tritFunction(source, "fn scheduler_pick_next()");
    ctx.check(!pick.empty(), "scheduler_pick_next source is extractable");
    ctx.contains(pick, "tier1_dequeue()",
                 "ordinary scheduling consumes the authoritative tier-one queue");
    ctx.check(pick.find("PROCESS_MAX") == std::string::npos &&
                  pick.find("process_find_pid") == std::string::npos &&
                  pick.find("macro_reconcile_processes") == std::string::npos,
              "ordinary scheduler path contains no process-table scan or reconciliation");
    const std::string pid_lookup = tritFunction(source, "fn process_find_pid(pid: t40)");
    ctx.contains(pid_lookup, "pid_index_find(pid)",
                 "process lookup uses the 243-entry PID index");
    ctx.check(pid_lookup.find("while") == std::string::npos,
              "process_find_pid does not scan the process table");

    JsonValue schema;
    JsonValue fixture;
    std::string schema_error;
    std::string fixture_error;
    ctx.check(parseJsonFile("SCHEDULER_ACCEPTANCE_SCHEMA.json", schema, schema_error),
              "scheduler schema is strict valid JSON: " + schema_error);
    ctx.check(parseJsonFile("benchmarks/reference/scheduler-acceptance.v1.json",
                            fixture, fixture_error),
              "scheduler reference is strict valid JSON: " + fixture_error);
    if (schema.kind != JsonValue::Kind::object ||
        fixture.kind != JsonValue::Kind::object) {
        ctx.fail("scheduler schema/reference roots must be JSON objects");
        return;
    }

    checkJsonStringField(ctx, schema, "$id", "trit.scheduler_acceptance.v1",
                         "scheduler schema has the authoritative id");
    const JsonValue* schema_required = jsonMember(schema, "required");
    bool root_required = schema_required != nullptr;
    for (const char* field : {"schema", "workload", "protocol", "thresholds",
                              "curve", "evidence"}) {
        root_required = root_required && arrayContainsString(*schema_required, field);
    }
    ctx.check(root_required, "scheduler schema requires every top-level section");
    const JsonValue* schema_properties = jsonMember(schema, "properties");
    const JsonValue* schema_protocol = schema_properties == nullptr ? nullptr :
        jsonMember(*schema_properties, "protocol");
    const JsonValue* protocol_properties = schema_protocol == nullptr ? nullptr :
        jsonMember(*schema_protocol, "properties");
    const JsonValue* cost_schema = protocol_properties == nullptr ? nullptr :
        jsonMember(*protocol_properties, "cost_metric");
    const JsonValue* normalization_schema = protocol_properties == nullptr ? nullptr :
        jsonMember(*protocol_properties, "normalization");
    const JsonValue* protocol_required = schema_protocol == nullptr ? nullptr :
        jsonMember(*schema_protocol, "required");
    bool protocol_fields = protocol_required != nullptr;
    for (const char* field : {"warmups", "iterations", "maximum_accepted_cv",
                              "cost_metric", "normalization"}) {
        protocol_fields = protocol_fields && arrayContainsString(*protocol_required, field);
    }
    ctx.check(protocol_fields, "schema requires the complete sampling protocol");
    const auto schemaConst = [](const JsonValue* properties,
                                const std::string& field) -> const JsonValue* {
        if (properties == nullptr) return nullptr;
        const JsonValue* property = jsonMember(*properties, field);
        return property == nullptr ? nullptr : jsonMember(*property, "const");
    };
    const JsonValue* warmups_schema = schemaConst(protocol_properties, "warmups");
    const JsonValue* iterations_schema = schemaConst(protocol_properties, "iterations");
    const JsonValue* cv_schema = schemaConst(protocol_properties, "maximum_accepted_cv");
    const JsonValue* wall_time_schema = schemaConst(protocol_properties, "wall_time");
    ctx.check(warmups_schema != nullptr && jsonNumber(*warmups_schema, 2),
              "schema fixes two warmups");
    ctx.check(iterations_schema != nullptr && jsonNumber(*iterations_schema, 7),
              "schema fixes seven measured samples");
    ctx.check(cv_schema != nullptr && jsonNumber(*cv_schema, 0.03),
              "schema fixes the three-percent CV threshold");
    ctx.check(wall_time_schema != nullptr && jsonBoolean(*wall_time_schema, false),
              "schema rejects host wall-time evidence");
    ctx.check(cost_schema != nullptr && jsonMember(*cost_schema, "const") != nullptr &&
                  jsonString(*jsonMember(*cost_schema, "const"),
                             "normalized_architectural_cycles"),
              "schema fixes the normalized architectural-cycle metric");
    ctx.check(normalization_schema != nullptr &&
                  jsonMember(*normalization_schema, "const") != nullptr &&
                  jsonString(*jsonMember(*normalization_schema, "const"),
                             "one_runnable_median_equals_1"),
              "schema fixes the one-runnable normalization rule");
    const JsonValue* threshold_schema = schema_properties == nullptr ? nullptr :
        jsonMember(*schema_properties, "thresholds");
    const JsonValue* threshold_properties = threshold_schema == nullptr ? nullptr :
        jsonMember(*threshold_schema, "properties");
    for (const auto& expected : std::array<std::pair<const char*, double>, 5>{{
             {"median_ratio_min", 0.8}, {"median_ratio_max", 1.2},
             {"max_starvation_rounds", 100}, {"max_duplicate_enqueues", 0},
             {"ordinary_process_table_scans", 0}}}) {
        const JsonValue* value = schemaConst(threshold_properties, expected.first);
        ctx.check(value != nullptr && jsonNumber(*value, expected.second),
                  std::string("schema fixes threshold ") + expected.first);
    }
    const JsonValue* curve_schema = schema_properties == nullptr ? nullptr :
        jsonMember(*schema_properties, "curve");
    const JsonValue* min_items = curve_schema == nullptr ? nullptr :
        jsonMember(*curve_schema, "minItems");
    ctx.check(min_items != nullptr && jsonNumber(*min_items, 8),
              "schema requires all eight scaling points");
    const JsonValue* item_schema = curve_schema == nullptr ? nullptr :
        jsonMember(*curve_schema, "items");
    const JsonValue* item_properties = item_schema == nullptr ? nullptr :
        jsonMember(*item_schema, "properties");
    const JsonValue* median_units = schemaConst(item_properties, "median_cost_units");
    const JsonValue* scan_units = schemaConst(item_properties, "process_table_scans");
    ctx.check(median_units != nullptr && jsonNumber(*median_units, 1),
              "schema fixes normalized median units to one");
    ctx.check(scan_units != nullptr && jsonNumber(*scan_units, 0),
              "schema fixes ordinary process-table scans to zero");

    checkJsonStringField(ctx, fixture, "schema", "trit.scheduler_acceptance.v1",
                         "scheduler fixture declares the authoritative schema");
    checkJsonStringField(ctx, fixture, "workload", "tier1-context-switch",
                         "scheduler fixture names the tier-one workload");
    const JsonValue* protocol = jsonMember(fixture, "protocol");
    const JsonValue* thresholds = jsonMember(fixture, "thresholds");
    const JsonValue* curve = jsonMember(fixture, "curve");
    const JsonValue* evidence = jsonMember(fixture, "evidence");
    ctx.check(protocol != nullptr && protocol->kind == JsonValue::Kind::object,
              "scheduler fixture protocol is an object");
    ctx.check(thresholds != nullptr && thresholds->kind == JsonValue::Kind::object,
              "scheduler fixture thresholds are an object");
    ctx.check(curve != nullptr && curve->kind == JsonValue::Kind::array,
              "scheduler fixture curve is an array");
    ctx.check(evidence != nullptr && evidence->kind == JsonValue::Kind::object,
              "scheduler fixture evidence is an object");
    if (protocol == nullptr || thresholds == nullptr || curve == nullptr ||
        evidence == nullptr) return;
    checkJsonNumberField(ctx, *protocol, "warmups", 2,
                         "scheduler fixture keeps two warmups");
    checkJsonNumberField(ctx, *protocol, "iterations", 7,
                         "scheduler fixture keeps seven measured samples");
    checkJsonNumberField(ctx, *protocol, "maximum_accepted_cv", 0.03,
                         "scheduler fixture keeps the three-percent CV gate");
    checkJsonStringField(ctx, *protocol, "cost_metric",
                         "normalized_architectural_cycles",
                         "scheduler fixture uses normalized architectural cycles");
    checkJsonStringField(ctx, *protocol, "normalization",
                         "one_runnable_median_equals_1",
                         "scheduler fixture names its normalization rule");
    checkJsonBooleanField(ctx, *protocol, "wall_time", false,
                          "scheduler fixture rejects wall-time evidence");
    checkJsonNumberField(ctx, *thresholds, "median_ratio_min", 0.8,
                         "scheduler fixture enforces the lower ratio bound");
    checkJsonNumberField(ctx, *thresholds, "median_ratio_max", 1.2,
                         "scheduler fixture enforces the upper ratio bound");
    checkJsonNumberField(ctx, *thresholds, "max_starvation_rounds", 100,
                         "scheduler fixture bounds starvation by one rotation");
    checkJsonNumberField(ctx, *thresholds, "max_duplicate_enqueues", 0,
                         "scheduler fixture rejects duplicate runnable entries");
    checkJsonNumberField(ctx, *thresholds, "ordinary_process_table_scans", 0,
                         "scheduler fixture requires zero ordinary table scans");

    const std::array<int, 8> expected_slots{{1, 2, 4, 8, 16, 32, 64, 100}};
    ctx.equal(static_cast<long long>(curve->array.size()), 8LL,
              "scheduler curve contains exactly eight scaling points");
    if (curve->array.size() == expected_slots.size()) {
        for (std::size_t i = 0; i < expected_slots.size(); ++i) {
            const JsonValue& point = curve->array[i];
            checkJsonNumberField(ctx, point, "runnable_slots", expected_slots[i],
                                 "scheduler curve preserves ordered slot count");
            checkJsonNumberField(ctx, point, "median_cost_units", 1,
                                 "scheduler curve normalizes median cost to one");
            checkJsonNumberField(ctx, point, "process_table_scans", 0,
                                 "scheduler curve records zero table scans");
            checkJsonNumberField(ctx, point, "max_wait_rounds", expected_slots[i],
                                 "scheduler curve bounds wait by one rotation");
        }
    }
    checkJsonStringField(ctx, *evidence, "target", "next_scheduler_acceptance",
                         "scheduler evidence names the executable target");
    checkJsonBooleanField(ctx, *evidence, "compiled_kernel", true,
                          "scheduler evidence requires compiled-kernel execution");
    checkJsonStringField(ctx, *evidence, "production_gate", "ci_production",
                         "scheduler evidence is attached to the production gate");
}

void schedulerIdleWaitAndEventResume(TestContext& ctx) {
    const LinkedImage image = buildImage(kIdleDriver, "next_scheduler_idle_acceptance.trit");
    sandbox::vm::VMState vm(sandbox::vm::ProductionProfile::minimum());
    if (!loadImage(ctx, image, vm, "compiled scheduler idle driver")) return;

    const auto waiting = sandbox::vm::run(vm, 1000000);
    ctx.check(waiting.status == sandbox::vm::VMStatus::WAITING && vm.isWaiting(),
              "empty compiled-kernel scheduler enters architectural WAITING");
    ctx.check(vm.cycle_count > 0, "architectural WAIT retires measurable cycles");
    vm.resumeFromEvent();
    const auto resumed = sandbox::vm::run(vm, 1000000);
    ctx.check(resumed.halted(), "architectural event resumes compiled-kernel scheduler");
    if (!resumed.halted()) return;
    ctx.equal(sandbox::vm::ops::toLong(vm.regfile.read(13)), 1LL,
              "resumed idle scheduler driver returns success");
    ctx.equal(wordAt(vm, kIdleResultsAddr), 1LL,
              "timer wake selects the sleeping process after WAIT");
    ctx.equal(wordAt(vm, kIdleResultsAddr + 1), 0LL,
              "resumed scheduler enqueues the woken slot exactly once");
    ctx.equal(wordAt(vm, kIdleResultsAddr + 2), static_cast<long long>(-1),
              "resumed scheduler queue is empty after one dequeue");
}

} // namespace

int main() {
    sandbox::LongTriple::initPowTable();
    const std::vector<TestCase> cases = {
        {"scheduler.acceptance.memory_layout",
         "process.ipc.scheduler_quantitative_acceptance",
         schedulerMemoryLayoutContract},
        {"scheduler.acceptance.scaling_fairness_wake_paths",
         "process.ipc.scheduler_quantitative_acceptance",
         schedulerScalingFairnessAndWakePaths},
        {"scheduler.acceptance.idle_wait_event_resume",
         "process.ipc.scheduler_quantitative_acceptance",
         schedulerIdleWaitAndEventResume},
    };
    return tests_next::runCases("next_scheduler_acceptance", cases);
}
