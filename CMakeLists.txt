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
option(
  PHOTOSPIDER_BUILD_MANUAL_FUZZ_TARGETS
  "Build explicit long-lived libFuzzer harnesses outside default build/CTest"
  OFF)

if(PHOTOSPIDER_ENABLE_ASAN AND PHOTOSPIDER_ENABLE_TSAN)
  message(FATAL_ERROR "ASAN and TSAN cannot be enabled together")
endif()

find_package(Threads REQUIRED)

set(
  PHOTOSPIDER_KERNEL_SOURCES
  src/lib/benchmark/raw_benchmark.cpp
  src/lib/compiler/compiler.cpp
  src/lib/data/region.cpp
  src/lib/data/value.cpp
  src/lib/execution/execution.cpp
  src/lib/graph/graph_context.cpp
  src/lib/plugin/data_definition_registry.cpp
  src/lib/plugin/operation_registry.cpp
  src/lib/plugin/utf8_validation.cpp)
add_library(photospider ${PHOTOSPIDER_KERNEL_SOURCES})
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

function(photospider_configure_kernel_target target)
  set_target_properties(
    ${target}
    PROPERTIES CXX_EXTENSIONS OFF
               CXX_VISIBILITY_PRESET hidden
               VISIBILITY_INLINES_HIDDEN YES)
  target_compile_features(${target} PUBLIC cxx_std_17)
  target_include_directories(
    ${target}
    PUBLIC $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
           $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>
    PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src/lib)
  target_link_libraries(
    ${target} PUBLIC Threads::Threads PRIVATE ${CMAKE_DL_LIBS})
  target_compile_definitions(${target} PRIVATE PHOTOSPIDER_BUILD)

  if(CMAKE_CXX_COMPILER_ID MATCHES "Clang|GNU")
    target_compile_options(${target} PRIVATE -Wall -Wextra -Wpedantic -Werror)
    if(PHOTOSPIDER_ENABLE_ASAN)
      target_compile_options(
        ${target} PUBLIC -fsanitize=address -fno-omit-frame-pointer)
      target_link_options(${target} PUBLIC -fsanitize=address)
    elseif(PHOTOSPIDER_ENABLE_TSAN)
      target_compile_options(
        ${target} PUBLIC -fsanitize=thread -fno-omit-frame-pointer)
      target_link_options(${target} PUBLIC -fsanitize=thread)
    endif()
  endif()
endfunction()

photospider_configure_kernel_target(photospider)
set_target_properties(photospider PROPERTIES EXPORT_NAME kernel)
if(NOT BUILD_SHARED_LIBS)
  target_compile_definitions(photospider PUBLIC PHOTOSPIDER_STATIC)
endif()

if(PHOTOSPIDER_BUILD_MANUAL_FUZZ_TARGETS)
  if(NOT CMAKE_CXX_COMPILER_ID MATCHES "Clang")
    message(FATAL_ERROR "manual fuzz targets require a Clang libFuzzer runtime")
  endif()
  if(PHOTOSPIDER_ENABLE_TSAN)
    message(FATAL_ERROR "manual libFuzzer targets cannot be combined with TSAN")
  endif()
  target_compile_options(
    photospider PRIVATE -fsanitize=fuzzer-no-link -fno-omit-frame-pointer)
  target_link_options(photospider INTERFACE -fsanitize=fuzzer-no-link)
  add_executable(
    photospider_operation_contract_ir_fuzz EXCLUDE_FROM_ALL
    tests/fuzz/operation_contract_ir_fuzz.cpp)
  target_link_libraries(
    photospider_operation_contract_ir_fuzz PRIVATE Photospider::kernel)
  target_compile_features(photospider_operation_contract_ir_fuzz
                          PRIVATE cxx_std_17)
  target_compile_options(
    photospider_operation_contract_ir_fuzz
    PRIVATE -fsanitize=fuzzer -fno-omit-frame-pointer -Wall -Wextra -Wpedantic
            -Werror)
  target_include_directories(photospider_operation_contract_ir_fuzz
                             PRIVATE tests)
  target_link_options(photospider_operation_contract_ir_fuzz
                      PRIVATE -fsanitize=fuzzer)
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
  add_library(
    photospider_test_kernel STATIC EXCLUDE_FROM_ALL
    ${PHOTOSPIDER_KERNEL_SOURCES}
    src/lib/benchmark/raw_benchmark_test_hooks.cpp
    src/lib/plugin/library_test_hooks.cpp)
  photospider_configure_kernel_target(photospider_test_kernel)
  target_compile_definitions(
    photospider_test_kernel
    PUBLIC PHOTOSPIDER_STATIC
    PRIVATE PHOTOSPIDER_ENABLE_LIBRARY_TEST_HOOKS
            PHOTOSPIDER_ENABLE_RAW_BENCHMARK_TEST_HOOKS)

  function(photospider_add_test target kernel_target)
    add_executable(${target} ${ARGN})
    target_link_libraries(${target} PRIVATE ${kernel_target})
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

  function(photospider_add_bad_parameter_fixture target case_number)
    add_library(
      ${target} MODULE
      tests/fixtures/operation_bad_parameter_fixture.cpp)
    target_link_libraries(${target} PRIVATE Photospider::operation_sdk)
    target_compile_definitions(
      ${target}
      PRIVATE PHOTOSPIDER_OPERATION_PLUGIN_BUILD
              PS_BAD_PARAMETER_CASE=${case_number})
    set_target_properties(${target} PROPERTIES PREFIX "")
  endfunction()
  photospider_add_bad_parameter_fixture(operation_bad_parameter_pointer_fixture 1)
  photospider_add_bad_parameter_fixture(operation_bad_parameter_size_fixture 2)
  photospider_add_bad_parameter_fixture(operation_bad_parameter_count_fixture 3)
  photospider_add_bad_parameter_fixture(operation_bad_parameter_bounds_fixture 4)
  photospider_add_bad_parameter_fixture(operation_bad_parameter_alignment_fixture 5)

  function(photospider_add_operation_utf8_fixture target case_number)
    add_library(${target} MODULE tests/fixtures/operation_utf8_fixture.cpp)
    target_link_libraries(${target} PRIVATE Photospider::operation_sdk)
    target_compile_definitions(
      ${target}
      PRIVATE PHOTOSPIDER_OPERATION_PLUGIN_BUILD
              PS_OPERATION_UTF8_CASE=${case_number})
    set_target_properties(${target} PROPERTIES PREFIX "")
  endfunction()
  photospider_add_operation_utf8_fixture(
    operation_invalid_utf8_overlong_fixture 1)
  photospider_add_operation_utf8_fixture(
    operation_invalid_utf8_truncated_fixture 2)
  photospider_add_operation_utf8_fixture(
    operation_invalid_utf8_surrogate_fixture 3)
  photospider_add_operation_utf8_fixture(
    operation_invalid_utf8_too_large_fixture 4)
  photospider_add_operation_utf8_fixture(
    operation_invalid_utf8_continuation_fixture 5)
  photospider_add_operation_utf8_fixture(operation_unicode_fixture 6)

  add_library(data_provider_fixture MODULE tests/fixtures/data_provider_fixture.cpp)
  target_link_libraries(data_provider_fixture PRIVATE Photospider::data_provider_sdk)
  target_compile_definitions(data_provider_fixture PRIVATE PHOTOSPIDER_DATA_PROVIDER_BUILD)
  set_target_properties(data_provider_fixture PROPERTIES PREFIX "")

  add_library(data_provider_bad_fixture MODULE tests/fixtures/data_provider_bad_fixture.cpp)
  target_link_libraries(data_provider_bad_fixture PRIVATE Photospider::data_provider_sdk)
  target_compile_definitions(data_provider_bad_fixture PRIVATE PHOTOSPIDER_DATA_PROVIDER_BUILD)
  set_target_properties(data_provider_bad_fixture PROPERTIES PREFIX "")

  function(photospider_add_provider_utf8_fixture target case_number)
    add_library(${target} MODULE tests/fixtures/data_provider_utf8_fixture.cpp)
    target_link_libraries(${target} PRIVATE Photospider::data_provider_sdk)
    target_compile_definitions(
      ${target}
      PRIVATE PHOTOSPIDER_DATA_PROVIDER_BUILD
              PS_PROVIDER_UTF8_CASE=${case_number})
    set_target_properties(${target} PROPERTIES PREFIX "")
  endfunction()
  photospider_add_provider_utf8_fixture(
    data_provider_invalid_utf8_overlong_fixture 1)
  photospider_add_provider_utf8_fixture(data_provider_unicode_fixture 2)

  photospider_add_test(test_value Photospider::kernel tests/unit/test_value.cpp)
  photospider_add_test(
    test_compiler Photospider::kernel tests/unit/test_compiler.cpp)
  photospider_add_test(
    test_operation_contract_ir_seeds
    Photospider::kernel
    tests/unit/test_operation_contract_ir_seeds.cpp
    tests/fuzz/operation_contract_ir_fuzz.cpp)
  target_compile_definitions(
    test_operation_contract_ir_seeds
    PRIVATE PS_OPERATION_VALID_SEED_PATH="${CMAKE_CURRENT_SOURCE_DIR}/tests/fuzz/corpus/operation_contract_ir/valid-source"
            PS_OPERATION_DUPLICATE_SEED_PATH="${CMAKE_CURRENT_SOURCE_DIR}/tests/fuzz/corpus/operation_contract_ir/malformed-schema")
  photospider_add_test(
    test_execution photospider_test_kernel
    tests/integration/test_execution.cpp)
  target_include_directories(test_execution PRIVATE src/lib)
  add_dependencies(test_execution operation_fixture)
  target_compile_definitions(
    test_execution
    PRIVATE PS_OPERATION_FIXTURE_PATH="$<TARGET_FILE:operation_fixture>")
  photospider_add_test(
    test_plugin_registry photospider_test_kernel
    tests/integration/test_plugin_registry.cpp)
  target_include_directories(test_plugin_registry PRIVATE src/lib)
  target_compile_definitions(test_plugin_registry
                             PRIVATE PHOTOSPIDER_ENABLE_LIBRARY_TEST_HOOKS)
  add_dependencies(
    test_plugin_registry
    operation_fixture
    operation_bad_fixture
    operation_bad_parameter_pointer_fixture
    operation_bad_parameter_size_fixture
    operation_bad_parameter_count_fixture
    operation_bad_parameter_bounds_fixture
    operation_bad_parameter_alignment_fixture
    operation_invalid_utf8_overlong_fixture
    operation_invalid_utf8_truncated_fixture
    operation_invalid_utf8_surrogate_fixture
    operation_invalid_utf8_too_large_fixture
    operation_invalid_utf8_continuation_fixture
    operation_unicode_fixture
    data_provider_fixture
    data_provider_bad_fixture
    data_provider_invalid_utf8_overlong_fixture
    data_provider_unicode_fixture)
  target_compile_definitions(
    test_plugin_registry
    PRIVATE PS_OPERATION_FIXTURE_PATH="$<TARGET_FILE:operation_fixture>"
            PS_OPERATION_BAD_FIXTURE_PATH="$<TARGET_FILE:operation_bad_fixture>"
            PS_OPERATION_BAD_PARAMETER_POINTER_FIXTURE_PATH="$<TARGET_FILE:operation_bad_parameter_pointer_fixture>"
            PS_OPERATION_BAD_PARAMETER_SIZE_FIXTURE_PATH="$<TARGET_FILE:operation_bad_parameter_size_fixture>"
            PS_OPERATION_BAD_PARAMETER_COUNT_FIXTURE_PATH="$<TARGET_FILE:operation_bad_parameter_count_fixture>"
            PS_OPERATION_BAD_PARAMETER_BOUNDS_FIXTURE_PATH="$<TARGET_FILE:operation_bad_parameter_bounds_fixture>"
            PS_OPERATION_BAD_PARAMETER_ALIGNMENT_FIXTURE_PATH="$<TARGET_FILE:operation_bad_parameter_alignment_fixture>"
            PS_OPERATION_INVALID_UTF8_OVERLONG_FIXTURE_PATH="$<TARGET_FILE:operation_invalid_utf8_overlong_fixture>"
            PS_OPERATION_INVALID_UTF8_TRUNCATED_FIXTURE_PATH="$<TARGET_FILE:operation_invalid_utf8_truncated_fixture>"
            PS_OPERATION_INVALID_UTF8_SURROGATE_FIXTURE_PATH="$<TARGET_FILE:operation_invalid_utf8_surrogate_fixture>"
            PS_OPERATION_INVALID_UTF8_TOO_LARGE_FIXTURE_PATH="$<TARGET_FILE:operation_invalid_utf8_too_large_fixture>"
            PS_OPERATION_INVALID_UTF8_CONTINUATION_FIXTURE_PATH="$<TARGET_FILE:operation_invalid_utf8_continuation_fixture>"
            PS_OPERATION_UNICODE_FIXTURE_PATH="$<TARGET_FILE:operation_unicode_fixture>"
            PS_DATA_PROVIDER_FIXTURE_PATH="$<TARGET_FILE:data_provider_fixture>"
            PS_DATA_PROVIDER_BAD_FIXTURE_PATH="$<TARGET_FILE:data_provider_bad_fixture>"
            PS_DATA_PROVIDER_INVALID_UTF8_OVERLONG_FIXTURE_PATH="$<TARGET_FILE:data_provider_invalid_utf8_overlong_fixture>"
            PS_DATA_PROVIDER_UNICODE_FIXTURE_PATH="$<TARGET_FILE:data_provider_unicode_fixture>")

  add_test(
    NAME test_installed_consumer
    COMMAND
      ${CMAKE_COMMAND}
      -DPHOTOSPIDER_SOURCE_DIR=${CMAKE_CURRENT_SOURCE_DIR}
      -DPHOTOSPIDER_BINARY_DIR=${CMAKE_CURRENT_BINARY_DIR}
      -DPHOTOSPIDER_BUILD_CONFIG=$<CONFIG>
      -P ${CMAKE_CURRENT_SOURCE_DIR}/cmake/RunInstalledConsumer.cmake)
endif()
