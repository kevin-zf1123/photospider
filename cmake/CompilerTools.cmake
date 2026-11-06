if(NOT PHOTOSPIDER_BUILD_TOOLS)
  return()
endif()
foreach(_file IN ITEMS yyjson.c yyjson.h LICENSE)
  if(NOT EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/third_party/yyjson/${_file}")
    message(FATAL_ERROR
      "psc/psx require third_party/yyjson/${_file}. Prepare the pinned source "
      "using third_party/YYJSON.md, or set PHOTOSPIDER_BUILD_TOOLS=OFF.")
  endif()
endforeach()
add_library(photospider_cli_json STATIC third_party/yyjson/yyjson.c)
target_include_directories(photospider_cli_json SYSTEM PUBLIC "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/third_party/yyjson>")
target_compile_definitions(photospider_cli_json PRIVATE
  YYJSON_DISABLE_NON_STANDARD=1 YYJSON_DISABLE_FILE=1
  YYJSON_READER_DEPTH_LIMIT=128 YYJSON_WRITER_DEPTH_LIMIT=128)
set_target_properties(photospider_cli_json PROPERTIES POSITION_INDEPENDENT_CODE ON)
install(FILES third_party/yyjson/LICENSE
  DESTINATION ${CMAKE_INSTALL_DATADIR}/licenses/Photospider/yyjson)
add_library(photospider_cli_common STATIC
  src/tools/common/json.cpp src/tools/common/options.cpp
  src/tools/common/file_io.cpp src/tools/common/diagnostic_renderer.cpp
  src/tools/common/process.cpp src/tools/common/environment.cpp
  src/tools/common/schema_codec.cpp src/tools/common/resource_manifest.cpp
  src/tools/common/request_manifest.cpp)
target_include_directories(photospider_cli_common PUBLIC "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/src/tools>")
target_link_libraries(photospider_cli_common PUBLIC Photospider::kernel PRIVATE photospider_cli_json)
target_compile_features(photospider_cli_common PUBLIC cxx_std_17)
target_compile_definitions(photospider_cli_common PRIVATE PHOTOSPIDER_CLI_VERSION="${PROJECT_VERSION}")
if(PHOTOSPIDER_BUILD_OPS)
  target_link_libraries(photospider_cli_common PRIVATE photospider_ops)
  target_compile_definitions(photospider_cli_common PRIVATE PHOTOSPIDER_CLI_BUILTINS=1)
endif()
add_library(photospider_psc STATIC
  src/tools/psc/options.cpp src/tools/psc/driver.cpp
  src/tools/psc/action_graph.cpp src/tools/psc/compilation_session.cpp
  src/tools/psc/dag_io.cpp)
add_library(photospider_psx STATIC
  src/tools/psx/options.cpp src/tools/psx/driver.cpp
  src/tools/psx/bindings_io.cpp)
add_library(photospider_result_files STATIC src/tools/psx/result_export.cpp)
add_library(Photospider::result_files ALIAS photospider_result_files)
set_target_properties(photospider_result_files PROPERTIES EXPORT_NAME result_files POSITION_INDEPENDENT_CODE ON)
target_include_directories(photospider_result_files PRIVATE src/lib third_party/yyjson)
target_link_libraries(photospider_result_files PUBLIC Photospider::kernel PRIVATE photospider_cli_common)
target_link_libraries(photospider_psx PUBLIC photospider_result_files)
install(TARGETS photospider_result_files photospider_cli_common photospider_cli_json
        EXPORT PhotospiderTargets ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR})
set_target_properties(photospider_cli_common PROPERTIES POSITION_INDEPENDENT_CODE ON)
foreach(_library IN ITEMS photospider_cli_common photospider_psc photospider_psx photospider_result_files)
  target_compile_options(${_library} PRIVATE -Wall -Wextra -Wpedantic -Werror
    -fno-fast-math -frounding-math -ffp-contract=off)
endforeach()
foreach(_tool IN ITEMS psc psx)
  target_link_libraries(photospider_${_tool} PUBLIC photospider_cli_common)
  target_include_directories(photospider_${_tool} PRIVATE third_party/yyjson)
  add_executable(${_tool} src/tools/${_tool}/main.cpp)
  target_link_libraries(${_tool} PRIVATE photospider_${_tool})
  install(TARGETS ${_tool} RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
endforeach()
if(BUILD_TESTING)
  find_package(Python3 COMPONENTS Interpreter REQUIRED)
  add_library(cli_resource_fixture MODULE tests/fixtures/cli_resource_fixture.cpp)
  target_link_libraries(cli_resource_fixture PRIVATE Photospider::operation_sdk)
  target_compile_features(cli_resource_fixture PRIVATE cxx_std_17)
  target_compile_options(cli_resource_fixture PRIVATE -Wall -Wextra -Wpedantic -Werror)
  target_compile_definitions(cli_resource_fixture PRIVATE PHOTOSPIDER_OPERATION_PLUGIN_BUILD)
  set_target_properties(cli_resource_fixture PROPERTIES PREFIX "")
  add_test(NAME test_compiler_tools COMMAND ${Python3_EXECUTABLE}
    ${CMAKE_CURRENT_SOURCE_DIR}/tests/tools/test_compiler_tools.py
    $<TARGET_FILE:psc> $<TARGET_FILE:psx> $<TARGET_FILE:cli_resource_fixture>
    ${PHOTOSPIDER_BUILD_OPS})
endif()
if(BUILD_TESTING)
  add_executable(test_cli_io tests/unit/kernel/test_cli_io.cpp)
  target_link_libraries(test_cli_io PRIVATE photospider_psx)
  target_include_directories(test_cli_io PRIVATE third_party/yyjson)
  add_test(NAME test_cli_io COMMAND test_cli_io)
endif()
