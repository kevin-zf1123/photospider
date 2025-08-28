# Minimum CMake version required
cmake_minimum_required(VERSION 3.16)

# Define the project and specify C++17
project(Photospider LANGUAGES CXX)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# --- Find Dependencies ---
# Find OpenCV
find_package(OpenCV REQUIRED)
if (NOT OpenCV_FOUND)
    message(FATAL_ERROR "OpenCV not found!")
endif()

# Find yaml-cpp
find_package(yaml-cpp REQUIRED)
if (NOT yaml-cpp_FOUND)
    message(FATAL_ERROR "yaml-cpp not found! Please ensure it is installed and findable by CMake.")
endif()

# --- Build FTXUI as a Static Library (mirroring Makefile logic) ---
# Gather FTXUI source files, excluding tests and fuzzers
file(GLOB_RECURSE FTXUI_SOURCES
    "${PROJECT_SOURCE_DIR}/extern/ftxui/src/*.cpp"
)
list(FILTER FTXUI_SOURCES EXCLUDE REGEX ".*_test\\.cpp$")
list(FILTER FTXUI_SOURCES EXCLUDE REGEX ".*_fuzzer\\.cpp$")
list(FILTER FTXUI_SOURCES EXCLUDE REGEX ".*_benchmark\\.cpp$")

# Create the static library target for FTXUI
add_library(ftxui_static STATIC ${FTXUI_SOURCES})

# Specify that targets linking to ftxui_static need its include directories
target_include_directories(ftxui_static PUBLIC
    "${PROJECT_SOURCE_DIR}/extern/ftxui/include"
    "${PROJECT_SOURCE_DIR}/extern/ftxui/src" # For internal FTXUI headers
)


# --- Configure Main Executable ---
# Gather all source files for the main application
file(GLOB APP_SOURCES
    "src/*.cpp"
    "cli/*.cpp"
)

# Add the main executable target
add_executable(graph_cli ${APP_SOURCES})

# Specify include directories for the main executable
target_include_directories(graph_cli PRIVATE
    "${PROJECT_SOURCE_DIR}/include"
)

# Link libraries to the main executable.
# Linking to ftxui_static will also add its PUBLIC include directories.
target_link_libraries(graph_cli PRIVATE
    ftxui_static
    ${OpenCV_LIBS}
    yaml-cpp::yaml-cpp # Use the modern imported target for yaml-cpp
)


# --- Configure Plugins ---
# Define the output directory for plugins
set(PLUGIN_OUTPUT_DIR ${CMAKE_BINARY_DIR}/plugins)
file(MAKE_DIRECTORY ${PLUGIN_OUTPUT_DIR})

# Gather all source files for the custom operations
file(GLOB PLUGIN_SOURCES "custom_ops/*.cpp")

# Loop over each plugin source and create a shared library for it
foreach(PLUGIN_SOURCE ${PLUGIN_SOURCES})
    get_filename_component(PLUGIN_NAME ${PLUGIN_SOURCE} NAME_WE)

    add_library(${PLUGIN_NAME} SHARED ${PLUGIN_SOURCE})

    # *** FIX: Set properties for the shared library, including the SUFFIX ***
    set_target_properties(${PLUGIN_NAME} PROPERTIES
        LIBRARY_OUTPUT_DIRECTORY "${PLUGIN_OUTPUT_DIR}"
        RUNTIME_OUTPUT_DIRECTORY "${PLUGIN_OUTPUT_DIR}"
        SUFFIX ".so"  # <--- THIS IS THE FIX
    )

    # Add the necessary include directories for the plugins
    target_include_directories(${PLUGIN_NAME} PRIVATE
        "${PROJECT_SOURCE_DIR}/include"
    )

    # Link the same dependencies to the plugin
    target_link_libraries(${PLUGIN_NAME} PRIVATE
        ftxui_static
        ${OpenCV_LIBS}
        yaml-cpp::yaml-cpp
    )
endforeach()


# --- Platform-Specific Adjustments ---
if(APPLE)
    # This handles the "-undefined dynamic_lookup" flag needed for plugins on macOS
    set(CMAKE_SHARED_LINKER_FLAGS "${CMAKE_SHARED_LINKER_FLAGS} -undefined dynamic_lookup")
endif()


# --- Final Output ---
message(STATUS "Main executable will be built in: ${CMAKE_BINARY_DIR}")
message(STATUS "Plugins will be built in: ${PLUGIN_OUTPUT_DIR}")