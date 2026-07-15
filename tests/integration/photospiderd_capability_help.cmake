cmake_minimum_required(VERSION 3.16)

# PASS_REGULAR_EXPRESSION ignores child exit status, so this driver owns both
# successful execution and capability-output assertions.
if(NOT DEFINED PHOTOSPIDERD OR "${PHOTOSPIDERD}" STREQUAL "")
  message(FATAL_ERROR
    "Photospiderd capability help driver requires PHOTOSPIDERD")
endif()

execute_process(
  COMMAND "${PHOTOSPIDERD}" --help
  RESULT_VARIABLE HELP_RESULT
  OUTPUT_VARIABLE HELP_STDOUT
  ERROR_VARIABLE HELP_STDERR
  TIMEOUT 5
)

if(NOT "${HELP_RESULT}" MATCHES "^[0-9]+$")
  message(FATAL_ERROR
    "Failed to launch photospiderd --help\n"
    "  executable: ${PHOTOSPIDERD}\n"
    "  result: ${HELP_RESULT}\n"
    "  stdout:\n${HELP_STDOUT}\n"
    "  stderr:\n${HELP_STDERR}")
endif()

if(NOT "${HELP_RESULT}" STREQUAL "0")
  message(FATAL_ERROR
    "photospiderd --help exited unsuccessfully\n"
    "  executable: ${PHOTOSPIDERD}\n"
    "  exit code: ${HELP_RESULT}\n"
    "  stdout:\n${HELP_STDOUT}\n"
    "  stderr:\n${HELP_STDERR}")
endif()

set(CAPABILITY_TEXT
  "foreground same-user local Unix-domain sidecar with one embedded Host, not a system service, multi-user service, remote endpoint, or TCP server")
set(HELP_OUTPUT "${HELP_STDOUT}\n${HELP_STDERR}")
string(FIND "${HELP_OUTPUT}" "${CAPABILITY_TEXT}" CAPABILITY_POSITION)
if(CAPABILITY_POSITION EQUAL -1)
  message(FATAL_ERROR
    "photospiderd --help omitted the capability boundary\n"
    "  executable: ${PHOTOSPIDERD}\n"
    "  stdout:\n${HELP_STDOUT}\n"
    "  stderr:\n${HELP_STDERR}")
endif()
