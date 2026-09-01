cmake_minimum_required(VERSION 3.21)

project(
  Photospider
  VERSION 0.2.0
  DESCRIPTION "Embeddable typed graph compiler and local execution kernel"
  LANGUAGES C CXX)

include(CMakePackageConfigHelpers)
include(CTest)
include(GNUInstallDirs)

option(PHOTOSPIDER_ENABLE_ASAN "Enable AddressSanitizer for local validation" OFF)
option(PHOTOSPIDER_ENABLE_TSAN "Enable ThreadSanitizer for local validation" OFF)

if(PHOTOSPIDER_ENABLE_ASAN AND PHOTOSPIDER_ENABLE_TSAN)
  message(FATAL_ERROR "ASAN and TSAN cannot be enabled together")
endif()

find_package(Threads REQUIRED)

add_library(
  photospider
  src/lib/benchmark/raw_benchmark.cpp
  src/lib/compiler/compiler.cpp
  src/lib/data/region.cpp
  src/lib/data/value.cpp
  src/lib/execution/execution.cpp
  src/lib/graph/graph_context.cpp
  src/lib/plugin/data_definition_registry.cpp
  src/lib/plugin/operation_registry.cpp)
add_library(Photospider::kernel ALIAS photospider)

add_library(photospider_operation_sdk INTERFACE)
add_library(Photospider::operation_sdk ALIAS photospider_operation_sdk)
set_target_properties(photospider_operation_sdk PROPERTIES EXPORT_NAME operation_sdk)
target_include_directories(
  photospider_operation_sdk
  INTERFACE $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
            $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>)

add_library(photospider_data_provider_sdk INTERFACE)
add_library(Photospider::data_provider_sdk ALIAS photospider_data_provider_sdk)
set_target_properties(photospider_data_provider_sdk
                      PROPERTIES EXPORT_NAME data_provider_sdk)
target_include_directories(
  photospider_data_provider_sdk
  INTERFACE $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
            $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>)

set_target_properties(
  photospider
  PROPERTIES CXX_EXTENSIONS OFF
             CXX_VISIBILITY_PRESET hidden
             VISIBILITY_INLINES_HIDDEN YES
             EXPORT_NAME kernel)
target_compile_features(photospider PUBLIC cxx_std_17)
target_include_directories(
  photospider
  PUBLIC $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
         $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>)
target_link_libraries(photospider PUBLIC Threads::Threads PRIVATE ${CMAKE_DL_LIBS})
target_compile_definitions(photospider PRIVATE PHOTOSPIDER_BUILD)
if(NOT BUILD_SHARED_LIBS)
  target_compile_definitions(photospider PUBLIC PHOTOSPIDER_STATIC)
endif()

if(CMAKE_CXX_COMPILER_ID MATCHES "Clang|GNU")
  target_compile_options(photospider PRIVATE -Wall -Wextra -Wpedantic -Werror)
  if(PHOTOSPIDER_ENABLE_ASAN)
    target_compile_options(photospider PUBLIC -fsanitize=address -fno-omit-frame-pointer)
    target_link_options(photospider PUBLIC -fsanitize=address)
  elseif(PHOTOSPIDER_ENABLE_TSAN)
    target_compile_options(photospider PUBLIC -fsanitize=thread -fno-omit-frame-pointer)
    target_link_options(photospider PUBLIC -fsanitize=thread)
  endif()
endif()

install(
  TARGETS photospider photospider_operation_sdk photospider_data_provider_sdk
  EXPORT PhotospiderTargets
  ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
  LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
  RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
  INCLUDES DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})
install(DIRECTORY include/photospider DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})
install(
  EXPORT PhotospiderTargets
  FILE PhotospiderTargets.cmake
  NAMESPACE Photospider::
  DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/Photospider)

configure_package_config_file(
  cmake/PhotospiderConfig.cmake.in
  ${CMAKE_CURRENT_BINARY_DIR}/PhotospiderConfig.cmake
  INSTALL_DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/Photospider)
write_basic_package_version_file(
  ${CMAKE_CURRENT_BINARY_DIR}/PhotospiderConfigVersion.cmake
  VERSION ${PROJECT_VERSION}
  COMPATIBILITY SameMinorVersion)
install(
  FILES ${CMAKE_CURRENT_BINARY_DIR}/PhotospiderConfig.cmake
        ${CMAKE_CURRENT_BINARY_DIR}/PhotospiderConfigVersion.cmake
  DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/Photospider)

if(BUILD_TESTING)
  function(photospider_add_test target source)
    add_executable(${target} ${source})
    target_link_libraries(${target} PRIVATE Photospider::kernel)
    target_include_directories(${target} PRIVATE tests)
    target_compile_features(${target} PRIVATE cxx_std_17)
    if(CMAKE_CXX_COMPILER_ID MATCHES "Clang|GNU")
      target_compile_options(${target} PRIVATE -Wall -Wextra -Wpedantic -Werror)
    endif()
    add_test(NAME ${target} COMMAND ${target})
  endfunction()

  add_library(operation_fixture MODULE tests/fixtures/operation_fixture.cpp)
  target_link_libraries(operation_fixture PRIVATE Photospider::operation_sdk)
  target_compile_definitions(operation_fixture PRIVATE PHOTOSPIDER_OPERATION_PLUGIN_BUILD)
  set_target_properties(operation_fixture PROPERTIES PREFIX "")

  add_library(operation_bad_fixture MODULE tests/fixtures/operation_bad_fixture.cpp)
  target_link_libraries(operation_bad_fixture PRIVATE Photospider::operation_sdk)
  target_compile_definitions(operation_bad_fixture PRIVATE PHOTOSPIDER_OPERATION_PLUGIN_BUILD)
  set_target_properties(operation_bad_fixture PROPERTIES PREFIX "")

  add_library(data_provider_fixture MODULE tests/fixtures/data_provider_fixture.cpp)
  target_link_libraries(data_provider_fixture PRIVATE Photospider::data_provider_sdk)
  target_compile_definitions(data_provider_fixture PRIVATE PHOTOSPIDER_DATA_PROVIDER_BUILD)
  set_target_properties(data_provider_fixture PROPERTIES PREFIX "")

  add_library(data_provider_bad_fixture MODULE tests/fixtures/data_provider_bad_fixture.cpp)
  target_link_libraries(data_provider_bad_fixture PRIVATE Photospider::data_provider_sdk)
  target_compile_definitions(data_provider_bad_fixture PRIVATE PHOTOSPIDER_DATA_PROVIDER_BUILD)
  set_target_properties(data_provider_bad_fixture PROPERTIES PREFIX "")

  photospider_add_test(test_value tests/unit/test_value.cpp)
  photospider_add_test(test_compiler tests/unit/test_compiler.cpp)
  photospider_add_test(test_execution tests/integration/test_execution.cpp)
  photospider_add_test(test_plugin_registry tests/integration/test_plugin_registry.cpp)
  add_dependencies(
    test_plugin_registry
    operation_fixture
    operation_bad_fixture
    data_provider_fixture
    data_provider_bad_fixture)
  target_compile_definitions(
    test_plugin_registry
    PRIVATE PS_OPERATION_FIXTURE_PATH="$<TARGET_FILE:operation_fixture>"
            PS_OPERATION_BAD_FIXTURE_PATH="$<TARGET_FILE:operation_bad_fixture>"
            PS_DATA_PROVIDER_FIXTURE_PATH="$<TARGET_FILE:data_provider_fixture>"
            PS_DATA_PROVIDER_BAD_FIXTURE_PATH="$<TARGET_FILE:data_provider_bad_fixture>")

  add_test(
    NAME test_installed_consumer
    COMMAND
      ${CMAKE_COMMAND}
      -DPHOTOSPIDER_SOURCE_DIR=${CMAKE_CURRENT_SOURCE_DIR}
      -DPHOTOSPIDER_BINARY_DIR=${CMAKE_CURRENT_BINARY_DIR}
      -DPHOTOSPIDER_BUILD_CONFIG=$<CONFIG>
      -P ${CMAKE_CURRENT_SOURCE_DIR}/cmake/RunInstalledConsumer.cmake)
endif()
