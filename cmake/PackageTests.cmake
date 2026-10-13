photospider_register_test(NAME test_package_version SUITE package LEVEL contract
    COMMAND ${CMAKE_COMMAND}
      -DVERSION_FILE=${CMAKE_CURRENT_BINARY_DIR}/PhotospiderConfigVersion.cmake
      -DCURRENT_VERSION=${PROJECT_VERSION}
      -P ${CMAKE_CURRENT_SOURCE_DIR}/tests/package/test_package_version.cmake)
