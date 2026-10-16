option(PHOTOSPIDER_BUILD_BENCHMARK_TOOLS "Build optional raw benchmark client library" OFF)
add_library(photospider_benchmark_tools src/tools/benchmark/raw_benchmark.cpp)
if(NOT PHOTOSPIDER_BUILD_BENCHMARK_TOOLS)
  set_target_properties(photospider_benchmark_tools PROPERTIES EXCLUDE_FROM_ALL ON)
endif()
add_library(Photospider::benchmark_tools ALIAS photospider_benchmark_tools)
set_target_properties(photospider_benchmark_tools PROPERTIES EXPORT_NAME benchmark_tools
  CXX_EXTENSIONS OFF POSITION_INDEPENDENT_CODE ON
  VERSION ${PROJECT_VERSION} SOVERSION "${PROJECT_VERSION_MAJOR}.${PROJECT_VERSION_MINOR}")
target_compile_features(photospider_benchmark_tools PUBLIC cxx_std_17)
target_link_libraries(photospider_benchmark_tools PUBLIC Photospider::kernel)
target_compile_options(photospider_benchmark_tools PRIVATE -Wall -Wextra -Wpedantic -Werror)
if(BUILD_SHARED_LIBS)
  target_compile_definitions(photospider_benchmark_tools PRIVATE PHOTOSPIDER_BENCHMARK_BUILD)
else()
  target_compile_definitions(photospider_benchmark_tools PUBLIC PHOTOSPIDER_BENCHMARK_STATIC)
endif()
if(PHOTOSPIDER_BUILD_BENCHMARK_TOOLS)
  install(TARGETS photospider_benchmark_tools EXPORT PhotospiderTargets
    ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
    LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
    RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
  install(DIRECTORY include/photospider/benchmark DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}/photospider
    FILES_MATCHING PATTERN "*.hpp")
endif()
if(BUILD_TESTING AND PHOTOSPIDER_BUILD_BENCHMARK_TOOLS)
  add_library(photospider_test_benchmark_tools STATIC EXCLUDE_FROM_ALL
    src/tools/benchmark/raw_benchmark.cpp src/tools/benchmark/raw_benchmark_test_hooks.cpp)
  target_compile_features(photospider_test_benchmark_tools PUBLIC cxx_std_17)
  target_include_directories(photospider_test_benchmark_tools PRIVATE src/tools)
  target_compile_definitions(photospider_test_benchmark_tools PUBLIC PHOTOSPIDER_BENCHMARK_STATIC
    PRIVATE PHOTOSPIDER_ENABLE_RAW_BENCHMARK_TEST_HOOKS)
  target_link_libraries(photospider_test_benchmark_tools PUBLIC Photospider::kernel)
  add_executable(test_raw_benchmark tests/tools/test_raw_benchmark.cpp)
  target_link_libraries(test_raw_benchmark PRIVATE photospider_test_benchmark_tools)
  target_include_directories(test_raw_benchmark PRIVATE tests src/tools)
  target_compile_options(test_raw_benchmark PRIVATE -Wall -Wextra -Wpedantic -Werror)
  add_test(NAME test_raw_benchmark COMMAND test_raw_benchmark)
  set_tests_properties(test_raw_benchmark PROPERTIES LABELS "tools;integration")
endif()
