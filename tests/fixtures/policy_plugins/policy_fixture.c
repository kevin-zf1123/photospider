#include <stdatomic.h>
#include <stddef.h>
#include <stdint.h>

#include "photospider/policy/policy_plugin_api.h"
#include "policy_fixture_control.h"  // NOLINT(build/include_subdir)

/**
 * @file policy_fixture.c
 * @brief Controllable pure-C ABI-v1 policy DSO for long-lived contract tests.
 *
 * The default fixture publishes one `fixture_policy` type supporting both
 * classes and selects the last candidate. Test-only control exports select
 * malformed/status/lifecycle/blocking behavior without changing the production
 * policy ABI. Compile definitions can still expose an ABI mismatch or omit the
 * API-table export so version-first symbol ordering remains independently
 * testable.
 */

#ifndef PHOTOSPIDER_POLICY_FIXTURE_ABI_VERSION
#define PHOTOSPIDER_POLICY_FIXTURE_ABI_VERSION PS_POLICY_PLUGIN_ABI_VERSION
#endif

/** @brief Suppresses helper warnings in the intentional missing-API fixture. */
#if defined(__GNUC__) || defined(__clang__)
#define PS_POLICY_FIXTURE_MAYBE_UNUSED __attribute__((unused))
#else
#define PS_POLICY_FIXTURE_MAYBE_UNUSED
#endif

/** @brief Current API-table behavior. */
static _Atomic(uint32_t) g_api_mode = PS_POLICY_FIXTURE_API_VALID;

/** @brief Current metadata behavior. */
static _Atomic(uint32_t) g_metadata_mode = PS_POLICY_FIXTURE_METADATA_VALID;

/** @brief Current context-create behavior. */
static _Atomic(uint32_t) g_create_mode = PS_POLICY_FIXTURE_CREATE_SUCCESS_NULL;

/** @brief Current candidate-selection behavior. */
static _Atomic(uint32_t) g_select_mode = PS_POLICY_FIXTURE_SELECT_LAST;

/** @brief Current context-destroy behavior. */
static _Atomic(uint32_t) g_destroy_mode = PS_POLICY_FIXTURE_DESTROY_OK;

/** @brief Number of create callback entries since reset. */
static _Atomic(uint32_t) g_create_count = 0U;

/** @brief Number of select callback entries since reset. */
static _Atomic(uint32_t) g_select_count = 0U;

/** @brief Number of destroy callback entries since reset. */
static _Atomic(uint32_t) g_destroy_count = 0U;

/** @brief Optional test-owned callback installed before controlled entry. */
static ps_policy_fixture_hook_v1 g_hook = NULL;

/** @brief Opaque context passed unchanged to the installed hook. */
static void* g_hook_context = NULL;

/** @brief Stable nonnull logical context used by controlled create modes. */
static uint32_t g_nonnull_context = UINT32_C(0x75);

/**
 * @brief Builds a borrowed string view over immutable fixture storage.
 * @param data Static bytes.
 * @param size Byte count excluding any terminator.
 * @return Borrowed view valid while the fixture DSO remains mapped.
 */
static PS_POLICY_FIXTURE_MAYBE_UNUSED ps_policy_string_view_v1
fixture_string_view(const char* data, uint64_t size) {
  ps_policy_string_view_v1 view;
  view.data = data;
  view.size = size;
  return view;
}

/**
 * @brief Invokes the installed test hook at one controlled boundary.
 * @param event Exact callback event.
 * @return Nothing.
 * @note Tests install/reset hooks only outside concurrent callback access.
 */
static PS_POLICY_FIXTURE_MAYBE_UNUSED void fixture_invoke_hook(
    ps_policy_fixture_hook_event event) {
  ps_policy_fixture_hook_v1 hook = g_hook;
  if (hook != NULL) {
    (void)hook(g_hook_context, event);
  }
}

/**
 * @brief Maps one controlled setup mode to a non-OK ABI status.
 * @param mode Domain-specific mode whose status values occupy one through five.
 * @return Selected ABI status, or OK for a structural/output mode.
 */
static PS_POLICY_FIXTURE_MAYBE_UNUSED ps_policy_status_v1
fixture_setup_status(uint32_t mode) {
  switch (mode) {
    case 1U:
      return PS_POLICY_STATUS_INVALID_ARGUMENT;
    case 2U:
      return PS_POLICY_STATUS_UNSUPPORTED;
    case 3U:
      return PS_POLICY_STATUS_OUT_OF_MEMORY;
    case 4U:
      return PS_POLICY_STATUS_INTERNAL_ERROR;
    case 5U:
      return UINT32_C(0xFFFFFFFF);
    default:
      return PS_POLICY_STATUS_OK;
  }
}

/**
 * @brief Returns one controlled metadata row.
 * @param type_index Zero-based row selected by the Host.
 * @param out_metadata Nonnull preinitialized exact ABI-v1 output record.
 * @return Controlled status or OK with controlled output bytes.
 * @note All ordinary returned strings are immutable DSO-lifetime values.
 */
static PS_POLICY_FIXTURE_MAYBE_UNUSED ps_policy_status_v1 PS_POLICY_CALL
fixture_get_metadata(uint32_t type_index,
                     ps_policy_type_metadata_v1* out_metadata) {
  static const char kName[] = "fixture_policy";
  static const char kSecondName[] = "fixture_policy_two";
  static const char kNoncanonicalName[] = "FixturePolicy";
  static const char kReservedName[] = "interactive";
  static const char kDescription[] = "Deterministic policy test fixture.";
  static const char kVersion[] = "test-v1";
  static const char kInvalidUtf8[] = {(char)0xC0, (char)0xAF};
  static const char kOversizedDescription[4097] = {'x'};
  const uint32_t mode =
      atomic_load_explicit(&g_metadata_mode, memory_order_acquire);
  const ps_policy_status_v1 controlled_status = fixture_setup_status(mode);
  if (controlled_status != PS_POLICY_STATUS_OK) {
    return controlled_status;
  }
  if (out_metadata == NULL || type_index > 1U) {
    return PS_POLICY_STATUS_INVALID_ARGUMENT;
  }

  *out_metadata = (ps_policy_type_metadata_v1){0};
  out_metadata->struct_size = sizeof(*out_metadata);
  out_metadata->struct_kind = PS_POLICY_STRUCT_TYPE_METADATA;
  out_metadata->name = fixture_string_view(
      type_index == 0U ? kName : kSecondName,
      type_index == 0U ? sizeof(kName) - 1U : sizeof(kSecondName) - 1U);
  out_metadata->description =
      fixture_string_view(kDescription, sizeof(kDescription) - 1U);
  out_metadata->implementation_version =
      fixture_string_view(kVersion, sizeof(kVersion) - 1U);
  out_metadata->supported_class_mask = PS_POLICY_CLASS_MASK_VALID;

  switch (mode) {
    case PS_POLICY_FIXTURE_METADATA_SIZE_UNDER:
      out_metadata->struct_size = sizeof(*out_metadata) - 1U;
      break;
    case PS_POLICY_FIXTURE_METADATA_SIZE_OVER:
      out_metadata->struct_size = sizeof(*out_metadata) + 8U;
      break;
    case PS_POLICY_FIXTURE_METADATA_KIND_INVALID:
      out_metadata->struct_kind = UINT32_C(0xFFFFFFFF);
      break;
    case PS_POLICY_FIXTURE_METADATA_RESERVED0_NONZERO:
      out_metadata->reserved0 = 1U;
      break;
    case PS_POLICY_FIXTURE_METADATA_RESERVED_NONZERO:
      out_metadata->reserved[0] = 1U;
      break;
    case PS_POLICY_FIXTURE_METADATA_MASK_ZERO:
      out_metadata->supported_class_mask = 0U;
      break;
    case PS_POLICY_FIXTURE_METADATA_MASK_UNKNOWN:
      out_metadata->supported_class_mask = UINT32_C(0x4);
      break;
    case PS_POLICY_FIXTURE_METADATA_NAME_NULL:
      out_metadata->name = fixture_string_view(NULL, 1U);
      break;
    case PS_POLICY_FIXTURE_METADATA_NAME_NONCANONICAL:
      out_metadata->name = fixture_string_view(kNoncanonicalName,
                                               sizeof(kNoncanonicalName) - 1U);
      break;
    case PS_POLICY_FIXTURE_METADATA_NAME_RESERVED:
      out_metadata->name =
          fixture_string_view(kReservedName, sizeof(kReservedName) - 1U);
      break;
    case PS_POLICY_FIXTURE_METADATA_DESCRIPTION_INVALID_UTF8:
      out_metadata->description =
          fixture_string_view(kInvalidUtf8, sizeof(kInvalidUtf8));
      break;
    case PS_POLICY_FIXTURE_METADATA_DESCRIPTION_TOO_LONG:
      out_metadata->description = fixture_string_view(
          kOversizedDescription, sizeof(kOversizedDescription));
      break;
    case PS_POLICY_FIXTURE_METADATA_DUPLICATE_NAMES:
      out_metadata->name = fixture_string_view(kName, sizeof(kName) - 1U);
      break;
    default:
      break;
  }
  return PS_POLICY_STATUS_OK;
}

/**
 * @brief Creates one controlled fixture context after exact argument checks.
 * @param type_index Zero-based row; only zero is bindable in valid mode.
 * @param args Nonnull exact creation arguments.
 * @param out_context Nonnull context output preinitialized to null by Host.
 * @return Controlled status or OK.
 * @note A failed create that writes a nonnull value deliberately violates the
 * ABI so tests can prove Host rejection and the rule that destroy is never
 * invoked for a failed create. Successful null and nonnull values both create
 * one logical instance that the Host must destroy exactly once.
 */
static PS_POLICY_FIXTURE_MAYBE_UNUSED ps_policy_status_v1 PS_POLICY_CALL
fixture_create(uint32_t type_index, const ps_policy_create_args_v1* args,
               void** out_context) {
  const uint32_t mode =
      atomic_load_explicit(&g_create_mode, memory_order_acquire);
  (void)atomic_fetch_add_explicit(&g_create_count, 1U, memory_order_relaxed);
  if (type_index != 0U || args == NULL || out_context == NULL ||
      args->struct_size != sizeof(*args) ||
      args->struct_kind != PS_POLICY_STRUCT_CREATE_ARGS ||
      args->binding_generation == 0U || args->reserved0 != 0U ||
      args->reserved[0] != 0U || args->reserved[1] != 0U ||
      (args->policy_class != PS_POLICY_CLASS_INTERACTIVE &&
       args->policy_class != PS_POLICY_CLASS_THROUGHPUT)) {
    return PS_POLICY_STATUS_INVALID_ARGUMENT;
  }
  if (mode == PS_POLICY_FIXTURE_CREATE_HOOK_SUCCESS) {
    fixture_invoke_hook(PS_POLICY_FIXTURE_HOOK_CREATE);
  }
  switch (mode) {
    case PS_POLICY_FIXTURE_CREATE_SUCCESS_NONNULL:
      *out_context = &g_nonnull_context;
      return PS_POLICY_STATUS_OK;
    case PS_POLICY_FIXTURE_CREATE_STATUS_INVALID_NULL:
      return PS_POLICY_STATUS_INVALID_ARGUMENT;
    case PS_POLICY_FIXTURE_CREATE_STATUS_INVALID_NONNULL:
      *out_context = &g_nonnull_context;
      return PS_POLICY_STATUS_INVALID_ARGUMENT;
    case PS_POLICY_FIXTURE_CREATE_STATUS_UNSUPPORTED:
      return PS_POLICY_STATUS_UNSUPPORTED;
    case PS_POLICY_FIXTURE_CREATE_STATUS_OUT_OF_MEMORY:
      return PS_POLICY_STATUS_OUT_OF_MEMORY;
    case PS_POLICY_FIXTURE_CREATE_STATUS_INTERNAL:
      return PS_POLICY_STATUS_INTERNAL_ERROR;
    case PS_POLICY_FIXTURE_CREATE_STATUS_UNKNOWN:
      return UINT32_C(0xFFFFFFFF);
    default:
      *out_context = NULL;
      return PS_POLICY_STATUS_OK;
  }
}

/**
 * @brief Selects one controlled decision from an immutable snapshot.
 * @param context Stateless or stable fixture context; otherwise ignored.
 * @param snapshot Nonnull exact snapshot with at least one candidate.
 * @param out_decision Nonnull preinitialized exact output record.
 * @return Controlled status or OK with controlled decision bytes.
 * @note The callback retains no Host pointer. The nonreturning mode is used
 * only in a child process that a parent watchdog terminates.
 */
static PS_POLICY_FIXTURE_MAYBE_UNUSED ps_policy_status_v1 PS_POLICY_CALL
fixture_select(void* context, const ps_policy_selection_snapshot_v1* snapshot,
               ps_policy_decision_v1* out_decision) {
  const uint32_t mode =
      atomic_load_explicit(&g_select_mode, memory_order_acquire);
  (void)context;
  (void)atomic_fetch_add_explicit(&g_select_count, 1U, memory_order_relaxed);
  if (snapshot == NULL || out_decision == NULL ||
      snapshot->struct_size != sizeof(*snapshot) ||
      snapshot->struct_kind != PS_POLICY_STRUCT_SELECTION_SNAPSHOT ||
      snapshot->candidate_count == 0U || snapshot->candidates == NULL ||
      snapshot->candidate_stride != sizeof(ps_policy_candidate_v1) ||
      snapshot->binding_generation == 0U ||
      snapshot->snapshot_generation == 0U) {
    return PS_POLICY_STATUS_INVALID_ARGUMENT;
  }
  if (mode == PS_POLICY_FIXTURE_SELECT_NONRETURNING) {
    for (;;) {
    }
  }
  if (mode == PS_POLICY_FIXTURE_SELECT_HOOK_LAST) {
    fixture_invoke_hook(PS_POLICY_FIXTURE_HOOK_SELECT);
  }
  if (mode == PS_POLICY_FIXTURE_SELECT_STATUS_INVALID) {
    return PS_POLICY_STATUS_INVALID_ARGUMENT;
  }
  if (mode == PS_POLICY_FIXTURE_SELECT_STATUS_OUT_OF_MEMORY) {
    return PS_POLICY_STATUS_OUT_OF_MEMORY;
  }
  if (mode == PS_POLICY_FIXTURE_SELECT_STATUS_UNKNOWN) {
    return UINT32_C(0xFFFFFFFF);
  }

  *out_decision = (ps_policy_decision_v1){0};
  out_decision->struct_size = sizeof(*out_decision);
  out_decision->struct_kind = PS_POLICY_STRUCT_DECISION;
  out_decision->decision_kind = PS_POLICY_DECISION_SELECT;
  out_decision->binding_generation = snapshot->binding_generation;
  out_decision->snapshot_generation = snapshot->snapshot_generation;
  out_decision->candidate_id =
      snapshot->candidates[snapshot->candidate_count - 1U].candidate_id;

  switch (mode) {
    case PS_POLICY_FIXTURE_SELECT_FIRST:
      out_decision->candidate_id = snapshot->candidates[0].candidate_id;
      break;
    case PS_POLICY_FIXTURE_SELECT_ABSTAIN:
      out_decision->decision_kind = PS_POLICY_DECISION_ABSTAIN;
      out_decision->candidate_id = 0U;
      break;
    case PS_POLICY_FIXTURE_SELECT_SIZE_UNDER:
      out_decision->struct_size = sizeof(*out_decision) - 1U;
      break;
    case PS_POLICY_FIXTURE_SELECT_SIZE_OVER:
      out_decision->struct_size = sizeof(*out_decision) + 8U;
      break;
    case PS_POLICY_FIXTURE_SELECT_KIND_INVALID:
      out_decision->struct_kind = UINT32_C(0xFFFFFFFF);
      break;
    case PS_POLICY_FIXTURE_SELECT_RESERVED0_NONZERO:
      out_decision->reserved0 = 1U;
      break;
    case PS_POLICY_FIXTURE_SELECT_RESERVED_NONZERO:
      out_decision->reserved[0] = 1U;
      break;
    case PS_POLICY_FIXTURE_SELECT_DECISION_UNKNOWN:
      out_decision->decision_kind = UINT32_C(0xFFFFFFFF);
      break;
    case PS_POLICY_FIXTURE_SELECT_BINDING_GENERATION_MISMATCH:
      ++out_decision->binding_generation;
      break;
    case PS_POLICY_FIXTURE_SELECT_SNAPSHOT_GENERATION_MISMATCH:
      ++out_decision->snapshot_generation;
      break;
    case PS_POLICY_FIXTURE_SELECT_CANDIDATE_ZERO:
      out_decision->candidate_id = 0U;
      break;
    case PS_POLICY_FIXTURE_SELECT_CANDIDATE_OUTSIDE:
      out_decision->candidate_id = UINT64_MAX;
      break;
    case PS_POLICY_FIXTURE_SELECT_ABSTAIN_NONNULL:
      out_decision->decision_kind = PS_POLICY_DECISION_ABSTAIN;
      break;
    default:
      break;
  }
  return PS_POLICY_STATUS_OK;
}

/**
 * @brief Completes one controlled fixture context lifetime.
 * @param context Stateless or stable fixture context; otherwise ignored.
 * @return Controlled diagnostic status.
 * @note The callback is counted exactly once per successful logical create,
 * including a successful null context.
 */
static PS_POLICY_FIXTURE_MAYBE_UNUSED ps_policy_status_v1 PS_POLICY_CALL
fixture_destroy(void* context) {
  const uint32_t mode =
      atomic_load_explicit(&g_destroy_mode, memory_order_acquire);
  (void)context;
  (void)atomic_fetch_add_explicit(&g_destroy_count, 1U, memory_order_relaxed);
  if (mode == PS_POLICY_FIXTURE_DESTROY_HOOK) {
    fixture_invoke_hook(PS_POLICY_FIXTURE_HOOK_DESTROY);
  }
  return mode == PS_POLICY_FIXTURE_DESTROY_STATUS_INTERNAL
             ? PS_POLICY_STATUS_INTERNAL_ERROR
             : PS_POLICY_STATUS_OK;
}

/** @copydoc ps_policy_fixture_reset */
PS_POLICY_PLUGIN_EXPORT void PS_POLICY_CALL ps_policy_fixture_reset(void) {
  atomic_store_explicit(&g_api_mode, PS_POLICY_FIXTURE_API_VALID,
                        memory_order_release);
  atomic_store_explicit(&g_metadata_mode, PS_POLICY_FIXTURE_METADATA_VALID,
                        memory_order_release);
  atomic_store_explicit(&g_create_mode, PS_POLICY_FIXTURE_CREATE_SUCCESS_NULL,
                        memory_order_release);
  atomic_store_explicit(&g_select_mode, PS_POLICY_FIXTURE_SELECT_LAST,
                        memory_order_release);
  atomic_store_explicit(&g_destroy_mode, PS_POLICY_FIXTURE_DESTROY_OK,
                        memory_order_release);
  atomic_store_explicit(&g_create_count, 0U, memory_order_relaxed);
  atomic_store_explicit(&g_select_count, 0U, memory_order_relaxed);
  atomic_store_explicit(&g_destroy_count, 0U, memory_order_relaxed);
  g_hook = NULL;
  g_hook_context = NULL;
}

/** @copydoc ps_policy_fixture_set_api_mode */
PS_POLICY_PLUGIN_EXPORT void PS_POLICY_CALL
ps_policy_fixture_set_api_mode(ps_policy_fixture_api_mode mode) {
  atomic_store_explicit(&g_api_mode, mode, memory_order_release);
}

/** @copydoc ps_policy_fixture_set_metadata_mode */
PS_POLICY_PLUGIN_EXPORT void PS_POLICY_CALL
ps_policy_fixture_set_metadata_mode(ps_policy_fixture_metadata_mode mode) {
  atomic_store_explicit(&g_metadata_mode, mode, memory_order_release);
}

/** @copydoc ps_policy_fixture_set_create_mode */
PS_POLICY_PLUGIN_EXPORT void PS_POLICY_CALL
ps_policy_fixture_set_create_mode(ps_policy_fixture_create_mode mode) {
  atomic_store_explicit(&g_create_mode, mode, memory_order_release);
}

/** @copydoc ps_policy_fixture_set_select_mode */
PS_POLICY_PLUGIN_EXPORT void PS_POLICY_CALL
ps_policy_fixture_set_select_mode(ps_policy_fixture_select_mode mode) {
  atomic_store_explicit(&g_select_mode, mode, memory_order_release);
}

/** @copydoc ps_policy_fixture_set_destroy_mode */
PS_POLICY_PLUGIN_EXPORT void PS_POLICY_CALL
ps_policy_fixture_set_destroy_mode(ps_policy_fixture_destroy_mode mode) {
  atomic_store_explicit(&g_destroy_mode, mode, memory_order_release);
}

/** @copydoc ps_policy_fixture_set_hook */
PS_POLICY_PLUGIN_EXPORT void PS_POLICY_CALL
ps_policy_fixture_set_hook(ps_policy_fixture_hook_v1 hook, void* context) {
  g_hook_context = context;
  g_hook = hook;
}

/** @copydoc ps_policy_fixture_create_count */
PS_POLICY_PLUGIN_EXPORT uint32_t PS_POLICY_CALL
ps_policy_fixture_create_count(void) {
  return atomic_load_explicit(&g_create_count, memory_order_relaxed);
}

/** @copydoc ps_policy_fixture_select_count */
PS_POLICY_PLUGIN_EXPORT uint32_t PS_POLICY_CALL
ps_policy_fixture_select_count(void) {
  return atomic_load_explicit(&g_select_count, memory_order_relaxed);
}

/** @copydoc ps_policy_fixture_destroy_count */
PS_POLICY_PLUGIN_EXPORT uint32_t PS_POLICY_CALL
ps_policy_fixture_destroy_count(void) {
  return atomic_load_explicit(&g_destroy_count, memory_order_relaxed);
}

/** @copydoc ps_policy_plugin_get_abi_version */
PS_POLICY_PLUGIN_EXPORT uint32_t PS_POLICY_CALL
ps_policy_plugin_get_abi_version(void) {
  return PHOTOSPIDER_POLICY_FIXTURE_ABI_VERSION;
}

#if !defined(PHOTOSPIDER_POLICY_FIXTURE_OMIT_API)
/** @copydoc ps_policy_plugin_get_api_v1 */
PS_POLICY_PLUGIN_EXPORT ps_policy_status_v1 PS_POLICY_CALL
ps_policy_plugin_get_api_v1(ps_policy_plugin_api_v1* out_api) {
  const uint32_t mode = atomic_load_explicit(&g_api_mode, memory_order_acquire);
  const ps_policy_status_v1 controlled_status = fixture_setup_status(mode);
  if (controlled_status != PS_POLICY_STATUS_OK) {
    return controlled_status;
  }
  if (out_api == NULL) {
    return PS_POLICY_STATUS_INVALID_ARGUMENT;
  }
  *out_api = (ps_policy_plugin_api_v1){0};
  out_api->struct_size = sizeof(*out_api);
  out_api->struct_kind = PS_POLICY_STRUCT_PLUGIN_API;
  out_api->abi_version = PS_POLICY_PLUGIN_ABI_VERSION;
  out_api->type_count = mode == PS_POLICY_FIXTURE_API_TWO_TYPES ? 2U : 1U;
  out_api->get_metadata = fixture_get_metadata;
  out_api->create = fixture_create;
  out_api->select = fixture_select;
  out_api->destroy = fixture_destroy;
  switch (mode) {
    case PS_POLICY_FIXTURE_API_SIZE_UNDER:
      out_api->struct_size = sizeof(*out_api) - 1U;
      break;
    case PS_POLICY_FIXTURE_API_SIZE_OVER:
      out_api->struct_size = sizeof(*out_api) + 8U;
      break;
    case PS_POLICY_FIXTURE_API_KIND_INVALID:
      out_api->struct_kind = UINT32_C(0xFFFFFFFF);
      break;
    case PS_POLICY_FIXTURE_API_ABI_INVALID:
      out_api->abi_version = PS_POLICY_PLUGIN_ABI_VERSION + 1U;
      break;
    case PS_POLICY_FIXTURE_API_TYPE_COUNT_ZERO:
      out_api->type_count = 0U;
      break;
    case PS_POLICY_FIXTURE_API_TYPE_COUNT_OVER:
      out_api->type_count = 257U;
      break;
    case PS_POLICY_FIXTURE_API_CALLBACK_NULL:
      out_api->select = NULL;
      break;
    case PS_POLICY_FIXTURE_API_RESERVED_NONZERO:
      out_api->reserved[0] = 1U;
      break;
    default:
      break;
  }
  return PS_POLICY_STATUS_OK;
}
#endif

#undef PS_POLICY_FIXTURE_MAYBE_UNUSED
