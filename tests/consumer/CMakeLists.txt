cmake_minimum_required(VERSION 3.21)
project(PhotospiderConsumer LANGUAGES C CXX)

find_package(
  Photospider 0.2 CONFIG REQUIRED
  COMPONENTS kernel operation_sdk data_provider_sdk)

add_library(photospider_sdk_consumer OBJECT sdk_smoke.c)
target_link_libraries(
  photospider_sdk_consumer
  PRIVATE Photospider::operation_sdk Photospider::data_provider_sdk)

add_library(photospider_consumer_bridge SHARED pipeline_bridge.cpp)
target_compile_features(photospider_consumer_bridge PRIVATE cxx_std_17)
target_link_libraries(
  photospider_consumer_bridge PRIVATE Photospider::kernel)
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

add_custom_target(
  run_photospider_consumer
  COMMAND "$<TARGET_FILE:photospider_consumer>"
  DEPENDS photospider_consumer
  VERBATIM)
