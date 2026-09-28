add_executable(photospider_dynamic_scheduler main.cpp)
target_link_libraries(photospider_dynamic_scheduler PRIVATE Photospider::kernel)
target_compile_features(photospider_dynamic_scheduler PRIVATE cxx_std_17)

add_executable(photospider_mixed_scheduler mixed.cpp)
target_link_libraries(photospider_mixed_scheduler PRIVATE Photospider::kernel)
target_compile_features(photospider_mixed_scheduler PRIVATE cxx_std_17)

if(UNIX)
  foreach(kind IN ITEMS dynamic mixed)
    if(kind STREQUAL "dynamic")
      set(profile_source main.cpp)
    else()
      set(profile_source mixed.cpp)
    endif()
    add_executable(photospider_${kind}_allocations EXCLUDE_FROM_ALL
      ${profile_source} allocation_profile.cpp)
    target_link_libraries(photospider_${kind}_allocations PRIVATE Photospider::kernel)
    target_compile_features(photospider_${kind}_allocations PRIVATE cxx_std_17)
    target_compile_definitions(photospider_${kind}_allocations PRIVATE
      PHOTOSPIDER_PROFILE_ALLOCATIONS=1)
  endforeach()
endif()

if(BUILD_TESTING)
  add_executable(photospider_dynamic_costs EXCLUDE_FROM_ALL
    main.cpp copy_profile.cpp)
  target_include_directories(photospider_dynamic_costs PRIVATE
    ${CMAKE_SOURCE_DIR}/src/lib)
  target_link_libraries(photospider_dynamic_costs PRIVATE photospider_test_kernel)
  target_compile_features(photospider_dynamic_costs PRIVATE cxx_std_17)
  target_compile_definitions(photospider_dynamic_costs PRIVATE
    PHOTOSPIDER_PROFILE_COPIES=1)
  add_executable(photospider_mixed_transfers EXCLUDE_FROM_ALL
    mixed.cpp copy_profile.cpp)
  target_include_directories(photospider_mixed_transfers PRIVATE
    ${CMAKE_SOURCE_DIR}/src/lib)
  target_link_libraries(photospider_mixed_transfers PRIVATE photospider_test_kernel)
  target_compile_features(photospider_mixed_transfers PRIVATE cxx_std_17)
  target_compile_definitions(photospider_mixed_transfers PRIVATE
    PHOTOSPIDER_PROFILE_COPIES=1)
endif()
