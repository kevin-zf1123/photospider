#include <stddef.h>

#include "photospider/policy/policy_plugin_api.h"

/**
 * @file fifo_policy.c
 * @brief Minimal pure-C FIFO ranking policy example for ABI v1.
 *
 * The plugin selects the first candidate in the Host-authored immutable
 * snapshot. It creates no executor, worker, queue, resource grant, or mutable
 * process owner and retains no borrowed snapshot memory.
 */

/**
 * @brief Builds a borrowed ABI string view for static storage.
 * @param data Static string bytes.
 * @param size Byte count excluding the terminator.
 * @return Borrowed immutable view valid for the DSO lifetime.
 * @note The helper allocates nothing and accepts only caller-supplied static
 * storage in this plugin.
 */
static ps_policy_string_view_v1 fifo_string_view(const char* data,
                                                 uint64_t size) {
  ps_policy_string_view_v1 view;
  view.data = data;
  view.size = size;
  return view;
}

/**
 * @brief Returns metadata for the single `fifo` policy type.
 * @param type_index Zero-based row; only zero is supported.
 * @param out_metadata Nonnull exact ABI-v1 output record.
 * @return OK on success, otherwise INVALID_ARGUMENT.
 * @note Returned string views refer only to immutable DSO storage.
 */
static ps_policy_status_v1 PS_POLICY_CALL fifo_get_metadata(
    uint32_t type_index, ps_policy_type_metadata_v1* out_metadata) {
  static const char kName[] = "fifo";
  static const char kDescription[] =
      "Selects the first candidate in the Host-authored snapshot.";
  static const char kVersion[] = "1.0.0";
  if (type_index != 0U || out_metadata == NULL) {
    return PS_POLICY_STATUS_INVALID_ARGUMENT;
  }
  *out_metadata = (ps_policy_type_metadata_v1){0};
  out_metadata->struct_size = sizeof(*out_metadata);
  out_metadata->struct_kind = PS_POLICY_STRUCT_TYPE_METADATA;
  out_metadata->name = fifo_string_view(kName, sizeof(kName) - 1U);
  out_metadata->description =
      fifo_string_view(kDescription, sizeof(kDescription) - 1U);
  out_metadata->implementation_version =
      fifo_string_view(kVersion, sizeof(kVersion) - 1U);
  out_metadata->supported_class_mask = PS_POLICY_CLASS_MASK_VALID;
  return PS_POLICY_STATUS_OK;
}

/**
 * @brief Creates one stateless class-specific FIFO context.
 * @param type_index Zero-based row; only zero is supported.
 * @param args Nonnull exact Host-authored creation arguments.
 * @param out_context Nonnull output initialized to a stateless null context.
 * @return OK for a valid class/generation, otherwise INVALID_ARGUMENT.
 * @note A successful null context still receives exactly one destroy call.
 */
static ps_policy_status_v1 PS_POLICY_CALL
fifo_create(uint32_t type_index, const ps_policy_create_args_v1* args,
            void** out_context) {
  if (type_index != 0U || args == NULL || out_context == NULL ||
      args->struct_size != sizeof(*args) ||
      args->struct_kind != PS_POLICY_STRUCT_CREATE_ARGS ||
      (args->policy_class != PS_POLICY_CLASS_INTERACTIVE &&
       args->policy_class != PS_POLICY_CLASS_THROUGHPUT) ||
      args->binding_generation == 0U) {
    return PS_POLICY_STATUS_INVALID_ARGUMENT;
  }
  *out_context = NULL;
  return PS_POLICY_STATUS_OK;
}

/**
 * @brief Selects the first candidate in one immutable snapshot.
 * @param context Stateless context; ignored and never retained.
 * @param snapshot Nonnull exact Host-authored candidate snapshot.
 * @param out_decision Nonnull exact decision record to populate.
 * @return OK on selection, otherwise INVALID_ARGUMENT.
 * @note No snapshot pointer or candidate record survives this call.
 */
static ps_policy_status_v1 PS_POLICY_CALL
fifo_select(void* context, const ps_policy_selection_snapshot_v1* snapshot,
            ps_policy_decision_v1* out_decision) {
  (void)context;
  if (snapshot == NULL || out_decision == NULL ||
      snapshot->struct_size != sizeof(*snapshot) ||
      snapshot->struct_kind != PS_POLICY_STRUCT_SELECTION_SNAPSHOT ||
      snapshot->candidate_count == 0U || snapshot->candidates == NULL ||
      snapshot->binding_generation == 0U ||
      snapshot->snapshot_generation == 0U) {
    return PS_POLICY_STATUS_INVALID_ARGUMENT;
  }
  *out_decision = (ps_policy_decision_v1){0};
  out_decision->struct_size = sizeof(*out_decision);
  out_decision->struct_kind = PS_POLICY_STRUCT_DECISION;
  out_decision->decision_kind = PS_POLICY_DECISION_SELECT;
  out_decision->binding_generation = snapshot->binding_generation;
  out_decision->snapshot_generation = snapshot->snapshot_generation;
  out_decision->candidate_id = snapshot->candidates[0].candidate_id;
  return PS_POLICY_STATUS_OK;
}

/**
 * @brief Completes one successful stateless context lifetime.
 * @param context Stateless null context supplied by the Host.
 * @return Always OK.
 * @note The callback owns no allocation and is safe for a null context.
 */
static ps_policy_status_v1 PS_POLICY_CALL fifo_destroy(void* context) {
  (void)context;
  return PS_POLICY_STATUS_OK;
}

/** @copydoc ps_policy_plugin_get_abi_version */
PS_POLICY_PLUGIN_EXPORT uint32_t PS_POLICY_CALL
ps_policy_plugin_get_abi_version(void) {
  return PS_POLICY_PLUGIN_ABI_VERSION;
}

/** @copydoc ps_policy_plugin_get_api_v1 */
PS_POLICY_PLUGIN_EXPORT ps_policy_status_v1 PS_POLICY_CALL
ps_policy_plugin_get_api_v1(ps_policy_plugin_api_v1* out_api) {
  if (out_api == NULL) {
    return PS_POLICY_STATUS_INVALID_ARGUMENT;
  }
  *out_api = (ps_policy_plugin_api_v1){0};
  out_api->struct_size = sizeof(*out_api);
  out_api->struct_kind = PS_POLICY_STRUCT_PLUGIN_API;
  out_api->abi_version = PS_POLICY_PLUGIN_ABI_VERSION;
  out_api->type_count = 1U;
  out_api->get_metadata = fifo_get_metadata;
  out_api->create = fifo_create;
  out_api->select = fifo_select;
  out_api->destroy = fifo_destroy;
  return PS_POLICY_STATUS_OK;
}
