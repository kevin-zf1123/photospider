#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <iterator>
#include <string_view>

#include "photospider/plugin/operation_plugin.hpp"

namespace ps::operation_plugin {
namespace {

/** @brief Environment variable selecting one conformance mutation mode. */
constexpr const char* kModeEnvironment = "PS_OPERATION_CONFORMANCE_MODE";
/** @brief Environment variable receiving any forbidden direct callback entry.
 */
constexpr const char* kTraceEnvironment = "PS_OPERATION_CONFORMANCE_TRACE";

/** @brief Permanent conformance fixture plugin identity. */
constexpr auto kPluginIdentity{
    make_identity(0x5053434F4E464F52ULL, 0x0001ULL),
};
/** @brief Permanent conformance operation identity. */
constexpr auto kOperationIdentity{
    make_identity(0x5053434F4E464F50ULL, 0x0001ULL),
};
/** @brief Permanent conformance implementation identity. */
constexpr auto kImplementationIdentity{
    make_identity(0x5053434F4E46494DULL, 0x0001ULL),
};
/** @brief Permanent HP-fast candidate implementation identity. */
constexpr auto kHpFastImplementationIdentity{
    make_identity(0x5053434F4E46494DULL, 0x0002ULL),
};
/** @brief Permanent RT-slow candidate implementation identity. */
constexpr auto kRtSlowImplementationIdentity{
    make_identity(0x5053434F4E46494DULL, 0x0003ULL),
};
/** @brief Permanent RT-fast candidate implementation identity. */
constexpr auto kRtFastImplementationIdentity{
    make_identity(0x5053434F4E46494DULL, 0x0004ULL),
};
/** @brief Permanent mixed-intent/mixed-shape implementation identity. */
constexpr auto kMixedImplementationIdentity{
    make_identity(0x5053434F4E46494DULL, 0x0005ULL),
};
/** @brief Permanent conformance configuration-Schema identity. */
constexpr auto kConfigurationIdentity{
    make_identity(0x5053434F4E464346ULL, 0x0001ULL),
};
/** @brief Permanent conformance output-port identity. */
constexpr auto kOutputIdentity{
    make_identity(0x5053434F4E464F55ULL, 0x0001ULL),
};
/** @brief Permanent conformance input-port identity. */
constexpr auto kInputIdentity{
    make_identity(0x5053434F4E46494EULL, 0x0001ULL),
};
/** @brief Permanent signed runtime package identity selected by the fixture. */
constexpr auto kRuntimePackageIdentity{
    make_identity(0x4142434445464748ULL, 0x494A4B4C4D4E4F50ULL),
};

/** @brief Non-default descriptor version emitted by the fixture. */
constexpr std::uint64_t kDescriptorVersion = 7U;
/** @brief Non-default Layout version emitted by the fixture. */
constexpr std::uint64_t kLayoutVersion = 11U;

/**
 * @brief Reads the process-global conformance mode selected before DSO load.
 * @return Borrowed environment bytes or a static empty string when absent.
 * @throws Nothing.
 * @note The serial test target keeps the process environment stable throughout
 * each discovery and callback lifetime.
 */
const char* selected_mode() noexcept {
  const char* mode = std::getenv(kModeEnvironment);
  return mode == nullptr ? "" : mode;
}

/**
 * @brief Reports whether the current test selected one exact mutation mode.
 * @param expected Stable mode spelling.
 * @return True only for an exact environment-string match.
 * @throws Nothing.
 */
bool mode_is(const char* expected) noexcept {
  return std::strcmp(selected_mode(), expected) == 0;
}

/**
 * @brief Reports whether the selected mode contains one stable token.
 * @param token Nonnull token to search for.
 * @return True when the token occurs in the current mode.
 * @throws Nothing.
 */
bool mode_contains(const char* token) noexcept {
  return token != nullptr && std::strstr(selected_mode(), token) != nullptr;
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
 * @brief Emits one deterministic DenseImage or facet-free DenseTensor plan.
 * @param plugin_context Unused definition-lifetime fixture context.
 * @param invocation Nonnull Host invocation identity and intent record.
 * @param configuration Borrowed immutable effective configuration.
 * @param inputs Exact one-row descriptor-only input-binding array.
 * @param sink Nonnull Host output-plan sink.
 * @return Sink status after synchronous emission.
 * @throws Nothing; every record borrows callback-local fixed storage.
 * @note Non-default versions and digests prove exact trusted/supervised plan
 * echo. Identity-mismatch modes deliberately violate the output-port contract.
 */
ps_operation_status_v1 PS_OPERATION_CALL infer_conformance(
    void* plugin_context, const ps_operation_invocation_v1* invocation,
    const ps_operation_configuration_view_v1* configuration,
    const ps_operation_array_ref_v1* inputs,
    const ps_operation_output_sink_v1* sink) noexcept {
  (void)plugin_context;
  (void)invocation;
  (void)configuration;
  if (sink == nullptr || sink->emit == nullptr) {
    return PS_OPERATION_STATUS_INVALID_DESCRIPTOR_V1;
  }
  if (mode_contains("input_metadata")) {
    const auto* input = array_element<ps_operation_input_binding_v1>(
        inputs, 0U, PS_OPERATION_INPUT_BINDING_V1_SIZE);
    if (input == nullptr || input->value == nullptr ||
        input->value->descriptor == nullptr) {
      return PS_OPERATION_STATUS_INVALID_DESCRIPTOR_V1;
    }
    const auto& input_descriptor = *input->value->descriptor;
    const bool facet_free_input = mode_contains("facet_free_input");
    const bool facet_identity_exact =
        facet_free_input ? identity_equal(input_descriptor.facet_identity,
                                          ps_operation_identity_v1{})
                         : identity_equal(input_descriptor.facet_identity,
                                          kBuiltinImageFacetIdentity);
    if (!identity_equal(input_descriptor.schema_identity,
                        kBuiltinDenseTensorSchemaIdentity) ||
        !facet_identity_exact ||
        !identity_equal(input_descriptor.layout_identity,
                        kBuiltinStridedLayoutIdentity) ||
        input_descriptor.descriptor_version != kDescriptorVersion ||
        input_descriptor.layout_version != kLayoutVersion ||
        input_descriptor.descriptor_digest.words[0] != 0x0102030405060708ULL ||
        input_descriptor.descriptor_digest.words[3] != 0x1112131415161718ULL ||
        input_descriptor.content_digest.words[1] != 0x2122232425262728ULL ||
        input_descriptor.layout_digest.words[2] != 0x3132333435363738ULL) {
      return PS_OPERATION_STATUS_INVALID_DESCRIPTOR_V1;
    }
  }
  const bool facet_free_output = mode_contains("facet_free_output");
  const std::uint64_t extents[]{2U, facet_free_output ? 3U : 2U, 1U};
  ps_operation_dense_tensor_descriptor_v1 dense{};
  dense.header =
      make_record_header(PS_OPERATION_DENSE_TENSOR_DESCRIPTOR_V1_SIZE,
                         PS_OPERATION_RECORD_DENSE_TENSOR_DESCRIPTOR_V1);
  dense.rank = facet_free_output ? 2U : 3U;
  dense.element_semantics = PS_OPERATION_ELEMENT_UNSIGNED_INTEGER_V1;
  dense.storage_encoding = PS_OPERATION_STORAGE_NATIVE_SCALAR_V1;
  dense.bit_width = 8U;
  dense.extents = make_array_ref(extents, facet_free_output ? 2U : 3U);

  ps_operation_image_facet_v1 image{};
  image.header = make_record_header(PS_OPERATION_IMAGE_FACET_V1_SIZE,
                                    PS_OPERATION_RECORD_IMAGE_FACET_V1);
  image.x_axis = 1U;
  image.y_axis = 0U;
  image.channel_axis = 2U;
  image.presence_mask = PS_OPERATION_IMAGE_HAS_CHANNEL_AXIS_V1;
  image.data_window = ps_operation_image_bounds_v1{0, 0, 2, 2};

  const std::int64_t strides[]{facet_free_output ? 3 : 2, 1, 1};
  ps_operation_strided_layout_v1 layout{};
  layout.header = make_record_header(PS_OPERATION_STRIDED_LAYOUT_V1_SIZE,
                                     PS_OPERATION_RECORD_STRIDED_LAYOUT_V1);
  layout.rank = facet_free_output ? 2U : 3U;
  layout.byte_strides = make_array_ref(strides, facet_free_output ? 2U : 3U);
  layout.storage_size = facet_free_output ? 6U : 4U;

  ps_operation_value_descriptor_v1 descriptor{};
  descriptor.header =
      make_record_header(PS_OPERATION_VALUE_DESCRIPTOR_V1_SIZE,
                         PS_OPERATION_RECORD_VALUE_DESCRIPTOR_V1);
  descriptor.schema_identity = kBuiltinDenseTensorSchemaIdentity;
  descriptor.facet_identity = facet_free_output ? ps_operation_identity_v1{}
                                                : kBuiltinImageFacetIdentity;
  descriptor.layout_identity = kBuiltinStridedLayoutIdentity;
  if (mode_contains("identity_mismatch")) {
    descriptor.schema_identity.word1 ^= 1U;
  }
  descriptor.descriptor_version = kDescriptorVersion;
  descriptor.layout_version = kLayoutVersion;
  descriptor.descriptor_digest.words[0] = 0x0102030405060708ULL;
  descriptor.descriptor_digest.words[3] = 0x1112131415161718ULL;
  descriptor.content_digest.words[1] = 0x2122232425262728ULL;
  descriptor.layout_digest.words[2] = 0x3132333435363738ULL;
  descriptor.dense_tensor = &dense;
  descriptor.image_facet = facet_free_output ? nullptr : &image;
  descriptor.strided_layout = &layout;

  const ps_operation_axis_range_v1 ranges[]{
      ps_operation_axis_range_v1{0, 2U},
      ps_operation_axis_range_v1{0, facet_free_output ? 3U : 2U},
  };
  ps_operation_region_atom_v1 atom{};
  atom.header = make_record_header(PS_OPERATION_REGION_ATOM_V1_SIZE,
                                   PS_OPERATION_RECORD_REGION_ATOM_V1);
  atom.atom_kind = facet_free_output ? PS_OPERATION_REGION_ATOM_TENSOR_SLICE_V1
                                     : PS_OPERATION_REGION_ATOM_IMAGE_RECT_V1;
  atom.rank = 2U;
  atom.domain_identity =
      facet_free_output
          ? make_identity(0x50484F544F535049ULL, 0x4445525F54454E31ULL)
          : make_identity(0x50484F544F535049ULL, 0x4445525F494D4731ULL);
  atom.axis_ranges = make_array_ref(ranges, 2U);
  ps_operation_region_set_view_v1 region{};
  region.header = make_record_header(PS_OPERATION_REGION_SET_VIEW_V1_SIZE,
                                     PS_OPERATION_RECORD_REGION_SET_VIEW_V1);
  region.set_kind = PS_OPERATION_REGION_SET_CLAUSE_V1;
  region.atoms = make_array_ref(&atom, 1U);

  ps_operation_output_buffer_plan_v1 buffer{};
  buffer.header = make_record_header(PS_OPERATION_OUTPUT_BUFFER_PLAN_V1_SIZE,
                                     PS_OPERATION_RECORD_OUTPUT_BUFFER_PLAN_V1);
  buffer.buffer_index = 0U;
  buffer.access_mask = PS_OPERATION_ACCESS_WRITE_V1;
  buffer.byte_size = facet_free_output ? 6U : 4U;
  buffer.alignment = 64U;

  ps_operation_output_plan_v1 plan{};
  plan.header = make_record_header(PS_OPERATION_OUTPUT_PLAN_V1_SIZE,
                                   PS_OPERATION_RECORD_OUTPUT_PLAN_V1);
  plan.port_identity = kOutputIdentity;
  plan.port_index = 0U;
  plan.buffer_count = 1U;
  plan.descriptor = &descriptor;
  plan.buffers = make_array_ref(&buffer, 1U);
  plan.full_region = &region;
  plan.access_mask = PS_OPERATION_ACCESS_WRITE_V1;
  return sink->emit(sink->host_context, PS_OPERATION_OUTPUT_PLAN_V1, &plan, 1U,
                    PS_OPERATION_OUTPUT_PLAN_V1_SIZE);
}

/**
 * @brief Checks the exact non-default descriptor metadata echoed by the Host.
 * @param output Nonnull mutable output binding.
 * @return True only when plan and binding descriptors preserve every fact.
 * @throws Nothing.
 */
bool metadata_is_exact(
    const ps_operation_mutable_output_binding_v1* output) noexcept {
  if (output == nullptr || output->plan == nullptr ||
      output->descriptor == nullptr ||
      output->plan->descriptor != output->descriptor) {
    return false;
  }
  const auto& descriptor = *output->descriptor;
  const bool facet_free_output = mode_contains("facet_free_output");
  const bool facet_identity_exact =
      facet_free_output ? identity_equal(descriptor.facet_identity,
                                         ps_operation_identity_v1{})
                        : identity_equal(descriptor.facet_identity,
                                         kBuiltinImageFacetIdentity);
  return identity_equal(descriptor.schema_identity,
                        kBuiltinDenseTensorSchemaIdentity) &&
         facet_identity_exact &&
         identity_equal(descriptor.layout_identity,
                        kBuiltinStridedLayoutIdentity) &&
         descriptor.descriptor_version == kDescriptorVersion &&
         descriptor.layout_version == kLayoutVersion &&
         descriptor.descriptor_digest.words[0] == 0x0102030405060708ULL &&
         descriptor.descriptor_digest.words[3] == 0x1112131415161718ULL &&
         descriptor.content_digest.words[1] == 0x2122232425262728ULL &&
         descriptor.layout_digest.words[2] == 0x3132333435363738ULL;
}

/**
 * @brief Checks a connected input retained exact non-default metadata.
 * @param inputs Exact one-row connected input-binding array.
 * @return True only when its ValueDescriptor matches the inferred output.
 * @throws Nothing.
 */
bool input_metadata_is_exact(const ps_operation_array_ref_v1* inputs) noexcept {
  const auto* input = array_element<ps_operation_input_binding_v1>(
      inputs, 0U, PS_OPERATION_INPUT_BINDING_V1_SIZE);
  if (input == nullptr || input->value == nullptr ||
      input->value->descriptor == nullptr) {
    return false;
  }
  const auto& descriptor = *input->value->descriptor;
  const bool facet_free_input = mode_contains("facet_free_input");
  const bool facet_identity_exact =
      facet_free_input ? identity_equal(descriptor.facet_identity,
                                        ps_operation_identity_v1{})
                       : identity_equal(descriptor.facet_identity,
                                        kBuiltinImageFacetIdentity);
  return identity_equal(descriptor.schema_identity,
                        kBuiltinDenseTensorSchemaIdentity) &&
         facet_identity_exact &&
         identity_equal(descriptor.layout_identity,
                        kBuiltinStridedLayoutIdentity) &&
         descriptor.descriptor_version == kDescriptorVersion &&
         descriptor.layout_version == kLayoutVersion &&
         descriptor.descriptor_digest.words[0] == 0x0102030405060708ULL &&
         descriptor.descriptor_digest.words[3] == 0x1112131415161718ULL &&
         descriptor.content_digest.words[1] == 0x2122232425262728ULL &&
         descriptor.layout_digest.words[2] == 0x3132333435363738ULL;
}

/**
 * @brief Applies one selected hostile mutation to the actual callback graph.
 * @param output Nonnull callback output record.
 * @return Nothing after at most one deterministic mutation.
 * @throws Nothing; tests supply the complete Host-owned graph.
 */
void mutate_output_authority(
    ps_operation_mutable_output_binding_v1* output) noexcept {
  if (output == nullptr) {
    return;
  }
  if (mode_contains("mutate_binding")) {
    output->binding_flags = 1U;
  } else if (mode_contains("mutate_descriptor")) {
    auto* descriptor =
        const_cast<ps_operation_value_descriptor_v1*>(output->descriptor);
    if (descriptor != nullptr) {
      ++descriptor->descriptor_version;
    }
  } else if (mode_contains("mutate_buffer_plan") && output->plan != nullptr) {
    auto* buffers = const_cast<ps_operation_output_buffer_plan_v1*>(
        static_cast<const ps_operation_output_buffer_plan_v1*>(
            output->plan->buffers.data));
    if (buffers != nullptr) {
      ++buffers[0].byte_size;
    }
  } else if (mode_contains("mutate_full_region") && output->plan != nullptr) {
    auto* region =
        const_cast<ps_operation_region_set_view_v1*>(output->plan->full_region);
    if (region != nullptr) {
      region->set_kind = PS_OPERATION_REGION_SET_WHOLE_V1;
    }
  } else if (mode_contains("mutate_grant_region")) {
    auto* region = const_cast<ps_operation_region_set_view_v1*>(output->region);
    if (region != nullptr) {
      region->set_kind = PS_OPERATION_REGION_SET_WHOLE_V1;
    }
  } else if (mode_contains("mutate_nested_extent") &&
             output->descriptor != nullptr &&
             output->descriptor->dense_tensor != nullptr) {
    auto* extents =
        const_cast<std::uint64_t*>(static_cast<const std::uint64_t*>(
            output->descriptor->dense_tensor->extents.data));
    if (extents != nullptr) {
      ++extents[0];
    }
  }
}

/**
 * @brief Executes one trusted conformance callback and optional mutation.
 * @param inputs Exact one-row input array.
 * @param outputs Exact one-row mutable output array.
 * @return Stable status after validation, payload fill, and mutation.
 * @throws Nothing.
 * @note Hostile mutation modes deliberately bypass the independent exact-
 * metadata assertion so the callback reaches and modifies the requested
 * authority surface even when replayed against the old Host implementation.
 */
ps_operation_status_v1 execute_conformance(
    const ps_operation_array_ref_v1* inputs,
    const ps_operation_array_ref_v1* outputs) noexcept {
  auto* output = const_cast<ps_operation_mutable_output_binding_v1*>(
      array_element<ps_operation_mutable_output_binding_v1>(
          outputs, 0U, PS_OPERATION_MUTABLE_OUTPUT_BINDING_V1_SIZE));
  if (output == nullptr) {
    return PS_OPERATION_STATUS_INVALID_DESCRIPTOR_V1;
  }
  const bool isolates_authority_mutation = mode_contains("mutate_");
  if (!mode_contains("identity_mismatch") && !isolates_authority_mutation &&
      !metadata_is_exact(output)) {
    return PS_OPERATION_STATUS_INVALID_DESCRIPTOR_V1;
  }
  if (mode_contains("input_metadata") && !input_metadata_is_exact(inputs)) {
    return PS_OPERATION_STATUS_INVALID_DESCRIPTOR_V1;
  }
  const auto* spans =
      static_cast<const ps_operation_output_grant_span_v1*>(output->spans.data);
  if (output->spans.count == 0U || spans == nullptr) {
    return PS_OPERATION_STATUS_INVALID_DESCRIPTOR_V1;
  }
  for (std::uint32_t index = 0U; index < output->spans.count; ++index) {
    if (spans[index].bytes.data == nullptr ||
        spans[index].bytes.size != spans[index].byte_size) {
      return PS_OPERATION_STATUS_INVALID_DESCRIPTOR_V1;
    }
    std::memset(spans[index].bytes.data, 0x5A,
                static_cast<std::size_t>(spans[index].bytes.size));
  }
  mutate_output_authority(output);
  return PS_OPERATION_STATUS_OK_V1;
}

/**
 * @brief Executes the trusted monolithic conformance callback.
 * @param plugin_context Unused definition-lifetime fixture context.
 * @param invocation Nonnull Host invocation identity and intent record.
 * @param configuration Borrowed immutable effective configuration.
 * @param inputs Exact one-row immutable input array.
 * @param outputs Exact one-row mutable output array.
 * @param sink Borrowed diagnostic sink, unused by the successful callback.
 * @return Stable status from shared metadata validation, fill, and mutation.
 * @throws Nothing.
 * @note Other callback records are intentionally unused by this output-only
 * fixture operation.
 */
ps_operation_status_v1 PS_OPERATION_CALL execute_monolithic_conformance(
    void* plugin_context, const ps_operation_invocation_v1* invocation,
    const ps_operation_configuration_view_v1* configuration,
    const ps_operation_array_ref_v1* inputs,
    const ps_operation_array_ref_v1* outputs,
    const ps_operation_output_sink_v1* sink) noexcept {
  (void)plugin_context;
  (void)invocation;
  (void)configuration;
  (void)sink;
  return execute_conformance(inputs, outputs);
}

/**
 * @brief Detects any forbidden trusted fallback for the supervised fixture.
 * @param plugin_context Unused definition-lifetime fixture context.
 * @param invocation Nonnull Host invocation identity and intent record.
 * @param configuration Borrowed immutable effective configuration.
 * @param inputs Exact one-row immutable input array.
 * @param outputs Exact one-row mutable output array.
 * @param tile Borrowed Host tile identity and grant-Region record.
 * @param sink Borrowed diagnostic sink, unused by the successful callback.
 * @return `INTERNAL_ERROR` after recording entry.
 * @throws Nothing.
 */
ps_operation_status_v1 PS_OPERATION_CALL execute_tiled_unexpected(
    void* plugin_context, const ps_operation_invocation_v1* invocation,
    const ps_operation_configuration_view_v1* configuration,
    const ps_operation_array_ref_v1* inputs,
    const ps_operation_array_ref_v1* outputs, const ps_operation_tile_v1* tile,
    const ps_operation_output_sink_v1* sink) noexcept {
  (void)plugin_context;
  (void)invocation;
  (void)configuration;
  (void)tile;
  (void)sink;
  if (mode_contains("supervised") || mode_is("valid")) {
    trace_direct_execution();
    return PS_OPERATION_STATUS_INTERNAL_ERROR_V1;
  }
  return execute_conformance(inputs, outputs);
}

/**
 * @brief Emits one implementation-distinguishing exact image Region binding.
 * @param invocation Nonnull candidate-set invocation identity.
 * @param port_identity Exact input or output port identity.
 * @param edge_identity Exact input edge or zero for an output binding.
 * @param dependency True for the dependency-specific HP-fast marker.
 * @param sink Nonnull Host Region sink.
 * @return Sink status after one synchronous exact binding emission.
 * @throws Nothing; every nested record is callback-local.
 */
ps_operation_status_v1 emit_candidate_region(
    const ps_operation_invocation_v1* invocation,
    ps_operation_identity_v1 port_identity,
    ps_operation_identity_v1 edge_identity, bool dependency,
    const ps_operation_output_sink_v1* sink) noexcept {
  if (invocation == nullptr || sink == nullptr || sink->emit == nullptr) {
    return PS_OPERATION_STATUS_INVALID_DESCRIPTOR_V1;
  }
  const bool hp_fast = identity_equal(invocation->implementation_identity,
                                      kHpFastImplementationIdentity);
  const bool rt_fast = identity_equal(invocation->implementation_identity,
                                      kRtFastImplementationIdentity);
  const std::int64_t x = hp_fast && !dependency ? 1 : 0;
  const std::int64_t y = (hp_fast && dependency) || rt_fast ? 1 : 0;
  const ps_operation_axis_range_v1 ranges[]{
      ps_operation_axis_range_v1{x, 1U},
      ps_operation_axis_range_v1{y, 1U},
  };
  ps_operation_region_atom_v1 atom{};
  atom.header = make_record_header(PS_OPERATION_REGION_ATOM_V1_SIZE,
                                   PS_OPERATION_RECORD_REGION_ATOM_V1);
  atom.atom_kind = PS_OPERATION_REGION_ATOM_IMAGE_RECT_V1;
  atom.rank = 2U;
  atom.domain_identity =
      make_identity(0x50484F544F535049ULL, 0x4445525F494D4731ULL);
  atom.axis_ranges = make_array_ref(ranges, 2U);
  ps_operation_region_set_view_v1 region{};
  region.header = make_record_header(PS_OPERATION_REGION_SET_VIEW_V1_SIZE,
                                     PS_OPERATION_RECORD_REGION_SET_VIEW_V1);
  region.set_kind = PS_OPERATION_REGION_SET_CLAUSE_V1;
  region.atoms = make_array_ref(&atom, 1U);
  ps_operation_region_binding_v1 binding{};
  binding.header = make_record_header(PS_OPERATION_REGION_BINDING_V1_SIZE,
                                      PS_OPERATION_RECORD_REGION_BINDING_V1);
  binding.port_identity = port_identity;
  binding.edge_identity = edge_identity;
  binding.region = &region;
  binding.outcome = PS_OPERATION_REGION_EXACT_V1;
  return sink->emit(sink->host_context, PS_OPERATION_OUTPUT_REGION_BINDING_V1,
                    &binding, 1U, PS_OPERATION_REGION_BINDING_V1_SIZE);
}

/**
 * @brief Maps demand through the exact selected candidate implementation.
 * @param plugin_context Unused definition-lifetime fixture context.
 * @param invocation Candidate-set invocation identity.
 * @param configuration Unused immutable effective configuration.
 * @param inputs Exact destination-indexed input bindings.
 * @param demands Unused demanded-output Region bindings.
 * @param sink Host Region sink.
 * @return Exact marker Region status.
 * @throws Nothing.
 */
ps_operation_status_v1 PS_OPERATION_CALL propagate_candidate_backward(
    void* plugin_context, const ps_operation_invocation_v1* invocation,
    const ps_operation_configuration_view_v1* configuration,
    const ps_operation_array_ref_v1* inputs,
    const ps_operation_array_ref_v1* demands,
    const ps_operation_output_sink_v1* sink) noexcept {
  (void)plugin_context;
  (void)configuration;
  (void)demands;
  const auto* input = array_element<ps_operation_input_binding_v1>(
      inputs, 0U, PS_OPERATION_INPUT_BINDING_V1_SIZE);
  if (input == nullptr || input->value == nullptr) {
    return PS_OPERATION_STATUS_INVALID_DESCRIPTOR_V1;
  }
  return emit_candidate_region(invocation, input->port_identity,
                               input->edge_identity, false, sink);
}

/**
 * @brief Maps one changed edge through the exact selected candidate.
 * @param plugin_context Unused definition-lifetime fixture context.
 * @param invocation Candidate-set invocation identity.
 * @param configuration Unused immutable effective configuration.
 * @param inputs Unused destination-indexed input bindings.
 * @param active_edge Nonnull active input-edge identity.
 * @param changed_region Unused changed upstream Region.
 * @param sink Host Region sink.
 * @return Exact marker Region status.
 * @throws Nothing.
 */
ps_operation_status_v1 PS_OPERATION_CALL propagate_candidate_forward(
    void* plugin_context, const ps_operation_invocation_v1* invocation,
    const ps_operation_configuration_view_v1* configuration,
    const ps_operation_array_ref_v1* inputs,
    const ps_operation_identity_v1* active_edge,
    const ps_operation_region_set_view_v1* changed_region,
    const ps_operation_output_sink_v1* sink) noexcept {
  (void)plugin_context;
  (void)configuration;
  (void)inputs;
  (void)changed_region;
  if (active_edge == nullptr) {
    return PS_OPERATION_STATUS_INVALID_DESCRIPTOR_V1;
  }
  return emit_candidate_region(invocation, kOutputIdentity,
                               ps_operation_identity_v1{}, false, sink);
}

/**
 * @brief Emits one selected-implementation dependency record.
 * @param plugin_context Unused definition-lifetime fixture context.
 * @param invocation Candidate-set invocation identity.
 * @param configuration Unused immutable effective configuration.
 * @param inputs Exact destination-indexed input bindings.
 * @param demands Unused demanded-output Region bindings.
 * @param sink Host dependency sink.
 * @return Sink status after one exact dependency row.
 * @throws Nothing; every nested record is callback-local.
 */
ps_operation_status_v1 PS_OPERATION_CALL build_candidate_dependencies(
    void* plugin_context, const ps_operation_invocation_v1* invocation,
    const ps_operation_configuration_view_v1* configuration,
    const ps_operation_array_ref_v1* inputs,
    const ps_operation_array_ref_v1* demands,
    const ps_operation_output_sink_v1* sink) noexcept {
  (void)plugin_context;
  (void)configuration;
  (void)demands;
  const auto* input = array_element<ps_operation_input_binding_v1>(
      inputs, 0U, PS_OPERATION_INPUT_BINDING_V1_SIZE);
  if (input == nullptr || input->value == nullptr || invocation == nullptr ||
      sink == nullptr || sink->emit == nullptr) {
    return PS_OPERATION_STATUS_INVALID_DESCRIPTOR_V1;
  }
  const bool hp_fast = identity_equal(invocation->implementation_identity,
                                      kHpFastImplementationIdentity);
  const ps_operation_axis_range_v1 ranges[]{
      ps_operation_axis_range_v1{0, 1U},
      ps_operation_axis_range_v1{hp_fast ? 1 : 0, 1U},
  };
  ps_operation_region_atom_v1 atom{};
  atom.header = make_record_header(PS_OPERATION_REGION_ATOM_V1_SIZE,
                                   PS_OPERATION_RECORD_REGION_ATOM_V1);
  atom.atom_kind = PS_OPERATION_REGION_ATOM_IMAGE_RECT_V1;
  atom.rank = 2U;
  atom.domain_identity =
      make_identity(0x50484F544F535049ULL, 0x4445525F494D4731ULL);
  atom.axis_ranges = make_array_ref(ranges, 2U);
  ps_operation_region_set_view_v1 region{};
  region.header = make_record_header(PS_OPERATION_REGION_SET_VIEW_V1_SIZE,
                                     PS_OPERATION_RECORD_REGION_SET_VIEW_V1);
  region.set_kind = PS_OPERATION_REGION_SET_CLAUSE_V1;
  region.atoms = make_array_ref(&atom, 1U);
  ps_operation_dependency_record_v1 dependency{};
  dependency.header =
      make_record_header(PS_OPERATION_DEPENDENCY_RECORD_V1_SIZE,
                         PS_OPERATION_RECORD_DEPENDENCY_RECORD_V1);
  dependency.output_port_identity = kOutputIdentity;
  dependency.output_site_identity =
      make_identity(0x505343414E445349ULL, 0x0001ULL);
  dependency.output_region_identity =
      make_identity(0x505343414E445247ULL, 0x0001ULL);
  dependency.input_edge_identity = input->edge_identity;
  dependency.input_region = &region;
  return sink->emit(sink->host_context,
                    PS_OPERATION_OUTPUT_DEPENDENCY_RECORD_V1, &dependency, 1U,
                    PS_OPERATION_DEPENDENCY_RECORD_V1_SIZE);
}

/**
 * @brief Creates the sole DenseImage or facet-free input-port descriptor.
 * @param facet_free True for a DenseTensor port without Image Facet.
 * @return Complete exact v1 input-port record with stable identities.
 * @throws Nothing.
 * @note All borrowed name bytes have DSO lifetime.
 */
ps_operation_port_descriptor_v1 make_input_port(bool facet_free) noexcept {
  ps_operation_port_descriptor_v1 port{};
  port.header = make_record_header(PS_OPERATION_PORT_DESCRIPTOR_V1_SIZE,
                                   PS_OPERATION_RECORD_PORT_DESCRIPTOR_V1);
  port.port_identity = kInputIdentity;
  port.index = 0U;
  port.direction = PS_OPERATION_PORT_INPUT_V1;
  port.name = make_bytes(facet_free ? "tensor" : "source");
  port.schema_identity = kBuiltinDenseTensorSchemaIdentity;
  port.facet_identity =
      facet_free ? ps_operation_identity_v1{} : kBuiltinImageFacetIdentity;
  port.layout_identity = kBuiltinStridedLayoutIdentity;
  return port;
}

/** @brief Stable ordinary-image input row borrowed for the DSO lifetime. */
const ps_operation_port_descriptor_v1 kImageInputPorts[]{
    make_input_port(false),
};
/** @brief Stable facet-free input row borrowed for the DSO lifetime. */
const ps_operation_port_descriptor_v1 kGenericInputPorts[]{
    make_input_port(true),
};

/**
 * @brief Creates the sole DenseImage or facet-free output-port descriptor.
 * @param facet_free True for a DenseTensor port without Image Facet.
 * @return Complete exact v1 output-port record with stable identities.
 * @throws Nothing.
 * @note All borrowed name bytes have DSO lifetime.
 */
ps_operation_port_descriptor_v1 make_output_port(bool facet_free) noexcept {
  ps_operation_port_descriptor_v1 port{};
  port.header = make_record_header(PS_OPERATION_PORT_DESCRIPTOR_V1_SIZE,
                                   PS_OPERATION_RECORD_PORT_DESCRIPTOR_V1);
  port.port_identity = kOutputIdentity;
  port.index = 0U;
  port.direction = PS_OPERATION_PORT_OUTPUT_V1;
  port.name = make_bytes(facet_free ? "tensor" : "image");
  port.schema_identity = kBuiltinDenseTensorSchemaIdentity;
  port.facet_identity =
      facet_free ? ps_operation_identity_v1{} : kBuiltinImageFacetIdentity;
  port.layout_identity = kBuiltinStridedLayoutIdentity;
  return port;
}

/** @brief Stable ordinary-image output row borrowed for the DSO lifetime. */
const ps_operation_port_descriptor_v1 kImageOutputPorts[]{
    make_output_port(false),
};
/** @brief Stable facet-free output row borrowed for the DSO lifetime. */
const ps_operation_port_descriptor_v1 kGenericOutputPorts[]{
    make_output_port(true),
};

/**
 * @brief Creates one trusted candidate-set implementation row.
 * @param identity Permanent public implementation identity.
 * @param name Stable diagnostic implementation name.
 * @param intent_mask Nonempty HP/RT eligibility mask.
 * @param shape_mask Nonempty monolithic/tiled shape mask.
 * @param relative_cost_binary64_bits Positive finite binary64 cost bits.
 * @param behavior_mask Optional SideEffect/DataDependent declaration bits.
 * @return Complete callback-bearing helper row.
 * @throws Nothing.
 * @note The default conformance mode rewrites the first copied descriptor into
 * its historical supervised-tiled profile; callback storage remains complete
 * so the dedicated candidate mode can publish mixed shapes atomically.
 */
Implementation make_implementation(
    ps_operation_identity_v1 identity, std::string_view name,
    ps_operation_intent_mask_v1 intent_mask,
    ps_operation_execution_shape_mask_v1 shape_mask,
    std::uint64_t relative_cost_binary64_bits,
    ps_operation_behavior_mask_v1 behavior_mask = 0U) noexcept {
  Implementation implementation;
  auto& descriptor = implementation.descriptor;
  descriptor.header =
      make_record_header(PS_OPERATION_IMPLEMENTATION_DESCRIPTOR_V1_SIZE,
                         PS_OPERATION_RECORD_IMPLEMENTATION_DESCRIPTOR_V1);
  descriptor.implementation_identity = identity;
  descriptor.operation_identity = kOperationIdentity;
  descriptor.name = make_bytes(name);
  descriptor.intent_mask = intent_mask;
  descriptor.execution_shape_mask = shape_mask;
  descriptor.device_kind = PS_OPERATION_DEVICE_CPU_V1;
  descriptor.behavior_mask = behavior_mask;
  descriptor.input_access_mask = PS_OPERATION_ACCESS_READ_V1;
  descriptor.output_access_mask = PS_OPERATION_ACCESS_WRITE_V1;
  descriptor.reentrant = 1U;
  descriptor.relative_cost_binary64_bits = relative_cost_binary64_bits;
  descriptor.execution_mode = PS_OPERATION_EXECUTION_TRUSTED_IN_PROCESS_V1;
  implementation.infer = infer_conformance;
  implementation.execute_monolithic = execute_monolithic_conformance;
  implementation.execute_tiled = execute_tiled_unexpected;
  return implementation;
}

/** @brief Stable complete candidate inventory before callback-local mutation.
 */
const Implementation kImplementations[]{
    make_implementation(kImplementationIdentity, "CANDIDATE_HP_MONO_SLOW",
                        PS_OPERATION_INTENT_HP_V1,
                        PS_OPERATION_EXECUTION_MONOLITHIC_V1,
                        0x4010000000000000ULL),
    make_implementation(
        kHpFastImplementationIdentity, "CANDIDATE_HP_MONO_FAST",
        PS_OPERATION_INTENT_HP_V1, PS_OPERATION_EXECUTION_MONOLITHIC_V1,
        0x3FF0000000000000ULL, PS_OPERATION_BEHAVIOR_DATA_DEPENDENT_V1),
    make_implementation(kRtSlowImplementationIdentity, "CANDIDATE_RT_TILE_SLOW",
                        PS_OPERATION_INTENT_RT_V1,
                        PS_OPERATION_EXECUTION_TILED_V1, 0x4008000000000000ULL),
    make_implementation(kRtFastImplementationIdentity, "CANDIDATE_RT_TILE_FAST",
                        PS_OPERATION_INTENT_RT_V1,
                        PS_OPERATION_EXECUTION_TILED_V1, 0x3FF0000000000000ULL),
    make_implementation(
        kMixedImplementationIdentity, "CANDIDATE_MIXED",
        PS_OPERATION_INTENT_HP_V1 | PS_OPERATION_INTENT_RT_V1,
        PS_OPERATION_EXECUTION_MONOLITHIC_V1 | PS_OPERATION_EXECUTION_TILED_V1,
        0x4000000000000000ULL),
};

/**
 * @brief Creates the valid one-input/one-output conformance operation.
 * @return Complete exact v1 operation descriptor with stable port storage.
 * @throws Nothing.
 * @note Most modes accept the ordinary public built-in version-two Host
 * input. Dedicated input-metadata modes require a preceding conformance
 * output and thereby prove immutable Value metadata survives into the next
 * invocation.
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
  operation.input_ports = make_array_ref(kImageInputPorts, 1U);
  operation.output_ports = make_array_ref(kImageOutputPorts, 1U);
  return operation;
}

/** @brief Stable complete conformance definition. */
const Definition kDefinition{
    kPluginIdentity,
    "operation-conformance-abi1",
    make_operation(),
    kImplementations,
    static_cast<std::uint32_t>(std::size(kImplementations)),
    nullptr,
    nullptr,
};

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
  if (mode_contains("facet_free_input")) {
    output->input_ports = make_array_ref(kGenericInputPorts, 1U);
  }
  if (mode_contains("facet_free_output")) {
    output->output_ports = make_array_ref(kGenericOutputPorts, 1U);
  }
  if (mode_contains("custom_input_identity")) {
    static const ps_operation_port_descriptor_v1 custom_input = [] {
      auto port = make_input_port(false);
      port.schema_identity = make_identity(0x5053435553544F4DULL, 0x1001U);
      return port;
    }();
    output->input_ports = make_array_ref(&custom_input, 1U);
  }
  return status;
}

/**
 * @brief Publishes one legacy row or the complete candidate-set inventory.
 * @param context Borrowed helper context.
 * @param operation Exact parent operation identity.
 * @param count Nonnull Host-owned count destination.
 * @param sink Borrowed Host diagnostic sink.
 * @return Helper status or `OK` with the mode-selected bounded count.
 * @throws Nothing.
 */
ps_operation_status_v1 PS_OPERATION_CALL conformance_implementation_count(
    void* context, const ps_operation_identity_v1* operation,
    std::uint32_t* count, const ps_operation_output_sink_v1* sink) noexcept {
  const ps_operation_status_v1 status =
      get_implementation_count(context, operation, count, sink);
  if (status == PS_OPERATION_STATUS_OK_V1 && !mode_is("candidate_set")) {
    *count = 1U;
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
  if (status == PS_OPERATION_STATUS_OK_V1 && !mode_is("candidate_set")) {
    output->name = make_bytes("SUPERVISED_CONFORMANCE_TILE");
    output->intent_mask = PS_OPERATION_INTENT_HP_V1;
    output->execution_shape_mask = PS_OPERATION_EXECUTION_TILED_V1;
    output->behavior_mask = 0U;
    output->relative_cost_binary64_bits = 0x3FF0000000000000ULL;
    output->execution_mode = PS_OPERATION_EXECUTION_SUPERVISED_PROCESS_V1;
    output->runtime_package_identity = kRuntimePackageIdentity;
  }
  if (status == PS_OPERATION_STATUS_OK_V1 &&
      mode_is("implementation_reserved")) {
    output->reserved[0] = 1U;
  }
  if (status == PS_OPERATION_STATUS_OK_V1 && mode_contains("monolithic")) {
    output->execution_shape_mask = PS_OPERATION_EXECUTION_MONOLITHIC_V1;
  }
  if (status == PS_OPERATION_STATUS_OK_V1 && mode_contains("trusted")) {
    output->execution_mode = PS_OPERATION_EXECUTION_TRUSTED_IN_PROCESS_V1;
    output->runtime_package_identity = ps_operation_identity_v1{};
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
  if (mode_is("candidate_set") &&
      suite_id == PS_OPERATION_SUITE_DEPENDENCY_V1 && requested_version == 1U) {
    const ps_operation_dependency_suite_v1 suite{
        make_suite_header(PS_OPERATION_SUITE_DEPENDENCY_V1),
        build_candidate_dependencies,
        nullptr,
        nullptr,
        nullptr,
        nullptr,
        nullptr,
    };
    return copy_prepared_suite(suite_out, suite,
                               PS_OPERATION_SUITE_DEPENDENCY_V1);
  }
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
  if (suite_id == PS_OPERATION_SUITE_EXECUTION_V1 &&
      !mode_is("suite_reserved")) {
    auto* suite = reinterpret_cast<ps_operation_execution_suite_v1*>(suite_out);
    if (mode_is("candidate_set")) {
      // The complete candidate generation declares both callback shapes.
    } else if (mode_contains("monolithic")) {
      suite->execute_tiled = nullptr;
    } else {
      suite->execute_monolithic = nullptr;
    }
  }
  if (suite_id == PS_OPERATION_SUITE_REGION_V1 && mode_is("candidate_set")) {
    auto* suite = reinterpret_cast<ps_operation_region_suite_v1*>(suite_out);
    suite->propagate_backward = propagate_candidate_backward;
    suite->propagate_forward = propagate_candidate_forward;
  }
  if (suite_id == PS_OPERATION_SUITE_DEFINITION_V1) {
    auto* suite =
        reinterpret_cast<ps_operation_definition_suite_v1*>(suite_out);
    suite->get_operation_count = conformance_operation_count;
    suite->get_operation = conformance_operation;
    suite->get_implementation_count = conformance_implementation_count;
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
