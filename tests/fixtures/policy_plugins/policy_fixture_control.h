#ifndef TESTS_FIXTURES_POLICY_PLUGINS_POLICY_FIXTURE_CONTROL_H_
#define TESTS_FIXTURES_POLICY_PLUGINS_POLICY_FIXTURE_CONTROL_H_

#include <stdint.h>

#include "photospider/policy/policy_plugin_api.h"

#if defined(__cplusplus)
extern "C" {
#endif

/** @brief API-table behaviors selected by the repository policy fixture. */
typedef uint32_t ps_policy_fixture_api_mode;

#define PS_POLICY_FIXTURE_API_VALID UINT32_C(0)
#define PS_POLICY_FIXTURE_API_STATUS_INVALID UINT32_C(1)
#define PS_POLICY_FIXTURE_API_STATUS_UNSUPPORTED UINT32_C(2)
#define PS_POLICY_FIXTURE_API_STATUS_OUT_OF_MEMORY UINT32_C(3)
#define PS_POLICY_FIXTURE_API_STATUS_INTERNAL UINT32_C(4)
#define PS_POLICY_FIXTURE_API_STATUS_UNKNOWN UINT32_C(5)
#define PS_POLICY_FIXTURE_API_SIZE_UNDER UINT32_C(6)
#define PS_POLICY_FIXTURE_API_SIZE_OVER UINT32_C(7)
#define PS_POLICY_FIXTURE_API_KIND_INVALID UINT32_C(8)
#define PS_POLICY_FIXTURE_API_ABI_INVALID UINT32_C(9)
#define PS_POLICY_FIXTURE_API_TYPE_COUNT_ZERO UINT32_C(10)
#define PS_POLICY_FIXTURE_API_TYPE_COUNT_OVER UINT32_C(11)
#define PS_POLICY_FIXTURE_API_CALLBACK_NULL UINT32_C(12)
#define PS_POLICY_FIXTURE_API_RESERVED_NONZERO UINT32_C(13)
#define PS_POLICY_FIXTURE_API_TWO_TYPES UINT32_C(14)

/** @brief Metadata behaviors selected by the repository policy fixture. */
typedef uint32_t ps_policy_fixture_metadata_mode;

#define PS_POLICY_FIXTURE_METADATA_VALID UINT32_C(0)
#define PS_POLICY_FIXTURE_METADATA_STATUS_INVALID UINT32_C(1)
#define PS_POLICY_FIXTURE_METADATA_STATUS_UNSUPPORTED UINT32_C(2)
#define PS_POLICY_FIXTURE_METADATA_STATUS_OUT_OF_MEMORY UINT32_C(3)
#define PS_POLICY_FIXTURE_METADATA_STATUS_INTERNAL UINT32_C(4)
#define PS_POLICY_FIXTURE_METADATA_STATUS_UNKNOWN UINT32_C(5)
#define PS_POLICY_FIXTURE_METADATA_SIZE_UNDER UINT32_C(6)
#define PS_POLICY_FIXTURE_METADATA_SIZE_OVER UINT32_C(7)
#define PS_POLICY_FIXTURE_METADATA_KIND_INVALID UINT32_C(8)
#define PS_POLICY_FIXTURE_METADATA_RESERVED0_NONZERO UINT32_C(9)
#define PS_POLICY_FIXTURE_METADATA_RESERVED_NONZERO UINT32_C(10)
#define PS_POLICY_FIXTURE_METADATA_MASK_ZERO UINT32_C(11)
#define PS_POLICY_FIXTURE_METADATA_MASK_UNKNOWN UINT32_C(12)
#define PS_POLICY_FIXTURE_METADATA_NAME_NULL UINT32_C(13)
#define PS_POLICY_FIXTURE_METADATA_NAME_NONCANONICAL UINT32_C(14)
#define PS_POLICY_FIXTURE_METADATA_NAME_RESERVED UINT32_C(15)
#define PS_POLICY_FIXTURE_METADATA_DESCRIPTION_INVALID_UTF8 UINT32_C(16)
#define PS_POLICY_FIXTURE_METADATA_DESCRIPTION_TOO_LONG UINT32_C(17)
#define PS_POLICY_FIXTURE_METADATA_DUPLICATE_NAMES UINT32_C(18)

/** @brief Context-create behaviors selected by the repository policy fixture.
 */
typedef uint32_t ps_policy_fixture_create_mode;

#define PS_POLICY_FIXTURE_CREATE_SUCCESS_NULL UINT32_C(0)
#define PS_POLICY_FIXTURE_CREATE_SUCCESS_NONNULL UINT32_C(1)
#define PS_POLICY_FIXTURE_CREATE_STATUS_INVALID_NULL UINT32_C(2)
#define PS_POLICY_FIXTURE_CREATE_STATUS_INVALID_NONNULL UINT32_C(3)
#define PS_POLICY_FIXTURE_CREATE_STATUS_UNSUPPORTED UINT32_C(4)
#define PS_POLICY_FIXTURE_CREATE_STATUS_OUT_OF_MEMORY UINT32_C(5)
#define PS_POLICY_FIXTURE_CREATE_STATUS_INTERNAL UINT32_C(6)
#define PS_POLICY_FIXTURE_CREATE_STATUS_UNKNOWN UINT32_C(7)
#define PS_POLICY_FIXTURE_CREATE_HOOK_SUCCESS UINT32_C(8)

/** @brief Selection behaviors selected by the repository policy fixture. */
typedef uint32_t ps_policy_fixture_select_mode;

#define PS_POLICY_FIXTURE_SELECT_LAST UINT32_C(0)
#define PS_POLICY_FIXTURE_SELECT_FIRST UINT32_C(1)
#define PS_POLICY_FIXTURE_SELECT_ABSTAIN UINT32_C(2)
#define PS_POLICY_FIXTURE_SELECT_STATUS_INVALID UINT32_C(3)
#define PS_POLICY_FIXTURE_SELECT_STATUS_OUT_OF_MEMORY UINT32_C(4)
#define PS_POLICY_FIXTURE_SELECT_STATUS_UNKNOWN UINT32_C(5)
#define PS_POLICY_FIXTURE_SELECT_SIZE_UNDER UINT32_C(6)
#define PS_POLICY_FIXTURE_SELECT_SIZE_OVER UINT32_C(7)
#define PS_POLICY_FIXTURE_SELECT_KIND_INVALID UINT32_C(8)
#define PS_POLICY_FIXTURE_SELECT_RESERVED0_NONZERO UINT32_C(9)
#define PS_POLICY_FIXTURE_SELECT_RESERVED_NONZERO UINT32_C(10)
#define PS_POLICY_FIXTURE_SELECT_DECISION_UNKNOWN UINT32_C(11)
#define PS_POLICY_FIXTURE_SELECT_BINDING_GENERATION_MISMATCH UINT32_C(12)
#define PS_POLICY_FIXTURE_SELECT_SNAPSHOT_GENERATION_MISMATCH UINT32_C(13)
#define PS_POLICY_FIXTURE_SELECT_CANDIDATE_ZERO UINT32_C(14)
#define PS_POLICY_FIXTURE_SELECT_CANDIDATE_OUTSIDE UINT32_C(15)
#define PS_POLICY_FIXTURE_SELECT_ABSTAIN_NONNULL UINT32_C(16)
#define PS_POLICY_FIXTURE_SELECT_HOOK_LAST UINT32_C(17)
#define PS_POLICY_FIXTURE_SELECT_NONRETURNING UINT32_C(18)

/** @brief Destroy behaviors selected by the repository policy fixture. */
typedef uint32_t ps_policy_fixture_destroy_mode;

#define PS_POLICY_FIXTURE_DESTROY_OK UINT32_C(0)
#define PS_POLICY_FIXTURE_DESTROY_STATUS_INTERNAL UINT32_C(1)
#define PS_POLICY_FIXTURE_DESTROY_HOOK UINT32_C(2)

/** @brief Hook event emitted from a controlled policy callback. */
typedef uint32_t ps_policy_fixture_hook_event;

#define PS_POLICY_FIXTURE_HOOK_CREATE UINT32_C(1)
#define PS_POLICY_FIXTURE_HOOK_SELECT UINT32_C(2)
#define PS_POLICY_FIXTURE_HOOK_DESTROY UINT32_C(3)

/**
 * @brief Repository-test hook invoked by controlled fixture callbacks.
 * @param context Test-owned opaque context set before callback entry.
 * @param event Exact callback boundary event.
 * @return Test-defined scalar ignored by the fixture.
 * @note A hook must remain valid until the controlled callback returns.
 */
typedef uint32_t PS_POLICY_CALL ps_policy_fixture_hook_signature_v1(
    void* context, ps_policy_fixture_hook_event event);
typedef ps_policy_fixture_hook_signature_v1* ps_policy_fixture_hook_v1;

/**
 * @brief Restores every fixture mode, hook, and counter to defaults.
 * @return Nothing.
 * @throws Nothing.
 * @note Call only outside concurrent fixture callbacks.
 */
PS_POLICY_PLUGIN_EXPORT void PS_POLICY_CALL ps_policy_fixture_reset(void);

/**
 * @brief Selects the next API-table behavior.
 * @param mode Exact fixture API-table mode.
 * @return Nothing.
 * @throws Nothing.
 */
PS_POLICY_PLUGIN_EXPORT void PS_POLICY_CALL
ps_policy_fixture_set_api_mode(ps_policy_fixture_api_mode mode);

/**
 * @brief Selects the next metadata behavior.
 * @param mode Exact fixture metadata mode.
 * @return Nothing.
 * @throws Nothing.
 */
PS_POLICY_PLUGIN_EXPORT void PS_POLICY_CALL
ps_policy_fixture_set_metadata_mode(ps_policy_fixture_metadata_mode mode);

/**
 * @brief Selects subsequent create behavior.
 * @param mode Exact fixture create mode.
 * @return Nothing.
 * @throws Nothing.
 */
PS_POLICY_PLUGIN_EXPORT void PS_POLICY_CALL
ps_policy_fixture_set_create_mode(ps_policy_fixture_create_mode mode);

/**
 * @brief Selects subsequent selection behavior.
 * @param mode Exact fixture select mode.
 * @return Nothing.
 * @throws Nothing.
 */
PS_POLICY_PLUGIN_EXPORT void PS_POLICY_CALL
ps_policy_fixture_set_select_mode(ps_policy_fixture_select_mode mode);

/**
 * @brief Selects subsequent destroy behavior.
 * @param mode Exact fixture destroy mode.
 * @return Nothing.
 * @throws Nothing.
 */
PS_POLICY_PLUGIN_EXPORT void PS_POLICY_CALL
ps_policy_fixture_set_destroy_mode(ps_policy_fixture_destroy_mode mode);

/**
 * @brief Installs one test-owned callback hook and opaque context.
 * @param hook Test-owned hook, or null to clear it.
 * @param context Opaque hook context retained only by the test.
 * @return Nothing.
 * @throws Nothing.
 * @note Hook and context must outlive every controlled callback.
 */
PS_POLICY_PLUGIN_EXPORT void PS_POLICY_CALL
ps_policy_fixture_set_hook(ps_policy_fixture_hook_v1 hook, void* context);

/**
 * @brief Returns the number of fixture create callback entries.
 * @return Exact count since the last reset.
 * @throws Nothing.
 */
PS_POLICY_PLUGIN_EXPORT uint32_t PS_POLICY_CALL
ps_policy_fixture_create_count(void);

/**
 * @brief Returns the number of fixture select callback entries.
 * @return Exact count since the last reset.
 * @throws Nothing.
 */
PS_POLICY_PLUGIN_EXPORT uint32_t PS_POLICY_CALL
ps_policy_fixture_select_count(void);

/**
 * @brief Returns the number of fixture destroy callback entries.
 * @return Exact count since the last reset.
 * @throws Nothing.
 */
PS_POLICY_PLUGIN_EXPORT uint32_t PS_POLICY_CALL
ps_policy_fixture_destroy_count(void);

#if defined(__cplusplus)
}  // extern "C"
#endif

#endif  // TESTS_FIXTURES_POLICY_PLUGINS_POLICY_FIXTURE_CONTROL_H_
