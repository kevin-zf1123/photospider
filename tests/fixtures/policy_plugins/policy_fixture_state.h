#ifndef TESTS_FIXTURES_POLICY_PLUGINS_POLICY_FIXTURE_STATE_H_
#define TESTS_FIXTURES_POLICY_PLUGINS_POLICY_FIXTURE_STATE_H_

#include <stdatomic.h>
#include <stdint.h>

#include "policy_fixture_control.h"  // NOLINT(build/include_subdir)

/** @brief Process-shared API behavior across every mapping of one fixture. */
extern _Atomic uint32_t g_policy_fixture_api_mode;

/** @brief Process-shared metadata behavior across fixture mappings. */
extern _Atomic uint32_t g_policy_fixture_metadata_mode;

/** @brief Process-shared context-create behavior across fixture mappings. */
extern _Atomic uint32_t g_policy_fixture_create_mode;

/** @brief Process-shared candidate-selection behavior across fixture mappings.
 */
extern _Atomic uint32_t g_policy_fixture_select_mode;

/** @brief Process-shared context-destroy behavior across fixture mappings. */
extern _Atomic uint32_t g_policy_fixture_destroy_mode;

/** @brief Process-shared create callback count since reset. */
extern _Atomic uint32_t g_policy_fixture_create_count;

/** @brief Process-shared select callback count since reset. */
extern _Atomic uint32_t g_policy_fixture_select_count;

/** @brief Process-shared destroy callback count since reset. */
extern _Atomic uint32_t g_policy_fixture_destroy_count;

/** @brief Test-owned hook shared across fixture mappings. */
extern ps_policy_fixture_hook_v1 g_policy_fixture_hook;

/** @brief Opaque hook context shared across fixture mappings. */
extern void* g_policy_fixture_hook_context;

/** @brief Stable nonnull logical context shared across fixture mappings. */
extern uint32_t g_policy_fixture_nonnull_context;

#endif  // TESTS_FIXTURES_POLICY_PLUGINS_POLICY_FIXTURE_STATE_H_
