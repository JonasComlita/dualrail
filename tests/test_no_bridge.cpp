#include "test_multiwidth_vm_common.h"

void testNoBridgeInExecutionHeaders() {
    std::cout << "[11] static no-bridge scan\n";
    const std::vector<std::string> files = {
        "ternary_native_ops.h",
        "ternary_backend.h",
        "ternary_kernel.h",
        "ternary_gpu_kernels.h",
        "ternary_lanes.h",
        "ternary_simd.h",
        "ternary_device_allocators.h",
        "ternary_isa.h",
        "ternary_vm_state.h",
        "ternary_vm.h",
        "ternary_asm.h",
        "ternary_transformer_runtime.h",
    };
    const std::vector<std::string> banned = {
        "long_ops::decode",
        "long_ops::encode",
        "fromDouble",
        "toDouble",
        "std::pow",
        "std::sqrt",
        "long double",
    };

    for (const auto& file : files) {
        std::ifstream in(file);
        expect(in.good(), "open " + file);
        std::string line;
        while (std::getline(in, line)) {
            const auto comment = line.find("//");
            if (comment != std::string::npos) line = line.substr(0, comment);
            for (const auto& token : banned) {
                expect(line.find(token) == std::string::npos,
                       file + " production path contains bridge token " + token);
            }
        }
    }

    {
        std::ifstream in("ternary_native_ops.h");
        expect(in.good(), "open ternary_native_ops.h for native scratch scan");
        std::string line;
        while (std::getline(in, line)) {
            const std::size_t comment = line.find("//");
            if (comment != std::string::npos) line = line.substr(0, comment);
            expect(line.find("__int128") == std::string::npos,
                   "ternary_native_ops.h reintroduced compiler __int128 scratch arithmetic");
        }
    }
}
