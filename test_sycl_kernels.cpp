#define TERNARY_ENABLE_SYCL
#include "ternary_gpu_kernels.h"
#include "ternary_device_allocators.h"

#include <iostream>
#include <fstream>
#include <vector>
#include <chrono>
#include <iomanip>
#include <sycl/sycl.hpp>

using namespace sandbox;
using namespace sandbox::backend;
using namespace sandbox::backend::gpu;
using namespace sandbox::vm;

int main() {
    std::cout << "Starting SYCL Lane Launch Wrapper Tuning Benchmark..." << std::endl;
    
    try {
        sycl::queue q(sycl::default_selector_v);
        std::string deviceName = q.get_device().get_info<sycl::info::device::name>();
        std::cout << "Running on: " << deviceName << std::endl;
        
        SyclSharedVMStateAllocator allocator(q);
        
        // Use a significantly large count to get measurable times
        const std::size_t count = 10000000; // 10 million operations
        const int trits = 20;

        uint64_t* a = sycl::malloc_shared<uint64_t>(count, q);
        uint64_t* b = sycl::malloc_shared<uint64_t>(count, q);
        uint64_t* out = sycl::malloc_shared<uint64_t>(count, q);

        for (std::size_t i = 0; i < count; ++i) {
            a[i] = 0xAAAAAAAAAAAAAAAAULL;
            b[i] = 0x5555555555555555ULL;
            out[i] = 0;
        }

        std::ofstream mdFile("tuning_results.md", std::ios_base::app);
        if (!mdFile.is_open()) {
            std::cerr << "Failed to open tuning_results.md for writing!" << std::endl;
            return 1;
        }

        mdFile << "## SYCL Geometry Tuning Results\n";
        mdFile << "- **Device:** `" << deviceName << "`\n";
        mdFile << "- **Payload Count:** `" << count << "` lanes\n";
        mdFile << "- **Operation:** `addLane64 (20 trits)`\n\n";
        mdFile << "| Local Size | Execution Time (us) |\n";
        mdFile << "| ---------- | ------------------- |\n";

        std::vector<int> sizes_to_test = {0, 32, 64, 128, 256, 512, 1024};
        
        for (int localSize : sizes_to_test) {
            std::cout << "Testing localSize = " << (localSize == 0 ? "Auto" : std::to_string(localSize)) << "..." << std::flush;
            
            // Warmup
            syclLaunchTritwiseAddRaw64(q, a, b, out, count, trits, localSize).wait();

            // Measure
            auto start = std::chrono::high_resolution_clock::now();
            syclLaunchTritwiseAddRaw64(q, a, b, out, count, trits, localSize).wait();
            auto end = std::chrono::high_resolution_clock::now();
            
            auto duration_us = std::chrono::duration_cast<std::chrono::microseconds>(end - start).count();
            
            std::cout << " " << duration_us << " us\n";
            
            std::string sizeLabel = (localSize == 0) ? "Auto" : std::to_string(localSize);
            mdFile << "| " << sizeLabel << " | " << duration_us << " |\n";
        }
        
        mdFile << "\n";
        mdFile.close();

        std::cout << "SYCL benchmark complete. Results appended to tuning_results.md" << std::endl;

        sycl::free(a, q);
        sycl::free(b, q);
        sycl::free(out, q);
    } catch (sycl::exception const& e) {
        std::cerr << "SYCL Exception: " << e.what() << std::endl;
        return 1;
    }

    return 0;
}
