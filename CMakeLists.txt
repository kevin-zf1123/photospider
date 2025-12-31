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

# --- Compiler Settings ---
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_POSITION_INDEPENDENT_CODE ON) # Required for static libs linking into shared libs

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
    src/kernel/services/graph_traversal_service.cpp
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
set(COMPUTE_SOURCES
    src/kernel/services/compute_service.cpp
    src/kernel/services/graph_event_service.cpp
    src/kernel/kernel.cpp
    src/kernel/interaction.cpp
    src/kernel/interaction_benchmark.cpp
    src/kernel/graph_runtime.mm 
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

add_library(photospider_lib SHARED ${PHOTOSPIDER_BENCHMARK_SOURCES})

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

foreach(PLUGIN_SOURCE ${PLUGIN_SOURCES})
    get_filename_component(PLUGIN_NAME ${PLUGIN_SOURCE} NAME_WE)
    add_library(${PLUGIN_NAME} SHARED ${PLUGIN_SOURCE})
    
    set_target_properties(${PLUGIN_NAME} PROPERTIES
        LIBRARY_OUTPUT_DIRECTORY "${PLUGIN_OUTPUT_DIR}"
        BUILD_RPATH "${CMAKE_BINARY_DIR}/lib"
    )
    
    target_include_directories(${PLUGIN_NAME} PRIVATE "${PROJECT_SOURCE_DIR}/include" "${PROJECT_SOURCE_DIR}/src")
    target_link_libraries(${PLUGIN_NAME} PRIVATE photospider_lib)
endforeach()

# ==============================================================================
# 6. Metal Support (Apple Only)
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
# 7. Tests
# ==============================================================================
if(BUILD_TESTING)
    enable_testing()
    set(TEST_OUTPUT_DIR ${CMAKE_BINARY_DIR}/tests)
    file(MAKE_DIRECTORY ${TEST_OUTPUT_DIR})
    
    include(GoogleTest)
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
    add_ps_test(test_propagation tests/test_propagation.cpp)
    
    # [M3.1] OpRegistry 多设备实现测试
    add_ps_test(test_op_registry_m31 tests/test_op_registry_m31.cpp)
    
    gtest_discover_tests(test_scheduler WORKING_DIRECTORY ${PROJECT_SOURCE_DIR})
    gtest_discover_tests(test_op_registry_m31 WORKING_DIRECTORY ${PROJECT_SOURCE_DIR})
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
if(APPLE)
    message(STATUS "  Metal Plugins     : ${METAL_ROOT_DIR}")
endif()
message(STATUS "-----------------------------------------------------------")