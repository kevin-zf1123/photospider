# Minimum CMake version required
cmake_minimum_required(VERSION 3.16)
cmake_policy(SET CMP0135 NEW) 
# Define the project
project(Photospider LANGUAGES CXX)
# 开启 LTO (Link Time Optimization)
include(CheckIPOSupported)
check_ipo_supported(RESULT result OUTPUT output)
if(result)
    set(CMAKE_INTERPROCEDURAL_OPTIMIZATION TRUE)
else()
    message(WARNING "LTO not supported: ${output}")
endif()

# 强制默认 Release 模式（如果用户未指定）
if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  message(STATUS "Setting build type to Release as none was specified.")
  set(CMAKE_BUILD_TYPE Release CACHE STRING "Choose the type of build." FORCE)
endif()
# --- Options ---
option(USE_ASAN "Enable AddressSanitizer" OFF)
option(USE_TSAN "Enable ThreadSanitizer" OFF)
option(BUILD_TESTING "Build tests" ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# --- Compiler Settings ---
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_POSITION_INDEPENDENT_CODE ON) # Required for static libs linking into shared libs

if(APPLE AND NOT CMAKE_OSX_ARCHITECTURES)
    set(CMAKE_OSX_ARCHITECTURES "arm64" CACHE STRING "macOS target architecture" FORCE)
endif()

if(CMAKE_CXX_COMPILER_ID MATCHES "Clang|GNU")
    add_compile_options(-Wall -Wextra -Wno-unused-parameter)
    if(USE_ASAN)
        add_compile_options(-fsanitize=address -g -fno-omit-frame-pointer)
        add_link_options(-fsanitize=address)
    elseif(USE_TSAN)
        add_compile_options(-fsanitize=thread -g)
        add_link_options(-fsanitize=thread)
    endif()
endif()

# --- Output Directories ---
set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/lib)
set(CMAKE_LIBRARY_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/lib)
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)
set(PLUGIN_OUTPUT_DIR ${CMAKE_BINARY_DIR}/plugins)

# --- Dependencies ---
if(APPLE)
    list(APPEND CMAKE_PREFIX_PATH "/opt/homebrew")
endif()

find_package(Threads REQUIRED)
find_package(OpenCV REQUIRED COMPONENTS core imgproc imgcodecs videoio)
find_package(yaml-cpp REQUIRED)
find_package(CURL) 
find_package(OpenSSL)

# --- FTXUI (Frontend UI Lib) ---
set(FTXUI_INCLUDE_DIR "${PROJECT_SOURCE_DIR}/extern/ftxui/include")
set(FTXUI_SRC_DIR "${PROJECT_SOURCE_DIR}/extern/ftxui/src")
set(FTXUI_TARGETS "")

if(EXISTS "${FTXUI_SRC_DIR}")
    file(GLOB_RECURSE FTXUI_SCREEN_SOURCES "${FTXUI_SRC_DIR}/ftxui/screen/*.cpp")
    file(GLOB_RECURSE FTXUI_DOM_SOURCES "${FTXUI_SRC_DIR}/ftxui/dom/*.cpp")
    file(GLOB_RECURSE FTXUI_COMPONENT_SOURCES "${FTXUI_SRC_DIR}/ftxui/component/*.cpp")
    
    foreach(src_list FTXUI_SCREEN_SOURCES FTXUI_DOM_SOURCES FTXUI_COMPONENT_SOURCES)
        list(FILTER ${src_list} EXCLUDE REGEX ".*_test\\.cpp$")
        list(FILTER ${src_list} EXCLUDE REGEX ".*_fuzzer\\.cpp$")
        list(FILTER ${src_list} EXCLUDE REGEX ".*_benchmark\\.cpp$")
    endforeach()

    add_library(ftxui-screen STATIC ${FTXUI_SCREEN_SOURCES})
    target_include_directories(ftxui-screen PUBLIC ${FTXUI_INCLUDE_DIR} PRIVATE ${FTXUI_SRC_DIR})

    add_library(ftxui-dom STATIC ${FTXUI_DOM_SOURCES})
    target_include_directories(ftxui-dom PUBLIC ${FTXUI_INCLUDE_DIR} PRIVATE ${FTXUI_SRC_DIR})
    target_link_libraries(ftxui-dom PUBLIC ftxui-screen)

    add_library(ftxui-component STATIC ${FTXUI_COMPONENT_SOURCES})
    target_include_directories(ftxui-component PUBLIC ${FTXUI_INCLUDE_DIR} PRIVATE ${FTXUI_SRC_DIR})
    target_link_libraries(ftxui-component PUBLIC ftxui-dom)
    
    set(FTXUI_TARGETS ftxui-component)
else()
    find_package(ftxui REQUIRED)
    set(FTXUI_TARGETS ftxui::component)
endif()

# ==============================================================================
# 1. Internal Static Modules (The Building Blocks)
# ==============================================================================

# Module: Core Types
add_library(photospider_core_types STATIC
    src/image_buffer.cpp
    src/ps_types.cpp
    src/adapter/buffer_adapter_opencv.cpp
    src/ops.cpp
    src/kernel/node_module/node_yaml.cpp
)
target_include_directories(photospider_core_types PUBLIC
    "${PROJECT_SOURCE_DIR}/include"
    "${PROJECT_SOURCE_DIR}/src"
    ${OpenCV_INCLUDE_DIRS}
)
target_link_libraries(photospider_core_types PUBLIC yaml-cpp::yaml-cpp)

# Module: Graph
add_library(photospider_graph STATIC
    src/kernel/graph_model.cpp
    src/kernel/services/graph_extent_resolver.cpp
    src/kernel/services/graph_traversal_service.cpp
    src/kernel/services/roi_propagation_service.cpp
    src/kernel/services/graph_io_service.cpp
    src/kernel/services/graph_cache_service.cpp
    src/kernel/services/graph_inspect_service.cpp
)
target_link_libraries(photospider_graph PUBLIC photospider_core_types)

# Module: Plugins
add_library(photospider_plugin STATIC
    src/kernel/plugin_manager.cpp
    src/kernel/plugin_manage_module/plugin_loader.cpp
)
target_link_libraries(photospider_plugin PUBLIC photospider_core_types)

# Module: Compute
set(KERNEL_FACADE_SOURCES
    src/kernel/kernel.cpp
    src/kernel/kernel_compute.cpp
    src/kernel/kernel_dirty_roi_facade.cpp
    src/kernel/kernel_inspection_facade.cpp
    src/kernel/kernel_io_cache_facade.cpp
    src/kernel/kernel_scheduler_facade.cpp
)

set(COMPUTE_SOURCES
    src/kernel/services/compute-service/compute_cache_policy.cpp
    src/kernel/services/compute-service/compute_dispatch_plan_builder.cpp
    src/kernel/services/compute-service/compute_geometry.cpp
    src/kernel/services/compute-service/compute_metrics_recorder.cpp
    src/kernel/services/compute-service/compute_node_task_runner.cpp
    src/kernel/services/compute-service/compute_result_committer.cpp
    src/kernel/services/compute-service/compute_task_dependency_state.cpp
    src/kernel/services/compute-service/compute_task_submission.cpp
    src/kernel/services/compute-service/task_graph_planning.cpp
    src/kernel/services/compute-service/task_population_strategy.cpp
    src/kernel/services/compute-service/tiled_input_normalizer.cpp
    src/kernel/services/compute-service/dirty_control_lane.cpp
    src/kernel/services/compute-service/dirty_execution_common.cpp
    src/kernel/services/compute-service/dirty_node_executor.cpp
    src/kernel/services/compute-service/dirty_region_planner.cpp
    src/kernel/services/compute-service/dirty_region_snapshot_builder.cpp
    src/kernel/services/compute-service/dirty_sibling_commit_gate.cpp
    src/kernel/services/compute-service/dirty_update_executor.cpp
    src/kernel/services/compute-service/dirty_write_buffers.cpp
    src/kernel/services/compute-service/downsample_executor.cpp
    src/kernel/services/compute-service/intent_update_coordinator.cpp
    src/kernel/services/compute-service/node_executor.cpp
    src/kernel/services/compute-service/node_input_resolver.cpp
    src/kernel/services/compute-service/realtime_proxy_graph.cpp
    src/kernel/services/compute-service/compute_task_dispatcher.cpp
    src/kernel/services/compute_service.cpp
    src/kernel/services/graph_event_service.cpp
    src/kernel/interaction.cpp
    src/kernel/interaction_benchmark.cpp
    src/kernel/graph_runtime.mm
    src/kernel/scheduler/cpu_work_stealing_scheduler.cpp
    src/kernel/scheduler/gpu_pipeline_scheduler.cpp
    src/kernel/scheduler/serial_debug_scheduler.cpp
    src/kernel/scheduler/scheduler_factory.cpp
    src/kernel/scheduler/scheduler_plugin_loader.cpp
)

if(APPLE)
    enable_language(OBJCXX)
else()
    set_source_files_properties(src/kernel/graph_runtime.mm PROPERTIES LANGUAGE CXX COMPILE_FLAGS "-x c++")
endif()

add_library(photospider_compute STATIC ${COMPUTE_SOURCES})
target_link_libraries(photospider_compute PUBLIC photospider_graph photospider_plugin)

# ==============================================================================
# 2. Core Shared Library (The Backend)
# ==============================================================================

file(GLOB PHOTOSPIDER_BENCHMARK_SOURCES "src/benchmark/*.cpp")

add_library(photospider_lib SHARED ${PHOTOSPIDER_BENCHMARK_SOURCES}
                                  ${KERNEL_FACADE_SOURCES})

target_compile_definitions(photospider_lib PRIVATE PHOTOSPIDER_LIB_BUILD)

target_link_libraries(photospider_lib PUBLIC
    photospider_compute
    photospider_plugin
    photospider_graph
    photospider_core_types
    ${OpenCV_LIBS}
)

if(WIN32)
    set_target_properties(photospider_lib PROPERTIES WINDOWS_EXPORT_ALL_SYMBOLS ON)
endif()

# ==============================================================================
# 3. CLI Static Library (The Frontend Logic)
# ==============================================================================

file(GLOB_RECURSE CLI_LOGIC_SOURCES "src/cli/*.cpp" "src/cli_config.cpp")
list(REMOVE_ITEM CLI_LOGIC_SOURCES "${PROJECT_SOURCE_DIR}/cli/graph_cli.cpp") 

add_library(photospider_cli_common STATIC ${CLI_LOGIC_SOURCES})

target_include_directories(photospider_cli_common PUBLIC
    "${PROJECT_SOURCE_DIR}/include"
    "${PROJECT_SOURCE_DIR}/src"
)

target_link_libraries(photospider_cli_common PUBLIC
    photospider_lib
    ${FTXUI_TARGETS}
)

# ==============================================================================
# 4. Main Executable
# ==============================================================================

add_executable(graph_cli cli/graph_cli.cpp)
target_link_libraries(graph_cli PRIVATE photospider_cli_common)

if (UNIX AND NOT APPLE)
    target_link_libraries(graph_cli PRIVATE dl pthread)
endif()

set_target_properties(graph_cli PROPERTIES
    BUILD_RPATH "${CMAKE_BINARY_DIR}/lib"
    INSTALL_RPATH "$ORIGIN/../lib"
)

# ==============================================================================
# 5. Standard Plugins (custom_ops)
# ==============================================================================
# Build everything in custom_ops/ EXCEPT the metal loader (which is special)

file(MAKE_DIRECTORY ${PLUGIN_OUTPUT_DIR})
file(GLOB PLUGIN_SOURCES "custom_ops/*.cpp")

# Exclude the Metal loader from the generic loop; it's handled in the APPLE block
list(REMOVE_ITEM PLUGIN_SOURCES "${CMAKE_CURRENT_SOURCE_DIR}/custom_ops/metal_ops_loader.cpp")

set(OP_PLUGIN_TARGETS "")
foreach(PLUGIN_SOURCE ${PLUGIN_SOURCES})
    get_filename_component(PLUGIN_NAME ${PLUGIN_SOURCE} NAME_WE)
    add_library(${PLUGIN_NAME} SHARED ${PLUGIN_SOURCE})
    list(APPEND OP_PLUGIN_TARGETS ${PLUGIN_NAME})
    
    set_target_properties(${PLUGIN_NAME} PROPERTIES
        LIBRARY_OUTPUT_DIRECTORY "${PLUGIN_OUTPUT_DIR}"
        BUILD_RPATH "${CMAKE_BINARY_DIR}/lib"
    )
    
    target_include_directories(${PLUGIN_NAME} PRIVATE "${PROJECT_SOURCE_DIR}/include" "${PROJECT_SOURCE_DIR}/src")
    target_link_libraries(${PLUGIN_NAME} PRIVATE photospider_lib)
endforeach()

# ==============================================================================
# 6. Scheduler Plugins (Dynamic Loadable Schedulers)
# ==============================================================================

set(SCHEDULER_PLUGIN_DIR ${CMAKE_BINARY_DIR}/schedulers)
file(MAKE_DIRECTORY ${SCHEDULER_PLUGIN_DIR})

# Production/demo scheduler plugins. Test-only fixtures are built into a
# separate directory below so config.yaml's default scheduler_dirs does not
# expose lifecycle test schedulers to users.
file(GLOB SCHEDULER_PLUGIN_SOURCES "scheduler_plugins/*_example_plugin.cpp")

# Remove stale lifecycle fixture outputs left by older build trees.
file(REMOVE
    "${SCHEDULER_PLUGIN_DIR}/libdestroy_count_scheduler_plugin.dylib"
    "${SCHEDULER_PLUGIN_DIR}/libdestroy_count_scheduler_plugin.so"
    "${SCHEDULER_PLUGIN_DIR}/destroy_count_scheduler_plugin.dll"
    "${SCHEDULER_PLUGIN_DIR}/libcpu_work_stealing_plugin.dylib"
    "${SCHEDULER_PLUGIN_DIR}/libcpu_work_stealing_plugin.so"
    "${SCHEDULER_PLUGIN_DIR}/cpu_work_stealing_plugin.dll"
    "${SCHEDULER_PLUGIN_DIR}/libgpu_pipeline_plugin.dylib"
    "${SCHEDULER_PLUGIN_DIR}/libgpu_pipeline_plugin.so"
    "${SCHEDULER_PLUGIN_DIR}/gpu_pipeline_plugin.dll"
    "${SCHEDULER_PLUGIN_DIR}/libserial_debug_plugin.dylib"
    "${SCHEDULER_PLUGIN_DIR}/libserial_debug_plugin.so"
    "${SCHEDULER_PLUGIN_DIR}/serial_debug_plugin.dll"
)

foreach(SCHEDULER_SOURCE ${SCHEDULER_PLUGIN_SOURCES})
    get_filename_component(SCHEDULER_NAME ${SCHEDULER_SOURCE} NAME_WE)
    add_library(${SCHEDULER_NAME} SHARED ${SCHEDULER_SOURCE})
    list(APPEND SCHEDULER_PLUGIN_TARGETS ${SCHEDULER_NAME})
    
    set_target_properties(${SCHEDULER_NAME} PROPERTIES
        LIBRARY_OUTPUT_DIRECTORY "${SCHEDULER_PLUGIN_DIR}"
        BUILD_RPATH "${CMAKE_BINARY_DIR}/lib"
        INSTALL_RPATH "$ORIGIN/../lib"
    )
    
    if(APPLE)
        set_target_properties(${SCHEDULER_NAME} PROPERTIES
            MACOSX_RPATH ON
            INSTALL_RPATH "@loader_path/../lib"
        )
    endif()
    
    target_include_directories(${SCHEDULER_NAME} PRIVATE 
        "${PROJECT_SOURCE_DIR}/include" 
        "${PROJECT_SOURCE_DIR}/src"
    )
    target_link_libraries(${SCHEDULER_NAME} PRIVATE photospider_lib)
endforeach()

if(BUILD_TESTING)
    set(TEST_SCHEDULER_PLUGIN_DIR ${CMAKE_BINARY_DIR}/test_schedulers)
    set(TEST_OP_PLUGIN_DIR ${CMAKE_BINARY_DIR}/test_plugins)
    file(MAKE_DIRECTORY ${TEST_SCHEDULER_PLUGIN_DIR})
    file(MAKE_DIRECTORY ${TEST_OP_PLUGIN_DIR})

    add_library(destroy_count_scheduler_plugin SHARED
        tests/fixtures/scheduler_plugins/destroy_count_scheduler_plugin.cpp
    )
    set_target_properties(destroy_count_scheduler_plugin PROPERTIES
        LIBRARY_OUTPUT_DIRECTORY "${TEST_SCHEDULER_PLUGIN_DIR}"
        BUILD_RPATH "${CMAKE_BINARY_DIR}/lib"
        INSTALL_RPATH "$ORIGIN/../lib"
    )
    if(APPLE)
        set_target_properties(destroy_count_scheduler_plugin PROPERTIES
            MACOSX_RPATH ON
            INSTALL_RPATH "@loader_path/../lib"
        )
    endif()
    target_include_directories(destroy_count_scheduler_plugin PRIVATE
        "${PROJECT_SOURCE_DIR}/include"
        "${PROJECT_SOURCE_DIR}/src"
    )
    target_link_libraries(destroy_count_scheduler_plugin PRIVATE photospider_lib)

    add_library(lifecycle_op_plugin SHARED
        tests/fixtures/ops/lifecycle_op_plugin.cpp
    )
    set_target_properties(lifecycle_op_plugin PROPERTIES
        LIBRARY_OUTPUT_DIRECTORY "${TEST_OP_PLUGIN_DIR}/lifecycle"
        RUNTIME_OUTPUT_DIRECTORY "${TEST_OP_PLUGIN_DIR}/lifecycle"
        BUILD_RPATH "${CMAKE_BINARY_DIR}/lib"
        INSTALL_RPATH "$ORIGIN/../../lib"
    )
    if(APPLE)
        set_target_properties(lifecycle_op_plugin PROPERTIES
            MACOSX_RPATH ON
            INSTALL_RPATH "@loader_path/../../lib"
        )
    endif()
    target_include_directories(lifecycle_op_plugin PRIVATE
        "${PROJECT_SOURCE_DIR}/include"
        "${PROJECT_SOURCE_DIR}/src"
    )
    target_link_libraries(lifecycle_op_plugin PRIVATE photospider_lib)

    add_library(override_lifecycle_op_plugin SHARED
        tests/fixtures/ops/override_lifecycle_op_plugin.cpp
    )
    set_target_properties(override_lifecycle_op_plugin PROPERTIES
        LIBRARY_OUTPUT_DIRECTORY "${TEST_OP_PLUGIN_DIR}/override"
        RUNTIME_OUTPUT_DIRECTORY "${TEST_OP_PLUGIN_DIR}/override"
        BUILD_RPATH "${CMAKE_BINARY_DIR}/lib"
        INSTALL_RPATH "$ORIGIN/../../lib"
    )
    if(APPLE)
        set_target_properties(override_lifecycle_op_plugin PROPERTIES
            MACOSX_RPATH ON
            INSTALL_RPATH "@loader_path/../../lib"
        )
    endif()
    target_include_directories(override_lifecycle_op_plugin PRIVATE
        "${PROJECT_SOURCE_DIR}/include"
        "${PROJECT_SOURCE_DIR}/src"
    )
    target_link_libraries(override_lifecycle_op_plugin PRIVATE photospider_lib)
endif()

# ==============================================================================
# 7. Metal Support (Apple Only)
# ==============================================================================
if(APPLE)
    set(METAL_ROOT_DIR ${CMAKE_BINARY_DIR}/high_performance/metal)
    set(METAL_OPS_DIR  ${METAL_ROOT_DIR}/ops)
    file(MAKE_DIRECTORY ${METAL_ROOT_DIR})
    file(MAKE_DIRECTORY ${METAL_OPS_DIR})

    find_library(METAL_FRAMEWORK Metal REQUIRED)
    find_library(FOUNDATION_FRAMEWORK Foundation REQUIRED)
    find_library(COREIMAGE_FRAMEWORK CoreImage REQUIRED)
    find_library(COREVIDEO_FRAMEWORK CoreVideo REQUIRED)

    # Core lib needs Metal framework
    target_link_libraries(photospider_lib PRIVATE ${METAL_FRAMEWORK} ${FOUNDATION_FRAMEWORK})

    # 1. The Implementation Library (the actual op logic)
    add_library(perlin_noise_metal SHARED src/metal/perlin_noise_metal.mm)
    set_target_properties(perlin_noise_metal PROPERTIES
        LIBRARY_OUTPUT_DIRECTORY "${METAL_OPS_DIR}"
        MACOSX_RPATH ON
    )
    target_link_libraries(perlin_noise_metal PRIVATE
        photospider_lib
        ${METAL_FRAMEWORK} ${FOUNDATION_FRAMEWORK} ${COREIMAGE_FRAMEWORK} ${COREVIDEO_FRAMEWORK}
    )
    target_include_directories(perlin_noise_metal PRIVATE "${PROJECT_SOURCE_DIR}/include")

    # 2. The Loader Plugin (registers the op with the Kernel)
    add_library(metal_ops_loader SHARED custom_ops/metal_ops_loader.cpp)
    set_target_properties(metal_ops_loader PROPERTIES
        LIBRARY_OUTPUT_DIRECTORY "${METAL_ROOT_DIR}"
        # Point RPATH to 'ops' subdir so loader can find perlin_noise_metal
        BUILD_RPATH "@loader_path/ops;${CMAKE_BINARY_DIR}/lib"
        INSTALL_RPATH "@loader_path/ops;$ORIGIN/../lib"
        MACOSX_RPATH ON
    )

    # [修复关键点] 必须链接 perlin_noise_metal，否则 loader 运行时找不到符号
    target_link_libraries(metal_ops_loader PRIVATE 
        perlin_noise_metal
        photospider_lib
    )

    # Allow undefined symbols for dynamic lookup (useful for plugins in general)
    set(CMAKE_SHARED_LINKER_FLAGS "${CMAKE_SHARED_LINKER_FLAGS} -undefined dynamic_lookup")
endif()

# ==============================================================================
# 8. Tests
# ==============================================================================
if(BUILD_TESTING)
    enable_testing()
    set(TEST_OUTPUT_DIR ${CMAKE_BINARY_DIR}/tests)
    file(MAKE_DIRECTORY ${TEST_OUTPUT_DIR})
    
    include(GoogleTest)
    find_package(Python3 REQUIRED COMPONENTS Interpreter)
    find_package(GTest REQUIRED)
    find_package(nlohmann_json REQUIRED)

    macro(add_ps_test TEST_NAME SOURCE_FILE)
        add_executable(${TEST_NAME} ${SOURCE_FILE})
        set_target_properties(${TEST_NAME} PROPERTIES
            RUNTIME_OUTPUT_DIRECTORY "${TEST_OUTPUT_DIR}"
            BUILD_RPATH "${CMAKE_BINARY_DIR}/lib"
        )
        target_include_directories(${TEST_NAME} PRIVATE "${PROJECT_SOURCE_DIR}/include" "${PROJECT_SOURCE_DIR}/src")
        target_link_libraries(${TEST_NAME} PRIVATE photospider_lib GTest::gtest GTest::gtest_main)
    endmacro()

    add_ps_test(test_scheduler tests/test_scheduler.cpp)
    target_link_libraries(test_scheduler PRIVATE nlohmann_json::nlohmann_json)
    
    add_ps_test(test_milestone2 tests/test_milestone2.cpp)
    add_ps_test(test_milestone3 tests/test_milestone3.cpp)
    add_ps_test(test_milestone34 tests/test_milestone34.cpp)
    add_ps_test(test_propagation tests/test_propagation.cpp)
    add_ps_test(test_propagation_contracts tests/test_propagation_contracts.cpp)
    add_ps_test(test_graph_topology_boundaries tests/test_graph_topology_boundaries.cpp)
    add_ps_test(test_kernel_contracts tests/test_kernel_contracts.cpp)
    add_ps_test(test_image_buffer_contracts tests/test_image_buffer_contracts.cpp)
    add_ps_test(test_compute_service_split tests/test_compute_service_split.cpp)
    add_ps_test(test_node_editor_layout tests/test_node_editor_layout.cpp)
    target_link_libraries(test_node_editor_layout PRIVATE ${FTXUI_TARGETS})

    add_executable(split_compute_service_trace tests/split_compute_service_trace.cpp)
    set_target_properties(split_compute_service_trace PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY "${TEST_OUTPUT_DIR}"
        BUILD_RPATH "${CMAKE_BINARY_DIR}/lib"
    )
    target_include_directories(split_compute_service_trace PRIVATE "${PROJECT_SOURCE_DIR}/include" "${PROJECT_SOURCE_DIR}/src")
    target_link_libraries(split_compute_service_trace PRIVATE photospider_lib nlohmann_json::nlohmann_json)

    # [M3.1] OpRegistry 多设备实现测试
    add_ps_test(test_op_registry_m31 tests/test_op_registry_m31.cpp)

    # Operation plugin manager handle lifetime tests
    add_ps_test(test_plugin_manager tests/test_plugin_manager.cpp)
    target_link_libraries(test_plugin_manager PRIVATE ${CMAKE_DL_LIBS})
    target_compile_definitions(test_plugin_manager PRIVATE
        PS_STANDARD_OP_PLUGIN_DIR="${PLUGIN_OUTPUT_DIR}"
        PS_TEST_OP_PLUGIN_DIR="${TEST_OP_PLUGIN_DIR}"
    )
    add_dependencies(test_plugin_manager lifecycle_op_plugin
        override_lifecycle_op_plugin ${OP_PLUGIN_TARGETS})

    # [M3.5] GPU Pipeline 调度器测试
    add_ps_test(test_gpu_pipeline_scheduler tests/test_gpu_pipeline_scheduler.cpp)

    # [M3.5] 调度器插件加载器测试
    add_ps_test(test_scheduler_plugin_loader tests/test_scheduler_plugin_loader.cpp)
    target_link_libraries(test_scheduler_plugin_loader PRIVATE ${CMAKE_DL_LIBS})
    target_compile_definitions(test_scheduler_plugin_loader PRIVATE
        PS_SCHEDULER_PLUGIN_DIR="${SCHEDULER_PLUGIN_DIR}"
        PS_TEST_SCHEDULER_PLUGIN_DIR="${TEST_SCHEDULER_PLUGIN_DIR}"
    )
    add_dependencies(test_scheduler_plugin_loader
        ${SCHEDULER_PLUGIN_TARGETS}
        destroy_count_scheduler_plugin
    )

    set(PHOTOSPIDER_PUBLIC_HEADER_ROOT
        "${PROJECT_SOURCE_DIR}/include/photospider")
    file(GLOB_RECURSE PHOTOSPIDER_INSTALLABLE_PUBLIC_HEADERS
        CONFIGURE_DEPENDS
        "${PHOTOSPIDER_PUBLIC_HEADER_ROOT}/*.h"
        "${PHOTOSPIDER_PUBLIC_HEADER_ROOT}/*.hpp"
        "${PHOTOSPIDER_PUBLIC_HEADER_ROOT}/*.hh"
        "${PHOTOSPIDER_PUBLIC_HEADER_ROOT}/*.hxx"
    )
    if(NOT PHOTOSPIDER_INSTALLABLE_PUBLIC_HEADERS)
        message(FATAL_ERROR
            "Expected installable public headers under include/photospider")
    endif()
    add_custom_target(public_header_self_containment)
    foreach(PUBLIC_HEADER ${PHOTOSPIDER_INSTALLABLE_PUBLIC_HEADERS})
        file(RELATIVE_PATH PUBLIC_HEADER_INCLUDE
            "${PROJECT_SOURCE_DIR}/include" "${PUBLIC_HEADER}")
        string(MAKE_C_IDENTIFIER "${PUBLIC_HEADER_INCLUDE}"
            PUBLIC_HEADER_TARGET_SUFFIX)
        set(PUBLIC_HEADER_SOURCE
            "${CMAKE_BINARY_DIR}/generated/public_header_self_containment/${PUBLIC_HEADER_TARGET_SUFFIX}.cpp")
        file(WRITE "${PUBLIC_HEADER_SOURCE}"
            "/* Generated by CMake to verify public header self-containment. */\n"
            "#include <${PUBLIC_HEADER_INCLUDE}>\n"
        )
        add_library(
            "public_header_self_containment_${PUBLIC_HEADER_TARGET_SUFFIX}"
            OBJECT "${PUBLIC_HEADER_SOURCE}"
        )
        target_include_directories(
            "public_header_self_containment_${PUBLIC_HEADER_TARGET_SUFFIX}"
            PRIVATE "${PROJECT_SOURCE_DIR}/include"
        )
        target_compile_features(
            "public_header_self_containment_${PUBLIC_HEADER_TARGET_SUFFIX}"
            PRIVATE cxx_std_17
        )
        add_dependencies(public_header_self_containment
            "public_header_self_containment_${PUBLIC_HEADER_TARGET_SUFFIX}")
    endforeach()
    add_test(NAME PublicHeaderBoundaryScan
        COMMAND ${Python3_EXECUTABLE}
            "${PROJECT_SOURCE_DIR}/tests/verification/codebase_structure/phase0_guardrail_scan.py"
            --repo "${PROJECT_SOURCE_DIR}"
            --out "${CMAKE_BINARY_DIR}/tests/public_header_boundary_scan"
        WORKING_DIRECTORY ${PROJECT_SOURCE_DIR})
    add_test(NAME Phase1CoreSeamScan
        COMMAND ${Python3_EXECUTABLE}
            "${PROJECT_SOURCE_DIR}/tests/verification/codebase_structure/phase1_core_seam_scan.py"
            --repo "${PROJECT_SOURCE_DIR}"
            --out "${CMAKE_BINARY_DIR}/tests/phase1_core_seam_scan"
        WORKING_DIRECTORY ${PROJECT_SOURCE_DIR})
    add_test(NAME PublicHeaderSelfContainment
        COMMAND ${CMAKE_COMMAND} --build "${CMAKE_BINARY_DIR}"
            --target public_header_self_containment --config $<CONFIG>
        WORKING_DIRECTORY ${PROJECT_SOURCE_DIR})
    
    gtest_discover_tests(test_scheduler WORKING_DIRECTORY ${PROJECT_SOURCE_DIR})
    gtest_discover_tests(test_milestone2 WORKING_DIRECTORY ${PROJECT_SOURCE_DIR})
    gtest_discover_tests(test_milestone3 WORKING_DIRECTORY ${PROJECT_SOURCE_DIR})
    gtest_discover_tests(test_op_registry_m31 WORKING_DIRECTORY ${PROJECT_SOURCE_DIR})
    gtest_discover_tests(test_plugin_manager WORKING_DIRECTORY ${PROJECT_SOURCE_DIR})
    gtest_discover_tests(test_milestone34 WORKING_DIRECTORY ${PROJECT_SOURCE_DIR})
    # test_propagation is intentionally a scriptable REPL/tool target rather
    # than a GTest binary. Keep it buildable for CI scripts, but do not run
    # gtest discovery on it.
    gtest_discover_tests(test_propagation_contracts WORKING_DIRECTORY ${PROJECT_SOURCE_DIR})
    gtest_discover_tests(test_graph_topology_boundaries WORKING_DIRECTORY ${PROJECT_SOURCE_DIR})
    gtest_discover_tests(test_kernel_contracts WORKING_DIRECTORY ${PROJECT_SOURCE_DIR})
    gtest_discover_tests(test_image_buffer_contracts WORKING_DIRECTORY ${PROJECT_SOURCE_DIR})
    gtest_discover_tests(test_compute_service_split WORKING_DIRECTORY ${PROJECT_SOURCE_DIR})
    gtest_discover_tests(test_node_editor_layout WORKING_DIRECTORY ${PROJECT_SOURCE_DIR})
    gtest_discover_tests(test_gpu_pipeline_scheduler WORKING_DIRECTORY ${PROJECT_SOURCE_DIR})
    gtest_discover_tests(test_scheduler_plugin_loader WORKING_DIRECTORY ${PROJECT_SOURCE_DIR})
    add_test(NAME SplitComputeServiceRuntimeTrace
        COMMAND split_compute_service_trace "${PROJECT_SOURCE_DIR}/tests/results/split-compute-service"
        WORKING_DIRECTORY ${PROJECT_SOURCE_DIR})
endif()

# --- Summary ---
message(STATUS "-----------------------------------------------------------")
message(STATUS "Build Configuration Summary:")
message(STATUS "  Core Lib (Shared) : libphotospider_lib (Backend)")
message(STATUS "  CLI Lib (Static)  : libphotospider_cli_common (Frontend)")
message(STATUS "  Executable        : graph_cli")
message(STATUS "  Output Bin        : ${CMAKE_RUNTIME_OUTPUT_DIRECTORY}")
message(STATUS "  Output Lib        : ${CMAKE_LIBRARY_OUTPUT_DIRECTORY}")
message(STATUS "  Output Plugins    : ${PLUGIN_OUTPUT_DIR}")
message(STATUS "  Scheduler Plugins : ${SCHEDULER_PLUGIN_DIR}")
if(APPLE)
    message(STATUS "  Metal Plugins     : ${METAL_ROOT_DIR}")
endif()
message(STATUS "-----------------------------------------------------------")
