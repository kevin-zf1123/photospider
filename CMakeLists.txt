cmake_minimum_required(VERSION 3.21)

project(
  Photospider
  VERSION 0.24.0
  DESCRIPTION "Embeddable typed graph compiler and local execution kernel"
  LANGUAGES C CXX)

if(NOT CMAKE_C_COMPILER_ID MATCHES "Clang" OR
   NOT CMAKE_CXX_COMPILER_ID MATCHES "Clang")
  message(FATAL_ERROR "Photospider requires Clang for C and C++ builds")
endif()

include(CMakePackageConfigHelpers)
include(CTest)
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
  if(NOT CMAKE_CXX_COMPILER_ID MATCHES "Clang")
    message(FATAL_ERROR "Photospider sanitizers require Clang C++")
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
option(PHOTOSPIDER_ENABLE_INPAINT_OPENCV "Build pinned OpenCV 4.12.0 inpaint adapter" OFF)
if(PHOTOSPIDER_ENABLE_INPAINT_OPENCV)
  find_package(OpenCV 4.12.0 EXACT REQUIRED COMPONENTS core photo)
endif()

set(PHOTOSPIDER_OPERATION_SOURCES
  plugins/ops/00-foundation/core_constant.cpp
  plugins/ops/00-foundation/core_delay.cpp
  plugins/ops/00-foundation/core_gpu_fallback_probe.cpp
  plugins/ops/00-foundation/core_identity.cpp
  plugins/ops/02-format-color/channel_extraction.cpp
  plugins/ops/02-format-color/numeric_conversion.cpp
  plugins/ops/02-format-color/metadata_assignment.cpp
  plugins/ops/02-format-color/channel_assembly.cpp
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
  plugins/ops/03-generation/field_constant.cpp
  plugins/ops/03-generation/field_coordinate.cpp
  plugins/ops/04-mask-morphology/component_area.cpp
  plugins/ops/04-mask-morphology/component_bbox.cpp
  plugins/ops/04-mask-morphology/component_count.cpp
  plugins/ops/04-mask-morphology/mask_combine.cpp
  plugins/ops/04-mask-morphology/mask_components.cpp
  plugins/ops/04-mask-morphology/mask_dilate.cpp
  plugins/ops/04-mask-morphology/mask_erode.cpp
  plugins/ops/04-mask-morphology/mask_invert.cpp
  plugins/ops/04-mask-morphology/mask_threshold.cpp
  plugins/ops/05-filter/field_box_mean.cpp
  plugins/ops/05-filter/field_convolve.cpp
  plugins/ops/05-filter/field_correlate.cpp
  plugins/ops/05-filter/field_gaussian_blur.cpp
  plugins/ops/05-filter/image_gaussian_blur.cpp
  plugins/ops/05-filter/image_gaussian_blur_with_kernel.cpp
  plugins/ops/05-filter/image_convolve_channels.cpp
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
  src/lib/plugin/layer_operation.cpp
  src/lib/data/result.cpp
  src/lib/data/result_relation.cpp
  src/lib/execution/resources.cpp
  src/lib/execution/cancellation.cpp
  src/lib/execution/data_movement.cpp
  src/lib/data/value.cpp
  src/lib/data/tensor_description.cpp
  src/lib/data/planar_image.cpp
  src/lib/data/semantic.cpp
  src/lib/data/color_array.cpp
  src/lib/data/icc_profile.cpp
  src/lib/data/icc_validation.cpp
  src/lib/data/ocio_config_resource.cpp
  src/lib/data/resource_bindings.cpp
  src/lib/plugin/dependency_plugin.cpp
  src/lib/plugin/dependency_program.cpp
  src/lib/plugin/dependency_metadata.cpp
  src/lib/plugin/result_program.cpp
  src/lib/plugin/dependency_joint.cpp
  src/lib/plugin/dependency_discovery.cpp
  src/lib/plugin/dependency_block.cpp
  src/lib/plugin/dependency_invocation.cpp
  src/lib/plugin/operation_contract.cpp
  src/lib/plugin/operation_semantics.cpp
  src/lib/plugin/expression.cpp
  src/lib/data/input_validation.cpp
  src/lib/data/input_snapshot.cpp
  src/lib/execution/execution.cpp
  src/lib/execution/structured_execution.cpp
  src/lib/execution/dependency_records.cpp
  src/lib/execution/native_gpu.cpp
  src/lib/graph/graph_context.cpp
  src/lib/plugin/data_definition_registry.cpp
  src/lib/plugin/operation_registry.cpp
  src/lib/plugin/planar_plugin.cpp
  src/lib/plugin/utf8_validation.cpp)
if(PHOTOSPIDER_ENABLE_METAL AND APPLE)
  list(APPEND PHOTOSPIDER_KERNEL_SOURCES src/lib/execution/native_gpu_metal.mm)
  set_source_files_properties(src/lib/execution/native_gpu_metal.mm PROPERTIES
    COMPILE_OPTIONS "-fobjc-arc")
endif()
# S1 binary32 contracts require nearest rounding and gradual underflow at runtime.
if(CMAKE_CXX_COMPILER_ID MATCHES "Clang")
  set_source_files_properties(${PHOTOSPIDER_OPERATION_SOURCES}

    src/lib/plugin/expression.cpp src/lib/plugin/operation_semantics.cpp
    src/lib/data/input_validation.cpp src/lib/compiler/compiler.cpp
    src/lib/data/layer.cpp src/lib/data/representation.cpp
    src/lib/data/quality.cpp
    src/lib/data/statistics.cpp
    src/lib/plugin/statistics_operation.cpp
    src/lib/plugin/fft_operation.cpp
    src/lib/plugin/layer_operation.cpp
    src/lib/plugin/operation_registry.cpp PROPERTIES
    COMPILE_OPTIONS "-fno-fast-math;-frounding-math;-ffp-contract=off")
elseif(MSVC)
  set_source_files_properties(${PHOTOSPIDER_OPERATION_SOURCES}

    src/lib/plugin/expression.cpp src/lib/plugin/operation_semantics.cpp
    src/lib/data/input_validation.cpp src/lib/compiler/compiler.cpp
    src/lib/data/layer.cpp src/lib/data/representation.cpp
    src/lib/data/quality.cpp
    src/lib/data/statistics.cpp
    src/lib/plugin/statistics_operation.cpp
    src/lib/plugin/fft_operation.cpp
    src/lib/plugin/layer_operation.cpp
    src/lib/plugin/operation_registry.cpp PROPERTIES COMPILE_OPTIONS "/fp:strict")
endif()
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

# Disposable disk entries are compatible only with this maintained implementation.
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
set(_cache_identity "matrix-backend=${PHOTOSPIDER_MATRIX_BACKEND};sme=${PHOTOSPIDER_ENABLE_MATRIX_SME};${CMAKE_CXX_COMPILER_ID};${CMAKE_CXX_COMPILER_VERSION};${CMAKE_SYSTEM_NAME};${CMAKE_SYSTEM_VERSION};${CMAKE_SYSTEM_PROCESSOR};${CMAKE_CXX_FLAGS};${CMAKE_CXX_FLAGS_${_cache_config}};${BUILD_SHARED_LIBS};${PHOTOSPIDER_SANITIZER_MODE};accelerate=${PHOTOSPIDER_ENABLE_ACCELERATE}")
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
  target_sources(${target} PRIVATE $<TARGET_OBJECTS:photospider_numeric_math>)
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
    PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src/lib ${CMAKE_CURRENT_SOURCE_DIR}/plugins/ops ${CMAKE_CURRENT_BINARY_DIR}/generated/$<CONFIG>
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
add_subdirectory(examples/inpaint_ns_workflow EXCLUDE_FROM_ALL)
add_subdirectory(examples/image_vertical EXCLUDE_FROM_ALL)
add_subdirectory(examples/regional_image_vertical EXCLUDE_FROM_ALL)
add_subdirectory(examples/s3_image_workflow EXCLUDE_FROM_ALL)
add_subdirectory(examples/s4_gpu_workflow EXCLUDE_FROM_ALL)
add_subdirectory(examples/foundations_workflow EXCLUDE_FROM_ALL)
add_subdirectory(examples/numeric_workflow EXCLUDE_FROM_ALL)
add_subdirectory(examples/channel_extraction_performance EXCLUDE_FROM_ALL)
add_subdirectory(examples/numeric_conversion_performance EXCLUDE_FROM_ALL)
add_subdirectory(examples/codec_performance EXCLUDE_FROM_ALL)
add_subdirectory(examples/channel_assembly_performance EXCLUDE_FROM_ALL)
add_subdirectory(examples/channel_editing_performance EXCLUDE_FROM_ALL)
add_subdirectory(examples/channel_editing EXCLUDE_FROM_ALL)
add_subdirectory(examples/metadata_performance EXCLUDE_FROM_ALL)
add_subdirectory(examples/metadata_workflow EXCLUDE_FROM_ALL)
add_subdirectory(examples/g4_workflow EXCLUDE_FROM_ALL)
add_subdirectory(examples/multi_output_workflow EXCLUDE_FROM_ALL)

if(BUILD_TESTING)
  # Legacy image examples are retained as migration sources, not active
  # executable storage contracts. The planar workflow below is the image gate.
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
    if(CMAKE_CXX_COMPILER_ID MATCHES "Clang")
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

  add_library(multi_output_fixture MODULE tests/fixtures/multi_output_fixture.c)
  target_link_libraries(multi_output_fixture PRIVATE Photospider::operation_sdk)
  target_compile_definitions(multi_output_fixture PRIVATE PHOTOSPIDER_OPERATION_PLUGIN_BUILD)
  photospider_add_test(test_multi_output_contract Photospider::kernel tests/unit/test_multi_output_contract.cpp)
  target_compile_definitions(test_multi_output_contract PRIVATE PS_MULTI_OUTPUT_FIXTURE="$<TARGET_FILE:multi_output_fixture>")
  add_dependencies(test_multi_output_contract multi_output_fixture)
  photospider_add_test(test_joint_execution Photospider::kernel tests/integration/test_joint_execution.cpp)
  photospider_add_test(test_multi_output_execution Photospider::kernel tests/integration/test_multi_output_execution.cpp)
  target_compile_definitions(test_multi_output_execution PRIVATE PS_MULTI_OUTPUT_FIXTURE="$<TARGET_FILE:multi_output_fixture>")
  add_dependencies(test_multi_output_execution multi_output_fixture)
  photospider_add_test(test_semantic_contract Photospider::kernel tests/unit/test_semantic_contract.cpp)
  photospider_add_test(test_planar_plugin photospider_test_kernel tests/integration/test_planar_plugin.cpp)
  foreach(_variant planar bad_planar projected_planar)
    add_library(${_variant}_fixture MODULE tests/fixtures/planar_operation_fixture.c)
    target_link_libraries(${_variant}_fixture PRIVATE Photospider::operation_sdk)
    set_target_properties(${_variant}_fixture PROPERTIES C_STANDARD 11 C_STANDARD_REQUIRED ON)
  endforeach()
  target_compile_definitions(bad_planar_fixture PRIVATE PS_BAD_PLANAR)
  target_compile_definitions(projected_planar_fixture PRIVATE PS_PROJECT_PLANAR)
  target_compile_definitions(test_planar_plugin PRIVATE
    PS_PLANAR_FIXTURE="$<TARGET_FILE:planar_fixture>"
    PS_BAD_PLANAR_FIXTURE="$<TARGET_FILE:bad_planar_fixture>"
    PS_PROJECTED_PLANAR_FIXTURE="$<TARGET_FILE:projected_planar_fixture>")
  add_dependencies(test_planar_plugin planar_fixture bad_planar_fixture projected_planar_fixture)
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

  set_property(TARGET photospider_foundations_workflow PROPERTY EXCLUDE_FROM_ALL FALSE)
  add_test(NAME test_workflow_numeric_reductions COMMAND photospider_foundations_workflow --scenario numeric)
  add_test(NAME test_workflow_expression_lut COMMAND photospider_foundations_workflow --scenario expression-lut)
  add_test(NAME test_workflow_filter_histogram COMMAND photospider_foundations_workflow --scenario basic-filters)
  photospider_add_test(test_basic_operations Photospider::kernel tests/integration/test_basic_operations.cpp)
  target_include_directories(test_basic_operations PRIVATE examples)
  photospider_add_test(test_generic_component_operations Photospider::kernel tests/integration/test_generic_component_operations.cpp)
  photospider_add_test(test_expression_operations Photospider::kernel tests/integration/test_expression_operations.cpp)
  add_library(expression_contract_fixture MODULE tests/fixtures/expression_contract_fixture.c)
  target_link_libraries(expression_contract_fixture PRIVATE Photospider::operation_sdk)
  set_target_properties(expression_contract_fixture PROPERTIES C_STANDARD 11 C_STANDARD_REQUIRED ON)
  target_compile_definitions(test_expression_operations PRIVATE PS_EXPRESSION_CONTRACT_FIXTURE="$<TARGET_FILE:expression_contract_fixture>")
  add_dependencies(test_expression_operations expression_contract_fixture)
  photospider_add_test(test_metadata_assignment Photospider::kernel tests/integration/test_metadata_assignment.cpp)
  target_include_directories(test_metadata_assignment PRIVATE examples/numeric_workflow)
  photospider_add_test(test_channel_editing Photospider::kernel tests/integration/test_channel_editing.cpp)
  photospider_add_test(test_channel_assembly Photospider::kernel tests/integration/test_channel_assembly.cpp)
  target_include_directories(test_channel_assembly PRIVATE examples/numeric_workflow)
  photospider_add_test(test_channel_extraction Photospider::kernel tests/integration/test_channel_extraction.cpp)
  photospider_add_test(test_numeric_conversion Photospider::kernel tests/integration/test_numeric_conversion.cpp)
  photospider_add_test(test_region_runs Photospider::kernel tests/unit/test_region_runs.cpp)
  photospider_add_test(test_data_movement_contract Photospider::kernel tests/unit/test_data_movement_contract.cpp)
  photospider_add_test(test_planar_preparation Photospider::kernel tests/unit/test_planar_preparation.cpp)
  if(PHOTOSPIDER_ENABLE_NUMERIC_CONVERSION_SME AND PHOTOSPIDER_COMPILER_HAS_CONVERSION_SME_ACLE)
    add_executable(test_numeric_conversion_sme tests/unit/test_numeric_conversion_sme.cpp plugins/ops/02-format-color/numeric_conversion_sme.cpp)
    target_include_directories(test_numeric_conversion_sme PRIVATE include src/lib tests)
    target_compile_features(test_numeric_conversion_sme PRIVATE cxx_std_17)
    target_link_libraries(test_numeric_conversion_sme PRIVATE Threads::Threads)
    add_test(NAME test_numeric_conversion_sme COMMAND test_numeric_conversion_sme)
    set_tests_properties(test_numeric_conversion_sme PROPERTIES SKIP_RETURN_CODE 77)
  endif()

  target_include_directories(test_channel_extraction PRIVATE examples/numeric_workflow)
  photospider_add_test(test_generic_convolution_roi Photospider::kernel tests/integration/test_generic_convolution_roi.cpp)
  photospider_add_test(test_numeric_operations Photospider::kernel tests/integration/test_numeric_operations.cpp)
  photospider_add_test(test_matrix_certificate photospider_test_kernel tests/unit/test_matrix_certificate.cpp)
  target_include_directories(test_matrix_certificate PRIVATE plugins/ops src/lib)
  if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    target_compile_options(test_matrix_certificate PRIVATE -fno-fast-math -frounding-math -ffp-contract=off)
  elseif(MSVC)
    target_compile_options(test_matrix_certificate PRIVATE /fp:strict)
  endif()
  photospider_add_test(test_ordered_reduction Photospider::kernel tests/integration/test_ordered_reduction.cpp)
  photospider_add_test(test_ordered_scan Photospider::kernel tests/integration/test_ordered_scan.cpp)
  photospider_add_test(test_scan_waiters photospider_test_kernel tests/integration/test_scan_waiters.cpp)
  target_include_directories(test_scan_waiters PRIVATE src/lib)
  # This test directly exercises private cache validation and allocation code.
  photospider_add_test(test_generic_result_cache photospider_test_kernel tests/integration/test_generic_result_cache.cpp)
  target_include_directories(test_generic_result_cache PRIVATE src/lib)
  photospider_add_test(test_frozen_execution Photospider::kernel tests/integration/test_frozen_execution.cpp)
  photospider_add_test(test_dependency_sampling Photospider::kernel tests/integration/test_dependency_sampling.cpp)
  photospider_add_test(test_execution_dependencies Photospider::kernel tests/integration/test_execution_dependencies.cpp)
  photospider_add_test(test_execution_demand Photospider::kernel tests/integration/test_execution_demand.cpp)
  add_library(dependency_fixture MODULE tests/fixtures/dependency_fixture.c)
  target_link_libraries(dependency_fixture PRIVATE Photospider::operation_sdk)
  if(UNIX)
    target_link_libraries(dependency_fixture PRIVATE m)
  endif()
  target_compile_definitions(dependency_fixture PRIVATE PHOTOSPIDER_OPERATION_PLUGIN_BUILD)
  set_target_properties(dependency_fixture PROPERTIES C_STANDARD 11 C_STANDARD_REQUIRED ON)
  add_library(dependency_joint_fixture MODULE tests/fixtures/dependency_joint_fixture.c)
  target_link_libraries(dependency_joint_fixture PRIVATE Photospider::operation_sdk)
  photospider_add_test(test_dependency_joint Photospider::kernel tests/integration/test_dependency_joint.cpp)
  set_tests_properties(test_dependency_joint PROPERTIES TIMEOUT 120)
  target_compile_definitions(test_dependency_joint PRIVATE PS_DEPENDENCY_JOINT_FIXTURE="$<TARGET_FILE:dependency_joint_fixture>")
  add_dependencies(test_dependency_joint dependency_joint_fixture)
  target_compile_definitions(test_joint_execution PRIVATE PS_DEPENDENCY_JOINT_FIXTURE="$<TARGET_FILE:dependency_joint_fixture>")
  add_dependencies(test_joint_execution dependency_joint_fixture)
  photospider_add_test(test_dependency_plugin Photospider::kernel tests/integration/test_dependency_plugin.cpp)
  target_link_libraries(test_dependency_plugin PRIVATE ${CMAKE_DL_LIBS})
  target_compile_definitions(test_dependency_plugin PRIVATE PS_DEPENDENCY_FIXTURE="$<TARGET_FILE:dependency_fixture>")
  add_dependencies(test_dependency_plugin dependency_fixture)
  foreach(_case RANGE 1 4)
    add_library(dependency_bad_${_case} MODULE tests/fixtures/dependency_fixture.c)
    target_link_libraries(dependency_bad_${_case} PRIVATE Photospider::operation_sdk)
    if(UNIX)
      target_link_libraries(dependency_bad_${_case} PRIVATE m)
    endif()
    target_compile_definitions(dependency_bad_${_case} PRIVATE PHOTOSPIDER_OPERATION_PLUGIN_BUILD PS_DEPENDENCY_BAD_CASE=${_case})
    set_target_properties(dependency_bad_${_case} PROPERTIES C_STANDARD 11 C_STANDARD_REQUIRED ON)
    target_compile_definitions(test_dependency_plugin PRIVATE PS_DEPENDENCY_BAD_${_case}="$<TARGET_FILE:dependency_bad_${_case}>")
    add_dependencies(test_dependency_plugin dependency_bad_${_case})
  endforeach()
  photospider_add_test(test_dependency_program photospider_test_kernel tests/integration/test_dependency_program.cpp)
  target_include_directories(test_dependency_program PRIVATE src/lib)
  photospider_add_test(test_dependency_dirty Photospider::kernel tests/unit/test_dependency_dirty.cpp)
  target_include_directories(test_dependency_dirty PRIVATE src/lib)
  photospider_add_test(test_dependency Photospider::kernel tests/unit/test_dependency.cpp)
  add_dependencies(test_dependency photospider_dependency_workflow)
  add_test(NAME test_dependency_workflow COMMAND photospider_dependency_workflow)
  photospider_add_test(test_footprint Photospider::kernel tests/unit/test_footprint.cpp)
  photospider_add_test(test_value_fragments Photospider::kernel tests/unit/test_value_fragments.cpp)
  photospider_add_test(test_fragment_atlas Photospider::kernel tests/unit/test_fragment_atlas.cpp)
  photospider_add_test(test_input_snapshot Photospider::kernel tests/unit/test_input_snapshot.cpp)
  photospider_add_test(test_planar_image_workflow Photospider::kernel tests/integration/test_planar_image_workflow.cpp)
  photospider_add_test(test_planar_import Photospider::kernel tests/unit/test_planar_import.cpp)
  target_include_directories(test_input_snapshot PRIVATE src/lib)
  photospider_add_test(test_native_execution Photospider::kernel tests/integration/test_native_execution.cpp)
  set_tests_properties(test_native_execution PROPERTIES SKIP_RETURN_CODE 77)
  photospider_add_test(test_dependency_gpu Photospider::kernel tests/unit/test_dependency_gpu.cpp)
  photospider_add_test(test_gpu_sync_fallback Photospider::kernel examples/g4_gpu_workflow/sync_main.cpp)
  set_tests_properties(test_gpu_sync_fallback PROPERTIES SKIP_RETURN_CODE 77)
  photospider_add_test(test_gpu_discovery Photospider::kernel tests/unit/test_gpu_discovery.cpp)
  add_library(gpu_discovery_fixture MODULE examples/g4_gpu_workflow/discovery_plugin.c)
  target_link_libraries(gpu_discovery_fixture PRIVATE Photospider::operation_sdk)
  target_compile_definitions(gpu_discovery_fixture PRIVATE PHOTOSPIDER_OPERATION_PLUGIN_BUILD)
  set_target_properties(gpu_discovery_fixture PROPERTIES C_STANDARD 11 C_STANDARD_REQUIRED ON)
  photospider_add_test(test_gpu_discovery_workflow Photospider::kernel examples/g4_gpu_workflow/discovery_main.cpp)
  target_compile_definitions(test_gpu_discovery_workflow PRIVATE PS_GPU_DISCOVERY_PLUGIN="$<TARGET_FILE:gpu_discovery_fixture>")
  add_dependencies(test_gpu_discovery_workflow gpu_discovery_fixture)
  set_tests_properties(test_gpu_discovery_workflow PROPERTIES SKIP_RETURN_CODE 77)
  add_library(gpu_dependency_c_fixture MODULE examples/g4_gpu_workflow/c_plugin.c)
  target_link_libraries(gpu_dependency_c_fixture PRIVATE Photospider::operation_sdk)
  target_compile_definitions(gpu_dependency_c_fixture PRIVATE PHOTOSPIDER_OPERATION_PLUGIN_BUILD)
  set_target_properties(gpu_dependency_c_fixture PROPERTIES C_STANDARD 11 C_STANDARD_REQUIRED ON)
  photospider_add_test(test_gpu_c_abi_execution Photospider::kernel examples/g4_gpu_workflow/c_main.cpp)
  target_compile_definitions(test_gpu_c_abi_execution PRIVATE PS_GPU_DEPENDENCY_PLUGIN="$<TARGET_FILE:gpu_dependency_c_fixture>")
  add_dependencies(test_gpu_c_abi_execution gpu_dependency_c_fixture)
  set_tests_properties(test_gpu_c_abi_execution PROPERTIES SKIP_RETURN_CODE 77)
  photospider_add_test(test_gpu_fragment_execution Photospider::kernel examples/g4_gpu_workflow/main.cpp)
  set_tests_properties(test_gpu_fragment_execution PROPERTIES SKIP_RETURN_CODE 77)
  photospider_add_test(test_fragment_atlas_gpu photospider_test_kernel tests/unit/test_fragment_atlas_gpu.cpp)
  target_include_directories(test_fragment_atlas_gpu PRIVATE src/lib)
  set_tests_properties(test_fragment_atlas_gpu PROPERTIES SKIP_RETURN_CODE 77)
  photospider_add_test(test_native_gpu photospider_test_kernel tests/unit/test_native_gpu.cpp)
  target_include_directories(test_native_gpu PRIVATE src/lib)
  set_tests_properties(test_native_gpu PROPERTIES SKIP_RETURN_CODE 77)
  photospider_add_test(test_memory_liveness photospider_test_kernel tests/integration/test_memory_liveness.cpp)
  target_include_directories(test_memory_liveness PRIVATE src/lib)
  photospider_add_test(test_representations Photospider::kernel tests/unit/test_representations.cpp)
  add_subdirectory(examples/atom_outcomes_workflow)
  add_test(NAME example_atom_outcomes_workflow COMMAND photospider_atom_outcomes_workflow)
  photospider_add_test(test_atom_outcomes Photospider::kernel tests/integration/test_atom_outcomes.cpp)
  photospider_add_test(test_statistics_callback Photospider::kernel tests/unit/test_statistics_callback.cpp)
  photospider_add_test(test_statistics Photospider::kernel tests/unit/test_statistics.cpp)
  photospider_add_test(test_quality Photospider::kernel tests/unit/test_quality.cpp)
  photospider_add_test(test_fft_contract Photospider::kernel tests/unit/test_fft_contract.cpp)
  photospider_add_test(test_component_contract Photospider::kernel tests/unit/test_component_contract.cpp)
  add_subdirectory(examples/components_workflow)
  add_test(NAME example_components_workflow COMMAND photospider_components_workflow)
  add_subdirectory(examples/fft_workflow)
  add_test(NAME example_fft_workflow COMMAND photospider_fft_workflow)
  add_subdirectory(examples/statistics_workflow)
  add_test(NAME example_statistics_workflow COMMAND photospider_statistics_workflow)
  photospider_add_test(photospider_representations_workflow Photospider::kernel examples/representations_workflow/main.cpp)
  photospider_add_test(test_result_execution Photospider::kernel tests/integration/test_result_execution.cpp)
  photospider_add_test(test_global_results Photospider::kernel tests/unit/test_global_results.cpp)
  photospider_add_test(test_shared_results photospider_test_kernel tests/unit/test_shared_results.cpp)
  photospider_add_test(test_result_metadata_budget photospider_test_kernel tests/unit/test_result_metadata_budget.cpp)
  if(PHOTOSPIDER_ENABLE_TSAN)
    # TSAN defines strong global new/delete interceptors. Keep the ordinary
    # budget/lifetime checks, while native builds retain allocator injection.
    target_compile_definitions(test_result_metadata_budget PRIVATE
      PHOTOSPIDER_TEST_NO_GLOBAL_ALLOCATOR_INTERPOSITION)
  endif()
  target_include_directories(test_result_metadata_budget PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src/lib")
  target_include_directories(test_shared_results PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src/lib")
  photospider_add_test(test_resources Photospider::kernel tests/unit/test_resources.cpp)
  target_include_directories(test_resources PRIVATE src/lib)
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

install(FILES plugins/ops/09-composite/inpaint_ns_license.txt DESTINATION ${CMAKE_INSTALL_DATADIR}/licenses/Photospider)

install(FILES third_party/IK_LLAMA_LICENSE.txt
  DESTINATION ${CMAKE_INSTALL_DATADIR}/licenses/Photospider)
install(FILES third_party/sleef/LICENSE.txt
  DESTINATION ${CMAKE_INSTALL_DATADIR}/licenses/Photospider RENAME SLEEF.txt)
