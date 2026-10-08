cmake_minimum_required(VERSION 3.21)
project(PhotospiderRegionalImageVertical LANGUAGES CXX)

if(NOT TARGET Photospider::kernel)
  find_package(Photospider 0.33 CONFIG REQUIRED COMPONENTS kernel)
endif()

add_executable(photospider_regional_image_vertical main.cpp)
target_include_directories(photospider_regional_image_vertical PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/..)
target_link_libraries(photospider_regional_image_vertical PRIVATE Photospider::kernel)
set_target_properties(photospider_regional_image_vertical PROPERTIES CXX_EXTENSIONS OFF)
if(CMAKE_CXX_COMPILER_ID MATCHES "Clang")
  target_compile_options(photospider_regional_image_vertical PRIVATE
    -Wall -Wextra -Wpedantic -Werror -fno-fast-math -ffp-contract=off)
endif()
