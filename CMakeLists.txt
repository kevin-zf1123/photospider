cmake_minimum_required(VERSION 3.21)

project(
  Photospider
  VERSION 0.33.0
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

option(PHOTOSPIDER_ENABLE_ACCELERATE "Enable Apple Accelerate matrix candidates" ${APPLE})
if(APPLE AND PHOTOSPIDER_ENABLE_ACCELERATE)
  find_library(PHOTOSPIDER_ACCELERATE_FRAMEWORK Accelerate REQUIRED)
endif()

option(PHOTOSPIDER_ENABLE_MATRIX_SME "Build the optional Apple SME FP64 matrix candidate" OFF)
set(PHOTOSPIDER_MATRIX_BACKEND "ACCELERATE" CACHE STRING "Apple Float32 matrix candidate: SCALAR, ACCELERATE, SME")
set_property(CACHE PHOTOSPIDER_MATRIX_BACKEND PROPERTY STRINGS SCALAR ACCELERATE SME)
if(NOT PHOTOSPIDER_MATRIX_BACKEND MATCHES "^(SCALAR|ACCELERATE|SME)$")
  message(FATAL_ERROR "Unknown PHOTOSPIDER_MATRIX_BACKEND")
endif()
if(PHOTOSPIDER_MATRIX_BACKEND STREQUAL "SME" AND NOT PHOTOSPIDER_ENABLE_MATRIX_SME)
  message(FATAL_ERROR "SME matrix backend requires PHOTOSPIDER_ENABLE_MATRIX_SME=ON")
endif()
if(PHOTOSPIDER_ENABLE_MATRIX_SME)
  if(NOT APPLE OR NOT CMAKE_SYSTEM_PROCESSOR MATCHES "^(arm64|aarch64)$")
    message(FATAL_ERROR "SME matrix candidate requires an Apple arm64 build")
  endif()
  include(CheckCXXCompilerFlag)
  check_cxx_compiler_flag("-march=armv8-a+sme+sme-f64f64" PHOTOSPIDER_COMPILER_HAS_SME_F64)
  if(NOT PHOTOSPIDER_COMPILER_HAS_SME_F64)
    message(FATAL_ERROR "Compiler does not support SME FP64 ACLE")
  endif()
endif()

option(PHOTOSPIDER_ENABLE_METAL "Build the optional Apple Silicon Metal backend" ${APPLE})
if(PHOTOSPIDER_ENABLE_METAL AND APPLE)
  enable_language(OBJCXX)
endif()
option(PHOTOSPIDER_ENABLE_VULKAN "Build the native Vulkan compute backend" OFF)
if(PHOTOSPIDER_ENABLE_VULKAN)
  if(PHOTOSPIDER_ENABLE_METAL AND APPLE)
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
option(PHOTOSPIDER_ENABLE_INPAINT_OPENCV "Build pinned OpenCV 4.12.0 inpaint adapter" OFF)
if(PHOTOSPIDER_ENABLE_INPAINT_OPENCV)
  find_package(OpenCV 4.12.0 EXACT REQUIRED COMPONENTS core photo)
endif()

option(PHOTOSPIDER_TRANSFER_FAST_MATH "Enable certified FMT-09 SIMD interval filter" ON)
add_compile_definitions(PHOTOSPIDER_TRANSFER_FAST_MATH=$<BOOL:${PHOTOSPIDER_TRANSFER_FAST_MATH}>)
option(PHOTOSPIDER_TRANSFER_COMPACT_MATH "Enable FMT-09 compact strict enclosure tier" ON)
option(PHOTOSPIDER_TRANSFER_GAMMA2_SIMD "Enable FMT-09 exact arithmetic and bit-classification SIMD" ON)
add_compile_definitions(
  PHOTOSPIDER_TRANSFER_COMPACT_MATH=$<BOOL:${PHOTOSPIDER_TRANSFER_COMPACT_MATH}>
  PHOTOSPIDER_TRANSFER_GAMMA2_SIMD=$<BOOL:${PHOTOSPIDER_TRANSFER_GAMMA2_SIMD}>)
# FMT-10 algorithm controls are private implementation choices, not semantics.
set(PHOTOSPIDER_FMT10_ALGORITHM "CERTIFIED" CACHE STRING "FMT-10: CERTIFIED or EXACT")
set_property(CACHE PHOTOSPIDER_FMT10_ALGORITHM PROPERTY STRINGS CERTIFIED EXACT)
if(NOT PHOTOSPIDER_FMT10_ALGORITHM MATCHES "^(CERTIFIED|EXACT)$")
  message(FATAL_ERROR "Unknown PHOTOSPIDER_FMT10_ALGORITHM")
endif()
option(PHOTOSPIDER_FMT10_SIMD "FMT-10 AVX2/Apple NEON certified candidates" ON)
if(PHOTOSPIDER_FMT10_ALGORITHM STREQUAL "EXACT")
  set_property(SOURCE plugins/ops/02-format-color/rgb_basis.cpp
    plugins/ops/02-format-color/rgb_basis_math.cpp APPEND PROPERTY
    COMPILE_DEFINITIONS PHOTOSPIDER_FMT10_EXACT_ONLY)
endif()
if(NOT PHOTOSPIDER_FMT10_SIMD)
  set_property(SOURCE plugins/ops/02-format-color/rgb_basis.cpp
    plugins/ops/02-format-color/rgb_basis_math.cpp APPEND PROPERTY
    COMPILE_DEFINITIONS PHOTOSPIDER_FMT10_NO_SIMD)
endif()

# Retain the allocation-based exact evaluator for controlled comparisons.
set(PHOTOSPIDER_FMT10_EXACT_KERNEL "LIMB" CACHE STRING "FMT-10 exact kernel: LIMB or REFERENCE")
set_property(CACHE PHOTOSPIDER_FMT10_EXACT_KERNEL PROPERTY STRINGS LIMB REFERENCE)
if(NOT PHOTOSPIDER_FMT10_EXACT_KERNEL MATCHES "^(LIMB|REFERENCE)$")
  message(FATAL_ERROR "Unknown PHOTOSPIDER_FMT10_EXACT_KERNEL")
endif()
if(PHOTOSPIDER_FMT10_EXACT_KERNEL STREQUAL "REFERENCE")
  set_property(SOURCE plugins/ops/02-format-color/rgb_basis_math.cpp APPEND PROPERTY
    COMPILE_DEFINITIONS PHOTOSPIDER_FMT10_REFERENCE_EXACT)
endif()

set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
  "${CMAKE_CURRENT_SOURCE_DIR}/plugins/ops/gpu/integer.metal"
  "${CMAKE_CURRENT_SOURCE_DIR}/plugins/ops/03-generation/perlin.metal"
  "${CMAKE_CURRENT_SOURCE_DIR}/plugins/ops/05-filter/gaussian.metal")
file(READ "${CMAKE_CURRENT_SOURCE_DIR}/plugins/ops/gpu/integer.metal" PHOTOSPIDER_INTEGER_MSL)
file(READ "${CMAKE_CURRENT_SOURCE_DIR}/plugins/ops/03-generation/perlin.metal" PHOTOSPIDER_PERLIN_MSL)
set(PHOTOSPIDER_PERLIN_MSL "${PHOTOSPIDER_INTEGER_MSL}\n${PHOTOSPIDER_PERLIN_MSL}")
configure_file(plugins/ops/03-generation/perlin_shader.hpp.in
  generated/perlin_shader.hpp @ONLY)
file(READ "${CMAKE_CURRENT_SOURCE_DIR}/plugins/ops/05-filter/gaussian.metal" PHOTOSPIDER_GAUSSIAN_MSL)
set(PHOTOSPIDER_GAUSSIAN_MSL "${PHOTOSPIDER_INTEGER_MSL}\n${PHOTOSPIDER_GAUSSIAN_MSL}")
configure_file(plugins/ops/05-filter/gaussian_shader.hpp.in
  generated/gaussian_shader.hpp @ONLY)

set(PHOTOSPIDER_OPERATION_SOURCES
  plugins/ops/02-format-color/alpha_authoring.cpp
  plugins/ops/02-format-color/alpha_operations.cpp
  plugins/ops/02-format-color/numeric_conversion.cpp
  plugins/ops/02-format-color/model_conversion.cpp
  plugins/ops/02-format-color/model_math.cpp
  plugins/ops/02-format-color/model_simd.cpp
  plugins/ops/02-format-color/rgb_basis.cpp
  plugins/ops/02-format-color/rgb_basis_math.cpp
  plugins/ops/02-format-color/transfer.cpp
  plugins/ops/02-format-color/transfer_definition.cpp
  plugins/ops/02-format-color/channel_assembly.cpp
  plugins/ops/02-format-color/channel_literal_like.cpp
  plugins/ops/02-format-color/channel_extraction.cpp
  plugins/ops/02-format-color/metadata_assignment.cpp
  plugins/ops/00-foundation/image_program.cpp
  plugins/ops/00-foundation/image_native.cpp
  plugins/ops/05-filter/gaussian.cpp
  plugins/ops/05-filter/gaussian_gpu.cpp
  plugins/ops/03-generation/perlin.cpp
  plugins/ops/03-generation/perlin_gpu.cpp
  plugins/ops/00-foundation/core_constant.cpp
  plugins/ops/00-foundation/core_delay.cpp
  plugins/ops/00-foundation/core_gpu_fallback_probe.cpp
  plugins/ops/00-foundation/core_identity.cpp
  plugins/ops/01-numeric/curve_sample_linear.cpp
  plugins/ops/01-numeric/curve_sample_monotone.cpp
  plugins/ops/01-numeric/field_apply_lut_1d.cpp
  plugins/ops/01-numeric/field_smoothstep.cpp
  plugins/ops/01-numeric/lut_apply_1d.cpp
  plugins/ops/01-numeric/math_add.cpp
  plugins/ops/01-numeric/numeric_abs.cpp
  plugins/ops/01-numeric/numeric_add.cpp
  plugins/ops/01-numeric/numeric_clamp.cpp
  plugins/ops/01-numeric/numeric_divide.cpp
  plugins/ops/01-numeric/numeric_maximum.cpp
  plugins/ops/01-numeric/numeric_mean.cpp
  plugins/ops/01-numeric/numeric_ordered_scan.cpp
  plugins/ops/01-numeric/numeric_minimum.cpp
  plugins/ops/01-numeric/numeric_multiply.cpp
  plugins/ops/01-numeric/numeric_sample_expression.cpp
  plugins/ops/01-numeric/numeric_interpolation.cpp
  plugins/ops/01-numeric/numeric_layouts.cpp
  plugins/ops/01-numeric/numeric_indexing.cpp
  plugins/ops/01-numeric/lut1d_application.cpp
  plugins/ops/01-numeric/lut3d_application.cpp
  plugins/ops/01-numeric/log_shaper.cpp
  plugins/ops/01-numeric/lut3d_bake_geometry.cpp
  plugins/ops/01-numeric/lut3d_bake_results.cpp
  plugins/ops/01-numeric/parametric_bezier.cpp
  plugins/ops/01-numeric/bezier_function.cpp
  plugins/ops/01-numeric/color_ramps.cpp
  plugins/ops/01-numeric/curve_interpolation.cpp
  plugins/ops/01-numeric/curve_inverse.cpp
  plugins/ops/01-numeric/lowpass_uniform.cpp
  plugins/ops/01-numeric/lowpass_nonuniform.cpp
  plugins/ops/01-numeric/numeric_expression.cpp
  plugins/ops/01-numeric/numeric_binary.cpp
  plugins/ops/01-numeric/numeric_unary.cpp
  plugins/ops/01-numeric/numeric_calculus.cpp
  plugins/ops/01-numeric/numeric_matrix.cpp
  plugins/ops/01-numeric/accelerated_matrix_apple.cpp
  plugins/ops/01-numeric/numeric_scans.cpp
  plugins/ops/01-numeric/numeric_reductions.cpp
  plugins/ops/01-numeric/numeric_ordering.cpp
  plugins/ops/01-numeric/numeric_ranges.cpp
  plugins/ops/01-numeric/numeric_comparisons.cpp
  plugins/ops/01-numeric/comparison_profiles.cpp
  plugins/ops/01-numeric/numeric_arrays.cpp
  plugins/ops/01-numeric/array_profiles.cpp
  plugins/ops/01-numeric/numeric_sequences.cpp
  plugins/ops/01-numeric/sequence_profiles.cpp
  plugins/ops/01-numeric/numeric_subtract.cpp
  plugins/ops/01-numeric/numeric_variance.cpp
  plugins/ops/07-grade/grade_levels.cpp
  plugins/ops/07-grade/image_exposure_gain.cpp
  plugins/ops/08-transform/dependency_sampling.cpp
  plugins/ops/08-transform/image_downsample_box.cpp
  plugins/ops/08-transform/image_split_horizontal.cpp
  plugins/ops/08-transform/mask_downsample_box.cpp
  plugins/ops/09-composite/image_brush_circle.cpp
  plugins/ops/09-composite/image_local_inpaint_navier_stokes.cpp
  plugins/ops/09-composite/inpaint_ns_native.cpp
  plugins/ops/09-composite/image_mask.cpp
  plugins/ops/09-composite/image_mix.cpp
  plugins/ops/09-composite/image_opacity.cpp
  plugins/ops/09-composite/image_source_over.cpp
  plugins/ops/10-analysis/analysis_histogram.cpp
  plugins/ops/10-analysis/analysis_histogram_out_of_range.cpp
)

if(PHOTOSPIDER_ENABLE_INPAINT_OPENCV)
  list(APPEND PHOTOSPIDER_OPERATION_SOURCES plugins/ops/09-composite/inpaint_ns_opencv.cpp)
endif()

set(
  PHOTOSPIDER_KERNEL_SOURCES
  ${PHOTOSPIDER_OPERATION_SOURCES}
  src/lib/plugin/builtin_operations.cpp

  src/lib/benchmark/raw_benchmark.cpp
  src/lib/compiler/compiler.cpp
  src/lib/data/dependency.cpp
  src/lib/data/dependency_metadata.cpp
  src/lib/data/dependency_mapping.cpp
  src/lib/data/footprint.cpp
  src/lib/data/value_fragments.cpp
  src/lib/data/fragment_atlas.cpp
  src/lib/data/region.cpp
  src/lib/data/region_runs.cpp
  src/lib/data/storage.cpp
  src/lib/data/temporary_storage.cpp
  src/lib/data/representation.cpp
  src/lib/data/layer.cpp
  src/lib/data/quality.cpp
  src/lib/data/statistics.cpp
  src/lib/data/lut3d_bake.cpp
  src/lib/data/lut3d_bake_validation.cpp
  src/lib/numeric/lut3d_baking.cpp
  src/lib/plugin/statistics_operation.cpp
  src/lib/plugin/fft_operation.cpp
  src/lib/plugin/component_operation.cpp
  src/lib/data/result.cpp
  src/lib/data/result_builder.cpp
  src/lib/data/result_tensor_store.cpp
  src/lib/data/result_tensor_publication.cpp
  src/lib/data/result_fields.cpp
  src/lib/data/result_tensor_window.cpp
  src/lib/data/result_capture.cpp
  src/lib/data/result_schema.cpp
  src/lib/data/result_support.cpp
  src/lib/data/result_relation.cpp
  src/lib/core/resources.cpp
  src/lib/data/resource_storage.cpp
  src/lib/core/cancellation.cpp
  src/lib/data/value.cpp
  src/lib/data/tensor_description.cpp
  src/lib/data/planar_image.cpp
  src/lib/data/semantic.cpp
  src/lib/data/color_array.cpp
  src/lib/data/icc_profile.cpp
  src/lib/data/icc_validation.cpp
  src/lib/data/ocio_config_resource.cpp
  src/lib/data/resource_bindings.cpp
  src/lib/plugin/result_program.cpp
  src/lib/plugin/result_joint.cpp
  src/lib/plugin/result_plugin_import.cpp
  src/lib/plugin/result_c_codec.cpp
  src/lib/plugin/result_c_member.cpp
  src/lib/plugin/result_c_poll_lease.cpp
  src/lib/plugin/result_c_joint.cpp
  src/lib/plugin/dependency_discovery.cpp
  src/lib/plugin/operation_contract.cpp
  src/lib/plugin/operation_semantics.cpp
  src/lib/plugin/expression.cpp
  src/lib/data/value_validation.cpp
  src/lib/plugin/port_validation.cpp
  src/lib/data/input_snapshot.cpp
  src/lib/execution/execution.cpp
  src/lib/execution/execution_context_resources.cpp
  src/lib/execution/result_run.cpp
  src/lib/execution/native_upload_codec.cpp
  src/lib/execution/execution_device.cpp
  src/lib/execution/callback_pool.cpp
  src/lib/execution/result_stage_submission.cpp
  src/lib/execution/execution_bindings.cpp
  src/lib/execution/frozen_execution.cpp
  src/lib/execution/demand_query.cpp
  src/lib/execution/demand_handle.cpp
  src/lib/execution/result_cache_proof.cpp
  src/lib/execution/result_publication_validation.cpp
  src/lib/execution/result_io.cpp
  src/lib/execution/result_fallback.cpp
  src/lib/execution/structured_joint_state.cpp
  src/lib/execution/structured_result_cache.cpp
  src/lib/execution/structured_cache_state.cpp
  src/lib/execution/structured_execution.cpp
  src/lib/execution/dependency_record.cpp
  src/lib/execution/dependency_queries.cpp
  src/lib/execution/dependency_capture.cpp
  src/lib/execution/dependency_records.cpp
  src/lib/execution/native_gpu.cpp
  src/lib/graph/graph_context.cpp
  src/lib/plugin/data_definition_registry.cpp
  src/lib/plugin/operation_registry.cpp
  src/lib/core/utf8_validation.cpp)
if(PHOTOSPIDER_ENABLE_METAL AND APPLE)
  list(APPEND PHOTOSPIDER_KERNEL_SOURCES src/lib/execution/native_gpu_metal.mm)
  set_source_files_properties(src/lib/execution/native_gpu_metal.mm PROPERTIES
    COMPILE_OPTIONS "-fobjc-arc")
endif()
if(PHOTOSPIDER_ENABLE_VULKAN)
  list(APPEND PHOTOSPIDER_KERNEL_SOURCES src/lib/execution/native_gpu_vulkan.cpp)
endif()
# S1 binary32 contracts require nearest rounding and gradual underflow at runtime.
set_source_files_properties(${PHOTOSPIDER_OPERATION_SOURCES}

  src/lib/plugin/expression.cpp src/lib/plugin/operation_semantics.cpp
  src/lib/data/value_validation.cpp
  src/lib/plugin/port_validation.cpp src/lib/compiler/compiler.cpp
  src/lib/data/layer.cpp src/lib/data/representation.cpp
  src/lib/data/quality.cpp
  src/lib/data/statistics.cpp
  src/lib/plugin/statistics_operation.cpp
  src/lib/plugin/fft_operation.cpp
  src/lib/plugin/operation_registry.cpp PROPERTIES
  COMPILE_OPTIONS "-fno-fast-math;-frounding-math;-ffp-contract=off")
# Keep optional ISA flags/definitions local; all other kernel code retains the
# ordinary host target and can perform runtime admission without SME opcodes.
set_property(SOURCE plugins/ops/01-numeric/numeric_matrix.cpp APPEND PROPERTY
  COMPILE_DEFINITIONS "PHOTOSPIDER_MATRIX_BACKEND_${PHOTOSPIDER_MATRIX_BACKEND}=1")
if(APPLE AND PHOTOSPIDER_ENABLE_ACCELERATE)
  set_property(SOURCE plugins/ops/01-numeric/accelerated_matrix_apple.cpp APPEND PROPERTY
    COMPILE_DEFINITIONS PHOTOSPIDER_HAS_ACCELERATE=1)
endif()
if(PHOTOSPIDER_ENABLE_MATRIX_SME)
  list(APPEND PHOTOSPIDER_KERNEL_SOURCES plugins/ops/01-numeric/accelerated_matrix_sme.cpp)
  set_source_files_properties(plugins/ops/01-numeric/accelerated_matrix_sme.cpp PROPERTIES
    COMPILE_OPTIONS "-march=armv8-a+sme+sme-f64f64;-fno-fast-math;-frounding-math;-ffp-contract=off")
  set_property(SOURCE plugins/ops/01-numeric/accelerated_matrix_apple.cpp APPEND PROPERTY
    COMPILE_DEFINITIONS PHOTOSPIDER_HAS_MATRIX_SME=1)
endif()
set(_numeric_conversion_sme_default OFF)
if(APPLE AND CMAKE_SYSTEM_PROCESSOR MATCHES "^(arm64|aarch64)$")
  set(_numeric_conversion_sme_default ON)
endif()
option(PHOTOSPIDER_ENABLE_NUMERIC_CONVERSION_SME "Enable runtime-dispatched SME numeric conversion tiles" ${_numeric_conversion_sme_default})
if(PHOTOSPIDER_ENABLE_NUMERIC_CONVERSION_SME)
  if(NOT APPLE OR NOT CMAKE_SYSTEM_PROCESSOR MATCHES "^(arm64|aarch64)$")
    message(FATAL_ERROR "Numeric conversion SME requires Apple arm64")
  endif()
  include(CheckCXXCompilerFlag)
  include(CheckCXXSourceCompiles)
  set(_conversion_required_flags "${CMAKE_REQUIRED_FLAGS}")
  set(CMAKE_REQUIRED_FLAGS "${CMAKE_REQUIRED_FLAGS} -march=armv8-a+sme+sme-f64f64")
  check_cxx_source_compiles("#include <arm_sme.h>
__arm_locally_streaming unsigned probe() { return svcntb(); }
int main() { return 0; }" PHOTOSPIDER_COMPILER_HAS_CONVERSION_SME_ACLE)
  set(CMAKE_REQUIRED_FLAGS "${_conversion_required_flags}")
  if(PHOTOSPIDER_COMPILER_HAS_CONVERSION_SME_ACLE)
    list(APPEND PHOTOSPIDER_KERNEL_SOURCES plugins/ops/02-format-color/numeric_conversion_sme.cpp)
    set_source_files_properties(plugins/ops/02-format-color/numeric_conversion_sme.cpp PROPERTIES
      COMPILE_OPTIONS "-march=armv8-a+sme+sme-f64f64;-fno-fast-math;-frounding-math;-ffp-contract=off")
    set_property(SOURCE plugins/ops/02-format-color/numeric_conversion.cpp APPEND PROPERTY
      COMPILE_DEFINITIONS PHOTOSPIDER_HAS_CONVERSION_SME=1)
  else()
    message(STATUS "Compiler lacks SME support; numeric conversion retains NEON")
  endif()
endif()
include(cmake/NumericMath.cmake)

# Native GPU cache identities include the maintained implementation.
file(GLOB_RECURSE _cache_identity_inputs CONFIGURE_DEPENDS
  "${CMAKE_CURRENT_SOURCE_DIR}/include/*.hpp" "${CMAKE_CURRENT_SOURCE_DIR}/include/*.h"
  "${CMAKE_CURRENT_SOURCE_DIR}/src/lib/*.cpp" "${CMAKE_CURRENT_SOURCE_DIR}/src/lib/*.hpp"
  "${CMAKE_CURRENT_SOURCE_DIR}/src/lib/*.mm" "${CMAKE_CURRENT_SOURCE_DIR}/plugins/ops/*.h"
  "${CMAKE_CURRENT_SOURCE_DIR}/plugins/ops/*.hpp"
  "${CMAKE_CURRENT_SOURCE_DIR}/plugins/ops/*.metal" "${CMAKE_CURRENT_SOURCE_DIR}/plugins/ops/*.cpp")
file(GLOB_RECURSE _numeric_math_identity_inputs CONFIGURE_DEPENDS
  "${CMAKE_CURRENT_SOURCE_DIR}/third_party/sleef/src/*.h"
  "${CMAKE_CURRENT_SOURCE_DIR}/third_party/sleef/src/*.c")
list(APPEND _cache_identity_inputs ${_numeric_math_identity_inputs}
  "${CMAKE_CURRENT_SOURCE_DIR}/third_party/photospider_sleef.c"
  "${CMAKE_CURRENT_SOURCE_DIR}/cmake/NumericMath.cmake"
  "${CMAKE_CURRENT_SOURCE_DIR}/CMakeLists.txt")
list(SORT _cache_identity_inputs)
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS ${_cache_identity_inputs})
string(TOUPPER "${CMAKE_BUILD_TYPE}" _cache_config)
set(_cache_identity "fmt10-algorithm=${PHOTOSPIDER_FMT10_ALGORITHM};fmt10-exact=${PHOTOSPIDER_FMT10_EXACT_KERNEL};fmt10-simd=${PHOTOSPIDER_FMT10_SIMD};transfer-compact=${PHOTOSPIDER_TRANSFER_COMPACT_MATH};transfer-gamma2-simd=${PHOTOSPIDER_TRANSFER_GAMMA2_SIMD};transfer-filter=${PHOTOSPIDER_TRANSFER_FAST_MATH};matrix-backend=${PHOTOSPIDER_MATRIX_BACKEND};sme=${PHOTOSPIDER_ENABLE_MATRIX_SME};${CMAKE_CXX_COMPILER_ID};${CMAKE_CXX_COMPILER_VERSION};${CMAKE_SYSTEM_NAME};${CMAKE_SYSTEM_VERSION};${CMAKE_SYSTEM_PROCESSOR};${CMAKE_CXX_FLAGS};${CMAKE_CXX_FLAGS_${_cache_config}};${BUILD_SHARED_LIBS};${PHOTOSPIDER_SANITIZER_MODE};accelerate=${PHOTOSPIDER_ENABLE_ACCELERATE}")
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
# Common translation units are compiled once for the package and internal tests.
set(PHOTOSPIDER_SEAM_SOURCES
  src/lib/benchmark/raw_benchmark.cpp
  src/lib/execution/execution_context_resources.cpp
  src/lib/execution/result_run.cpp
  src/lib/execution/native_upload_codec.cpp
  src/lib/execution/execution_device.cpp
  src/lib/execution/callback_pool.cpp
  src/lib/execution/result_stage_submission.cpp
  src/lib/execution/structured_execution.cpp
  src/lib/plugin/data_definition_registry.cpp
  src/lib/plugin/operation_registry.cpp)
foreach(_native IN ITEMS src/lib/execution/native_gpu_metal.mm src/lib/execution/native_gpu_vulkan.cpp)
  if(_native IN_LIST PHOTOSPIDER_KERNEL_SOURCES)
    list(APPEND PHOTOSPIDER_SEAM_SOURCES ${_native})
  endif()
endforeach()
set(PHOTOSPIDER_COMMON_SOURCES ${PHOTOSPIDER_KERNEL_SOURCES})
list(REMOVE_ITEM PHOTOSPIDER_COMMON_SOURCES ${PHOTOSPIDER_SEAM_SOURCES})
add_library(photospider_common_objects OBJECT ${PHOTOSPIDER_COMMON_SOURCES})
add_library(photospider_runtime_objects OBJECT ${PHOTOSPIDER_SEAM_SOURCES})
add_library(photospider
  $<TARGET_OBJECTS:photospider_common_objects>
  $<TARGET_OBJECTS:photospider_runtime_objects>)
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
  get_target_property(_kind ${target} TYPE)
  if(NOT _kind STREQUAL "OBJECT_LIBRARY")
    target_sources(${target} PRIVATE $<TARGET_OBJECTS:photospider_numeric_math>)
  endif()
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
    PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src/lib ${CMAKE_CURRENT_SOURCE_DIR}/plugins/ops ${CMAKE_CURRENT_BINARY_DIR}/generated/$<CONFIG> ${CMAKE_CURRENT_BINARY_DIR}/generated
      ${CMAKE_CURRENT_BINARY_DIR}/plugins/ops/rgba32f)
  target_link_libraries(
    ${target} PUBLIC Threads::Threads PRIVATE ${CMAKE_DL_LIBS})
  target_compile_definitions(${target} PRIVATE PHOTOSPIDER_BUILD)
  if(PHOTOSPIDER_ENABLE_INPAINT_OPENCV)
    target_compile_definitions(${target} PRIVATE PHOTOSPIDER_HAS_INPAINT_OPENCV)
    target_include_directories(${target} SYSTEM PRIVATE ${OpenCV_INCLUDE_DIRS})
    target_link_libraries(${target} PRIVATE opencv_core opencv_photo)
  endif()
  if(APPLE AND PHOTOSPIDER_ENABLE_ACCELERATE)
    target_link_libraries(${target} PRIVATE "-framework Accelerate")
  endif()
  if(PHOTOSPIDER_ENABLE_METAL AND APPLE)
    target_compile_definitions(${target} PRIVATE PHOTOSPIDER_HAS_METAL)
    target_link_libraries(${target} PRIVATE "-framework Metal" "-framework Foundation")
    set_target_properties(${target} PROPERTIES OBJCXX_STANDARD 17 OBJCXX_STANDARD_REQUIRED ON)
  endif()
  if(PHOTOSPIDER_ENABLE_VULKAN)
    target_compile_definitions(${target} PRIVATE PHOTOSPIDER_HAS_VULKAN)
    target_link_libraries(${target} PRIVATE Vulkan::Vulkan)
  endif()

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

photospider_configure_kernel_target(photospider_common_objects)
photospider_configure_kernel_target(photospider_runtime_objects)
photospider_configure_kernel_target(photospider)
set_target_properties(photospider PROPERTIES
  EXPORT_NAME kernel
  VERSION ${PROJECT_VERSION}
  SOVERSION "${PROJECT_VERSION_MAJOR}.${PROJECT_VERSION_MINOR}")
if(NOT BUILD_SHARED_LIBS)
  target_compile_definitions(photospider PUBLIC PHOTOSPIDER_STATIC)
  target_compile_definitions(photospider_common_objects PRIVATE PHOTOSPIDER_STATIC)
  target_compile_definitions(photospider_runtime_objects PRIVATE PHOTOSPIDER_STATIC)
endif()

if(PHOTOSPIDER_BUILD_MANUAL_FUZZ_TARGETS)
  if(PHOTOSPIDER_ENABLE_TSAN)
    message(FATAL_ERROR "manual libFuzzer targets cannot be combined with TSAN")
  endif()
  foreach(_objects IN ITEMS photospider_common_objects photospider_runtime_objects)
    target_compile_options(${_objects} PRIVATE -fsanitize=fuzzer-no-link -fno-omit-frame-pointer)
  endforeach()
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
  FILES_MATCHING PATTERN "*.hpp" PATTERN "*.h" PATTERN "format" EXCLUDE)
install(FILES include/photospider/format/alpha.hpp include/photospider/format/channel.hpp include/photospider/format/channel_assembly.hpp include/photospider/format/channel_editing.hpp include/photospider/format/metadata.hpp include/photospider/format/transfer.hpp include/photospider/format/rgb_basis.hpp include/photospider/format/model_conversion.hpp DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}/photospider/format)
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
add_subdirectory(examples/inpaint_ns_workflow EXCLUDE_FROM_ALL)
add_subdirectory(examples/metadata_workflow EXCLUDE_FROM_ALL)
add_subdirectory(examples/metadata_performance EXCLUDE_FROM_ALL)
add_subdirectory(examples/channel_editing EXCLUDE_FROM_ALL)
add_subdirectory(examples/channel_assembly_performance EXCLUDE_FROM_ALL)
add_subdirectory(examples/alpha_performance EXCLUDE_FROM_ALL)
add_subdirectory(examples/numeric_conversion_performance EXCLUDE_FROM_ALL)
add_subdirectory(examples/transfer_performance EXCLUDE_FROM_ALL)
add_subdirectory(examples/rgb_basis_performance EXCLUDE_FROM_ALL)
add_subdirectory(examples/model_conversion_performance EXCLUDE_FROM_ALL)
add_subdirectory(examples/channel_editing_performance EXCLUDE_FROM_ALL)
add_subdirectory(examples/channel_extraction_workflow EXCLUDE_FROM_ALL)
add_subdirectory(examples/channel_extraction_performance EXCLUDE_FROM_ALL)
add_subdirectory(examples/image_vertical EXCLUDE_FROM_ALL)
add_subdirectory(examples/regional_image_vertical EXCLUDE_FROM_ALL)
add_subdirectory(examples/foundations_workflow EXCLUDE_FROM_ALL)
add_subdirectory(examples/numeric_workflow EXCLUDE_FROM_ALL)
add_subdirectory(examples/codec_performance EXCLUDE_FROM_ALL)
add_subdirectory(examples/dynamic_scheduler EXCLUDE_FROM_ALL)
add_subdirectory(examples/perlin_workflow EXCLUDE_FROM_ALL)
add_subdirectory(examples/gaussian_workflow EXCLUDE_FROM_ALL)
add_subdirectory(examples/multi_output_workflow EXCLUDE_FROM_ALL)
add_subdirectory(examples/unified_result_workflow EXCLUDE_FROM_ALL)
add_subdirectory(examples/result_quickstart EXCLUDE_FROM_ALL)

if(BUILD_TESTING)
  photospider_register_test(NAME test_result_quickstart COMMAND photospider_result_quickstart)
  add_library(photospider_internal_kernel STATIC EXCLUDE_FROM_ALL
    $<TARGET_OBJECTS:photospider_common_objects>
    $<TARGET_OBJECTS:photospider_runtime_objects>)
  photospider_configure_kernel_target(photospider_internal_kernel)
  target_compile_definitions(photospider_internal_kernel PUBLIC PHOTOSPIDER_STATIC)

  # Fault injection recompiles only translation units that contain a seam.
  add_library(
    photospider_test_kernel STATIC EXCLUDE_FROM_ALL
    $<TARGET_OBJECTS:photospider_common_objects>
    ${PHOTOSPIDER_SEAM_SOURCES}
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

  if(PHOTOSPIDER_BUILD_MANUAL_FUZZ_TARGETS)
    foreach(_archive IN ITEMS photospider_internal_kernel photospider_test_kernel)
      target_link_options(${_archive} INTERFACE "$<BUILD_INTERFACE:-fsanitize=fuzzer-no-link>")
    endforeach()
  endif()

  function(photospider_add_test target kernel_target)
    add_executable(${target} ${ARGN})
    target_link_libraries(${target} PRIVATE ${kernel_target})
    target_include_directories(${target} PRIVATE tests)
    target_compile_features(${target} PRIVATE cxx_std_17)
    if(CMAKE_CXX_COMPILER_ID MATCHES "Clang")
      target_compile_options(${target} PRIVATE -Wall -Wextra -Wpedantic -Werror)
    endif()
    photospider_register_test(NAME ${target} COMMAND ${target})
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

  add_library(multi_output_fixture MODULE tests/fixtures/multi_output_fixture.c)
  target_include_directories(multi_output_fixture PRIVATE include)
  set_target_properties(multi_output_fixture PROPERTIES C_STANDARD 11 C_STANDARD_REQUIRED ON)
  target_compile_definitions(multi_output_fixture PRIVATE PHOTOSPIDER_OPERATION_PLUGIN_BUILD)
  photospider_add_test(test_multi_output_contract Photospider::kernel tests/unit/test_multi_output_contract.cpp)
  target_compile_definitions(test_multi_output_contract PRIVATE PS_MULTI_OUTPUT_FIXTURE="$<TARGET_FILE:multi_output_fixture>")
  add_dependencies(test_multi_output_contract multi_output_fixture)
  photospider_add_test(test_joint_execution Photospider::kernel tests/integration/test_joint_execution.cpp)
  photospider_register_test(NAME test_result_joint_color_validation COMMAND photospider_numeric_color_array --joint-validation)
  photospider_register_test(NAME test_numeric_color_array_result COMMAND photospider_numeric_color_array)
  add_dependencies(test_joint_execution photospider_numeric_color_array)
  photospider_add_test(test_multi_output_execution Photospider::kernel tests/integration/test_multi_output_execution.cpp)
  photospider_add_test(test_result_c_checkpoint Photospider::kernel tests/integration/test_result_c_checkpoint.cpp)
  photospider_add_test(test_result_c_block Photospider::kernel tests/integration/test_result_c_block.cpp)
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
  photospider_add_test(test_result_c_joint Photospider::kernel tests/integration/test_result_c_joint.cpp)
  photospider_add_result_table_fixture(result_joint_fixture tests/fixtures/result_joint_fixture.c)
  target_compile_definitions(test_result_c_joint PRIVATE PS_RESULT_JOINT_FIXTURE="$<TARGET_FILE:result_joint_fixture>")
  add_dependencies(test_result_c_joint result_joint_fixture)
  add_library(result_joint_throw_fixture MODULE tests/fixtures/result_joint_throw_fixture.cpp)
  target_link_libraries(result_joint_throw_fixture PRIVATE Photospider::operation_sdk Threads::Threads)
  target_compile_features(result_joint_throw_fixture PRIVATE cxx_std_17)
  target_compile_definitions(test_result_c_joint PRIVATE PS_RESULT_JOINT_THROW_FIXTURE="$<TARGET_FILE:result_joint_throw_fixture>")
  add_dependencies(test_result_c_joint result_joint_throw_fixture)
  photospider_add_result_table_fixture(result_bad_joint_fixture tests/fixtures/result_joint_fixture.c BAD)
  target_compile_definitions(test_result_c_joint PRIVATE PS_BAD_JOINT_FIXTURE=\"$<TARGET_FILE:result_bad_joint_fixture>\")
  photospider_add_test(test_result_joint Photospider::kernel tests/integration/test_result_joint.cpp)
  photospider_add_test(test_result_exceptions Photospider::kernel tests/integration/test_result_exceptions.cpp)
  photospider_add_test(test_result_queries Photospider::kernel tests/integration/test_result_queries.cpp)
  photospider_add_test(test_result_c_atoms Photospider::kernel tests/integration/test_result_c_atoms.cpp)
  add_library(result_atoms_fixture MODULE tests/fixtures/result_atoms_fixture.c)
  target_link_libraries(result_atoms_fixture PRIVATE Photospider::operation_sdk)
  set_target_properties(result_atoms_fixture PROPERTIES C_STANDARD 11 C_STANDARD_REQUIRED ON C_EXTENSIONS OFF)
  target_compile_definitions(test_result_c_atoms PRIVATE PS_RESULT_ATOMS_FIXTURE="$<TARGET_FILE:result_atoms_fixture>")
  add_dependencies(test_result_c_atoms result_atoms_fixture)
  add_library(result_atoms_wide_fixture MODULE tests/fixtures/result_atoms_fixture.c)
  target_link_libraries(result_atoms_wide_fixture PRIVATE Photospider::operation_sdk)
  target_compile_definitions(result_atoms_wide_fixture PRIVATE PS_RESULT_ATOMS_WIDTH=64)
  set_target_properties(result_atoms_wide_fixture PROPERTIES C_STANDARD 11 C_STANDARD_REQUIRED ON C_EXTENSIONS OFF)
  target_compile_definitions(test_result_c_atoms PRIVATE PS_RESULT_ATOMS_WIDE_FIXTURE="$<TARGET_FILE:result_atoms_wide_fixture>")
  add_dependencies(test_result_c_atoms result_atoms_wide_fixture)
  photospider_add_test(test_result_atoms Photospider::kernel tests/integration/test_result_atoms.cpp)
  photospider_add_test(test_result_joint_retirement photospider_test_kernel tests/integration/test_result_joint.cpp)
  target_include_directories(test_result_joint_retirement PRIVATE src/lib)
  target_compile_definitions(test_result_joint_retirement PRIVATE PHOTOSPIDER_JOINT_RETIREMENT_TEST)
  photospider_add_test(test_result_request_record Photospider::kernel tests/integration/test_result_request_record.cpp)
  photospider_add_result_table_fixture(result_request_record_fixture tests/fixtures/result_request_record_fixture.c)
  target_compile_definitions(test_result_request_record PRIVATE PS_REQUEST_RECORD_FIXTURE="$<TARGET_FILE:result_request_record_fixture>")
  add_dependencies(test_result_request_record result_request_record_fixture)
  photospider_add_result_table_fixture(result_bad_request_record_fixture tests/fixtures/result_request_record_fixture.c BAD)
  target_compile_definitions(test_result_request_record PRIVATE PS_BAD_REQUEST_RECORD_FIXTURE=\"$<TARGET_FILE:result_bad_request_record_fixture>\")
  target_compile_definitions(test_multi_output_execution PRIVATE PS_MULTI_OUTPUT_FIXTURE="$<TARGET_FILE:multi_output_fixture>")
  add_dependencies(test_multi_output_execution multi_output_fixture)
  photospider_add_test(test_semantic_contract Photospider::kernel tests/unit/test_semantic_contract.cpp)
  add_library(operation_contract_fixture MODULE tests/fixtures/operation_contract_fixture.c)
  target_link_libraries(operation_contract_fixture PRIVATE Photospider::operation_sdk)
  set_target_properties(operation_contract_fixture PROPERTIES C_STANDARD 11 C_STANDARD_REQUIRED ON)
  target_compile_definitions(test_semantic_contract PRIVATE PS_CONTRACT_FIXTURE="$<TARGET_FILE:operation_contract_fixture>")
  add_dependencies(test_semantic_contract operation_contract_fixture)
  foreach(_case RANGE 1 12)
    add_library(operation_bad_contract_${_case} MODULE tests/fixtures/operation_contract_fixture.c)
    target_link_libraries(operation_bad_contract_${_case} PRIVATE Photospider::operation_sdk)
    set_target_properties(operation_bad_contract_${_case} PROPERTIES C_STANDARD 11 C_STANDARD_REQUIRED ON)
    target_compile_definitions(operation_bad_contract_${_case} PRIVATE PS_BAD_CONTRACT_CASE=${_case})
    target_compile_definitions(test_semantic_contract PRIVATE PS_BAD_CONTRACT_${_case}="$<TARGET_FILE:operation_bad_contract_${_case}>")
    add_dependencies(test_semantic_contract operation_bad_contract_${_case})
  endforeach()

  photospider_add_test(test_generic_binding_identity Photospider::kernel tests/unit/test_generic_binding_identity.cpp)
  target_include_directories(test_generic_binding_identity PRIVATE examples)
  set_property(TARGET photospider_image_vertical photospider_regional_image_vertical PROPERTY EXCLUDE_FROM_ALL FALSE)
  photospider_register_test(NAME example_image_vertical COMMAND photospider_image_vertical)
  photospider_register_test(NAME example_regional_image_vertical COMMAND photospider_regional_image_vertical)
  set_property(TARGET photospider_multi_output_workflow PROPERTY EXCLUDE_FROM_ALL FALSE)
  foreach(_joint on off)
    photospider_register_test(NAME example_multi_output_${_joint} COMMAND photospider_multi_output_workflow --joint ${_joint})
  endforeach()
  set_property(TARGET photospider_foundations_workflow PROPERTY EXCLUDE_FROM_ALL FALSE)
  photospider_register_test(NAME test_workflow_numeric_reductions COMMAND photospider_foundations_workflow --scenario numeric)
  photospider_register_test(NAME test_workflow_expression_lut COMMAND photospider_foundations_workflow --scenario expression-lut)
  photospider_add_test(test_basic_operations Photospider::kernel tests/integration/test_basic_operations.cpp)
  target_include_directories(test_basic_operations PRIVATE examples)
  photospider_add_test(test_expression_operations Photospider::kernel tests/integration/test_expression_operations.cpp)
  add_library(expression_contract_fixture MODULE
    tests/fixtures/expression_contract_fixture.c
    tests/fixtures/expression_contract_metadata.cpp)
  target_link_libraries(expression_contract_fixture PRIVATE Photospider::kernel)
  target_compile_definitions(expression_contract_fixture PRIVATE PHOTOSPIDER_OPERATION_PLUGIN_BUILD)
  set_target_properties(expression_contract_fixture PROPERTIES C_STANDARD 11 C_STANDARD_REQUIRED ON)
  target_compile_definitions(test_expression_operations PRIVATE PS_EXPRESSION_CONTRACT_FIXTURE="$<TARGET_FILE:expression_contract_fixture>")
  add_dependencies(test_expression_operations expression_contract_fixture)
  photospider_add_test(test_alpha_operations Photospider::kernel tests/integration/test_alpha_operations.cpp)
  photospider_add_test(test_numeric_conversion Photospider::kernel tests/integration/test_numeric_conversion.cpp)
  target_include_directories(test_numeric_conversion PRIVATE examples)
  photospider_add_test(test_alpha_numeric_interop Photospider::kernel tests/integration/test_alpha_numeric_interop.cpp)
  target_include_directories(test_alpha_numeric_interop PRIVATE examples)
  target_include_directories(test_alpha_operations PRIVATE examples)
  photospider_add_test(test_alpha_authoring Photospider::kernel tests/integration/test_alpha_authoring.cpp)
  target_include_directories(test_alpha_authoring PRIVATE examples)
  photospider_add_test(test_alpha_math photospider_internal_kernel tests/unit/test_alpha_math.cpp)
  target_include_directories(test_alpha_math PRIVATE plugins/ops src/lib)
  target_compile_options(test_alpha_math PRIVATE -fno-fast-math -frounding-math -ffp-contract=off)
  photospider_add_test(test_channel_assembly Photospider::kernel tests/integration/test_channel_assembly.cpp)
  target_include_directories(test_channel_assembly PRIVATE examples examples/numeric_workflow)
  photospider_add_test(test_channel_editing Photospider::kernel tests/integration/test_channel_editing.cpp)
  target_include_directories(test_channel_editing PRIVATE examples)
  photospider_add_test(test_channel_literal_like Photospider::kernel tests/integration/test_channel_literal_like.cpp)
  target_include_directories(test_channel_literal_like PRIVATE examples)
  photospider_add_test(test_channel_extraction Photospider::kernel tests/integration/test_channel_extraction.cpp)
  target_include_directories(test_channel_extraction PRIVATE examples examples/numeric_workflow)
  photospider_add_test(test_metadata_assignment Photospider::kernel tests/integration/test_metadata_assignment.cpp)
  target_include_directories(test_metadata_assignment PRIVATE examples/numeric_workflow)
  photospider_add_test(test_model_metadata Photospider::kernel tests/integration/test_model_metadata.cpp)
  add_executable(test_model_rational tests/unit/test_model_rational.cpp)
  target_link_libraries(test_model_rational PRIVATE Photospider::kernel)
  target_compile_options(test_model_rational PRIVATE -Wall -Wextra -Wpedantic -Werror)
  # This test reads an independently generated exact Fraction oracle.
  photospider_register_test(NAME test_model_rational COMMAND test_model_rational
    "${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures/fmt11/rational_oracles.txt")
  photospider_add_test(test_numeric_neighbors Photospider::kernel tests/unit/test_numeric_neighbors.cpp)
  foreach(_fmt11_test IN ITEMS test_model_rational test_numeric_neighbors)
    target_include_directories(${_fmt11_test} PRIVATE plugins/ops src/lib)
    target_compile_options(${_fmt11_test} PRIVATE -fno-fast-math -frounding-math -ffp-contract=off)
  endforeach()
  photospider_register_test(NAME test_package_version
    COMMAND ${CMAKE_COMMAND}
      -DVERSION_FILE=${CMAKE_CURRENT_BINARY_DIR}/PhotospiderConfigVersion.cmake
      -DCURRENT_VERSION=${PROJECT_VERSION}
      -P ${CMAKE_CURRENT_SOURCE_DIR}/tests/cmake/test_package_version.cmake)
  photospider_add_test(test_model_math photospider_internal_kernel tests/unit/test_model_math.cpp)
  target_include_directories(test_model_math PRIVATE plugins/ops src/lib)
  target_compile_options(test_model_math PRIVATE -fno-fast-math -frounding-math -ffp-contract=off)
  photospider_add_test(test_model_conversion Photospider::kernel tests/integration/test_model_conversion.cpp)
  target_include_directories(test_model_conversion PRIVATE examples)
  photospider_add_test(test_model_result Photospider::kernel tests/integration/test_model_result.cpp)
  target_include_directories(test_model_result PRIVATE examples)
  target_compile_options(test_model_result PRIVATE -fno-fast-math -frounding-math -ffp-contract=off)
  photospider_add_test(test_alpha_model_interop Photospider::kernel tests/integration/test_alpha_model_interop.cpp)
  target_include_directories(test_alpha_model_interop PRIVATE examples)
  photospider_add_test(test_rgb_basis Photospider::kernel tests/integration/test_rgb_basis.cpp)
  target_include_directories(test_rgb_basis PRIVATE examples)
  target_compile_options(test_rgb_basis PRIVATE -fno-fast-math -frounding-math -ffp-contract=off)
  photospider_add_test(test_rgb_basis_math photospider_internal_kernel tests/unit/test_rgb_basis_math.cpp)
  target_include_directories(test_rgb_basis_math PRIVATE src/lib plugins/ops)
  target_compile_options(test_rgb_basis_math PRIVATE -fno-fast-math -frounding-math -ffp-contract=off)
  photospider_add_test(test_transfer_simd photospider_internal_kernel tests/unit/test_transfer_simd.cpp)
  photospider_add_test(test_transfer_operations Photospider::kernel tests/integration/test_transfer_operations.cpp)
  target_include_directories(test_transfer_operations PRIVATE examples)
  target_compile_options(test_transfer_operations PRIVATE -fno-fast-math -frounding-math -ffp-contract=off)
  photospider_add_test(test_transfer_runtime Photospider::kernel tests/integration/test_transfer_runtime.cpp)
  target_include_directories(test_transfer_runtime PRIVATE examples)
  target_compile_options(test_transfer_runtime PRIVATE -fno-fast-math -frounding-math -ffp-contract=off)
  target_include_directories(test_transfer_simd PRIVATE src/lib plugins/ops)
  target_compile_options(test_transfer_simd PRIVATE -fno-fast-math -frounding-math -ffp-contract=off)
  set_tests_properties(test_transfer_simd PROPERTIES SKIP_RETURN_CODE 77)
  photospider_add_test(test_transfer_math photospider_internal_kernel tests/unit/test_transfer_math.cpp)
  target_include_directories(test_transfer_math PRIVATE src/lib plugins/ops)
  target_compile_options(test_transfer_math PRIVATE -fno-fast-math -frounding-math -ffp-contract=off)
  if(PHOTOSPIDER_ENABLE_NUMERIC_CONVERSION_SME AND PHOTOSPIDER_COMPILER_HAS_CONVERSION_SME_ACLE)
    add_executable(test_numeric_conversion_sme tests/unit/test_numeric_conversion_sme.cpp plugins/ops/02-format-color/numeric_conversion_sme.cpp)
    target_include_directories(test_numeric_conversion_sme PRIVATE include src/lib tests)
    target_compile_features(test_numeric_conversion_sme PRIVATE cxx_std_17)
    target_link_libraries(test_numeric_conversion_sme PRIVATE Threads::Threads)
    photospider_register_test(NAME test_numeric_conversion_sme COMMAND test_numeric_conversion_sme)
    set_tests_properties(test_numeric_conversion_sme PROPERTIES SKIP_RETURN_CODE 77)
  endif()
  photospider_add_test(test_numeric_operations Photospider::kernel tests/integration/test_numeric_operations.cpp)
  foreach(_numeric_family IN ITEMS arithmetic arrays sequences statistics matrix calculus curves bezier lut1d lut3d baking expression filters color_ramps)
    photospider_add_test(test_numeric_result_math_${_numeric_family} Photospider::kernel
      tests/integration/numeric/test_numeric_result_math_${_numeric_family}.cpp)
  endforeach()
  target_include_directories(test_numeric_result_math_arrays PRIVATE src/lib)
  photospider_register_test(NAME test_numeric_unary_result COMMAND photospider_numeric_unary strict)
  photospider_register_test(NAME test_numeric_binary_result COMMAND photospider_numeric_binary strict)
  photospider_register_test(NAME test_numeric_comparisons_result COMMAND photospider_numeric_comparisons _strict)
  photospider_register_test(NAME test_numeric_expression_result COMMAND photospider_numeric_expression strict)
  photospider_register_test(NAME test_numeric_sequences_result COMMAND photospider_numeric_sequences strict)
  photospider_register_test(NAME test_numeric_ranges_result COMMAND photospider_numeric_ranges _strict)
  photospider_register_test(NAME test_numeric_interpolation_result COMMAND photospider_numeric_interpolation _strict)
  photospider_register_test(NAME test_numeric_layouts_result COMMAND photospider_numeric_layouts strict)
  photospider_register_test(NAME test_numeric_indexing_result COMMAND photospider_numeric_indexing strict)
  photospider_register_test(NAME test_numeric_reductions_result COMMAND photospider_numeric_reductions strict)
  photospider_register_test(NAME test_numeric_ordering_result COMMAND photospider_numeric_ordering strict)
  photospider_register_test(NAME test_numeric_scans_result COMMAND photospider_numeric_scans strict)
  photospider_register_test(NAME test_numeric_matrix_result COMMAND photospider_numeric_matrix strict)
  photospider_register_test(NAME test_numeric_curves_result COMMAND photospider_numeric_curves strict)
  photospider_register_test(NAME test_numeric_calculus_result COMMAND photospider_numeric_calculus strict)
  photospider_register_test(NAME test_numeric_lut1d_result COMMAND photospider_numeric_lut1d strict)
  photospider_register_test(NAME test_numeric_bezier_result COMMAND photospider_numeric_bezier strict)
  photospider_register_test(NAME test_numeric_parametric_result COMMAND photospider_numeric_parametric strict)
  photospider_register_test(NAME test_numeric_baking_result COMMAND photospider_numeric_baking strict)
  photospider_register_test(NAME test_numeric_baking3d_result COMMAND photospider_numeric_baking3d strict)
  photospider_register_test(NAME test_numeric_lowpass_execution_result COMMAND photospider_numeric_lowpass_execution strict)
  photospider_register_test(NAME test_numeric_lowpass_nonuniform_result COMMAND photospider_numeric_lowpass_nonuniform strict)
  photospider_register_test(NAME test_numeric_lowpass_result COMMAND photospider_numeric_lowpass strict)
  photospider_register_test(NAME test_numeric_resampling_result COMMAND photospider_numeric_resampling strict)
  photospider_register_test(NAME test_numeric_lut3d_result COMMAND photospider_numeric_lut3d strict)
  photospider_register_test(NAME test_numeric_inverse_result COMMAND photospider_numeric_inverse strict)
  photospider_register_test(NAME test_numeric_math_batch COMMAND photospider_numeric_math_batch)
  set_tests_properties(test_numeric_math_batch PROPERTIES SKIP_RETURN_CODE 77)
  photospider_add_test(test_matrix_certificate photospider_internal_kernel tests/unit/test_matrix_certificate.cpp)
  target_include_directories(test_matrix_certificate PRIVATE plugins/ops src/lib)
  target_compile_options(test_matrix_certificate PRIVATE -fno-fast-math -frounding-math -ffp-contract=off)
  photospider_add_test(test_ordered_reduction Photospider::kernel tests/integration/test_ordered_reduction.cpp)
  photospider_add_test(test_ordered_scan Photospider::kernel tests/integration/test_ordered_scan.cpp)
  photospider_add_test(test_scan_waiters photospider_test_kernel tests/integration/test_scan_waiters.cpp)
  target_include_directories(test_scan_waiters PRIVATE src/lib)
  # This test directly exercises private cache validation and allocation code.
  photospider_add_test(test_generic_result_cache photospider_internal_kernel tests/integration/test_generic_result_cache.cpp)
  target_include_directories(test_generic_result_cache PRIVATE src/lib)
  photospider_add_test(test_frozen_execution Photospider::kernel tests/integration/test_frozen_execution.cpp)
  photospider_add_test(test_dependency_sampling photospider_test_kernel tests/integration/test_dependency_sampling.cpp)
  target_include_directories(test_dependency_sampling PRIVATE src/lib)
  photospider_add_test(test_execution_dependencies Photospider::kernel tests/integration/test_execution_dependencies.cpp)
  photospider_add_test(test_execution_demand Photospider::kernel tests/integration/test_execution_demand.cpp)
  photospider_add_test(test_dependency_joint Photospider::kernel tests/integration/test_dependency_joint.cpp)
  photospider_add_test(test_dependency_program photospider_internal_kernel tests/integration/test_dependency_program.cpp)
  photospider_add_test(test_numeric_prepared Photospider::kernel examples/numeric_workflow/prepared.cpp)
  target_include_directories(test_dependency_program PRIVATE src/lib)
  photospider_add_test(test_dependency_dirty Photospider::kernel tests/unit/test_dependency_dirty.cpp)
  target_include_directories(test_dependency_dirty PRIVATE src/lib)
  photospider_add_test(test_dependency Photospider::kernel tests/unit/test_dependency.cpp)
  add_executable(test_dependency_workflows
    tests/integration/dependency_workflows/main.cpp
    tests/integration/dependency_workflows/progressive.cpp
    tests/integration/dependency_workflows/dynamic.cpp
    tests/integration/dependency_workflows/demand.cpp
    tests/integration/dependency_workflows/sharing.cpp
    tests/integration/dependency_workflows/reductions.cpp
    tests/integration/dependency_workflows/scan.cpp
    tests/integration/dependency_workflows/blocks.cpp)
  target_link_libraries(test_dependency_workflows PRIVATE Photospider::kernel)
  target_include_directories(test_dependency_workflows PRIVATE tests ${CMAKE_CURRENT_SOURCE_DIR})
  foreach(_behavior IN ITEMS data progressive shared reductions scan blocks dynamic demand radius)
    photospider_register_test(NAME test_dependency_workflows_${_behavior}
      COMMAND test_dependency_workflows --scenario ${_behavior})
  endforeach()
  photospider_add_test(test_footprint Photospider::kernel tests/unit/test_footprint.cpp)
  photospider_add_test(test_value_runs Photospider::kernel tests/unit/test_value_runs.cpp)
  photospider_add_test(test_value_fragments Photospider::kernel tests/unit/test_value_fragments.cpp)
  photospider_add_test(test_fragment_atlas Photospider::kernel tests/unit/test_fragment_atlas.cpp)
  photospider_add_test(test_input_snapshot Photospider::kernel tests/unit/test_input_snapshot.cpp)
  target_include_directories(test_input_snapshot PRIVATE src/lib)
  photospider_add_test(test_scheduler_timing Photospider::kernel tests/integration/test_scheduler_timing.cpp)
  target_include_directories(test_scheduler_timing PRIVATE src/lib)
  target_compile_definitions(test_scheduler_timing PRIVATE PHOTOSPIDER_TEST_QUEUE_METER)
  photospider_add_test(test_native_execution Photospider::kernel tests/integration/test_native_execution.cpp)
  set_tests_properties(test_native_execution PROPERTIES SKIP_RETURN_CODE 77)
  photospider_add_test(test_native_dependency photospider_test_kernel tests/integration/test_native_dependency.cpp)
  target_include_directories(test_native_dependency PRIVATE src/lib)
  set_tests_properties(test_native_dependency PROPERTIES SKIP_RETURN_CODE 77)
  photospider_add_test(test_dependency_gpu Photospider::kernel tests/unit/test_dependency_gpu.cpp)
  set_tests_properties(test_dependency_gpu PROPERTIES SKIP_RETURN_CODE 77)
  photospider_add_test(test_gpu_sync_fallback Photospider::kernel tests/integration/gpu/test_gpu_sync_fallback.cpp)
  photospider_add_test(test_result_gpu_service Photospider::kernel tests/unit/test_result_gpu_service.cpp)
  target_include_directories(test_result_gpu_service PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src/lib")
  set_tests_properties(test_gpu_sync_fallback PROPERTIES SKIP_RETURN_CODE 77)
  photospider_add_test(test_gpu_discovery Photospider::kernel tests/unit/test_gpu_discovery.cpp)
  set_tests_properties(test_gpu_discovery PROPERTIES SKIP_RETURN_CODE 77)
  photospider_add_test(test_result_gpu_discovery photospider_internal_kernel tests/unit/test_result_gpu_discovery.cpp)
  target_include_directories(test_result_gpu_discovery PRIVATE src/lib)
  add_library(gpu_discovery_fixture MODULE tests/fixtures/gpu_result_discovery_plugin.c)
  target_link_libraries(gpu_discovery_fixture PRIVATE Photospider::operation_sdk)
  target_compile_definitions(gpu_discovery_fixture PRIVATE PHOTOSPIDER_OPERATION_PLUGIN_BUILD)
  set_target_properties(gpu_discovery_fixture PROPERTIES C_STANDARD 11 C_STANDARD_REQUIRED ON)
  photospider_add_test(test_gpu_discovery_workflow Photospider::kernel tests/integration/gpu/test_gpu_discovery_workflow.cpp)
  target_compile_definitions(test_gpu_discovery_workflow PRIVATE PS_GPU_DISCOVERY_PLUGIN="$<TARGET_FILE:gpu_discovery_fixture>")
  add_dependencies(test_gpu_discovery_workflow gpu_discovery_fixture)
  set_tests_properties(test_gpu_discovery_workflow PROPERTIES SKIP_RETURN_CODE 77)
  add_library(gpu_dependency_c_fixture MODULE tests/fixtures/gpu_result_c_plugin.c)
  target_link_libraries(gpu_dependency_c_fixture PRIVATE Photospider::operation_sdk)
  target_compile_definitions(gpu_dependency_c_fixture PRIVATE PHOTOSPIDER_OPERATION_PLUGIN_BUILD)
  set_target_properties(gpu_dependency_c_fixture PROPERTIES C_STANDARD 11 C_STANDARD_REQUIRED ON)
  photospider_add_test(test_gpu_c_abi_execution Photospider::kernel tests/integration/gpu/test_gpu_c_abi_execution.cpp)
  target_compile_definitions(test_gpu_c_abi_execution PRIVATE PS_GPU_DEPENDENCY_PLUGIN="$<TARGET_FILE:gpu_dependency_c_fixture>")
  add_dependencies(test_gpu_c_abi_execution gpu_dependency_c_fixture)
  set_tests_properties(test_gpu_c_abi_execution PROPERTIES SKIP_RETURN_CODE 77)
  photospider_add_test(test_gpu_fragment_execution Photospider::kernel tests/integration/gpu/test_gpu_fragment_execution.cpp)
  set_tests_properties(test_gpu_fragment_execution PROPERTIES SKIP_RETURN_CODE 77)
  photospider_add_test(test_fragment_atlas_gpu photospider_internal_kernel tests/unit/test_fragment_atlas_gpu.cpp)
  target_include_directories(test_fragment_atlas_gpu PRIVATE src/lib)
  set_tests_properties(test_fragment_atlas_gpu PROPERTIES SKIP_RETURN_CODE 77)
  photospider_add_test(test_native_gpu photospider_test_kernel tests/unit/test_native_gpu.cpp)
  photospider_add_test(test_pipeline_key Photospider::kernel tests/unit/test_pipeline_key.cpp)
  target_include_directories(test_pipeline_key PRIVATE src/lib)
  target_include_directories(test_native_gpu PRIVATE src/lib)
  set_tests_properties(test_native_gpu PROPERTIES SKIP_RETURN_CODE 77)
  photospider_add_test(test_vulkan_gpu photospider_test_kernel tests/unit/test_vulkan_gpu.cpp)
  target_include_directories(test_vulkan_gpu PRIVATE src/lib)
  set_tests_properties(test_vulkan_gpu PROPERTIES SKIP_RETURN_CODE 77)
  photospider_add_test(test_memory_liveness photospider_test_kernel tests/integration/test_memory_liveness.cpp)
  target_include_directories(test_memory_liveness PRIVATE src/lib)
  photospider_add_test(test_result_dag_parallel Photospider::kernel tests/integration/test_result_dag_parallel.cpp)
  photospider_add_test(test_representations Photospider::kernel tests/unit/test_representations.cpp)
  add_subdirectory(examples/atom_outcomes_workflow)
  photospider_register_test(NAME example_atom_outcomes_workflow COMMAND photospider_atom_outcomes_workflow)
  photospider_add_test(test_atom_outcomes Photospider::kernel tests/integration/test_atom_outcomes.cpp)
  photospider_add_test(test_statistics_callback Photospider::kernel tests/unit/test_statistics_callback.cpp)
  photospider_add_test(test_statistics Photospider::kernel tests/unit/test_statistics.cpp)
  photospider_add_test(test_quality Photospider::kernel tests/unit/test_quality.cpp)
  photospider_add_test(test_layer Photospider::kernel tests/unit/test_layer.cpp)
  photospider_add_test(test_fft_contract Photospider::kernel tests/unit/test_fft_contract.cpp)
  photospider_add_test(test_component_contract Photospider::kernel tests/unit/test_component_contract.cpp)
  add_subdirectory(examples/components_workflow)
  photospider_register_test(NAME example_components_workflow COMMAND photospider_components_workflow)
  add_subdirectory(examples/fft_workflow)
  photospider_register_test(NAME example_fft_workflow COMMAND photospider_fft_workflow)
  add_subdirectory(examples/statistics_workflow)
  photospider_register_test(NAME example_statistics_workflow COMMAND photospider_statistics_workflow)
  photospider_add_test(photospider_representations_workflow Photospider::kernel examples/representations_workflow/main.cpp)
  photospider_add_test(test_result_execution Photospider::kernel tests/integration/test_result_execution.cpp)
  photospider_add_test(test_structured_submission photospider_test_kernel tests/integration/test_structured_submission.cpp)
  target_include_directories(test_structured_submission PRIVATE src/lib)
  photospider_add_test(test_result_neighborhood Photospider::kernel tests/unit/test_result_neighborhood.cpp)
  photospider_add_result_table_fixture(result_repeated_fixture tests/fixtures/result_repeated_fixture.c)
  photospider_add_test(test_result_repeated Photospider::kernel tests/integration/test_result_repeated.cpp)
  target_compile_definitions(test_result_repeated PRIVATE PS_REPEATED_FIXTURE="$<TARGET_FILE:result_repeated_fixture>")
  add_dependencies(test_result_repeated result_repeated_fixture)
  photospider_add_result_table_fixture(result_bad_repeated_fixture tests/fixtures/result_repeated_fixture.c BAD)
  target_compile_definitions(test_result_repeated PRIVATE PS_BAD_REPEATED_FIXTURE=\"$<TARGET_FILE:result_bad_repeated_fixture>\")
  photospider_add_result_table_fixture(result_operation_fixture tests/fixtures/result_operation_fixture.c)
  photospider_add_test(test_result_plugin Photospider::kernel tests/integration/test_result_plugin.cpp)
  photospider_add_test(test_result_output_payload Photospider::kernel examples/numeric_workflow/arrays.cpp)
  target_compile_definitions(test_result_output_payload PRIVATE PS_RESULT_PAYLOAD_TEST)
  photospider_add_test(test_numeric_result_arrays Photospider::kernel examples/numeric_workflow/arrays.cpp)
  target_compile_definitions(test_numeric_result_arrays PRIVATE PS_RESULT_ARRAY_TEST)
  target_compile_definitions(test_result_plugin PRIVATE PS_RESULT_FIXTURE="$<TARGET_FILE:result_operation_fixture>")
  add_dependencies(test_result_plugin result_operation_fixture)
  photospider_add_result_table_fixture(result_bad_fixture tests/fixtures/result_operation_fixture.c BAD)
  target_compile_definitions(test_result_plugin PRIVATE PS_BAD_RESULT_FIXTURE=\"$<TARGET_FILE:result_bad_fixture>\")
  photospider_register_test(NAME test_result_native_gpu COMMAND test_result_plugin --gpu-only)
  set_tests_properties(test_result_native_gpu PROPERTIES SKIP_RETURN_CODE 77)
  photospider_add_test(test_result_image_contracts Photospider::kernel tests/integration/test_result_image_contracts.cpp)
  photospider_add_test(test_metal_images Photospider::kernel tests/integration/test_metal_images.cpp)
  target_include_directories(test_metal_images PRIVATE examples)
  photospider_register_test(NAME test_metal_images_native COMMAND test_metal_images --native-only)
  set_tests_properties(test_metal_images_native PROPERTIES SKIP_RETURN_CODE 77)
  photospider_add_test(test_computed_scalar Photospider::kernel tests/integration/test_computed_scalar.cpp)
  target_include_directories(test_computed_scalar PRIVATE examples)
  photospider_register_test(NAME test_computed_scalar_sharing COMMAND test_computed_scalar --sharing-only)
  foreach(_image_family IN ITEMS backing views relations execution format filter transform composite native)
    photospider_add_test(test_result_image_${_image_family} Photospider::kernel
      tests/integration/image/test_result_image_${_image_family}.cpp)
  endforeach()
  set_tests_properties(test_result_image_native PROPERTIES SKIP_RETURN_CODE 77)
  photospider_add_test(test_unified_result_images Photospider::kernel tests/integration/test_unified_result_images.cpp)
  photospider_add_test(test_global_results Photospider::kernel tests/unit/test_global_results.cpp)
  photospider_add_test(test_shared_results photospider_internal_kernel tests/unit/test_shared_results.cpp)
  photospider_add_test(test_result_metadata_budget photospider_internal_kernel tests/unit/test_result_metadata_budget.cpp)
  if(PHOTOSPIDER_ENABLE_TSAN)
    # TSAN defines strong global new/delete interceptors. Keep the ordinary
    # budget/lifetime checks, while native builds retain allocator injection.
    target_compile_definitions(test_result_metadata_budget PRIVATE
      PHOTOSPIDER_TEST_NO_GLOBAL_ALLOCATOR_INTERPOSITION)
  endif()
  target_include_directories(test_result_metadata_budget PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src/lib")
  target_include_directories(test_shared_results PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src/lib")
  photospider_add_test(test_resources Photospider::kernel tests/unit/test_resources.cpp)
  photospider_add_test(test_resource_observation photospider_test_kernel tests/unit/test_resource_observation.cpp)
  target_include_directories(test_resource_observation PRIVATE src/lib)
  photospider_add_test(test_resource_internals photospider_internal_kernel tests/unit/test_resource_internals.cpp)
  target_include_directories(test_resource_internals PRIVATE src/lib)
  photospider_add_test(test_expression_literals Photospider::kernel tests/unit/test_expression_literals.cpp)
  target_include_directories(test_expression_literals PRIVATE src/lib)
  photospider_add_test(test_prepared_workspace Photospider::kernel tests/integration/test_prepared_workspace.cpp)
  photospider_add_test(test_inpaint_ns Photospider::kernel tests/integration/test_inpaint_ns.cpp)
  target_include_directories(test_inpaint_ns PRIVATE examples)
  if(PHOTOSPIDER_ENABLE_INPAINT_OPENCV)
    target_compile_definitions(test_inpaint_ns PRIVATE PHOTOSPIDER_TEST_INPAINT_OPENCV)
    target_include_directories(test_inpaint_ns PRIVATE ${OpenCV_INCLUDE_DIRS})
    target_link_libraries(test_inpaint_ns PRIVATE ${OpenCV_LIBS})
  endif()
  photospider_add_test(test_gaussian_tiled Photospider::kernel tests/integration/test_gaussian_tiled.cpp)
  photospider_add_test(test_gaussian_gpu photospider_test_kernel tests/integration/test_gaussian_gpu.cpp)
  target_include_directories(test_gaussian_gpu PRIVATE src/lib)
  set_tests_properties(test_gaussian_gpu PROPERTIES SKIP_RETURN_CODE 77)
  photospider_add_test(test_gaussian_workflow Photospider::kernel tests/integration/test_gaussian_workflow.cpp)
  photospider_add_test(test_gaussian_exact Photospider::kernel tests/unit/test_gaussian_exact.cpp)
  target_include_directories(test_gaussian_exact PRIVATE plugins/ops src/lib)
  photospider_add_test(test_gaussian_coefficients Photospider::kernel tests/unit/test_gaussian_coefficients.cpp)
  target_include_directories(test_gaussian_coefficients PRIVATE plugins/ops src/lib)
  photospider_add_test(test_perlin_exact Photospider::kernel tests/unit/test_perlin_exact.cpp)
  target_include_directories(test_perlin_exact PRIVATE plugins/ops src/lib)
  photospider_add_test(test_perlin_workflow Photospider::kernel tests/integration/test_perlin_workflow.cpp)
  photospider_add_test(test_perlin_tiled Photospider::kernel tests/integration/test_perlin_tiled.cpp)
  photospider_add_test(test_backend_admission Photospider::kernel tests/integration/test_backend_admission.cpp)
  photospider_add_test(test_result_tuple_validation Photospider::kernel tests/integration/test_result_tuple_validation.cpp)
  photospider_add_test(test_perlin_gpu Photospider::kernel tests/integration/test_perlin_gpu.cpp)
  set_tests_properties(test_perlin_gpu PROPERTIES SKIP_RETURN_CODE 77)
  photospider_add_test(test_cpu_tiles Photospider::kernel tests/integration/test_cpu_tiles.cpp)
  photospider_add_test(test_cpu_parallel Photospider::kernel tests/integration/test_cpu_parallel.cpp)
  photospider_add_test(test_cpu_range Photospider::kernel tests/unit/test_cpu_range.cpp)
  target_include_directories(test_cpu_range PRIVATE src/lib)
  photospider_add_test(test_managed_dispatch Photospider::kernel tests/integration/test_managed_dispatch.cpp)
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
  photospider_register_test(NAME test_execution_result_dso_native COMMAND test_execution --result-dso-native)
  set_tests_properties(test_execution_result_dso_native PROPERTIES SKIP_RETURN_CODE 77)
  add_dependencies(test_execution operation_fixture)
  target_compile_definitions(
    test_execution
    PRIVATE PS_OPERATION_FIXTURE_PATH="$<TARGET_FILE:operation_fixture>")
  photospider_add_test(
    test_plugin_registry photospider_test_kernel
    tests/integration/test_plugin_registry.cpp)
  photospider_add_test(
    test_result_diagnostics Photospider::kernel
    tests/integration/test_result_diagnostics.cpp)
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
            PS_OPERATION_UNICODE_FIXTURE_PATH="$<TARGET_FILE:operation_unicode_fixture>"
            PS_DATA_PROVIDER_FIXTURE_PATH="$<TARGET_FILE:data_provider_fixture>"
            PS_DATA_PROVIDER_BAD_FIXTURE_PATH="$<TARGET_FILE:data_provider_bad_fixture>"
            PS_DATA_PROVIDER_INVALID_UTF8_OVERLONG_FIXTURE_PATH="$<TARGET_FILE:data_provider_invalid_utf8_overlong_fixture>"
            PS_DATA_PROVIDER_UNICODE_FIXTURE_PATH="$<TARGET_FILE:data_provider_unicode_fixture>")

  photospider_register_test(
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
  photospider_finalize_behavior_targets()
  get_property(_behavior_targets GLOBAL PROPERTY PHOTOSPIDER_BEHAVIOR_TARGETS)
  list(REMOVE_DUPLICATES _behavior_targets)
  add_custom_target(photospider_tests ALL DEPENDS ${_behavior_targets})
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
    if(_includes MATCHES "${CMAKE_CURRENT_SOURCE_DIR}/(src/lib|plugins/ops)(;|$)" AND
       "Photospider::kernel" IN_LIST _links)
      list(REMOVE_ITEM _links Photospider::kernel)
      list(APPEND _links photospider_internal_kernel)
      set_property(TARGET ${_target} PROPERTY LINK_LIBRARIES ${_links})
    endif()
  endforeach()
endif()

install(FILES plugins/ops/09-composite/inpaint_ns_license.txt DESTINATION ${CMAKE_INSTALL_DATADIR}/licenses/Photospider)

install(FILES third_party/IK_LLAMA_LICENSE.txt
  DESTINATION ${CMAKE_INSTALL_DATADIR}/licenses/Photospider)
install(FILES third_party/sleef/LICENSE.txt
  DESTINATION ${CMAKE_INSTALL_DATADIR}/licenses/Photospider RENAME SLEEF.txt)
