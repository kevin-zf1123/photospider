cmake_minimum_required(VERSION 3.21)

project(
  Photospider
  VERSION 0.7.0
  DESCRIPTION "Embeddable typed graph compiler and local execution kernel"
  LANGUAGES C CXX)

include(CMakePackageConfigHelpers)
include(CTest)
include(GNUInstallDirs)

option(PHOTOSPIDER_ENABLE_METAL "Build the optional Apple Silicon Metal backend" ${APPLE})
if(PHOTOSPIDER_ENABLE_METAL AND APPLE)
  enable_language(OBJCXX)
endif()

option(PHOTOSPIDER_ENABLE_ASAN "Enable AddressSanitizer for local validation" OFF)
option(PHOTOSPIDER_ENABLE_TSAN "Enable ThreadSanitizer for local validation" OFF)
option(
  PHOTOSPIDER_BUILD_MANUAL_FUZZ_TARGETS
  "Build explicit long-lived libFuzzer harnesses outside default build/CTest"
  OFF)

if(PHOTOSPIDER_ENABLE_ASAN AND PHOTOSPIDER_ENABLE_TSAN)
  message(FATAL_ERROR "ASAN and TSAN cannot be enabled together")
endif()

set(PHOTOSPIDER_SANITIZER_MODE none)
set(PHOTOSPIDER_SANITIZER_FLAG "")
if(PHOTOSPIDER_ENABLE_ASAN OR PHOTOSPIDER_ENABLE_TSAN)
  if(NOT CMAKE_CXX_COMPILER_ID MATCHES "Clang|GNU")
    message(FATAL_ERROR "Photospider sanitizers require Clang or GNU C++")
  endif()
  include(CheckCXXCompilerFlag)
  include(CheckLinkerFlag)
  if(PHOTOSPIDER_ENABLE_ASAN)
    set(PHOTOSPIDER_SANITIZER_MODE address)
    set(PHOTOSPIDER_SANITIZER_FLAG -fsanitize=address)
  else()
    set(PHOTOSPIDER_SANITIZER_MODE thread)
    set(PHOTOSPIDER_SANITIZER_FLAG -fsanitize=thread)
  endif()
  string(TOUPPER "${PHOTOSPIDER_SANITIZER_MODE}"
         _photospider_sanitizer_mode_upper)
  set(_photospider_cxx_sanitizer_check
      "PHOTOSPIDER_CXX_${_photospider_sanitizer_mode_upper}_SANITIZER_SUPPORTED")
  set(_photospider_cxx_sanitizer_link_check
      "PHOTOSPIDER_CXX_${_photospider_sanitizer_mode_upper}_SANITIZER_LINK_SUPPORTED")
  set(CMAKE_REQUIRED_LINK_OPTIONS "${PHOTOSPIDER_SANITIZER_FLAG}")
  check_cxx_compiler_flag(
    "${PHOTOSPIDER_SANITIZER_FLAG}"
    ${_photospider_cxx_sanitizer_check})
  unset(CMAKE_REQUIRED_LINK_OPTIONS)
  check_cxx_compiler_flag(
    "-fno-omit-frame-pointer"
    PHOTOSPIDER_CXX_FRAME_POINTER_FLAG_SUPPORTED)
  check_linker_flag(
    CXX "${PHOTOSPIDER_SANITIZER_FLAG}"
    ${_photospider_cxx_sanitizer_link_check})
  if(NOT ${_photospider_cxx_sanitizer_check} OR
     NOT PHOTOSPIDER_CXX_FRAME_POINTER_FLAG_SUPPORTED OR
     NOT ${_photospider_cxx_sanitizer_link_check})
    message(
      FATAL_ERROR
        "Requested ${PHOTOSPIDER_SANITIZER_MODE} sanitizer is unsupported")
  endif()
endif()

find_package(Threads REQUIRED)

set(
  PHOTOSPIDER_KERNEL_SOURCES
  plugins/ops/image_operations.cpp
  src/lib/benchmark/raw_benchmark.cpp
  src/lib/compiler/compiler.cpp
  src/lib/data/region.cpp
  src/lib/data/storage.cpp
  src/lib/data/value.cpp
  src/lib/data/semantic.cpp
  src/lib/plugin/operation_contract.cpp
  src/lib/data/input_validation.cpp
  src/lib/data/input_snapshot.cpp
  src/lib/execution/execution.cpp
  src/lib/execution/native_gpu.cpp
  src/lib/graph/graph_context.cpp
  src/lib/plugin/data_definition_registry.cpp
  src/lib/plugin/operation_registry.cpp
  src/lib/plugin/utf8_validation.cpp)
if(PHOTOSPIDER_ENABLE_METAL AND APPLE)
  list(APPEND PHOTOSPIDER_KERNEL_SOURCES src/lib/execution/native_gpu_metal.mm)
  set_source_files_properties(src/lib/execution/native_gpu_metal.mm PROPERTIES
    COMPILE_OPTIONS "-fobjc-arc")
endif()
# S1 binary32 contracts require nearest rounding and gradual underflow at runtime.
if(CMAKE_CXX_COMPILER_ID MATCHES "Clang|GNU")
  set_source_files_properties(plugins/ops/image_operations.cpp
    src/lib/data/input_validation.cpp src/lib/compiler/compiler.cpp
    src/lib/plugin/operation_registry.cpp PROPERTIES
    COMPILE_OPTIONS "-fno-fast-math;-frounding-math;-ffp-contract=off")
elseif(MSVC)
  set_source_files_properties(plugins/ops/image_operations.cpp
    src/lib/data/input_validation.cpp src/lib/compiler/compiler.cpp
    src/lib/plugin/operation_registry.cpp PROPERTIES COMPILE_OPTIONS "/fp:strict")
endif()
# Disposable disk entries are compatible only with this maintained implementation.
file(GLOB_RECURSE _cache_identity_inputs CONFIGURE_DEPENDS
  "${CMAKE_CURRENT_SOURCE_DIR}/include/*.hpp" "${CMAKE_CURRENT_SOURCE_DIR}/include/*.h"
  "${CMAKE_CURRENT_SOURCE_DIR}/src/lib/*.cpp" "${CMAKE_CURRENT_SOURCE_DIR}/src/lib/*.hpp"
  "${CMAKE_CURRENT_SOURCE_DIR}/src/lib/*.mm" "${CMAKE_CURRENT_SOURCE_DIR}/plugins/ops/*.h"
  "${CMAKE_CURRENT_SOURCE_DIR}/plugins/ops/*.metal" "${CMAKE_CURRENT_SOURCE_DIR}/plugins/ops/*.cpp")
list(APPEND _cache_identity_inputs "${CMAKE_CURRENT_SOURCE_DIR}/CMakeLists.txt")
list(SORT _cache_identity_inputs)
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS ${_cache_identity_inputs})
string(TOUPPER "${CMAKE_BUILD_TYPE}" _cache_config)
set(_cache_identity "${CMAKE_CXX_COMPILER_ID};${CMAKE_CXX_COMPILER_VERSION};${CMAKE_SYSTEM_NAME};${CMAKE_SYSTEM_VERSION};${CMAKE_SYSTEM_PROCESSOR};${CMAKE_CXX_FLAGS};${CMAKE_CXX_FLAGS_${_cache_config}};${BUILD_SHARED_LIBS};${PHOTOSPIDER_SANITIZER_MODE}")
foreach(_cache_input IN LISTS _cache_identity_inputs)
  file(SHA256 "${_cache_input}" _cache_source_hash)
  file(RELATIVE_PATH _cache_relative "${CMAKE_CURRENT_SOURCE_DIR}" "${_cache_input}")
  string(APPEND _cache_identity ";${_cache_relative}:${_cache_source_hash}")
endforeach()
if(CMAKE_CONFIGURATION_TYPES)
  foreach(_configuration IN LISTS CMAKE_CONFIGURATION_TYPES)
    string(TOUPPER "${_configuration}" _configuration_upper)
    string(SHA256 PHOTOSPIDER_CACHE_BUILD_ID "${_cache_identity};${_configuration};${CMAKE_CXX_FLAGS_${_configuration_upper}}")
    configure_file(cmake/cache_build_identity.hpp.in "generated/${_configuration}/cache_build_identity.hpp" @ONLY)
  endforeach()
else()
  string(SHA256 PHOTOSPIDER_CACHE_BUILD_ID "${_cache_identity};${CMAKE_BUILD_TYPE}")
  configure_file(cmake/cache_build_identity.hpp.in "generated/${CMAKE_BUILD_TYPE}/cache_build_identity.hpp" @ONLY)
endif()
add_library(photospider ${PHOTOSPIDER_KERNEL_SOURCES})
add_library(Photospider::kernel ALIAS photospider)

add_library(photospider_operation_sdk INTERFACE)
add_library(Photospider::operation_sdk ALIAS photospider_operation_sdk)
set_target_properties(photospider_operation_sdk PROPERTIES EXPORT_NAME operation_sdk)
target_compile_features(photospider_operation_sdk INTERFACE cxx_std_17)
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
               POSITION_INDEPENDENT_CODE ON
               CXX_VISIBILITY_PRESET hidden
               VISIBILITY_INLINES_HIDDEN YES)
  target_compile_features(${target} PUBLIC cxx_std_17)
  target_include_directories(
    ${target}
    PUBLIC $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
           $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>
    PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src/lib ${CMAKE_CURRENT_BINARY_DIR}/generated/$<CONFIG>
      ${CMAKE_CURRENT_BINARY_DIR}/plugins/ops/rgba32f)
  target_link_libraries(
    ${target} PUBLIC Threads::Threads PRIVATE ${CMAKE_DL_LIBS})
  target_compile_definitions(${target} PRIVATE PHOTOSPIDER_BUILD)
  if(PHOTOSPIDER_ENABLE_METAL AND APPLE)
    target_compile_definitions(${target} PRIVATE PHOTOSPIDER_HAS_METAL)
    target_link_libraries(${target} PRIVATE "-framework Metal" "-framework Foundation")
    set_target_properties(${target} PROPERTIES OBJCXX_STANDARD 17 OBJCXX_STANDARD_REQUIRED ON)
  endif()

  if(CMAKE_CXX_COMPILER_ID MATCHES "Clang|GNU")
    target_compile_options(${target} PRIVATE -Wall -Wextra -Wpedantic -Werror)
    if(NOT PHOTOSPIDER_SANITIZER_MODE STREQUAL "none")
      target_compile_options(
        ${target}
        PRIVATE ${PHOTOSPIDER_SANITIZER_FLAG} -fno-omit-frame-pointer
        INTERFACE
          "$<BUILD_INTERFACE:${PHOTOSPIDER_SANITIZER_FLAG}>"
          "$<BUILD_INTERFACE:-fno-omit-frame-pointer>")
      target_link_options(
        ${target}
        PRIVATE ${PHOTOSPIDER_SANITIZER_FLAG}
        INTERFACE "$<BUILD_INTERFACE:${PHOTOSPIDER_SANITIZER_FLAG}>")
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
  target_link_options(
    photospider INTERFACE "$<BUILD_INTERFACE:-fsanitize=fuzzer-no-link>")
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
install(DIRECTORY include/photospider DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}
  FILES_MATCHING PATTERN "*.hpp" PATTERN "*.h")
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

add_subdirectory(plugins/ops/rgba32f EXCLUDE_FROM_ALL)
add_subdirectory(examples/image_vertical EXCLUDE_FROM_ALL)
add_subdirectory(examples/regional_image_vertical EXCLUDE_FROM_ALL)
add_subdirectory(examples/s3_image_workflow EXCLUDE_FROM_ALL)
add_subdirectory(examples/s4_gpu_workflow EXCLUDE_FROM_ALL)

if(BUILD_TESTING)
  foreach(_scenario resident-chain all-operations cache-edits preview-export fallback)
    add_test(NAME test_s4_${_scenario} COMMAND photospider_s4_gpu_workflow --scenario ${_scenario} --backend metal --require-native)
    add_test(NAME test_s4_${_scenario}_plugin COMMAND photospider_s4_gpu_workflow --scenario ${_scenario} --backend metal --require-native --module $<TARGET_FILE:photospider_rgba32f_ops>)
    set_tests_properties(test_s4_${_scenario} test_s4_${_scenario}_plugin PROPERTIES SKIP_RETURN_CODE 77)
  endforeach()
  add_test(NAME test_s4_cpu COMMAND photospider_s4_gpu_workflow --backend cpu --layout roi --no-cache)
  add_test(NAME test_s3_cache COMMAND photospider_s3_image_workflow --scenario cache)
  add_test(NAME test_s3_cache_plugin COMMAND photospider_s3_image_workflow --scenario cache --module $<TARGET_FILE:photospider_rgba32f_ops>)
  add_test(NAME test_s3_preview COMMAND photospider_s3_image_workflow)
  add_test(NAME test_s3_preview_plugin COMMAND photospider_s3_image_workflow --module $<TARGET_FILE:photospider_rgba32f_ops>)
  add_test(NAME test_s2_vertical COMMAND photospider_regional_image_vertical)
  add_test(NAME test_s2_vertical_plugin COMMAND photospider_regional_image_vertical
    $<TARGET_FILE:photospider_rgba32f_ops>)
  add_test(NAME test_image_vertical COMMAND photospider_image_vertical)
  add_test(NAME test_image_vertical_plugin COMMAND photospider_image_vertical
    "$<TARGET_FILE:photospider_rgba32f_ops>")
  add_library(
    photospider_test_kernel STATIC EXCLUDE_FROM_ALL
    ${PHOTOSPIDER_KERNEL_SOURCES}
    src/lib/benchmark/raw_benchmark_test_hooks.cpp
    src/lib/execution/execution_test_hooks.cpp
    src/lib/plugin/library_test_hooks.cpp)
  photospider_configure_kernel_target(photospider_test_kernel)
  target_compile_definitions(
    photospider_test_kernel
    PUBLIC PHOTOSPIDER_STATIC
    PRIVATE PHOTOSPIDER_ENABLE_EXECUTION_TEST_HOOKS
            PHOTOSPIDER_ENABLE_LIBRARY_TEST_HOOKS
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

  add_library(
    operation_dense_overflow_fixture MODULE
    tests/fixtures/operation_dense_overflow_fixture.cpp)
  target_link_libraries(
    operation_dense_overflow_fixture PRIVATE Photospider::operation_sdk)
  target_compile_definitions(
    operation_dense_overflow_fixture PRIVATE PHOTOSPIDER_OPERATION_PLUGIN_BUILD)
  set_target_properties(operation_dense_overflow_fixture PROPERTIES PREFIX "")

  function(photospider_add_dense_limit_fixture target case_number)
    add_library(
      ${target} MODULE
      tests/fixtures/operation_dense_limit_fixture.cpp)
    target_link_libraries(${target} PRIVATE Photospider::operation_sdk)
    target_compile_definitions(
      ${target}
      PRIVATE PHOTOSPIDER_OPERATION_PLUGIN_BUILD
              PS_DENSE_LIMIT_CASE=${case_number})
    set_target_properties(${target} PROPERTIES PREFIX "")
  endfunction()
  photospider_add_dense_limit_fixture(operation_dense_rank1_limit_fixture 1)
  photospider_add_dense_limit_fixture(operation_dense_rank1_overflow_fixture 2)
  photospider_add_dense_limit_fixture(operation_dense_rank2_limit_fixture 3)
  photospider_add_dense_limit_fixture(operation_dense_rank2_overflow_fixture 4)
  photospider_add_dense_limit_fixture(operation_dense_multi_overflow_fixture 5)

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

  photospider_add_test(test_semantic_contract Photospider::kernel tests/unit/test_semantic_contract.cpp)
  add_library(operation_contract_fixture MODULE tests/fixtures/operation_contract_fixture.c)
  target_link_libraries(operation_contract_fixture PRIVATE Photospider::operation_sdk)
  set_target_properties(operation_contract_fixture PROPERTIES C_STANDARD 11 C_STANDARD_REQUIRED ON)
  target_compile_definitions(test_semantic_contract PRIVATE PS_CONTRACT_FIXTURE="$<TARGET_FILE:operation_contract_fixture>")
  add_dependencies(test_semantic_contract operation_contract_fixture)
  foreach(_case RANGE 1 7)
    add_library(operation_bad_contract_${_case} MODULE tests/fixtures/operation_contract_fixture.c)
    target_link_libraries(operation_bad_contract_${_case} PRIVATE Photospider::operation_sdk)
    set_target_properties(operation_bad_contract_${_case} PROPERTIES C_STANDARD 11 C_STANDARD_REQUIRED ON)
    target_compile_definitions(operation_bad_contract_${_case} PRIVATE PS_BAD_CONTRACT_CASE=${_case})
    target_compile_definitions(test_semantic_contract PRIVATE PS_BAD_CONTRACT_${_case}="$<TARGET_FILE:operation_bad_contract_${_case}>")
    add_dependencies(test_semantic_contract operation_bad_contract_${_case})
  endforeach()

  photospider_add_test(test_bindings Photospider::kernel tests/integration/test_bindings.cpp)
  target_include_directories(test_bindings PRIVATE examples)
  add_dependencies(test_bindings photospider_image_vertical photospider_regional_image_vertical)
  add_library(operation_bad_image_fixture MODULE tests/fixtures/operation_bad_image_fixture.c)
  target_link_libraries(operation_bad_image_fixture PRIVATE Photospider::operation_sdk)
  target_compile_definitions(operation_bad_image_fixture PRIVATE PHOTOSPIDER_OPERATION_PLUGIN_BUILD)
  add_library(data_provider_float32_fixture MODULE tests/fixtures/data_provider_float32_fixture.c)
  target_link_libraries(data_provider_float32_fixture PRIVATE Photospider::data_provider_sdk)
  target_compile_definitions(data_provider_float32_fixture PRIVATE PHOTOSPIDER_DATA_PROVIDER_BUILD)
  add_dependencies(test_bindings photospider_rgba32f_ops operation_bad_image_fixture data_provider_float32_fixture)
  target_compile_definitions(test_bindings PRIVATE
    PS_IMAGE_FIXTURE_PATH="$<TARGET_FILE:photospider_rgba32f_ops>"
    PS_BAD_IMAGE_FIXTURE_PATH="$<TARGET_FILE:operation_bad_image_fixture>"
    PS_FLOAT32_PROVIDER_PATH="$<TARGET_FILE:data_provider_float32_fixture>")
  foreach(_port_case RANGE 1 8)
    add_library(operation_bad_port_${_port_case} MODULE tests/fixtures/operation_bad_port_fixture.cpp)
    target_link_libraries(operation_bad_port_${_port_case} PRIVATE Photospider::operation_sdk)
    target_compile_definitions(operation_bad_port_${_port_case} PRIVATE PHOTOSPIDER_OPERATION_PLUGIN_BUILD PS_BAD_PORT_CASE=${_port_case})
    add_dependencies(test_bindings operation_bad_port_${_port_case})
    target_compile_definitions(test_bindings PRIVATE PS_BAD_PORT_${_port_case}="$<TARGET_FILE:operation_bad_port_${_port_case}>")
  endforeach()

  photospider_add_test(test_regional_execution Photospider::kernel tests/integration/test_regional_execution.cpp)
  target_include_directories(test_regional_execution PRIVATE examples)
  add_dependencies(test_regional_execution photospider_rgba32f_ops)
  target_compile_definitions(test_regional_execution PRIVATE PS_IMAGE_FIXTURE_PATH="$<TARGET_FILE:photospider_rgba32f_ops>")
  add_executable(test_disk_cache tests/integration/test_disk_cache.cpp)
  target_link_libraries(test_disk_cache PRIVATE Photospider::kernel)
  target_include_directories(test_disk_cache PRIVATE tests examples src/lib)
  add_test(NAME test_disk_cache COMMAND ${CMAKE_COMMAND}
    -DPHOTOSPIDER_BINARY_DIR=${CMAKE_CURRENT_BINARY_DIR}
    -DPHOTOSPIDER_DISK_TEST=$<TARGET_FILE:test_disk_cache>
    -P ${CMAKE_CURRENT_SOURCE_DIR}/cmake/RunDiskCache.cmake)
  photospider_add_test(test_s3_operations Photospider::kernel tests/integration/test_s3_operations.cpp)
  add_dependencies(test_s3_operations photospider_rgba32f_ops photospider_s3_image_workflow)
  add_test(NAME test_s3_operations_plugin COMMAND test_s3_operations $<TARGET_FILE:photospider_rgba32f_ops>)
  photospider_add_test(test_result_cache Photospider::kernel tests/integration/test_result_cache.cpp)
  target_include_directories(test_result_cache PRIVATE src/lib)
  photospider_add_test(test_frozen_execution Photospider::kernel tests/integration/test_frozen_execution.cpp)
  photospider_add_test(test_input_snapshot Photospider::kernel tests/unit/test_input_snapshot.cpp)
  target_include_directories(test_input_snapshot PRIVATE src/lib)
  photospider_add_test(test_scaled_regions Photospider::kernel tests/unit/test_scaled_regions.cpp)
  photospider_add_test(test_tile_plan Photospider::kernel tests/unit/test_tile_plan.cpp)
  photospider_add_test(test_native_cache Photospider::kernel tests/integration/test_native_cache.cpp)
  target_include_directories(test_native_cache PRIVATE examples)
  add_dependencies(test_native_cache photospider_rgba32f_ops)
  add_test(NAME test_native_cache_plugin COMMAND test_native_cache $<TARGET_FILE:photospider_rgba32f_ops>)
  set_tests_properties(test_native_cache test_native_cache_plugin PROPERTIES SKIP_RETURN_CODE 77)
  photospider_add_test(test_metal_images Photospider::kernel tests/integration/test_metal_images.cpp)
  target_include_directories(test_metal_images PRIVATE examples)
  add_dependencies(test_metal_images photospider_rgba32f_ops photospider_s4_gpu_workflow)
  add_test(NAME test_metal_images_plugin COMMAND test_metal_images $<TARGET_FILE:photospider_rgba32f_ops>)
  set_tests_properties(test_metal_images test_metal_images_plugin PROPERTIES SKIP_RETURN_CODE 77)
  photospider_add_test(test_native_execution Photospider::kernel tests/integration/test_native_execution.cpp)
  set_tests_properties(test_native_execution PROPERTIES SKIP_RETURN_CODE 77)
  photospider_add_test(test_native_gpu photospider_test_kernel tests/unit/test_native_gpu.cpp)
  target_include_directories(test_native_gpu PRIVATE src/lib)
  set_tests_properties(test_native_gpu PROPERTIES SKIP_RETURN_CODE 77)
  photospider_add_test(test_memory_liveness photospider_test_kernel tests/integration/test_memory_liveness.cpp)
  target_include_directories(test_memory_liveness PRIVATE src/lib)
  photospider_add_test(test_storage Photospider::kernel tests/unit/test_storage.cpp)
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
    operation_dense_overflow_fixture
    operation_dense_rank1_limit_fixture
    operation_dense_rank1_overflow_fixture
    operation_dense_rank2_limit_fixture
    operation_dense_rank2_overflow_fixture
    operation_dense_multi_overflow_fixture
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
            PS_OPERATION_DENSE_OVERFLOW_FIXTURE_PATH="$<TARGET_FILE:operation_dense_overflow_fixture>"
            PS_OPERATION_DENSE_RANK1_LIMIT_FIXTURE_PATH="$<TARGET_FILE:operation_dense_rank1_limit_fixture>"
            PS_OPERATION_DENSE_RANK1_OVERFLOW_FIXTURE_PATH="$<TARGET_FILE:operation_dense_rank1_overflow_fixture>"
            PS_OPERATION_DENSE_RANK2_LIMIT_FIXTURE_PATH="$<TARGET_FILE:operation_dense_rank2_limit_fixture>"
            PS_OPERATION_DENSE_RANK2_OVERFLOW_FIXTURE_PATH="$<TARGET_FILE:operation_dense_rank2_overflow_fixture>"
            PS_OPERATION_DENSE_MULTI_OVERFLOW_FIXTURE_PATH="$<TARGET_FILE:operation_dense_multi_overflow_fixture>"
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
      "-DPHOTOSPIDER_SOURCE_DIR:PATH=${CMAKE_CURRENT_SOURCE_DIR}"
      "-DPHOTOSPIDER_BINARY_DIR:PATH=${CMAKE_CURRENT_BINARY_DIR}"
      "-DPHOTOSPIDER_BUILD_CONFIG:STRING=$<CONFIG>"
      "-DPHOTOSPIDER_GENERATOR:STRING=${CMAKE_GENERATOR}"
      "-DPHOTOSPIDER_GENERATOR_PLATFORM:STRING=${CMAKE_GENERATOR_PLATFORM}"
      "-DPHOTOSPIDER_GENERATOR_TOOLSET:STRING=${CMAKE_GENERATOR_TOOLSET}"
      "-DPHOTOSPIDER_CONSUMER_SANITIZER:STRING=${PHOTOSPIDER_SANITIZER_MODE}"
      -P "${CMAKE_CURRENT_SOURCE_DIR}/cmake/RunInstalledConsumer.cmake")
endif()
