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


option(PHOTOSPIDER_TRANSFER_FAST_MATH "Enable certified FMT-09 SIMD interval filter" ON)
option(PHOTOSPIDER_TRANSFER_COMPACT_MATH "Enable FMT-09 compact strict enclosure tier" ON)
option(PHOTOSPIDER_TRANSFER_GAMMA2_SIMD "Enable FMT-09 exact arithmetic and bit-classification SIMD" ON)
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

set(PHOTOSPIDER_OPERATION_SOURCES
  plugins/ops/01-numeric/expression.cpp
  plugins/ops/01-numeric/expression_metadata.cpp
  plugins/ops/02-format-color/alpha_authoring.cpp
  plugins/ops/02-format-color/alpha_operations.cpp
  plugins/ops/02-format-color/numeric_conversion.cpp
  plugins/ops/02-format-color/model_conversion.cpp
  plugins/ops/02-format-color/model_math.cpp
  plugins/ops/02-format-color/model_simd.cpp
  plugins/ops/02-format-color/rgb_basis.cpp
  plugins/ops/02-format-color/rgb_basis_math.cpp
  plugins/ops/02-format-color/transfer.cpp
  plugins/ops/02-format-color/channel_assembly.cpp
  plugins/ops/02-format-color/channel_literal_like.cpp
  plugins/ops/02-format-color/channel_extraction.cpp
  plugins/ops/01-numeric/core_constant.cpp
  plugins/ops/01-numeric/core_identity.cpp
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
  plugins/ops/08-transform/stmap.cpp
)



# Keep optional ISA flags/definitions local; all other kernel code retains the
# ordinary host target and can perform runtime admission without SME opcodes.
set_property(SOURCE plugins/ops/01-numeric/numeric_matrix.cpp APPEND PROPERTY
  COMPILE_DEFINITIONS "PHOTOSPIDER_MATRIX_BACKEND_${PHOTOSPIDER_MATRIX_BACKEND}=1")
if(APPLE AND PHOTOSPIDER_ENABLE_ACCELERATE)
  set_property(SOURCE plugins/ops/01-numeric/accelerated_matrix_apple.cpp APPEND PROPERTY
    COMPILE_DEFINITIONS PHOTOSPIDER_HAS_ACCELERATE=1)
endif()
if(PHOTOSPIDER_ENABLE_MATRIX_SME)
  list(APPEND PHOTOSPIDER_OPERATION_SOURCES plugins/ops/01-numeric/accelerated_matrix_sme.cpp)
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
    list(APPEND PHOTOSPIDER_OPERATION_SOURCES plugins/ops/02-format-color/numeric_conversion_sme.cpp)
    set_source_files_properties(plugins/ops/02-format-color/numeric_conversion_sme.cpp PROPERTIES
      COMPILE_OPTIONS "-march=armv8-a+sme+sme-f64f64;-fno-fast-math;-frounding-math;-ffp-contract=off")
    set_property(SOURCE plugins/ops/02-format-color/numeric_conversion.cpp APPEND PROPERTY
      COMPILE_DEFINITIONS PHOTOSPIDER_HAS_CONVERSION_SME=1)
  else()
    message(STATUS "Compiler lacks SME support; numeric conversion retains NEON")
  endif()
endif()
set_property(SOURCE ${PHOTOSPIDER_OPERATION_SOURCES} APPEND PROPERTY
  COMPILE_OPTIONS "-fno-fast-math" "-frounding-math" "-ffp-contract=off")
include(${CMAKE_CURRENT_LIST_DIR}/NumericMath.cmake)


add_library(photospider_ops_headers INTERFACE)
add_library(Photospider::ops_headers ALIAS photospider_ops_headers)
set_target_properties(photospider_ops_headers PROPERTIES EXPORT_NAME ops_headers)
target_compile_features(photospider_ops_headers INTERFACE cxx_std_17)
target_include_directories(
  photospider_ops_headers
  INTERFACE $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/plugins/ops/include>
            $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>)


add_library(photospider_ops
  ${PHOTOSPIDER_OPERATION_SOURCES}
  plugins/ops/01-numeric/lut3d_baking.cpp
  plugins/ops/registry.cpp
  $<TARGET_OBJECTS:photospider_numeric_math>)
add_library(Photospider::ops ALIAS photospider_ops)
set_target_properties(photospider_ops PROPERTIES
  EXPORT_NAME ops CXX_EXTENSIONS OFF POSITION_INDEPENDENT_CODE ON
  CXX_VISIBILITY_PRESET hidden VISIBILITY_INLINES_HIDDEN YES
  VERSION ${PROJECT_VERSION} SOVERSION "${PROJECT_VERSION_MAJOR}.${PROJECT_VERSION_MINOR}")
target_compile_features(photospider_ops PUBLIC cxx_std_17)
target_compile_definitions(photospider_ops PRIVATE PHOTOSPIDER_OPS_BUILD)
foreach(_target IN ITEMS photospider_ops photospider_numeric_math)
  target_compile_definitions(${_target} PRIVATE
    PHOTOSPIDER_TRANSFER_FAST_MATH=$<BOOL:${PHOTOSPIDER_TRANSFER_FAST_MATH}>
    PHOTOSPIDER_TRANSFER_COMPACT_MATH=$<BOOL:${PHOTOSPIDER_TRANSFER_COMPACT_MATH}>
    PHOTOSPIDER_TRANSFER_GAMMA2_SIMD=$<BOOL:${PHOTOSPIDER_TRANSFER_GAMMA2_SIMD}>)
endforeach()
if(NOT BUILD_SHARED_LIBS)
  target_compile_definitions(photospider_ops PUBLIC PHOTOSPIDER_OPS_STATIC)
endif()
target_link_libraries(photospider_ops PUBLIC Photospider::ops_sdk Photospider::ops_headers)
target_include_directories(photospider_ops PRIVATE
  "${CMAKE_CURRENT_SOURCE_DIR}/plugins/ops")
if(APPLE AND PHOTOSPIDER_ENABLE_ACCELERATE)
  target_link_libraries(photospider_ops PRIVATE "-framework Accelerate")
endif()
if(NOT PHOTOSPIDER_SANITIZER_MODE STREQUAL "none")
  target_compile_options(photospider_ops PRIVATE ${PHOTOSPIDER_SANITIZER_FLAG} -fno-omit-frame-pointer)
  target_link_options(photospider_ops PRIVATE ${PHOTOSPIDER_SANITIZER_FLAG})
endif()
install(TARGETS photospider_ops photospider_ops_headers EXPORT PhotospiderTargets
  ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
  LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
  RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
  INCLUDES DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})
install(DIRECTORY plugins/ops/include/photospider/
  DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}/photospider
  FILES_MATCHING PATTERN "*.hpp" PATTERN "*.h")
install(FILES third_party/sleef/LICENSE.txt
  DESTINATION ${CMAKE_INSTALL_DATADIR}/licenses/Photospider RENAME SLEEF.txt)
