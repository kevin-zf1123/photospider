# Kernel behavior tests register their own operations and require no bundled ops.
photospider_add_kernel_test(test_result_publication Photospider::kernel tests/unit/kernel/test_result_publication.cpp)
photospider_add_kernel_test(test_tiles Photospider::kernel tests/unit/kernel/test_tiles.cpp)
photospider_add_kernel_test(test_tile_records photospider_internal_kernel tests/unit/kernel/test_tile_records.cpp)
target_include_directories(test_tile_records PRIVATE src/lib)
photospider_add_kernel_test(test_tile_cache photospider_internal_kernel tests/unit/kernel/test_tile_cache.cpp)
target_include_directories(test_tile_cache PRIVATE src/lib)
photospider_add_kernel_test(test_whole_needs Photospider::kernel tests/integration/kernel/test_whole_needs.cpp)
photospider_add_kernel_test(test_tile_demand Photospider::kernel tests/integration/kernel/test_tile_demand.cpp)
photospider_add_kernel_test(test_result_schema_validation Photospider::kernel tests/integration/kernel/test_result_schema_validation.cpp)

photospider_add_result_table_fixture(result_field_need_fixture tests/fixtures/result_field_need_fixture.c)
photospider_add_kernel_test(test_result_c_fields Photospider::kernel tests/integration/kernel/test_result_c_fields.cpp)
target_compile_definitions(test_result_c_fields PRIVATE PS_RESULT_FIXTURE="$<TARGET_FILE:result_field_need_fixture>")
photospider_add_result_table_fixture(result_tile_need_fixture tests/fixtures/result_tile_need_fixture.c)
photospider_add_kernel_test(test_result_c_tiles Photospider::kernel tests/integration/kernel/test_result_c_tiles.cpp)
target_compile_definitions(test_result_c_tiles PRIVATE PS_RESULT_TILE_NEED_FIXTURE="$<TARGET_FILE:result_tile_need_fixture>")

photospider_add_kernel_test(test_tensor_validation photospider_internal_kernel tests/unit/kernel/test_tensor_validation.cpp)
target_include_directories(test_tensor_validation PRIVATE src/lib)

photospider_add_kernel_test(test_result_failure_reporting photospider_internal_kernel tests/unit/kernel/test_result_failure_reporting.cpp)
target_include_directories(test_result_failure_reporting PRIVATE src/lib)

foreach(_mode RANGE 0 6)
  photospider_add_result_table_fixture(result_abi_fixture_${_mode} tests/fixtures/result_abi_fixture.c)
  target_compile_definitions(result_abi_fixture_${_mode} PRIVATE PS_TEST_ABI_MODE=${_mode})
endforeach()
photospider_add_kernel_test(test_result_c_abi photospider_test_kernel tests/integration/kernel/test_result_c_abi.cpp)
target_include_directories(test_result_c_abi PRIVATE src/lib)
target_compile_definitions(test_result_c_abi PRIVATE
  PS_ABI_FAILURE_DELIVERY="$<TARGET_FILE:result_abi_fixture_6>"
  PS_ABI_VALID="$<TARGET_FILE:result_abi_fixture_0>"
  PS_ABI_VERSION="$<TARGET_FILE:result_abi_fixture_1>"
  PS_ABI_SIZE="$<TARGET_FILE:result_abi_fixture_2>"
  PS_ABI_OPERATION="$<TARGET_FILE:result_abi_fixture_3>"
  PS_ABI_UNSUPPORTED_V2="$<TARGET_FILE:result_abi_fixture_4>"
  PS_ABI_UNSUPPORTED_V1="$<TARGET_FILE:result_abi_fixture_5>")
add_dependencies(test_result_c_abi result_abi_fixture_0 result_abi_fixture_1 result_abi_fixture_2 result_abi_fixture_3 result_abi_fixture_4 result_abi_fixture_5)

photospider_add_kernel_test(test_result_c_checkpoint Photospider::kernel tests/integration/kernel/test_result_c_checkpoint.cpp)
photospider_add_kernel_test(test_result_c_block Photospider::kernel tests/integration/kernel/test_result_c_block.cpp)
add_library(result_block_fixture MODULE tests/fixtures/result_block_fixture.c)
target_link_libraries(result_block_fixture PRIVATE Photospider::operation_sdk)
if(UNIX)
  target_link_libraries(result_block_fixture PRIVATE m)
endif()
target_compile_options(result_block_fixture PRIVATE -fno-fast-math -frounding-math -ffp-contract=off)
set_target_properties(result_block_fixture PROPERTIES C_STANDARD 11 C_STANDARD_REQUIRED ON C_EXTENSIONS OFF)
target_compile_definitions(test_result_c_block PRIVATE PS_RESULT_BLOCK_FIXTURE="$<TARGET_FILE:result_block_fixture>")
add_dependencies(test_result_c_block result_block_fixture)
add_library(result_checkpoint_fixture MODULE tests/fixtures/result_checkpoint_fixture.c)
target_link_libraries(result_checkpoint_fixture PRIVATE Photospider::operation_sdk)
set_target_properties(result_checkpoint_fixture PROPERTIES C_STANDARD 11 C_STANDARD_REQUIRED ON C_EXTENSIONS OFF)
target_compile_definitions(test_result_c_checkpoint PRIVATE PS_RESULT_CHECKPOINT_FIXTURE="$<TARGET_FILE:result_checkpoint_fixture>")
add_dependencies(test_result_c_checkpoint result_checkpoint_fixture)
photospider_add_kernel_test(test_generic_result_cache photospider_internal_kernel tests/integration/kernel/test_generic_result_cache.cpp)
target_include_directories(test_generic_result_cache PRIVATE src/lib)
photospider_add_kernel_test(test_structured_submission photospider_test_kernel tests/integration/kernel/test_structured_submission.cpp)
target_include_directories(test_structured_submission PRIVATE src/lib)
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
target_link_libraries(operation_fixture PRIVATE Photospider::operation_sdk Threads::Threads)
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

foreach(case_number RANGE 1 3)
  set(target operation_tensor_extent_fixture_${case_number})
  add_library(${target} MODULE tests/fixtures/operation_tensor_extent_fixture.cpp)
  target_link_libraries(${target} PRIVATE Photospider::operation_sdk)
  target_compile_definitions(${target} PRIVATE
    PHOTOSPIDER_OPERATION_PLUGIN_BUILD PS_TENSOR_EXTENT_CASE=${case_number})
  set_target_properties(${target} PROPERTIES PREFIX "")
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
  operation_dense_overflow_fixture
  operation_dense_rank1_limit_fixture
  operation_dense_rank1_overflow_fixture
  operation_dense_rank2_limit_fixture
  operation_dense_rank2_overflow_fixture
  operation_dense_multi_overflow_fixture
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
          PS_OPERATION_DENSE_OVERFLOW_FIXTURE_PATH="$<TARGET_FILE:operation_dense_overflow_fixture>"
          PS_OPERATION_DENSE_RANK1_LIMIT_FIXTURE_PATH="$<TARGET_FILE:operation_dense_rank1_limit_fixture>"
          PS_OPERATION_DENSE_RANK1_OVERFLOW_FIXTURE_PATH="$<TARGET_FILE:operation_dense_rank1_overflow_fixture>"
          PS_OPERATION_DENSE_RANK2_LIMIT_FIXTURE_PATH="$<TARGET_FILE:operation_dense_rank2_limit_fixture>"
          PS_OPERATION_DENSE_RANK2_OVERFLOW_FIXTURE_PATH="$<TARGET_FILE:operation_dense_rank2_overflow_fixture>"
          PS_OPERATION_DENSE_MULTI_OVERFLOW_FIXTURE_PATH="$<TARGET_FILE:operation_dense_multi_overflow_fixture>"
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

photospider_add_kernel_test(test_scan_waiters photospider_test_kernel tests/integration/kernel/test_scan_waiters.cpp)
target_include_directories(test_scan_waiters PRIVATE src/lib)

add_library(multi_output_fixture MODULE tests/fixtures/multi_output_fixture.c)
target_link_libraries(multi_output_fixture PRIVATE Photospider::operation_sdk)
set_target_properties(multi_output_fixture PROPERTIES C_STANDARD 11 C_STANDARD_REQUIRED ON)
target_compile_definitions(multi_output_fixture PRIVATE PHOTOSPIDER_OPERATION_PLUGIN_BUILD)
photospider_add_kernel_test(test_multi_output_contract Photospider::kernel tests/unit/kernel/test_multi_output_contract.cpp)
target_compile_definitions(test_multi_output_contract PRIVATE PS_MULTI_OUTPUT_FIXTURE="$<TARGET_FILE:multi_output_fixture>")
add_dependencies(test_multi_output_contract multi_output_fixture)
photospider_add_kernel_test(test_multi_output_execution Photospider::kernel tests/integration/kernel/test_multi_output_execution.cpp)
photospider_add_kernel_test(test_result_exceptions Photospider::kernel tests/integration/kernel/test_result_exceptions.cpp)
target_compile_definitions(test_multi_output_execution PRIVATE PS_MULTI_OUTPUT_FIXTURE="$<TARGET_FILE:multi_output_fixture>")
add_dependencies(test_multi_output_execution multi_output_fixture)
photospider_add_kernel_test(test_generic_binding_identity Photospider::kernel tests/unit/kernel/test_generic_binding_identity.cpp)
photospider_add_kernel_test(test_computed_scalar photospider_test_kernel tests/integration/kernel/test_computed_scalar.cpp)
target_include_directories(test_computed_scalar PRIVATE src/lib)
target_compile_definitions(test_computed_scalar PRIVATE PHOTOSPIDER_LOCAL_EXECUTION_SYNC_TESTS)
photospider_add_kernel_test(test_resource_internals photospider_internal_kernel tests/unit/kernel/test_resource_internals.cpp)
target_include_directories(test_resource_internals PRIVATE src/lib)
photospider_add_kernel_test(test_prepared_workspace Photospider::kernel tests/integration/kernel/test_prepared_workspace.cpp)
photospider_add_kernel_test(test_backend_admission Photospider::kernel tests/integration/kernel/test_backend_admission.cpp)

photospider_add_kernel_test(test_result_dag_parallel Photospider::kernel tests/integration/kernel/test_result_dag_parallel.cpp)
photospider_add_kernel_test(test_cpu_tiles Photospider::kernel tests/integration/kernel/test_cpu_tiles.cpp)
photospider_add_kernel_test(test_cpu_parallel Photospider::kernel tests/integration/kernel/test_cpu_parallel.cpp)
photospider_add_kernel_test(test_managed_dispatch Photospider::kernel tests/integration/kernel/test_managed_dispatch.cpp)

photospider_add_kernel_test(test_value_fragments Photospider::kernel tests/unit/kernel/test_value_fragments.cpp)
photospider_add_kernel_test(test_memory_liveness photospider_test_kernel tests/integration/kernel/test_memory_liveness.cpp)
target_include_directories(test_memory_liveness PRIVATE src/lib)
photospider_add_kernel_test(test_resource_observation photospider_test_kernel tests/unit/kernel/test_resource_observation.cpp)
target_include_directories(test_resource_observation PRIVATE src/lib)
photospider_add_kernel_test(test_result_image_backing Photospider::kernel tests/integration/kernel/image/test_result_image_backing.cpp)
photospider_add_kernel_test(test_result_objects Photospider::kernel tests/unit/kernel/test_result_objects.cpp)

photospider_add_result_table_fixture(result_repeated_fixture tests/fixtures/result_repeated_fixture.c)
photospider_add_kernel_test(test_result_repeated Photospider::kernel tests/integration/kernel/test_result_repeated.cpp)
target_compile_definitions(test_result_repeated PRIVATE PS_REPEATED_FIXTURE="$<TARGET_FILE:result_repeated_fixture>")
add_dependencies(test_result_repeated result_repeated_fixture)
photospider_add_result_table_fixture(result_bad_repeated_fixture tests/fixtures/result_repeated_fixture.c BAD)
target_compile_definitions(test_result_repeated PRIVATE PS_BAD_REPEATED_FIXTURE=\"$<TARGET_FILE:result_bad_repeated_fixture>\")
photospider_add_kernel_test(test_result_tuple_validation Photospider::kernel tests/integration/kernel/test_result_tuple_validation.cpp)
photospider_add_kernel_test(test_result_queries Photospider::kernel tests/integration/kernel/test_result_queries.cpp)

photospider_add_kernel_test(test_native_dependency photospider_test_kernel tests/integration/kernel/test_native_dependency.cpp)
target_include_directories(test_native_dependency PRIVATE src/lib)
set_tests_properties(test_native_dependency PROPERTIES SKIP_RETURN_CODE 77)
photospider_add_kernel_test(test_result_atlas_services Photospider::kernel tests/unit/kernel/test_result_atlas_services.cpp)
photospider_register_test(NAME test_result_atlas_native
  SUITE kernel LEVEL integration COMMAND test_result_atlas_services --native
  LABELS gpu native metal SKIP_RETURN_CODE 77)
set_property(TEST test_result_atlas_services APPEND PROPERTY LABELS mock gpu)
photospider_add_kernel_test(test_result_discovery_services Photospider::kernel tests/unit/kernel/test_result_discovery_services.cpp)
photospider_register_test(NAME test_result_discovery_native
  SUITE kernel LEVEL integration COMMAND test_result_discovery_services --native
  LABELS gpu native metal SKIP_RETURN_CODE 77)
set_property(TEST test_result_discovery_services APPEND PROPERTY LABELS mock gpu)
photospider_add_kernel_test(test_vulkan_gpu photospider_test_kernel tests/unit/kernel/test_vulkan_gpu.cpp)
target_include_directories(test_vulkan_gpu PRIVATE src/lib)
set_tests_properties(test_vulkan_gpu PROPERTIES SKIP_RETURN_CODE 77)
photospider_add_kernel_test(test_progressive_needs Photospider::kernel tests/integration/kernel/test_progressive_needs.cpp)

photospider_add_kernel_test(test_native_execution Photospider::kernel tests/integration/kernel/test_native_execution.cpp)
set_tests_properties(test_native_execution PROPERTIES SKIP_RETURN_CODE 77)

add_library(gpu_discovery_fixture MODULE tests/fixtures/gpu_result_discovery_plugin.c)
target_link_libraries(gpu_discovery_fixture PRIVATE Photospider::operation_sdk)
target_compile_definitions(gpu_discovery_fixture PRIVATE PHOTOSPIDER_OPERATION_PLUGIN_BUILD)
set_target_properties(gpu_discovery_fixture PROPERTIES C_STANDARD 11 C_STANDARD_REQUIRED ON)
photospider_add_kernel_test(test_gpu_discovery_workflow Photospider::kernel tests/integration/kernel/gpu/test_gpu_discovery_workflow.cpp)
target_compile_definitions(test_gpu_discovery_workflow PRIVATE PS_GPU_DISCOVERY_PLUGIN="$<TARGET_FILE:gpu_discovery_fixture>")
add_dependencies(test_gpu_discovery_workflow gpu_discovery_fixture)
set_tests_properties(test_gpu_discovery_workflow PROPERTIES SKIP_RETURN_CODE 77)

photospider_add_kernel_test(test_gpu_sync_fallback Photospider::kernel tests/integration/kernel/gpu/test_gpu_sync_fallback.cpp)
set_tests_properties(test_gpu_sync_fallback PROPERTIES SKIP_RETURN_CODE 77)
photospider_add_kernel_test(test_execution_dependencies Photospider::kernel tests/integration/kernel/test_execution_dependencies.cpp)
add_dependencies(test_result_repeated result_bad_repeated_fixture)

photospider_add_kernel_test(test_result_tensor_grants Photospider::kernel tests/integration/kernel/test_result_tensor_grants.cpp)

photospider_add_kernel_test(test_result_execution Photospider::kernel tests/integration/kernel/test_result_execution.cpp)

photospider_add_kernel_test(test_dependency_program photospider_internal_kernel tests/integration/kernel/test_dependency_program.cpp)
target_include_directories(test_dependency_program PRIVATE src/lib)

photospider_add_kernel_test(test_execution_demand Photospider::kernel tests/integration/kernel/test_execution_demand.cpp)

add_library(gpu_dependency_c_fixture MODULE tests/fixtures/gpu_result_c_plugin.c)
target_link_libraries(gpu_dependency_c_fixture PRIVATE Photospider::operation_sdk)
target_compile_definitions(gpu_dependency_c_fixture PRIVATE PHOTOSPIDER_OPERATION_PLUGIN_BUILD)
set_target_properties(gpu_dependency_c_fixture PROPERTIES C_STANDARD 11 C_STANDARD_REQUIRED ON C_EXTENSIONS OFF)
photospider_add_kernel_test(test_gpu_c_abi_execution Photospider::kernel tests/integration/kernel/gpu/test_gpu_c_abi_execution.cpp)
target_compile_definitions(test_gpu_c_abi_execution PRIVATE PS_GPU_DEPENDENCY_PLUGIN="$<TARGET_FILE:gpu_dependency_c_fixture>")
add_dependencies(test_gpu_c_abi_execution gpu_dependency_c_fixture)
set_tests_properties(test_gpu_c_abi_execution PROPERTIES SKIP_RETURN_CODE 77)
photospider_add_kernel_test(test_gpu_fragment_execution Photospider::kernel tests/integration/kernel/gpu/test_gpu_fragment_execution.cpp)
set_tests_properties(test_gpu_fragment_execution PROPERTIES SKIP_RETURN_CODE 77)

photospider_add_result_table_fixture(result_operation_fixture tests/fixtures/result_operation_fixture.c)
photospider_add_kernel_test(test_result_plugin Photospider::kernel tests/integration/kernel/test_result_plugin.cpp)
target_compile_definitions(test_result_plugin PRIVATE PS_RESULT_FIXTURE="$<TARGET_FILE:result_operation_fixture>")
add_dependencies(test_result_plugin result_operation_fixture result_bad_fixture)
photospider_add_result_table_fixture(result_bad_fixture tests/fixtures/result_operation_fixture.c BAD)
target_compile_definitions(test_result_plugin PRIVATE PS_BAD_RESULT_FIXTURE=\"$<TARGET_FILE:result_bad_fixture>\")
photospider_register_test(NAME test_result_native_gpu SUITE kernel LEVEL integration COMMAND test_result_plugin --gpu-only)
set_tests_properties(test_result_native_gpu PROPERTIES SKIP_RETURN_CODE 77)

photospider_add_kernel_test(test_result_image_contracts Photospider::kernel tests/integration/kernel/test_result_image_contracts.cpp)

photospider_add_kernel_test(test_semantic_contract Photospider::kernel tests/unit/kernel/test_semantic_contract.cpp)
add_library(operation_contract_fixture MODULE tests/fixtures/operation_contract_fixture.c)
target_link_libraries(operation_contract_fixture PRIVATE Photospider::operation_sdk)
set_target_properties(operation_contract_fixture PROPERTIES C_STANDARD 11 C_STANDARD_REQUIRED ON C_EXTENSIONS OFF)
target_compile_definitions(test_semantic_contract PRIVATE PS_CONTRACT_FIXTURE="$<TARGET_FILE:operation_contract_fixture>")
add_dependencies(test_semantic_contract operation_contract_fixture)
foreach(_case RANGE 1 12)
  add_library(operation_bad_contract_${_case} MODULE tests/fixtures/operation_contract_fixture.c)
  target_link_libraries(operation_bad_contract_${_case} PRIVATE Photospider::operation_sdk)
  set_target_properties(operation_bad_contract_${_case} PROPERTIES C_STANDARD 11 C_STANDARD_REQUIRED ON C_EXTENSIONS OFF)
  target_compile_definitions(operation_bad_contract_${_case} PRIVATE PS_BAD_CONTRACT_CASE=${_case})
  target_compile_definitions(test_semantic_contract PRIVATE PS_BAD_CONTRACT_${_case}="$<TARGET_FILE:operation_bad_contract_${_case}>")
  add_dependencies(test_semantic_contract operation_bad_contract_${_case})
endforeach()

photospider_add_kernel_test(test_footprint Photospider::kernel tests/unit/kernel/test_footprint.cpp)
photospider_add_kernel_test(test_value_runs Photospider::kernel tests/unit/kernel/test_value_runs.cpp)
photospider_add_kernel_test(test_fragment_atlas Photospider::kernel tests/unit/kernel/test_fragment_atlas.cpp)
photospider_add_kernel_test(test_input_snapshot Photospider::kernel tests/unit/kernel/test_input_snapshot.cpp)
target_include_directories(test_input_snapshot PRIVATE src/lib)
photospider_add_kernel_test(test_scheduler_timing Photospider::kernel tests/integration/kernel/test_scheduler_timing.cpp)
target_include_directories(test_scheduler_timing PRIVATE src/lib)
target_compile_definitions(test_scheduler_timing PRIVATE PHOTOSPIDER_TEST_QUEUE_METER)
photospider_add_kernel_test(test_result_gpu_service Photospider::kernel tests/unit/kernel/test_result_gpu_service.cpp)
target_include_directories(test_result_gpu_service PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src/lib")
photospider_add_kernel_test(test_result_discovery_decoder photospider_internal_kernel tests/unit/kernel/test_result_discovery_decoder.cpp)
target_include_directories(test_result_discovery_decoder PRIVATE src/lib)
photospider_add_kernel_test(test_fragment_atlas_gpu photospider_internal_kernel tests/unit/kernel/test_fragment_atlas_gpu.cpp)
target_include_directories(test_fragment_atlas_gpu PRIVATE src/lib)
set_tests_properties(test_fragment_atlas_gpu PROPERTIES SKIP_RETURN_CODE 77)
photospider_add_kernel_test(test_native_gpu photospider_test_kernel tests/unit/kernel/test_native_gpu.cpp)
photospider_add_kernel_test(test_pipeline_key Photospider::kernel tests/unit/kernel/test_pipeline_key.cpp)
target_include_directories(test_pipeline_key PRIVATE src/lib)
target_include_directories(test_native_gpu PRIVATE src/lib)
set_tests_properties(test_native_gpu PROPERTIES SKIP_RETURN_CODE 77)
photospider_add_kernel_test(test_quality_report Photospider::kernel tests/unit/kernel/test_quality_report.cpp)
photospider_add_kernel_test(test_layer Photospider::kernel tests/unit/kernel/test_layer.cpp)
photospider_add_kernel_test(test_cpu_range Photospider::kernel tests/unit/kernel/test_cpu_range.cpp)
target_include_directories(test_cpu_range PRIVATE src/lib)
photospider_add_kernel_test(test_storage Photospider::kernel tests/unit/kernel/test_storage.cpp)
photospider_add_kernel_test(test_value Photospider::kernel tests/unit/kernel/test_value.cpp)
photospider_add_kernel_test(test_checked_math Photospider::kernel tests/unit/kernel/test_checked_math.cpp)
target_include_directories(test_checked_math PRIVATE src/lib)
photospider_add_kernel_test(test_lut3d_axis Photospider::kernel tests/unit/kernel/test_lut3d_axis.cpp)
target_include_directories(test_lut3d_axis PRIVATE src/lib)
photospider_add_kernel_test(
    test_compiler Photospider::kernel tests/unit/kernel/test_compiler.cpp)
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
    test_execution photospider_test_kernel
    tests/integration/kernel/test_execution.cpp)
target_include_directories(test_execution PRIVATE src/lib)
photospider_register_test(NAME test_execution_result_dso_native SUITE kernel LEVEL integration COMMAND test_execution --result-dso-native)
set_tests_properties(test_execution_result_dso_native PROPERTIES SKIP_RETURN_CODE 77)
add_dependencies(test_execution operation_fixture)
target_compile_definitions(
    test_execution
    PRIVATE PS_OPERATION_FIXTURE_PATH="$<TARGET_FILE:operation_fixture>")
photospider_add_kernel_test(
    test_result_diagnostics Photospider::kernel
    tests/integration/kernel/test_result_diagnostics.cpp)

photospider_add_kernel_test(test_model_metadata Photospider::kernel tests/integration/kernel/test_model_metadata.cpp)
photospider_add_kernel_test(test_frozen_execution Photospider::kernel tests/integration/kernel/test_frozen_execution.cpp)

photospider_add_kernel_test(test_result_image_views Photospider::kernel tests/integration/kernel/image/test_result_image_views.cpp)

photospider_add_kernel_test(test_result_output_payload Photospider::kernel tests/integration/kernel/test_result_output_payload.cpp)

photospider_add_kernel_test(test_result_image_execution Photospider::kernel tests/integration/kernel/image/test_result_image_execution.cpp)

photospider_add_kernel_test(test_sparse_input_workflow Photospider::kernel tests/integration/kernel/test_sparse_input_workflow.cpp)

photospider_add_kernel_test(test_unified_result_images Photospider::kernel tests/integration/kernel/test_unified_result_images.cpp)

photospider_add_kernel_test(test_prepared_operation Photospider::kernel tests/unit/kernel/test_prepared_operation.cpp)

photospider_add_kernel_test(test_result_metadata_cache Photospider::kernel tests/integration/kernel/test_result_metadata_cache.cpp)

set_property(TEST test_result_gpu_service test_result_discovery_decoder
  APPEND PROPERTY LABELS mock gpu)
set_property(TEST
  test_native_dependency test_vulkan_gpu test_native_execution
  test_gpu_discovery_workflow test_gpu_sync_fallback test_gpu_c_abi_execution
  test_gpu_fragment_execution test_result_native_gpu test_fragment_atlas_gpu
  test_native_gpu test_execution_result_dso_native
  APPEND PROPERTY LABELS native gpu)
set_property(TEST test_vulkan_gpu APPEND PROPERTY LABELS vulkan)
set_property(TEST test_native_gpu test_fragment_atlas_gpu
  test_gpu_discovery_workflow test_gpu_c_abi_execution test_gpu_fragment_execution
  APPEND PROPERTY LABELS metal)
set_property(TEST
  test_result_c_fields test_result_c_tiles test_result_c_abi
  test_result_c_checkpoint test_result_c_block test_plugin_registry
  test_multi_output_contract test_multi_output_execution test_result_repeated
  test_result_plugin test_semantic_contract test_operation_contract_validation
  test_result_native_gpu test_gpu_c_abi_execution test_execution_result_dso_native
  APPEND PROPERTY LABELS abi)
