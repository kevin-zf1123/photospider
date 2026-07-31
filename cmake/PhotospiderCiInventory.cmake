# @brief Reject bytes that cannot be represented safely in a line manifest.
#
# @param FIELD_CONTEXT Controlled diagnostic context that identifies the field
#   position without reproducing its untrusted contents.
# @param FIELD_VALUE Exact CMake string that would otherwise be serialized.
# @param ALLOW_BACKSLASH Whether a backslash is data rather than a path-format
#   violation at this manifest boundary.
# @return None; validation success leaves the caller's state unchanged.
# @throws FATAL_ERROR If FIELD_VALUE contains a disallowed backslash, an ASCII
#   control character in the range 1 through 31, or ASCII DEL (127).
# @note CMake strings cannot represent NUL. Downstream readers still reject NUL
#   so forged or externally modified manifests fail closed.
function(_photospider_require_ci_inventory_field
        FIELD_CONTEXT FIELD_VALUE ALLOW_BACKSLASH)
    if(NOT ALLOW_BACKSLASH)
        string(FIND "${FIELD_VALUE}" "\\" PHOTOSPIDER_BACKSLASH_OFFSET)
        if(NOT PHOTOSPIDER_BACKSLASH_OFFSET EQUAL -1)
            message(FATAL_ERROR
                "${FIELD_CONTEXT} contains a forbidden backslash")
        endif()
    endif()

    foreach(PHOTOSPIDER_CONTROL_CODE RANGE 1 31)
        string(ASCII "${PHOTOSPIDER_CONTROL_CODE}"
            PHOTOSPIDER_CONTROL_CHARACTER)
        string(FIND "${FIELD_VALUE}" "${PHOTOSPIDER_CONTROL_CHARACTER}"
            PHOTOSPIDER_CONTROL_OFFSET)
        if(NOT PHOTOSPIDER_CONTROL_OFFSET EQUAL -1)
            message(FATAL_ERROR
                "${FIELD_CONTEXT} contains forbidden ASCII control "
                "code ${PHOTOSPIDER_CONTROL_CODE}")
        endif()
    endforeach()

    string(ASCII 127 PHOTOSPIDER_DELETE_CHARACTER)
    string(FIND "${FIELD_VALUE}" "${PHOTOSPIDER_DELETE_CHARACTER}"
        PHOTOSPIDER_DELETE_OFFSET)
    if(NOT PHOTOSPIDER_DELETE_OFFSET EQUAL -1)
        message(FATAL_ERROR
            "${FIELD_CONTEXT} contains forbidden ASCII control code 127")
    endif()
endfunction()

# @brief Serialize the configured installable public-header allowlist.
#
# @param OUTPUT_PATH Build-tree manifest path written during configuration.
# @param HEADER_LIST_VARIABLE Name of the caller-owned CMake list containing
#   paths relative to include/photospider.
# @return None; writes one install-relative POSIX header path per line.
# @throws FATAL_ERROR If an entry contains a backslash or a CMake-representable
#   ASCII control character, or if the output cannot be written.
# @note Ordinary spaces remain valid POSIX path data. Validation happens before
#   any entry is appended, so a rejected field cannot inject another record.
function(photospider_write_public_header_inventory
        OUTPUT_PATH HEADER_LIST_VARIABLE)
    set(PHOTOSPIDER_PUBLIC_HEADER_INVENTORY_CONTENT "")
    set(PHOTOSPIDER_PUBLIC_HEADER_INDEX 0)
    foreach(PHOTOSPIDER_PUBLIC_HEADER_RELATIVE_PATH
            IN LISTS ${HEADER_LIST_VARIABLE})
        math(EXPR PHOTOSPIDER_PUBLIC_HEADER_INDEX
            "${PHOTOSPIDER_PUBLIC_HEADER_INDEX} + 1")
        _photospider_require_ci_inventory_field(
            "Public-header allowlist entry ${PHOTOSPIDER_PUBLIC_HEADER_INDEX}"
            "${PHOTOSPIDER_PUBLIC_HEADER_RELATIVE_PATH}"
            FALSE)
        string(APPEND PHOTOSPIDER_PUBLIC_HEADER_INVENTORY_CONTENT
            "include/photospider/${PHOTOSPIDER_PUBLIC_HEADER_RELATIVE_PATH}\n")
    endforeach()

    get_filename_component(PHOTOSPIDER_PUBLIC_HEADER_INVENTORY_DIRECTORY
        "${OUTPUT_PATH}" DIRECTORY)
    file(MAKE_DIRECTORY
        "${PHOTOSPIDER_PUBLIC_HEADER_INVENTORY_DIRECTORY}")
    file(WRITE "${OUTPUT_PATH}"
        "${PHOTOSPIDER_PUBLIC_HEADER_INVENTORY_CONTENT}")
endfunction()

# @brief Generate the configuration-specific registered-GTest TSV manifest.
#
# @param OUTPUT_PATH Build-tree output path, normally containing $<CONFIG>.
# @param TARGET_LIST_VARIABLE Name of the caller-owned sorted list of targets
#   registered exactly once through gtest_discover_tests.
# @return None; schedules one generated TSV per selected configuration.
# @throws FATAL_ERROR If a target name contains unsafe manifest data, is not a
#   legal local CMake target name, names a _NOT_BUILT sentinel, is missing, or
#   is not an executable target.
# @note Executable paths remain $<TARGET_FILE:...> generator expressions so
#   single- and multi-config generators resolve the correct native path. The
#   downstream parser validates the resolved field, including control bytes.
function(photospider_generate_registered_gtest_target_inventory
        OUTPUT_PATH TARGET_LIST_VARIABLE)
    set(PHOTOSPIDER_GTEST_TARGET_INVENTORY_CONTENT
        "# target\tconfigured executable\n")
    set(PHOTOSPIDER_GTEST_TARGET_INDEX 0)
    foreach(PHOTOSPIDER_REGISTERED_GTEST_TARGET
            IN LISTS ${TARGET_LIST_VARIABLE})
        math(EXPR PHOTOSPIDER_GTEST_TARGET_INDEX
            "${PHOTOSPIDER_GTEST_TARGET_INDEX} + 1")
        set(PHOTOSPIDER_GTEST_TARGET_CONTEXT
            "Registered GoogleTest target entry ${PHOTOSPIDER_GTEST_TARGET_INDEX}")
        _photospider_require_ci_inventory_field(
            "${PHOTOSPIDER_GTEST_TARGET_CONTEXT}"
            "${PHOTOSPIDER_REGISTERED_GTEST_TARGET}"
            FALSE)
        if(NOT "${PHOTOSPIDER_REGISTERED_GTEST_TARGET}" MATCHES
                "^[A-Za-z0-9_.+-]+$" OR
           "${PHOTOSPIDER_REGISTERED_GTEST_TARGET}" MATCHES "_NOT_BUILT$")
            message(FATAL_ERROR
                "${PHOTOSPIDER_GTEST_TARGET_CONTEXT} has an invalid CMake target name")
        endif()
        if(NOT TARGET "${PHOTOSPIDER_REGISTERED_GTEST_TARGET}")
            message(FATAL_ERROR
                "${PHOTOSPIDER_GTEST_TARGET_CONTEXT} does not name a target")
        endif()
        get_target_property(PHOTOSPIDER_GTEST_TARGET_TYPE
            "${PHOTOSPIDER_REGISTERED_GTEST_TARGET}" TYPE)
        if(NOT PHOTOSPIDER_GTEST_TARGET_TYPE STREQUAL "EXECUTABLE")
            message(FATAL_ERROR
                "${PHOTOSPIDER_GTEST_TARGET_CONTEXT} is not executable")
        endif()
        string(APPEND PHOTOSPIDER_GTEST_TARGET_INVENTORY_CONTENT
            "${PHOTOSPIDER_REGISTERED_GTEST_TARGET}\t"
            "$<TARGET_FILE:${PHOTOSPIDER_REGISTERED_GTEST_TARGET}>\n")
    endforeach()

    get_filename_component(PHOTOSPIDER_GTEST_TARGET_INVENTORY_DIRECTORY
        "${OUTPUT_PATH}" DIRECTORY)
    file(MAKE_DIRECTORY "${PHOTOSPIDER_GTEST_TARGET_INVENTORY_DIRECTORY}")
    file(GENERATE OUTPUT "${OUTPUT_PATH}"
        CONTENT "${PHOTOSPIDER_GTEST_TARGET_INVENTORY_CONTENT}")
endfunction()
