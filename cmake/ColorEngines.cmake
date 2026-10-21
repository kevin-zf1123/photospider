# Private external CMMs belong to bundled operations, never to the kernel.
include(FetchContent)
find_package(Python3 COMPONENTS Interpreter REQUIRED)
FetchContent_Declare(photospider_lcms_source
  URL https://github.com/mm2/Little-CMS/archive/refs/tags/lcms2.19.1.tar.gz
  URL_HASH SHA256=267705e278e2f7c2fb886c259dadcbaeb2be52748bcbc71c79f08aacacb7a709
  DOWNLOAD_EXTRACT_TIMESTAMP TRUE)
FetchContent_GetProperties(photospider_lcms_source)
if(NOT photospider_lcms_source_POPULATED)
  FetchContent_Populate(photospider_lcms_source)
endif()
set(_ps_color "${CMAKE_CURRENT_SOURCE_DIR}/plugins/ops/02-format-color")
set(_ps_lcms "${CMAKE_CURRENT_BINARY_DIR}/color-engines/lcms2.19.1")
execute_process(COMMAND "${Python3_EXECUTABLE}" "${_ps_color}/prepare_engines.py"
  --lcms-source "${photospider_lcms_source_SOURCE_DIR}" --lcms-output "${_ps_lcms}"
  COMMAND_ERROR_IS_FATAL ANY)
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
  "${_ps_color}/prepare_engines.py")
file(GLOB _ps_lcms_sources CONFIGURE_DEPENDS "${_ps_lcms}/src/*.c")
add_library(photospider_lcms_objects OBJECT ${_ps_lcms_sources})
set_target_properties(photospider_lcms_objects PROPERTIES
  POSITION_INDEPENDENT_CODE ON C_VISIBILITY_PRESET hidden C_STANDARD 11)
target_include_directories(photospider_lcms_objects PRIVATE
  "${_ps_lcms}/include" "${_ps_color}")
target_compile_definitions(photospider_lcms_objects PRIVATE
  HAVE_GMTIME_R=1 HasTHREADS=1)
target_compile_options(photospider_lcms_objects PRIVATE
  -include "${_ps_color}/engine_redirect.h"
  -fno-fast-math -ffp-contract=off)
add_library(photospider_color_engine SHARED
  "${_ps_color}/engine_memory.c" "${_ps_color}/icc_engine.c"
  $<TARGET_OBJECTS:photospider_lcms_objects>)
set_target_properties(photospider_color_engine PROPERTIES
  EXPORT_NAME color_engine C_STANDARD 11 CXX_STANDARD 17
  C_VISIBILITY_PRESET hidden CXX_VISIBILITY_PRESET hidden
  VISIBILITY_INLINES_HIDDEN YES VERSION 1 SOVERSION 1)
target_include_directories(photospider_color_engine PRIVATE
  "${_ps_lcms}/include" "${_ps_lcms}/src" "${_ps_color}")
target_compile_options(photospider_color_engine PRIVATE
  -fno-fast-math -ffp-contract=off)
find_package(Threads REQUIRED)
target_link_libraries(photospider_color_engine PRIVATE Threads::Threads)
if(UNIX AND NOT APPLE)
  target_link_libraries(photospider_color_engine PRIVATE m)
endif()
install(TARGETS photospider_color_engine EXPORT PhotospiderTargets
  LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
  RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
install(FILES "${photospider_lcms_source_SOURCE_DIR}/LICENSE"
  DESTINATION ${CMAKE_INSTALL_DATADIR}/licenses/Photospider RENAME LittleCMS.txt)

include("${CMAKE_CURRENT_SOURCE_DIR}/cmake/ColorOCIO.cmake")
