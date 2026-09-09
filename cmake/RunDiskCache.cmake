# Separate executable processes prove that no in-memory object authorizes hits.
set(_directory "${PHOTOSPIDER_BINARY_DIR}/disk-cache-test")
file(REMOVE_RECURSE "${_directory}")
foreach(_mode write read corrupt read truncate read checksum read version read delete read write_failure quota pressure)
  execute_process(COMMAND "${PHOTOSPIDER_DISK_TEST}" "${_mode}" "${_directory}"
                  RESULT_VARIABLE _result)
  if(NOT _result EQUAL 0)
    message(FATAL_ERROR "S3 disk scenario ${_mode} failed: ${_result}")
  endif()
endforeach()
file(REMOVE_RECURSE "${_directory}")
