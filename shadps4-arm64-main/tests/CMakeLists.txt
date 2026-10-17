# SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
# SPDX-License-Identifier: GPL-2.0-or-later

# Find or download Google Test
include(FetchContent)
    FetchContent_Declare(
    googletest
    URL https://github.com/google/googletest/archive/refs/tags/v1.17.0.zip
)
FetchContent_MakeAvailable(googletest)

set(TEST_TARGETS
    shadps4_settings_test
    shadps4_gcn_test
    shadps4_guest_memory_validation_test
    shadps4_imgui_memory_test
    shadps4_libc_mspace_test
)

set(SETTINGS_TEST_SOURCES
    # Under test
    ${CMAKE_SOURCE_DIR}/src/core/emulator_settings.cpp
    ${CMAKE_SOURCE_DIR}/src/core/emulator_state.cpp

    # Minimal common support
    ${CMAKE_SOURCE_DIR}/src/common/path_util.cpp
    ${CMAKE_SOURCE_DIR}/src/common/assert.cpp
    ${CMAKE_SOURCE_DIR}/src/common/error.cpp
    ${CMAKE_SOURCE_DIR}/src/common/string_util.cpp
    ${CMAKE_SOURCE_DIR}/src/common/logging/log.cpp

    # Stubs that replace dependencies
    stubs/common_stub.cpp
    stubs/core_stub.cpp
    stubs/scm_rev_stub.cpp
    stubs/sdl_stub.cpp

    # Tests
    test_emulator_settings.cpp
)

set(GCN_TEST_SOURCES
    # Under test
    ${CMAKE_SOURCE_DIR}/src/core/emulator_settings.cpp
    ${CMAKE_SOURCE_DIR}/src/core/emulator_state.cpp
    ${CMAKE_SOURCE_DIR}/src/shader_recompiler/backend/spirv/emit_spirv.cpp
    ${CMAKE_SOURCE_DIR}/src/shader_recompiler/backend/spirv/emit_spirv.h
    ${CMAKE_SOURCE_DIR}/src/shader_recompiler/backend/spirv/emit_spirv_atomic.cpp
    ${CMAKE_SOURCE_DIR}/src/shader_recompiler/backend/spirv/emit_spirv_barriers.cpp
    ${CMAKE_SOURCE_DIR}/src/shader_recompiler/backend/spirv/emit_spirv_bitwise_conversion.cpp
    ${CMAKE_SOURCE_DIR}/src/shader_recompiler/backend/spirv/emit_spirv_composite.cpp
    ${CMAKE_SOURCE_DIR}/src/shader_recompiler/backend/spirv/emit_spirv_context_get_set.cpp
    ${CMAKE_SOURCE_DIR}/src/shader_recompiler/backend/spirv/emit_spirv_convert.cpp
    ${CMAKE_SOURCE_DIR}/src/shader_recompiler/backend/spirv/emit_spirv_floating_point.cpp
    ${CMAKE_SOURCE_DIR}/src/shader_recompiler/backend/spirv/emit_spirv_image.cpp
    ${CMAKE_SOURCE_DIR}/src/shader_recompiler/backend/spirv/emit_spirv_integer.cpp
    ${CMAKE_SOURCE_DIR}/src/shader_recompiler/backend/spirv/emit_spirv_logical.cpp
    ${CMAKE_SOURCE_DIR}/src/shader_recompiler/backend/spirv/emit_spirv_select.cpp
    ${CMAKE_SOURCE_DIR}/src/shader_recompiler/backend/spirv/emit_spirv_shared_memory.cpp
    ${CMAKE_SOURCE_DIR}/src/shader_recompiler/backend/spirv/emit_spirv_special.cpp
    ${CMAKE_SOURCE_DIR}/src/shader_recompiler/backend/spirv/emit_spirv_undefined.cpp
    ${CMAKE_SOURCE_DIR}/src/shader_recompiler/backend/spirv/emit_spirv_warp.cpp
    ${CMAKE_SOURCE_DIR}/src/shader_recompiler/backend/spirv/emit_spirv_instructions.h
    ${CMAKE_SOURCE_DIR}/src/shader_recompiler/backend/spirv/spirv_emit_context.cpp
    ${CMAKE_SOURCE_DIR}/src/shader_recompiler/backend/spirv/spirv_emit_context.h
    ${CMAKE_SOURCE_DIR}/src/shader_recompiler/frontend/format.cpp
    ${CMAKE_SOURCE_DIR}/src/shader_recompiler/frontend/decode.cpp
    ${CMAKE_SOURCE_DIR}/src/shader_recompiler/frontend/decode.h
    ${CMAKE_SOURCE_DIR}/src/shader_recompiler/frontend/fetch_shader.cpp
    ${CMAKE_SOURCE_DIR}/src/shader_recompiler/frontend/fetch_shader.h
    ${CMAKE_SOURCE_DIR}/src/shader_recompiler/frontend/translate/data_share.cpp
    ${CMAKE_SOURCE_DIR}/src/shader_recompiler/frontend/translate/export.cpp
    ${CMAKE_SOURCE_DIR}/src/shader_recompiler/frontend/translate/translate.cpp
    ${CMAKE_SOURCE_DIR}/src/shader_recompiler/frontend/translate/translate.h
    ${CMAKE_SOURCE_DIR}/src/shader_recompiler/frontend/translate/scalar_alu.cpp
    ${CMAKE_SOURCE_DIR}/src/shader_recompiler/frontend/translate/scalar_flow.cpp
    ${CMAKE_SOURCE_DIR}/src/shader_recompiler/frontend/translate/scalar_memory.cpp
    ${CMAKE_SOURCE_DIR}/src/shader_recompiler/frontend/translate/vector_alu.cpp
    ${CMAKE_SOURCE_DIR}/src/shader_recompiler/frontend/translate/vector_interpolation.cpp
    ${CMAKE_SOURCE_DIR}/src/shader_recompiler/frontend/translate/vector_memory.cpp
    ${CMAKE_SOURCE_DIR}/src/shader_recompiler/ir/abstract_syntax_list.cpp
    ${CMAKE_SOURCE_DIR}/src/shader_recompiler/ir/abstract_syntax_list.h
    ${CMAKE_SOURCE_DIR}/src/shader_recompiler/ir/basic_block.cpp
    ${CMAKE_SOURCE_DIR}/src/shader_recompiler/ir/basic_block.h
    ${CMAKE_SOURCE_DIR}/src/shader_recompiler/ir/microinstruction.cpp
    ${CMAKE_SOURCE_DIR}/src/shader_recompiler/ir/attribute.cpp
    ${CMAKE_SOURCE_DIR}/src/shader_recompiler/ir/attribute.h
    ${CMAKE_SOURCE_DIR}/src/shader_recompiler/ir/ir_emitter.cpp
    ${CMAKE_SOURCE_DIR}/src/shader_recompiler/ir/ir_emitter.h
    ${CMAKE_SOURCE_DIR}/src/shader_recompiler/ir/opcodes.cpp
    ${CMAKE_SOURCE_DIR}/src/shader_recompiler/ir/opcodes.h
    ${CMAKE_SOURCE_DIR}/src/shader_recompiler/ir/patch.cpp
    ${CMAKE_SOURCE_DIR}/src/shader_recompiler/ir/patch.h
    ${CMAKE_SOURCE_DIR}/src/shader_recompiler/ir/program.cpp
    ${CMAKE_SOURCE_DIR}/src/shader_recompiler/ir/program.h
    ${CMAKE_SOURCE_DIR}/src/shader_recompiler/ir/passes/ir_passes.h
    ${CMAKE_SOURCE_DIR}/src/shader_recompiler/ir/passes/constant_propagation_pass.cpp
    ${CMAKE_SOURCE_DIR}/src/shader_recompiler/ir/passes/dead_code_elimination_pass.cpp
    ${CMAKE_SOURCE_DIR}/src/shader_recompiler/ir/passes/identity_removal_pass.cpp
    ${CMAKE_SOURCE_DIR}/src/shader_recompiler/ir/passes/resource_tracking_pass.cpp
    ${CMAKE_SOURCE_DIR}/src/shader_recompiler/ir/passes/shader_info_collection_pass.cpp
    ${CMAKE_SOURCE_DIR}/src/shader_recompiler/ir/passes/ssa_rewrite_pass.cpp
    ${CMAKE_SOURCE_DIR}/src/shader_recompiler/ir/post_order.cpp
    ${CMAKE_SOURCE_DIR}/src/shader_recompiler/ir/post_order.h
    ${CMAKE_SOURCE_DIR}/src/shader_recompiler/ir/type.cpp
    ${CMAKE_SOURCE_DIR}/src/shader_recompiler/ir/type.h
    ${CMAKE_SOURCE_DIR}/src/shader_recompiler/ir/value.cpp
    ${CMAKE_SOURCE_DIR}/src/shader_recompiler/ir/value.h
    ${CMAKE_SOURCE_DIR}/src/video_core/amdgpu/pixel_format.cpp
    ${CMAKE_SOURCE_DIR}/src/video_core/amdgpu/pixel_format.h
    ${CMAKE_SOURCE_DIR}/src/video_core/amdgpu/resource.h

    # Minimal common support
    ${CMAKE_SOURCE_DIR}/src/common/path_util.cpp
    ${CMAKE_SOURCE_DIR}/src/common/assert.cpp
    ${CMAKE_SOURCE_DIR}/src/common/error.cpp
    ${CMAKE_SOURCE_DIR}/src/common/io_file.cpp
    ${CMAKE_SOURCE_DIR}/src/common/string_util.cpp
    ${CMAKE_SOURCE_DIR}/src/common/logging/log.cpp
    ${CMAKE_SOURCE_DIR}/src/common/ntapi.cpp

    # Stubs that replace dependencies
    stubs/common_stub.cpp
    stubs/resource_tracking_pass_stub.cpp
    stubs/scm_rev_stub.cpp
    stubs/sdl_stub.cpp
    stubs/core_stub.cpp

    gcn/gcn_test_runner.hpp
    gcn/gcn_test_runner.cpp
    gcn/translator.hpp
    gcn/translator.cpp

    # Tests
    gcn/test_gcn_instructions.cpp
    gcn/test_geometry_inputs.cpp
    gcn/test_image_host_view.cpp
    gcn/test_pm4_stall_log.cpp

    # Vulkan-Hpp dispatch test (vkCmdBindVertexBuffers2)
    gcn/test_vk_dispatch_bindvb2.cpp
    ${CMAKE_SOURCE_DIR}/src/video_core/buffer_cache/vk_dispatch_reload.cpp
    ${CMAKE_SOURCE_DIR}/src/video_core/buffer_cache/vk_dispatch_reload.h
)

set(IMGUI_MEMORY_TEST_SOURCES
    imgui/test_memory_type_selection.cpp
)

set(GUEST_MEMORY_VALIDATION_TEST_SOURCES
    core/test_guest_memory_validation_cache.cpp
    core/test_pthread_stack_placement.cpp
    core/test_mouse_open_param.cpp
    core/test_flip_label_tracker.cpp
    core/test_fios2_module_choice.cpp
    video_core/test_legacy_vertex_attributes.cpp
    video_core/test_readconst_snapshot_diag.cpp
    video_core/test_fma_split_diag.cpp
    video_core/test_recip_refine_fix.cpp
)

set(LIBC_MSPACE_TEST_SOURCES
    ${CMAKE_SOURCE_DIR}/src/core/libraries/libc_internal/mspace.cpp
    core/test_libc_mspace.cpp
)

add_executable(shadps4_settings_test ${SETTINGS_TEST_SOURCES})
add_executable(shadps4_gcn_test ${GCN_TEST_SOURCES})
add_executable(shadps4_guest_memory_validation_test ${GUEST_MEMORY_VALIDATION_TEST_SOURCES})
add_executable(shadps4_imgui_memory_test ${IMGUI_MEMORY_TEST_SOURCES})
add_executable(shadps4_libc_mspace_test ${LIBC_MSPACE_TEST_SOURCES})

foreach(t ${TEST_TARGETS})
    target_include_directories(${t} PRIVATE
        ${CMAKE_SOURCE_DIR}/src
        ${CMAKE_SOURCE_DIR}
        ${CMAKE_SOURCE_DIR}/tests
    )
    target_compile_features(${t} PRIVATE cxx_std_23)
    target_compile_definitions(${t} PRIVATE BOOST_ASIO_STANDALONE)
endforeach()

target_link_libraries(shadps4_settings_test PRIVATE
    GTest::gtest_main
    fmt::fmt
    nlohmann_json::nlohmann_json
    toml11::toml11
    SDL3::SDL3
    spdlog::spdlog
)
target_link_libraries(shadps4_gcn_test PRIVATE
    GTest::gtest_main
    fmt::fmt
    nlohmann_json::nlohmann_json
    magic_enum::magic_enum
    toml11::toml11
    Boost::headers
    Vulkan::Headers
    tsl::robin_map
    GPUOpen::VulkanMemoryAllocator
    sirit
    SDL3::SDL3
    spdlog::spdlog
    half::half
)

# vk_dispatch_reload.cpp must not be LTO-optimized (same as in the main binary)
set_source_files_properties(
    ${CMAKE_SOURCE_DIR}/src/video_core/buffer_cache/vk_dispatch_reload.cpp
    PROPERTIES COMPILE_FLAGS "-fno-lto"
)
target_link_libraries(shadps4_imgui_memory_test PRIVATE
    GTest::gtest_main
    Vulkan::Headers
)
target_link_libraries(shadps4_guest_memory_validation_test PRIVATE
    GTest::gtest_main
    fmt::fmt
    spdlog::spdlog
)
target_link_libraries(shadps4_libc_mspace_test PRIVATE
    GTest::gtest_main
)

if (CMAKE_CXX_COMPILER_ID STREQUAL "Clang" OR
    CMAKE_CXX_COMPILER_ID STREQUAL "AppleClang")
    include(CheckCXXSymbolExists)
    check_cxx_symbol_exists(_LIBCPP_VERSION version LIBCPP)
    if (LIBCPP)
        foreach(t ${TEST_TARGETS})
            target_compile_options(${t} PRIVATE -fexperimental-library)
        endforeach()
    endif()
endif()

foreach(t ${TEST_TARGETS})
    if (WIN32)
        target_link_libraries(${t} PRIVATE onecore)
        target_compile_definitions(${t} PRIVATE
            NOMINMAX
            WIN32_LEAN_AND_MEAN
            NTDDI_VERSION=0x0A000006
            _WIN32_WINNT=0x0A00
            WINVER=0x0A00
        )
        if (MSVC)
            target_compile_definitions(${t} PRIVATE
                _CRT_SECURE_NO_WARNINGS
                _CRT_NONSTDC_NO_DEPRECATE
                _SCL_SECURE_NO_WARNINGS
                _TIMESPEC_DEFINED
            )
        endif()
    endif()
endforeach()

include(GoogleTest)

foreach(t ${TEST_TARGETS})
    gtest_discover_tests(${t}
        WORKING_DIRECTORY ${CMAKE_BINARY_DIR}
        DISCOVERY_TIMEOUT 60
    )
endforeach()

# ===========================================================================
# HTTP tests (libSceHttp - URI helpers and status-line parsing)
# ===========================================================================
# Self-contained block - mirrors the structure of the targets above so the
# new test does not depend on any earlier foreach iterations.

set(HTTP_TEST_SOURCES
    # Under test
    ${CMAKE_SOURCE_DIR}/src/core/libraries/network/http.cpp
    # Required to link RegisterLib's LIB_FUNCTION calls and the logger's
    # access to EmulatorSettings.
    ${CMAKE_SOURCE_DIR}/src/core/emulator_settings.cpp
    ${CMAKE_SOURCE_DIR}/src/core/emulator_state.cpp

    # Minimal common support
    ${CMAKE_SOURCE_DIR}/src/common/path_util.cpp
    ${CMAKE_SOURCE_DIR}/src/common/assert.cpp
    ${CMAKE_SOURCE_DIR}/src/common/error.cpp
    ${CMAKE_SOURCE_DIR}/src/common/string_util.cpp
    ${CMAKE_SOURCE_DIR}/src/common/logging/log.cpp

    # Stubs that replace dependencies
    stubs/common_stub.cpp
    stubs/scm_rev_stub.cpp
    stubs/sdl_stub.cpp
    stubs/loader_stub.cpp
    stubs/core_stub.cpp
    stubs/kernel_stub.cpp

    # Tests
    network/test_http_uri.cpp
    network/test_http_status_line.cpp
    network/test_http_parse_response_header.cpp
    network/test_http_lifecycle.cpp
)

add_executable(shadps4_http_test ${HTTP_TEST_SOURCES})

list(APPEND TEST_TARGETS shadps4_http_test)

target_include_directories(shadps4_http_test PRIVATE
    ${CMAKE_SOURCE_DIR}/src
    ${CMAKE_SOURCE_DIR}
)
target_compile_features(shadps4_http_test PRIVATE cxx_std_23)
target_compile_definitions(shadps4_http_test PRIVATE BOOST_ASIO_STANDALONE)

target_link_libraries(shadps4_http_test PRIVATE
    GTest::gtest_main
    fmt::fmt
    magic_enum::magic_enum
    nlohmann_json::nlohmann_json
    toml11::toml11
    SDL3::SDL3
    spdlog::spdlog
)

if (CMAKE_CXX_COMPILER_ID STREQUAL "Clang" OR
    CMAKE_CXX_COMPILER_ID STREQUAL "AppleClang")
    include(CheckCXXSymbolExists)
    check_cxx_symbol_exists(_LIBCPP_VERSION version LIBCPP)
    if (LIBCPP)
        target_compile_options(shadps4_http_test PRIVATE -fexperimental-library)
    endif()
endif()

if (WIN32)
    target_link_libraries(shadps4_http_test PRIVATE onecore)
    target_compile_definitions(shadps4_http_test PRIVATE
        NOMINMAX
        WIN32_LEAN_AND_MEAN
        NTDDI_VERSION=0x0A000006
        _WIN32_WINNT=0x0A00
        WINVER=0x0A00
    )
    if (MSVC)
        target_compile_definitions(shadps4_http_test PRIVATE
            _CRT_SECURE_NO_WARNINGS
            _CRT_NONSTDC_NO_DEPRECATE
            _SCL_SECURE_NO_WARNINGS
            _TIMESPEC_DEFINED
        )
    endif()
endif()

gtest_discover_tests(shadps4_http_test
    WORKING_DIRECTORY ${CMAKE_BINARY_DIR}
    DISCOVERY_TIMEOUT 60
)

# ===========================================================================
# Bachata Android runtime protocol tests
# ===========================================================================

set(BACHATA_RUNTIME_TEST_SOURCES
    ${CMAKE_SOURCE_DIR}/src/platform/bachata/runtime_client.cpp
    ${CMAKE_SOURCE_DIR}/src/platform/bachata/controller_snapshot.cpp
    ${CMAKE_SOURCE_DIR}/src/platform/bachata/audio_transport.cpp
    platform/test_bachata_runtime_client.cpp
    platform/test_bachata_controller_snapshot.cpp
)

add_executable(shadps4_bachata_runtime_test ${BACHATA_RUNTIME_TEST_SOURCES})

target_include_directories(shadps4_bachata_runtime_test PRIVATE
    ${CMAKE_SOURCE_DIR}/src
    ${CMAKE_SOURCE_DIR}
)
target_compile_features(shadps4_bachata_runtime_test PRIVATE cxx_std_23)

target_link_libraries(shadps4_bachata_runtime_test PRIVATE
    FFmpeg::ffmpeg
    GTest::gtest_main
)

gtest_discover_tests(shadps4_bachata_runtime_test
    WORKING_DIRECTORY ${CMAKE_BINARY_DIR}
    DISCOVERY_TIMEOUT 60
)

# ===========================================================================
# libSceUsbd event-loop null-context safety (Driveclub FFB thread)
# ===========================================================================

add_executable(shadps4_usbd_test
    libraries/test_usbd_event_loop.cpp
)

target_include_directories(shadps4_usbd_test PRIVATE
    ${CMAKE_SOURCE_DIR}/src
    ${CMAKE_SOURCE_DIR}
)
target_compile_features(shadps4_usbd_test PRIVATE cxx_std_23)
target_link_libraries(shadps4_usbd_test PRIVATE
    GTest::gtest_main
    libusb::usb
)

gtest_discover_tests(shadps4_usbd_test
    WORKING_DIRECTORY ${CMAKE_BINARY_DIR}
    DISCOVERY_TIMEOUT 60
)

# ===========================================================================
# libSceJpegDec HLE (Driveclub splash / packed JPEG textures)
# ===========================================================================

add_executable(shadps4_jpegdec_test
    libraries/test_jpegdec.cpp
    ${CMAKE_SOURCE_DIR}/src/core/libraries/jpeg/jpegdec.cpp
)
target_include_directories(shadps4_jpegdec_test PRIVATE
    ${CMAKE_SOURCE_DIR}/src
    ${CMAKE_SOURCE_DIR}
    ${CMAKE_SOURCE_DIR}/externals/stb
)
target_compile_definitions(shadps4_jpegdec_test PRIVATE JPEGDEC_STANDALONE_TEST)
target_compile_features(shadps4_jpegdec_test PRIVATE cxx_std_23)
target_link_libraries(shadps4_jpegdec_test PRIVATE
    GTest::gtest_main
)
gtest_discover_tests(shadps4_jpegdec_test
    WORKING_DIRECTORY ${CMAKE_BINARY_DIR}
    DISCOVERY_TIMEOUT 60
)

# ===========================================================================
# AvPlayer buffering hysteresis (Driveclub attract video freeze)
# ===========================================================================

add_executable(shadps4_avplayer_buffering_test
    libraries/test_avplayer_buffering.cpp
)
target_include_directories(shadps4_avplayer_buffering_test PRIVATE
    ${CMAKE_SOURCE_DIR}/src
    ${CMAKE_SOURCE_DIR}
)
target_compile_features(shadps4_avplayer_buffering_test PRIVATE cxx_std_23)
target_link_libraries(shadps4_avplayer_buffering_test PRIVATE
    GTest::gtest_main
)
gtest_discover_tests(shadps4_avplayer_buffering_test
    WORKING_DIRECTORY ${CMAKE_BINARY_DIR}
    DISCOVERY_TIMEOUT 60
)

add_executable(shadps4_avplayer_path_test
    libraries/test_avplayer_path.cpp
)
target_include_directories(shadps4_avplayer_path_test PRIVATE
    ${CMAKE_SOURCE_DIR}/src
    ${CMAKE_SOURCE_DIR}
)
target_compile_features(shadps4_avplayer_path_test PRIVATE cxx_std_23)
target_link_libraries(shadps4_avplayer_path_test PRIVATE
    GTest::gtest_main
)
gtest_discover_tests(shadps4_avplayer_path_test
    WORKING_DIRECTORY ${CMAKE_BINARY_DIR}
    DISCOVERY_TIMEOUT 60
)

add_executable(shadps4_avplayer_read_test
    libraries/test_avplayer_read.cpp
)
target_include_directories(shadps4_avplayer_read_test PRIVATE
    ${CMAKE_SOURCE_DIR}/src
    ${CMAKE_SOURCE_DIR}
)
target_compile_features(shadps4_avplayer_read_test PRIVATE cxx_std_23)
target_link_libraries(shadps4_avplayer_read_test PRIVATE
    GTest::gtest_main
)
gtest_discover_tests(shadps4_avplayer_read_test
    WORKING_DIRECTORY ${CMAKE_BINARY_DIR}
    DISCOVERY_TIMEOUT 60
)

add_executable(shadps4_pm4_type0_test
    video_core/test_pm4_type0.cpp
)
target_include_directories(shadps4_pm4_type0_test PRIVATE
    ${CMAKE_SOURCE_DIR}/src
    ${CMAKE_SOURCE_DIR}
)
target_compile_features(shadps4_pm4_type0_test PRIVATE cxx_std_23)
target_link_libraries(shadps4_pm4_type0_test PRIVATE
    GTest::gtest_main
)
gtest_discover_tests(shadps4_pm4_type0_test
    WORKING_DIRECTORY ${CMAKE_BINARY_DIR}
    DISCOVERY_TIMEOUT 60
)

add_executable(shadps4_pm4_type1_test
    video_core/test_pm4_type1.cpp
)
target_include_directories(shadps4_pm4_type1_test PRIVATE
    ${CMAKE_SOURCE_DIR}/src
    ${CMAKE_SOURCE_DIR}
)
target_compile_features(shadps4_pm4_type1_test PRIVATE cxx_std_23)
target_link_libraries(shadps4_pm4_type1_test PRIVATE
    GTest::gtest_main
)
gtest_discover_tests(shadps4_pm4_type1_test
    WORKING_DIRECTORY ${CMAKE_BINARY_DIR}
    DISCOVERY_TIMEOUT 60
)

add_executable(shadps4_pm4_packet_test
    video_core/test_pm4_packet.cpp
)
target_include_directories(shadps4_pm4_packet_test PRIVATE
    ${CMAKE_SOURCE_DIR}/src
    ${CMAKE_SOURCE_DIR}
)
target_compile_features(shadps4_pm4_packet_test PRIVATE cxx_std_23)
target_link_libraries(shadps4_pm4_packet_test PRIVATE
    GTest::gtest_main
)
gtest_discover_tests(shadps4_pm4_packet_test
    WORKING_DIRECTORY ${CMAKE_BINARY_DIR}
    DISCOVERY_TIMEOUT 60
)

# ===========================================================================
# FEX HLE stderr traces stay off unless BACHATA_FEX_HLE_TRACE is set
# ===========================================================================

add_executable(shadps4_hle_trace_test
    core/test_hle_trace.cpp
)
target_include_directories(shadps4_hle_trace_test PRIVATE
    ${CMAKE_SOURCE_DIR}/src
    ${CMAKE_SOURCE_DIR}
)
target_compile_features(shadps4_hle_trace_test PRIVATE cxx_std_23)
target_link_libraries(shadps4_hle_trace_test PRIVATE
    GTest::gtest_main
)
gtest_discover_tests(shadps4_hle_trace_test
    WORKING_DIRECTORY ${CMAKE_BINARY_DIR}
    DISCOVERY_TIMEOUT 60
)

# ===========================================================================
# Memory patch selection seam (Milestone 1)
# ===========================================================================

set(MEMORY_PATCHER_TEST_SOURCES
    # Under test
    ${CMAKE_SOURCE_DIR}/src/common/memory_patcher.cpp
    ${CMAKE_SOURCE_DIR}/src/common/patch_session.cpp
    ${CMAKE_SOURCE_DIR}/src/common/patch_preset.cpp
    ${CMAKE_SOURCE_DIR}/src/common/patch_repository.cpp
    ${CMAKE_SOURCE_DIR}/src/core/file_format/psf.cpp
    ${CMAKE_SOURCE_DIR}/src/common/io_file.cpp

    # EmulatorState is used by OnGameLoaded(); EmulatorSettings is used by log.cpp.
    ${CMAKE_SOURCE_DIR}/src/core/emulator_state.cpp
    ${CMAKE_SOURCE_DIR}/src/core/emulator_settings.cpp

    # Minimal common support
    ${CMAKE_SOURCE_DIR}/src/common/path_util.cpp
    ${CMAKE_SOURCE_DIR}/src/common/assert.cpp
    ${CMAKE_SOURCE_DIR}/src/common/error.cpp
    ${CMAKE_SOURCE_DIR}/src/common/string_util.cpp
    ${CMAKE_SOURCE_DIR}/src/common/logging/log.cpp

    # Stubs that replace dependencies
    stubs/common_stub.cpp
    stubs/core_stub.cpp
    stubs/scm_rev_stub.cpp
    stubs/sdl_stub.cpp

    # Tests
    common/test_memory_patcher.cpp
)

add_executable(shadps4_memory_patcher_test ${MEMORY_PATCHER_TEST_SOURCES})

target_include_directories(shadps4_memory_patcher_test PRIVATE
    ${CMAKE_SOURCE_DIR}/src
    ${CMAKE_SOURCE_DIR}
    ${CMAKE_SOURCE_DIR}/tests
)
target_compile_features(shadps4_memory_patcher_test PRIVATE cxx_std_23)
target_compile_definitions(shadps4_memory_patcher_test PRIVATE
    BOOST_ASIO_STANDALONE
    MEMORY_PATCHER_TEST_OBSERVER
)

target_link_libraries(shadps4_memory_patcher_test PRIVATE
    GTest::gtest_main
    fmt::fmt
    nlohmann_json::nlohmann_json
    pugixml::pugixml
    SDL3::SDL3
    spdlog::spdlog
    toml11::toml11
)

list(APPEND TEST_TARGETS shadps4_memory_patcher_test)

gtest_discover_tests(shadps4_memory_patcher_test
    WORKING_DIRECTORY ${CMAKE_BINARY_DIR}
    DISCOVERY_TIMEOUT 60
)

# ===========================================================================
# Local repository v1 foundation (Milestone 2)
# ===========================================================================

set(PATCH_REPOSITORY_TEST_SOURCES
    # Under test
    ${CMAKE_SOURCE_DIR}/src/common/patch_repository.cpp
    ${CMAKE_SOURCE_DIR}/src/common/memory_patcher.cpp
    ${CMAKE_SOURCE_DIR}/src/common/patch_session.cpp
    ${CMAKE_SOURCE_DIR}/src/common/patch_preset.cpp
    ${CMAKE_SOURCE_DIR}/src/core/file_format/psf.cpp
    ${CMAKE_SOURCE_DIR}/src/common/io_file.cpp

    # EmulatorState is used by OnGameLoaded(); EmulatorSettings is used by log.cpp.
    ${CMAKE_SOURCE_DIR}/src/core/emulator_state.cpp
    ${CMAKE_SOURCE_DIR}/src/core/emulator_settings.cpp

    # Minimal common support
    ${CMAKE_SOURCE_DIR}/src/common/path_util.cpp
    ${CMAKE_SOURCE_DIR}/src/common/assert.cpp
    ${CMAKE_SOURCE_DIR}/src/common/error.cpp
    ${CMAKE_SOURCE_DIR}/src/common/string_util.cpp
    ${CMAKE_SOURCE_DIR}/src/common/logging/log.cpp

    # Stubs that replace dependencies
    stubs/common_stub.cpp
    stubs/core_stub.cpp
    stubs/scm_rev_stub.cpp
    stubs/sdl_stub.cpp

    # Tests
    common/test_patch_repository.cpp
)

add_executable(shadps4_patch_repository_test ${PATCH_REPOSITORY_TEST_SOURCES})

target_include_directories(shadps4_patch_repository_test PRIVATE
    ${CMAKE_SOURCE_DIR}/src
    ${CMAKE_SOURCE_DIR}
    ${CMAKE_SOURCE_DIR}/tests
)
target_compile_features(shadps4_patch_repository_test PRIVATE cxx_std_23)
target_compile_definitions(shadps4_patch_repository_test PRIVATE BOOST_ASIO_STANDALONE)

target_link_libraries(shadps4_patch_repository_test PRIVATE
    GTest::gtest_main
    fmt::fmt
    nlohmann_json::nlohmann_json
    pugixml::pugixml
    SDL3::SDL3
    spdlog::spdlog
    toml11::toml11
)

list(APPEND TEST_TARGETS shadps4_patch_repository_test)

gtest_discover_tests(shadps4_patch_repository_test
    WORKING_DIRECTORY ${CMAKE_BINARY_DIR}
    DISCOVERY_TIMEOUT 60
)

# ===========================================================================
# Persistent per-game user state (Milestone 3)
# ===========================================================================

set(PATCH_STATE_TEST_SOURCES
    # Under test
    ${CMAKE_SOURCE_DIR}/src/common/patch_state.cpp
    ${CMAKE_SOURCE_DIR}/src/common/patch_repository.cpp
    ${CMAKE_SOURCE_DIR}/src/common/memory_patcher.cpp
    ${CMAKE_SOURCE_DIR}/src/common/patch_session.cpp
    ${CMAKE_SOURCE_DIR}/src/common/patch_preset.cpp
    ${CMAKE_SOURCE_DIR}/src/core/file_format/psf.cpp
    ${CMAKE_SOURCE_DIR}/src/common/io_file.cpp

    # EmulatorState is used by OnGameLoaded(); EmulatorSettings is used by log.cpp.
    ${CMAKE_SOURCE_DIR}/src/core/emulator_state.cpp
    ${CMAKE_SOURCE_DIR}/src/core/emulator_settings.cpp

    # Minimal common support
    ${CMAKE_SOURCE_DIR}/src/common/path_util.cpp
    ${CMAKE_SOURCE_DIR}/src/common/assert.cpp
    ${CMAKE_SOURCE_DIR}/src/common/error.cpp
    ${CMAKE_SOURCE_DIR}/src/common/string_util.cpp
    ${CMAKE_SOURCE_DIR}/src/common/logging/log.cpp

    # Stubs that replace dependencies
    stubs/common_stub.cpp
    stubs/core_stub.cpp
    stubs/scm_rev_stub.cpp
    stubs/sdl_stub.cpp

    # Tests
    common/test_patch_state.cpp
)

add_executable(shadps4_patch_state_test ${PATCH_STATE_TEST_SOURCES})

target_include_directories(shadps4_patch_state_test PRIVATE
    ${CMAKE_SOURCE_DIR}/src
    ${CMAKE_SOURCE_DIR}
    ${CMAKE_SOURCE_DIR}/tests
)
target_compile_features(shadps4_patch_state_test PRIVATE cxx_std_23)
target_compile_definitions(shadps4_patch_state_test PRIVATE BOOST_ASIO_STANDALONE)

target_link_libraries(shadps4_patch_state_test PRIVATE
    GTest::gtest_main
    fmt::fmt
    nlohmann_json::nlohmann_json
    pugixml::pugixml
    SDL3::SDL3
    spdlog::spdlog
    toml11::toml11
)

list(APPEND TEST_TARGETS shadps4_patch_state_test)

gtest_discover_tests(shadps4_patch_state_test
    WORKING_DIRECTORY ${CMAKE_BINARY_DIR}
    DISCOVERY_TIMEOUT 60
)

# ===========================================================================
# Patch domain C ABI bridge (Milestone 4)
# ===========================================================================

set(PATCH_DOMAIN_SOURCES
    ${CMAKE_SOURCE_DIR}/src/common/patch_domain.cpp
    ${CMAKE_SOURCE_DIR}/src/common/patch_state.cpp
    ${CMAKE_SOURCE_DIR}/src/common/patch_repository.cpp
    ${CMAKE_SOURCE_DIR}/src/common/patch_session.cpp
    ${CMAKE_SOURCE_DIR}/src/common/patch_preset.cpp
    ${CMAKE_SOURCE_DIR}/src/common/memory_patcher.cpp
    ${CMAKE_SOURCE_DIR}/src/core/file_format/psf.cpp
    ${CMAKE_SOURCE_DIR}/src/common/io_file.cpp
    ${CMAKE_SOURCE_DIR}/src/core/emulator_state.cpp
    ${CMAKE_SOURCE_DIR}/src/common/path_util.cpp
    ${CMAKE_SOURCE_DIR}/src/common/assert.cpp
    ${CMAKE_SOURCE_DIR}/src/common/error.cpp
    ${CMAKE_SOURCE_DIR}/src/common/string_util.cpp
    ${CMAKE_SOURCE_DIR}/src/common/logging/log.cpp
)

# Executable exercising the C ABI directly (no JVM involved).
add_executable(shadps4_patch_domain_test ${PATCH_DOMAIN_SOURCES}
    stubs/patch_domain_stub.cpp
    stubs/core_stub.cpp
    stubs/sdl_stub.cpp
    common/test_patch_domain.cpp
)

target_include_directories(shadps4_patch_domain_test PRIVATE
    ${CMAKE_SOURCE_DIR}/src
    ${CMAKE_SOURCE_DIR}
    ${CMAKE_SOURCE_DIR}/tests
)
target_compile_features(shadps4_patch_domain_test PRIVATE cxx_std_23)
target_compile_definitions(shadps4_patch_domain_test PRIVATE BOOST_ASIO_STANDALONE)

target_link_libraries(shadps4_patch_domain_test PRIVATE
    GTest::gtest_main
    fmt::fmt
    nlohmann_json::nlohmann_json
    pugixml::pugixml
    SDL3::SDL3
    spdlog::spdlog
)

list(APPEND TEST_TARGETS shadps4_patch_domain_test)

gtest_discover_tests(shadps4_patch_domain_test
    WORKING_DIRECTORY ${CMAKE_BINARY_DIR}
    DISCOVERY_TIMEOUT 60
)

# Host shared library exposing the same JNI symbols the Android .so will use, so the Kotlin
# end-to-end boundary test can drive the REAL native resolver from a JVM unit test.
add_library(shadps4_patch_domain_shared SHARED ${PATCH_DOMAIN_SOURCES}
    ${CMAKE_SOURCE_DIR}/src/common/patch_domain_jni.cpp
    stubs/patch_domain_stub.cpp
    stubs/core_stub.cpp
    stubs/sdl_stub.cpp
)
set_target_properties(shadps4_patch_domain_shared PROPERTIES
    OUTPUT_NAME bachata_patch_domain
    LIBRARY_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/patch_domain
    RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/patch_domain
)
target_include_directories(shadps4_patch_domain_shared PRIVATE
    ${CMAKE_SOURCE_DIR}/src
    ${CMAKE_SOURCE_DIR}
    ${CMAKE_SOURCE_DIR}/tests
    /usr/lib/jvm/java-17-openjdk-amd64/include
    /usr/lib/jvm/java-17-openjdk-amd64/include/linux
)
target_compile_features(shadps4_patch_domain_shared PRIVATE cxx_std_23)
target_compile_definitions(shadps4_patch_domain_shared PRIVATE BOOST_ASIO_STANDALONE)
target_link_libraries(shadps4_patch_domain_shared PRIVATE
    fmt::fmt
    nlohmann_json::nlohmann_json
    pugixml::pugixml
    SDL3::SDL3
    spdlog::spdlog
)

# ===========================================================================
# Managed launch session (Milestone 5)
# ===========================================================================

set(PATCH_SESSION_TEST_SOURCES
    # Under test
    ${CMAKE_SOURCE_DIR}/src/common/patch_session.cpp
    ${CMAKE_SOURCE_DIR}/src/common/patch_preset.cpp
    ${CMAKE_SOURCE_DIR}/src/common/patch_domain.cpp
    ${CMAKE_SOURCE_DIR}/src/common/patch_state.cpp
    ${CMAKE_SOURCE_DIR}/src/common/patch_repository.cpp
    ${CMAKE_SOURCE_DIR}/src/common/memory_patcher.cpp
    ${CMAKE_SOURCE_DIR}/src/core/file_format/psf.cpp
    ${CMAKE_SOURCE_DIR}/src/common/io_file.cpp

    # EmulatorState is used by OnGameLoaded(); EmulatorSettings is used by log.cpp.
    ${CMAKE_SOURCE_DIR}/src/core/emulator_state.cpp
    ${CMAKE_SOURCE_DIR}/src/core/emulator_settings.cpp

    # Minimal common support
    ${CMAKE_SOURCE_DIR}/src/common/path_util.cpp
    ${CMAKE_SOURCE_DIR}/src/common/assert.cpp
    ${CMAKE_SOURCE_DIR}/src/common/error.cpp
    ${CMAKE_SOURCE_DIR}/src/common/string_util.cpp
    ${CMAKE_SOURCE_DIR}/src/common/logging/log.cpp

    # Stubs that replace dependencies
    stubs/common_stub.cpp
    stubs/core_stub.cpp
    stubs/scm_rev_stub.cpp
    stubs/sdl_stub.cpp

    # Tests
    common/test_patch_session.cpp
)

add_executable(shadps4_patch_session_test ${PATCH_SESSION_TEST_SOURCES})

target_include_directories(shadps4_patch_session_test PRIVATE
    ${CMAKE_SOURCE_DIR}/src
    ${CMAKE_SOURCE_DIR}
    ${CMAKE_SOURCE_DIR}/tests
)
target_compile_features(shadps4_patch_session_test PRIVATE cxx_std_23)
target_compile_definitions(shadps4_patch_session_test PRIVATE
    BOOST_ASIO_STANDALONE
    MEMORY_PATCHER_TEST_OBSERVER
)

target_link_libraries(shadps4_patch_session_test PRIVATE
    GTest::gtest_main
    fmt::fmt
    nlohmann_json::nlohmann_json
    pugixml::pugixml
    SDL3::SDL3
    spdlog::spdlog
    toml11::toml11
)

list(APPEND TEST_TARGETS shadps4_patch_session_test)

gtest_discover_tests(shadps4_patch_session_test
    WORKING_DIRECTORY ${CMAKE_BINARY_DIR}
    DISCOVERY_TIMEOUT 60
)

# ===========================================================================
# Preset resolution + Bloodborne local pilot (Milestone 6)
# ===========================================================================

set(PRESET_TEST_SOURCES
    # Under test
    ${CMAKE_SOURCE_DIR}/src/common/patch_preset.cpp
    ${CMAKE_SOURCE_DIR}/src/common/patch_domain.cpp
    ${CMAKE_SOURCE_DIR}/src/common/patch_state.cpp
    ${CMAKE_SOURCE_DIR}/src/common/patch_repository.cpp
    ${CMAKE_SOURCE_DIR}/src/common/patch_session.cpp
    ${CMAKE_SOURCE_DIR}/src/common/memory_patcher.cpp
    ${CMAKE_SOURCE_DIR}/src/core/file_format/psf.cpp
    ${CMAKE_SOURCE_DIR}/src/common/io_file.cpp

    # EmulatorState is used by OnGameLoaded(); EmulatorSettings is used by log.cpp.
    ${CMAKE_SOURCE_DIR}/src/core/emulator_state.cpp
    ${CMAKE_SOURCE_DIR}/src/core/emulator_settings.cpp

    # Minimal common support
    ${CMAKE_SOURCE_DIR}/src/common/path_util.cpp
    ${CMAKE_SOURCE_DIR}/src/common/assert.cpp
    ${CMAKE_SOURCE_DIR}/src/common/error.cpp
    ${CMAKE_SOURCE_DIR}/src/common/string_util.cpp
    ${CMAKE_SOURCE_DIR}/src/common/logging/log.cpp

    # Stubs that replace dependencies
    stubs/common_stub.cpp
    stubs/core_stub.cpp
    stubs/scm_rev_stub.cpp
    stubs/sdl_stub.cpp

    # Tests
    common/test_patch_preset.cpp
)

add_executable(shadps4_preset_test ${PRESET_TEST_SOURCES})

target_include_directories(shadps4_preset_test PRIVATE
    ${CMAKE_SOURCE_DIR}/src
    ${CMAKE_SOURCE_DIR}
    ${CMAKE_SOURCE_DIR}/tests
)
target_compile_features(shadps4_preset_test PRIVATE cxx_std_23)
target_compile_definitions(shadps4_preset_test PRIVATE
    BOOST_ASIO_STANDALONE
    MEMORY_PATCHER_TEST_OBSERVER
)

target_link_libraries(shadps4_preset_test PRIVATE
    GTest::gtest_main
    fmt::fmt
    nlohmann_json::nlohmann_json
    pugixml::pugixml
    SDL3::SDL3
    spdlog::spdlog
    toml11::toml11
)

list(APPEND TEST_TARGETS shadps4_preset_test)

gtest_discover_tests(shadps4_preset_test
    WORKING_DIRECTORY ${CMAKE_BINARY_DIR}
    DISCOVERY_TIMEOUT 60
)
