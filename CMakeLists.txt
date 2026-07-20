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
option(PHOTOSPIDER_ENABLE_OPENCV
    "Enable OpenCV-backed image processing, codecs, and public adapters" ON)
option(PHOTOSPIDER_ENABLE_YAML
    "Enable YAML graph-document and cache-metadata persistence" ON)
set(PHOTOSPIDER_BUILD_OPENCV_OPERATION_PROVIDER_DEFAULT
    ${PHOTOSPIDER_ENABLE_OPENCV})
option(PHOTOSPIDER_BUILD_OPENCV_OPERATION_PROVIDER
    "Build the repository-owned OpenCV CPU operation provider"
    ${PHOTOSPIDER_BUILD_OPENCV_OPERATION_PROVIDER_DEFAULT})
set(PHOTOSPIDER_BUILD_OPENCV_OPERATION_PLUGINS_DEFAULT
    ${PHOTOSPIDER_ENABLE_OPENCV})
option(PHOTOSPIDER_BUILD_OPENCV_OPERATION_PLUGINS
    "Build repository-owned OpenCV operation plugin shared libraries"
    ${PHOTOSPIDER_BUILD_OPENCV_OPERATION_PLUGINS_DEFAULT})
if(PHOTOSPIDER_ENABLE_OPENCV AND PHOTOSPIDER_ENABLE_YAML)
    set(PHOTOSPIDER_BUILD_GRAPH_CLI_DEFAULT ON)
else()
    set(PHOTOSPIDER_BUILD_GRAPH_CLI_DEFAULT OFF)
endif()
option(PHOTOSPIDER_BUILD_GRAPH_CLI
    "Build the graph_cli frontend and its FTXUI support library"
    ${PHOTOSPIDER_BUILD_GRAPH_CLI_DEFAULT})
if(PHOTOSPIDER_BUILD_OPENCV_OPERATION_PROVIDER AND
   NOT PHOTOSPIDER_ENABLE_OPENCV)
    message(FATAL_ERROR
        "PHOTOSPIDER_BUILD_OPENCV_OPERATION_PROVIDER=ON requires PHOTOSPIDER_ENABLE_OPENCV=ON")
endif()
if(PHOTOSPIDER_BUILD_OPENCV_OPERATION_PLUGINS AND
   NOT PHOTOSPIDER_ENABLE_OPENCV)
    message(FATAL_ERROR
        "PHOTOSPIDER_BUILD_OPENCV_OPERATION_PLUGINS=ON requires PHOTOSPIDER_ENABLE_OPENCV=ON")
endif()
if(PHOTOSPIDER_BUILD_GRAPH_CLI AND
   (NOT PHOTOSPIDER_ENABLE_OPENCV OR NOT PHOTOSPIDER_ENABLE_YAML))
    message(FATAL_ERROR
        "PHOTOSPIDER_BUILD_GRAPH_CLI=ON requires PHOTOSPIDER_ENABLE_OPENCV=ON and PHOTOSPIDER_ENABLE_YAML=ON")
endif()
if(BUILD_TESTING AND PHOTOSPIDER_ENABLE_OPENCV AND
   PHOTOSPIDER_ENABLE_YAML)
    set(PHOTOSPIDER_BUILD_PROVIDER_INDEPENDENT_FOCUSED_TESTS ON)
else()
    set(PHOTOSPIDER_BUILD_PROVIDER_INDEPENDENT_FOCUSED_TESTS OFF)
endif()
if(PHOTOSPIDER_BUILD_PROVIDER_INDEPENDENT_FOCUSED_TESTS AND
   PHOTOSPIDER_BUILD_GRAPH_CLI AND
   PHOTOSPIDER_BUILD_OPENCV_OPERATION_PROVIDER AND
   PHOTOSPIDER_BUILD_OPENCV_OPERATION_PLUGINS)
    set(PHOTOSPIDER_BUILD_FULL_TEST_SUITE ON)
else()
    set(PHOTOSPIDER_BUILD_FULL_TEST_SUITE OFF)
endif()
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
# The public contract is an explicit allowlist. Keep this inventory independent
# of filesystem globs so editor metadata, migration leftovers, and accidental
# headers can never become generated or installed package surface.
set(PHOTOSPIDER_BASE_PUBLIC_HEADER_RELATIVE_PATHS
    core/compute_intent.hpp
    core/device.hpp
    core/export.hpp
    core/geometry.hpp
    core/graph_error.hpp
    core/image_buffer.hpp
    core/inspection_types.hpp
    core/result_types.hpp
    host/compute_request.hpp
    host/event_stream.hpp
    host/graph_session.hpp
    host/host.hpp
    plugin/node_view.hpp
    plugin/op_contract.hpp
    plugin/plugin_api.hpp
    public_boundary.hpp
    scheduler/scheduler.hpp
    scheduler/scheduler_plugin_api.hpp
    scheduler/scheduler_task_runtime.hpp
)
set(PHOTOSPIDER_IPC_PUBLIC_HEADER_RELATIVE_PATHS
    ipc/client.hpp
    ipc/host.hpp
    ipc/protocol.hpp
)
set(PHOTOSPIDER_INSTALLABLE_PUBLIC_HEADER_RELATIVE_PATHS
    ${PHOTOSPIDER_BASE_PUBLIC_HEADER_RELATIVE_PATHS})
if(PHOTOSPIDER_ENABLE_OPENCV)
    list(APPEND PHOTOSPIDER_INSTALLABLE_PUBLIC_HEADER_RELATIVE_PATHS
        plugin/opencv_adapter.hpp)
endif()
set(PHOTOSPIDER_EXPECTED_PUBLIC_HEADER_COUNT 19)
if(PHOTOSPIDER_ENABLE_OPENCV)
    math(EXPR PHOTOSPIDER_EXPECTED_PUBLIC_HEADER_COUNT
        "${PHOTOSPIDER_EXPECTED_PUBLIC_HEADER_COUNT} + 1")
endif()
if(PHOTOSPIDER_BUILD_IPC)
    list(APPEND PHOTOSPIDER_INSTALLABLE_PUBLIC_HEADER_RELATIVE_PATHS
        ${PHOTOSPIDER_IPC_PUBLIC_HEADER_RELATIVE_PATHS})
    math(EXPR PHOTOSPIDER_EXPECTED_PUBLIC_HEADER_COUNT
        "${PHOTOSPIDER_EXPECTED_PUBLIC_HEADER_COUNT} + 3")
endif()
list(LENGTH PHOTOSPIDER_INSTALLABLE_PUBLIC_HEADER_RELATIVE_PATHS
    PHOTOSPIDER_ACTUAL_PUBLIC_HEADER_COUNT)
if(NOT PHOTOSPIDER_ACTUAL_PUBLIC_HEADER_COUNT EQUAL
   PHOTOSPIDER_EXPECTED_PUBLIC_HEADER_COUNT)
    message(FATAL_ERROR
        "Public header allowlist count mismatch: expected ${PHOTOSPIDER_EXPECTED_PUBLIC_HEADER_COUNT}, got ${PHOTOSPIDER_ACTUAL_PUBLIC_HEADER_COUNT}")
endif()

file(GLOB_RECURSE PHOTOSPIDER_EXISTING_BUILD_PUBLIC_HEADERS
    "${PHOTOSPIDER_BUILD_PUBLIC_TREE}/*.h"
    "${PHOTOSPIDER_BUILD_PUBLIC_TREE}/*.hpp"
    "${PHOTOSPIDER_BUILD_PUBLIC_TREE}/*.hh"
    "${PHOTOSPIDER_BUILD_PUBLIC_TREE}/*.hxx"
)
set(PHOTOSPIDER_EXPECTED_BUILD_PUBLIC_HEADERS "")
foreach(PUBLIC_HEADER_RELATIVE_PATH
        IN LISTS PHOTOSPIDER_INSTALLABLE_PUBLIC_HEADER_RELATIVE_PATHS)
    set(PUBLIC_HEADER
        "${PHOTOSPIDER_SOURCE_PUBLIC_INCLUDE_DIR}/${PUBLIC_HEADER_RELATIVE_PATH}")
    if(NOT EXISTS "${PUBLIC_HEADER}")
        message(FATAL_ERROR
            "Allowlisted public header does not exist: ${PUBLIC_HEADER}")
    endif()
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

# Public image-buffer factory implementation. This target deliberately has no
# OpenCV, yaml-cpp, Threads, graph, registry, or embedded product dependency.
add_library(photospider_operation_runtime STATIC
    src/lib/core/image_buffer.cpp)
add_library(Photospider::operation_runtime ALIAS
    photospider_operation_runtime)
set_target_properties(photospider_operation_runtime PROPERTIES
    EXPORT_NAME operation_runtime)
target_compile_features(photospider_operation_runtime PUBLIC cxx_std_17)
target_include_directories(photospider_operation_runtime
    PUBLIC
        "$<BUILD_INTERFACE:${PHOTOSPIDER_BUILD_PUBLIC_INCLUDE_DIR}>"
        "$<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>"
    PRIVATE
        "${PROJECT_SOURCE_DIR}/src/lib")

# Public operation contracts plus the standard-library-only image factory.
# Linking the SDK is sufficient for a complete external operation DSO.
add_library(photospider_operation_sdk INTERFACE)
add_library(Photospider::operation_sdk ALIAS photospider_operation_sdk)
set_target_properties(photospider_operation_sdk PROPERTIES
    EXPORT_NAME operation_sdk)
target_compile_features(photospider_operation_sdk INTERFACE cxx_std_17)
target_include_directories(photospider_operation_sdk INTERFACE
    "$<BUILD_INTERFACE:${PHOTOSPIDER_BUILD_PUBLIC_INCLUDE_DIR}>"
    "$<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>")
target_link_libraries(photospider_operation_sdk INTERFACE
    Photospider::operation_runtime)

# Public scheduler SDK: headers and language level only. Concrete plugins add
# their own threading/runtime dependencies and never inherit the embedded
# product's OpenCV, yaml-cpp, Metal, or host implementation dependencies.
add_library(photospider_scheduler_sdk INTERFACE)
add_library(Photospider::scheduler_sdk ALIAS photospider_scheduler_sdk)
set_target_properties(photospider_scheduler_sdk PROPERTIES
    EXPORT_NAME scheduler_sdk)
target_compile_features(photospider_scheduler_sdk INTERFACE cxx_std_17)
target_include_directories(photospider_scheduler_sdk INTERFACE
    "$<BUILD_INTERFACE:${PHOTOSPIDER_BUILD_PUBLIC_INCLUDE_DIR}>"
    "$<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>")

# --- Dependencies ---
if(APPLE)
    list(APPEND CMAKE_PREFIX_PATH "/opt/homebrew")
endif()

find_package(Threads REQUIRED)
if(PHOTOSPIDER_ENABLE_OPENCV)
    find_package(OpenCV REQUIRED COMPONENTS core imgproc imgcodecs videoio)
endif()
if(PHOTOSPIDER_ENABLE_YAML)
    find_package(yaml-cpp REQUIRED)
endif()
find_package(CURL) 
find_package(OpenSSL)
if(PHOTOSPIDER_BUILD_IPC OR BUILD_TESTING)
    find_package(nlohmann_json 3.9 REQUIRED)
endif()

# OpenCV packages normally publish component targets. Accept common namespaced
# variants while refusing a raw `${OpenCV_LIBS}` fallback from the public
# adapter export.
if(PHOTOSPIDER_ENABLE_OPENCV)
    if(TARGET opencv_core)
        set(PHOTOSPIDER_OPENCV_CORE_TARGET opencv_core)
    elseif(TARGET OpenCV::opencv_core)
        set(PHOTOSPIDER_OPENCV_CORE_TARGET OpenCV::opencv_core)
    elseif(TARGET OpenCV::Core)
        set(PHOTOSPIDER_OPENCV_CORE_TARGET OpenCV::Core)
    else()
        message(FATAL_ERROR
            "OpenCV core component did not provide an exportable CMake target")
    endif()
    if(TARGET opencv_imgproc)
        set(PHOTOSPIDER_OPENCV_IMGPROC_TARGET opencv_imgproc)
    elseif(TARGET OpenCV::opencv_imgproc)
        set(PHOTOSPIDER_OPENCV_IMGPROC_TARGET OpenCV::opencv_imgproc)
    elseif(TARGET OpenCV::Imgproc)
        set(PHOTOSPIDER_OPENCV_IMGPROC_TARGET OpenCV::Imgproc)
    else()
        message(FATAL_ERROR
            "OpenCV imgproc component did not provide an exportable CMake target")
    endif()
    if(TARGET opencv_imgcodecs)
        set(PHOTOSPIDER_OPENCV_IMGCODECS_TARGET opencv_imgcodecs)
    elseif(TARGET OpenCV::opencv_imgcodecs)
        set(PHOTOSPIDER_OPENCV_IMGCODECS_TARGET OpenCV::opencv_imgcodecs)
    elseif(TARGET OpenCV::Imgcodecs)
        set(PHOTOSPIDER_OPENCV_IMGCODECS_TARGET OpenCV::Imgcodecs)
    else()
        message(FATAL_ERROR
            "OpenCV imgcodecs component did not provide an exportable CMake target")
    endif()

    # Optional public OpenCV adapter. Consumers requesting only neutral
    # components do not inherit or discover OpenCV.
    add_library(photospider_operation_opencv STATIC
        src/lib/adapters/opencv/buffer_adapter_opencv.cpp)
    add_library(Photospider::operation_opencv ALIAS
        photospider_operation_opencv)
    set_target_properties(photospider_operation_opencv PROPERTIES
        EXPORT_NAME operation_opencv)
    target_compile_features(photospider_operation_opencv PUBLIC cxx_std_17)
    target_include_directories(photospider_operation_opencv PRIVATE
        ${PHOTOSPIDER_PRIVATE_INCLUDE_DIRS})
    target_link_libraries(photospider_operation_opencv PUBLIC
        Photospider::operation_sdk
        Photospider::operation_runtime
        ${PHOTOSPIDER_OPENCV_CORE_TARGET})
endif()

# --- FTXUI (Frontend UI Lib) ---
set(FTXUI_TARGETS "")
if(PHOTOSPIDER_BUILD_GRAPH_CLI)
    set(FTXUI_INCLUDE_DIR "${PROJECT_SOURCE_DIR}/extern/ftxui/include")
    set(FTXUI_SRC_DIR "${PROJECT_SOURCE_DIR}/extern/ftxui/src")

    if(EXISTS "${FTXUI_SRC_DIR}")
        file(GLOB_RECURSE FTXUI_SCREEN_SOURCES
            "${FTXUI_SRC_DIR}/ftxui/screen/*.cpp")
        file(GLOB_RECURSE FTXUI_DOM_SOURCES
            "${FTXUI_SRC_DIR}/ftxui/dom/*.cpp")
        file(GLOB_RECURSE FTXUI_COMPONENT_SOURCES
            "${FTXUI_SRC_DIR}/ftxui/component/*.cpp")

        foreach(src_list FTXUI_SCREEN_SOURCES FTXUI_DOM_SOURCES
                FTXUI_COMPONENT_SOURCES)
            list(FILTER ${src_list} EXCLUDE REGEX ".*_test\\.cpp$")
            list(FILTER ${src_list} EXCLUDE REGEX ".*_fuzzer\\.cpp$")
            list(FILTER ${src_list} EXCLUDE REGEX ".*_benchmark\\.cpp$")
        endforeach()

        add_library(ftxui-screen STATIC ${FTXUI_SCREEN_SOURCES})
        target_include_directories(ftxui-screen PUBLIC ${FTXUI_INCLUDE_DIR}
            PRIVATE ${FTXUI_SRC_DIR})

        add_library(ftxui-dom STATIC ${FTXUI_DOM_SOURCES})
        target_include_directories(ftxui-dom PUBLIC ${FTXUI_INCLUDE_DIR}
            PRIVATE ${FTXUI_SRC_DIR})
        target_link_libraries(ftxui-dom PUBLIC ftxui-screen)

        add_library(ftxui-component STATIC ${FTXUI_COMPONENT_SOURCES})
        target_include_directories(ftxui-component PUBLIC ${FTXUI_INCLUDE_DIR}
            PRIVATE ${FTXUI_SRC_DIR})
        target_link_libraries(ftxui-component PUBLIC ftxui-dom)

        set(FTXUI_TARGETS ftxui-component)
    else()
        find_package(ftxui REQUIRED)
        set(FTXUI_TARGETS ftxui::component)
    endif()
endif()

# ==============================================================================
# 1. Internal Static Modules (The Building Blocks)
# ==============================================================================

# Module: dependency-neutral private core values, display formatting, and the
# build-selected image-processing/image-artifact implementations.
set(PHOTOSPIDER_CORE_INTERNAL_SOURCES
    src/lib/core/ps_types.cpp
    src/lib/core/parameter_value_text.cpp
)

set(PHOTOSPIDER_IMAGE_ARTIFACT_CODEC_SOURCES
    src/lib/providers/configured_image_artifact_codec.cpp
)
set(PHOTOSPIDER_IMAGE_PROCESSING_SOURCES "")
if(PHOTOSPIDER_ENABLE_OPENCV)
    list(APPEND PHOTOSPIDER_IMAGE_ARTIFACT_CODEC_SOURCES
        src/lib/adapters/opencv/image_artifact_codec_opencv.cpp)
    list(APPEND PHOTOSPIDER_IMAGE_PROCESSING_SOURCES
        src/lib/adapters/opencv/image_buffer_processing_opencv.cpp)
else()
    list(APPEND PHOTOSPIDER_IMAGE_PROCESSING_SOURCES
        src/lib/core/image_buffer_processing.cpp)
endif()

add_library(photospider_core_internal STATIC
    ${PHOTOSPIDER_CORE_INTERNAL_SOURCES}
    ${PHOTOSPIDER_IMAGE_ARTIFACT_CODEC_SOURCES}
    ${PHOTOSPIDER_IMAGE_PROCESSING_SOURCES})
target_include_directories(photospider_core_internal PRIVATE
    ${PHOTOSPIDER_PRIVATE_INCLUDE_DIRS})
target_link_libraries(photospider_core_internal PRIVATE
    Photospider::operation_runtime)
if(PHOTOSPIDER_ENABLE_OPENCV)
    target_compile_definitions(photospider_core_internal PRIVATE
        PHOTOSPIDER_HAS_OPENCV)
    target_include_directories(photospider_core_internal PRIVATE
        ${OpenCV_INCLUDE_DIRS})
    target_link_libraries(photospider_core_internal PRIVATE
        Photospider::operation_opencv
        ${PHOTOSPIDER_OPENCV_IMGPROC_TARGET}
        ${PHOTOSPIDER_OPENCV_IMGCODECS_TARGET})
endif()

# Module: private graph model, registry behavior, dependency-neutral cache
# coordination, and format-neutral graph services.
set(PHOTOSPIDER_GRAPH_INTERNAL_SOURCES
    src/lib/core/ops.cpp
    src/lib/graph/in_memory_graph_document_adapter.cpp
    src/lib/graph/graph_model.cpp
    src/lib/graph/graph_state_executor.cpp
    src/lib/graph/graph_extent_resolver.cpp
    src/lib/graph/graph_traversal_service.cpp
    src/lib/graph/roi_propagation_service.cpp
    src/lib/graph/graph_io_service.cpp
    src/lib/graph/graph_cache_service.cpp
    src/lib/graph/graph_inspect_service.cpp
)

add_library(photospider_graph_internal STATIC
    ${PHOTOSPIDER_GRAPH_INTERNAL_SOURCES})
target_include_directories(photospider_graph_internal PRIVATE
    ${PHOTOSPIDER_PRIVATE_INCLUDE_DIRS})
target_link_libraries(photospider_graph_internal PRIVATE
    photospider_core_internal
    Photospider::operation_sdk)

# Module: configured YAML value translation, graph-document IO, and cache
# metadata persistence. GraphIO, cache, and Kernel see only neutral contracts;
# the embedded Host root selects these adapters for the current product profile.
set(PHOTOSPIDER_YAML_ADAPTER_SOURCES
    src/lib/adapters/yaml/parameter_value_yaml.cpp
    src/lib/adapters/yaml/graph_definition_yaml.cpp
    src/lib/adapters/yaml/yaml_graph_document_adapter.cpp
    src/lib/adapters/yaml/yaml_cache_metadata_codec.cpp
)

if(PHOTOSPIDER_ENABLE_YAML)
    add_library(photospider_yaml_adapter_internal STATIC
        ${PHOTOSPIDER_YAML_ADAPTER_SOURCES})
    target_include_directories(
        photospider_yaml_adapter_internal PRIVATE
        ${PHOTOSPIDER_PRIVATE_INCLUDE_DIRS})
    target_link_libraries(
        photospider_yaml_adapter_internal PRIVATE
        photospider_core_internal
        yaml-cpp::yaml-cpp)
endif()

set(PHOTOSPIDER_CONFIGURED_PERSISTENCE_SOURCES
    src/lib/providers/configured_persistence_adapters.cpp)

# Module: optional repository-owned OpenCV CPU operation provider. The graph
# registry remains provider-neutral; only this target owns algorithm callbacks,
# OpenCV process initialization, and OpenCV exception translation.
set(PHOTOSPIDER_OPENCV_OPERATION_PROVIDER_SOURCES "")
if(PHOTOSPIDER_BUILD_OPENCV_OPERATION_PROVIDER)
    list(APPEND PHOTOSPIDER_OPENCV_OPERATION_PROVIDER_SOURCES
        src/lib/providers/opencv/opencv_operation_provider.cpp)
    add_library(photospider_opencv_operation_provider_internal STATIC
        ${PHOTOSPIDER_OPENCV_OPERATION_PROVIDER_SOURCES})
    target_include_directories(
        photospider_opencv_operation_provider_internal PRIVATE
        ${PHOTOSPIDER_PRIVATE_INCLUDE_DIRS}
        ${OpenCV_INCLUDE_DIRS})
    target_link_libraries(
        photospider_opencv_operation_provider_internal PRIVATE
        photospider_graph_internal
        Photospider::operation_opencv
        ${PHOTOSPIDER_OPENCV_IMGPROC_TARGET}
        ${PHOTOSPIDER_OPENCV_IMGCODECS_TARGET})
    if(PHOTOSPIDER_BUILD_FULL_TEST_SUITE)
        target_compile_definitions(
            photospider_opencv_operation_provider_internal PRIVATE
            PHOTOSPIDER_INTERNAL_OPENCV_PROVIDER_TESTING)
    endif()
endif()

# Module: host-side operation plugin loading and lifetime ownership.
set(PHOTOSPIDER_PLUGIN_HOST_INTERNAL_SOURCES
    src/lib/plugin/plugin_manager.cpp
    src/lib/plugin/plugin_loader.cpp
    src/lib/plugin/operation_host_adapter.cpp
    src/lib/providers/configured_operation_providers.cpp
)

add_library(photospider_plugin_host_internal STATIC
    ${PHOTOSPIDER_PLUGIN_HOST_INTERNAL_SOURCES})
target_include_directories(photospider_plugin_host_internal PRIVATE
    ${PHOTOSPIDER_PRIVATE_INCLUDE_DIRS})
target_link_libraries(photospider_plugin_host_internal PRIVATE
    photospider_core_internal
    photospider_graph_internal
    Photospider::operation_sdk
    ${CMAKE_DL_LIBS})
if(PHOTOSPIDER_BUILD_OPENCV_OPERATION_PROVIDER)
    target_compile_definitions(photospider_plugin_host_internal PRIVATE
        PHOTOSPIDER_HAS_OPENCV_OPERATION_PROVIDER)
    target_link_libraries(photospider_plugin_host_internal PRIVATE
        photospider_opencv_operation_provider_internal)
endif()

# Module: built-in schedulers, scheduler factory, and scheduler DSO loader.
set(PHOTOSPIDER_SCHEDULER_INTERNAL_SOURCES
    src/lib/scheduler/cpu_work_stealing_scheduler.cpp
    src/lib/scheduler/gpu_pipeline_scheduler.cpp
    src/lib/scheduler/serial_debug_scheduler.cpp
    src/lib/scheduler/scheduler_factory.cpp
    src/lib/scheduler/scheduler_plugin_loader.cpp
    src/lib/scheduler/scheduler_reservation_owner.cpp
    src/lib/scheduler/scheduler_worker_budget.cpp
)

add_library(photospider_scheduler_internal STATIC
    ${PHOTOSPIDER_SCHEDULER_INTERNAL_SOURCES})
target_include_directories(photospider_scheduler_internal PRIVATE
    ${PHOTOSPIDER_PRIVATE_INCLUDE_DIRS})
target_link_libraries(photospider_scheduler_internal PRIVATE
    Photospider::scheduler_sdk
    Threads::Threads
    ${CMAKE_DL_LIBS})

# Module: compute, dirty execution, GraphRuntime, and interaction ownership.
set(KERNEL_FACADE_SOURCES
    src/lib/host/embedded_host.cpp
    src/lib/runtime/kernel.cpp
    src/lib/runtime/kernel_compute.cpp
    src/lib/runtime/kernel_dirty_roi_facade.cpp
    src/lib/runtime/kernel_inspection_facade.cpp
    src/lib/runtime/kernel_io_cache_facade.cpp
    src/lib/runtime/kernel_scheduler_facade.cpp
)

set(PHOTOSPIDER_COMPUTE_INTERNAL_SOURCES
    src/lib/compute/compute_cache_policy.cpp
    src/lib/compute/compute_dispatch_plan_builder.cpp
    src/lib/compute/compute_geometry.cpp
    src/lib/compute/compute_metrics_recorder.cpp
    src/lib/compute/compute_node_task_runner.cpp
    src/lib/compute/compute_result_committer.cpp
    src/lib/compute/compute_run.cpp
    src/lib/compute/compute_task_dependency_state.cpp
    src/lib/compute/compute_task_submission.cpp
    src/lib/compute/execution_service.cpp
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
)

if(APPLE)
    enable_language(OBJCXX)
else()
    set_source_files_properties(src/lib/runtime/graph_runtime.mm PROPERTIES LANGUAGE CXX COMPILE_FLAGS "-x c++")
endif()

add_library(photospider_compute_internal STATIC
    ${PHOTOSPIDER_COMPUTE_INTERNAL_SOURCES})
target_include_directories(photospider_compute_internal PRIVATE
    ${PHOTOSPIDER_PRIVATE_INCLUDE_DIRS})
target_link_libraries(photospider_compute_internal PRIVATE
    photospider_core_internal
    photospider_graph_internal
    photospider_plugin_host_internal
    photospider_scheduler_internal
    Photospider::operation_sdk)

# Module: private Kernel/Interaction facades and the embedded Host composition
# root. The installed product compiles the same source set into its standalone
# archive; this module keeps the kernel aggregate independently buildable.
add_library(photospider_host_internal STATIC
    ${KERNEL_FACADE_SOURCES}
    ${PHOTOSPIDER_CONFIGURED_PERSISTENCE_SOURCES})
target_include_directories(photospider_host_internal PRIVATE
    ${PHOTOSPIDER_PRIVATE_INCLUDE_DIRS})
target_link_libraries(photospider_host_internal PRIVATE
    photospider_core_internal
    photospider_graph_internal
    photospider_plugin_host_internal
    photospider_scheduler_internal
    photospider_compute_internal
    Photospider::operation_sdk)
if(PHOTOSPIDER_ENABLE_YAML)
    target_compile_definitions(photospider_host_internal PRIVATE
        PHOTOSPIDER_HAS_YAML)
    target_link_libraries(photospider_host_internal PRIVATE
        photospider_yaml_adapter_internal)
endif()

# Buildable aggregate for the private kernel modules. This target has no
# placeholder source: invoking it compiles the actual dependency-selected
# module graph while keeping the installed Host product a separate artifact.
add_custom_target(photospider_kernel)
add_dependencies(photospider_kernel
    photospider_core_internal
    photospider_graph_internal
    photospider_plugin_host_internal
    photospider_scheduler_internal
    photospider_compute_internal
    photospider_host_internal)
if(PHOTOSPIDER_ENABLE_YAML)
    add_dependencies(photospider_kernel photospider_yaml_adapter_internal)
endif()
if(PHOTOSPIDER_BUILD_OPENCV_OPERATION_PROVIDER)
    add_dependencies(photospider_kernel
        photospider_opencv_operation_provider_internal)
endif()

# ==============================================================================
# 2. Installable Static Library (The Embedded Frontend Product)
# ==============================================================================

set(PHOTOSPIDER_PRODUCT_SOURCES
    ${PHOTOSPIDER_CORE_INTERNAL_SOURCES}
    ${PHOTOSPIDER_IMAGE_ARTIFACT_CODEC_SOURCES}
    ${PHOTOSPIDER_IMAGE_PROCESSING_SOURCES}
    ${PHOTOSPIDER_GRAPH_INTERNAL_SOURCES}
    ${PHOTOSPIDER_CONFIGURED_PERSISTENCE_SOURCES}
    ${PHOTOSPIDER_PLUGIN_HOST_INTERNAL_SOURCES}
    ${PHOTOSPIDER_OPENCV_OPERATION_PROVIDER_SOURCES}
    ${PHOTOSPIDER_COMPUTE_INTERNAL_SOURCES}
    ${PHOTOSPIDER_SCHEDULER_INTERNAL_SOURCES}
    ${KERNEL_FACADE_SOURCES}
)
if(PHOTOSPIDER_ENABLE_YAML)
    list(APPEND PHOTOSPIDER_PRODUCT_SOURCES
        ${PHOTOSPIDER_YAML_ADAPTER_SOURCES})
endif()

add_library(photospider STATIC ${PHOTOSPIDER_PRODUCT_SOURCES})

target_compile_definitions(photospider PUBLIC PHOTOSPIDER_STATIC)
if(PHOTOSPIDER_BUILD_OPENCV_OPERATION_PROVIDER)
    target_compile_definitions(photospider PRIVATE
        PHOTOSPIDER_HAS_OPENCV_OPERATION_PROVIDER)
endif()
if(PHOTOSPIDER_ENABLE_OPENCV)
    target_compile_definitions(photospider PRIVATE PHOTOSPIDER_HAS_OPENCV)
endif()
if(PHOTOSPIDER_ENABLE_YAML)
    target_compile_definitions(photospider PRIVATE PHOTOSPIDER_HAS_YAML)
endif()
if(PHOTOSPIDER_BUILD_PROVIDER_INDEPENDENT_FOCUSED_TESTS)
    target_compile_definitions(photospider PRIVATE
        PHOTOSPIDER_INTERNAL_GRAPH_STATE_EXECUTOR_TESTING)
endif()
if(PHOTOSPIDER_BUILD_FULL_TEST_SUITE)
    target_compile_definitions(photospider PRIVATE
        PHOTOSPIDER_INTERNAL_BAD_ALLOC_TESTING
        PHOTOSPIDER_INTERNAL_YAML_GRAPH_DOCUMENT_ADAPTER_TESTING
        PHOTOSPIDER_INTERNAL_HOST_OPERATION_TESTING
        PHOTOSPIDER_INTERNAL_OPENCV_PROVIDER_TESTING
        PHOTOSPIDER_INTERNAL_REQUIRED_TARGET_TESTING)
endif()
target_compile_features(photospider PUBLIC cxx_std_17)

target_include_directories(photospider
    PUBLIC
        "$<BUILD_INTERFACE:${PHOTOSPIDER_BUILD_PUBLIC_INCLUDE_DIR}>"
        "$<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>"
    PRIVATE
        ${PHOTOSPIDER_PRIVATE_INCLUDE_DIRS}
)

target_link_libraries(photospider PRIVATE
    Photospider::operation_sdk
    Photospider::operation_runtime
    Photospider::scheduler_sdk
    Threads::Threads
    ${CMAKE_DL_LIBS}
)
if(PHOTOSPIDER_ENABLE_OPENCV)
    target_include_directories(photospider PRIVATE ${OpenCV_INCLUDE_DIRS})
    target_link_libraries(photospider PRIVATE
        Photospider::operation_opencv
        ${OpenCV_LIBS})
endif()
if(PHOTOSPIDER_ENABLE_YAML)
    target_link_libraries(photospider PRIVATE yaml-cpp::yaml-cpp)
endif()

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
        src/lib/ipc/host.cpp
    )
    target_compile_features(photospider_ipc_client_objects PRIVATE cxx_std_17)
    target_include_directories(photospider_ipc_client_objects PRIVATE
        "${PROJECT_SOURCE_DIR}/include"
        "${PROJECT_SOURCE_DIR}/src/lib"
    )
    target_link_libraries(photospider_ipc_client_objects PRIVATE
        nlohmann_json::nlohmann_json
        Threads::Threads)

    add_library(photospider_ipc_client STATIC
        $<TARGET_OBJECTS:photospider_ipc_client_objects>)
    target_compile_definitions(photospider_ipc_client PUBLIC PHOTOSPIDER_STATIC)
    target_compile_features(photospider_ipc_client PUBLIC cxx_std_17)
    target_include_directories(photospider_ipc_client
        PUBLIC
            "$<BUILD_INTERFACE:${PHOTOSPIDER_BUILD_PUBLIC_INCLUDE_DIR}>"
            "$<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>"
    )
    target_link_libraries(photospider_ipc_client PUBLIC Threads::Threads)

    add_library(photospider_ipc_server_internal STATIC
        src/lib/ipc/collection_snapshot_registry.cpp
        src/lib/ipc/compute_request_registry.cpp
        src/lib/ipc/output_store.cpp
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
    EXPORT PhotospiderEmbeddedTargets
    ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
    LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
    RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
    INCLUDES DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}
)

install(TARGETS
    photospider_operation_sdk
    photospider_operation_runtime
    photospider_scheduler_sdk
    EXPORT PhotospiderTargets
    ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
    LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
    RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
    INCLUDES DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}
)

if(PHOTOSPIDER_ENABLE_OPENCV)
    install(TARGETS photospider_operation_opencv
        EXPORT PhotospiderOpenCVTargets
        ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
        LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
        RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
        INCLUDES DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}
    )
endif()

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

foreach(PUBLIC_HEADER_RELATIVE_PATH
        IN LISTS PHOTOSPIDER_INSTALLABLE_PUBLIC_HEADER_RELATIVE_PATHS)
    get_filename_component(PUBLIC_HEADER_RELATIVE_DIRECTORY
        "${PUBLIC_HEADER_RELATIVE_PATH}" DIRECTORY)
    install(FILES
        "${PHOTOSPIDER_SOURCE_PUBLIC_INCLUDE_DIR}/${PUBLIC_HEADER_RELATIVE_PATH}"
        DESTINATION
        "${CMAKE_INSTALL_INCLUDEDIR}/photospider/${PUBLIC_HEADER_RELATIVE_DIRECTORY}")
endforeach()

install(EXPORT PhotospiderTargets
    NAMESPACE Photospider::
    DESTINATION ${PHOTOSPIDER_INSTALL_CMAKEDIR}
)

if(PHOTOSPIDER_ENABLE_OPENCV)
    install(EXPORT PhotospiderOpenCVTargets
        NAMESPACE Photospider::
        DESTINATION ${PHOTOSPIDER_INSTALL_CMAKEDIR}
    )
endif()

install(EXPORT PhotospiderEmbeddedTargets
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

if(PHOTOSPIDER_BUILD_GRAPH_CLI)
    file(GLOB_RECURSE CLI_LOGIC_SOURCES CONFIGURE_DEPENDS
        "apps/graph_cli/src/*.cpp")
    set(PHOTOSPIDER_CLI_BENCHMARK_SOURCES
        src/lib/benchmark/benchmark_service.cpp
        src/lib/benchmark/benchmark_yaml_generator.cpp)

    add_library(photospider_cli_common STATIC
        ${CLI_LOGIC_SOURCES}
        ${PHOTOSPIDER_CLI_BENCHMARK_SOURCES})

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
endif()

# ==============================================================================
# 4. Main Executable
# ==============================================================================

if(PHOTOSPIDER_BUILD_GRAPH_CLI)
    add_executable(graph_cli apps/graph_cli/main.cpp)
    target_include_directories(graph_cli PRIVATE
        "${PROJECT_SOURCE_DIR}/apps/graph_cli/include"
        "${PROJECT_SOURCE_DIR}/include"
        "${PROJECT_SOURCE_DIR}/src/lib"
        ${OpenCV_INCLUDE_DIRS}
    )
    target_link_libraries(graph_cli PRIVATE photospider_cli_common)

    if(UNIX AND NOT APPLE)
        target_link_libraries(graph_cli PRIVATE dl pthread)
    endif()

    set_target_properties(graph_cli PROPERTIES
        BUILD_RPATH "${CMAKE_BINARY_DIR}/lib"
        INSTALL_RPATH "$ORIGIN/../lib"
    )
endif()

# ==============================================================================
# 5. Standard Operation Plugins
# ==============================================================================
# Build everything in plugins/ops/ except the special Metal loader.

file(MAKE_DIRECTORY ${PLUGIN_OUTPUT_DIR})
set(OP_PLUGIN_TARGETS "")
if(PHOTOSPIDER_BUILD_OPENCV_OPERATION_PLUGINS)
    file(GLOB PLUGIN_SOURCES "plugins/ops/*.cpp")
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

        target_link_libraries(${PLUGIN_NAME} PRIVATE
            Photospider::operation_opencv)
        if(PLUGIN_NAME STREQUAL "threshold_op")
            target_link_libraries(${PLUGIN_NAME} PRIVATE
                ${PHOTOSPIDER_OPENCV_IMGPROC_TARGET})
        elseif(PLUGIN_NAME STREQUAL "save_op")
            target_link_libraries(${PLUGIN_NAME} PRIVATE
                ${PHOTOSPIDER_OPENCV_IMGCODECS_TARGET})
        endif()
    endforeach()
endif()

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
set(SCHEDULER_PLUGIN_TARGETS "")

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
    target_compile_definitions(${SCHEDULER_NAME} PRIVATE
        PHOTOSPIDER_SCHEDULER_PLUGIN_BUILD)
    
    set_target_properties(${SCHEDULER_NAME} PROPERTIES
        LIBRARY_OUTPUT_DIRECTORY "${SCHEDULER_PLUGIN_DIR}"
        RUNTIME_OUTPUT_DIRECTORY "${SCHEDULER_PLUGIN_DIR}"
    )
    
    if(APPLE)
        set_target_properties(${SCHEDULER_NAME} PROPERTIES
            MACOSX_RPATH ON
        )
    endif()
    
    target_include_directories(${SCHEDULER_NAME} PRIVATE
        "${PROJECT_SOURCE_DIR}/plugins")
    target_link_libraries(${SCHEDULER_NAME} PRIVATE
        Photospider::scheduler_sdk
        Threads::Threads)
endforeach()

if(BUILD_TESTING)
    set(TEST_SCHEDULER_PLUGIN_DIR ${CMAKE_BINARY_DIR}/test_schedulers)
    set(TEST_OP_PLUGIN_DIR ${CMAKE_BINARY_DIR}/test_plugins)
    file(MAKE_DIRECTORY ${TEST_SCHEDULER_PLUGIN_DIR})
    file(MAKE_DIRECTORY ${TEST_OP_PLUGIN_DIR})

    set(TEST_INCOMPATIBLE_SCHEDULER_PLUGIN_DIR
        ${CMAKE_BINARY_DIR}/test_scheduler_incompatible)
    file(MAKE_DIRECTORY ${TEST_INCOMPATIBLE_SCHEDULER_PLUGIN_DIR})

    function(add_scheduler_fixture TARGET_NAME OUTPUT_DIRECTORY)
        add_library(${TARGET_NAME} SHARED
            tests/fixtures/scheduler_plugins/destroy_count_scheduler_plugin.cpp)
        target_compile_definitions(${TARGET_NAME} PRIVATE
            PHOTOSPIDER_SCHEDULER_PLUGIN_BUILD
            ${ARGN})
        set_target_properties(${TARGET_NAME} PROPERTIES
            LIBRARY_OUTPUT_DIRECTORY "${OUTPUT_DIRECTORY}"
            RUNTIME_OUTPUT_DIRECTORY "${OUTPUT_DIRECTORY}")
        if(APPLE)
            set_target_properties(${TARGET_NAME} PROPERTIES MACOSX_RPATH ON)
        endif()
        target_link_libraries(${TARGET_NAME} PRIVATE
            Photospider::scheduler_sdk)
    endfunction()

    add_scheduler_fixture(destroy_count_scheduler_plugin
        "${TEST_SCHEDULER_PLUGIN_DIR}")
    add_scheduler_fixture(missing_handshake_scheduler_plugin
        "${TEST_INCOMPATIBLE_SCHEDULER_PLUGIN_DIR}/missing"
        PHOTOSPIDER_SCHEDULER_FIXTURE_OMIT_HANDSHAKE)
    add_scheduler_fixture(mismatched_abi_scheduler_plugin
        "${TEST_INCOMPATIBLE_SCHEDULER_PLUGIN_DIR}/mismatch"
        PHOTOSPIDER_SCHEDULER_FIXTURE_ABI_VERSION=1U)

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
    target_link_libraries(lifecycle_op_plugin PRIVATE
        Photospider::operation_sdk)

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
    target_link_libraries(override_lifecycle_op_plugin PRIVATE
        Photospider::operation_sdk)

    add_library(resize_replacement_op_plugin SHARED
        tests/fixtures/ops/resize_replacement_op_plugin.cpp
    )
    target_compile_definitions(resize_replacement_op_plugin PRIVATE
        PHOTOSPIDER_PLUGIN_BUILD)
    set_target_properties(resize_replacement_op_plugin PROPERTIES
        LIBRARY_OUTPUT_DIRECTORY
            "${TEST_OP_PLUGIN_DIR}/resize_replacement"
        RUNTIME_OUTPUT_DIRECTORY
            "${TEST_OP_PLUGIN_DIR}/resize_replacement"
        BUILD_RPATH "${CMAKE_BINARY_DIR}/lib"
        INSTALL_RPATH "$ORIGIN/../../lib"
    )
    if(APPLE)
        set_target_properties(resize_replacement_op_plugin PROPERTIES
            MACOSX_RPATH ON
            INSTALL_RPATH "@loader_path/../../lib"
        )
    endif()
    target_link_libraries(resize_replacement_op_plugin PRIVATE
        Photospider::operation_sdk)

    add_library(v1_only_op_plugin SHARED
        tests/fixtures/ops/v1_only_op_plugin.cpp)
    set_target_properties(v1_only_op_plugin PROPERTIES
        LIBRARY_OUTPUT_DIRECTORY "${TEST_OP_PLUGIN_DIR}/v1_only"
        RUNTIME_OUTPUT_DIRECTORY "${TEST_OP_PLUGIN_DIR}/v1_only")
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
    if(NOT METAL_FRAMEWORK OR NOT FOUNDATION_FRAMEWORK)
        message(FATAL_ERROR
            "Photospider requires Metal and Foundation on Apple platforms")
    endif()

    # Core product needs Metal framework for Objective-C++ runtime sources.
    target_link_libraries(photospider PRIVATE
        "-framework Metal"
        "-framework Foundation"
    )

    if(PHOTOSPIDER_BUILD_OPENCV_OPERATION_PLUGINS)
        find_library(COREIMAGE_FRAMEWORK NAMES CoreImage)
        find_library(COREVIDEO_FRAMEWORK NAMES CoreVideo)
        if(NOT COREIMAGE_FRAMEWORK OR NOT COREVIDEO_FRAMEWORK)
            message(FATAL_ERROR
                "Photospider Metal operation plugins require CoreImage and CoreVideo")
        endif()

        # 1. The Implementation Library (the actual op logic)
        add_library(perlin_noise_metal SHARED
            plugins/ops/metal/perlin_noise_metal.mm)
        set_target_properties(perlin_noise_metal PROPERTIES
            LIBRARY_OUTPUT_DIRECTORY "${METAL_OPS_DIR}"
            MACOSX_RPATH ON
        )
        target_link_libraries(perlin_noise_metal PRIVATE
            Photospider::operation_opencv
            ${METAL_FRAMEWORK}
            ${FOUNDATION_FRAMEWORK}
            ${COREIMAGE_FRAMEWORK}
            ${COREVIDEO_FRAMEWORK}
        )
        target_include_directories(perlin_noise_metal PRIVATE
            "${PROJECT_SOURCE_DIR}/plugins/ops"
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
            "${PROJECT_SOURCE_DIR}/plugins/ops"
        )

        # The loader directly calls implementation symbols and therefore owns
        # the implementation DSO dependency.
        target_link_libraries(metal_ops_loader PRIVATE
            perlin_noise_metal
            Photospider::operation_sdk
        )
    endif()

endif()

# ==============================================================================
# 8. Tests
# ==============================================================================
set(PHOTOSPIDER_BUILD_SMOKE_LABEL "build-smoke")
if(BUILD_TESTING)
    enable_testing()
    find_package(Python3 3.9 REQUIRED COMPONENTS Interpreter)
    add_test(NAME DependencyDisabledInstallSmoke
        COMMAND ${Python3_EXECUTABLE} -B
            "${PROJECT_SOURCE_DIR}/tests/integration/dependency_disabled_install_smoke.py"
            --repo "${PROJECT_SOURCE_DIR}"
            --work
                "${CMAKE_BINARY_DIR}/tests/dependency_disabled_install_smoke"
            --cmake-executable "${CMAKE_COMMAND}"
            --config $<CONFIG>
        WORKING_DIRECTORY ${PROJECT_SOURCE_DIR})
    set_tests_properties(DependencyDisabledInstallSmoke PROPERTIES
        LABELS "${PHOTOSPIDER_BUILD_SMOKE_LABEL}"
        RUN_SERIAL TRUE
        TIMEOUT 900)
endif()

if(PHOTOSPIDER_BUILD_PROVIDER_INDEPENDENT_FOCUSED_TESTS)
    set(TEST_OUTPUT_DIR ${CMAKE_BINARY_DIR}/tests)
    file(MAKE_DIRECTORY ${TEST_OUTPUT_DIR})

    include(GoogleTest)
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

    # These targets exercise provider-independent product seams. Keep them
    # buildable when the repository OpenCV operation provider is disabled
    # without registering the provider-dependent broad suite.
    add_ps_test(test_kernel_contracts
        tests/integration/test_kernel_contracts.cpp)
    target_compile_definitions(test_kernel_contracts PRIVATE
        PHOTOSPIDER_INTERNAL_GRAPH_STATE_EXECUTOR_TESTING)
    if(PHOTOSPIDER_BUILD_FULL_TEST_SUITE)
        target_compile_definitions(test_kernel_contracts PRIVATE
            PHOTOSPIDER_INTERNAL_YAML_GRAPH_DOCUMENT_ADAPTER_TESTING)
    endif()

    add_ps_test(test_optional_opencv_operation_provider
        tests/integration/test_optional_opencv_operation_provider.cpp)
    target_link_libraries(test_optional_opencv_operation_provider PRIVATE
        ${CMAKE_DL_LIBS})
    target_compile_definitions(
        test_optional_opencv_operation_provider PRIVATE
        PS_RESIZE_REPLACEMENT_PLUGIN_PATH="$<TARGET_FILE:resize_replacement_op_plugin>")
    if(PHOTOSPIDER_BUILD_OPENCV_OPERATION_PROVIDER)
        target_compile_definitions(
            test_optional_opencv_operation_provider PRIVATE
            PS_EXPECT_OPENCV_OPERATION_PROVIDER=1)
    else()
        target_compile_definitions(
            test_optional_opencv_operation_provider PRIVATE
            PS_EXPECT_OPENCV_OPERATION_PROVIDER=0)
    endif()
    add_dependencies(test_optional_opencv_operation_provider
        resize_replacement_op_plugin)
    gtest_discover_tests(test_optional_opencv_operation_provider
        WORKING_DIRECTORY ${PROJECT_SOURCE_DIR}
        PROPERTIES TIMEOUT 30)
endif()

if(PHOTOSPIDER_BUILD_FULL_TEST_SUITE)
    add_ps_test(test_scheduler tests/integration/test_scheduler.cpp)
    target_link_libraries(test_scheduler PRIVATE nlohmann_json::nlohmann_json)

    add_executable(test_scheduler_sdk_example
        tests/unit/test_scheduler_sdk_example.cpp)
    set_target_properties(test_scheduler_sdk_example PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY "${TEST_OUTPUT_DIR}")
    target_compile_definitions(test_scheduler_sdk_example PRIVATE
        PHOTOSPIDER_SCHEDULER_EXAMPLE_TESTING)
    target_include_directories(test_scheduler_sdk_example PRIVATE
        "${PROJECT_SOURCE_DIR}/plugins")
    target_link_libraries(test_scheduler_sdk_example PRIVATE
        Photospider::scheduler_sdk
        Threads::Threads
        GTest::gtest
        GTest::gtest_main)

    add_executable(test_serial_debug_scheduler
        tests/unit/test_serial_debug_scheduler.cpp
        src/lib/scheduler/serial_debug_scheduler.cpp)
    set_target_properties(test_serial_debug_scheduler PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY "${TEST_OUTPUT_DIR}")
    target_compile_definitions(test_serial_debug_scheduler PRIVATE
        PHOTOSPIDER_INTERNAL_SCHEDULER_TESTING)
    target_include_directories(test_serial_debug_scheduler PRIVATE
        "${PROJECT_SOURCE_DIR}/src/lib")
    target_link_libraries(test_serial_debug_scheduler PRIVATE
        Photospider::scheduler_sdk
        Threads::Threads
        GTest::gtest
        GTest::gtest_main)

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
            PS_TEST_SCHEDULER_PLUGIN_PATH="$<TARGET_FILE:destroy_count_scheduler_plugin>"
            PS_TEST_OP_PLUGIN_DIR="$<TARGET_FILE_DIR:lifecycle_op_plugin>")
        target_link_libraries(test_ipc_protocol PRIVATE
            photospider_ipc_server_internal
            photospider_ipc_client
            photospider
            GTest::gtest
            GTest::gtest_main
            nlohmann_json::nlohmann_json
            Threads::Threads
            ${CMAKE_DL_LIBS})
        add_dependencies(test_ipc_protocol destroy_count_scheduler_plugin
            lifecycle_op_plugin)

        add_executable(test_ipc_host tests/unit/test_ipc_host.cpp)
        set_target_properties(test_ipc_host PROPERTIES
            RUNTIME_OUTPUT_DIRECTORY "${TEST_OUTPUT_DIR}"
            BUILD_RPATH "${CMAKE_BINARY_DIR}/lib")
        target_compile_features(test_ipc_host PRIVATE cxx_std_17)
        target_include_directories(test_ipc_host PRIVATE
            "${PROJECT_SOURCE_DIR}/include"
            "${PROJECT_SOURCE_DIR}/src/lib"
            "${PROJECT_SOURCE_DIR}/tests")
        target_link_libraries(test_ipc_host PRIVATE
            photospider_ipc_server_internal
            photospider_ipc_client
            photospider
            GTest::gtest
            GTest::gtest_main
            nlohmann_json::nlohmann_json
            Threads::Threads)

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

        add_executable(test_collection_snapshot_registry
            tests/unit/test_collection_snapshot_registry.cpp)
        set_target_properties(test_collection_snapshot_registry PROPERTIES
            RUNTIME_OUTPUT_DIRECTORY "${TEST_OUTPUT_DIR}"
            BUILD_RPATH "${CMAKE_BINARY_DIR}/lib")
        target_compile_features(test_collection_snapshot_registry PRIVATE cxx_std_17)
        target_include_directories(test_collection_snapshot_registry PRIVATE
            "${PROJECT_SOURCE_DIR}/include"
            "${PROJECT_SOURCE_DIR}/src/lib"
            "${PROJECT_SOURCE_DIR}/tests")
        target_link_libraries(test_collection_snapshot_registry PRIVATE
            photospider_ipc_server_internal
            photospider_ipc_client
            GTest::gtest
            GTest::gtest_main
            Threads::Threads)

        add_executable(test_output_store
            tests/unit/test_output_store.cpp)
        set_target_properties(test_output_store PROPERTIES
            RUNTIME_OUTPUT_DIRECTORY "${TEST_OUTPUT_DIR}"
            BUILD_RPATH "${CMAKE_BINARY_DIR}/lib")
        target_compile_features(test_output_store PRIVATE cxx_std_17)
        target_include_directories(test_output_store PRIVATE
            "${PROJECT_SOURCE_DIR}/include"
            "${PROJECT_SOURCE_DIR}/src/lib"
            "${PROJECT_SOURCE_DIR}/tests")
        target_link_libraries(test_output_store PRIVATE
            photospider_ipc_server_internal
            photospider_ipc_client
            photospider
            GTest::gtest
            GTest::gtest_main
            Threads::Threads)

        # Long-lived integration fixture: a separate process with the real
        # internal Server/router/socket/output stack and a deterministic Host.
        # The helper itself is intentionally not registered with CTest.
        add_executable(ipc_output_fixture_daemon
            tests/support/ipc_output_fixture_daemon.cpp)
        set_target_properties(ipc_output_fixture_daemon PROPERTIES
            RUNTIME_OUTPUT_DIRECTORY "${TEST_OUTPUT_DIR}"
            BUILD_RPATH "${CMAKE_BINARY_DIR}/lib")
        target_compile_features(ipc_output_fixture_daemon PRIVATE cxx_std_17)
        target_include_directories(ipc_output_fixture_daemon PRIVATE
            "${PROJECT_SOURCE_DIR}/include"
            "${PROJECT_SOURCE_DIR}/src/lib"
            "${PROJECT_SOURCE_DIR}/tests")
        target_link_libraries(ipc_output_fixture_daemon PRIVATE
            photospider_ipc_server_internal
            photospider_ipc_client
            photospider
            Threads::Threads)

        add_executable(test_ipc_daemon
            tests/integration/test_ipc_daemon.cpp)
        set_target_properties(test_ipc_daemon PROPERTIES
            RUNTIME_OUTPUT_DIRECTORY "${TEST_OUTPUT_DIR}"
            BUILD_RPATH "${CMAKE_BINARY_DIR}/lib")
        target_compile_features(test_ipc_daemon PRIVATE cxx_std_17)
        target_compile_definitions(test_ipc_daemon PRIVATE
            PS_PHOTOSPIDERD_PATH="$<TARGET_FILE:photospiderd>"
            PS_IPC_OUTPUT_FIXTURE_DAEMON_PATH="$<TARGET_FILE:ipc_output_fixture_daemon>"
            PS_TEST_OP_PLUGIN_DIR="$<TARGET_FILE_DIR:lifecycle_op_plugin>"
            PS_TEST_SCHEDULER_PLUGIN_PATH="$<TARGET_FILE:destroy_count_scheduler_plugin>")
        target_include_directories(test_ipc_daemon PRIVATE
            "${PROJECT_SOURCE_DIR}/include"
            "${PROJECT_SOURCE_DIR}/src/lib"
            "${PROJECT_SOURCE_DIR}/tests")
        target_link_libraries(test_ipc_daemon PRIVATE
            photospider_ipc_server_internal
            photospider_ipc_client
            photospider
            GTest::gtest
            GTest::gtest_main
            Threads::Threads)
        add_dependencies(test_ipc_daemon
            photospiderd ipc_output_fixture_daemon lifecycle_op_plugin
            destroy_count_scheduler_plugin)
    endif()

    add_ps_test(test_milestone2 tests/integration/test_milestone2.cpp)
    add_ps_test(test_milestone3 tests/integration/test_milestone3.cpp)
    add_ps_test(test_milestone34 tests/integration/test_milestone34.cpp)
    add_ps_test(test_propagation tests/integration/test_propagation.cpp)
    add_ps_test(test_propagation_contracts
        tests/unit/test_propagation_contracts.cpp)
    add_ps_test(test_graph_topology_boundaries
        tests/unit/test_graph_topology_boundaries.cpp)
    add_ps_test(test_graph_document_adapter
        tests/unit/test_graph_document_adapter.cpp)
    add_ps_test(test_image_buffer_contracts
        tests/unit/test_image_buffer_contracts.cpp)
    add_executable(test_stdlib_image_buffer_processing
        tests/unit/test_stdlib_image_buffer_processing.cpp
        src/lib/core/image_buffer_processing.cpp)
    set_target_properties(test_stdlib_image_buffer_processing PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY "${TEST_OUTPUT_DIR}")
    target_include_directories(test_stdlib_image_buffer_processing PRIVATE
        "${PROJECT_SOURCE_DIR}/include"
        "${PROJECT_SOURCE_DIR}/src/lib")
    target_link_libraries(test_stdlib_image_buffer_processing PRIVATE
        Photospider::operation_runtime
        GTest::gtest
        GTest::gtest_main)
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
        PHOTOSPIDER_INTERNAL_GRAPH_STATE_EXECUTOR_TESTING
        PHOTOSPIDER_INTERNAL_HOST_OPERATION_TESTING
        PHOTOSPIDER_INTERNAL_REQUIRED_TARGET_TESTING
    )
    add_dependencies(test_host_adapter lifecycle_op_plugin
        override_lifecycle_op_plugin destroy_count_scheduler_plugin)
    add_ps_test(test_graph_document_errors
        tests/integration/test_graph_document_errors.cpp)
    target_compile_definitions(test_graph_document_errors PRIVATE
        PHOTOSPIDER_INTERNAL_YAML_GRAPH_DOCUMENT_ADAPTER_TESTING)
    add_ps_test(test_graph_document_injection
        tests/integration/test_graph_document_injection.cpp)
    add_ps_test(test_scheduler_worker_budget
        tests/integration/test_scheduler_worker_budget.cpp)
    target_compile_definitions(test_scheduler_worker_budget PRIVATE
        PHOTOSPIDER_INTERNAL_BAD_ALLOC_TESTING)
    if(PHOTOSPIDER_BUILD_OPENCV_OPERATION_PROVIDER)
        add_ps_test(test_opencv_operation_concurrency
            tests/integration/test_opencv_operation_concurrency.cpp)
        target_compile_definitions(test_opencv_operation_concurrency PRIVATE
            PHOTOSPIDER_INTERNAL_OPENCV_PROVIDER_TESTING)
        target_link_libraries(test_opencv_operation_concurrency PRIVATE
            photospider_cli_common)
        add_ps_test(test_opencv_operation_provider_exceptions
            tests/integration/test_opencv_operation_provider_exceptions.cpp)
        target_compile_definitions(
            test_opencv_operation_provider_exceptions PRIVATE
            PHOTOSPIDER_INTERNAL_OPENCV_PROVIDER_TESTING)
        add_executable(opencv_operation_concurrency_benchmark
            tests/verification/opencv_operation_concurrency_benchmark.cpp)
        set_target_properties(opencv_operation_concurrency_benchmark PROPERTIES
            RUNTIME_OUTPUT_DIRECTORY "${TEST_OUTPUT_DIR}")
        target_include_directories(
            opencv_operation_concurrency_benchmark PRIVATE
            "${PROJECT_SOURCE_DIR}/include"
            "${PROJECT_SOURCE_DIR}/src/lib"
            ${OpenCV_INCLUDE_DIRS})
        target_compile_definitions(
            opencv_operation_concurrency_benchmark PRIVATE
            PHOTOSPIDER_INTERNAL_OPENCV_PROVIDER_TESTING)
        target_link_libraries(opencv_operation_concurrency_benchmark PRIVATE
            photospider_cli_common
            yaml-cpp::yaml-cpp
            ${OpenCV_LIBS})
    endif()
    add_ps_test(test_scheduler_worker_budget_unit
        tests/unit/test_scheduler_worker_budget.cpp)
    add_ps_test(test_scheduler_factory_plan
        tests/unit/test_scheduler_factory_plan.cpp)
    add_ps_test(test_scheduler_reservation_owner
        tests/unit/test_scheduler_reservation_owner.cpp)
    add_ps_test(test_scheduler_instance_limits
        tests/unit/test_scheduler_instance_limits.cpp)
    target_compile_definitions(test_scheduler_instance_limits PRIVATE
        PHOTOSPIDER_INTERNAL_BAD_ALLOC_TESTING)
    add_ps_test(test_compute_run tests/unit/test_compute_run.cpp)
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
    add_ps_test(test_cli_scheduler_config
        tests/integration/test_cli_scheduler_config.cpp)
    target_include_directories(test_cli_scheduler_config PRIVATE
        "${PROJECT_SOURCE_DIR}/apps/graph_cli/include")
    target_link_libraries(test_cli_scheduler_config PRIVATE
        photospider_cli_common)
    add_ps_test(test_compute_service_split
        tests/integration/test_compute_service_split.cpp)
    add_ps_test(test_stride_aware_compute_paths
        tests/integration/test_stride_aware_compute_paths.cpp)
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
        override_lifecycle_op_plugin v1_only_op_plugin ${OP_PLUGIN_TARGETS})

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
        PS_TEST_SCHEDULER_MISSING_HANDSHAKE_PATH="$<TARGET_FILE:missing_handshake_scheduler_plugin>"
        PS_TEST_SCHEDULER_MISMATCHED_ABI_PATH="$<TARGET_FILE:mismatched_abi_scheduler_plugin>"
    )
    add_dependencies(test_scheduler_plugin_loader
        ${SCHEDULER_PLUGIN_TARGETS}
        destroy_count_scheduler_plugin
        missing_handshake_scheduler_plugin
        mismatched_abi_scheduler_plugin
    )

    set(PHOTOSPIDER_PUBLIC_HEADER_ROOT
        "${PROJECT_SOURCE_DIR}/include/photospider")
    file(MAKE_DIRECTORY
        "${CMAKE_BINARY_DIR}/generated/public_header_self_containment")
    set(PHOTOSPIDER_PUBLIC_HEADER_SELF_CONTAINMENT_SOURCES "")
    set(PHOTOSPIDER_OPENCV_HEADER_SELF_CONTAINMENT_SOURCE "")
    foreach(PUBLIC_HEADER_RELATIVE_PATH
            IN LISTS PHOTOSPIDER_INSTALLABLE_PUBLIC_HEADER_RELATIVE_PATHS)
        set(PUBLIC_HEADER_INCLUDE
            "photospider/${PUBLIC_HEADER_RELATIVE_PATH}")
        string(MAKE_C_IDENTIFIER "${PUBLIC_HEADER_INCLUDE}"
            PUBLIC_HEADER_TARGET_SUFFIX)
        set(PUBLIC_HEADER_SOURCE
            "${CMAKE_BINARY_DIR}/generated/public_header_self_containment/${PUBLIC_HEADER_TARGET_SUFFIX}.cpp")
        file(WRITE "${PUBLIC_HEADER_SOURCE}"
            "/* Generated by CMake to verify public header self-containment. */\n"
            "#include <${PUBLIC_HEADER_INCLUDE}>\n"
        )
        if(PUBLIC_HEADER_RELATIVE_PATH STREQUAL
                "plugin/opencv_adapter.hpp")
            set(PHOTOSPIDER_OPENCV_HEADER_SELF_CONTAINMENT_SOURCE
                "${PUBLIC_HEADER_SOURCE}")
        else()
            list(APPEND PHOTOSPIDER_PUBLIC_HEADER_SELF_CONTAINMENT_SOURCES
                "${PUBLIC_HEADER_SOURCE}")
        endif()
    endforeach()

    # Core, Host, IPC, operation-SDK, and scheduler headers must remain
    # self-contained without inheriting the optional OpenCV component.
    add_library(public_header_self_containment_objects OBJECT
        ${PHOTOSPIDER_PUBLIC_HEADER_SELF_CONTAINMENT_SOURCES}
    )
    target_include_directories(public_header_self_containment_objects
        PRIVATE "${PROJECT_SOURCE_DIR}/include"
    )
    target_compile_features(public_header_self_containment_objects
        PRIVATE cxx_std_17
    )

    # The opt-in adapter is compiled separately with exactly its declared
    # operation_opencv usage requirements.
    if(PHOTOSPIDER_ENABLE_OPENCV)
        add_library(public_opencv_header_self_containment_objects OBJECT
            "${PHOTOSPIDER_OPENCV_HEADER_SELF_CONTAINMENT_SOURCE}"
        )
        target_link_libraries(public_opencv_header_self_containment_objects
            PRIVATE Photospider::operation_opencv
        )
        target_compile_features(public_opencv_header_self_containment_objects
            PRIVATE cxx_std_17
        )
    endif()

    add_custom_target(public_header_self_containment)
    add_dependencies(public_header_self_containment
        public_header_self_containment_objects)
    if(PHOTOSPIDER_ENABLE_OPENCV)
        add_dependencies(public_header_self_containment
            public_opencv_header_self_containment_objects)
    endif()
    # CTest is reserved for long-lived runtime, package-consumer, compile, and
    # error-boundary behavior. Migration/residue scanners and source-quality
    # audits are intentionally absent from the test inventory.
    set(PHOTOSPIDER_INSTALL_CONSUMER_EXPECTED_CTEST_ARGUMENTS
        --expected-smoke DependencyDisabledInstallSmoke
        --expected-smoke IpcDisabledInstallSmoke)
    if(PHOTOSPIDER_BUILD_IPC)
        list(APPEND
            PHOTOSPIDER_INSTALL_CONSUMER_EXPECTED_CTEST_ARGUMENTS
            --expected-smoke StaticProductConsumerSmoke)
    endif()
    add_test(NAME InstallConsumerArchitecturePropagationSafety
        COMMAND ${Python3_EXECUTABLE} -B
            "${PROJECT_SOURCE_DIR}/tests/integration/install_consumer_architecture_propagation_test.py"
            --build-dir "${CMAKE_BINARY_DIR}"
            --ctest-executable "${CMAKE_CTEST_COMMAND}"
            --config "$<CONFIG>"
            --python-executable "${Python3_EXECUTABLE}"
            ${PHOTOSPIDER_INSTALL_CONSUMER_EXPECTED_CTEST_ARGUMENTS}
        WORKING_DIRECTORY
            "${PROJECT_SOURCE_DIR}/tests/integration")
    set_tests_properties(
        InstallConsumerArchitecturePropagationSafety PROPERTIES
        TIMEOUT 30)
    if(PHOTOSPIDER_BUILD_IPC)
        add_test(NAME PhotospiderdCapabilityHelp
            COMMAND ${CMAKE_COMMAND}
                "-DPHOTOSPIDERD=$<TARGET_FILE:photospiderd>"
                -P "${PROJECT_SOURCE_DIR}/tests/integration/photospiderd_capability_help.cmake")
        set_tests_properties(PhotospiderdCapabilityHelp PROPERTIES
            TIMEOUT 10)
        add_test(NAME StaticProductConsumerSmoke
            COMMAND ${Python3_EXECUTABLE} -B
                "${PROJECT_SOURCE_DIR}/tests/integration/static_product_consumer_smoke.py"
                --repo "${PROJECT_SOURCE_DIR}"
                --build "${CMAKE_BINARY_DIR}"
                --work "${CMAKE_BINARY_DIR}/tests/static_product_consumer_smoke"
                --cmake-executable "${CMAKE_COMMAND}"
                --config $<CONFIG>
            WORKING_DIRECTORY ${PROJECT_SOURCE_DIR})
        set_tests_properties(StaticProductConsumerSmoke PROPERTIES
            LABELS "${PHOTOSPIDER_BUILD_SMOKE_LABEL}"
            RUN_SERIAL TRUE
            TIMEOUT 900)
    endif()
    add_test(NAME IpcDisabledInstallSmoke
        COMMAND ${Python3_EXECUTABLE} -B
            "${PROJECT_SOURCE_DIR}/tests/integration/ipc_disabled_install_smoke.py"
            --repo "${PROJECT_SOURCE_DIR}"
            --work "${CMAKE_BINARY_DIR}/tests/ipc_disabled_install_smoke"
            --cmake-executable "${CMAKE_COMMAND}"
            --config $<CONFIG>
        WORKING_DIRECTORY ${PROJECT_SOURCE_DIR})
    set_tests_properties(IpcDisabledInstallSmoke PROPERTIES
        LABELS "${PHOTOSPIDER_BUILD_SMOKE_LABEL}"
        RUN_SERIAL TRUE
        TIMEOUT 600)
    add_test(NAME ImageArtifactCodecDependencyDisabledBuild
        COMMAND ${Python3_EXECUTABLE}
            "${PROJECT_SOURCE_DIR}/tests/integration/image_artifact_codec_dependency_disabled_smoke.py"
            --repo "${PROJECT_SOURCE_DIR}"
            --work
                "${CMAKE_BINARY_DIR}/tests/image_artifact_codec_dependency_disabled"
            --cmake-executable "${CMAKE_COMMAND}"
            --config $<CONFIG>
        WORKING_DIRECTORY ${PROJECT_SOURCE_DIR})
    set_tests_properties(ImageArtifactCodecDependencyDisabledBuild PROPERTIES
        LABELS "${PHOTOSPIDER_BUILD_SMOKE_LABEL}"
        RUN_SERIAL TRUE
        TIMEOUT 900)
    if(PHOTOSPIDER_BUILD_OPENCV_OPERATION_PROVIDER)
        add_test(NAME OpenCvOperationProviderBuildSmokeSafety
            COMMAND ${Python3_EXECUTABLE} -B
                "${PROJECT_SOURCE_DIR}/tests/integration/optional_opencv_operation_provider_build_smoke_test.py"
            WORKING_DIRECTORY
                "${PROJECT_SOURCE_DIR}/tests/integration")
        set_tests_properties(
            OpenCvOperationProviderBuildSmokeSafety PROPERTIES
            TIMEOUT 30)
        add_test(NAME OpenCvOperationProviderDisabledBuild
            COMMAND ${Python3_EXECUTABLE}
                "${PROJECT_SOURCE_DIR}/tests/integration/optional_opencv_operation_provider_build_smoke.py"
                --repo "${PROJECT_SOURCE_DIR}"
                --work
                    "${CMAKE_BINARY_DIR}/tests/optional_opencv_provider_disabled"
                --cmake-executable "${CMAKE_COMMAND}"
                --ctest-executable "${CMAKE_CTEST_COMMAND}"
                --config $<CONFIG>
            WORKING_DIRECTORY ${PROJECT_SOURCE_DIR})
        set_tests_properties(OpenCvOperationProviderDisabledBuild PROPERTIES
            LABELS "${PHOTOSPIDER_BUILD_SMOKE_LABEL}"
            RUN_SERIAL TRUE
            TIMEOUT 900)
    endif()
    add_test(NAME GraphCliOptionBadAlloc
        COMMAND ${Python3_EXECUTABLE}
            "${PROJECT_SOURCE_DIR}/tests/integration/verify_graph_cli_option_bad_alloc.py"
            --repo "${PROJECT_SOURCE_DIR}"
            --binary "$<TARGET_FILE:graph_cli>"
            --out "${CMAKE_BINARY_DIR}/tests/graph_cli_option_bad_alloc"
        WORKING_DIRECTORY ${PROJECT_SOURCE_DIR})
    set_tests_properties(GraphCliOptionBadAlloc PROPERTIES RUN_SERIAL TRUE)
    add_test(NAME GraphCliPluginComputeSmoke
        COMMAND ${Python3_EXECUTABLE}
            "${PROJECT_SOURCE_DIR}/tests/integration/graph_cli_plugin_compute_smoke.py"
            --binary "$<TARGET_FILE:graph_cli>"
            --operation-plugin-dir "$<TARGET_FILE_DIR:lifecycle_op_plugin>"
            --scheduler-plugin "$<TARGET_FILE:destroy_count_scheduler_plugin>"
            --work "${CMAKE_BINARY_DIR}/tests/graph_cli_plugin_compute_smoke"
        WORKING_DIRECTORY ${PROJECT_SOURCE_DIR})
    set_tests_properties(GraphCliPluginComputeSmoke PROPERTIES
        RUN_SERIAL TRUE TIMEOUT 60)
    add_test(NAME PublicHeaderSelfContainment
        COMMAND ${CMAKE_COMMAND} --build "${CMAKE_BINARY_DIR}"
            --target public_header_self_containment --config $<CONFIG>
        WORKING_DIRECTORY ${PROJECT_SOURCE_DIR})
    set_tests_properties(PublicHeaderSelfContainment PROPERTIES
        LABELS "${PHOTOSPIDER_BUILD_SMOKE_LABEL}"
        RUN_SERIAL TRUE
        TIMEOUT 300)

    gtest_discover_tests(test_scheduler WORKING_DIRECTORY ${PROJECT_SOURCE_DIR})
    gtest_discover_tests(test_scheduler_sdk_example
        WORKING_DIRECTORY ${PROJECT_SOURCE_DIR})
    gtest_discover_tests(test_serial_debug_scheduler
        WORKING_DIRECTORY ${PROJECT_SOURCE_DIR})
    if(PHOTOSPIDER_BUILD_IPC)
        gtest_discover_tests(test_ipc_protocol
            WORKING_DIRECTORY ${PROJECT_SOURCE_DIR}
            PROPERTIES TIMEOUT 30)
        gtest_discover_tests(test_ipc_host
            WORKING_DIRECTORY ${PROJECT_SOURCE_DIR}
            PROPERTIES TIMEOUT 60)
        gtest_discover_tests(test_compute_request_registry
            WORKING_DIRECTORY ${PROJECT_SOURCE_DIR}
            PROPERTIES TIMEOUT 30)
        gtest_discover_tests(test_collection_snapshot_registry
            WORKING_DIRECTORY ${PROJECT_SOURCE_DIR}
            PROPERTIES TIMEOUT 30)
        gtest_discover_tests(test_output_store
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
    gtest_discover_tests(test_graph_document_adapter
        WORKING_DIRECTORY ${PROJECT_SOURCE_DIR}
        PROPERTIES TIMEOUT 30)
    gtest_discover_tests(test_kernel_contracts WORKING_DIRECTORY ${PROJECT_SOURCE_DIR})
    gtest_discover_tests(test_image_buffer_contracts WORKING_DIRECTORY ${PROJECT_SOURCE_DIR})
    gtest_discover_tests(test_stdlib_image_buffer_processing
        WORKING_DIRECTORY ${PROJECT_SOURCE_DIR})
    gtest_discover_tests(test_event_stream_boundaries
        WORKING_DIRECTORY ${PROJECT_SOURCE_DIR})
    gtest_discover_tests(test_cli_compute_event_paging
        WORKING_DIRECTORY ${PROJECT_SOURCE_DIR})
    gtest_discover_tests(test_host_adapter WORKING_DIRECTORY ${PROJECT_SOURCE_DIR})
    gtest_discover_tests(test_graph_document_errors
        WORKING_DIRECTORY ${PROJECT_SOURCE_DIR}
        PROPERTIES TIMEOUT 30)
    gtest_discover_tests(test_graph_document_injection
        WORKING_DIRECTORY ${PROJECT_SOURCE_DIR}
        PROPERTIES TIMEOUT 30)
    gtest_discover_tests(test_scheduler_worker_budget
        WORKING_DIRECTORY ${PROJECT_SOURCE_DIR}
        PROPERTIES TIMEOUT 30)
    if(PHOTOSPIDER_BUILD_OPENCV_OPERATION_PROVIDER)
        gtest_discover_tests(test_opencv_operation_provider_exceptions
            WORKING_DIRECTORY ${PROJECT_SOURCE_DIR}
            PROPERTIES TIMEOUT 30)
        gtest_discover_tests(test_opencv_operation_concurrency
            WORKING_DIRECTORY ${PROJECT_SOURCE_DIR}
            PROPERTIES TIMEOUT 60)
    endif()
    gtest_discover_tests(test_scheduler_worker_budget_unit
        WORKING_DIRECTORY ${PROJECT_SOURCE_DIR}
        PROPERTIES TIMEOUT 30)
    gtest_discover_tests(test_scheduler_factory_plan
        WORKING_DIRECTORY ${PROJECT_SOURCE_DIR}
        PROPERTIES TIMEOUT 30)
    gtest_discover_tests(test_scheduler_reservation_owner
        WORKING_DIRECTORY ${PROJECT_SOURCE_DIR}
        PROPERTIES TIMEOUT 30)
    gtest_discover_tests(test_scheduler_instance_limits
        WORKING_DIRECTORY ${PROJECT_SOURCE_DIR}
        PROPERTIES TIMEOUT 30)
    gtest_discover_tests(test_compute_run
        WORKING_DIRECTORY ${PROJECT_SOURCE_DIR}
        PROPERTIES TIMEOUT 30)
    gtest_discover_tests(test_bad_alloc_boundaries
        WORKING_DIRECTORY ${PROJECT_SOURCE_DIR})
    gtest_discover_tests(test_cli_dirty_snapshot_formatter WORKING_DIRECTORY ${PROJECT_SOURCE_DIR})
    gtest_discover_tests(test_cli_scheduler_config
        WORKING_DIRECTORY ${PROJECT_SOURCE_DIR})
    gtest_discover_tests(test_compute_service_split
        WORKING_DIRECTORY ${PROJECT_SOURCE_DIR}
        PROPERTIES TIMEOUT 60)
    gtest_discover_tests(test_stride_aware_compute_paths
        WORKING_DIRECTORY ${PROJECT_SOURCE_DIR}
        PROPERTIES TIMEOUT 60)
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
message(STATUS "  OpenCV Capability : ${PHOTOSPIDER_ENABLE_OPENCV}")
message(STATUS "  YAML Capability   : ${PHOTOSPIDER_ENABLE_YAML}")
message(STATUS "  Graph CLI         : ${PHOTOSPIDER_BUILD_GRAPH_CLI}")
message(STATUS "  OpenCV Op Plugins : ${PHOTOSPIDER_BUILD_OPENCV_OPERATION_PLUGINS}")
message(STATUS "  Output Bin        : ${CMAKE_RUNTIME_OUTPUT_DIRECTORY}")
message(STATUS "  Output Lib        : ${CMAKE_LIBRARY_OUTPUT_DIRECTORY}")
message(STATUS "  Output Plugins    : ${PLUGIN_OUTPUT_DIR}")
message(STATUS "  Scheduler Plugins : ${SCHEDULER_PLUGIN_DIR}")
message(STATUS "  OpenCV Op Provider: ${PHOTOSPIDER_BUILD_OPENCV_OPERATION_PROVIDER}")
message(STATUS "  Focused Tests     : ${PHOTOSPIDER_BUILD_PROVIDER_INDEPENDENT_FOCUSED_TESTS}")
message(STATUS "  Full Test Suite   : ${PHOTOSPIDER_BUILD_FULL_TEST_SUITE}")
if(APPLE AND PHOTOSPIDER_BUILD_OPENCV_OPERATION_PLUGINS)
    message(STATUS "  Metal Plugins     : ${METAL_ROOT_DIR}")
endif()
message(STATUS "-----------------------------------------------------------")
