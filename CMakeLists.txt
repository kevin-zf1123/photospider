# Minimum CMake version required
cmake_minimum_required(VERSION 3.16)
# CMP0135 was introduced after the supported CMake 3.16 minimum. Older CMake
# versions do not recognize its name, so opt in only when the policy exists.
if(POLICY CMP0135)
  cmake_policy(SET CMP0135 NEW)
endif()
# Define the project
project(Photospider VERSION 0.1.0 LANGUAGES CXX)
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
if(CMAKE_SYSTEM_NAME STREQUAL "Darwin" OR
   CMAKE_SYSTEM_NAME STREQUAL "Linux")
    set(PHOTOSPIDER_IPC_PLATFORM_SUPPORTED ON)
    set(PHOTOSPIDER_BUILD_IPC_DEFAULT ON)
else()
    set(PHOTOSPIDER_IPC_PLATFORM_SUPPORTED OFF)
    set(PHOTOSPIDER_BUILD_IPC_DEFAULT OFF)
endif()
option(PHOTOSPIDER_BUILD_IPC
    "Build the Unix local IPC client, server, and photospiderd"
    ${PHOTOSPIDER_BUILD_IPC_DEFAULT})
if(PHOTOSPIDER_BUILD_IPC AND NOT PHOTOSPIDER_IPC_PLATFORM_SUPPORTED)
    message(FATAL_ERROR
        "PHOTOSPIDER_BUILD_IPC requires CMAKE_SYSTEM_NAME Darwin or Linux")
endif()
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

include(GNUInstallDirs)
include(CMakePackageConfigHelpers)

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
set(PHOTOSPIDER_PUBLIC_INCLUDE_DIR "${PROJECT_SOURCE_DIR}/include")
set(PHOTOSPIDER_BUILD_PUBLIC_INCLUDE_DIR
    "${CMAKE_BINARY_DIR}/generated/photospider_public_include")
set(PHOTOSPIDER_BUILD_PUBLIC_TREE
    "${PHOTOSPIDER_BUILD_PUBLIC_INCLUDE_DIR}/photospider")
set(PHOTOSPIDER_SOURCE_PUBLIC_INCLUDE_DIR
    "${PROJECT_SOURCE_DIR}/include/photospider")
# Phase-4 replaced the old build-tree directory symlink with generated
# forwarding headers. Remove that exact legacy symlink before inventorying or
# writing forwarders so an incremental configure cannot write through the link
# and truncate source headers.
if(IS_SYMLINK "${PHOTOSPIDER_BUILD_PUBLIC_TREE}")
    file(REMOVE "${PHOTOSPIDER_BUILD_PUBLIC_TREE}")
endif()
file(GLOB_RECURSE PHOTOSPIDER_BUILD_PUBLIC_HEADERS
    CONFIGURE_DEPENDS
    "${PHOTOSPIDER_SOURCE_PUBLIC_INCLUDE_DIR}/*.h"
    "${PHOTOSPIDER_SOURCE_PUBLIC_INCLUDE_DIR}/*.hpp"
    "${PHOTOSPIDER_SOURCE_PUBLIC_INCLUDE_DIR}/*.hh"
    "${PHOTOSPIDER_SOURCE_PUBLIC_INCLUDE_DIR}/*.hxx"
)
if(NOT PHOTOSPIDER_BUILD_IPC)
    list(FILTER PHOTOSPIDER_BUILD_PUBLIC_HEADERS EXCLUDE REGEX "/ipc/")
endif()
file(GLOB_RECURSE PHOTOSPIDER_EXISTING_BUILD_PUBLIC_HEADERS
    "${PHOTOSPIDER_BUILD_PUBLIC_TREE}/*.h"
    "${PHOTOSPIDER_BUILD_PUBLIC_TREE}/*.hpp"
    "${PHOTOSPIDER_BUILD_PUBLIC_TREE}/*.hh"
    "${PHOTOSPIDER_BUILD_PUBLIC_TREE}/*.hxx"
)
set(PHOTOSPIDER_EXPECTED_BUILD_PUBLIC_HEADERS "")
foreach(PUBLIC_HEADER ${PHOTOSPIDER_BUILD_PUBLIC_HEADERS})
    file(RELATIVE_PATH PUBLIC_HEADER_RELATIVE_PATH
        "${PHOTOSPIDER_SOURCE_PUBLIC_INCLUDE_DIR}" "${PUBLIC_HEADER}")
    set(PUBLIC_HEADER_COPY
        "${PHOTOSPIDER_BUILD_PUBLIC_TREE}/${PUBLIC_HEADER_RELATIVE_PATH}")
    get_filename_component(PUBLIC_HEADER_COPY_DIR
        "${PUBLIC_HEADER_COPY}" DIRECTORY)
    file(MAKE_DIRECTORY "${PUBLIC_HEADER_COPY_DIR}")
    list(APPEND PHOTOSPIDER_EXPECTED_BUILD_PUBLIC_HEADERS
        "${PUBLIC_HEADER_COPY}")
    file(TO_CMAKE_PATH "${PUBLIC_HEADER}" PUBLIC_HEADER_INCLUDE_PATH)
    file(GENERATE
        OUTPUT "${PUBLIC_HEADER_COPY}"
        CONTENT "#pragma once\n#include \"${PUBLIC_HEADER_INCLUDE_PATH}\"\n")
endforeach()
foreach(EXISTING_PUBLIC_HEADER ${PHOTOSPIDER_EXISTING_BUILD_PUBLIC_HEADERS})
    if(NOT EXISTING_PUBLIC_HEADER IN_LIST PHOTOSPIDER_EXPECTED_BUILD_PUBLIC_HEADERS)
        file(REMOVE "${EXISTING_PUBLIC_HEADER}")
    endif()
endforeach()
# Linked consumers receive only the public include root. Kernel/Interaction
# facades live under src/lib/ as source-tree internals: repository targets that
# include them must use the private roots below, and the installable
# photospider product must not publish src/lib/ to external consumers.
set(PHOTOSPIDER_PRIVATE_INCLUDE_DIRS
    "${PROJECT_SOURCE_DIR}/include"
    "${PROJECT_SOURCE_DIR}/src/lib")
set(PHOTOSPIDER_INSTALL_CMAKEDIR
    "${CMAKE_INSTALL_LIBDIR}/cmake/Photospider"
    CACHE STRING "Install directory for Photospider CMake package files")

# --- Dependencies ---
if(APPLE)
    list(APPEND CMAKE_PREFIX_PATH "/opt/homebrew")
endif()

find_package(Threads REQUIRED)
find_package(OpenCV REQUIRED COMPONENTS core imgproc imgcodecs videoio)
find_package(yaml-cpp REQUIRED)
find_package(CURL) 
find_package(OpenSSL)
if(PHOTOSPIDER_BUILD_IPC OR BUILD_TESTING)
    find_package(nlohmann_json 3.9 REQUIRED)
endif()

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
set(PHOTOSPIDER_CORE_TYPE_SOURCES
    src/lib/core/image_buffer.cpp
    src/lib/core/ps_types.cpp
    src/lib/adapters/opencv/buffer_adapter_opencv.cpp
    src/lib/core/ops.cpp
    src/lib/graph/node_yaml.cpp
)

add_library(photospider_core_types STATIC ${PHOTOSPIDER_CORE_TYPE_SOURCES})
target_include_directories(photospider_core_types PRIVATE
    ${PHOTOSPIDER_PRIVATE_INCLUDE_DIRS}
    ${OpenCV_INCLUDE_DIRS}
)
target_link_libraries(photospider_core_types PRIVATE yaml-cpp::yaml-cpp)

# Module: Operation Plugin Runtime Shim
add_library(photospider_operation_plugin_shim SHARED
    src/lib/core/image_buffer.cpp
    src/lib/adapters/opencv/buffer_adapter_opencv.cpp
)
target_include_directories(photospider_operation_plugin_shim
    PUBLIC
        "$<BUILD_INTERFACE:${PHOTOSPIDER_PUBLIC_INCLUDE_DIR}>"
    PRIVATE
        ${PHOTOSPIDER_PRIVATE_INCLUDE_DIRS}
        ${OpenCV_INCLUDE_DIRS}
)
target_link_libraries(photospider_operation_plugin_shim PUBLIC
    ${OpenCV_LIBS}
)
set_target_properties(photospider_operation_plugin_shim PROPERTIES
    BUILD_RPATH "${CMAKE_BINARY_DIR}/lib"
    INSTALL_RPATH "$ORIGIN"
)
if(WIN32)
    set_target_properties(photospider_operation_plugin_shim PROPERTIES
        WINDOWS_EXPORT_ALL_SYMBOLS ON)
endif()

# Module: Graph
set(PHOTOSPIDER_GRAPH_SOURCES
    src/lib/graph/graph_model.cpp
    src/lib/graph/graph_extent_resolver.cpp
    src/lib/graph/graph_traversal_service.cpp
    src/lib/graph/roi_propagation_service.cpp
    src/lib/graph/graph_io_service.cpp
    src/lib/graph/graph_cache_service.cpp
    src/lib/graph/graph_inspect_service.cpp
)

add_library(photospider_graph STATIC ${PHOTOSPIDER_GRAPH_SOURCES})
target_include_directories(photospider_graph PRIVATE
    ${PHOTOSPIDER_PRIVATE_INCLUDE_DIRS}
    ${OpenCV_INCLUDE_DIRS}
)
target_link_libraries(photospider_graph PRIVATE
    photospider_core_types
    yaml-cpp::yaml-cpp
)

# Module: Plugins
set(PHOTOSPIDER_PLUGIN_SOURCES
    src/lib/plugin/plugin_manager.cpp
    src/lib/plugin/plugin_loader.cpp
)

add_library(photospider_plugin STATIC ${PHOTOSPIDER_PLUGIN_SOURCES})
target_include_directories(photospider_plugin PRIVATE
    ${PHOTOSPIDER_PRIVATE_INCLUDE_DIRS}
    ${OpenCV_INCLUDE_DIRS}
)
target_link_libraries(photospider_plugin PRIVATE
    photospider_core_types
    yaml-cpp::yaml-cpp
)

# Module: Compute
set(KERNEL_FACADE_SOURCES
    src/lib/host/embedded_host.cpp
    src/lib/runtime/kernel.cpp
    src/lib/runtime/kernel_compute.cpp
    src/lib/runtime/kernel_dirty_roi_facade.cpp
    src/lib/runtime/kernel_inspection_facade.cpp
    src/lib/runtime/kernel_io_cache_facade.cpp
    src/lib/runtime/kernel_scheduler_facade.cpp
)

set(COMPUTE_SOURCES
    src/lib/compute/compute_cache_policy.cpp
    src/lib/compute/compute_dispatch_plan_builder.cpp
    src/lib/compute/compute_geometry.cpp
    src/lib/compute/compute_metrics_recorder.cpp
    src/lib/compute/compute_node_task_runner.cpp
    src/lib/compute/compute_result_committer.cpp
    src/lib/compute/compute_task_dependency_state.cpp
    src/lib/compute/compute_task_submission.cpp
    src/lib/compute/task_graph_planning.cpp
    src/lib/compute/task_population_strategy.cpp
    src/lib/compute/tiled_input_normalizer.cpp
    src/lib/compute/dirty_control_lane.cpp
    src/lib/compute/dirty_execution_common.cpp
    src/lib/compute/dirty_node_executor.cpp
    src/lib/compute/dirty_region_planner.cpp
    src/lib/compute/dirty_region_snapshot_builder.cpp
    src/lib/compute/dirty_sibling_commit_gate.cpp
    src/lib/compute/dirty_update_executor.cpp
    src/lib/compute/dirty_write_buffers.cpp
    src/lib/compute/downsample_executor.cpp
    src/lib/compute/intent_update_coordinator.cpp
    src/lib/compute/node_executor.cpp
    src/lib/compute/node_input_resolver.cpp
    src/lib/compute/realtime_proxy_graph.cpp
    src/lib/compute/compute_task_dispatcher.cpp
    src/lib/compute/compute_service.cpp
    src/lib/runtime/graph_event_service.cpp
    src/lib/runtime/interaction.cpp
    src/lib/runtime/graph_runtime.mm
    src/lib/scheduler/cpu_work_stealing_scheduler.cpp
    src/lib/scheduler/gpu_pipeline_scheduler.cpp
    src/lib/scheduler/serial_debug_scheduler.cpp
    src/lib/scheduler/scheduler_factory.cpp
    src/lib/scheduler/scheduler_plugin_loader.cpp
)

if(APPLE)
    enable_language(OBJCXX)
else()
    set_source_files_properties(src/lib/runtime/graph_runtime.mm PROPERTIES LANGUAGE CXX COMPILE_FLAGS "-x c++")
endif()

add_library(photospider_compute STATIC ${COMPUTE_SOURCES})
target_include_directories(photospider_compute PRIVATE
    ${PHOTOSPIDER_PRIVATE_INCLUDE_DIRS}
    ${OpenCV_INCLUDE_DIRS}
)
target_link_libraries(photospider_compute PRIVATE
    photospider_graph
    photospider_plugin
    yaml-cpp::yaml-cpp
)

# ==============================================================================
# 2. Installable Static Library (The Embedded Frontend Product)
# ==============================================================================

file(GLOB PHOTOSPIDER_BENCHMARK_SOURCES "src/lib/benchmark/*.cpp")

set(PHOTOSPIDER_PRODUCT_SOURCES
    ${PHOTOSPIDER_CORE_TYPE_SOURCES}
    ${PHOTOSPIDER_GRAPH_SOURCES}
    ${PHOTOSPIDER_PLUGIN_SOURCES}
    ${COMPUTE_SOURCES}
    ${PHOTOSPIDER_BENCHMARK_SOURCES}
    ${KERNEL_FACADE_SOURCES}
)

add_library(photospider STATIC ${PHOTOSPIDER_PRODUCT_SOURCES})

target_compile_definitions(photospider PUBLIC PHOTOSPIDER_STATIC)
if(BUILD_TESTING)
    target_compile_definitions(photospider PRIVATE
        PHOTOSPIDER_INTERNAL_BAD_ALLOC_TESTING)
endif()
target_compile_features(photospider PUBLIC cxx_std_17)

target_include_directories(photospider
    PUBLIC
        "$<BUILD_INTERFACE:${PHOTOSPIDER_BUILD_PUBLIC_INCLUDE_DIR}>"
        "$<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>"
    PRIVATE
        ${PHOTOSPIDER_PRIVATE_INCLUDE_DIRS}
        ${OpenCV_INCLUDE_DIRS}
)

target_link_libraries(photospider PRIVATE
    Threads::Threads
    yaml-cpp::yaml-cpp
    ${OpenCV_LIBS}
    ${CMAKE_DL_LIBS}
)

# ==============================================================================
# 2a. Versioned Local IPC Products (Unix only)
# ==============================================================================

if(PHOTOSPIDER_BUILD_IPC)
    configure_file(
        "${PROJECT_SOURCE_DIR}/src/lib/ipc/service_version.hpp.in"
        "${CMAKE_BINARY_DIR}/generated/ipc/service_version.hpp"
        @ONLY)

    add_library(photospider_ipc_client_objects OBJECT
        src/lib/ipc/frame.cpp
        src/lib/ipc/unix_socket.cpp
        src/lib/ipc/codec.cpp
        src/lib/ipc/client.cpp
    )
    target_compile_features(photospider_ipc_client_objects PRIVATE cxx_std_17)
    target_include_directories(photospider_ipc_client_objects PRIVATE
        "${PROJECT_SOURCE_DIR}/include"
        "${PROJECT_SOURCE_DIR}/src/lib"
    )
    target_link_libraries(photospider_ipc_client_objects PRIVATE
        nlohmann_json::nlohmann_json)

    add_library(photospider_ipc_client STATIC
        $<TARGET_OBJECTS:photospider_ipc_client_objects>)
    target_compile_definitions(photospider_ipc_client PUBLIC PHOTOSPIDER_STATIC)
    target_compile_features(photospider_ipc_client PUBLIC cxx_std_17)
    target_include_directories(photospider_ipc_client
        PUBLIC
            "$<BUILD_INTERFACE:${PHOTOSPIDER_BUILD_PUBLIC_INCLUDE_DIR}>"
            "$<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>"
    )

    add_library(photospider_ipc_server_internal STATIC
        src/lib/ipc/compute_request_registry.cpp
        src/lib/ipc/session_registry.cpp
        src/lib/ipc/request_router.cpp
        src/lib/ipc/server.cpp
    )
    target_compile_features(photospider_ipc_server_internal PRIVATE cxx_std_17)
    target_include_directories(photospider_ipc_server_internal PRIVATE
        "${PROJECT_SOURCE_DIR}/include"
        "${PROJECT_SOURCE_DIR}/src/lib"
    )
    target_link_libraries(photospider_ipc_server_internal PRIVATE
        photospider_ipc_client
        Threads::Threads
        nlohmann_json::nlohmann_json)

    add_executable(photospiderd apps/photospiderd/main.cpp)
    target_compile_features(photospiderd PRIVATE cxx_std_17)
    target_include_directories(photospiderd PRIVATE
        "${PROJECT_SOURCE_DIR}/include"
        "${PROJECT_SOURCE_DIR}/src/lib"
        "${CMAKE_BINARY_DIR}/generated"
    )
    target_link_libraries(photospiderd PRIVATE
        photospider_ipc_server_internal
        photospider_ipc_client
        photospider
        Threads::Threads)

    message(STATUS
        "Photospider IPC: enabled (Unix socket client + photospiderd v${PROJECT_VERSION})")
else()
    message(STATUS "Photospider IPC: disabled")
endif()

install(TARGETS photospider
    EXPORT PhotospiderTargets
    ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
    LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
    RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
    INCLUDES DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}
)

if(PHOTOSPIDER_BUILD_IPC)
    install(TARGETS photospider_ipc_client
        EXPORT PhotospiderTargets
        ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
        LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
        RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
        INCLUDES DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}
    )
    install(TARGETS photospiderd
        RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
endif()

install(DIRECTORY "${PROJECT_SOURCE_DIR}/include/photospider"
    DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}
    FILES_MATCHING
        PATTERN "*.h"
        PATTERN "*.hpp"
        PATTERN "*.hh"
        PATTERN "*.hxx"
        PATTERN "ipc" EXCLUDE
)

if(PHOTOSPIDER_BUILD_IPC)
    install(DIRECTORY "${PROJECT_SOURCE_DIR}/include/photospider/ipc"
        DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}/photospider
        FILES_MATCHING
            PATTERN "*.h"
            PATTERN "*.hpp"
            PATTERN "*.hh"
            PATTERN "*.hxx"
    )
endif()

install(EXPORT PhotospiderTargets
    NAMESPACE Photospider::
    DESTINATION ${PHOTOSPIDER_INSTALL_CMAKEDIR}
)

configure_package_config_file(
    "${PROJECT_SOURCE_DIR}/cmake/PhotospiderConfig.cmake.in"
    "${CMAKE_CURRENT_BINARY_DIR}/PhotospiderConfig.cmake"
    INSTALL_DESTINATION ${PHOTOSPIDER_INSTALL_CMAKEDIR}
)

write_basic_package_version_file(
    "${CMAKE_CURRENT_BINARY_DIR}/PhotospiderConfigVersion.cmake"
    VERSION ${PROJECT_VERSION}
    COMPATIBILITY SameMajorVersion
)

install(FILES
    "${CMAKE_CURRENT_BINARY_DIR}/PhotospiderConfig.cmake"
    "${CMAKE_CURRENT_BINARY_DIR}/PhotospiderConfigVersion.cmake"
    DESTINATION ${PHOTOSPIDER_INSTALL_CMAKEDIR}
)

# ==============================================================================
# 3. CLI Static Library (The Frontend Logic)
# ==============================================================================

file(GLOB_RECURSE CLI_LOGIC_SOURCES CONFIGURE_DEPENDS
    "apps/graph_cli/src/*.cpp")

add_library(photospider_cli_common STATIC ${CLI_LOGIC_SOURCES})

target_include_directories(photospider_cli_common PRIVATE
    "${PROJECT_SOURCE_DIR}/apps/graph_cli/include"
    "${PROJECT_SOURCE_DIR}/include"
    "${PROJECT_SOURCE_DIR}/src/lib"
    ${OpenCV_INCLUDE_DIRS}
)
target_compile_definitions(photospider_cli_common PRIVATE
    PHOTOSPIDER_GRAPH_CLI_HELP_DIR=\"${PROJECT_SOURCE_DIR}/apps/graph_cli/resources/help\")

target_link_libraries(photospider_cli_common PUBLIC
    photospider
    ${FTXUI_TARGETS}
)
target_link_libraries(photospider_cli_common PRIVATE
    yaml-cpp::yaml-cpp
    ${OpenCV_LIBS}
)

# ==============================================================================
# 4. Main Executable
# ==============================================================================

add_executable(graph_cli apps/graph_cli/main.cpp)
target_include_directories(graph_cli PRIVATE
    "${PROJECT_SOURCE_DIR}/apps/graph_cli/include"
    "${PROJECT_SOURCE_DIR}/include"
    "${PROJECT_SOURCE_DIR}/src/lib"
    ${OpenCV_INCLUDE_DIRS}
)
target_link_libraries(graph_cli PRIVATE photospider_cli_common)

if (UNIX AND NOT APPLE)
    target_link_libraries(graph_cli PRIVATE dl pthread)
endif()

set_target_properties(graph_cli PROPERTIES
    BUILD_RPATH "${CMAKE_BINARY_DIR}/lib"
    INSTALL_RPATH "$ORIGIN/../lib"
)

# ==============================================================================
# 5. Standard Operation Plugins
# ==============================================================================
# Build everything in plugins/ops/ except the special Metal loader.

file(MAKE_DIRECTORY ${PLUGIN_OUTPUT_DIR})
file(GLOB PLUGIN_SOURCES "plugins/ops/*.cpp")

set(OP_PLUGIN_TARGETS "")
foreach(PLUGIN_SOURCE ${PLUGIN_SOURCES})
    get_filename_component(PLUGIN_NAME ${PLUGIN_SOURCE} NAME_WE)
    add_library(${PLUGIN_NAME} SHARED ${PLUGIN_SOURCE})
    list(APPEND OP_PLUGIN_TARGETS ${PLUGIN_NAME})
    target_compile_definitions(${PLUGIN_NAME} PRIVATE
        PHOTOSPIDER_PLUGIN_BUILD)
    
    set_target_properties(${PLUGIN_NAME} PROPERTIES
        LIBRARY_OUTPUT_DIRECTORY "${PLUGIN_OUTPUT_DIR}"
        BUILD_RPATH "${CMAKE_BINARY_DIR}/lib"
    )
    
    target_include_directories(${PLUGIN_NAME} PRIVATE
        "${PROJECT_SOURCE_DIR}/include"
        "${PROJECT_SOURCE_DIR}/src/lib"
        ${OpenCV_INCLUDE_DIRS}
    )
    target_link_libraries(${PLUGIN_NAME} PRIVATE
        photospider_operation_plugin_shim
        yaml-cpp::yaml-cpp
    )
endforeach()

# ==============================================================================
# 6. Scheduler Plugins (Dynamic Loadable Schedulers)
# ==============================================================================

set(SCHEDULER_PLUGIN_DIR ${CMAKE_BINARY_DIR}/schedulers)
file(MAKE_DIRECTORY ${SCHEDULER_PLUGIN_DIR})

# Production/demo scheduler plugins. Test-only fixtures are built into a
# separate directory below so config.yaml's default scheduler_dirs does not
# expose lifecycle test schedulers to users.
file(GLOB SCHEDULER_PLUGIN_SOURCES
    "plugins/schedulers/*_example_plugin.cpp")

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
        "${PROJECT_SOURCE_DIR}/src/lib"
        ${OpenCV_INCLUDE_DIRS}
    )
    target_link_libraries(${SCHEDULER_NAME} PRIVATE
        photospider
        yaml-cpp::yaml-cpp
        ${OpenCV_LIBS}
    )
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
        "${PROJECT_SOURCE_DIR}/src/lib"
        ${OpenCV_INCLUDE_DIRS}
    )
    target_link_libraries(destroy_count_scheduler_plugin PRIVATE
        photospider
        yaml-cpp::yaml-cpp
        ${OpenCV_LIBS}
    )

    add_library(lifecycle_op_plugin SHARED
        tests/fixtures/ops/lifecycle_op_plugin.cpp
    )
    target_compile_definitions(lifecycle_op_plugin PRIVATE
        PHOTOSPIDER_PLUGIN_BUILD)
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
        "${PROJECT_SOURCE_DIR}/src/lib"
        ${OpenCV_INCLUDE_DIRS}
    )
    target_link_libraries(lifecycle_op_plugin PRIVATE
        yaml-cpp::yaml-cpp
        ${OpenCV_LIBS}
    )

    add_library(override_lifecycle_op_plugin SHARED
        tests/fixtures/ops/override_lifecycle_op_plugin.cpp
    )
    target_compile_definitions(override_lifecycle_op_plugin PRIVATE
        PHOTOSPIDER_PLUGIN_BUILD)
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
        "${PROJECT_SOURCE_DIR}/src/lib"
        ${OpenCV_INCLUDE_DIRS}
    )
    target_link_libraries(override_lifecycle_op_plugin PRIVATE
        yaml-cpp::yaml-cpp
        ${OpenCV_LIBS}
    )
endif()

# ==============================================================================
# 7. Metal Support (Apple Only)
# ==============================================================================
if(APPLE)
    set(METAL_ROOT_DIR ${CMAKE_BINARY_DIR}/high_performance/metal)
    set(METAL_OPS_DIR  ${METAL_ROOT_DIR}/ops)
    file(MAKE_DIRECTORY ${METAL_ROOT_DIR})
    file(MAKE_DIRECTORY ${METAL_OPS_DIR})

    find_library(METAL_FRAMEWORK NAMES Metal)
    find_library(FOUNDATION_FRAMEWORK NAMES Foundation)
    find_library(COREIMAGE_FRAMEWORK NAMES CoreImage)
    find_library(COREVIDEO_FRAMEWORK NAMES CoreVideo)
    if(NOT METAL_FRAMEWORK OR NOT FOUNDATION_FRAMEWORK OR
       NOT COREIMAGE_FRAMEWORK OR NOT COREVIDEO_FRAMEWORK)
        message(FATAL_ERROR
            "Photospider requires Metal, Foundation, CoreImage, and CoreVideo on Apple platforms")
    endif()

    # Core product needs Metal framework for Objective-C++ runtime sources.
    target_link_libraries(photospider PRIVATE
        "-framework Metal"
        "-framework Foundation"
    )

    # 1. The Implementation Library (the actual op logic)
    add_library(perlin_noise_metal SHARED
        plugins/ops/metal/perlin_noise_metal.mm)
    set_target_properties(perlin_noise_metal PROPERTIES
        LIBRARY_OUTPUT_DIRECTORY "${METAL_OPS_DIR}"
        MACOSX_RPATH ON
    )
    target_link_libraries(perlin_noise_metal PRIVATE
        photospider_operation_plugin_shim
        yaml-cpp::yaml-cpp
        ${METAL_FRAMEWORK} ${FOUNDATION_FRAMEWORK} ${COREIMAGE_FRAMEWORK} ${COREVIDEO_FRAMEWORK}
    )
    target_include_directories(perlin_noise_metal PRIVATE
        "${PROJECT_SOURCE_DIR}/include"
        "${PROJECT_SOURCE_DIR}/src/lib"
        "${PROJECT_SOURCE_DIR}/plugins/ops"
        ${OpenCV_INCLUDE_DIRS}
    )

    # 2. The Loader Plugin (registers the op with the Kernel)
    add_library(metal_ops_loader SHARED
        plugins/ops/metal/metal_ops_loader.cpp)
    target_compile_definitions(metal_ops_loader PRIVATE
        PHOTOSPIDER_PLUGIN_BUILD)
    set_target_properties(metal_ops_loader PROPERTIES
        LIBRARY_OUTPUT_DIRECTORY "${METAL_ROOT_DIR}"
        # Point RPATH to 'ops' subdir so loader can find perlin_noise_metal
        BUILD_RPATH "@loader_path/ops;${CMAKE_BINARY_DIR}/lib"
        INSTALL_RPATH "@loader_path/ops;$ORIGIN/../lib"
        MACOSX_RPATH ON
    )

    target_include_directories(metal_ops_loader PRIVATE
        "${PROJECT_SOURCE_DIR}/include"
        "${PROJECT_SOURCE_DIR}/src/lib"
        "${PROJECT_SOURCE_DIR}/plugins/ops"
        ${OpenCV_INCLUDE_DIRS}
    )

    # [修复关键点] 必须链接 perlin_noise_metal，否则 loader 运行时找不到符号
    target_link_libraries(metal_ops_loader PRIVATE 
        perlin_noise_metal
        yaml-cpp::yaml-cpp
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
    find_package(Python3 3.9 REQUIRED COMPONENTS Interpreter)
    find_package(GTest REQUIRED)
    macro(add_ps_test TEST_NAME SOURCE_FILE)
        add_executable(${TEST_NAME} ${SOURCE_FILE})
        set_target_properties(${TEST_NAME} PROPERTIES
            RUNTIME_OUTPUT_DIRECTORY "${TEST_OUTPUT_DIR}"
            BUILD_RPATH "${CMAKE_BINARY_DIR}/lib"
        )
        target_include_directories(${TEST_NAME} PRIVATE
            "${PROJECT_SOURCE_DIR}/include"
            "${PROJECT_SOURCE_DIR}/src/lib"
            "${PROJECT_SOURCE_DIR}/tests"
            ${OpenCV_INCLUDE_DIRS})
        target_link_libraries(${TEST_NAME} PRIVATE
            photospider
            GTest::gtest
            GTest::gtest_main
            yaml-cpp::yaml-cpp
            ${OpenCV_LIBS})
    endmacro()

    add_ps_test(test_scheduler tests/integration/test_scheduler.cpp)
    target_link_libraries(test_scheduler PRIVATE nlohmann_json::nlohmann_json)

    if(PHOTOSPIDER_BUILD_IPC)
        add_executable(test_ipc_protocol tests/unit/test_ipc_protocol.cpp)
        set_target_properties(test_ipc_protocol PROPERTIES
            RUNTIME_OUTPUT_DIRECTORY "${TEST_OUTPUT_DIR}"
            BUILD_RPATH "${CMAKE_BINARY_DIR}/lib")
        target_compile_features(test_ipc_protocol PRIVATE cxx_std_17)
        target_include_directories(test_ipc_protocol PRIVATE
            "${PROJECT_SOURCE_DIR}/include"
            "${PROJECT_SOURCE_DIR}/src/lib"
            "${PROJECT_SOURCE_DIR}/tests"
            ${OpenCV_INCLUDE_DIRS})
        target_compile_definitions(test_ipc_protocol PRIVATE
            PS_TEST_SCHEDULER_PLUGIN_PATH="$<TARGET_FILE:destroy_count_scheduler_plugin>")
        target_link_libraries(test_ipc_protocol PRIVATE
            photospider_ipc_server_internal
            photospider_ipc_client
            photospider
            GTest::gtest
            GTest::gtest_main
            nlohmann_json::nlohmann_json
            Threads::Threads
            ${CMAKE_DL_LIBS})
        add_dependencies(test_ipc_protocol destroy_count_scheduler_plugin)

        add_executable(test_compute_request_registry
            tests/unit/test_compute_request_registry.cpp)
        set_target_properties(test_compute_request_registry PROPERTIES
            RUNTIME_OUTPUT_DIRECTORY "${TEST_OUTPUT_DIR}"
            BUILD_RPATH "${CMAKE_BINARY_DIR}/lib")
        target_compile_features(test_compute_request_registry PRIVATE cxx_std_17)
        target_include_directories(test_compute_request_registry PRIVATE
            "${PROJECT_SOURCE_DIR}/include"
            "${PROJECT_SOURCE_DIR}/src/lib"
            "${PROJECT_SOURCE_DIR}/tests")
        target_link_libraries(test_compute_request_registry PRIVATE
            photospider_ipc_server_internal
            photospider_ipc_client
            photospider
            GTest::gtest
            GTest::gtest_main
            Threads::Threads)

        add_executable(test_ipc_daemon
            tests/integration/test_ipc_daemon.cpp)
        set_target_properties(test_ipc_daemon PROPERTIES
            RUNTIME_OUTPUT_DIRECTORY "${TEST_OUTPUT_DIR}"
            BUILD_RPATH "${CMAKE_BINARY_DIR}/lib")
        target_compile_features(test_ipc_daemon PRIVATE cxx_std_17)
        target_compile_definitions(test_ipc_daemon PRIVATE
            PS_PHOTOSPIDERD_PATH="$<TARGET_FILE:photospiderd>")
        target_include_directories(test_ipc_daemon PRIVATE
            "${PROJECT_SOURCE_DIR}/include"
            "${PROJECT_SOURCE_DIR}/src/lib")
        target_link_libraries(test_ipc_daemon PRIVATE
            photospider_ipc_client
            GTest::gtest
            GTest::gtest_main
            Threads::Threads)
        add_dependencies(test_ipc_daemon photospiderd)
    endif()

    add_ps_test(test_milestone2 tests/integration/test_milestone2.cpp)
    add_ps_test(test_milestone3 tests/integration/test_milestone3.cpp)
    add_ps_test(test_milestone34 tests/integration/test_milestone34.cpp)
    add_ps_test(test_propagation tests/integration/test_propagation.cpp)
    add_ps_test(test_propagation_contracts
        tests/unit/test_propagation_contracts.cpp)
    add_ps_test(test_graph_topology_boundaries
        tests/unit/test_graph_topology_boundaries.cpp)
    add_ps_test(test_kernel_contracts
        tests/integration/test_kernel_contracts.cpp)
    add_ps_test(test_image_buffer_contracts
        tests/unit/test_image_buffer_contracts.cpp)
    add_ps_test(test_event_stream_boundaries
        tests/unit/test_event_stream_boundaries.cpp)
    add_ps_test(test_cli_compute_event_paging
        tests/unit/test_cli_compute_event_paging.cpp)
    target_include_directories(test_cli_compute_event_paging PRIVATE
        "${PROJECT_SOURCE_DIR}/apps/graph_cli/include")
    add_ps_test(test_host_adapter tests/integration/test_host_adapter.cpp)
    target_link_libraries(test_host_adapter PRIVATE ${CMAKE_DL_LIBS})
    target_compile_definitions(test_host_adapter PRIVATE
        PS_TEST_OP_PLUGIN_DIR="${TEST_OP_PLUGIN_DIR}"
        PS_TEST_SCHEDULER_PLUGIN_PATH="$<TARGET_FILE:destroy_count_scheduler_plugin>"
        PHOTOSPIDER_INTERNAL_BAD_ALLOC_TESTING
    )
    add_dependencies(test_host_adapter lifecycle_op_plugin
        override_lifecycle_op_plugin destroy_count_scheduler_plugin)
    add_ps_test(test_bad_alloc_boundaries
        tests/integration/test_bad_alloc_boundaries.cpp)
    target_sources(test_bad_alloc_boundaries PRIVATE
        plugins/ops/threshold_op.cpp)
    target_include_directories(test_bad_alloc_boundaries PRIVATE
        "${PROJECT_SOURCE_DIR}/apps/graph_cli/include"
        "${PROJECT_SOURCE_DIR}/plugins/ops"
        "${PROJECT_SOURCE_DIR}")
    target_compile_definitions(test_bad_alloc_boundaries PRIVATE
        PHOTOSPIDER_PLUGIN_BUILD
        PHOTOSPIDER_THRESHOLD_BAD_ALLOC_TESTING)
    target_link_libraries(test_bad_alloc_boundaries PRIVATE
        photospider_cli_common)
    if(APPLE)
        target_sources(test_bad_alloc_boundaries PRIVATE
            plugins/ops/metal/perlin_noise_metal.mm)
        target_link_libraries(test_bad_alloc_boundaries PRIVATE
            ${METAL_FRAMEWORK}
            ${FOUNDATION_FRAMEWORK}
            ${COREIMAGE_FRAMEWORK}
            ${COREVIDEO_FRAMEWORK})
    endif()
    add_ps_test(test_cli_dirty_snapshot_formatter
        tests/integration/test_cli_dirty_snapshot_formatter.cpp)
    target_include_directories(test_cli_dirty_snapshot_formatter PRIVATE
        "${PROJECT_SOURCE_DIR}/apps/graph_cli/include")
    target_link_libraries(test_cli_dirty_snapshot_formatter PRIVATE
        photospider_cli_common)
    add_ps_test(test_compute_service_split
        tests/integration/test_compute_service_split.cpp)
    add_ps_test(test_node_editor_layout
        tests/unit/test_node_editor_layout.cpp)
    target_include_directories(test_node_editor_layout PRIVATE
        "${PROJECT_SOURCE_DIR}/apps/graph_cli/include")
    target_link_libraries(test_node_editor_layout PRIVATE ${FTXUI_TARGETS})

    # [M3.1] OpRegistry 多设备实现测试
    add_ps_test(test_op_registry_m31 tests/unit/test_op_registry_m31.cpp)

    # Operation plugin manager handle lifetime tests
    add_ps_test(test_plugin_manager
        tests/integration/test_plugin_manager.cpp)
    target_link_libraries(test_plugin_manager PRIVATE ${CMAKE_DL_LIBS})
    target_compile_definitions(test_plugin_manager PRIVATE
        PS_STANDARD_OP_PLUGIN_DIR="${PLUGIN_OUTPUT_DIR}"
        PS_TEST_OP_PLUGIN_DIR="${TEST_OP_PLUGIN_DIR}"
        PHOTOSPIDER_INTERNAL_BAD_ALLOC_TESTING
    )
    add_dependencies(test_plugin_manager lifecycle_op_plugin
        override_lifecycle_op_plugin ${OP_PLUGIN_TARGETS})

    # [M3.5] GPU Pipeline 调度器测试
    add_ps_test(test_gpu_pipeline_scheduler
        tests/integration/test_gpu_pipeline_scheduler.cpp)

    # Deterministic CPU/GPU exception-publication race and reuse coverage.
    add_ps_test(test_scheduler_exception_publication
        tests/unit/test_scheduler_exception_publication.cpp)
    target_compile_definitions(test_scheduler_exception_publication PRIVATE
        PHOTOSPIDER_INTERNAL_BAD_ALLOC_TESTING)

    # [M3.5] 调度器插件加载器测试
    add_ps_test(test_scheduler_plugin_loader
        tests/integration/test_scheduler_plugin_loader.cpp)
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
    if(NOT PHOTOSPIDER_BUILD_IPC)
        list(FILTER PHOTOSPIDER_INSTALLABLE_PUBLIC_HEADERS
            EXCLUDE REGEX "/ipc/")
    endif()
    if(NOT PHOTOSPIDER_INSTALLABLE_PUBLIC_HEADERS)
        message(FATAL_ERROR
            "Expected installable public headers under include/photospider")
    endif()
    file(MAKE_DIRECTORY
        "${CMAKE_BINARY_DIR}/generated/public_header_self_containment")
    set(PHOTOSPIDER_PUBLIC_HEADER_SELF_CONTAINMENT_SOURCES "")
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
        list(APPEND PHOTOSPIDER_PUBLIC_HEADER_SELF_CONTAINMENT_SOURCES
            "${PUBLIC_HEADER_SOURCE}")
    endforeach()
    add_library(public_header_self_containment_objects OBJECT
        ${PHOTOSPIDER_PUBLIC_HEADER_SELF_CONTAINMENT_SOURCES}
    )
    target_include_directories(public_header_self_containment_objects
        PRIVATE "${PROJECT_SOURCE_DIR}/include"
    )
    target_compile_features(public_header_self_containment_objects
        PRIVATE cxx_std_17
    )
    add_custom_target(public_header_self_containment)
    add_dependencies(public_header_self_containment
        public_header_self_containment_objects)
    # CTest is reserved for long-lived runtime, package-consumer, compile, and
    # error-boundary behavior. Migration/residue scanners and source-quality
    # audits are intentionally absent from the test inventory.
    if(PHOTOSPIDER_BUILD_IPC)
        add_test(NAME StaticProductConsumerSmoke
            COMMAND ${Python3_EXECUTABLE}
                "${PROJECT_SOURCE_DIR}/tests/integration/static_product_consumer_smoke.py"
                --repo "${PROJECT_SOURCE_DIR}"
                --build "${CMAKE_BINARY_DIR}"
                --work "${CMAKE_BINARY_DIR}/tests/static_product_consumer_smoke"
                --cmake-executable "${CMAKE_COMMAND}"
                --config $<CONFIG>
            WORKING_DIRECTORY ${PROJECT_SOURCE_DIR})
        set_tests_properties(StaticProductConsumerSmoke PROPERTIES
            RUN_SERIAL TRUE TIMEOUT 600)
    endif()
    add_test(NAME IpcDisabledInstallSmoke
        COMMAND ${Python3_EXECUTABLE}
            "${PROJECT_SOURCE_DIR}/tests/integration/ipc_disabled_install_smoke.py"
            --repo "${PROJECT_SOURCE_DIR}"
            --work "${CMAKE_BINARY_DIR}/tests/ipc_disabled_install_smoke"
            --cmake-executable "${CMAKE_COMMAND}"
            --config $<CONFIG>
        WORKING_DIRECTORY ${PROJECT_SOURCE_DIR})
    set_tests_properties(IpcDisabledInstallSmoke PROPERTIES
        RUN_SERIAL TRUE TIMEOUT 600)
    add_test(NAME GraphCliOptionBadAlloc
        COMMAND ${Python3_EXECUTABLE}
            "${PROJECT_SOURCE_DIR}/tests/integration/verify_graph_cli_option_bad_alloc.py"
            --repo "${PROJECT_SOURCE_DIR}"
            --binary "$<TARGET_FILE:graph_cli>"
            --out "${CMAKE_BINARY_DIR}/tests/graph_cli_option_bad_alloc"
        WORKING_DIRECTORY ${PROJECT_SOURCE_DIR})
    set_tests_properties(GraphCliOptionBadAlloc PROPERTIES RUN_SERIAL TRUE)
    add_test(NAME PublicHeaderSelfContainment
        COMMAND ${CMAKE_COMMAND} --build "${CMAKE_BINARY_DIR}"
            --target public_header_self_containment --config $<CONFIG>
        WORKING_DIRECTORY ${PROJECT_SOURCE_DIR})
    
    gtest_discover_tests(test_scheduler WORKING_DIRECTORY ${PROJECT_SOURCE_DIR})
    if(PHOTOSPIDER_BUILD_IPC)
        gtest_discover_tests(test_ipc_protocol
            WORKING_DIRECTORY ${PROJECT_SOURCE_DIR}
            PROPERTIES TIMEOUT 30)
        gtest_discover_tests(test_compute_request_registry
            WORKING_DIRECTORY ${PROJECT_SOURCE_DIR}
            PROPERTIES TIMEOUT 30)
        gtest_discover_tests(test_ipc_daemon
            WORKING_DIRECTORY ${PROJECT_SOURCE_DIR}
            PROPERTIES TIMEOUT 60)
    endif()
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
    gtest_discover_tests(test_event_stream_boundaries
        WORKING_DIRECTORY ${PROJECT_SOURCE_DIR})
    gtest_discover_tests(test_cli_compute_event_paging
        WORKING_DIRECTORY ${PROJECT_SOURCE_DIR})
    gtest_discover_tests(test_host_adapter WORKING_DIRECTORY ${PROJECT_SOURCE_DIR})
    gtest_discover_tests(test_bad_alloc_boundaries
        WORKING_DIRECTORY ${PROJECT_SOURCE_DIR})
    gtest_discover_tests(test_cli_dirty_snapshot_formatter WORKING_DIRECTORY ${PROJECT_SOURCE_DIR})
    gtest_discover_tests(test_compute_service_split WORKING_DIRECTORY ${PROJECT_SOURCE_DIR})
    gtest_discover_tests(test_node_editor_layout WORKING_DIRECTORY ${PROJECT_SOURCE_DIR})
    gtest_discover_tests(test_gpu_pipeline_scheduler WORKING_DIRECTORY ${PROJECT_SOURCE_DIR})
    gtest_discover_tests(test_scheduler_exception_publication
        WORKING_DIRECTORY ${PROJECT_SOURCE_DIR})
    gtest_discover_tests(test_scheduler_plugin_loader WORKING_DIRECTORY ${PROJECT_SOURCE_DIR})
endif()

# --- Summary ---
message(STATUS "-----------------------------------------------------------")
message(STATUS "Build Configuration Summary:")
message(STATUS "  Product Lib (Static): libphotospider (Embedded frontend)")
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
