cmake_minimum_required(VERSION 3.21)
project(PhotospiderImageVertical LANGUAGES CXX)

if(NOT TARGET Photospider::kernel)
  find_package(Photospider 0.5 CONFIG REQUIRED COMPONENTS kernel)
endif()

add_executable(photospider_image_vertical main.cpp)
target_link_libraries(photospider_image_vertical PRIVATE Photospider::kernel)
set_target_properties(photospider_image_vertical PROPERTIES CXX_EXTENSIONS OFF)
if(TARGET photospider_rgba32f_ops)
  add_dependencies(photospider_image_vertical photospider_rgba32f_ops)
endif()
if(CMAKE_CXX_COMPILER_ID MATCHES "Clang|GNU")
  target_compile_options(photospider_image_vertical PRIVATE
    -Wall -Wextra -Wpedantic -Werror -fno-fast-math -ffp-contract=off)
endif()
