# FILE: CMakeLists.txt (Complete and Corrected)

# Minimum CMake version required
cmake_minimum_required(VERSION 3.16)

# Define the project and specify C++17
project(Photospider LANGUAGES CXX)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# --- Find Dependencies ---
find_package(OpenCV REQUIRED COMPONENTS core imgproc imgcodecs videoio)
find_package(yaml-cpp REQUIRED)

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

# --- [REFACTOR] Create a Static Library for Core Logic ---
# This library will contain all sources EXCEPT the main() entry point.
file(GLOB_RECURSE CORE_LIB_SOURCES 
    "src/*.cpp"
    "src/*.hpp"
)
add_library(photospider_lib STATIC ${CORE_LIB_SOURCES})
target_include_directories(photospider_lib PUBLIC 
    "${PROJECT_SOURCE_DIR}/include"
    "${PROJECT_SOURCE_DIR}/src"
)
# The library itself needs to link to its dependencies.
target_link_libraries(photospider_lib PRIVATE
    ftxui-component
    ${OpenCV_LIBS}
    yaml-cpp::yaml-cpp
)

# --- Configure Main Executable ---
# The executable now only contains its own main() and links to the core library.
add_executable(graph_cli cli/graph_cli.cpp)
target_include_directories(graph_cli PRIVATE 
    "${PROJECT_SOURCE_DIR}/include"
    "${PROJECT_SOURCE_DIR}/src"
)
find_library(FW_METAL Metal)
find_library(FW_COREVIDEO CoreVideo)
find_library(FW_COREIMAGE CoreImage)
find_library(FW_QUARTZCORE QuartzCore)
target_link_libraries(graph_cli PRIVATE
    photospider_lib # <-- Link to our core library
    ftxui-component
    ${OpenCV_LIBS}
    yaml-cpp::yaml-cpp
    ${FW_METAL}
    ${FW_COREVIDEO}
    ${FW_COREIMAGE}
    ${FW_QUARTZCORE}
)
if (UNIX AND NOT APPLE)
    target_link_libraries(graph_cli PRIVATE anl)
endif()

# --- Configure Plugins ---
set(PLUGIN_OUTPUT_DIR ${CMAKE_BINARY_DIR}/plugins)
file(MAKE_DIRECTORY ${PLUGIN_OUTPUT_DIR})
file(GLOB PLUGIN_SOURCES "custom_ops/*.cpp")

# Exclude platform-specific plugins from the generic list
if(APPLE)
    list(REMOVE_ITEM PLUGIN_SOURCES "${CMAKE_CURRENT_SOURCE_DIR}/custom_ops/metal_ops_loader.cpp")
endif()

foreach(PLUGIN_SOURCE ${PLUGIN_SOURCES})
    get_filename_component(PLUGIN_NAME ${PLUGIN_SOURCE} NAME_WE)
    add_library(${PLUGIN_NAME} SHARED ${PLUGIN_SOURCE})
    set_target_properties(${PLUGIN_NAME} PROPERTIES
        LIBRARY_OUTPUT_DIRECTORY "${PLUGIN_OUTPUT_DIR}"
        RUNTIME_OUTPUT_DIRECTORY "${PLUGIN_OUTPUT_DIR}"
    )
    target_include_directories(${PLUGIN_NAME} PRIVATE "${PROJECT_SOURCE_DIR}/include" "${PROJECT_SOURCE_DIR}/src")
    target_link_libraries(${PLUGIN_NAME} PRIVATE
        photospider_lib # <-- Plugins also depend on core types/functions
        ftxui-component
        ${OpenCV_LIBS}
        yaml-cpp::yaml-cpp
    )
endforeach()

# --- Platform-Specific Adjustments ---
if(APPLE)
    # Output layout for Metal components
    set(METAL_ROOT_DIR ${CMAKE_BINARY_DIR}/high_performance/metal)
    set(METAL_OPS_DIR  ${METAL_ROOT_DIR}/ops)
    file(MAKE_DIRECTORY ${METAL_ROOT_DIR})
    file(MAKE_DIRECTORY ${METAL_OPS_DIR})

    enable_language(OBJCXX)

    # Use CMake's standard way to find and link Apple frameworks
    find_library(METAL_FRAMEWORK Metal REQUIRED)
    find_library(FOUNDATION_FRAMEWORK Foundation REQUIRED)
    find_library(COREIMAGE_FRAMEWORK CoreImage REQUIRED)
    find_library(COREVIDEO_FRAMEWORK CoreVideo REQUIRED)

    # Main Metal implementation library (goes under ops/)
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

    # Loader plugin that registers the Metal op (stays at metal/ root)
    add_library(metal_ops_loader SHARED custom_ops/metal_ops_loader.cpp)
    set_target_properties(metal_ops_loader PROPERTIES
        LIBRARY_OUTPUT_DIRECTORY "${METAL_ROOT_DIR}"
        RUNTIME_OUTPUT_DIRECTORY "${METAL_ROOT_DIR}"
        # Ensure dependent Metal ops under ops/ are found at runtime
        BUILD_RPATH "@loader_path/ops"
        INSTALL_RPATH "@loader_path/ops"
        MACOSX_RPATH ON
    )
    
    target_include_directories(metal_ops_loader PRIVATE
        "${PROJECT_SOURCE_DIR}/include"
        "${PROJECT_SOURCE_DIR}/src"
    )
    target_link_libraries(metal_ops_loader PRIVATE
        perlin_noise_metal
        photospider_lib # <-- Metal plugin loader also needs core logic
        ${OpenCV_LIBS}
        yaml-cpp::yaml-cpp
        ftxui-component
    )

    set(CMAKE_SHARED_LINKER_FLAGS "${CMAKE_SHARED_LINKER_FLAGS} -undefined dynamic_lookup")
endif()

# --- Testing Target ---
enable_testing()
set(TEST_OUTPUT_DIR ${CMAKE_BINARY_DIR}/tests)
file(MAKE_DIRECTORY ${TEST_OUTPUT_DIR})

# *** FIX: The test executable now only compiles its own source file. ***
add_executable(test_milestone2 tests/test_milestone2.cpp)
set_target_properties(test_milestone2 PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${TEST_OUTPUT_DIR}"
)
target_include_directories(test_milestone2 PRIVATE 
    "${PROJECT_SOURCE_DIR}/include"
    "${PROJECT_SOURCE_DIR}/src"
)
# *** FIX: Link the test executable against the core static library. ***
target_link_libraries(test_milestone2 PRIVATE
    photospider_lib
    ftxui-component
    ${OpenCV_LIBS}
    yaml-cpp::yaml-cpp
)
add_test(NAME Milestone2Tests COMMAND test_milestone2)

# [新增] 为里程碑3添加新的测试目标
add_executable(test_milestone3 tests/test_milestone3.cpp)
set_target_properties(test_milestone3 PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${TEST_OUTPUT_DIR}"
)
target_include_directories(test_milestone3 PRIVATE 
    "${PROJECT_SOURCE_DIR}/include"
    "${PROJECT_SOURCE_DIR}/src"
)
target_link_libraries(test_milestone3 PRIVATE
    photospider_lib
    ftxui-component
    ${OpenCV_LIBS}
    yaml-cpp::yaml-cpp
)
add_test(NAME Milestone3Tests COMMAND test_milestone3)


# --- Final Output ---
message(STATUS "Main executable will be built in: ${CMAKE_BINARY_DIR}")
message(STATUS "Plugins will be built in: ${PLUGIN_OUTPUT_DIR}")
if(APPLE)
    message(STATUS "Metal plugins will be built in: ${METAL_ROOT_DIR}")
    message(STATUS "Metal ops will be built in: ${METAL_OPS_DIR}")
endif()
message(STATUS "Test executables will be built in: ${TEST_OUTPUT_DIR}")