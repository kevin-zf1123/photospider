add_executable(photospider_fmt10_performance main.cpp)
target_link_libraries(photospider_fmt10_performance PRIVATE Photospider::kernel)
target_compile_features(photospider_fmt10_performance PRIVATE cxx_std_17)
target_compile_options(photospider_fmt10_performance PRIVATE
  -Wall -Wextra -Wpedantic -Werror -fno-fast-math -frounding-math -ffp-contract=off)

# Private diagnostic tool: never installed and no allocator override in library.
add_executable(photospider_fmt10_kernel EXCLUDE_FROM_ALL kernel.cpp)
target_link_libraries(photospider_fmt10_kernel PRIVATE Photospider::kernel)
target_include_directories(photospider_fmt10_kernel PRIVATE
  ${PROJECT_SOURCE_DIR}/plugins/ops ${PROJECT_SOURCE_DIR}/src/lib)
target_compile_features(photospider_fmt10_kernel PRIVATE cxx_std_17)
target_compile_options(photospider_fmt10_kernel PRIVATE
  -Wall -Wextra -Wpedantic -Werror -fno-fast-math -frounding-math -ffp-contract=off)
