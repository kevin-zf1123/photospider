#include <cstdio>
#include <cstdlib>

#include "photospider/plugin/operation_plugin.hpp"

namespace ps::operation_plugin {
namespace {

/** @brief Environment variable selecting the shared lifecycle trace file. */
constexpr const char* kTraceEnvironment = "PS_LIFECYCLE_PLUGIN_TRACE";
/** @brief Permanent replacement plugin identity. */
constexpr auto kPluginIdentity =
    make_identity(0x50534F5645525244ULL, 0x0001ULL);
/** @brief Exact operation identity shared with the predecessor generation. */
constexpr auto kOperationIdentity =
    make_identity(0x50534C4946454F50ULL, 0x0001ULL);
/** @brief Permanent replacement implementation identity. */
constexpr auto kImplementationIdentity =
    make_identity(0x50534F564552494DULL, 0x0001ULL);
/** @brief Shared operation configuration-schema identity. */
constexpr auto kConfigurationIdentity =
    make_identity(0x50534C4946454346ULL, 0x0001ULL);

/**
 * @brief Appends one replacement-generation lifecycle trace event.
 * @param event Stable event label.
 * @return Nothing.
 * @throws Nothing; missing configuration and I/O failures are ignored.
 */
void append_lifecycle_trace(const char* event) noexcept {
  const char* path = std::getenv(kTraceEnvironment);
  if (path == nullptr || path[0] == '\0') {
    return;
  }
  std::FILE* output = std::fopen(path, "a");
  if (output == nullptr) {
    return;
  }
  (void)std::fputs(event, output);
  (void)std::fputc('\n', output);
  (void)std::fclose(output);
}

/** @brief Static probe corresponding to replacement DSO retirement. */
struct OverrideLibraryLifetimeProbe final {
  /** @brief Traces final native-library unmapping. */
  ~OverrideLibraryLifetimeProbe() {
    append_lifecycle_trace("override_library_unload");
  }
};

/** @brief Process-per-load replacement library probe. */
OverrideLibraryLifetimeProbe override_library_lifetime_probe;

/**
 * @brief Accepts the replacement operation's empty output plan.
 * @param invocation Exact callback invocation.
 * @param inputs Exact empty input array.
 * @return Stable ABI status.
 * @throws Nothing.
 */
ps_operation_status_v1 PS_OPERATION_CALL
infer_override(void*, const ps_operation_invocation_v1* invocation,
               const ps_operation_configuration_view_v1*,
               const ps_operation_array_ref_v1* inputs,
               const ps_operation_output_sink_v1*) noexcept {
  return invocation != nullptr && inputs != nullptr && inputs->count == 0U
             ? PS_OPERATION_STATUS_OK_V1
             : PS_OPERATION_STATUS_INVALID_DESCRIPTOR_V1;
}

/**
 * @brief Executes the replacement operation without producing a value.
 * @param invocation Exact callback invocation.
 * @param inputs Exact empty input array.
 * @param outputs Exact empty output array.
 * @return Stable ABI status.
 * @throws Nothing.
 */
ps_operation_status_v1 PS_OPERATION_CALL
execute_override(void*, const ps_operation_invocation_v1* invocation,
                 const ps_operation_configuration_view_v1*,
                 const ps_operation_array_ref_v1* inputs,
                 const ps_operation_array_ref_v1* outputs,
                 const ps_operation_output_sink_v1*) noexcept {
  return invocation != nullptr && inputs != nullptr && outputs != nullptr &&
                 inputs->count == 0U && outputs->count == 0U
             ? PS_OPERATION_STATUS_OK_V1
             : PS_OPERATION_STATUS_INVALID_DESCRIPTOR_V1;
}

/**
 * @brief Observes exactly-once replacement-generation destruction.
 * @return Stable ABI status.
 * @throws Nothing.
 */
ps_operation_status_v1 PS_OPERATION_CALL destroy_override_generation(
    void*, const ps_operation_output_sink_v1*) noexcept {
  append_lifecycle_trace("override_callback_destroy");
  return PS_OPERATION_STATUS_OK_V1;
}

/** @brief Creates the replacement implementation definition. */
Implementation make_implementation() noexcept {
  Implementation implementation;
  auto& descriptor = implementation.descriptor;
  descriptor.header =
      make_record_header(PS_OPERATION_IMPLEMENTATION_DESCRIPTOR_V1_SIZE,
                         PS_OPERATION_RECORD_IMPLEMENTATION_DESCRIPTOR_V1);
  descriptor.implementation_identity = kImplementationIdentity;
  descriptor.operation_identity = kOperationIdentity;
  descriptor.name = make_bytes("PLUGIN_OVERRIDE_TEST");
  descriptor.intent_mask = PS_OPERATION_INTENT_HP_V1;
  descriptor.execution_shape_mask = PS_OPERATION_EXECUTION_MONOLITHIC_V1;
  descriptor.device_kind = PS_OPERATION_DEVICE_CPU_V1;
  descriptor.reentrant = 1U;
  descriptor.relative_cost_binary64_bits = 0x4000000000000000ULL;
  descriptor.execution_mode = PS_OPERATION_EXECUTION_TRUSTED_IN_PROCESS_V1;
  implementation.infer = infer_override;
  implementation.execute_monolithic = execute_override;
  return implementation;
}

/** @brief Stable replacement implementation row. */
const Implementation kImplementations[]{make_implementation()};

/** @brief Creates the replacement definition for the predecessor key. */
ps_operation_descriptor_v1 make_operation() noexcept {
  ps_operation_descriptor_v1 operation{};
  operation.header =
      make_record_header(PS_OPERATION_DESCRIPTOR_V1_SIZE,
                         PS_OPERATION_RECORD_OPERATION_DESCRIPTOR_V1);
  operation.operation_identity = kOperationIdentity;
  operation.type = make_bytes("plugin_lifecycle");
  operation.subtype = make_bytes("op");
  operation.display_name = make_bytes("Lifecycle replacement ABI fixture");
  operation.configuration_schema_identity = kConfigurationIdentity;
  operation.input_ports = empty_array_ref();
  operation.output_ports = empty_array_ref();
  return operation;
}

/** @brief Stable complete replacement fixture definition. */
const Definition kDefinition{kPluginIdentity,
                             "lifecycle-override-abi1",
                             make_operation(),
                             kImplementations,
                             1U,
                             destroy_override_generation,
                             nullptr};

}  // namespace

/** @copydoc plugin_definition */
const Definition& plugin_definition() noexcept {
  return kDefinition;
}

}  // namespace ps::operation_plugin

PS_DEFINE_OPERATION_PLUGIN_V1()
