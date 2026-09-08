cmake_minimum_required(VERSION 3.21)
project(PhotospiderRgba32fOperations VERSION 1.0.0 LANGUAGES C)

if(NOT TARGET Photospider::operation_sdk)
  find_package(Photospider 0.4 CONFIG REQUIRED COMPONENTS operation_sdk)
endif()

add_library(photospider_rgba32f_ops MODULE image_plugin.c)
target_link_libraries(photospider_rgba32f_ops PRIVATE Photospider::operation_sdk)
target_compile_definitions(photospider_rgba32f_ops PRIVATE PHOTOSPIDER_OPERATION_PLUGIN_BUILD)
set_target_properties(photospider_rgba32f_ops PROPERTIES
  C_STANDARD 11 C_STANDARD_REQUIRED ON C_EXTENSIONS OFF)
if(CMAKE_C_COMPILER_ID MATCHES "Clang|GNU")
  target_compile_options(photospider_rgba32f_ops PRIVATE
    -Wall -Wextra -Wpedantic -Werror -fno-fast-math -frounding-math -ffp-contract=off)
elseif(MSVC)
  target_compile_options(photospider_rgba32f_ops PRIVATE /fp:strict)
endif()

if(UNIX)
  target_link_libraries(photospider_rgba32f_ops PRIVATE m)
endif()
