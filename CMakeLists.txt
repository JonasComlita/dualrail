cmake_minimum_required(VERSION 3.20)
project(TernaryGPUBackendValidation LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
enable_testing()

add_compile_definitions(NOMINMAX)

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

# Define shaders when optional Qwen assets are present.
set(SHADERS "")
if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/qwen3.627b_weights/shaders/matmul_int8.comp")
    list(APPEND SHADERS qwen3.627b_weights/shaders/matmul_int8.comp)
endif()
if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/qwen3.627b_weights/shaders/geglu.comp")
    list(APPEND SHADERS qwen3.627b_weights/shaders/geglu.comp)
endif()
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
add_executable(build_tos_image build_tos_image.cpp)
target_compile_features(test_bitnet_loading PUBLIC cxx_std_17)
target_compile_definitions(test_bitnet_loading PRIVATE TERNARY_IGNORE_LONG_DOUBLE_ASSERT)

target_compile_features(run_bitnet PUBLIC cxx_std_17)
target_compile_definitions(run_bitnet PRIVATE TERNARY_IGNORE_LONG_DOUBLE_ASSERT)

if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/qwen3.627b_weights/run_qwen.cpp")
    add_executable(run_qwen qwen3.627b_weights/run_qwen.cpp)
    target_compile_features(run_qwen PUBLIC cxx_std_17)
    target_compile_definitions(run_qwen PRIVATE TERNARY_IGNORE_LONG_DOUBLE_ASSERT)
    target_link_libraries(run_qwen PRIVATE Vulkan::Vulkan)
    add_dependencies(run_qwen compile_shaders)
else()
    message(STATUS "Qwen runner source not found; run_qwen target will not be built")
endif()

target_compile_features(build_tos_image PUBLIC cxx_std_17)
target_compile_definitions(build_tos_image PRIVATE TERNARY_IGNORE_LONG_DOUBLE_ASSERT)

find_package(SDL2 CONFIG QUIET)
if(SDL2_FOUND)
    add_executable(run_tos_sdl run_tos_sdl.cpp)
    target_compile_features(run_tos_sdl PUBLIC cxx_std_17)
    target_compile_definitions(run_tos_sdl PRIVATE TERNARY_IGNORE_LONG_DOUBLE_ASSERT)
    if(TARGET SDL2::SDL2main)
        target_link_libraries(run_tos_sdl PRIVATE SDL2::SDL2main)
    endif()
    if(TARGET SDL2::SDL2)
        target_link_libraries(run_tos_sdl PRIVATE SDL2::SDL2)
    elseif(TARGET SDL2::SDL2-static)
        target_link_libraries(run_tos_sdl PRIVATE SDL2::SDL2-static)
    else()
        target_include_directories(run_tos_sdl PRIVATE ${SDL2_INCLUDE_DIRS})
        target_link_libraries(run_tos_sdl PRIVATE ${SDL2_LIBRARIES})
    endif()

    set(TOS_RELEASE_VERSION "dev" CACHE STRING "Version label embedded in staged Ternary OS images")
    set(TOS_RELEASE_ROOT "${CMAKE_CURRENT_BINARY_DIR}/release")
    set(TOS_RELEASE_DIR "${TOS_RELEASE_ROOT}/TernaryOS")
    get_filename_component(MINGW_RUNTIME_DIR "${CMAKE_CXX_COMPILER}" DIRECTORY)
    set(TOS_RELEASE_RUNTIME_DLLS
        "${MINGW_RUNTIME_DIR}/SDL2.dll"
        "${MINGW_RUNTIME_DIR}/libgcc_s_seh-1.dll"
        "${MINGW_RUNTIME_DIR}/libstdc++-6.dll"
        "${MINGW_RUNTIME_DIR}/libwinpthread-1.dll"
    )
    foreach(TOS_DLL ${TOS_RELEASE_RUNTIME_DLLS})
        if(NOT EXISTS "${TOS_DLL}")
            message(WARNING "Expected runtime DLL not found: ${TOS_DLL}")
        endif()
    endforeach()

    add_custom_target(stage_tos_release
        COMMAND ${CMAKE_COMMAND} -E rm -rf "${TOS_RELEASE_DIR}"
        COMMAND ${CMAKE_COMMAND} -E make_directory "${TOS_RELEASE_DIR}"
        COMMAND ${CMAKE_COMMAND} -E make_directory "${TOS_RELEASE_DIR}/diagnostics"
        COMMAND $<TARGET_FILE:build_tos_image>
                "${TOS_RELEASE_DIR}/ternary-os.tboot"
                "${TOS_RELEASE_DIR}/ternary-os.tdisk"
                "${TOS_RELEASE_VERSION}"
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
                "$<TARGET_FILE:run_tos_sdl>"
                "${TOS_RELEASE_DIR}/TernaryOS.exe"
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
                ${TOS_RELEASE_RUNTIME_DLLS}
                "${TOS_RELEASE_DIR}"
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
                "${CMAKE_CURRENT_SOURCE_DIR}/packaging/windows/README.txt"
                "${TOS_RELEASE_DIR}/README.txt"
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
                "${CMAKE_CURRENT_SOURCE_DIR}/packaging/windows/LICENSES.txt"
                "${TOS_RELEASE_DIR}/LICENSES.txt"
        DEPENDS build_tos_image run_tos_sdl
        WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
        USES_TERMINAL
    )

    add_custom_target(smoke_tos_release
        COMMAND "${TOS_RELEASE_DIR}/TernaryOS.exe"
                --smoke-test
                --frames 10
                "${TOS_RELEASE_DIR}/ternary-os.tboot"
                "${TOS_RELEASE_DIR}/ternary-os.tdisk"
        DEPENDS stage_tos_release
        WORKING_DIRECTORY "${TOS_RELEASE_DIR}"
        USES_TERMINAL
    )

    add_custom_target(package_tos_release
        COMMAND ${CMAKE_COMMAND} -E rm -f "${TOS_RELEASE_ROOT}/TernaryOS-windows.zip"
        COMMAND ${CMAKE_COMMAND} -E tar cf
                "${TOS_RELEASE_ROOT}/TernaryOS-windows.zip"
                --format=zip
                "TernaryOS"
        DEPENDS stage_tos_release
        WORKING_DIRECTORY "${TOS_RELEASE_ROOT}"
        USES_TERMINAL
    )
else()
    message(STATUS "SDL2 not found; run_tos_sdl target will not be built")
endif()

add_executable(test_ternary_ir tests/test_ternary_ir.cpp)
target_compile_features(test_ternary_ir PUBLIC cxx_std_17)
target_compile_definitions(test_ternary_ir PRIVATE TERNARY_IGNORE_LONG_DOUBLE_ASSERT)

add_executable(test_phase7_compiler tests/test_phase7_compiler.cpp)
target_compile_features(test_phase7_compiler PUBLIC cxx_std_17)
target_compile_definitions(test_phase7_compiler PRIVATE TERNARY_IGNORE_LONG_DOUBLE_ASSERT)

add_executable(test_phase_c5_pred tests/test_phase_c5_pred.cpp)
target_compile_features(test_phase_c5_pred PUBLIC cxx_std_17)
target_compile_definitions(test_phase_c5_pred PRIVATE TERNARY_IGNORE_LONG_DOUBLE_ASSERT)

add_executable(test_phase_d_kernel tests/test_phase_d_kernel.cpp)
target_compile_features(test_phase_d_kernel PUBLIC cxx_std_17)
target_compile_definitions(test_phase_d_kernel PRIVATE TERNARY_IGNORE_LONG_DOUBLE_ASSERT)

add_executable(test_layer1_hal tests/test_layer1_hal.cpp)
target_compile_features(test_layer1_hal PUBLIC cxx_std_17)
target_compile_definitions(test_layer1_hal PRIVATE TERNARY_IGNORE_LONG_DOUBLE_ASSERT)

add_executable(test_production_layers tests/test_production_layers.cpp)
target_compile_features(test_production_layers PUBLIC cxx_std_17)
target_compile_definitions(test_production_layers PRIVATE TERNARY_IGNORE_LONG_DOUBLE_ASSERT)

add_executable(test_production_hardening tests/test_production_hardening.cpp)
target_compile_features(test_production_hardening PUBLIC cxx_std_17)
target_compile_definitions(test_production_hardening PRIVATE TERNARY_IGNORE_LONG_DOUBLE_ASSERT)

add_executable(test_native_apps tests/test_native_apps.cpp)
target_compile_features(test_native_apps PUBLIC cxx_std_17)
target_compile_definitions(test_native_apps PRIVATE TERNARY_IGNORE_LONG_DOUBLE_ASSERT)

add_executable(test_consumer_shell_productization tests/test_consumer_shell_productization.cpp)
target_compile_features(test_consumer_shell_productization PUBLIC cxx_std_17)
target_compile_definitions(test_consumer_shell_productization PRIVATE TERNARY_IGNORE_LONG_DOUBLE_ASSERT)

add_executable(test_process_handoff tests/test_process_handoff.cpp)
target_compile_features(test_process_handoff PUBLIC cxx_std_17)
target_compile_definitions(test_process_handoff PRIVATE TERNARY_IGNORE_LONG_DOUBLE_ASSERT)

add_executable(test_os_platform tests/test_os_platform.cpp)
target_compile_features(test_os_platform PUBLIC cxx_std_17)
target_compile_definitions(test_os_platform PRIVATE TERNARY_IGNORE_LONG_DOUBLE_ASSERT)

add_executable(test_scaling_profile_compact tests/test_scaling_profile.cpp)
target_compile_features(test_scaling_profile_compact PUBLIC cxx_std_17)
target_compile_definitions(test_scaling_profile_compact PRIVATE TERNARY_IGNORE_LONG_DOUBLE_ASSERT)

add_executable(test_host_runtime tests/test_host_runtime.cpp)
target_compile_features(test_host_runtime PUBLIC cxx_std_17)
target_compile_definitions(test_host_runtime PRIVATE TERNARY_IGNORE_LONG_DOUBLE_ASSERT)

find_package(Python3 COMPONENTS Interpreter QUIET)
if(Python3_Interpreter_FOUND)
    add_test(NAME test_agent_tooling
             COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/tests/test_agent_tooling.py
             WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR})
    add_custom_target(agent_doctor
        COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/tools/trit_tool.py doctor
        WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
        USES_TERMINAL
    )
endif()

add_test(NAME test_production_hardening
         COMMAND test_production_hardening
         WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR})

add_custom_target(production_profile
    DEPENDS
        test_production_layers
        test_production_hardening
        test_os_platform
        test_scaling_profile_compact
        test_consumer_shell_productization
        test_host_runtime
)

add_custom_target(ci_production
    COMMAND $<TARGET_FILE:test_production_layers>
    COMMAND $<TARGET_FILE:test_production_hardening>
    COMMAND $<TARGET_FILE:test_os_platform>
    COMMAND $<TARGET_FILE:test_scaling_profile_compact>
    COMMAND $<TARGET_FILE:test_consumer_shell_productization>
    COMMAND $<TARGET_FILE:test_host_runtime>
    DEPENDS production_profile
    WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
    USES_TERMINAL
)

add_executable(tritc tritc.cpp)
target_compile_features(tritc PUBLIC cxx_std_17)
target_compile_definitions(tritc PRIVATE TERNARY_IGNORE_LONG_DOUBLE_ASSERT)

add_executable(trit_ast_dump tools/trit_ast_dump.cpp)
target_compile_features(trit_ast_dump PUBLIC cxx_std_17)
target_compile_definitions(trit_ast_dump PRIVATE TERNARY_IGNORE_LONG_DOUBLE_ASSERT)

if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/benchmark_architecture.cpp")
    add_executable(benchmark_architecture benchmark_architecture.cpp)
    target_compile_features(benchmark_architecture PUBLIC cxx_std_17)
    target_compile_definitions(benchmark_architecture PRIVATE TERNARY_IGNORE_LONG_DOUBLE_ASSERT)
endif()

add_executable(test_benchmark tests/test_benchmark.cpp)
target_compile_features(test_benchmark PUBLIC cxx_std_17)
target_compile_definitions(test_benchmark PRIVATE TERNARY_IGNORE_LONG_DOUBLE_ASSERT)

add_executable(test_trace_jit_benchmark tests/test_trace_jit_benchmark.cpp)
target_compile_features(test_trace_jit_benchmark PUBLIC cxx_std_17)
target_compile_definitions(test_trace_jit_benchmark PRIVATE TERNARY_IGNORE_LONG_DOUBLE_ASSERT)

add_executable(test_malloc_micro tests/test_malloc_micro.cpp)
target_compile_features(test_malloc_micro PUBLIC cxx_std_17)
target_compile_definitions(test_malloc_micro PRIVATE TERNARY_IGNORE_LONG_DOUBLE_ASSERT)

add_executable(test_tcl_asm tests/test_tcl_asm.cpp)
target_compile_features(test_tcl_asm PUBLIC cxx_std_17)
target_compile_definitions(test_tcl_asm PRIVATE TERNARY_IGNORE_LONG_DOUBLE_ASSERT)

add_executable(next_isa_encoding tests_next/01_isa/next_isa_encoding.cpp)
target_compile_features(next_isa_encoding PUBLIC cxx_std_17)
target_compile_definitions(next_isa_encoding PRIVATE TERNARY_IGNORE_LONG_DOUBLE_ASSERT)

add_executable(next_assembler_goldens tests_next/03_assembler/next_assembler_goldens.cpp)
target_compile_features(next_assembler_goldens PUBLIC cxx_std_17)
target_compile_definitions(next_assembler_goldens PRIVATE TERNARY_IGNORE_LONG_DOUBLE_ASSERT)

add_executable(next_assembler_tcl_parity tests_next/03_assembler/next_assembler_tcl_parity.cpp)
target_compile_features(next_assembler_tcl_parity PUBLIC cxx_std_17)
target_compile_definitions(next_assembler_tcl_parity PRIVATE TERNARY_IGNORE_LONG_DOUBLE_ASSERT)

add_executable(next_compiler_pipeline tests_next/04_compiler/next_compiler_pipeline.cpp)
target_compile_features(next_compiler_pipeline PUBLIC cxx_std_17)
target_compile_definitions(next_compiler_pipeline PRIVATE TERNARY_IGNORE_LONG_DOUBLE_ASSERT)

add_executable(next_compiler_native_ulib tests_next/04_compiler/next_compiler_native_ulib.cpp)
target_compile_features(next_compiler_native_ulib PUBLIC cxx_std_17)
target_compile_definitions(next_compiler_native_ulib PRIVATE TERNARY_IGNORE_LONG_DOUBLE_ASSERT)

add_executable(next_compiler_phase_c5_pred tests_next/04_compiler/next_compiler_phase_c5_pred.cpp)
target_compile_features(next_compiler_phase_c5_pred PUBLIC cxx_std_17)
target_compile_definitions(next_compiler_phase_c5_pred PRIVATE TERNARY_IGNORE_LONG_DOUBLE_ASSERT)

add_executable(next_vm_core tests_next/02_vm/next_vm_core.cpp)
target_compile_features(next_vm_core PUBLIC cxx_std_17)
target_compile_definitions(next_vm_core PRIVATE TERNARY_IGNORE_LONG_DOUBLE_ASSERT)

add_executable(next_kernel_native_os tests_next/07_kernel/next_kernel_native_os.cpp)
target_compile_features(next_kernel_native_os PUBLIC cxx_std_17)
target_compile_definitions(next_kernel_native_os PRIVATE TERNARY_IGNORE_LONG_DOUBLE_ASSERT)

add_executable(next_vfs_persistence tests_next/08_filesystem_vfs/next_vfs_persistence.cpp)
target_compile_features(next_vfs_persistence PUBLIC cxx_std_17)
target_compile_definitions(next_vfs_persistence PRIVATE TERNARY_IGNORE_LONG_DOUBLE_ASSERT)

add_executable(next_process_ipc_scheduler tests_next/09_process_ipc_scheduler/next_process_ipc_scheduler.cpp)
target_compile_features(next_process_ipc_scheduler PUBLIC cxx_std_17)
target_compile_definitions(next_process_ipc_scheduler PRIVATE TERNARY_IGNORE_LONG_DOUBLE_ASSERT)

add_custom_target(tests_next_fast
    COMMAND $<TARGET_FILE:next_isa_encoding>
    COMMAND $<TARGET_FILE:next_assembler_goldens>
    COMMAND $<TARGET_FILE:next_assembler_tcl_parity>
    COMMAND $<TARGET_FILE:next_vm_core>
    COMMAND $<TARGET_FILE:next_compiler_pipeline>
    DEPENDS
        next_isa_encoding
        next_assembler_goldens
        next_assembler_tcl_parity
        next_vm_core
        next_compiler_pipeline
    WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
    USES_TERMINAL
)

add_custom_target(tests_next_compiler_full
    COMMAND $<TARGET_FILE:next_compiler_pipeline>
    COMMAND $<TARGET_FILE:next_compiler_native_ulib>
    COMMAND $<TARGET_FILE:next_compiler_phase_c5_pred>
    DEPENDS
        next_compiler_pipeline
        next_compiler_native_ulib
        next_compiler_phase_c5_pred
    WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
    USES_TERMINAL
)

add_custom_target(tests_next_phase_d
    COMMAND $<TARGET_FILE:next_kernel_native_os>
    COMMAND $<TARGET_FILE:next_vfs_persistence>
    COMMAND $<TARGET_FILE:next_process_ipc_scheduler>
    DEPENDS
        next_kernel_native_os
        next_vfs_persistence
        next_process_ipc_scheduler
    WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
    USES_TERMINAL
)

add_test(NAME next_isa_encoding
         COMMAND next_isa_encoding
         WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR})
add_test(NAME next_assembler_goldens
         COMMAND next_assembler_goldens
         WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR})
add_test(NAME next_assembler_tcl_parity
         COMMAND next_assembler_tcl_parity
         WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR})
add_test(NAME next_vm_core
         COMMAND next_vm_core
         WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR})
add_test(NAME next_compiler_pipeline
         COMMAND next_compiler_pipeline
         WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR})
add_test(NAME next_compiler_native_ulib
         COMMAND next_compiler_native_ulib
         WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR})
add_test(NAME next_compiler_phase_c5_pred
         COMMAND next_compiler_phase_c5_pred
         WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR})
add_test(NAME next_kernel_native_os
         COMMAND next_kernel_native_os
         WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR})
add_test(NAME next_vfs_persistence
         COMMAND next_vfs_persistence
         WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR})
add_test(NAME next_process_ipc_scheduler
         COMMAND next_process_ipc_scheduler
         WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR})

add_executable(test_all_apps tests/test_all_apps.cpp)
target_compile_features(test_all_apps PUBLIC cxx_std_17)
target_compile_definitions(test_all_apps PRIVATE TERNARY_IGNORE_LONG_DOUBLE_ASSERT)


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
