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
  if(NOT CMAKE_C_COMPILER_ID MATCHES "Clang" OR
     NOT CMAKE_CXX_COMPILER_ID MATCHES "Clang")
    message(FATAL_ERROR "Sanitized consumer requires Clang C/C++")
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
  Photospider 0.33 CONFIG REQUIRED
  COMPONENTS kernel ops_headers operation_sdk data_provider_sdk)


enable_testing()
include(${CMAKE_CURRENT_LIST_DIR}/../../cmake/BehaviorTests.cmake)
function(photospider_add_consumer target)
  add_executable(${target} ${ARGN})
  target_link_libraries(${target} PRIVATE Photospider::kernel Photospider::ops_headers)
  target_include_directories(${target} PRIVATE ${CMAKE_CURRENT_LIST_DIR}/..)
endfunction()
function(photospider_add_installed_test)
  cmake_parse_arguments(PS "" "NAME;TARGET;TIMEOUT;SKIP_RETURN_CODE" "COMMAND;SOURCES;FIXTURES;DEPENDENCIES;LABELS" ${ARGN})
  if(PS_SOURCES)
    photospider_add_consumer(${PS_TARGET} ${PS_SOURCES})
  endif()
  set(_core)
  if(PS_NAME MATCHES "^installed_(package_exports|result_cpp_minimal|unified_result_c11|result_repeated|expression_contract|result_backend_admission|result_execution|execution_demand|execution_dependencies|semantic_contract|resources|gpu_sync_fallback)$")
    set(_core CORE)
  endif()
  photospider_register_test(NAME ${PS_NAME} TARGET ${PS_TARGET}
    COMMAND ${PS_COMMAND} FIXTURES ${PS_FIXTURES} DEPENDENCIES ${PS_DEPENDENCIES}
    LABELS ${PS_LABELS} TIMEOUT "${PS_TIMEOUT}"
    SKIP_RETURN_CODE "${PS_SKIP_RETURN_CODE}" ${_core})
endfunction()
foreach(
  _photospider_cxx17_target
  IN ITEMS Photospider::kernel Photospider::ops_headers Photospider::operation_sdk)
  get_target_property(
    _photospider_compile_features
    ${_photospider_cxx17_target}
    INTERFACE_COMPILE_FEATURES)
  if(NOT "cxx_std_17" IN_LIST _photospider_compile_features)
    message(
      FATAL_ERROR
        "${_photospider_cxx17_target} must propagate cxx_std_17; "
        "found '${_photospider_compile_features}'")
  endif()
endforeach()

get_target_property(
  _photospider_provider_compile_features
  Photospider::data_provider_sdk
  INTERFACE_COMPILE_FEATURES)
foreach(_photospider_provider_feature IN LISTS
                                             _photospider_provider_compile_features)
  if(_photospider_provider_feature MATCHES "^cxx_")
    message(
      FATAL_ERROR
        "Photospider::data_provider_sdk must remain pure C; found "
        "'${_photospider_provider_feature}'")
  endif()
endforeach()
add_library(photospider_sdk_consumer OBJECT sdk_smoke.c)

target_link_libraries(
  photospider_sdk_consumer
  PRIVATE Photospider::operation_sdk Photospider::data_provider_sdk)

set_target_properties(
  photospider_sdk_consumer
  PROPERTIES C_STANDARD 11
             C_STANDARD_REQUIRED ON
             C_EXTENSIONS OFF)

photospider_add_result_table_fixture(photospider_repeated_c_fixture ../fixtures/result_repeated_fixture.c)

photospider_add_installed_test(NAME installed_result_repeated TARGET photospider_repeated_result_consumer
  SOURCES ../integration/test_result_repeated.cpp
  COMMAND photospider_repeated_result_consumer)

target_link_libraries(photospider_repeated_result_consumer PRIVATE Photospider::kernel ${CMAKE_DL_LIBS})

target_compile_definitions(photospider_repeated_result_consumer PRIVATE PS_REPEATED_FIXTURE="$<TARGET_FILE:photospider_repeated_c_fixture>")


photospider_add_installed_test(NAME installed_layer_values TARGET photospider_layer_values_consumer
  SOURCES ../unit/test_layer.cpp
  COMMAND photospider_layer_values_consumer)


photospider_add_installed_test(NAME installed_result_c_checkpoint TARGET photospider_result_c_checkpoint_consumer
  SOURCES ../integration/test_result_c_checkpoint.cpp
  COMMAND photospider_result_c_checkpoint_consumer)

add_library(photospider_result_checkpoint_fixture MODULE ../fixtures/result_checkpoint_fixture.c)

target_link_libraries(photospider_result_checkpoint_fixture PRIVATE Photospider::operation_sdk)

set_target_properties(photospider_result_checkpoint_fixture PROPERTIES C_STANDARD 11 C_STANDARD_REQUIRED ON C_EXTENSIONS OFF)

target_compile_definitions(photospider_result_c_checkpoint_consumer PRIVATE PS_RESULT_CHECKPOINT_FIXTURE="$<TARGET_FILE:photospider_result_checkpoint_fixture>")


photospider_add_installed_test(NAME installed_result_c_block TARGET photospider_result_c_block_consumer
  SOURCES ../integration/test_result_c_block.cpp
  COMMAND photospider_result_c_block_consumer)

add_library(photospider_result_block_fixture MODULE ../fixtures/result_block_fixture.c)

target_link_libraries(photospider_result_block_fixture PRIVATE Photospider::operation_sdk)

if(UNIX)
  target_link_libraries(photospider_result_block_fixture PRIVATE m)
endif()

target_compile_options(photospider_result_block_fixture PRIVATE -fno-fast-math -frounding-math -ffp-contract=off)

set_target_properties(photospider_result_block_fixture PROPERTIES C_STANDARD 11 C_STANDARD_REQUIRED ON C_EXTENSIONS OFF)

target_compile_definitions(photospider_result_c_block_consumer PRIVATE PS_RESULT_BLOCK_FIXTURE="$<TARGET_FILE:photospider_result_block_fixture>")


photospider_add_installed_test(NAME installed_result_queries TARGET photospider_result_queries_consumer
  SOURCES ../integration/test_result_queries.cpp
  COMMAND photospider_result_queries_consumer)


photospider_add_installed_test(NAME installed_result_exceptions TARGET photospider_result_exceptions_consumer
  SOURCES ../integration/test_result_exceptions.cpp
  COMMAND photospider_result_exceptions_consumer)


photospider_add_installed_test(NAME installed_result_backend_admission TARGET photospider_backend_admission_consumer
  SOURCES ../integration/test_backend_admission.cpp
  COMMAND photospider_backend_admission_consumer)


photospider_add_installed_test(NAME installed_numeric_prepared TARGET photospider_numeric_prepared_consumer
  SOURCES ../../examples/numeric_workflow/prepared.cpp
  COMMAND photospider_numeric_prepared_consumer)


photospider_add_installed_test(NAME installed_result_c_atoms TARGET photospider_result_c_atoms_consumer
  SOURCES ../integration/test_result_c_atoms.cpp
  COMMAND photospider_result_c_atoms_consumer)

add_library(photospider_result_atoms_fixture MODULE ../fixtures/result_atoms_fixture.c)

target_link_libraries(photospider_result_atoms_fixture PRIVATE Photospider::operation_sdk)

set_target_properties(photospider_result_atoms_fixture PROPERTIES C_STANDARD 11 C_STANDARD_REQUIRED ON C_EXTENSIONS OFF)

target_compile_definitions(photospider_result_c_atoms_consumer PRIVATE PS_RESULT_ATOMS_FIXTURE="$<TARGET_FILE:photospider_result_atoms_fixture>")

add_library(photospider_result_atoms_wide_fixture MODULE ../fixtures/result_atoms_fixture.c)

target_link_libraries(photospider_result_atoms_wide_fixture PRIVATE Photospider::operation_sdk)

target_compile_definitions(photospider_result_atoms_wide_fixture PRIVATE PS_RESULT_ATOMS_WIDTH=64)

set_target_properties(photospider_result_atoms_wide_fixture PROPERTIES C_STANDARD 11 C_STANDARD_REQUIRED ON C_EXTENSIONS OFF)

target_compile_definitions(photospider_result_c_atoms_consumer PRIVATE PS_RESULT_ATOMS_WIDE_FIXTURE="$<TARGET_FILE:photospider_result_atoms_wide_fixture>")


photospider_add_installed_test(NAME installed_result_c_joint TARGET photospider_result_c_joint_consumer
  SOURCES ../integration/test_result_c_joint.cpp
  COMMAND photospider_result_c_joint_consumer)

photospider_add_result_table_fixture(photospider_result_joint_fixture ../fixtures/result_joint_fixture.c)

target_compile_definitions(photospider_result_c_joint_consumer PRIVATE PS_RESULT_JOINT_FIXTURE="$<TARGET_FILE:photospider_result_joint_fixture>")

add_library(photospider_result_joint_throw_fixture MODULE ../fixtures/result_joint_throw_fixture.cpp)

target_link_libraries(photospider_result_joint_throw_fixture PRIVATE Photospider::operation_sdk Threads::Threads)

target_compile_features(photospider_result_joint_throw_fixture PRIVATE cxx_std_17)

target_compile_definitions(photospider_result_c_joint_consumer PRIVATE PS_RESULT_JOINT_THROW_FIXTURE="$<TARGET_FILE:photospider_result_joint_throw_fixture>")


photospider_add_installed_test(NAME installed_result_joint TARGET photospider_result_joint_consumer
  SOURCES ../integration/test_result_joint.cpp
  COMMAND photospider_result_joint_consumer)


photospider_add_installed_test(NAME installed_result_request_record TARGET photospider_request_record_consumer
  SOURCES ../integration/test_result_request_record.cpp
  COMMAND photospider_request_record_consumer)


photospider_add_result_table_fixture(photospider_request_record_fixture ../fixtures/result_request_record_fixture.c)

target_compile_definitions(photospider_request_record_consumer PRIVATE PS_REQUEST_RECORD_FIXTURE="$<TARGET_FILE:photospider_request_record_fixture>")

photospider_add_result_table_fixture(photospider_bad_request_record_fixture ../fixtures/result_request_record_fixture.c BAD)
target_compile_definitions(photospider_request_record_consumer PRIVATE PS_BAD_REQUEST_RECORD_FIXTURE=\"$<TARGET_FILE:photospider_bad_request_record_fixture>\")

add_library(photospider_consumer_bridge SHARED pipeline_bridge.cpp)

target_link_libraries(
  photospider_consumer_bridge PRIVATE Photospider::kernel)

set_target_properties(
  photospider_consumer_bridge
  PROPERTIES CXX_STANDARD 14
             CXX_STANDARD_REQUIRED ON
             CXX_EXTENSIONS OFF
             WINDOWS_EXPORT_ALL_SYMBOLS ON)

photospider_add_installed_test(NAME installed_package_exports TARGET photospider_consumer
  SOURCES main.cpp
  $<TARGET_OBJECTS:photospider_sdk_consumer>
  COMMAND photospider_consumer)

target_link_libraries(
  photospider_consumer
  PRIVATE photospider_consumer_bridge Photospider::operation_sdk
          Photospider::data_provider_sdk)

set_target_properties(
  photospider_consumer
  PROPERTIES CXX_STANDARD 14
             CXX_STANDARD_REQUIRED ON
             CXX_EXTENSIONS OFF)

add_library(photospider_contract_fixture MODULE ../fixtures/operation_contract_fixture.c)

target_link_libraries(photospider_contract_fixture PRIVATE Photospider::operation_sdk)

set_target_properties(photospider_contract_fixture PROPERTIES C_STANDARD 11 C_STANDARD_REQUIRED ON)

photospider_add_installed_test(NAME installed_semantic_contract TARGET photospider_semantic_consumer
  SOURCES ../unit/test_semantic_contract.cpp
  COMMAND photospider_semantic_consumer)

target_compile_definitions(photospider_semantic_consumer PRIVATE PS_CONTRACT_FIXTURE="$<TARGET_FILE:photospider_contract_fixture>")


add_library(photospider_expression_contract MODULE
  ../fixtures/expression_contract_fixture.c
  ../fixtures/expression_contract_metadata.cpp)
target_link_libraries(photospider_expression_contract PRIVATE Photospider::kernel)

set_target_properties(photospider_expression_contract PROPERTIES C_STANDARD 11 C_STANDARD_REQUIRED ON)

photospider_add_installed_test(NAME installed_expression_contract TARGET photospider_expression_consumer
  SOURCES ../integration/test_expression_operations.cpp
  COMMAND photospider_expression_consumer)

target_compile_definitions(photospider_expression_consumer PRIVATE PS_EXPRESSION_CONTRACT_FIXTURE="$<TARGET_FILE:photospider_expression_contract>")

photospider_add_installed_test(NAME installed_execution_dependencies TARGET photospider_execution_dependencies_consumer
  SOURCES ../integration/test_execution_dependencies.cpp
  COMMAND photospider_execution_dependencies_consumer)


photospider_add_installed_test(NAME installed_execution_demand TARGET photospider_execution_demand_consumer
  SOURCES ../integration/test_execution_demand.cpp
  COMMAND photospider_execution_demand_consumer)


photospider_add_installed_test(NAME installed_dependency_gpu SKIP_RETURN_CODE 77 TARGET photospider_dependency_gpu_consumer
  SOURCES ../unit/test_dependency_gpu.cpp
  COMMAND photospider_dependency_gpu_consumer)


photospider_add_installed_test(NAME installed_gpu_discovery SKIP_RETURN_CODE 77 TARGET photospider_gpu_discovery_consumer
  SOURCES ../unit/test_gpu_discovery.cpp
  COMMAND photospider_gpu_discovery_consumer)


add_library(gpu_discovery_fixture MODULE ../fixtures/gpu_result_discovery_plugin.c)

target_link_libraries(gpu_discovery_fixture PRIVATE Photospider::operation_sdk)

target_compile_definitions(gpu_discovery_fixture PRIVATE PHOTOSPIDER_OPERATION_PLUGIN_BUILD)

set_target_properties(gpu_discovery_fixture PROPERTIES C_STANDARD 11 C_STANDARD_REQUIRED ON)

photospider_add_installed_test(NAME installed_gpu_discovery_workflow SKIP_RETURN_CODE 77 TARGET test_gpu_discovery_workflow
  SOURCES ../integration/gpu/test_gpu_discovery_workflow.cpp
  COMMAND test_gpu_discovery_workflow)

target_compile_options(test_gpu_discovery_workflow PRIVATE ${_photospider_consumer_compile_options})

target_link_options(test_gpu_discovery_workflow PRIVATE ${_photospider_consumer_link_options})

target_compile_definitions(test_gpu_discovery_workflow PRIVATE PS_GPU_DISCOVERY_PLUGIN="$<TARGET_FILE:gpu_discovery_fixture>")

add_library(gpu_dependency_c_fixture MODULE ../fixtures/gpu_result_c_plugin.c)

target_link_libraries(gpu_dependency_c_fixture PRIVATE Photospider::operation_sdk)

target_compile_definitions(gpu_dependency_c_fixture PRIVATE PHOTOSPIDER_OPERATION_PLUGIN_BUILD)

set_target_properties(gpu_dependency_c_fixture PROPERTIES C_STANDARD 11 C_STANDARD_REQUIRED ON)

photospider_add_installed_test(NAME installed_gpu_c_abi_execution SKIP_RETURN_CODE 77 TARGET test_gpu_c_abi_execution
  SOURCES ../integration/gpu/test_gpu_c_abi_execution.cpp
  COMMAND test_gpu_c_abi_execution)

target_compile_options(test_gpu_c_abi_execution PRIVATE ${_photospider_consumer_compile_options})

target_link_options(test_gpu_c_abi_execution PRIVATE ${_photospider_consumer_link_options})

target_compile_definitions(test_gpu_c_abi_execution PRIVATE PS_GPU_DEPENDENCY_PLUGIN="$<TARGET_FILE:gpu_dependency_c_fixture>")

photospider_add_installed_test(NAME installed_gpu_sync_fallback SKIP_RETURN_CODE 77 TARGET test_gpu_sync_fallback
  SOURCES ../integration/gpu/test_gpu_sync_fallback.cpp
  COMMAND test_gpu_sync_fallback)

target_compile_options(test_gpu_sync_fallback PRIVATE ${_photospider_consumer_compile_options})

target_link_options(test_gpu_sync_fallback PRIVATE ${_photospider_consumer_link_options})

photospider_add_installed_test(NAME installed_gpu_fragment_execution SKIP_RETURN_CODE 77 TARGET test_gpu_fragment_execution
  SOURCES ../integration/gpu/test_gpu_fragment_execution.cpp
  COMMAND test_gpu_fragment_execution)

target_compile_options(test_gpu_fragment_execution PRIVATE ${_photospider_consumer_compile_options})

target_link_options(test_gpu_fragment_execution PRIVATE ${_photospider_consumer_link_options})

photospider_add_installed_test(NAME installed_fragment_atlas TARGET photospider_fragment_atlas_consumer
  SOURCES ../unit/test_fragment_atlas.cpp
  COMMAND photospider_fragment_atlas_consumer)

photospider_add_installed_test(NAME installed_resources TARGET photospider_resource_consumer
  SOURCES ../unit/test_resources.cpp
  COMMAND photospider_resource_consumer)


photospider_add_installed_test(NAME installed_result_execution TARGET photospider_result_consumer
  SOURCES ../integration/test_result_execution.cpp
  COMMAND photospider_result_consumer)


photospider_add_installed_test(NAME installed_result_diagnostics TARGET photospider_result_diagnostics_consumer
  SOURCES ../integration/test_result_diagnostics.cpp
  COMMAND photospider_result_diagnostics_consumer)


add_subdirectory(../../examples/unified_result_workflow unified_result_workflow)

photospider_add_installed_test(NAME installed_unified_result_cpp TARGET photospider_unified_result_consumer
  SOURCES ../integration/test_unified_result_images.cpp
  COMMAND photospider_unified_result_consumer)

photospider_add_installed_test(NAME installed_image_domain TARGET photospider_image_domain_consumer
  SOURCES ../integration/test_metal_images.cpp
  COMMAND photospider_image_domain_consumer)

target_include_directories(photospider_image_domain_consumer PRIVATE ../../examples)

photospider_add_installed_test(NAME installed_computed_scalar TARGET photospider_computed_scalar_consumer
  SOURCES ../integration/test_computed_scalar.cpp
  COMMAND photospider_computed_scalar_consumer)

target_include_directories(photospider_computed_scalar_consumer PRIVATE .. ../../examples)

photospider_add_installed_test(NAME installed_unified_result_contracts TARGET photospider_result_contract_consumer
  SOURCES ../integration/test_result_image_contracts.cpp
  COMMAND photospider_result_contract_consumer)

photospider_add_result_table_fixture(result_consumer_fixture ../fixtures/result_operation_fixture.c)

photospider_add_installed_test(NAME installed_unified_result_c11 TARGET photospider_result_c_consumer
  SOURCES ../integration/test_result_plugin.cpp
  COMMAND photospider_result_c_consumer)

photospider_add_installed_test(NAME installed_result_output_payload TARGET photospider_result_output_payload_consumer
  SOURCES ../../examples/numeric_workflow/arrays.cpp
  COMMAND photospider_result_output_payload_consumer)

target_compile_definitions(photospider_result_output_payload_consumer PRIVATE PS_RESULT_PAYLOAD_TEST)

target_compile_definitions(photospider_result_c_consumer PRIVATE PS_RESULT_FIXTURE="$<TARGET_FILE:result_consumer_fixture>")

photospider_add_result_table_fixture(result_consumer_bad_fixture ../fixtures/result_operation_fixture.c BAD)
target_compile_definitions(photospider_result_c_consumer PRIVATE PS_BAD_RESULT_FIXTURE=\"$<TARGET_FILE:result_consumer_bad_fixture>\")





photospider_add_installed_test(NAME installed_result_numeric TARGET photospider_result_numeric_consumer
  SOURCES result_numeric.cpp
  COMMAND photospider_result_numeric_consumer)


photospider_add_installed_test(NAME installed_unified_result_workflow COMMAND photospider_unified_result_workflow)




photospider_add_installed_test(NAME installed_image_domain_native SKIP_RETURN_CODE 77 COMMAND photospider_image_domain_consumer --native-only)





photospider_add_installed_test(NAME installed_unified_result_native_gpu SKIP_RETURN_CODE 77 COMMAND photospider_result_c_consumer --gpu-only)

photospider_add_installed_test(NAME installed_numeric_conversion TARGET photospider_numeric_conversion_consumer
  SOURCES ../integration/test_numeric_conversion.cpp
  COMMAND photospider_numeric_conversion_consumer)

target_include_directories(photospider_numeric_conversion_consumer PRIVATE ../../examples)


add_subdirectory(../../examples/result_quickstart result_quickstart)

photospider_add_installed_test(NAME installed_result_cpp_minimal COMMAND photospider_result_quickstart)


photospider_add_result_table_fixture(photospider_bad_joint_fixture ../fixtures/result_joint_fixture.c BAD)
target_compile_definitions(photospider_result_c_joint_consumer PRIVATE PS_BAD_JOINT_FIXTURE=\"$<TARGET_FILE:photospider_bad_joint_fixture>\")

photospider_add_result_table_fixture(photospider_bad_repeated_fixture ../fixtures/result_repeated_fixture.c BAD)
target_compile_definitions(photospider_repeated_result_consumer PRIVATE PS_BAD_REPEATED_FIXTURE=\"$<TARGET_FILE:photospider_bad_repeated_fixture>\")

# Apply the matching sanitizer to every local target, including DSO fixtures
# and public examples created by subdirectories. Exported flags are not assumed.
function(photospider_configure_consumer_directory directory)
  get_property(_targets DIRECTORY "${directory}" PROPERTY BUILDSYSTEM_TARGETS)
  foreach(_target IN LISTS _targets)
    get_target_property(_type ${_target} TYPE)
    if(_type STREQUAL "MODULE_LIBRARY")
      photospider_configure_test_module(${_target} "${PHOTOSPIDER_CONSUMER_SANITIZER}")
    elseif(NOT _type STREQUAL "UTILITY" AND NOT _type STREQUAL "INTERFACE_LIBRARY")
      target_compile_options(${_target} PRIVATE ${_photospider_consumer_compile_options})
      if(NOT _type STREQUAL "OBJECT_LIBRARY")
        target_link_options(${_target} PRIVATE ${_photospider_consumer_link_options})
      endif()
    endif()
  endforeach()
  get_property(_directories DIRECTORY "${directory}" PROPERTY SUBDIRECTORIES)
  foreach(_child IN LISTS _directories)
    photospider_configure_consumer_directory("${_child}")
  endforeach()
endfunction()
photospider_configure_consumer_directory("${CMAKE_CURRENT_SOURCE_DIR}")

# Gate membership and build dependencies derive from the same test declarations.
photospider_finalize_behavior_targets()
get_property(_core_targets GLOBAL PROPERTY PHOTOSPIDER_CORE_TARGETS)
get_property(_all_targets GLOBAL PROPERTY PHOTOSPIDER_INSTALLED_TARGETS)
list(REMOVE_DUPLICATES _core_targets)
list(REMOVE_DUPLICATES _all_targets)
add_custom_target(run_photospider_consumer
  COMMAND ${CMAKE_CTEST_COMMAND} --test-dir ${CMAKE_CURRENT_BINARY_DIR} --output-on-failure -L package_core
  DEPENDS ${_core_targets}
  USES_TERMINAL VERBATIM)
add_custom_target(run_photospider_consumer_all
  COMMAND ${CMAKE_CTEST_COMMAND} --test-dir ${CMAKE_CURRENT_BINARY_DIR} --output-on-failure -L installed
  DEPENDS ${_all_targets}
  USES_TERMINAL VERBATIM)
