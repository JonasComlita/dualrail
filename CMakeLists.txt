cmake_minimum_required(VERSION 3.20)
project(TernaryGPUBackendValidation LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

include_directories(${CMAKE_CURRENT_SOURCE_DIR})
include_directories(${CMAKE_CURRENT_SOURCE_DIR}/bitnet_weights)
include_directories(${CMAKE_CURRENT_SOURCE_DIR}/qwen3.627b_weights)

# ------------------------------------------------------------------------------
# Vulkan Integration
# ------------------------------------------------------------------------------
find_package(Vulkan REQUIRED)

find_program(GLSLC_EXECUTABLE NAMES glslc HINTS $ENV{VULKAN_SDK}/Bin)
if(GLSLC_EXECUTABLE)
    message(STATUS "Found glslc: ${GLSLC_EXECUTABLE}")
else()
    message(WARNING "glslc not found. Cannot compile shaders.")
endif()

# Define shaders
set(SHADERS 
    qwen3.627b_weights/shaders/matmul_int8.comp
    qwen3.627b_weights/shaders/geglu.comp
)
set(SPV_FILES "")
foreach(SHADER ${SHADERS})
    get_filename_component(SHADER_NAME ${SHADER} NAME)
    set(SPV_FILE "${CMAKE_CURRENT_BINARY_DIR}/${SHADER_NAME}.spv")
    list(APPEND SPV_FILES ${SPV_FILE})
    add_custom_command(
        OUTPUT ${SPV_FILE}
        COMMAND ${GLSLC_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/${SHADER} -o ${SPV_FILE}
        DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/${SHADER}
        COMMENT "Compiling shader ${SHADER_NAME} to SPIR-V..."
    )
endforeach()

add_custom_target(compile_shaders DEPENDS ${SPV_FILES})

add_executable(test_multiwidth_vm
    tests/test_multiwidth_vm_main.cpp
    tests/test_uint128.cpp
    tests/test_formats.cpp
    tests/test_isa_asm.cpp
    tests/test_vm_widths.cpp
    tests/test_kernel.cpp
    tests/test_no_bridge.cpp
    tests/test_hello_world.cpp
)
target_compile_features(test_multiwidth_vm PUBLIC cxx_std_17)
target_compile_definitions(test_multiwidth_vm PRIVATE TERNARY_IGNORE_LONG_DOUBLE_ASSERT)

add_executable(test_ternary_lanes tests/test_ternary_lanes.cpp)
target_compile_features(test_ternary_lanes PUBLIC cxx_std_17)
target_compile_definitions(test_ternary_lanes PRIVATE TERNARY_IGNORE_LONG_DOUBLE_ASSERT)

add_executable(test_native_ops tests/test_native_ops.cpp)
target_compile_features(test_native_ops PUBLIC cxx_std_17)
target_compile_definitions(test_native_ops PRIVATE TERNARY_IGNORE_LONG_DOUBLE_ASSERT)

add_executable(test_numeric_workloads tests/test_numeric_workloads.cpp)
target_compile_features(test_numeric_workloads PUBLIC cxx_std_17)
target_compile_definitions(test_numeric_workloads PRIVATE TERNARY_IGNORE_LONG_DOUBLE_ASSERT)

add_executable(test_tiny_transformer_runtime tests/test_tiny_transformer_runtime.cpp)
target_compile_features(test_tiny_transformer_runtime PUBLIC cxx_std_17)
target_compile_definitions(test_tiny_transformer_runtime PRIVATE TERNARY_IGNORE_LONG_DOUBLE_ASSERT)

add_executable(test_bitnet_loading tests/test_bitnet_loading.cpp)
add_executable(run_bitnet bitnet_weights/run_bitnet.cpp)
add_executable(run_qwen qwen3.627b_weights/run_qwen.cpp)
add_executable(run_kernel run_kernel.cpp)
add_executable(run_gui_console run_gui_console.cpp)
target_compile_features(test_bitnet_loading PUBLIC cxx_std_17)
target_compile_definitions(test_bitnet_loading PRIVATE TERNARY_IGNORE_LONG_DOUBLE_ASSERT)

target_compile_features(run_bitnet PUBLIC cxx_std_17)
target_compile_definitions(run_bitnet PRIVATE TERNARY_IGNORE_LONG_DOUBLE_ASSERT)

target_compile_features(run_qwen PUBLIC cxx_std_17)
target_compile_definitions(run_qwen PRIVATE TERNARY_IGNORE_LONG_DOUBLE_ASSERT)
target_link_libraries(run_qwen PRIVATE Vulkan::Vulkan)
add_dependencies(run_qwen compile_shaders)

target_compile_features(run_kernel PUBLIC cxx_std_17)
target_compile_definitions(run_kernel PRIVATE TERNARY_IGNORE_LONG_DOUBLE_ASSERT)

target_compile_features(run_gui_console PUBLIC cxx_std_17)
target_compile_definitions(run_gui_console PRIVATE TERNARY_IGNORE_LONG_DOUBLE_ASSERT)

add_executable(test_ternary_ir tests/test_ternary_ir.cpp)
target_compile_features(test_ternary_ir PUBLIC cxx_std_17)
target_compile_definitions(test_ternary_ir PRIVATE TERNARY_IGNORE_LONG_DOUBLE_ASSERT)

add_executable(test_phase7_compiler tests/test_phase7_compiler.cpp)
target_compile_features(test_phase7_compiler PUBLIC cxx_std_17)
target_compile_definitions(test_phase7_compiler PRIVATE TERNARY_IGNORE_LONG_DOUBLE_ASSERT)

add_executable(test_os_platform tests/test_os_platform.cpp)
target_compile_features(test_os_platform PUBLIC cxx_std_17)
target_compile_definitions(test_os_platform PRIVATE TERNARY_IGNORE_LONG_DOUBLE_ASSERT)

add_executable(run_os_demo run_os_demo.cpp)
target_compile_features(run_os_demo PUBLIC cxx_std_17)
target_compile_definitions(run_os_demo PRIVATE TERNARY_IGNORE_LONG_DOUBLE_ASSERT)

add_executable(tritc tritc.cpp)
target_compile_features(tritc PUBLIC cxx_std_17)
target_compile_definitions(tritc PRIVATE TERNARY_IGNORE_LONG_DOUBLE_ASSERT)

add_executable(benchmark_architecture benchmark_architecture.cpp)
target_compile_features(benchmark_architecture PUBLIC cxx_std_17)
target_compile_definitions(benchmark_architecture PRIVATE TERNARY_IGNORE_LONG_DOUBLE_ASSERT)

# ------------------------------------------------------------------------------
# CUDA Validation
# ------------------------------------------------------------------------------
option(ENABLE_CUDA "Enable CUDA validation test" OFF)
if(ENABLE_CUDA)
    enable_language(CUDA)
    add_executable(test_cuda_kernels tests/test_cuda_kernels.cu)
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
    add_executable(test_sycl_kernels tests/test_sycl_kernels.cpp)
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
