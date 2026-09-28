#include "ternary_compiler.h"
#include "ternary_vm.h"

#include <cstdlib>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>

namespace {

std::string readSource(const std::string& name) {
    std::ifstream input(std::string(TRIT_SOURCE_DIR) + "/" + name);
    std::ostringstream contents;
    contents << input.rdbuf();
    return contents.str();
}

long long returnValue(const sandbox::vm::VMState& vm) {
    return sandbox::vm::ops::toLong(vm.regfile.read(13));
}

} // namespace

int main() {
    sandbox::LongTriple::initPowTable();

    const std::string driver = R"(
        fn check(actual: t40, expected: t40, code: t40) -> t40 {
            match actual - expected {
                zero => { return 0; }
                _ => { return code; }
            }
        }

        fn main() -> t40 {
            var failed: t40 = 0;

            // Scheduler construction rejects invalid capacity.
            failed = check(df_sched_new(0), 0, 10);
            match failed { zero => {} _ => { return failed; } }
            failed = check(df_sched_new(1), 0, 14);
            match failed { zero => {} _ => { return failed; } }

            // Node pools are bounded and independently owned by schedulers.
            var sched_a: t40 = df_sched_new(3);
            var sched_b: t40 = df_sched_new(3);
            var a1: t40 = df_node_new(sched_a, 0, DF_STREAM_KIND_FILTER, 0);
            var a2: t40 = df_node_new(sched_a, 0, DF_STREAM_KIND_FILTER, 0);
            var a3: t40 = df_node_new(sched_a, 0, DF_STREAM_KIND_FILTER, 0);
            var a4: t40 = df_node_new(sched_a, 0, DF_STREAM_KIND_FILTER, 0);
            failed = check(a4, 0, 11);
            match failed { zero => {} _ => { return failed; } }
            var b1: t40 = df_node_new(sched_b, 0, DF_STREAM_KIND_FILTER, 0);
            match b1 { pos => {} _ => { return 12; } }
            df_sched_free(sched_a);
            var b2: t40 = df_node_new(sched_b, 0, DF_STREAM_KIND_FILTER, 0);
            match b2 { pos => {} _ => { return 13; } }

            // Vector and node wiring APIs report success or failure.
            var zero_vec: t40 = df_vec_new_sized(0);
            failed = check(df_vec_push_safe(zero_vec, 7), 1, 20);
            match failed { zero => {} _ => { return failed; } }
            failed = check(df_vec_push_safe(0, 8), 0, 21);
            match failed { zero => {} _ => { return failed; } }
            failed = check(vec_get(zero_vec, 0), 7, 22);
            match failed { zero => {} _ => { return failed; } }

            var sched: t40 = df_sched_new(27);
            var left: t40 = df_node_new(sched, 0, DF_STREAM_KIND_FILTER, 0);
            var right: t40 = df_node_new(sched, 0, DF_STREAM_KIND_FILTER, 0);
            failed = check(df_node_add_dependent(left, right), 1, 23);
            match failed { zero => {} _ => { return failed; } }

            // Validation and topo traversal preserve scheduler state.
            unsafe {
                store(left, 1);
                store(right, 0 - 1);
            }
            var nodes: t40 = vec_new();
            vec_push(nodes, left);
            vec_push(nodes, right);
            failed = check(df_graph_validate(nodes), 0, 30);
            match failed { zero => {} _ => { return failed; } }
            unsafe {
                failed = check(load(left), 1, 31);
                match failed { zero => {} _ => { return failed; } }
                failed = check(load(right), 0 - 1, 32);
                match failed { zero => {} _ => { return failed; } }
            }

            // Topological sorting detects cycles itself and returns no order.
            df_node_add_dependent(right, left);
            failed = check(df_graph_validate(nodes), 0 - 1, 33);
            match failed { zero => {} _ => { return failed; } }
            failed = check(df_graph_topo_sort(nodes), 0, 34);
            match failed { zero => {} _ => { return failed; } }
            unsafe {
                failed = check(load(left), 1, 35);
                match failed { zero => {} _ => { return failed; } }
            }

            // Tagged FIFO channels preserve order and all t40 payload values.
            var chan: t40 = df_channel_new(5);
            var event_value: t40 = malloc_raw(1);
            df_channel_write(chan, 0 - 7);
            df_channel_write(chan, 0);
            df_channel_write(chan, 5);
            df_channel_write_signal(chan, DF_EVENT_EOS);
            failed = check(df_channel_read(chan, event_value), DF_EVENT_DATA, 40);
            match failed { zero => {} _ => { return failed; } }
            unsafe { failed = check(load(event_value), 0 - 7, 41); }
            match failed { zero => {} _ => { return failed; } }
            failed = check(df_channel_read(chan, event_value), DF_EVENT_DATA, 42);
            match failed { zero => {} _ => { return failed; } }
            unsafe { failed = check(load(event_value), 0, 43); }
            match failed { zero => {} _ => { return failed; } }
            failed = check(df_channel_read(chan, event_value), DF_EVENT_DATA, 44);
            match failed { zero => {} _ => { return failed; } }
            unsafe { failed = check(load(event_value), 5, 45); }
            match failed { zero => {} _ => { return failed; } }
            failed = check(df_channel_read(chan, event_value), DF_EVENT_EOS, 46);
            match failed { zero => {} _ => { return failed; } }

            // A full ready queue does not mutate an unqueued node.
            var small_sched: t40 = df_sched_new(2);
            var queued: t40 = df_node_new(small_sched, 0, 9, 0);
            var rejected: t40 = df_node_new(small_sched, 0, 9, 0);
            unsafe { store(rejected, 1); store(rejected + 5, 7); }
            failed = check(df_sched_submit(small_sched, queued), 1, 50);
            match failed { zero => {} _ => { return failed; } }
            failed = check(df_sched_submit(small_sched, rejected), 0, 51);
            match failed { zero => {} _ => { return failed; } }
            unsafe {
                failed = check(load(rejected), 1, 52);
                match failed { zero => {} _ => { return failed; } }
                failed = check(load(rejected + 5), 7, 53);
                match failed { zero => {} _ => { return failed; } }
            }

            var priority_sched: t40 = df_sched_new(2);
            var urgent_queued: t40 = df_node_new(priority_sched, 0, 9, 0);
            var urgent_rejected: t40 = df_node_new(priority_sched, 0, 9, 0);
            unsafe { store(urgent_rejected, 1); store(urgent_rejected + 5, 8); }
            failed = check(df_sched_submit_priority(priority_sched, urgent_queued), 1, 57);
            match failed { zero => {} _ => { return failed; } }
            failed = check(df_sched_submit_priority(priority_sched, urgent_rejected), 0, 58);
            match failed { zero => {} _ => { return failed; } }
            unsafe {
                failed = check(load(urgent_rejected), 1, 59);
                match failed { zero => {} _ => { return failed; } }
                failed = check(load(urgent_rejected + 5), 8, 65);
                match failed { zero => {} _ => { return failed; } }
            }

            // Unknown kinds fail dispatch and are cancelled by the scheduler.
            failed = check(df_node_execute(rejected), 0 - 1, 54);
            match failed { zero => {} _ => { return failed; } }
            failed = check(df_sched_run_loop(small_sched), 0 - 1, 55);
            match failed { zero => {} _ => { return failed; } }
            unsafe { failed = check(load(queued), 0 - 1, 56); }
            match failed { zero => {} _ => { return failed; } }

            // Source EOS is explicit, ordered, and emitted only once.
            var source_ctx: t40 = malloc_raw(3);
            unsafe {
                store(source_ctx, 0);
                store(source_ctx + 1, 6);
                store(source_ctx + 2, 0);
            }
            var source_chan: t40 = df_channel_new(4);
            var source: t40 = df_node_new(sched, 0, DF_STREAM_KIND_SOURCE, source_ctx);
            df_node_add_output(source, source_chan);
            failed = check(df_node_execute(source), 1, 60);
            match failed { zero => {} _ => { return failed; } }
            failed = check(df_channel_read(source_chan, event_value), DF_EVENT_EOS, 61);
            match failed { zero => {} _ => { return failed; } }
            df_node_execute(source);
            failed = check(df_channel_read(source_chan, event_value), DF_EVENT_NONE, 62);
            match failed { zero => {} _ => { return failed; } }

            // Negative data remains data through stream processing.
            var filter_in: t40 = df_channel_new(4);
            var filter_out: t40 = df_channel_new(4);
            var filter: t40 = df_node_new(sched, 0, DF_STREAM_KIND_FILTER, 0 - 10);
            df_node_add_input(filter, filter_in);
            df_node_add_output(filter, filter_out);
            df_channel_write(filter_in, 0 - 7);
            df_node_execute(filter);
            failed = check(df_channel_read(filter_out, event_value), DF_EVENT_DATA, 63);
            match failed { zero => {} _ => { return failed; } }
            unsafe { failed = check(load(event_value), 0 - 7, 64); }
            match failed { zero => {} _ => { return failed; } }

            // Candidate baselines reject isolated outliers, accept equality,
            // and adapt only after a consistent three-sample shift.
            var hist: t40 = malloc_raw(10);
            unsafe {
                store(hist, 10); store(hist + 1, 10); store(hist + 2, 10);
                store(hist + 3, 3); store(hist + 4, 10);
                store(hist + 5, 0); store(hist + 6, 0); store(hist + 7, 0);
                store(hist + 8, 0); store(hist + 9, 3);
            }
            var anomaly_in: t40 = df_channel_new(12);
            var anomaly_out: t40 = df_channel_new(12);
            var anomaly: t40 = df_node_new(sched, 0, DF_STREAM_KIND_ANOMALY, hist);
            df_node_add_input(anomaly, anomaly_in);
            df_node_add_output(anomaly, anomaly_out);

            df_channel_write(anomaly_in, 20);
            df_node_execute(anomaly);
            failed = check(df_channel_read(anomaly_out, event_value), DF_EVENT_DATA, 70);
            match failed { zero => {} _ => { return failed; } }

            unsafe {
                store(hist, 10); store(hist + 1, 10); store(hist + 2, 10);
                store(hist + 3, 3); store(hist + 8, 0);
            }
            df_channel_write(anomaly_in, 100);
            df_node_execute(anomaly);
            failed = check(df_channel_read(anomaly_out, event_value), DF_EVENT_ANOMALY, 71);
            match failed { zero => {} _ => { return failed; } }
            df_channel_write(anomaly_in, 10);
            df_node_execute(anomaly);
            failed = check(df_channel_read(anomaly_out, event_value), DF_EVENT_DATA, 72);
            match failed { zero => {} _ => { return failed; } }
            unsafe { failed = check(load(hist + 8), 0, 73); }
            match failed { zero => {} _ => { return failed; } }

            var i: t40 = 0;
            while 3 - i > 0 {
                df_channel_write(anomaly_in, 100);
                df_node_execute(anomaly);
                failed = check(df_channel_read(anomaly_out, event_value), DF_EVENT_ANOMALY, 74);
                match failed { zero => {} _ => { return failed; } }
                i = i + 1;
            }
            df_channel_write(anomaly_in, 100);
            df_node_execute(anomaly);
            failed = check(df_channel_read(anomaly_out, event_value), DF_EVENT_DATA, 75);
            match failed { zero => {} _ => { return failed; } }

            return 1;
        }
    )";

    const std::string source =
        readSource("ulib.trit") + "\n" +
        readSource("ternary_dataflow.trit") + "\n" +
        readSource("ternary_stream.trit") + "\n" + driver;

    auto compiled = sandbox::compiler::compileSource("dataflow_stream_test.trit", source);
    if (!compiled.success) {
        for (const auto& diagnostic : compiled.diagnostics) {
            std::cerr << diagnostic.format() << "\n";
        }
        return EXIT_FAILURE;
    }

    auto linked = sandbox::compiler::linkModules({compiled.object});
    if (!linked.success) {
        for (const auto& diagnostic : linked.diagnostics) {
            std::cerr << diagnostic.format() << "\n";
        }
        return EXIT_FAILURE;
    }

    sandbox::vm::VMState vm(131072, 2000000);
    if (!sandbox::vm::assembler::loadAndReset(vm, linked.assembled)) {
        std::cerr << "failed to load dataflow stream image\n";
        return EXIT_FAILURE;
    }
    const auto result = sandbox::vm::run(vm, 1500000);
    if (!result.halted() || returnValue(vm) != 1) {
        std::cerr << "dataflow stream runtime failure: status="
                  << static_cast<int>(result.status)
                  << " return=" << returnValue(vm)
                  << " pc=" << vm.pc << " cause=" << vm.cause << "\n";
        return EXIT_FAILURE;
    }

    std::cout << "dataflow and stream regressions passed\n";
    return EXIT_SUCCESS;
}
