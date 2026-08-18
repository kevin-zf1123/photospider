#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "photospider/plugin/operation_plugin.hpp"

namespace ps::operation_plugin {
namespace {

/** @brief Environment variable selecting one conformance mutation mode. */
constexpr const char* kModeEnvironment = "PS_OPERATION_CONFORMANCE_MODE";
/** @brief Environment variable receiving any forbidden direct callback entry.
 */
constexpr const char* kTraceEnvironment = "PS_OPERATION_CONFORMANCE_TRACE";

/** @brief Permanent conformance fixture plugin identity. */
constexpr auto kPluginIdentity =
    make_identity(0x5053434F4E464F52ULL, 0x0001ULL);
/** @brief Permanent conformance operation identity. */
constexpr auto kOperationIdentity =
    make_identity(0x5053434F4E464F50ULL, 0x0001ULL);
/** @brief Permanent conformance implementation identity. */
constexpr auto kImplementationIdentity =
    make_identity(0x5053434F4E46494DULL, 0x0001ULL);
/** @brief Permanent conformance configuration-Schema identity. */
constexpr auto kConfigurationIdentity =
    make_identity(0x5053434F4E464346ULL, 0x0001ULL);
/** @brief Permanent conformance output-port identity. */
constexpr auto kOutputIdentity =
    make_identity(0x5053434F4E464F55ULL, 0x0001ULL);
/** @brief Permanent signed runtime package identity selected by the fixture. */
constexpr auto kRuntimePackageIdentity =
    make_identity(0x5053434F4E465254ULL, 0x0001ULL);

/**
 * @brief Reports whether the current test selected one exact mutation mode.
 * @param expected Stable mode spelling.
 * @return True only for an exact environment-string match.
 * @throws Nothing.
 */
bool mode_is(const char* expected) noexcept {
  const char* mode = std::getenv(kModeEnvironment);
  return mode != nullptr && std::strcmp(mode, expected) == 0;
}

/**
 * @brief Records forbidden entry into the in-process tiled callback.
 * @return Nothing.
 * @throws Nothing; missing configuration and I/O failures are ignored.
 * @note A valid supervised descriptor must reach the runtime router first and
 * therefore leave the trace absent when its exact route is unavailable.
 */
void trace_direct_execution() noexcept {
  const char* path = std::getenv(kTraceEnvironment);
  if (path == nullptr || path[0] == '\0') {
    return;
  }
  std::FILE* output = std::fopen(path, "a");
  if (output == nullptr) {
    return;
  }
  (void)std::fputs("execute_tiled\n", output);
  (void)std::fclose(output);
}

/**
 * @brief Supplies a nonnull reserved suite callback for hostile-table tests.
 * @return Nothing.
 * @throws Nothing.
 */
void PS_OPERATION_CALL reserved_callback() noexcept {}

/**
 * @brief Returns a deterministic inference failure if unexpectedly selected.
 * @return `INVALID_DESCRIPTOR` because focused tiled calls already own a plan.
 * @throws Nothing.
 */
ps_operation_status_v1 PS_OPERATION_CALL infer_unexpected(
    void*, const ps_operation_invocation_v1*,
    const ps_operation_configuration_view_v1*, const ps_operation_array_ref_v1*,
    const ps_operation_output_sink_v1*) noexcept {
  return PS_OPERATION_STATUS_INVALID_DESCRIPTOR_V1;
}

/**
 * @brief Detects any forbidden trusted fallback for the supervised fixture.
 * @return `INTERNAL_ERROR` after recording entry.
 * @throws Nothing.
 */
ps_operation_status_v1 PS_OPERATION_CALL execute_tiled_unexpected(
    void*, const ps_operation_invocation_v1*,
    const ps_operation_configuration_view_v1*, const ps_operation_array_ref_v1*,
    const ps_operation_array_ref_v1*, const ps_operation_tile_v1*,
    const ps_operation_output_sink_v1*) noexcept {
  trace_direct_execution();
  return PS_OPERATION_STATUS_INTERNAL_ERROR_V1;
}

/**
 * @brief Creates the sole DenseImage output-port descriptor.
 * @return Complete exact v1 output-port record with stable identities.
 * @throws Nothing.
 * @note All borrowed name bytes have DSO lifetime.
 */
ps_operation_port_descriptor_v1 make_output_port() noexcept {
  ps_operation_port_descriptor_v1 port{};
  port.header = make_record_header(PS_OPERATION_PORT_DESCRIPTOR_V1_SIZE,
                                   PS_OPERATION_RECORD_PORT_DESCRIPTOR_V1);
  port.port_identity = kOutputIdentity;
  port.index = 0U;
  port.direction = PS_OPERATION_PORT_OUTPUT_V1;
  port.name = make_bytes("image");
  port.schema_identity = make_identity(0x50534449U, 0x1001U);
  port.facet_identity = make_identity(0x50534449U, 0x1002U);
  port.layout_identity = make_identity(0x50534449U, 0x1003U);
  return port;
}

/** @brief Stable output-port row borrowed for the DSO lifetime. */
const ps_operation_port_descriptor_v1 kOutputPorts[]{make_output_port()};

/**
 * @brief Creates the valid supervised tiled implementation descriptor.
 * @return Complete helper row selecting a signed runtime package identity.
 * @throws Nothing.
 * @note Its direct tiled callback is a test tripwire and must remain uncalled
 * when the Host honors supervised-process routing.
 */
Implementation make_implementation() noexcept {
  Implementation implementation;
  auto& descriptor = implementation.descriptor;
  descriptor.header =
      make_record_header(PS_OPERATION_IMPLEMENTATION_DESCRIPTOR_V1_SIZE,
                         PS_OPERATION_RECORD_IMPLEMENTATION_DESCRIPTOR_V1);
  descriptor.implementation_identity = kImplementationIdentity;
  descriptor.operation_identity = kOperationIdentity;
  descriptor.name = make_bytes("SUPERVISED_CONFORMANCE_TILE");
  descriptor.intent_mask = PS_OPERATION_INTENT_HP_V1;
  descriptor.execution_shape_mask = PS_OPERATION_EXECUTION_TILED_V1;
  descriptor.device_kind = PS_OPERATION_DEVICE_CPU_V1;
  descriptor.output_access_mask = PS_OPERATION_ACCESS_WRITE_V1;
  descriptor.reentrant = 1U;
  descriptor.relative_cost_binary64_bits = 0x3FF0000000000000ULL;
  descriptor.execution_mode = PS_OPERATION_EXECUTION_SUPERVISED_PROCESS_V1;
  descriptor.runtime_package_identity = kRuntimePackageIdentity;
  implementation.infer = infer_unexpected;
  implementation.execute_tiled = execute_tiled_unexpected;
  return implementation;
}

/** @brief Stable valid implementation row before callback-local mutation. */
const Implementation kImplementations[]{make_implementation()};

/**
 * @brief Creates the valid single-output conformance operation.
 * @return Complete exact v1 operation descriptor with stable output storage.
 * @throws Nothing.
 * @note The fixture deliberately declares no inputs because only route
 * selection and Host-owned output-plan transport are under test.
 */
ps_operation_descriptor_v1 make_operation() noexcept {
  ps_operation_descriptor_v1 operation{};
  operation.header =
      make_record_header(PS_OPERATION_DESCRIPTOR_V1_SIZE,
                         PS_OPERATION_RECORD_OPERATION_DESCRIPTOR_V1);
  operation.operation_identity = kOperationIdentity;
  operation.type = make_bytes("operation_conformance");
  operation.subtype = make_bytes("supervised_tile");
  operation.display_name = make_bytes("Operation ABI conformance fixture");
  operation.configuration_schema_identity = kConfigurationIdentity;
  operation.input_ports = empty_array_ref();
  operation.output_ports = make_array_ref(kOutputPorts, 1U);
  return operation;
}

/** @brief Stable complete conformance definition. */
const Definition kDefinition{kPluginIdentity,
                             "operation-conformance-abi1",
                             make_operation(),
                             kImplementations,
                             1U,
                             nullptr,
                             nullptr};

/**
 * @brief Returns the operation count or one deliberately excessive count.
 * @param context Borrowed helper context.
 * @param count Nonnull Host count destination.
 * @param sink Borrowed Host diagnostic sink.
 * @return Helper status or `OK` with the hostile count.
 * @throws Nothing.
 */
ps_operation_status_v1 PS_OPERATION_CALL
conformance_operation_count(void* context, std::uint32_t* count,
                            const ps_operation_output_sink_v1* sink) noexcept {
  if (mode_is("count_bound")) {
    if (count == nullptr) {
      return PS_OPERATION_STATUS_INVALID_DESCRIPTOR_V1;
    }
    *count = PS_OPERATION_MAX_OPERATIONS_V1 + 1U;
    return PS_OPERATION_STATUS_OK_V1;
  }
  return get_operation_count(context, count, sink);
}

/**
 * @brief Copies then optionally corrupts one operation record.
 * @param context Borrowed helper context.
 * @param index Requested dense operation index.
 * @param output Host-prepared operation record.
 * @param sink Borrowed Host diagnostic sink.
 * @return Helper status.
 * @throws Nothing.
 */
ps_operation_status_v1 PS_OPERATION_CALL conformance_operation(
    void* context, std::uint32_t index, ps_operation_descriptor_v1* output,
    const ps_operation_output_sink_v1* sink) noexcept {
  const ps_operation_status_v1 status =
      get_operation(context, index, output, sink);
  if (status != PS_OPERATION_STATUS_OK_V1) {
    return status;
  }
  if (mode_is("operation_tail")) {
    output->header.struct_size -= 8U;
  }
  if (mode_is("output_stride")) {
    output->output_ports.stride -= 8U;
  }
  return status;
}

/**
 * @brief Copies then optionally corrupts one implementation record.
 * @param context Borrowed helper context.
 * @param operation Exact parent operation identity.
 * @param index Requested dense implementation index.
 * @param output Host-prepared implementation record.
 * @param sink Borrowed Host diagnostic sink.
 * @return Helper status.
 * @throws Nothing.
 */
ps_operation_status_v1 PS_OPERATION_CALL conformance_implementation(
    void* context, const ps_operation_identity_v1* operation,
    std::uint32_t index, ps_operation_implementation_descriptor_v1* output,
    const ps_operation_output_sink_v1* sink) noexcept {
  const ps_operation_status_v1 status =
      get_implementation(context, operation, index, output, sink);
  if (status == PS_OPERATION_STATUS_OK_V1 &&
      mode_is("implementation_reserved")) {
    output->reserved[0] = 1U;
  }
  return status;
}

/**
 * @brief Copies then optionally corrupts one required suite table.
 * @param context Borrowed helper context.
 * @param suite_id Requested suite identity.
 * @param requested_version Required exact version.
 * @param suite_out Host-prepared suite destination.
 * @return Helper status.
 * @throws Nothing.
 */
ps_operation_status_v1 PS_OPERATION_CALL
conformance_query_suite(void* context, ps_operation_suite_id_v1 suite_id,
                        std::uint32_t requested_version,
                        ps_operation_suite_header_v1* suite_out) noexcept {
  const ps_operation_status_v1 status =
      query_suite(context, suite_id, requested_version, suite_out);
  if (status != PS_OPERATION_STATUS_OK_V1) {
    return status;
  }
  if (mode_is("suite_tail") && suite_id == PS_OPERATION_SUITE_EXECUTION_V1) {
    suite_out->struct_size -= 8U;
  }
  if (mode_is("suite_reserved") &&
      suite_id == PS_OPERATION_SUITE_EXECUTION_V1) {
    auto* suite = reinterpret_cast<ps_operation_execution_suite_v1*>(suite_out);
    suite->reserved0 = reserved_callback;
  }
  if (suite_id == PS_OPERATION_SUITE_DEFINITION_V1) {
    auto* suite =
        reinterpret_cast<ps_operation_definition_suite_v1*>(suite_out);
    suite->get_operation_count = conformance_operation_count;
    suite->get_operation = conformance_operation;
    suite->get_implementation = conformance_implementation;
  }
  return status;
}

}  // namespace

/** @copydoc plugin_definition */
const Definition& plugin_definition() noexcept {
  return kDefinition;
}

}  // namespace ps::operation_plugin

/**
 * @brief Returns the separately versioned operation ABI selected by the DSO.
 * @return Exactly `PS_OPERATION_PLUGIN_ABI_VERSION`.
 * @throws Nothing.
 * @note Numeric discovery performs no allocation or registration side effect.
 */
extern "C" PS_OPERATION_PLUGIN_EXPORT std::uint32_t PS_OPERATION_CALL
ps_operation_plugin_get_abi_version(void) noexcept {
  return PS_OPERATION_PLUGIN_ABI_VERSION;
}

/**
 * @brief Fills then optionally corrupts the exact conformance root table.
 * @param api Host-prepared root destination.
 * @return Stable ABI status.
 * @throws Nothing.
 * @note Mutations are confined to hostile-record test modes and are rejected
 * by Host validation before publication or callback invocation.
 */
extern "C" PS_OPERATION_PLUGIN_EXPORT ps_operation_status_v1 PS_OPERATION_CALL
ps_operation_plugin_get_api_v1(ps_operation_plugin_api_v1* api) noexcept {
  const ps_operation_status_v1 status = ::ps::operation_plugin::get_api(api);
  if (status != PS_OPERATION_STATUS_OK_V1) {
    return status;
  }
  api->query_suite = ::ps::operation_plugin::conformance_query_suite;
  if (::ps::operation_plugin::mode_is("root_reserved")) {
    api->reserved[0] = 1U;
  }
  return status;
}
