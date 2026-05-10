#define TERNARY_ENABLE_CUDA
#include "ternary_gpu_kernels.h"
#include "ternary_device_allocators.h"

#include <iostream>
#include <fstream>
#include <vector>
#include <string>

using namespace sandbox;
using namespace sandbox::backend;
using namespace sandbox::backend::gpu;
using namespace sandbox::vm;

int main() {
    std::cout << "Starting CUDA Lane Launch Wrapper Tuning Benchmark..." << std::endl;
    
    // Check for CUDA device
    int deviceCount = 0;
    cudaError_t err = cudaGetDeviceCount(&deviceCount);
    if (err != cudaSuccess || deviceCount == 0) {
        std::cerr << "No CUDA devices found. Exiting benchmark." << std::endl;
        return 1;
    }

    cudaDeviceProp prop;
    cudaGetDeviceProperties(&prop, 0);
    std::string deviceName = prop.name;
    std::cout << "Running on: " << deviceName << std::endl;

    CudaManagedVMStateAllocator allocator;
    
    // Use a significantly large count to get measurable times
    const std::size_t count = 10000000; // 10 million operations
    const int trits = 20;

    uint64_t* a = nullptr;
    uint64_t* b = nullptr;
    uint64_t* out = nullptr;

    cudaMallocManaged(&a, count * sizeof(uint64_t));
    cudaMallocManaged(&b, count * sizeof(uint64_t));
    cudaMallocManaged(&out, count * sizeof(uint64_t));

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

    mdFile << "## CUDA Geometry Tuning Results\n";
    mdFile << "- **Device:** `" << deviceName << "`\n";
    mdFile << "- **Payload Count:** `" << count << "` lanes\n";
    mdFile << "- **Operation:** `addLane64 (20 trits)`\n\n";
    mdFile << "| Block Size | Execution Time (us) |\n";
    mdFile << "| ---------- | ------------------- |\n";

    std::vector<int> sizes_to_test = {32, 64, 128, 256, 512, 1024};
    
    cudaEvent_t start, stop;
    cudaEventCreate(&start);
    cudaEventCreate(&stop);

    for (int blockSize : sizes_to_test) {
        std::cout << "Testing blockSize = " << blockSize << "..." << std::flush;
        
        // Warmup
        cudaLaunchTritwiseAddRaw64(a, b, out, count, trits, blockSize);
        cudaDeviceSynchronize();

        // Measure
        cudaEventRecord(start);
        cudaLaunchTritwiseAddRaw64(a, b, out, count, trits, blockSize);
        cudaEventRecord(stop);
        cudaEventSynchronize(stop);
        
        float milliseconds = 0;
        cudaEventElapsedTime(&milliseconds, start, stop);
        int duration_us = static_cast<int>(milliseconds * 1000.0f);
        
        std::cout << " " << duration_us << " us\n";
        mdFile << "| " << blockSize << " | " << duration_us << " |\n";
    }
    
    mdFile << "\n";
    mdFile.close();

    cudaEventDestroy(start);
    cudaEventDestroy(stop);

    std::cout << "CUDA benchmark complete. Results appended to tuning_results.md" << std::endl;

    cudaFree(a);
    cudaFree(b);
    cudaFree(out);

    return 0;
}
