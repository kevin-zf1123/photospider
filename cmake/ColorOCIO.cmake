# Isolated, pinned OCIO source and a private static C++ runtime. Nothing is
# added to the kernel's targets, standard library, or public include paths.
function(ps_color_fetch name url hash)
  FetchContent_Declare(${name} URL "${url}" URL_HASH "SHA256=${hash}"
    DOWNLOAD_EXTRACT_TIMESTAMP TRUE SOURCE_SUBDIR __photospider_private_source__)
  FetchContent_MakeAvailable(${name})
  set(${name}_SOURCE_DIR "${${name}_SOURCE_DIR}" PARENT_SCOPE)
endfunction()

function(ps_configure_ocio)
  # Parent workspace export hooks apply only to the host project.
  set(CMAKE_PROJECT_INCLUDE "")
  set(BUILD_SHARED_LIBS OFF)
  set(BUILD_TESTING OFF)
  set(CMAKE_POSITION_INDEPENDENT_CODE ON)
  set(CMAKE_CXX_STANDARD 17)
  set(CMAKE_C_VISIBILITY_PRESET hidden)
  set(CMAKE_CXX_VISIBILITY_PRESET hidden)
  set(CMAKE_VISIBILITY_INLINES_HIDDEN ON)
  find_program(PS_COLOR_LLVM_CONFIG NAMES llvm-config
    HINTS /opt/homebrew/opt/llvm/bin /usr/local/llvm/bin REQUIRED)
  execute_process(COMMAND "${PS_COLOR_LLVM_CONFIG}" --libdir
    OUTPUT_VARIABLE _runtime_lib OUTPUT_STRIP_TRAILING_WHITESPACE COMMAND_ERROR_IS_FATAL ANY)
  execute_process(COMMAND "${PS_COLOR_LLVM_CONFIG}" --includedir
    OUTPUT_VARIABLE _runtime_inc OUTPUT_STRIP_TRAILING_WHITESPACE COMMAND_ERROR_IS_FATAL ANY)
  get_filename_component(_llvm_bin "${PS_COLOR_LLVM_CONFIG}" DIRECTORY)
  find_program(PS_COLOR_OBJCOPY NAMES llvm-objcopy HINTS "${_llvm_bin}" REQUIRED)
  find_file(PS_COLOR_LIBCXX_ARCHIVE libc++.a
    HINTS "${_runtime_lib}/c++" "${_runtime_lib}" /usr/lib REQUIRED)
  find_path(PS_COLOR_LIBCXX_HEADERS vector
    HINTS "${_runtime_inc}/c++/v1" /usr/include/c++/v1 REQUIRED)
  set(_private_runtime "${CMAKE_CURRENT_BINARY_DIR}/color-engines/libcxx-private.a")
  set(_apple "")
  if(APPLE)
    set(_apple --apple)
  endif()
  execute_process(COMMAND "${Python3_EXECUTABLE}" "${_ps_color}/prepare_runtime.py"
    --archive "${PS_COLOR_LIBCXX_ARCHIVE}" --output "${_private_runtime}"
    --objcopy "${PS_COLOR_OBJCOPY}" ${_apple}
    OUTPUT_VARIABLE _runtime_hash OUTPUT_STRIP_TRAILING_WHITESPACE
    COMMAND_ERROR_IS_FATAL ANY)
  find_file(PS_COLOR_LIBCXXABI_ARCHIVE libc++abi.a
    HINTS "${_runtime_lib}/c++" "${_runtime_lib}" /usr/lib REQUIRED)
  set(_private_abi "${CMAKE_CURRENT_BINARY_DIR}/color-engines/libcxxabi-private.a")
  execute_process(COMMAND "${Python3_EXECUTABLE}" "${_ps_color}/prepare_runtime.py"
    --archive "${PS_COLOR_LIBCXXABI_ARCHIVE}" --output "${_private_abi}"
    --objcopy "${PS_COLOR_OBJCOPY}" --abi ${_apple}
    OUTPUT_VARIABLE _abi_hash OUTPUT_STRIP_TRAILING_WHITESPACE
    COMMAND_ERROR_IS_FATAL ANY)
  string(SHA256 _runtime_hash "${_runtime_hash};${_abi_hash}")
  # Content identity changes with the adapter, compiler, math flags, platform,
  # runtime and actual SIMD configuration. Resource snapshots bind this value.
  file(GLOB _integration_files CONFIGURE_DEPENDS
    "${_ps_color}/*.c" "${_ps_color}/*.h" "${_ps_color}/*.cpp"
    "${_ps_color}/*.hpp" "${_ps_color}/prepare_*.py")
  list(APPEND _integration_files
    "${CMAKE_CURRENT_SOURCE_DIR}/cmake/ColorEngines.cmake"
    "${CMAKE_CURRENT_SOURCE_DIR}/cmake/ColorOCIO.cmake")
  set(_integration_hashes "")
  foreach(source IN LISTS _integration_files)
    file(SHA256 "${source}" source_hash)
    string(APPEND _integration_hashes ";${source_hash}")
  endforeach()
  set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS ${_integration_files})
  set(_build_basis "${_runtime_hash};${_integration_hashes};${CMAKE_SYSTEM_NAME};${CMAKE_SYSTEM_VERSION};${CMAKE_SYSTEM_PROCESSOR};${CMAKE_C_COMPILER_ID};${CMAKE_C_COMPILER_VERSION};${CMAKE_CXX_COMPILER_ID};${CMAKE_CXX_COMPILER_VERSION};${CMAKE_BUILD_TYPE};${CMAKE_C_FLAGS};${CMAKE_CXX_FLAGS};${CMAKE_C_FLAGS_RELEASE};${CMAKE_CXX_FLAGS_RELEASE};${CMAKE_C_FLAGS_RELWITHDEBINFO};${CMAKE_CXX_FLAGS_RELWITHDEBINFO};${CMAKE_OSX_ARCHITECTURES};${CMAKE_OSX_SYSROOT};${CMAKE_OSX_DEPLOYMENT_TARGET}")

  target_compile_options(photospider_color_engine PRIVATE
    $<$<COMPILE_LANGUAGE:CXX>:-nostdinc++>
    "$<$<COMPILE_LANGUAGE:CXX>:SHELL:-isystem ${PS_COLOR_LIBCXX_HEADERS}>")
  target_sources(photospider_color_engine PRIVATE "${_ps_color}/engine_cpp_memory.cpp" "${_ps_color}/ocio_engine.cpp")
  set_source_files_properties("${_ps_color}/ocio_engine.cpp" PROPERTIES
    COMPILE_OPTIONS "-include;${_ps_color}/engine_cpp_redirect.hpp")
  target_link_options(photospider_color_engine PRIVATE -nostdlib++)
  target_link_libraries(photospider_color_engine PRIVATE "${_private_runtime}" "${_private_abi}")
  if(APPLE)
    file(WRITE "${CMAKE_CURRENT_BINARY_DIR}/color-engines/exports.txt" "_ps_color_*\n")
    target_link_options(photospider_color_engine PRIVATE
      "LINKER:-exported_symbols_list,${CMAKE_CURRENT_BINARY_DIR}/color-engines/exports.txt")
  else()
    file(WRITE "${CMAKE_CURRENT_BINARY_DIR}/color-engines/exports.map"
      "{ global: ps_color_*; local: *; };\n")
    target_link_options(photospider_color_engine PRIVATE
      "LINKER:--version-script=${CMAKE_CURRENT_BINARY_DIR}/color-engines/exports.map")
  endif()

  set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -include ${_ps_color}/engine_redirect.h")
  set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -nostdinc++ -isystem ${PS_COLOR_LIBCXX_HEADERS} -include ${_ps_color}/engine_cpp_redirect.hpp")
  ps_color_fetch(ps_color_yaml https://github.com/jbeder/yaml-cpp/archive/refs/tags/0.8.0.tar.gz
    fbe74bbdcee21d656715688706da3c8becfd946d92cd44705cc6098bb23b3a16)
  set(YAML_CPP_BUILD_TESTS OFF)
  set(YAML_CPP_BUILD_TOOLS OFF)
  set(YAML_CPP_INSTALL OFF)
  set(YAML_CPP_FORMAT_SOURCE OFF)
  add_subdirectory("${ps_color_yaml_SOURCE_DIR}" color-engines/yaml EXCLUDE_FROM_ALL)
  # Bootstrap the pinned parser's process-lifetime regex storage before any
  # invocation allocator is active. This private header is not exported.
  target_include_directories(photospider_color_engine PRIVATE
    "${ps_color_yaml_SOURCE_DIR}")
  ps_color_fetch(ps_color_expat https://github.com/libexpat/libexpat/archive/refs/tags/R_2_7_2.tar.gz
    d09e2dd23398805cec1bac2860304714c96dc2fde629a7a1a77d0880ab7cd242)
  set(EXPAT_BUILD_EXAMPLES OFF)
  set(EXPAT_BUILD_TESTS OFF)
  set(EXPAT_BUILD_TOOLS OFF)
  set(EXPAT_SHARED_LIBS OFF)
  set(EXPAT_BUILD_DOCS OFF)
  set(EXPAT_ENABLE_INSTALL OFF)
  add_subdirectory("${ps_color_expat_SOURCE_DIR}/expat" color-engines/expat EXCLUDE_FROM_ALL)
  ps_color_fetch(ps_color_pystring https://github.com/imageworks/pystring/archive/refs/tags/v1.1.4.tar.gz
    49da0fe2a049340d3c45cce530df63a2278af936003642330287b68cefd788fb)
  add_library(pystring STATIC "${ps_color_pystring_SOURCE_DIR}/pystring.cpp")
  target_include_directories(pystring PUBLIC "${ps_color_pystring_SOURCE_DIR}")
  add_library(pystring::pystring ALIAS pystring)
  ps_color_fetch(ps_color_imath https://github.com/AcademySoftwareFoundation/Imath/archive/refs/tags/v3.2.1.tar.gz
    b2c8a44c3e4695b74e9644c76f5f5480767355c6f98cde58ba0e82b4ad8c63ce)
  set(IMATH_INSTALL OFF)
  set(IMATH_BUILD_TESTS OFF)
  set(IMATH_BUILD_EXAMPLES OFF)
  set(PYTHON OFF)
  set(PYBIND11 OFF)
  add_subdirectory("${ps_color_imath_SOURCE_DIR}" color-engines/imath EXCLUDE_FROM_ALL)
  ps_color_fetch(ps_color_minizip https://github.com/zlib-ng/minizip-ng/archive/refs/tags/4.0.10.tar.gz
    c362e35ee973fa7be58cc5e38a4a6c23cc8f7e652555daf4f115a9eb2d3a6be7)
  foreach(feature COMPAT ZLIB BZIP2 LZMA ZSTD LIBCOMP FETCH_LIBS PKCRYPT WZAES
      OPENSSL LIBBSD ICONV BUILD_TESTS BUILD_UNIT_TESTS BUILD_FUZZ_TESTS)
    set(MZ_${feature} OFF)
  endforeach()
  add_subdirectory("${ps_color_minizip_SOURCE_DIR}" color-engines/minizip EXCLUDE_FROM_ALL)
  ps_color_fetch(ps_color_sse2neon
    https://github.com/DLTcollab/sse2neon/archive/227cc413fb2d50b2a10073087be96b59d5364aea.tar.gz
    3427a495743bb6fd1b5f9f806b80f57d67b1ac7ccf39a5f44aedd487fd7e6da1)
  set(sse2neon_INCLUDE_DIR "${ps_color_sse2neon_SOURCE_DIR}")
  ps_color_fetch(ps_color_ocio https://github.com/AcademySoftwareFoundation/OpenColorIO/archive/refs/tags/v2.5.2.tar.gz
    722601e01b78b7a12da4829cb450674935f404b0e508f3f20046fa77570e3272)
  set(_ocio "${CMAKE_CURRENT_BINARY_DIR}/color-engines/ocio2.5.2")
  execute_process(COMMAND "${Python3_EXECUTABLE}" "${_ps_color}/prepare_ocio.py"
    --source "${ps_color_ocio_SOURCE_DIR}" --output "${_ocio}"
    COMMAND_ERROR_IS_FATAL ANY)
  foreach(feature APPS TESTS GPU_TESTS PYTHON DOCS NUKE OPENFX JAVA)
    set(OCIO_BUILD_${feature} OFF)
  endforeach()
  set(OCIO_INSTALL_EXT_PACKAGES NONE)
  set(OCIO_NAMESPACE Photospider_OCIO_2_5_2)
  set(OCIO_USE_AVX OFF)
  set(OCIO_USE_AVX2 OFF)
  set(OCIO_USE_AVX512 OFF)
  set(OCIO_USE_F16C OFF)
  add_subdirectory("${_ocio}" color-engines/ocio-build EXCLUDE_FROM_ALL)
  foreach(dependency OpenColorIO yaml-cpp pystring Imath expat minizip-ng)
    if(TARGET ${dependency})
      target_compile_options(${dependency} PRIVATE -fno-fast-math -ffp-contract=off)
    endif()
  endforeach()
  foreach(feature SIMD SSE2 SSE3 SSSE3 SSE4 SSE42 SSE2NEON AVX AVX2 AVX512 F16C)
    string(APPEND _build_basis ";${feature}=${OCIO_USE_${feature}}")
  endforeach()
  string(SHA256 _build_hash "${_build_basis}")
  string(SUBSTRING "${_build_hash}" 0 24 _build_short)
  target_compile_definitions(photospider_color_engine PRIVATE
    PS_COLOR_BUILD_HASH="${_build_short}")
  target_include_directories(OpenColorIO PRIVATE "${ps_color_imath_SOURCE_DIR}/src")
  target_link_libraries(photospider_color_engine PRIVATE OpenColorIO yaml-cpp pystring)
  set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
    "${_ps_color}/prepare_ocio.py" "${_ps_color}/prepare_runtime.py")
  install(FILES "${ps_color_ocio_SOURCE_DIR}/LICENSE"
    DESTINATION ${CMAKE_INSTALL_DATADIR}/licenses/Photospider RENAME OpenColorIO.txt)

  foreach(dependency yaml pystring minizip sse2neon)
    install(FILES "${ps_color_${dependency}_SOURCE_DIR}/LICENSE"
      DESTINATION ${CMAKE_INSTALL_DATADIR}/licenses/Photospider
      RENAME "${dependency}.txt")
  endforeach()
  install(FILES "${ps_color_expat_SOURCE_DIR}/expat/COPYING"
    DESTINATION ${CMAKE_INSTALL_DATADIR}/licenses/Photospider RENAME expat.txt)
  install(FILES "${ps_color_imath_SOURCE_DIR}/LICENSE.md"
    DESTINATION ${CMAKE_INSTALL_DATADIR}/licenses/Photospider RENAME Imath.txt)
  get_filename_component(_runtime_root "${_runtime_inc}" DIRECTORY)
  find_file(PS_COLOR_RUNTIME_LICENSE NAMES LICENSE.TXT
    HINTS "${_runtime_root}" "${_runtime_root}/share/licenses/llvm" REQUIRED)
  install(FILES "${PS_COLOR_RUNTIME_LICENSE}"
    DESTINATION ${CMAKE_INSTALL_DATADIR}/licenses/Photospider RENAME LLVM-runtime.txt)
endfunction()
ps_configure_ocio()
