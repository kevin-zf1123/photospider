# SLEEF is compiled into each kernel archive/shared library, with no installed
# third-party target or header requirement and no network access at configure.
set(_sleef_root "${PROJECT_SOURCE_DIR}/third_party/sleef")
# Source acquisition is a builder prerequisite, never a configure-time action.
foreach(_sleef_file IN ITEMS CMakeLists.txt LICENSE.txt src/common/misc.h
    src/common/quaddef.h src/common/estrin.h src/common/dd.h src/common/commonfuncs.h
    src/arch/helperadvsimd.h src/arch/helperavx2.h src/arch/helperpurec_scalar.h
    src/libm/sleefsimddp.c src/libm/rempitab.c)
  if(NOT EXISTS "${_sleef_root}/${_sleef_file}")
    message(FATAL_ERROR
      "SLEEF 3.9.0 source is required in ${_sleef_root}; missing ${_sleef_file}. "
      "Download the pinned source manually as described in third_party/SLEEF.md.")
  endif()
endforeach()
file(STRINGS "${_sleef_root}/CMakeLists.txt" _sleef_version
  REGEX "^set\\(SLEEF_VERSION 3\\.9\\.0\\)$")
if(NOT _sleef_version)
  message(FATAL_ERROR
    "Photospider requires SLEEF 3.9.0 in ${_sleef_root}. "
    "See third_party/SLEEF.md for the pinned source download commands.")
endif()
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
  "${_sleef_root}/CMakeLists.txt" "${_sleef_root}/LICENSE.txt")
file(WRITE "${PROJECT_BINARY_DIR}/generated/sleef-config.h"
  "/* Binary64-only private build; no quad arithmetic enabled. */\n")
add_library(photospider_numeric_math OBJECT
  "${PROJECT_SOURCE_DIR}/third_party/photospider_sleef.c"
  "${PROJECT_SOURCE_DIR}/plugins/ops/01-numeric/exp_simd.cpp"
  "${PROJECT_SOURCE_DIR}/plugins/ops/01-numeric/trig_simd.cpp")
target_include_directories(photospider_numeric_math PRIVATE
  "${PROJECT_SOURCE_DIR}/plugins/ops"
  "${PROJECT_BINARY_DIR}/generated"
  "${_sleef_root}/src/common" "${_sleef_root}/src/arch" "${_sleef_root}/src/libm")
target_compile_options(photospider_numeric_math PRIVATE
  -O3 -fno-fast-math -ffp-contract=off -fvisibility=hidden)
if(CMAKE_SYSTEM_PROCESSOR MATCHES "^(x86_64|AMD64|amd64)$")
  target_compile_options(photospider_numeric_math PRIVATE -mavx2 -mfma)
endif()
set_target_properties(photospider_numeric_math PROPERTIES
  POSITION_INDEPENDENT_CODE ON C_STANDARD 99 C_VISIBILITY_PRESET hidden)
