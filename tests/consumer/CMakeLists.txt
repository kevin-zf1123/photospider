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

foreach(_old_minor IN ITEMS 0.23 0.24 0.25 0.26 0.27 0.28)
  find_package(Photospider ${_old_minor} CONFIG QUIET COMPONENTS kernel)
  if(Photospider_FOUND)
    message(FATAL_ERROR "Photospider 0.29 must reject a ${_old_minor} consumer")
  endif()
endforeach()

find_package(
  Photospider 0.29 CONFIG REQUIRED
  COMPONENTS kernel operation_sdk data_provider_sdk)

foreach(
  _photospider_cxx17_target
  IN ITEMS Photospider::kernel Photospider::operation_sdk)
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
if(TARGET Photospider::data_definition_sdk)
  message(
    FATAL_ERROR
      "Photospider must not export the nonexistent data_definition_sdk target")
endif()

add_library(photospider_sdk_consumer OBJECT sdk_smoke.c)
target_link_libraries(
  photospider_sdk_consumer
  PRIVATE Photospider::operation_sdk Photospider::data_provider_sdk)
target_compile_options(
  photospider_sdk_consumer PRIVATE ${_photospider_consumer_compile_options})
set_target_properties(
  photospider_sdk_consumer
  PROPERTIES C_STANDARD 11
             C_STANDARD_REQUIRED ON
             C_EXTENSIONS OFF)

# Both source packages consume only the installed public targets above.
add_subdirectory(../../examples/foundations_workflow foundations_workflow)
target_compile_options(photospider_foundations_workflow PRIVATE ${_photospider_consumer_compile_options})
target_link_options(photospider_foundations_workflow PRIVATE ${_photospider_consumer_link_options})

add_library(photospider_consumer_bridge SHARED pipeline_bridge.cpp)
target_link_libraries(
  photospider_consumer_bridge PRIVATE Photospider::kernel)
target_compile_options(
  photospider_consumer_bridge PRIVATE ${_photospider_consumer_compile_options})
target_link_options(
  photospider_consumer_bridge PRIVATE ${_photospider_consumer_link_options})
set_target_properties(
  photospider_consumer_bridge
  PROPERTIES CXX_STANDARD 14
             CXX_STANDARD_REQUIRED ON
             CXX_EXTENSIONS OFF
             WINDOWS_EXPORT_ALL_SYMBOLS ON)

add_executable(
  photospider_consumer
  main.cpp
  $<TARGET_OBJECTS:photospider_sdk_consumer>)
target_link_libraries(
  photospider_consumer
  PRIVATE photospider_consumer_bridge Photospider::operation_sdk
          Photospider::data_provider_sdk)
target_compile_options(
  photospider_consumer PRIVATE ${_photospider_consumer_compile_options})
target_link_options(
  photospider_consumer PRIVATE ${_photospider_consumer_link_options})
set_target_properties(
  photospider_consumer
  PROPERTIES CXX_STANDARD 14
             CXX_STANDARD_REQUIRED ON
             CXX_EXTENSIONS OFF)

add_library(photospider_contract_fixture MODULE ../fixtures/operation_contract_fixture.c)
target_link_libraries(photospider_contract_fixture PRIVATE Photospider::operation_sdk)
set_target_properties(photospider_contract_fixture PROPERTIES C_STANDARD 11 C_STANDARD_REQUIRED ON)
add_executable(photospider_semantic_consumer ../unit/test_semantic_contract.cpp)
target_include_directories(photospider_semantic_consumer PRIVATE ..)
target_link_libraries(photospider_semantic_consumer PRIVATE Photospider::kernel)
target_compile_definitions(photospider_semantic_consumer PRIVATE PS_CONTRACT_FIXTURE="$<TARGET_FILE:photospider_contract_fixture>")
target_compile_options(photospider_semantic_consumer PRIVATE ${_photospider_consumer_compile_options})
target_link_options(photospider_semantic_consumer PRIVATE ${_photospider_consumer_link_options})
add_dependencies(photospider_semantic_consumer photospider_contract_fixture)

add_library(photospider_expression_contract MODULE ../fixtures/expression_contract_fixture.c)
target_link_libraries(photospider_expression_contract PRIVATE Photospider::operation_sdk)
set_target_properties(photospider_expression_contract PROPERTIES C_STANDARD 11 C_STANDARD_REQUIRED ON)
add_executable(photospider_expression_consumer ../integration/test_expression_operations.cpp)
target_include_directories(photospider_expression_consumer PRIVATE ..)
target_link_libraries(photospider_expression_consumer PRIVATE Photospider::kernel)
target_compile_definitions(photospider_expression_consumer PRIVATE PS_EXPRESSION_CONTRACT_FIXTURE="$<TARGET_FILE:photospider_expression_contract>")
target_compile_options(photospider_expression_consumer PRIVATE ${_photospider_consumer_compile_options})
target_link_options(photospider_expression_consumer PRIVATE ${_photospider_consumer_link_options})
add_dependencies(photospider_expression_consumer photospider_expression_contract)

add_executable(photospider_basic_consumer ../integration/test_basic_operations.cpp)
target_include_directories(photospider_basic_consumer PRIVATE ../../examples)
target_link_libraries(photospider_basic_consumer PRIVATE Photospider::kernel)
target_compile_options(photospider_basic_consumer PRIVATE ${_photospider_consumer_compile_options})
target_link_options(photospider_basic_consumer PRIVATE ${_photospider_consumer_link_options})

# Public structural-image DAG through the installed C++ package.

add_library(photospider_dependency_fixture MODULE ../fixtures/dependency_fixture.c)
target_link_libraries(photospider_dependency_fixture PRIVATE Photospider::operation_sdk)
if(UNIX)
  target_link_libraries(photospider_dependency_fixture PRIVATE m)
endif()
target_compile_definitions(photospider_dependency_fixture PRIVATE PHOTOSPIDER_OPERATION_PLUGIN_BUILD)
set_target_properties(photospider_dependency_fixture PROPERTIES C_STANDARD 11 C_STANDARD_REQUIRED ON)
add_executable(photospider_dependency_consumer ../integration/test_dependency_plugin.cpp)
target_include_directories(photospider_dependency_consumer PRIVATE ..)
target_link_libraries(photospider_dependency_consumer PRIVATE Photospider::kernel ${CMAKE_DL_LIBS})
target_compile_definitions(photospider_dependency_consumer PRIVATE PS_DEPENDENCY_FIXTURE="$<TARGET_FILE:photospider_dependency_fixture>")
target_compile_options(photospider_dependency_consumer PRIVATE ${_photospider_consumer_compile_options})
target_link_options(photospider_dependency_consumer PRIVATE ${_photospider_consumer_link_options})
add_dependencies(photospider_dependency_consumer photospider_dependency_fixture)

add_executable(photospider_execution_dependencies_consumer ../integration/test_execution_dependencies.cpp)
target_include_directories(photospider_execution_dependencies_consumer PRIVATE ..)
target_link_libraries(photospider_execution_dependencies_consumer PRIVATE Photospider::kernel)
target_compile_options(photospider_execution_dependencies_consumer PRIVATE ${_photospider_consumer_compile_options})
target_link_options(photospider_execution_dependencies_consumer PRIVATE ${_photospider_consumer_link_options})

add_executable(photospider_execution_demand_consumer ../integration/test_execution_demand.cpp)
target_include_directories(photospider_execution_demand_consumer PRIVATE ..)
target_link_libraries(photospider_execution_demand_consumer PRIVATE Photospider::kernel)
target_compile_options(photospider_execution_demand_consumer PRIVATE ${_photospider_consumer_compile_options})
target_link_options(photospider_execution_demand_consumer PRIVATE ${_photospider_consumer_link_options})

add_executable(photospider_dependency_gpu_consumer ../unit/test_dependency_gpu.cpp)
target_include_directories(photospider_dependency_gpu_consumer PRIVATE ..)
target_link_libraries(photospider_dependency_gpu_consumer PRIVATE Photospider::kernel)
target_compile_options(photospider_dependency_gpu_consumer PRIVATE ${_photospider_consumer_compile_options})
target_link_options(photospider_dependency_gpu_consumer PRIVATE ${_photospider_consumer_link_options})

add_executable(photospider_gpu_discovery_consumer ../unit/test_gpu_discovery.cpp)
target_include_directories(photospider_gpu_discovery_consumer PRIVATE ..)
target_link_libraries(photospider_gpu_discovery_consumer PRIVATE Photospider::kernel)
target_compile_options(photospider_gpu_discovery_consumer PRIVATE ${_photospider_consumer_compile_options})
target_link_options(photospider_gpu_discovery_consumer PRIVATE ${_photospider_consumer_link_options})
add_library(gpu_discovery_fixture MODULE ../../examples/g4_gpu_workflow/discovery_plugin.c)
target_link_libraries(gpu_discovery_fixture PRIVATE Photospider::operation_sdk)
target_compile_definitions(gpu_discovery_fixture PRIVATE PHOTOSPIDER_OPERATION_PLUGIN_BUILD)
set_target_properties(gpu_discovery_fixture PROPERTIES C_STANDARD 11 C_STANDARD_REQUIRED ON)
add_executable(test_gpu_discovery_workflow ../../examples/g4_gpu_workflow/discovery_main.cpp)
target_link_libraries(test_gpu_discovery_workflow PRIVATE Photospider::kernel)
target_compile_options(test_gpu_discovery_workflow PRIVATE ${_photospider_consumer_compile_options})
target_link_options(test_gpu_discovery_workflow PRIVATE ${_photospider_consumer_link_options})
target_compile_definitions(test_gpu_discovery_workflow PRIVATE PS_GPU_DISCOVERY_PLUGIN="$<TARGET_FILE:gpu_discovery_fixture>")
add_dependencies(test_gpu_discovery_workflow gpu_discovery_fixture)

add_library(gpu_dependency_c_fixture MODULE ../../examples/g4_gpu_workflow/c_plugin.c)
target_link_libraries(gpu_dependency_c_fixture PRIVATE Photospider::operation_sdk)
target_compile_definitions(gpu_dependency_c_fixture PRIVATE PHOTOSPIDER_OPERATION_PLUGIN_BUILD)
set_target_properties(gpu_dependency_c_fixture PROPERTIES C_STANDARD 11 C_STANDARD_REQUIRED ON)
add_executable(test_gpu_c_abi_execution ../../examples/g4_gpu_workflow/c_main.cpp)
target_link_libraries(test_gpu_c_abi_execution PRIVATE Photospider::kernel)
target_compile_options(test_gpu_c_abi_execution PRIVATE ${_photospider_consumer_compile_options})
target_link_options(test_gpu_c_abi_execution PRIVATE ${_photospider_consumer_link_options})
target_compile_definitions(test_gpu_c_abi_execution PRIVATE PS_GPU_DEPENDENCY_PLUGIN="$<TARGET_FILE:gpu_dependency_c_fixture>")
add_dependencies(test_gpu_c_abi_execution gpu_dependency_c_fixture)

add_executable(test_gpu_sync_fallback ../../examples/g4_gpu_workflow/sync_main.cpp)
target_link_libraries(test_gpu_sync_fallback PRIVATE Photospider::kernel)
target_compile_options(test_gpu_sync_fallback PRIVATE ${_photospider_consumer_compile_options})
target_link_options(test_gpu_sync_fallback PRIVATE ${_photospider_consumer_link_options})

add_executable(test_gpu_fragment_execution ../../examples/g4_gpu_workflow/main.cpp)
target_link_libraries(test_gpu_fragment_execution PRIVATE Photospider::kernel)
target_compile_options(test_gpu_fragment_execution PRIVATE ${_photospider_consumer_compile_options})
target_link_options(test_gpu_fragment_execution PRIVATE ${_photospider_consumer_link_options})

add_executable(photospider_fragment_atlas_consumer ../unit/test_fragment_atlas.cpp)
target_include_directories(photospider_fragment_atlas_consumer PRIVATE ..)
target_link_libraries(photospider_fragment_atlas_consumer PRIVATE Photospider::kernel)
target_compile_options(photospider_fragment_atlas_consumer PRIVATE ${_photospider_consumer_compile_options})
target_link_options(photospider_fragment_atlas_consumer PRIVATE ${_photospider_consumer_link_options})

add_executable(photospider_ordered_scan_consumer ../integration/test_ordered_scan.cpp)
target_include_directories(photospider_ordered_scan_consumer PRIVATE ..)
target_link_libraries(photospider_ordered_scan_consumer PRIVATE Photospider::kernel)
target_compile_options(photospider_ordered_scan_consumer PRIVATE ${_photospider_consumer_compile_options})
target_link_options(photospider_ordered_scan_consumer PRIVATE ${_photospider_consumer_link_options})

add_executable(photospider_ordered_reduction_consumer ../integration/test_ordered_reduction.cpp)
target_include_directories(photospider_ordered_reduction_consumer PRIVATE ..)
target_link_libraries(photospider_ordered_reduction_consumer PRIVATE Photospider::kernel)
target_compile_options(photospider_ordered_reduction_consumer PRIVATE ${_photospider_consumer_compile_options})
target_link_options(photospider_ordered_reduction_consumer PRIVATE ${_photospider_consumer_link_options})

add_subdirectory(../../examples/multi_output_workflow multi-output)
target_compile_options(photospider_multi_output_workflow PRIVATE ${_photospider_consumer_compile_options})
target_link_options(photospider_multi_output_workflow PRIVATE ${_photospider_consumer_link_options})

add_custom_target(
  run_photospider_consumer
  COMMAND "$<TARGET_FILE:photospider_consumer>"
  COMMAND "$<TARGET_FILE:photospider_dependency_consumer>"
  COMMAND "$<TARGET_FILE:photospider_execution_dependencies_consumer>"
  COMMAND "$<TARGET_FILE:photospider_execution_demand_consumer>"
  COMMAND "$<TARGET_FILE:photospider_gpu_discovery_consumer>"
  COMMAND "$<TARGET_FILE:photospider_dependency_gpu_consumer>"
  COMMAND "$<TARGET_FILE:photospider_fragment_atlas_consumer>"
  COMMAND "$<TARGET_FILE:photospider_ordered_scan_consumer>"
  COMMAND "$<TARGET_FILE:photospider_ordered_reduction_consumer>"
  COMMAND "$<TARGET_FILE:photospider_model_metadata_consumer>"
  COMMAND "$<TARGET_FILE:photospider_expression_consumer>"
  COMMAND "$<TARGET_FILE:photospider_basic_consumer>"
  COMMAND "$<TARGET_FILE:photospider_semantic_consumer>"
  DEPENDS test_gpu_sync_fallback test_gpu_discovery_workflow photospider_gpu_discovery_consumer test_gpu_c_abi_execution test_gpu_fragment_execution photospider_dependency_gpu_consumer photospider_fragment_atlas_consumer photospider_ordered_scan_consumer photospider_ordered_reduction_consumer photospider_execution_demand_consumer photospider_execution_dependencies_consumer photospider_dependency_consumer photospider_basic_consumer photospider_foundations_workflow photospider_expression_consumer photospider_semantic_consumer photospider_consumer
  USES_TERMINAL
  VERBATIM)

# Focused installed managed-resource consumer; only installed kernel headers.
add_executable(photospider_resource_consumer ../unit/test_resources.cpp)
target_include_directories(photospider_resource_consumer PRIVATE ..)
target_link_libraries(photospider_resource_consumer PRIVATE Photospider::kernel)
target_compile_options(photospider_resource_consumer PRIVATE ${_photospider_consumer_compile_options})
target_link_options(photospider_resource_consumer PRIVATE ${_photospider_consumer_link_options})

# Public staged ResultRef DAG against the installed package only.
add_executable(photospider_result_consumer ../integration/test_result_execution.cpp)
target_include_directories(photospider_result_consumer PRIVATE ..)
target_link_libraries(photospider_result_consumer PRIVATE Photospider::kernel)
target_compile_options(photospider_result_consumer PRIVATE ${_photospider_consumer_compile_options})
target_link_options(photospider_result_consumer PRIVATE ${_photospider_consumer_link_options})

add_subdirectory(../../examples/representations_workflow representations_workflow)

# Format/color family gates use only installed public kernel headers.
foreach(_format_kind IN ITEMS model_metadata)
  add_executable(photospider_${_format_kind}_consumer
    ../integration/test_${_format_kind}.cpp)
  target_include_directories(photospider_${_format_kind}_consumer PRIVATE ..)
  target_link_libraries(photospider_${_format_kind}_consumer PRIVATE Photospider::kernel)
  target_compile_options(photospider_${_format_kind}_consumer PRIVATE ${_photospider_consumer_compile_options})
  target_link_options(photospider_${_format_kind}_consumer PRIVATE ${_photospider_consumer_link_options})
  add_dependencies(run_photospider_consumer photospider_${_format_kind}_consumer)
endforeach()

add_executable(photospider_scheduler_consumer ../../examples/dynamic_scheduler/main.cpp)
target_link_libraries(photospider_scheduler_consumer PRIVATE Photospider::kernel)
target_compile_features(photospider_scheduler_consumer PRIVATE cxx_std_17)
target_compile_options(photospider_scheduler_consumer PRIVATE ${_photospider_consumer_compile_options})
target_link_options(photospider_scheduler_consumer PRIVATE ${_photospider_consumer_link_options})

# Unified Result workflow and C11 DSO consume only the installed kernel/SDK.
add_subdirectory(../../examples/unified_result_workflow unified_result_workflow)
add_executable(photospider_unified_result_consumer ../integration/test_unified_result_images.cpp)
target_include_directories(photospider_unified_result_consumer PRIVATE ..)
target_link_libraries(photospider_unified_result_consumer PRIVATE Photospider::kernel)
add_executable(photospider_result_contract_consumer ../integration/test_result_image_contracts.cpp)
target_link_libraries(photospider_result_contract_consumer PRIVATE Photospider::kernel)
add_library(result_consumer_fixture MODULE ../fixtures/result_operation_fixture.c)
target_link_libraries(result_consumer_fixture PRIVATE Photospider::operation_sdk)
set_target_properties(result_consumer_fixture PROPERTIES C_STANDARD 11 C_STANDARD_REQUIRED ON)
add_executable(photospider_result_c_consumer ../integration/test_result_plugin.cpp)
target_link_libraries(photospider_result_c_consumer PRIVATE Photospider::kernel)
target_compile_definitions(photospider_result_c_consumer PRIVATE PS_RESULT_FIXTURE="$<TARGET_FILE:result_consumer_fixture>")
add_dependencies(photospider_result_c_consumer result_consumer_fixture)
foreach(_case RANGE 1 6)
  add_library(result_consumer_bad_${_case} MODULE ../fixtures/result_operation_fixture.c)
  target_link_libraries(result_consumer_bad_${_case} PRIVATE Photospider::operation_sdk)
  set_target_properties(result_consumer_bad_${_case} PROPERTIES C_STANDARD 11 C_STANDARD_REQUIRED ON)
  target_compile_definitions(result_consumer_bad_${_case} PRIVATE PS_BAD_RESULT_CASE=${_case})
  target_compile_definitions(photospider_result_c_consumer PRIVATE PS_BAD_RESULT_${_case}="$<TARGET_FILE:result_consumer_bad_${_case}>")
  add_dependencies(photospider_result_c_consumer result_consumer_bad_${_case})
endforeach()
enable_testing()
add_test(NAME installed_unified_result_workflow COMMAND photospider_unified_result_workflow)
add_test(NAME installed_unified_result_cpp COMMAND photospider_unified_result_consumer)
add_test(NAME installed_unified_result_contracts COMMAND photospider_result_contract_consumer)
add_test(NAME installed_unified_result_c11 COMMAND photospider_result_c_consumer)
add_test(NAME installed_unified_result_native_gpu COMMAND photospider_result_c_consumer --gpu-only)
set_tests_properties(installed_unified_result_native_gpu PROPERTIES SKIP_RETURN_CODE 77)
add_custom_target(run_unified_result_consumer
  COMMAND ${CMAKE_CTEST_COMMAND} --output-on-failure -R "^installed_unified_result_"
  DEPENDS photospider_unified_result_workflow photospider_unified_result_consumer photospider_result_contract_consumer photospider_result_c_consumer)
