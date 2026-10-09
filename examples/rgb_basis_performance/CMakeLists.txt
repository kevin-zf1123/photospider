cmake_minimum_required(VERSION 3.21)
project(PhotospiderRgbBasisPerformance LANGUAGES CXX)
if(NOT TARGET Photospider::kernel)
  find_package(Photospider 0.33 CONFIG REQUIRED COMPONENTS kernel ops_headers)
endif()
add_executable(photospider_fmt10_performance main.cpp)
target_link_libraries(photospider_fmt10_performance PRIVATE Photospider::kernel Photospider::ops_headers)
target_compile_features(photospider_fmt10_performance PRIVATE cxx_std_17)
target_compile_options(photospider_fmt10_performance PRIVATE
  -Wall -Wextra -Wpedantic -Werror -fno-fast-math -frounding-math -ffp-contract=off)

# Private diagnostic tool: never installed and no allocator override in library.
add_executable(photospider_fmt10_kernel EXCLUDE_FROM_ALL kernel.cpp)
target_link_libraries(photospider_fmt10_kernel PRIVATE Photospider::kernel Photospider::ops_headers)
target_include_directories(photospider_fmt10_kernel PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}/../../plugins/ops ${CMAKE_CURRENT_SOURCE_DIR}/../../src/lib)
target_compile_features(photospider_fmt10_kernel PRIVATE cxx_std_17)
target_compile_options(photospider_fmt10_kernel PRIVATE
  -Wall -Wextra -Wpedantic -Werror -fno-fast-math -frounding-math -ffp-contract=off)
