cmake_minimum_required(VERSION 3.21)
project(PhotospiderConsumer LANGUAGES C CXX)

set(PHOTOSPIDER_CONSUMER_SANITIZER none CACHE STRING
    "Matching producer sanitizer: none, address, or thread")
set_property(
  CACHE PHOTOSPIDER_CONSUMER_SANITIZER PROPERTY STRINGS none address thread)
if(NOT PHOTOSPIDER_CONSUMER_SANITIZER MATCHES "^(none|address|thread)$")
  message(FATAL_ERROR "Photospider consumer sanitizer mode is invalid")
endif()

set(_photospider_consumer_compile_options)
set(_photospider_consumer_link_options)
if(NOT PHOTOSPIDER_CONSUMER_SANITIZER STREQUAL "none")
  if(NOT CMAKE_C_COMPILER_ID MATCHES "Clang|GNU" OR
     NOT CMAKE_CXX_COMPILER_ID MATCHES "Clang|GNU")
    message(FATAL_ERROR "Sanitized consumer requires Clang or GNU C/C++")
  endif()
  include(CheckCCompilerFlag)
  include(CheckCXXCompilerFlag)
  include(CheckLinkerFlag)
  set(_photospider_consumer_sanitizer_flag
      "-fsanitize=${PHOTOSPIDER_CONSUMER_SANITIZER}")
  set(CMAKE_REQUIRED_LINK_OPTIONS "${_photospider_consumer_sanitizer_flag}")
  check_c_compiler_flag(
    "${_photospider_consumer_sanitizer_flag}"
    PHOTOSPIDER_CONSUMER_C_SANITIZER_SUPPORTED)
  check_cxx_compiler_flag(
    "${_photospider_consumer_sanitizer_flag}"
    PHOTOSPIDER_CONSUMER_CXX_SANITIZER_SUPPORTED)
  unset(CMAKE_REQUIRED_LINK_OPTIONS)
  check_c_compiler_flag(
    "-fno-omit-frame-pointer"
    PHOTOSPIDER_CONSUMER_C_FRAME_POINTER_SUPPORTED)
  check_cxx_compiler_flag(
    "-fno-omit-frame-pointer"
    PHOTOSPIDER_CONSUMER_CXX_FRAME_POINTER_SUPPORTED)
  check_linker_flag(
    CXX "${_photospider_consumer_sanitizer_flag}"
    PHOTOSPIDER_CONSUMER_CXX_SANITIZER_LINK_SUPPORTED)
  if(NOT PHOTOSPIDER_CONSUMER_C_SANITIZER_SUPPORTED OR
     NOT PHOTOSPIDER_CONSUMER_CXX_SANITIZER_SUPPORTED OR
     NOT PHOTOSPIDER_CONSUMER_C_FRAME_POINTER_SUPPORTED OR
     NOT PHOTOSPIDER_CONSUMER_CXX_FRAME_POINTER_SUPPORTED OR
     NOT PHOTOSPIDER_CONSUMER_CXX_SANITIZER_LINK_SUPPORTED)
    message(
      FATAL_ERROR
        "Requested ${PHOTOSPIDER_CONSUMER_SANITIZER} consumer sanitizer is unsupported")
  endif()
  list(
    APPEND _photospider_consumer_compile_options
    "${_photospider_consumer_sanitizer_flag}" -fno-omit-frame-pointer)
  list(
    APPEND _photospider_consumer_link_options
    "${_photospider_consumer_sanitizer_flag}")
endif()

find_package(
  Photospider 0.2 CONFIG REQUIRED
  COMPONENTS kernel operation_sdk data_provider_sdk)

add_library(photospider_sdk_consumer OBJECT sdk_smoke.c)
target_link_libraries(
  photospider_sdk_consumer
  PRIVATE Photospider::operation_sdk Photospider::data_provider_sdk)
target_compile_options(
  photospider_sdk_consumer PRIVATE ${_photospider_consumer_compile_options})

add_library(photospider_consumer_bridge SHARED pipeline_bridge.cpp)
target_compile_features(photospider_consumer_bridge PRIVATE cxx_std_17)
target_link_libraries(
  photospider_consumer_bridge PRIVATE Photospider::kernel)
target_compile_options(
  photospider_consumer_bridge PRIVATE ${_photospider_consumer_compile_options})
target_link_options(
  photospider_consumer_bridge PRIVATE ${_photospider_consumer_link_options})
set_target_properties(
  photospider_consumer_bridge PROPERTIES WINDOWS_EXPORT_ALL_SYMBOLS ON)

add_executable(
  photospider_consumer
  main.cpp
  $<TARGET_OBJECTS:photospider_sdk_consumer>)
target_compile_features(photospider_consumer PRIVATE cxx_std_17)
target_link_libraries(
  photospider_consumer
  PRIVATE photospider_consumer_bridge Photospider::operation_sdk
          Photospider::data_provider_sdk)
target_compile_options(
  photospider_consumer PRIVATE ${_photospider_consumer_compile_options})
target_link_options(
  photospider_consumer PRIVATE ${_photospider_consumer_link_options})

add_custom_target(
  run_photospider_consumer
  COMMAND "$<TARGET_FILE:photospider_consumer>"
  DEPENDS photospider_consumer
  USES_TERMINAL
  VERBATIM)
