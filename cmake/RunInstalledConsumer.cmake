# Installs the just-built kernel and verifies a clean external package consumer.
set(_install_prefix "${PHOTOSPIDER_BINARY_DIR}/consumer-install")
set(_consumer_binary "${PHOTOSPIDER_BINARY_DIR}/consumer-build")

file(REMOVE_RECURSE "${_install_prefix}" "${_consumer_binary}")

set(_config_args)
if(PHOTOSPIDER_BUILD_CONFIG)
  list(APPEND _config_args --config "${PHOTOSPIDER_BUILD_CONFIG}")
endif()

if(NOT PHOTOSPIDER_GENERATOR)
  message(FATAL_ERROR "Photospider consumer generator was not provided")
endif()
if(NOT PHOTOSPIDER_CONSUMER_SANITIZER MATCHES "^(none|address|thread)$")
  message(FATAL_ERROR "Photospider consumer sanitizer mode is invalid")
endif()
set(_generator_args -G "${PHOTOSPIDER_GENERATOR}")
if(PHOTOSPIDER_GENERATOR_PLATFORM)
  list(APPEND _generator_args -A "${PHOTOSPIDER_GENERATOR_PLATFORM}")
endif()
if(PHOTOSPIDER_GENERATOR_TOOLSET)
  list(APPEND _generator_args -T "${PHOTOSPIDER_GENERATOR_TOOLSET}")
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
          ${_generator_args}
          -S "${PHOTOSPIDER_SOURCE_DIR}/tests/consumer"
          -B "${_consumer_binary}"
          "-DCMAKE_PREFIX_PATH:PATH=${_install_prefix}"
          "-DCMAKE_BUILD_TYPE:STRING=${PHOTOSPIDER_BUILD_CONFIG}"
          "-DPHOTOSPIDER_CONSUMER_SANITIZER:STRING=${PHOTOSPIDER_CONSUMER_SANITIZER}"
  RESULT_VARIABLE _configure_result)
if(NOT _configure_result EQUAL 0)
  message(FATAL_ERROR "Photospider consumer configure failed")
endif()

execute_process(
  COMMAND "${CMAKE_COMMAND}" --build "${_consumer_binary}"
          --target run_photospider_consumer ${_config_args}
  RESULT_VARIABLE _build_result)
if(NOT _build_result EQUAL 0)
  message(FATAL_ERROR "Photospider consumer build or runtime failed")
endif()
