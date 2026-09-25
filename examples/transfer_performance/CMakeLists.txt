add_executable(photospider_transfer_performance main.cpp)
target_link_libraries(photospider_transfer_performance PRIVATE Photospider::kernel)
target_compile_features(photospider_transfer_performance PRIVATE cxx_std_17)
# Development benchmark: independent checked-in golden values gate timing.
target_include_directories(photospider_transfer_performance PRIVATE ${PROJECT_SOURCE_DIR}/tests)
target_compile_options(photospider_transfer_performance PRIVATE -fno-fast-math -frounding-math -ffp-contract=off)
