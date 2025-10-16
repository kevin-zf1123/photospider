# FILE: CMakeLists.txt (Complete and Corrected)

# Minimum CMake version required
cmake_minimum_required(VERSION 3.16)

# Define the project and specify C++17
project(Photospider LANGUAGES CXX)
option(USE_ASAN "Enable AddressSanitizer" OFF)
option(USE_TSAN "Enable ThreadSanitizer" OFF)

if(CMAKE_CXX_COMPILER_ID MATCHES "Clang|GNU")
    if(USE_ASAN)
        add_compile_options(-fsanitize=address -g)
        add_link_options(-fsanitize=address)
    elseif(USE_TSAN)
        add_compile_options(-fsanitize=thread -g)
        add_link_options(-fsanitize=thread)
    endif()
endif()
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# --- Find Dependencies ---
find_package(OpenCV REQUIRED COMPONENTS core imgproc imgcodecs videoio)
find_package(yaml-cpp REQUIRED)

include(FetchContent)

FetchContent_Declare(
  googletest
  URL https://github.com/google/googletest/archive/refs/tags/v1.15.2.zip
)
FetchContent_MakeAvailable(googletest)

FetchContent_Declare(
  nlohmann_json
  URL https://github.com/nlohmann/json/releases/download/v3.11.3/json.tar.xz
)
FetchContent_MakeAvailable(nlohmann_json)

# --- Build FTXUI as Modular Static Libraries ---
set(FTXUI_INCLUDE_DIR "${PROJECT_SOURCE_DIR}/extern/ftxui/include")
set(FTXUI_SRC_DIR "${PROJECT_SOURCE_DIR}/extern/ftxui/src")

function(filter_ftxui_sources out_var)
    list(FILTER ${out_var} EXCLUDE REGEX ".*_test\\.cpp$")
    list(FILTER ${out_var} EXCLUDE REGEX ".*_fuzzer\\.cpp$")
    list(FILTER ${out_var} EXCLUDE REGEX ".*_benchmark\\.cpp$")
    set(${out_var} ${${out_var}} PARENT_SCOPE)
endfunction()

file(GLOB_RECURSE FTXUI_SCREEN_SOURCES "${FTXUI_SRC_DIR}/ftxui/screen/*.cpp")
filter_ftxui_sources(FTXUI_SCREEN_SOURCES)
add_library(ftxui-screen STATIC ${FTXUI_SCREEN_SOURCES})
target_include_directories(ftxui-screen PUBLIC ${FTXUI_INCLUDE_DIR} PRIVATE ${FTXUI_SRC_DIR})

file(GLOB_RECURSE FTXUI_DOM_SOURCES "${FTXUI_SRC_DIR}/ftxui/dom/*.cpp")
filter_ftxui_sources(FTXUI_DOM_SOURCES)
add_library(ftxui-dom STATIC ${FTXUI_DOM_SOURCES})
target_include_directories(ftxui-dom PUBLIC ${FTXUI_INCLUDE_DIR} PRIVATE ${FTXUI_SRC_DIR})
target_link_libraries(ftxui-dom PUBLIC ftxui-screen)

file(GLOB_RECURSE FTXUI_COMPONENT_SOURCES "${FTXUI_SRC_DIR}/ftxui/component/*.cpp")
filter_ftxui_sources(FTXUI_COMPONENT_SOURCES)
add_library(ftxui-component STATIC ${FTXUI_COMPONENT_SOURCES})
target_include_directories(ftxui-component PUBLIC ${FTXUI_INCLUDE_DIR} PRIVATE ${FTXUI_SRC_DIR})
target_link_libraries(ftxui-component PUBLIC ftxui-dom)

if(APPLE)
    enable_language(OBJCXX)
endif()

# --- Modular Photospider Libraries ---
set(PHOTOSPIDER_CORE_TYPES_SOURCES
    src/ps_types.cpp
    src/adapter/buffer_adapter_opencv.cpp
    src/ops.cpp
    src/kernel/node_module/node_yaml.cpp
)

add_library(photospider_core_types STATIC ${PHOTOSPIDER_CORE_TYPES_SOURCES})
target_include_directories(photospider_core_types PUBLIC
    "${PROJECT_SOURCE_DIR}/include"
    "${PROJECT_SOURCE_DIR}/src"
    ${OpenCV_INCLUDE_DIRS}
)
target_link_libraries(photospider_core_types PUBLIC yaml-cpp::yaml-cpp)
set_target_properties(photospider_core_types PROPERTIES POSITION_INDEPENDENT_CODE ON)

set(PHOTOSPIDER_GRAPH_SOURCES
    src/kernel/graph_model.cpp
    src/kernel/services/graph_traversal_service.cpp
    src/kernel/services/graph_io_service.cpp
    src/kernel/services/graph_cache_service.cpp
)

add_library(photospider_graph STATIC ${PHOTOSPIDER_GRAPH_SOURCES})
target_include_directories(photospider_graph PUBLIC
    "${PROJECT_SOURCE_DIR}/include"
    "${PROJECT_SOURCE_DIR}/src"
    ${OpenCV_INCLUDE_DIRS}
)
target_link_libraries(photospider_graph PUBLIC photospider_core_types)
set_target_properties(photospider_graph PROPERTIES POSITION_INDEPENDENT_CODE ON)

set(PHOTOSPIDER_PLUGIN_SOURCES
    src/kernel/plugin_manager.cpp
    src/kernel/plugin_manage_module/plugin_loader.cpp
)

add_library(photospider_plugin STATIC ${PHOTOSPIDER_PLUGIN_SOURCES})
target_include_directories(photospider_plugin PUBLIC
    "${PROJECT_SOURCE_DIR}/include"
    "${PROJECT_SOURCE_DIR}/src"
)
target_link_libraries(photospider_plugin PUBLIC photospider_core_types)
set_target_properties(photospider_plugin PROPERTIES POSITION_INDEPENDENT_CODE ON)

set(PHOTOSPIDER_COMPUTE_SOURCES
    src/kernel/services/compute_service.cpp
    src/kernel/services/graph_event_service.cpp
    src/kernel/kernel.cpp
    src/kernel/interaction.cpp
    src/kernel/interaction_benchmark.cpp
)
if(APPLE)
    list(APPEND PHOTOSPIDER_COMPUTE_SOURCES src/kernel/graph_runtime.mm)
endif()

add_library(photospider_compute STATIC ${PHOTOSPIDER_COMPUTE_SOURCES})
target_include_directories(photospider_compute PUBLIC
    "${PROJECT_SOURCE_DIR}/include"
    "${PROJECT_SOURCE_DIR}/src"
    ${OpenCV_INCLUDE_DIRS}
)
target_link_libraries(photospider_compute PUBLIC photospider_graph photospider_plugin)
set_target_properties(photospider_compute PROPERTIES POSITION_INDEPENDENT_CODE ON)

file(GLOB_RECURSE PHOTOSPIDER_CLI_SOURCES
    "src/cli/*.cpp"
    "src/benchmark/*.cpp"
    "src/cli_config.cpp"
)

add_library(photospider_lib SHARED ${PHOTOSPIDER_CLI_SOURCES})
target_compile_definitions(photospider_lib PRIVATE PHOTOSPIDER_LIB_BUILD)

set_target_properties(photospider_lib PROPERTIES
    CXX_VISIBILITY_PRESET default
    DEFINE_SYMBOL "PHOTOSPIDER_LIBRARY"
)
if(WIN32)
    set_target_properties(photospider_lib PROPERTIES WINDOWS_EXPORT_ALL_SYMBOLS ON)
endif()

target_include_directories(photospider_lib PUBLIC
    "${PROJECT_SOURCE_DIR}/include"
    "${PROJECT_SOURCE_DIR}/src"
)

target_link_libraries(photospider_lib PUBLIC
    photospider_core_types
    photospider_graph
    photospider_compute
    photospider_plugin
    ftxui-component
    ${OpenCV_LIBS}
    yaml-cpp::yaml-cpp
)

# --- Configure Main Executable ---
add_executable(graph_cli cli/graph_cli.cpp)
target_include_directories(graph_cli PRIVATE 
    "${PROJECT_SOURCE_DIR}/include"
    "${PROJECT_SOURCE_DIR}/src"
)
target_link_libraries(graph_cli PRIVATE photospider_lib)
if (UNIX AND NOT APPLE)
    target_link_libraries(graph_cli PRIVATE anl)
endif()
set_target_properties(graph_cli PROPERTIES
    BUILD_RPATH "${CMAKE_BINARY_DIR}"
    INSTALL_RPATH "$ORIGIN/../lib"
)

# --- Configure Plugins ---
set(PLUGIN_OUTPUT_DIR ${CMAKE_BINARY_DIR}/plugins)
file(MAKE_DIRECTORY ${PLUGIN_OUTPUT_DIR})
file(GLOB PLUGIN_SOURCES "custom_ops/*.cpp")

if(APPLE)
    list(REMOVE_ITEM PLUGIN_SOURCES "${CMAKE_CURRENT_SOURCE_DIR}/custom_ops/metal_ops_loader.cpp")
endif()

foreach(PLUGIN_SOURCE ${PLUGIN_SOURCES})
    get_filename_component(PLUGIN_NAME ${PLUGIN_SOURCE} NAME_WE)
    add_library(${PLUGIN_NAME} SHARED ${PLUGIN_SOURCE})
    set_target_properties(${PLUGIN_NAME} PROPERTIES
        LIBRARY_OUTPUT_DIRECTORY "${PLUGIN_OUTPUT_DIR}"
        RUNTIME_OUTPUT_DIRECTORY "${PLUGIN_OUTPUT_DIR}"
        BUILD_RPATH "${CMAKE_BINARY_DIR}"
        INSTALL_RPATH "$ORIGIN/../lib"
    )
    target_include_directories(${PLUGIN_NAME} PRIVATE "${PROJECT_SOURCE_DIR}/include" "${PROJECT_SOURCE_DIR}/src")
    target_link_libraries(${PLUGIN_NAME} PRIVATE photospider_lib)
endforeach()

# --- Platform-Specific Adjustments ---
if(APPLE)
    set(METAL_ROOT_DIR ${CMAKE_BINARY_DIR}/high_performance/metal)
    set(METAL_OPS_DIR  ${METAL_ROOT_DIR}/ops)
    file(MAKE_DIRECTORY ${METAL_ROOT_DIR})
    file(MAKE_DIRECTORY ${METAL_OPS_DIR})

    find_library(METAL_FRAMEWORK Metal REQUIRED)
    find_library(FOUNDATION_FRAMEWORK Foundation REQUIRED)
    find_library(COREIMAGE_FRAMEWORK CoreImage REQUIRED)
    find_library(COREVIDEO_FRAMEWORK CoreVideo REQUIRED)
    
    target_link_libraries(photospider_lib PRIVATE
        ${METAL_FRAMEWORK}
        ${FOUNDATION_FRAMEWORK}
    )

    add_library(perlin_noise_metal SHARED src/metal/perlin_noise_metal.mm)
    set_target_properties(perlin_noise_metal PROPERTIES
        LIBRARY_OUTPUT_DIRECTORY "${METAL_OPS_DIR}"
        RUNTIME_OUTPUT_DIRECTORY "${METAL_OPS_DIR}"
        MACOSX_RPATH ON
    )
    target_link_libraries(perlin_noise_metal PRIVATE
        ${METAL_FRAMEWORK}
        ${FOUNDATION_FRAMEWORK}
        ${COREIMAGE_FRAMEWORK}
        ${COREVIDEO_FRAMEWORK}
        ${OpenCV_LIBS}
        yaml-cpp::yaml-cpp
    )
    target_include_directories(perlin_noise_metal PRIVATE "${PROJECT_SOURCE_DIR}/include")

    add_library(metal_ops_loader SHARED custom_ops/metal_ops_loader.cpp)
    set_target_properties(metal_ops_loader PROPERTIES
        LIBRARY_OUTPUT_DIRECTORY "${METAL_ROOT_DIR}"
        RUNTIME_OUTPUT_DIRECTORY "${METAL_ROOT_DIR}"
        BUILD_RPATH "@loader_path/ops;${CMAKE_BINARY_DIR}"
        INSTALL_RPATH "@loader_path/ops;$ORIGIN/../lib"
        MACOSX_RPATH ON
    )
    
    target_include_directories(metal_ops_loader PRIVATE
        "${PROJECT_SOURCE_DIR}/include"
        "${PROJECT_SOURCE_DIR}/src"
    )
    target_link_libraries(metal_ops_loader PRIVATE
        perlin_noise_metal
        photospider_lib
    )

    set(CMAKE_SHARED_LINKER_FLAGS "${CMAKE_SHARED_LINKER_FLAGS} -undefined dynamic_lookup")
endif()

# --- Testing Target ---
enable_testing()
set(TEST_OUTPUT_DIR ${CMAKE_BINARY_DIR}/tests)
file(MAKE_DIRECTORY ${TEST_OUTPUT_DIR})

add_executable(test_scheduler tests/test_scheduler.cpp)
set_target_properties(test_scheduler PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${TEST_OUTPUT_DIR}"
    BUILD_RPATH "${CMAKE_BINARY_DIR}"
)
target_include_directories(test_scheduler PRIVATE
    "${PROJECT_SOURCE_DIR}/include"
    "${PROJECT_SOURCE_DIR}/src"
)
target_link_libraries(test_scheduler PRIVATE
    photospider_lib
    GTest::gtest_main
    nlohmann_json::nlohmann_json
)

add_executable(test_milestone2 tests/test_milestone2.cpp)
set_target_properties(test_milestone2 PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${TEST_OUTPUT_DIR}"
    BUILD_RPATH "${CMAKE_BINARY_DIR}"
)
target_include_directories(test_milestone2 PRIVATE 
    "${PROJECT_SOURCE_DIR}/include"
    "${PROJECT_SOURCE_DIR}/src"
)
target_link_libraries(test_milestone2 PRIVATE photospider_lib)
add_test(NAME Milestone2Tests COMMAND test_milestone2)

add_executable(test_milestone3 tests/test_milestone3.cpp)
set_target_properties(test_milestone3 PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${TEST_OUTPUT_DIR}"
    BUILD_RPATH "${CMAKE_BINARY_DIR}"
)
target_include_directories(test_milestone3 PRIVATE 
    "${PROJECT_SOURCE_DIR}/include"
    "${PROJECT_SOURCE_DIR}/src"
)
target_link_libraries(test_milestone3 PRIVATE photospider_lib)
add_test(NAME Milestone3Tests COMMAND test_milestone3)

include(GoogleTest)
gtest_discover_tests(test_scheduler)

# --- Final Output ---
message(STATUS "Main executable will be built in: ${CMAKE_BINARY_DIR}")
message(STATUS "Core shared library will be built in: ${CMAKE_BINARY_DIR}")
message(STATUS "Plugins will be built in: ${PLUGIN_OUTPUT_DIR}")
if(APPLE)
    message(STATUS "Metal plugins will be built in: ${METAL_ROOT_DIR}")
    message(STATUS "Metal ops will be built in: ${METAL_OPS_DIR}")
endif()
message(STATUS "Test executables will be built in: ${TEST_OUTPUT_DIR}")
