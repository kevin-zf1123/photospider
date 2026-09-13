cmake_minimum_required(VERSION 3.21)
project(PhotospiderInpaintNsWorkflow LANGUAGES CXX)

if(NOT TARGET Photospider::kernel)
  find_package(Photospider 0.10 CONFIG REQUIRED COMPONENTS kernel)
endif()

add_executable(photospider_inpaint_ns_workflow main.cpp)
target_link_libraries(photospider_inpaint_ns_workflow PRIVATE Photospider::kernel)
set_target_properties(photospider_inpaint_ns_workflow PROPERTIES CXX_EXTENSIONS OFF)
if(CMAKE_CXX_COMPILER_ID MATCHES "Clang|GNU")
  target_compile_options(photospider_inpaint_ns_workflow PRIVATE
    -Wall -Wextra -Wpedantic -Werror -fno-fast-math -ffp-contract=off)
endif()
