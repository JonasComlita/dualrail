cmake_minimum_required(VERSION 3.20)
project(TernaryGPUBackendValidation LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

add_executable(test_tiny_transformer_runtime test_tiny_transformer_runtime.cpp)
target_compile_features(test_tiny_transformer_runtime PUBLIC cxx_std_17)
target_compile_definitions(test_tiny_transformer_runtime PRIVATE TERNARY_IGNORE_LONG_DOUBLE_ASSERT)

add_executable(test_bitnet_loading test_bitnet_loading.cpp)
add_executable(run_bitnet run_bitnet.cpp)
target_compile_features(test_bitnet_loading PUBLIC cxx_std_17)
target_compile_definitions(test_bitnet_loading PRIVATE TERNARY_IGNORE_LONG_DOUBLE_ASSERT)

target_compile_features(run_bitnet PUBLIC cxx_std_17)
target_compile_definitions(run_bitnet PRIVATE TERNARY_IGNORE_LONG_DOUBLE_ASSERT)

if(MSVC)
    target_compile_options(run_bitnet PRIVATE /arch:AVX2)
else()
    target_compile_options(run_bitnet PRIVATE -mavx2)
endif()

add_executable(test_ternary_ir test_ternary_ir.cpp)
target_compile_features(test_ternary_ir PUBLIC cxx_std_17)
target_compile_definitions(test_ternary_ir PRIVATE TERNARY_IGNORE_LONG_DOUBLE_ASSERT)

add_executable(benchmark_architecture benchmark_architecture.cpp)
target_compile_features(benchmark_architecture PUBLIC cxx_std_17)
target_compile_definitions(benchmark_architecture PRIVATE TERNARY_IGNORE_LONG_DOUBLE_ASSERT)

# ------------------------------------------------------------------------------
# CUDA Validation
# ------------------------------------------------------------------------------
option(ENABLE_CUDA "Enable CUDA validation test" OFF)
if(ENABLE_CUDA)
    enable_language(CUDA)
    add_executable(test_cuda_kernels test_cuda_kernels.cu)
    target_compile_features(test_cuda_kernels PUBLIC cxx_std_17)
    target_compile_definitions(test_cuda_kernels PRIVATE TERNARY_IGNORE_LONG_DOUBLE_ASSERT)
    # Set CUDA architecture or rely on default
    # set_target_properties(test_cuda_kernels PROPERTIES CUDA_ARCHITECTURES "80;89")
endif()

# ------------------------------------------------------------------------------
# SYCL Validation
# ------------------------------------------------------------------------------
option(ENABLE_SYCL "Enable SYCL validation test" OFF)
if(ENABLE_SYCL)
    find_package(IntelSYCL QUIET)
    add_executable(test_sycl_kernels test_sycl_kernels.cpp)
    target_compile_options(test_sycl_kernels PRIVATE -fsycl -U_CONTAINER_DEBUG_LEVEL)
    target_link_options(test_sycl_kernels PRIVATE -fsycl)
    target_compile_definitions(test_sycl_kernels PRIVATE TERNARY_IGNORE_LONG_DOUBLE_ASSERT _ALLOW_COMPILER_AND_STL_VERSION_MISMATCH)
    if(MSVC)
        target_compile_options(test_sycl_kernels PRIVATE /EHsc)
    endif()
endif()

message(STATUS "Build Configuration:")
message(STATUS "  CUDA Enabled: ${ENABLE_CUDA}")
message(STATUS "  SYCL Enabled: ${ENABLE_SYCL}")
