/**
 * @file policy_fixture_state.c
 * @brief Defines process-shared control state for policy fixture DSO aliases.
 *
 * Production trust deliberately maps an authorized DSO through its retained
 * file descriptor. Tests may also map the original path to call control
 * exports. Both DSO instances link this one test-only state library, preserving
 * deterministic control without weakening exact-object production loading.
 */

#include "policy_fixture_state.h"  // NOLINT(build/include_subdir)

#include <stddef.h>

/** @copydoc g_policy_fixture_api_mode */
_Atomic(uint32_t) g_policy_fixture_api_mode = PS_POLICY_FIXTURE_API_VALID;

/** @copydoc g_policy_fixture_metadata_mode */
_Atomic(uint32_t) g_policy_fixture_metadata_mode =
    PS_POLICY_FIXTURE_METADATA_VALID;

/** @copydoc g_policy_fixture_create_mode */
_Atomic(uint32_t) g_policy_fixture_create_mode =
    PS_POLICY_FIXTURE_CREATE_SUCCESS_NULL;

/** @copydoc g_policy_fixture_select_mode */
_Atomic(uint32_t) g_policy_fixture_select_mode = PS_POLICY_FIXTURE_SELECT_LAST;

/** @copydoc g_policy_fixture_destroy_mode */
_Atomic(uint32_t) g_policy_fixture_destroy_mode = PS_POLICY_FIXTURE_DESTROY_OK;

/** @copydoc g_policy_fixture_create_count */
_Atomic(uint32_t) g_policy_fixture_create_count = 0U;

/** @copydoc g_policy_fixture_select_count */
_Atomic(uint32_t) g_policy_fixture_select_count = 0U;

/** @copydoc g_policy_fixture_destroy_count */
_Atomic(uint32_t) g_policy_fixture_destroy_count = 0U;

/** @copydoc g_policy_fixture_hook */
ps_policy_fixture_hook_v1 g_policy_fixture_hook = NULL;

/** @copydoc g_policy_fixture_hook_context */
void* g_policy_fixture_hook_context = NULL;

/** @copydoc g_policy_fixture_nonnull_context */
uint32_t g_policy_fixture_nonnull_context = UINT32_C(0x75);
