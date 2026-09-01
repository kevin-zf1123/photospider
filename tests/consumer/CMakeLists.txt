cmake_minimum_required(VERSION 3.21)
project(PhotospiderConsumer LANGUAGES C CXX)

find_package(
  Photospider 0.2 CONFIG REQUIRED
  COMPONENTS kernel operation_sdk data_provider_sdk)

add_library(photospider_sdk_consumer OBJECT sdk_smoke.c)
target_link_libraries(
  photospider_sdk_consumer
  PRIVATE Photospider::operation_sdk Photospider::data_provider_sdk)

add_executable(photospider_consumer main.cpp)
target_compile_features(photospider_consumer PRIVATE cxx_std_17)
target_link_libraries(
  photospider_consumer
  PRIVATE Photospider::kernel Photospider::operation_sdk
          Photospider::data_provider_sdk)
