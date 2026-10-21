option(PHOTOSPIDER_BUILD_OPS_SDK "Build shared operator domain SDK" ${PHOTOSPIDER_BUILD_OPS})
if(PHOTOSPIDER_BUILD_OPS AND NOT PHOTOSPIDER_BUILD_OPS_SDK)
  message(FATAL_ERROR "Bundled operations require PHOTOSPIDER_BUILD_OPS_SDK")
endif()
set(PHOTOSPIDER_OPS_SDK_SOURCES
  plugins/ops/common/domain/exact_numeric.cpp
  plugins/ops/common/domain/transfer_definition.cpp
  plugins/ops/common/domain/rgb_geometry.cpp
  plugins/ops/common/domain/rgb_geometry_codec.cpp
  plugins/ops/common/domain/description_editor.cpp
  plugins/ops/common/domain/color_array.cpp
  plugins/ops/common/domain/tensor_description.cpp
  plugins/ops/common/domain/icc_profile.cpp
  plugins/ops/common/domain/icc_validation.cpp
  plugins/ops/common/domain/ocio_config_resource.cpp
  plugins/ops/01-numeric/lut3d_bake.cpp
  plugins/ops/01-numeric/lut3d_bake_validation.cpp
  plugins/ops/01-numeric/quality.cpp
  plugins/ops/09-composite/layer.cpp)
add_library(photospider_ops_sdk ${PHOTOSPIDER_OPS_SDK_SOURCES})
if(NOT PHOTOSPIDER_BUILD_OPS_SDK)
  set_target_properties(photospider_ops_sdk PROPERTIES EXCLUDE_FROM_ALL ON)
endif()
add_library(Photospider::ops_sdk ALIAS photospider_ops_sdk)
set_target_properties(photospider_ops_sdk PROPERTIES EXPORT_NAME ops_sdk
  CXX_EXTENSIONS OFF POSITION_INDEPENDENT_CODE ON
  VERSION ${PROJECT_VERSION} SOVERSION "${PROJECT_VERSION_MAJOR}.${PROJECT_VERSION_MINOR}")
target_compile_features(photospider_ops_sdk PUBLIC cxx_std_17)
target_link_libraries(photospider_ops_sdk PUBLIC Photospider::kernel)
target_include_directories(photospider_ops_sdk PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/plugins/ops"
  PUBLIC $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/plugins/ops/include>)
target_compile_options(photospider_ops_sdk PRIVATE -Wall -Wextra -Wpedantic -Werror
  -fno-fast-math -frounding-math -ffp-contract=off)
if(PHOTOSPIDER_BUILD_OPS_SDK)
install(TARGETS photospider_ops_sdk EXPORT PhotospiderTargets
  ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
  LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
  RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
install(DIRECTORY include/photospider/ops DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}/photospider FILES_MATCHING PATTERN "*.hpp")
endif()

if(BUILD_SHARED_LIBS)
  target_compile_definitions(photospider_ops_sdk PRIVATE PHOTOSPIDER_OPS_SDK_BUILD)
else()
  target_compile_definitions(photospider_ops_sdk PUBLIC PHOTOSPIDER_OPS_SDK_STATIC)
endif()
