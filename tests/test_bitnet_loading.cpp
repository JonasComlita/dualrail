#include "bitnet_weights/bitnet_loader.h"
#include "ternary_transformer_runtime.h"
#include <iostream>
#include <iomanip>

using namespace sandbox;

int main() {
    LongTriple::initPowTable();

    std::string baseDir = "../bitnet_weights/converted";
    bitnet::BitNetLoader loader;
    
    std::cout << "--- BitNet Diagnostic Loader ---" << std::endl;
    if (!loader.init(baseDir)) {
        std::cerr << "Error: Could not initialize loader from " << baseDir << std::endl;
        return 1;
    }

    size_t totalParams = loader.getTotalWeightCount();
    std::cout << "Manifest loaded. Total parameters in model: " << totalParams << std::endl;
    std::cout << "Estimated memory if fully loaded: " << (totalParams * 24 / (1024 * 1024)) << " MB" << std::endl;

    // Allocate a safe amount of DMEM (60 million words = 1.4 GB)
    int safeDmemSize = 60000000;
    std::cout << "Allocating " << (safeDmemSize * 24 / (1024 * 1024)) << " MB for DMEM..." << std::endl;
    
    vm::VMState vm(4096, safeDmemSize);
    std::cout << "VM initialized." << std::endl;

    // Try loading only the embedding layer
    std::string embedLayer = "model.embed_tokens.weight";
    auto layerInfo = loader.getLayer(embedLayer);
    
    if (layerInfo.name.empty()) {
        std::cerr << "Error: Layer " << embedLayer << " not found in manifest." << std::endl;
        return 1;
    }

    std::cout << "Loading embedding sample: " << embedLayer << " (1M weights)..." << std::endl;
    // We manually limit loading to 1 million weights to save space for Step 1
    int loaded = loader.loadLayerToVM(vm, embedLayer, 0, 1000000);
    
    if (loaded == 0) {
        std::cerr << "Error: Failed to load weights." << std::endl;
        return 1;
    }

    std::cout << "Successfully loaded " << loaded << " weights." << std::endl;

    // Sample first 10 weights
    std::cout << "\nFirst 10 weights in DMEM (T40):" << std::endl;
    for (int i = 0; i < 10; ++i) {
        auto [val, fault] = vm.dmem.load(i);
        if (fault != vm::MemFaultCode::OK) break;
        
        std::cout << "  [" << i << "]: ";
        if (val.mode == TernaryMode::T40) {
            auto lt = val.toLongTriple();
            auto decoded = long_ops::decode(lt);
            std::cout << "Mode=T40, Value=" << (double)decoded.first * std::pow(3.0, decoded.second);
        } else {
            std::cout << "Mode=" << (int)val.mode << " (Raw bits: " << std::hex << val.bits.hi << " " << val.bits.lo << std::dec << ")";
        }
        std::cout << std::endl;
    }

    // --- STEP 1: Verify L1 Layer ---
    std::string l1Layer = "model.layers.0.mlp.down_proj.weight";
    std::cout << "\n--- Step 1: Loading Ternary Layer: " << l1Layer << " ---" << std::endl;
    
    // Load at offset after embedding layer to avoid overwrite
    int l1Addr = loaded;
    int l1Loaded = loader.loadLayerToVM(vm, l1Layer, l1Addr);
    
    if (l1Loaded == 0) {
        std::cerr << "Error: Failed to load ternary layer." << std::endl;
    } else {
        std::cout << "Successfully loaded " << l1Loaded << " ternary weights." << std::endl;
        std::cout << "First 10 ternary weights in DMEM (L1):" << std::endl;
        for (int i = 0; i < 10; ++i) {
            auto [val, fault] = vm.dmem.load(l1Addr + i);
            if (fault != vm::MemFaultCode::OK) break;
            
            auto l1 = val.asL1();
            int8_t trit = l1.tritAt(0);
            std::cout << "  [" << (l1Addr + i) << "]: Mode=L1, Trit=" << (int)trit << std::endl;
        }
    }

    // --- STEP 2: Run a Matmul ---
    std::cout << "\n--- Step 2: Running Sample Matmul ---" << std::endl;
    
    // We'll perform a 1 x 128 x 128 matmul for speed/safety
    // Weight matrix A starts at l1Addr (Row-major 128x128 subset)
    // Input vector B starts at l1Addr + l1Loaded
    // Output vector C starts at l1Addr + l1Loaded + 128
    int m = 1, n = 128, k = 128;
    int bAddr = l1Addr + l1Loaded;
    int cAddr = bAddr + n * k; // Ensure space for B if it were a matrix

    std::cout << "Initializing random input vector B at 0x" << std::hex << bAddr << std::dec << "..." << std::endl;
    for (int i = 0; i < k; ++i) {
        // Use ops::fromDouble to get a Triple, then promote to LongTriple
        LongTriple lt = native_ops::toLongTriple(ops::fromDouble(0.1 * (i % 10)));
        vm.dmem.store(bAddr + i, vm::TernaryValue::fromLongTriple(lt));
    }

    std::cout << "Executing matmul kernel (L50 Packed)..." << std::endl;
    // Wrap memory in TensorViews
    transformer_runtime::TensorView viewA = { l1Addr, n, k, TernaryMode::L50 };
    transformer_runtime::TensorView viewB = { bAddr, k, m, TernaryMode::T40 };
    transformer_runtime::TensorView viewOut = { cAddr, n, m, TernaryMode::T40 };

    transformer_runtime::matmulAccumulator(vm, viewA, viewB, viewOut);

    std::cout << "Matmul complete. First 5 output values:" << std::endl;
    for (int i = 0; i < 5; ++i) {
        auto [val, fault] = vm.dmem.load(cAddr + i);
        if (fault != vm::MemFaultCode::OK) break;
        
        auto lt = val.toLongTriple();
        auto decoded = long_ops::decode(lt);
        std::cout << "  Output[" << i << "]: " << (double)decoded.first * std::pow(3.0, decoded.second) << std::endl;
    }

    return 0;
}
