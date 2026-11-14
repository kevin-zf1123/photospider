cmake_minimum_required(VERSION 3.21)

project(
  Photospider
  VERSION 0.40.0
  DESCRIPTION "Embeddable typed graph compiler and local execution kernel"
  LANGUAGES C CXX)

if(NOT CMAKE_C_COMPILER_ID MATCHES "Clang" OR
   NOT CMAKE_CXX_COMPILER_ID MATCHES "Clang")
  message(FATAL_ERROR "Photospider requires Clang for C and C++ builds")
endif()

include(CMakePackageConfigHelpers)
include(CTest)
include(cmake/BehaviorTests.cmake)
include(GNUInstallDirs)

option(PHOTOSPIDER_BUILD_COMPILER "Build compiler, planning providers and psc" ON)
option(PHOTOSPIDER_BUILD_EXECUTOR "Build execution providers and psx" ON)
option(PHOTOSPIDER_BUILD_TOOLS "Build psc compiler and psx executor" ON)
option(PHOTOSPIDER_BUILD_OPS "Build bundled operation implementations" OFF)

option(PHOTOSPIDER_ENABLE_METAL "Build the optional Apple Silicon Metal backend" ${APPLE})
if(PHOTOSPIDER_BUILD_EXECUTOR AND PHOTOSPIDER_ENABLE_METAL AND APPLE)
  enable_language(OBJCXX)
endif()
option(PHOTOSPIDER_ENABLE_VULKAN "Build the native Vulkan compute backend" OFF)
if(PHOTOSPIDER_BUILD_EXECUTOR AND PHOTOSPIDER_ENABLE_VULKAN)
  if(PHOTOSPIDER_BUILD_EXECUTOR AND PHOTOSPIDER_ENABLE_METAL AND APPLE)
    message(FATAL_ERROR "Select one native GPU backend per kernel build")
  endif()
  find_package(Vulkan REQUIRED)
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

include(cmake/KernelComponents.cmake)
# Preserve callback floating-environment and parameter-validation semantics.
set_source_files_properties(
  src/lib/data/tensor_validation.cpp
  sdk/operation/port_validation.cpp plugins/compiler/default/compiler.cpp
  plugins/compiler/default/frontend.cpp
  host/frontend_registry.cpp
  plugins/frontends/dag/dag.cpp sdk/program/ir_validation.cpp
  plugins/compiler/default/optimizer.cpp
  host/optimizer_registry.cpp
  plugins/optimizers/deduplicate/deduplicate.cpp
  sdk/program/plan_linking.cpp
  sdk/program/plan_validation.cpp
  sdk/program/plan_selection.cpp
  plugins/compiler/default/planner.cpp
  host/planner_registry.cpp
  plugins/planners/static_tiles/static_tiles.cpp
  sdk/program/identity.cpp sdk/program/artifact.cpp
  sdk/program/artifact_text.cpp
  sdk/operation/operation_registry.cpp PROPERTIES
  COMPILE_OPTIONS "-fno-fast-math;-frounding-math;-ffp-contract=off")
# Native GPU cache identities include the maintained implementation.
file(GLOB_RECURSE _cache_identity_inputs CONFIGURE_DEPENDS
  "${CMAKE_CURRENT_SOURCE_DIR}/include/*.hpp" "${CMAKE_CURRENT_SOURCE_DIR}/include/*.h"
  "${CMAKE_CURRENT_SOURCE_DIR}/src/lib/*.cpp" "${CMAKE_CURRENT_SOURCE_DIR}/src/lib/*.hpp"
  "${CMAKE_CURRENT_SOURCE_DIR}/sdk/*" "${CMAKE_CURRENT_SOURCE_DIR}/host/*"
  "${CMAKE_CURRENT_SOURCE_DIR}/plugins/executors/physical/*"
  "${CMAKE_CURRENT_SOURCE_DIR}/cmake/KernelComponents.cmake")
list(FILTER _cache_identity_inputs EXCLUDE REGEX "/include/photospider/ops/")
list(FILTER _cache_identity_inputs INCLUDE REGEX "[.](cpp|hpp|h|mm|cmake)$")
list(APPEND _cache_identity_inputs "${CMAKE_CURRENT_SOURCE_DIR}/CMakeLists.txt")
list(SORT _cache_identity_inputs)
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS ${_cache_identity_inputs})
string(TOUPPER "${CMAKE_BUILD_TYPE}" _cache_config)
set(_cache_identity "${CMAKE_CXX_COMPILER_ID};${CMAKE_CXX_COMPILER_VERSION};${CMAKE_SYSTEM_NAME};${CMAKE_SYSTEM_VERSION};${CMAKE_SYSTEM_PROCESSOR};${CMAKE_CXX_FLAGS};${CMAKE_CXX_FLAGS_${_cache_config}};${BUILD_SHARED_LIBS};${PHOTOSPIDER_SANITIZER_MODE};metal=${PHOTOSPIDER_ENABLE_METAL};vulkan=${PHOTOSPIDER_ENABLE_VULKAN}")
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
add_library(photospider_operation_sdk INTERFACE)
add_library(Photospider::operation_sdk ALIAS photospider_operation_sdk)
set_target_properties(photospider_operation_sdk PROPERTIES EXPORT_NAME operation_sdk)
target_include_directories(
  photospider_operation_sdk
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
    PRIVATE ${CMAKE_CURRENT_SOURCE_DIR} ${CMAKE_CURRENT_BINARY_DIR}/generated/$<CONFIG> ${CMAKE_CURRENT_BINARY_DIR}/generated)
  target_link_libraries(
    ${target} PUBLIC Threads::Threads PRIVATE ${CMAKE_DL_LIBS})
  target_compile_definitions(${target} PRIVATE PHOTOSPIDER_BUILD)
  if(CMAKE_CXX_COMPILER_ID MATCHES "Clang")
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

photospider_make_components()
include(cmake/OpsSdk.cmake)
if(TARGET Photospider::kernel)
  include(cmake/BenchmarkTools.cmake)
endif()
if(PHOTOSPIDER_BUILD_OPS)
  include(cmake/BundledOperations.cmake)
endif()

if(PHOTOSPIDER_BUILD_MANUAL_FUZZ_TARGETS)
  if(PHOTOSPIDER_ENABLE_TSAN)
    message(FATAL_ERROR "manual libFuzzer targets cannot be combined with TSAN")
  endif()
  # Keep fuzz instrumentation out of the installed kernel and test objects.
  add_library(photospider_fuzz_kernel STATIC EXCLUDE_FROM_ALL ${PHOTOSPIDER_KERNEL_SOURCES})
  photospider_configure_kernel_target(photospider_fuzz_kernel)
  target_include_directories(photospider_fuzz_kernel PRIVATE src/lib)
  photospider_configure_gpu(photospider_fuzz_kernel)
  target_compile_definitions(photospider_fuzz_kernel PUBLIC PHOTOSPIDER_STATIC)
  target_compile_options(photospider_fuzz_kernel PRIVATE -fsanitize=fuzzer-no-link -fno-omit-frame-pointer)
  add_executable(
    photospider_operation_contract_ir_fuzz EXCLUDE_FROM_ALL
    tests/fuzz/operation_contract_ir_fuzz.cpp)
  target_link_libraries(
    photospider_operation_contract_ir_fuzz PRIVATE photospider_fuzz_kernel)
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
  TARGETS photospider_operation_sdk ${PHOTOSPIDER_COMPONENT_TARGETS}
  EXPORT PhotospiderTargets
  ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
  LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
  RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
  INCLUDES DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})
install(DIRECTORY include/photospider DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}
  FILES_MATCHING PATTERN "*.hpp" PATTERN "*.h" PATTERN "ops" EXCLUDE PATTERN "benchmark" EXCLUDE)
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

if(BUILD_TESTING AND TARGET Photospider::kernel)
  add_library(photospider_internal_kernel STATIC EXCLUDE_FROM_ALL ${PHOTOSPIDER_KERNEL_SOURCES})
  photospider_configure_kernel_target(photospider_internal_kernel)
  target_include_directories(photospider_internal_kernel PRIVATE src/lib)
  photospider_configure_gpu(photospider_internal_kernel)
  target_compile_definitions(photospider_internal_kernel PUBLIC PHOTOSPIDER_STATIC)
  add_library(photospider_test_kernel STATIC EXCLUDE_FROM_ALL ${PHOTOSPIDER_KERNEL_SOURCES}
    plugins/executors/physical/execution_test_hooks.cpp sdk/operation/library_test_hooks.cpp)
  photospider_configure_kernel_target(photospider_test_kernel)
  target_include_directories(photospider_test_kernel PRIVATE src/lib)
  photospider_configure_gpu(photospider_test_kernel)
  target_compile_definitions(photospider_test_kernel PUBLIC PHOTOSPIDER_STATIC
    PRIVATE PHOTOSPIDER_ENABLE_EXECUTION_TEST_HOOKS PHOTOSPIDER_ENABLE_LIBRARY_TEST_HOOKS)

  function(photospider_add_kernel_test target kernel_target)
    add_executable(${target} ${ARGN})
    target_link_libraries(${target} PRIVATE ${kernel_target})
    target_include_directories(${target} PRIVATE tests ${CMAKE_CURRENT_SOURCE_DIR})
    target_compile_features(${target} PRIVATE cxx_std_17)
    if(CMAKE_CXX_COMPILER_ID MATCHES "Clang")
      target_compile_options(${target} PRIVATE -Wall -Wextra -Wpedantic -Werror)
    endif()
    if("${ARGN}" MATCHES "tests/unit/kernel/")
      set(_level unit)
    else()
      set(_level integration)
    endif()
    photospider_register_test(NAME ${target} COMMAND ${target}
      SUITE kernel LEVEL ${_level})
  endfunction()

  include(cmake/KernelTests.cmake)
  include(cmake/PackageTests.cmake)
  photospider_finalize_behavior_targets()
  get_property(_behavior_targets GLOBAL PROPERTY PHOTOSPIDER_BEHAVIOR_TARGETS)
  list(REMOVE_DUPLICATES _behavior_targets)
  add_custom_target(photospider_kernel_tests DEPENDS ${_behavior_targets})
  add_custom_target(photospider_tests ALL DEPENDS photospider_kernel_tests)
  # Tests that call private symbols cannot link the hidden shared package.
  # Their private include paths declare that boundary explicitly.
  get_property(_test_targets DIRECTORY PROPERTY BUILDSYSTEM_TARGETS)
  foreach(_target IN LISTS _test_targets)
    get_target_property(_type ${_target} TYPE)
    if(_type STREQUAL "MODULE_LIBRARY")
      photospider_configure_test_module(${_target} "${PHOTOSPIDER_SANITIZER_MODE}")
    endif()
    if(NOT _type STREQUAL "EXECUTABLE")
      continue()
    endif()
    get_target_property(_includes ${_target} INCLUDE_DIRECTORIES)
    get_target_property(_links ${_target} LINK_LIBRARIES)
    if(_includes MATCHES "${CMAKE_CURRENT_SOURCE_DIR}/src/lib(;|$)" AND
       "Photospider::kernel" IN_LIST _links)
      list(REMOVE_ITEM _links Photospider::kernel)
      list(APPEND _links photospider_internal_kernel)
      set_property(TARGET ${_target} PROPERTY LINK_LIBRARIES ${_links})
    endif()
  endforeach()
endif()


install(FILES third_party/IK_LLAMA_LICENSE.txt
  DESTINATION ${CMAKE_INSTALL_DATADIR}/licenses/Photospider)

include(cmake/CompilerTools.cmake)
