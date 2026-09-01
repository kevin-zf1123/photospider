# Installs the just-built kernel and verifies a clean external package consumer.
set(_install_prefix "${PHOTOSPIDER_BINARY_DIR}/consumer-install")
set(_consumer_binary "${PHOTOSPIDER_BINARY_DIR}/consumer-build")

file(REMOVE_RECURSE "${_install_prefix}" "${_consumer_binary}")

set(_config_args)
if(PHOTOSPIDER_BUILD_CONFIG)
  list(APPEND _config_args --config "${PHOTOSPIDER_BUILD_CONFIG}")
endif()

execute_process(
  COMMAND "${CMAKE_COMMAND}" --install "${PHOTOSPIDER_BINARY_DIR}"
          --prefix "${_install_prefix}" ${_config_args}
  RESULT_VARIABLE _install_result)
if(NOT _install_result EQUAL 0)
  message(FATAL_ERROR "Photospider isolated install failed")
endif()

execute_process(
  COMMAND "${CMAKE_COMMAND}"
          -S "${PHOTOSPIDER_SOURCE_DIR}/tests/consumer"
          -B "${_consumer_binary}"
          -DCMAKE_PREFIX_PATH=${_install_prefix}
  RESULT_VARIABLE _configure_result)
if(NOT _configure_result EQUAL 0)
  message(FATAL_ERROR "Photospider consumer configure failed")
endif()

execute_process(
  COMMAND "${CMAKE_COMMAND}" --build "${_consumer_binary}" ${_config_args}
  RESULT_VARIABLE _build_result)
if(NOT _build_result EQUAL 0)
  message(FATAL_ERROR "Photospider consumer build failed")
endif()

execute_process(
  COMMAND "${_consumer_binary}/photospider_consumer"
  RESULT_VARIABLE _run_result)
if(NOT _run_result EQUAL 0)
  message(FATAL_ERROR "Photospider consumer runtime failed")
endif()
