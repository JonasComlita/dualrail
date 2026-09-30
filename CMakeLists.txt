cmake_minimum_required(VERSION 3.20)
project(TernaryCurrentPlatform LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

include(CTest)

# The supported build graph is intentionally small and current-only:
# ISA v2 instructions, executable/function ABI v3, tBoot v3, and tDisk v2.
# Historical tests, migration utilities, and accelerator experiments are not
# CMake targets. Keep this file honest: every source named here is part of the
# current checkout and every test is runnable after configuration.
add_compile_definitions(NOMINMAX)
include_directories(${CMAKE_CURRENT_SOURCE_DIR})
include_directories(${CMAKE_CURRENT_SOURCE_DIR}/bitnet_weights)

add_executable(tritc tritc.cpp)
target_compile_features(tritc PRIVATE cxx_std_17)
target_compile_definitions(tritc PRIVATE TERNARY_IGNORE_LONG_DOUBLE_ASSERT)

add_executable(build_tos_image build_tos_image.cpp)
target_compile_features(build_tos_image PRIVATE cxx_std_17)
target_compile_definitions(build_tos_image PRIVATE TERNARY_IGNORE_LONG_DOUBLE_ASSERT)

# Current-platform conformance is kept as a small end-to-end executable. The
# source is checked in with the current test surface; guarding the target also
# keeps a source-only configure useful while a checkout is being repaired.
if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/tests/current_only_conformance.cpp")
    add_executable(trit_current_conformance
        tests/current_only_conformance.cpp)
    target_compile_features(trit_current_conformance PRIVATE cxx_std_17)
    target_compile_definitions(trit_current_conformance
        PRIVATE TERNARY_IGNORE_LONG_DOUBLE_ASSERT)
    if(BUILD_TESTING)
        add_test(NAME trit_current_conformance
                 COMMAND trit_current_conformance
                 WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR})
    endif()

    add_custom_target(current_validate
        COMMAND $<TARGET_FILE:trit_current_conformance>
        DEPENDS trit_current_conformance
        WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
        USES_TERMINAL)
else()
    add_custom_target(current_validate
        COMMAND ${CMAKE_COMMAND} -E echo
                "current conformance source is not present"
        USES_TERMINAL)
endif()

# SDL is optional because the compiler, image builder, and conformance check
# must remain buildable on hosts without a desktop development package.
option(TRIT_ENABLE_SDL_RELEASE
       "Build the optional SDL runner and release-image convenience targets"
       ON)
if(TRIT_ENABLE_SDL_RELEASE)
    find_package(SDL2 CONFIG QUIET)
    if(SDL2_FOUND AND EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/run_tos_sdl.cpp")
        add_executable(run_tos_sdl run_tos_sdl.cpp)
        target_compile_features(run_tos_sdl PRIVATE cxx_std_17)
        target_compile_definitions(run_tos_sdl
            PRIVATE TERNARY_IGNORE_LONG_DOUBLE_ASSERT)
        if(TARGET SDL2::SDL2main)
            target_link_libraries(run_tos_sdl PRIVATE SDL2::SDL2main)
        endif()
        if(TARGET SDL2::SDL2)
            target_link_libraries(run_tos_sdl PRIVATE SDL2::SDL2)
        elseif(TARGET SDL2::SDL2-static)
            target_link_libraries(run_tos_sdl PRIVATE SDL2::SDL2-static)
        elseif(SDL2_LIBRARIES)
            target_include_directories(run_tos_sdl PRIVATE ${SDL2_INCLUDE_DIRS})
            target_link_libraries(run_tos_sdl PRIVATE ${SDL2_LIBRARIES})
        endif()

        set(TOS_RELEASE_VERSION "dev" CACHE STRING
            "Version label embedded in staged Ternary OS images")
        set(TOS_RELEASE_ROOT "${CMAKE_CURRENT_BINARY_DIR}/release")
        set(TOS_RELEASE_DIR "${TOS_RELEASE_ROOT}/TernaryOS")

        add_custom_target(stage_tos_release
            COMMAND ${CMAKE_COMMAND} -E rm -rf "${TOS_RELEASE_DIR}"
            COMMAND ${CMAKE_COMMAND} -E make_directory "${TOS_RELEASE_DIR}"
            COMMAND ${CMAKE_COMMAND} -E make_directory
                    "${TOS_RELEASE_DIR}/diagnostics"
            COMMAND $<TARGET_FILE:build_tos_image>
                    "${TOS_RELEASE_DIR}/ternary-os.tboot"
                    "${TOS_RELEASE_DIR}/ternary-os.tdisk"
                    "${TOS_RELEASE_VERSION}"
            COMMAND ${CMAKE_COMMAND} -E copy_if_different
                    "$<TARGET_FILE:run_tos_sdl>"
                    "${TOS_RELEASE_DIR}/TernaryOS.exe"
            DEPENDS build_tos_image run_tos_sdl
            WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
            USES_TERMINAL)

        # The desktop workflow conformance check consumes the same staged
        # current-only image as the SDL runner.
        add_dependencies(current_validate stage_tos_release)

        add_custom_target(smoke_tos_release
            COMMAND "$<TARGET_FILE:run_tos_sdl>"
                    --smoke-test --frames 10
                    "${TOS_RELEASE_DIR}/ternary-os.tboot"
                    "${TOS_RELEASE_DIR}/ternary-os.tdisk"
            DEPENDS stage_tos_release
            WORKING_DIRECTORY "${TOS_RELEASE_DIR}"
            USES_TERMINAL)

        add_custom_target(package_tos_release
            COMMAND ${CMAKE_COMMAND} -E rm -f
                    "${TOS_RELEASE_ROOT}/TernaryOS-windows.zip"
            COMMAND ${CMAKE_COMMAND} -E tar cf
                    "${TOS_RELEASE_ROOT}/TernaryOS-windows.zip"
                    --format=zip TernaryOS
            DEPENDS stage_tos_release
            WORKING_DIRECTORY "${TOS_RELEASE_ROOT}"
            USES_TERMINAL)
        message(STATUS "SDL2 found: optional release targets enabled")
    else()
        message(STATUS "SDL2 not found: optional release targets disabled")
    endif()
endif()

# Keep both the standard CTest entry point and a named production gate. The
# latter is intentionally only the current graph, so it cannot accidentally
# resurrect a deleted test or an offline migration executable.
if(BUILD_TESTING)
    add_custom_target(ctest
        COMMAND ${CMAKE_CTEST_COMMAND}
                --test-dir ${CMAKE_CURRENT_BINARY_DIR}
                --output-on-failure
        DEPENDS current_validate
        WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
        USES_TERMINAL)
else()
    add_custom_target(ctest
        COMMAND ${CMAKE_COMMAND} -E echo "BUILD_TESTING=OFF"
        USES_TERMINAL)
endif()

add_custom_target(ci_production
    COMMAND ${CMAKE_COMMAND} --build ${CMAKE_CURRENT_BINARY_DIR}
            --target tritc build_tos_image
    COMMAND ${CMAKE_COMMAND} --build ${CMAKE_CURRENT_BINARY_DIR}
            --target current_validate
    COMMAND ${CMAKE_COMMAND} --build ${CMAKE_CURRENT_BINARY_DIR}
            --target ctest
    DEPENDS tritc build_tos_image current_validate
    WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
    USES_TERMINAL)

message(STATUS
    "Current platform: ISA v2 / executable ABI v3 / tBoot v3 / tDisk v2")
