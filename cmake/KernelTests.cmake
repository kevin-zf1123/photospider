# Pure C producers exercise the public module ABI in the build tree.
add_library(fixed_contract_fixture MODULE tests/fixtures/fixed_contract_fixture.c)
target_link_libraries(fixed_contract_fixture PRIVATE Photospider::operation_sdk)
target_compile_definitions(fixed_contract_fixture PRIVATE PHOTOSPIDER_OPERATION_PLUGIN_BUILD)
target_compile_options(fixed_contract_fixture PRIVATE -Wall -Wextra -Wpedantic -Werror)
set_target_properties(fixed_contract_fixture PROPERTIES C_STANDARD 11 C_STANDARD_REQUIRED ON C_EXTENSIONS OFF)
photospider_add_kernel_test(test_fixed_contracts Photospider::kernel tests/integration/kernel/test_fixed_contracts.cpp)
target_compile_definitions(test_fixed_contracts PRIVATE PS_CONTRACT_MODULE="$<TARGET_FILE:fixed_contract_fixture>")
target_link_libraries(test_fixed_contracts PRIVATE ${CMAKE_DL_LIBS})
set_property(TEST test_fixed_contracts APPEND PROPERTY LABELS abi)

photospider_add_kernel_test(test_operation_module Photospider::kernel tests/integration/kernel/test_operation_module.cpp)
set_property(TEST test_operation_module APPEND PROPERTY LABELS abi)
target_link_libraries(test_operation_module PRIVATE ${CMAKE_DL_LIBS})
foreach(_case RANGE 0 5)
  set(_module fixed_module_fixture_${_case})
  add_library(${_module} MODULE tests/fixtures/fixed_module_fixture.c)
  target_link_libraries(${_module} PRIVATE Photospider::operation_sdk)
  target_compile_definitions(${_module} PRIVATE PHOTOSPIDER_OPERATION_PLUGIN_BUILD PS_MODULE_BAD=${_case})
  target_compile_options(${_module} PRIVATE -Wall -Wextra -Wpedantic -Werror)
  set_target_properties(${_module} PROPERTIES C_STANDARD 11 C_STANDARD_REQUIRED ON C_EXTENSIONS OFF)
  add_dependencies(test_operation_module ${_module})
  if(_case EQUAL 0)
    target_compile_definitions(test_operation_module PRIVATE PS_MODULE_FIXTURE="$<TARGET_FILE:${_module}>")
  else()
    target_compile_definitions(test_operation_module PRIVATE PS_MODULE_BAD_${_case}="$<TARGET_FILE:${_module}>")
  endif()
endforeach()

photospider_add_kernel_test(test_operation_program_limits Photospider::kernel tests/unit/kernel/test_operation_program_limits.cpp)
photospider_add_kernel_test(test_fixed_identity Photospider::kernel tests/integration/kernel/test_fixed_identity.cpp)
photospider_add_kernel_test(test_fixed_cfg Photospider::kernel tests/integration/kernel/test_fixed_cfg.cpp)
photospider_add_kernel_test(test_result_identity photospider_internal_kernel tests/unit/kernel/test_result_identity.cpp)
target_include_directories(test_result_identity PRIVATE src/lib)
photospider_add_kernel_test(test_fixed_execution Photospider::kernel tests/integration/kernel/test_fixed_execution.cpp)
photospider_add_kernel_test(test_run_work_v9 Photospider::kernel tests/integration/kernel/test_run_work_v9.cpp)
photospider_add_kernel_test(test_execution_observation photospider_internal_kernel tests/integration/kernel/test_execution_observation.cpp)
target_include_directories(test_execution_observation PRIVATE src/lib)
photospider_add_kernel_test(test_fixed_async Photospider::kernel tests/integration/kernel/test_fixed_async.cpp)
set_property(TEST test_fixed_async APPEND PROPERTY LABELS mock gpu)
photospider_add_kernel_test(test_block_memory photospider_internal_kernel tests/unit/kernel/test_block_memory.cpp)
target_include_directories(test_block_memory PRIVATE src/lib)
photospider_add_kernel_test(test_native_block_memory photospider_test_kernel tests/integration/kernel/test_native_block_memory.cpp)
target_include_directories(test_native_block_memory PRIVATE src/lib)
set_property(TEST test_native_block_memory APPEND PROPERTY LABELS native gpu)
set_property(TEST test_native_block_memory PROPERTY SKIP_RETURN_CODE 77)
photospider_add_kernel_test(test_device_result photospider_internal_kernel tests/integration/kernel/test_device_result.cpp)
target_include_directories(test_device_result PRIVATE src/lib)
set_property(TEST test_device_result APPEND PROPERTY LABELS native gpu)
set_property(TEST test_device_result PROPERTY SKIP_RETURN_CODE 77)
photospider_add_kernel_test(test_native_copy photospider_internal_kernel tests/integration/kernel/test_native_copy.cpp)
target_include_directories(test_native_copy PRIVATE src/lib)
set_property(TEST test_native_copy APPEND PROPERTY LABELS native gpu)
set_property(TEST test_native_copy PROPERTY SKIP_RETURN_CODE 77)
add_library(native_kernel_fixture MODULE tests/fixtures/native_kernel_fixture.c)
target_link_libraries(native_kernel_fixture PRIVATE Photospider::operation_sdk)
target_compile_definitions(native_kernel_fixture PRIVATE PHOTOSPIDER_OPERATION_PLUGIN_BUILD)
target_compile_options(native_kernel_fixture PRIVATE -Wall -Wextra -Wpedantic -Werror)
set_target_properties(native_kernel_fixture PROPERTIES C_STANDARD 11 C_STANDARD_REQUIRED ON C_EXTENSIONS OFF)
photospider_add_kernel_test(test_fixed_native photospider_internal_kernel tests/integration/kernel/test_fixed_native.cpp)
target_include_directories(test_fixed_native PRIVATE src/lib)
target_compile_definitions(test_fixed_native PRIVATE PS_NATIVE_KERNEL_FIXTURE="$<TARGET_FILE:native_kernel_fixture>")
add_dependencies(test_fixed_native native_kernel_fixture)
set_property(TEST test_fixed_native APPEND PROPERTY LABELS native gpu metal abi)
set_property(TEST test_fixed_native PROPERTY SKIP_RETURN_CODE 77)
# Kernel behavior tests register their own operations and require no bundled ops.
photospider_add_kernel_test(test_physical_program Photospider::kernel tests/unit/kernel/test_physical_program.cpp)
photospider_add_kernel_test(test_physical_compiler Photospider::kernel tests/unit/kernel/test_physical_compiler.cpp)
photospider_add_kernel_test(test_fixed_resources Photospider::kernel tests/unit/kernel/test_fixed_resources.cpp)
photospider_add_kernel_test(test_task_schedule photospider_internal_kernel tests/unit/kernel/test_task_schedule.cpp)
target_include_directories(test_task_schedule PRIVATE src/lib)
photospider_add_kernel_test(test_fair_admission photospider_internal_kernel tests/unit/kernel/test_fair_admission.cpp)
target_include_directories(test_fair_admission PRIVATE src/lib)
photospider_add_kernel_test(test_physical_cpu_schedule photospider_internal_kernel tests/integration/kernel/test_physical_cpu_schedule.cpp)
target_include_directories(test_physical_cpu_schedule PRIVATE src/lib)
photospider_add_kernel_test(test_static_compiler Photospider::kernel tests/unit/kernel/test_static_compiler.cpp)
photospider_add_kernel_test(test_result_publication Photospider::kernel tests/unit/kernel/test_result_publication.cpp)
photospider_add_kernel_test(test_tiles Photospider::kernel tests/unit/kernel/test_tiles.cpp)
photospider_add_kernel_test(test_operation_subgraphs Photospider::kernel tests/integration/kernel/test_operation_subgraphs.cpp)
photospider_add_kernel_test(test_result_copy_bits Photospider::kernel tests/integration/kernel/test_result_copy_bits.cpp)

add_library(module_tiles_fixture MODULE tests/fixtures/module_tiles_fixture.c)
target_link_libraries(module_tiles_fixture PRIVATE Photospider::operation_sdk)
target_compile_definitions(module_tiles_fixture PRIVATE PHOTOSPIDER_OPERATION_PLUGIN_BUILD)
target_compile_options(module_tiles_fixture PRIVATE -Wall -Wextra -Wpedantic -Werror)
set_target_properties(module_tiles_fixture PROPERTIES C_STANDARD 11 C_STANDARD_REQUIRED ON C_EXTENSIONS OFF)
photospider_add_kernel_test(test_module_tiles Photospider::kernel tests/integration/kernel/test_module_tiles.cpp)
target_compile_definitions(test_module_tiles PRIVATE PS_MODULE_TILES_FIXTURE="$<TARGET_FILE:module_tiles_fixture>")
target_link_libraries(test_module_tiles PRIVATE ${CMAKE_DL_LIBS})
add_dependencies(test_module_tiles module_tiles_fixture)

photospider_add_kernel_test(test_result_metadata_budget photospider_internal_kernel tests/unit/kernel/test_result_metadata_budget.cpp)
if(PHOTOSPIDER_ENABLE_TSAN)
  # TSAN defines strong global new/delete interceptors. Keep the ordinary
  # budget/lifetime checks, while native builds retain allocator injection.
  target_compile_definitions(test_result_metadata_budget PRIVATE
    PHOTOSPIDER_TEST_NO_GLOBAL_ALLOCATOR_INTERPOSITION)
endif()
target_include_directories(test_result_metadata_budget PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src/lib")
photospider_add_kernel_test(test_resources Photospider::kernel tests/unit/kernel/test_resources.cpp)

add_library(operation_fixture MODULE tests/fixtures/operation_fixture.cpp)
target_link_libraries(operation_fixture PRIVATE Photospider::operation_sdk)
target_compile_definitions(operation_fixture PRIVATE PHOTOSPIDER_OPERATION_PLUGIN_BUILD)
set_target_properties(operation_fixture PROPERTIES PREFIX "" CXX_STANDARD 17 CXX_STANDARD_REQUIRED ON CXX_EXTENSIONS OFF)
target_compile_options(operation_fixture PRIVATE -Wall -Wextra -Wpedantic -Werror)

add_library(operation_bad_fixture MODULE tests/fixtures/operation_bad_fixture.cpp)
target_link_libraries(operation_bad_fixture PRIVATE Photospider::operation_sdk)
target_compile_definitions(operation_bad_fixture PRIVATE PHOTOSPIDER_OPERATION_PLUGIN_BUILD)
set_target_properties(operation_bad_fixture PROPERTIES PREFIX "" CXX_STANDARD 17 CXX_STANDARD_REQUIRED ON CXX_EXTENSIONS OFF)
target_compile_options(operation_bad_fixture PRIVATE -Wall -Wextra -Wpedantic -Werror)

foreach(case_number RANGE 1 3)
  set(target operation_tensor_extent_fixture_${case_number})
  add_library(${target} MODULE tests/fixtures/operation_tensor_extent_fixture.cpp)
  target_link_libraries(${target} PRIVATE Photospider::operation_sdk)
  target_compile_definitions(${target} PRIVATE
    PHOTOSPIDER_OPERATION_PLUGIN_BUILD PS_TENSOR_EXTENT_CASE=${case_number})
  set_target_properties(${target} PROPERTIES PREFIX "" CXX_STANDARD 17 CXX_STANDARD_REQUIRED ON CXX_EXTENSIONS OFF)
  target_compile_options(${target} PRIVATE -Wall -Wextra -Wpedantic -Werror)
endforeach()

function(photospider_add_bad_parameter_fixture target case_number)
  add_library(
    ${target} MODULE
    tests/fixtures/operation_bad_parameter_fixture.cpp)
  target_link_libraries(${target} PRIVATE Photospider::operation_sdk)
  target_compile_definitions(
    ${target}
    PRIVATE PHOTOSPIDER_OPERATION_PLUGIN_BUILD
            PS_BAD_PARAMETER_CASE=${case_number})
  set_target_properties(${target} PROPERTIES PREFIX "" CXX_STANDARD 17 CXX_STANDARD_REQUIRED ON CXX_EXTENSIONS OFF)
  target_compile_options(${target} PRIVATE -Wall -Wextra -Wpedantic -Werror)
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
  set_target_properties(${target} PROPERTIES PREFIX "" CXX_STANDARD 17 CXX_STANDARD_REQUIRED ON CXX_EXTENSIONS OFF)
  target_compile_options(${target} PRIVATE -Wall -Wextra -Wpedantic -Werror)
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

photospider_add_kernel_test(
  test_plugin_registry photospider_test_kernel
  tests/integration/kernel/test_plugin_registry.cpp)
target_include_directories(test_plugin_registry PRIVATE src/lib)
target_compile_definitions(test_plugin_registry
                           PRIVATE PHOTOSPIDER_ENABLE_LIBRARY_TEST_HOOKS)
add_dependencies(
  test_plugin_registry
  operation_fixture
  operation_bad_fixture
  operation_tensor_extent_fixture_1
  operation_tensor_extent_fixture_2
  operation_tensor_extent_fixture_3
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
  operation_unicode_fixture)
target_compile_definitions(
  test_plugin_registry
  PRIVATE PS_OPERATION_FIXTURE_PATH="$<TARGET_FILE:operation_fixture>"
          PS_OPERATION_BAD_FIXTURE_PATH="$<TARGET_FILE:operation_bad_fixture>"
          PS_OPERATION_TENSOR_EXTENT_FIXTURE_PATH="$<TARGET_FILE:operation_tensor_extent_fixture_1>"
          PS_OPERATION_TENSOR_ZERO_FIXTURE_PATH="$<TARGET_FILE:operation_tensor_extent_fixture_2>"
          PS_OPERATION_TENSOR_RANK_FIXTURE_PATH="$<TARGET_FILE:operation_tensor_extent_fixture_3>"
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
          PS_OPERATION_UNICODE_FIXTURE_PATH="$<TARGET_FILE:operation_unicode_fixture>")


add_library(multi_output_fixture MODULE tests/fixtures/multi_output_fixture.c)
target_link_libraries(multi_output_fixture PRIVATE Photospider::operation_sdk)
set_target_properties(multi_output_fixture PROPERTIES C_STANDARD 11 C_STANDARD_REQUIRED ON C_EXTENSIONS OFF)
target_compile_options(multi_output_fixture PRIVATE -Wall -Wextra -Wpedantic -Werror)
target_compile_definitions(multi_output_fixture PRIVATE PHOTOSPIDER_OPERATION_PLUGIN_BUILD)
photospider_add_kernel_test(test_multi_output_contract Photospider::kernel tests/unit/kernel/test_multi_output_contract.cpp)
target_compile_definitions(test_multi_output_contract PRIVATE PS_MULTI_OUTPUT_FIXTURE="$<TARGET_FILE:multi_output_fixture>")
add_dependencies(test_multi_output_contract multi_output_fixture)
photospider_add_kernel_test(test_multi_output_execution Photospider::kernel tests/integration/kernel/test_multi_output_execution.cpp)
photospider_add_kernel_test(test_result_exceptions Photospider::kernel tests/integration/kernel/test_result_exceptions.cpp)
target_compile_definitions(test_multi_output_execution PRIVATE PS_MULTI_OUTPUT_FIXTURE="$<TARGET_FILE:multi_output_fixture>")
add_dependencies(test_multi_output_execution multi_output_fixture)
photospider_add_kernel_test(test_prepared_workspace Photospider::kernel tests/integration/kernel/test_prepared_workspace.cpp)
photospider_add_kernel_test(test_backend_admission Photospider::kernel tests/integration/kernel/test_backend_admission.cpp)


photospider_add_kernel_test(test_value_fragments Photospider::kernel tests/unit/kernel/test_value_fragments.cpp)
photospider_add_kernel_test(test_resource_observation photospider_test_kernel tests/unit/kernel/test_resource_observation.cpp)
target_include_directories(test_resource_observation PRIVATE src/lib)
photospider_add_kernel_test(test_result_image_backing Photospider::kernel tests/integration/kernel/image/test_result_image_backing.cpp)
photospider_add_kernel_test(test_result_objects Photospider::kernel tests/unit/kernel/test_result_objects.cpp)

photospider_add_kernel_test(test_vulkan_gpu photospider_test_kernel tests/unit/kernel/test_vulkan_gpu.cpp)
target_include_directories(test_vulkan_gpu PRIVATE src/lib)
set_tests_properties(test_vulkan_gpu PROPERTIES SKIP_RETURN_CODE 77)



photospider_add_kernel_test(test_port_contracts Photospider::kernel tests/unit/kernel/test_port_contracts.cpp)
add_library(operation_contract_fixture MODULE tests/fixtures/operation_contract_fixture.c)
target_link_libraries(operation_contract_fixture PRIVATE Photospider::operation_sdk)
target_compile_options(operation_contract_fixture PRIVATE -Wall -Wextra -Wpedantic -Werror)
target_compile_definitions(operation_contract_fixture PRIVATE PHOTOSPIDER_OPERATION_PLUGIN_BUILD)
set_target_properties(operation_contract_fixture PROPERTIES C_STANDARD 11 C_STANDARD_REQUIRED ON C_EXTENSIONS OFF)
target_compile_definitions(test_port_contracts PRIVATE PS_CONTRACT_FIXTURE="$<TARGET_FILE:operation_contract_fixture>")
add_dependencies(test_port_contracts operation_contract_fixture)
foreach(_case 1 3 5 6 7 8 9 10 11 12 13)
  add_library(operation_bad_contract_${_case} MODULE tests/fixtures/operation_contract_fixture.c)
  target_link_libraries(operation_bad_contract_${_case} PRIVATE Photospider::operation_sdk)
  target_compile_options(operation_bad_contract_${_case} PRIVATE -Wall -Wextra -Wpedantic -Werror)
  target_compile_definitions(operation_bad_contract_${_case} PRIVATE PHOTOSPIDER_OPERATION_PLUGIN_BUILD)
  set_target_properties(operation_bad_contract_${_case} PROPERTIES C_STANDARD 11 C_STANDARD_REQUIRED ON C_EXTENSIONS OFF)
  target_compile_definitions(operation_bad_contract_${_case} PRIVATE PS_BAD_CONTRACT_CASE=${_case})
  target_compile_definitions(test_port_contracts PRIVATE PS_BAD_CONTRACT_${_case}="$<TARGET_FILE:operation_bad_contract_${_case}>")
  add_dependencies(test_port_contracts operation_bad_contract_${_case})
endforeach()

photospider_add_kernel_test(test_footprint Photospider::kernel tests/unit/kernel/test_footprint.cpp)
photospider_add_kernel_test(test_value_runs Photospider::kernel tests/unit/kernel/test_value_runs.cpp)
photospider_add_kernel_test(test_fragment_atlas Photospider::kernel tests/unit/kernel/test_fragment_atlas.cpp)
photospider_add_kernel_test(test_input_snapshot Photospider::kernel tests/unit/kernel/test_input_snapshot.cpp)
target_include_directories(test_input_snapshot PRIVATE src/lib)
photospider_add_kernel_test(test_fragment_atlas_gpu photospider_internal_kernel tests/unit/kernel/test_fragment_atlas_gpu.cpp)
target_include_directories(test_fragment_atlas_gpu PRIVATE src/lib)
set_tests_properties(test_fragment_atlas_gpu PROPERTIES SKIP_RETURN_CODE 77)
photospider_add_kernel_test(test_native_gpu photospider_internal_kernel tests/unit/kernel/test_native_gpu.cpp)
photospider_add_kernel_test(test_pipeline_key Photospider::kernel tests/unit/kernel/test_pipeline_key.cpp)
target_include_directories(test_pipeline_key PRIVATE src/lib)
target_include_directories(test_native_gpu PRIVATE src/lib)
set_tests_properties(test_native_gpu PROPERTIES SKIP_RETURN_CODE 77)
photospider_add_kernel_test(test_storage Photospider::kernel tests/unit/kernel/test_storage.cpp)
photospider_add_kernel_test(test_value Photospider::kernel tests/unit/kernel/test_value.cpp)
photospider_add_kernel_test(test_checked_math Photospider::kernel tests/unit/kernel/test_checked_math.cpp)
target_include_directories(test_checked_math PRIVATE src/lib)
photospider_add_kernel_test(
    test_compiler Photospider::kernel tests/unit/kernel/test_compiler.cpp)
photospider_add_kernel_test(test_frontend_registry Photospider::kernel tests/unit/kernel/test_frontend_registry.cpp)
photospider_add_kernel_test(test_planner_registry Photospider::kernel tests/unit/kernel/test_planner_registry.cpp)
photospider_add_kernel_test(test_compiler_pipeline Photospider::kernel tests/unit/kernel/test_compiler_pipeline.cpp)
photospider_add_kernel_test(
    test_compiler_artifact Photospider::kernel tests/unit/kernel/test_compiler_artifact.cpp)
photospider_add_kernel_test(
    test_operation_contract_validation
    Photospider::kernel
    tests/unit/kernel/test_operation_contract_validation.cpp
    tests/fuzz/operation_contract_ir_fuzz.cpp)
target_compile_definitions(
    test_operation_contract_validation
    PRIVATE PS_OPERATION_VALID_SEED_PATH="${CMAKE_CURRENT_SOURCE_DIR}/tests/fuzz/corpus/operation_contract_ir/valid-source"
            PS_OPERATION_DUPLICATE_SEED_PATH="${CMAKE_CURRENT_SOURCE_DIR}/tests/fuzz/corpus/operation_contract_ir/malformed-schema")
photospider_add_kernel_test(
    test_diagnostic_accumulator Photospider::kernel
    tests/unit/kernel/test_diagnostic_accumulator.cpp)


photospider_add_kernel_test(test_result_image_views Photospider::kernel tests/integration/kernel/image/test_result_image_views.cpp)


photospider_add_kernel_test(test_result_image_execution Photospider::kernel tests/integration/kernel/image/test_result_image_execution.cpp)

photospider_add_kernel_test(test_sparse_input_workflow Photospider::kernel tests/integration/kernel/test_sparse_input_workflow.cpp)

photospider_add_kernel_test(test_prepared_operation Photospider::kernel tests/unit/kernel/test_prepared_operation.cpp)


set_property(TEST
  test_vulkan_gpu
  test_fragment_atlas_gpu
  test_native_gpu
  APPEND PROPERTY LABELS native gpu)
set_property(TEST test_vulkan_gpu APPEND PROPERTY LABELS vulkan)
set_property(TEST test_native_gpu test_fragment_atlas_gpu
  APPEND PROPERTY LABELS metal)
set_property(TEST
  test_module_tiles
  test_plugin_registry
  test_multi_output_contract test_multi_output_execution
  test_port_contracts test_operation_contract_validation
  APPEND PROPERTY LABELS abi)

photospider_add_kernel_test(test_tensor_work Photospider::kernel tests/unit/kernel/test_tensor_work.cpp)
