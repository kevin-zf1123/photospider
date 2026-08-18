#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>

#include "photospider/plugin/operation_plugin.hpp"

namespace ps::operation_plugin {
namespace {

/** @brief Permanent fixture plugin identity. */
constexpr auto kPluginIdentity{
    make_identity(0x50535245504C4143ULL, 0x0001ULL),
};
/** @brief Permanent resize operation identity. */
constexpr auto kOperationIdentity{
    make_identity(0x5053524553495A45ULL, 0x0001ULL),
};
/** @brief Permanent trusted implementation identity. */
constexpr auto kImplementationIdentity{
    make_identity(0x5053524553495A45ULL, 0x1001ULL),
};
/** @brief Permanent configuration-schema identity. */
constexpr auto kConfigurationIdentity{
    make_identity(0x5053524553495A45ULL, 0x2001ULL),
};
/** @brief Permanent input-port identity. */
constexpr auto kInputPortIdentity{
    make_identity(0x5053524553495A45ULL, 0x3001ULL),
};
/** @brief Permanent output-port identity. */
constexpr auto kOutputPortIdentity{
    make_identity(0x5053524553495A45ULL, 0x3002ULL),
};
/** @brief Stable built-in Image Region-domain identity. */
constexpr auto kImageRegionDomainIdentity{
    make_identity(0x50484F544F535049ULL, 0x4445525F494D4731ULL),
};

/**
 * @brief Creates one immutable DenseImage port definition.
 * @param identity Permanent port identity.
 * @param direction Input or output direction.
 * @return Complete exact port record.
 * @throws Nothing.
 */
ps_operation_port_descriptor_v1 make_port(
    ps_operation_identity_v1 identity,
    ps_operation_port_direction_v1 direction) noexcept {
  ps_operation_port_descriptor_v1 port{};
  port.header = make_record_header(PS_OPERATION_PORT_DESCRIPTOR_V1_SIZE,
                                   PS_OPERATION_RECORD_PORT_DESCRIPTOR_V1);
  port.port_identity = identity;
  port.index = 0U;
  port.direction = direction;
  port.name = make_bytes("image");
  port.schema_identity = make_identity(0x50534449U, 0x1001U);
  port.facet_identity = make_identity(0x50534449U, 0x1002U);
  port.layout_identity = make_identity(0x50534449U, 0x1003U);
  return port;
}

/** @brief Stable input/output port rows. */
const ps_operation_port_descriptor_v1 kPorts[]{
    make_port(kInputPortIdentity, PS_OPERATION_PORT_INPUT_V1),
    make_port(kOutputPortIdentity, PS_OPERATION_PORT_OUTPUT_V1),
};

/**
 * @brief Emits the fixture's fixed 3-by-2 FP32 output plan.
 * @param inputs Host-projected immutable input bindings.
 * @param sink Host output-plan sink.
 * @return Stable ABI status.
 * @throws Nothing; all arithmetic failures become a status.
 * @note Nested records borrow input facets only through the synchronous sink
 * call; the Host deep-copies and validates every field before allocation.
 */
ps_operation_status_v1 emit_fixed_plan(
    const ps_operation_array_ref_v1* inputs,
    const ps_operation_output_sink_v1* sink) noexcept {
  return fence(sink, [&]() -> ps_operation_status_v1 {
    const auto* input = array_element<ps_operation_input_binding_v1>(
        inputs, 0U, PS_OPERATION_INPUT_BINDING_V1_SIZE);
    if (input == nullptr || input->value == nullptr ||
        input->value->descriptor == nullptr ||
        input->value->descriptor->dense_tensor == nullptr ||
        input->value->descriptor->image_facet == nullptr ||
        input->value->descriptor->strided_layout == nullptr ||
        sink == nullptr || sink->emit == nullptr) {
      return PS_OPERATION_STATUS_INVALID_DESCRIPTOR_V1;
    }
    const auto& source_dense = *input->value->descriptor->dense_tensor;
    const auto& source_image = *input->value->descriptor->image_facet;
    if (source_dense.rank == 0U ||
        source_dense.rank > PS_OPERATION_MAX_RANK_V1 ||
        source_dense.extents.data == nullptr ||
        source_dense.extents.count != source_dense.rank ||
        source_image.x_axis >= source_dense.rank ||
        source_image.y_axis >= source_dense.rank ||
        source_dense.element_semantics !=
            PS_OPERATION_ELEMENT_FLOATING_POINT_V1 ||
        source_dense.bit_width != 32U) {
      return PS_OPERATION_STATUS_UNSUPPORTED_V1;
    }

    std::array<std::uint64_t, PS_OPERATION_MAX_RANK_V1> extents{};
    std::array<std::int64_t, PS_OPERATION_MAX_RANK_V1> strides{};
    const auto* source_extents =
        static_cast<const std::uint64_t*>(source_dense.extents.data);
    for (std::uint32_t axis = 0U; axis < source_dense.rank; ++axis) {
      extents[axis] = source_extents[axis];
    }
    extents[source_image.x_axis] = 3U;
    extents[source_image.y_axis] = 2U;
    std::uint64_t storage_size = sizeof(float);
    for (std::uint32_t reverse = source_dense.rank; reverse > 0U; --reverse) {
      const std::uint32_t axis = reverse - 1U;
      if (storage_size > static_cast<std::uint64_t>(
                             std::numeric_limits<std::int64_t>::max())) {
        return PS_OPERATION_STATUS_INVALID_DESCRIPTOR_V1;
      }
      strides[axis] = static_cast<std::int64_t>(storage_size);
      if (extents[axis] == 0U ||
          storage_size >
              std::numeric_limits<std::uint64_t>::max() / extents[axis]) {
        return PS_OPERATION_STATUS_INVALID_DESCRIPTOR_V1;
      }
      storage_size *= extents[axis];
    }

    ps_operation_dense_tensor_descriptor_v1 dense = source_dense;
    dense.extents = make_array_ref(extents.data(), source_dense.rank);
    dense.quantization_present = 0U;
    dense.quantization_block_shape = empty_array_ref();
    dense.quantization_scales_binary32 = empty_array_ref();

    constexpr std::int64_t kOutputWidth = 3;
    constexpr std::int64_t kOutputHeight = 2;
    const auto maximum = std::numeric_limits<std::int64_t>::max();
    if (source_image.data_window.x_begin > maximum - kOutputWidth ||
        source_image.data_window.y_begin > maximum - kOutputHeight ||
        (((source_image.presence_mask &
           PS_OPERATION_IMAGE_HAS_DISPLAY_WINDOW_V1) != 0U) &&
         (source_image.display_window.x_begin > maximum - kOutputWidth ||
          source_image.display_window.y_begin > maximum - kOutputHeight))) {
      return PS_OPERATION_STATUS_INVALID_DESCRIPTOR_V1;
    }
    ps_operation_image_facet_v1 image = source_image;
    image.data_window.x_end = image.data_window.x_begin + kOutputWidth;
    image.data_window.y_end = image.data_window.y_begin + kOutputHeight;
    if ((image.presence_mask & PS_OPERATION_IMAGE_HAS_DISPLAY_WINDOW_V1) !=
        0U) {
      image.display_window.x_end = image.display_window.x_begin + kOutputWidth;
      image.display_window.y_end = image.display_window.y_begin + kOutputHeight;
    }

    ps_operation_strided_layout_v1 layout =
        *input->value->descriptor->strided_layout;
    layout.byte_offset = 0U;
    layout.storage_size = storage_size;
    layout.byte_strides = make_array_ref(strides.data(), source_dense.rank);

    ps_operation_value_descriptor_v1 descriptor = *input->value->descriptor;
    descriptor.dense_tensor = &dense;
    descriptor.image_facet = &image;
    descriptor.strided_layout = &layout;

    std::array<ps_operation_axis_range_v1, 2U> ranges{
        ps_operation_axis_range_v1{image.data_window.x_begin, 3U},
        ps_operation_axis_range_v1{image.data_window.y_begin, 2U}};
    ps_operation_region_atom_v1 atom{};
    atom.header = make_record_header(PS_OPERATION_REGION_ATOM_V1_SIZE,
                                     PS_OPERATION_RECORD_REGION_ATOM_V1);
    atom.atom_kind = PS_OPERATION_REGION_ATOM_IMAGE_RECT_V1;
    atom.rank = 2U;
    atom.domain_identity = kImageRegionDomainIdentity;
    atom.axis_ranges = make_array_ref(ranges.data(), ranges.size());
    ps_operation_region_set_view_v1 region{};
    region.header = make_record_header(PS_OPERATION_REGION_SET_VIEW_V1_SIZE,
                                       PS_OPERATION_RECORD_REGION_SET_VIEW_V1);
    region.set_kind = PS_OPERATION_REGION_SET_CLAUSE_V1;
    region.atoms = make_array_ref(&atom, 1U);

    ps_operation_output_buffer_plan_v1 buffer{};
    buffer.header =
        make_record_header(PS_OPERATION_OUTPUT_BUFFER_PLAN_V1_SIZE,
                           PS_OPERATION_RECORD_OUTPUT_BUFFER_PLAN_V1);
    buffer.buffer_index = 0U;
    buffer.access_mask = PS_OPERATION_ACCESS_WRITE_V1;
    buffer.byte_size = storage_size;
    buffer.alignment = 64U;
    ps_operation_output_plan_v1 plan{};
    plan.header = make_record_header(PS_OPERATION_OUTPUT_PLAN_V1_SIZE,
                                     PS_OPERATION_RECORD_OUTPUT_PLAN_V1);
    plan.port_identity = kOutputPortIdentity;
    plan.port_index = 0U;
    plan.buffer_count = 1U;
    plan.descriptor = &descriptor;
    plan.buffers = make_array_ref(&buffer, 1U);
    plan.full_region = &region;
    plan.access_mask = PS_OPERATION_ACCESS_WRITE_V1;
    return sink->emit(sink->host_context, PS_OPERATION_OUTPUT_PLAN_V1, &plan,
                      1U, PS_OPERATION_OUTPUT_PLAN_V1_SIZE);
  });
}

/**
 * @brief Handles output-plan inference for the replacement fixture.
 * @param inputs Immutable input bindings.
 * @param sink Host output-plan sink.
 * @return Stable ABI status.
 * @throws Nothing.
 */
ps_operation_status_v1 PS_OPERATION_CALL
infer_resize(void*, const ps_operation_invocation_v1*,
             const ps_operation_configuration_view_v1*,
             const ps_operation_array_ref_v1* inputs,
             const ps_operation_output_sink_v1* sink) noexcept {
  return emit_fixed_plan(inputs, sink);
}

/**
 * @brief Fills the complete Host-owned output grant with the sentinel value.
 * @param outputs Exact mutable output binding array.
 * @param sink Host diagnostic sink used by the exception fence.
 * @return Stable ABI status.
 * @throws Nothing.
 * @note The callback writes only callback-scoped Host-granted spans and retains
 * no pointer after return.
 */
ps_operation_status_v1 PS_OPERATION_CALL execute_resize(
    void*, const ps_operation_invocation_v1*,
    const ps_operation_configuration_view_v1*, const ps_operation_array_ref_v1*,
    const ps_operation_array_ref_v1* outputs,
    const ps_operation_output_sink_v1* sink) noexcept {
  return fence(sink, [&]() -> ps_operation_status_v1 {
    const auto* output = array_element<ps_operation_mutable_output_binding_v1>(
        outputs, 0U, PS_OPERATION_MUTABLE_OUTPUT_BINDING_V1_SIZE);
    if (output == nullptr || output->spans.count != 1U ||
        output->spans.stride != PS_OPERATION_OUTPUT_GRANT_SPAN_V1_SIZE ||
        output->spans.data == nullptr) {
      return PS_OPERATION_STATUS_INVALID_DESCRIPTOR_V1;
    }
    const auto& span = *static_cast<const ps_operation_output_grant_span_v1*>(
        output->spans.data);
    if (span.bytes.data == nullptr || span.bytes.size != 6U * sizeof(float)) {
      return PS_OPERATION_STATUS_INVALID_DESCRIPTOR_V1;
    }
    constexpr float kValue = 0.625F;
    for (std::size_t index = 0U; index < 6U; ++index) {
      std::memcpy(span.bytes.data + index * sizeof(float), &kValue,
                  sizeof(kValue));
    }
    return PS_OPERATION_STATUS_OK_V1;
  });
}

/** @brief Creates the trusted replacement implementation record. */
Implementation make_implementation() noexcept {
  Implementation implementation;
  auto& descriptor = implementation.descriptor;
  descriptor.header =
      make_record_header(PS_OPERATION_IMPLEMENTATION_DESCRIPTOR_V1_SIZE,
                         PS_OPERATION_RECORD_IMPLEMENTATION_DESCRIPTOR_V1);
  descriptor.implementation_identity = kImplementationIdentity;
  descriptor.operation_identity = kOperationIdentity;
  descriptor.name = make_bytes("STDLIB_RESIZE_REPLACEMENT");
  descriptor.intent_mask = PS_OPERATION_INTENT_HP_V1;
  descriptor.execution_shape_mask = PS_OPERATION_EXECUTION_MONOLITHIC_V1;
  descriptor.device_kind = PS_OPERATION_DEVICE_CPU_V1;
  descriptor.input_access_mask = PS_OPERATION_ACCESS_READ_V1;
  descriptor.output_access_mask = PS_OPERATION_ACCESS_WRITE_V1;
  descriptor.reentrant = 1U;
  descriptor.relative_cost_binary64_bits = 0x3FF0000000000000ULL;
  descriptor.execution_mode = PS_OPERATION_EXECUTION_TRUSTED_IN_PROCESS_V1;
  implementation.infer = infer_resize;
  implementation.execute_monolithic = execute_resize;
  return implementation;
}

/** @brief Stable implementation row. */
const Implementation kImplementations[]{make_implementation()};

/** @brief Creates the complete immutable resize operation definition. */
ps_operation_descriptor_v1 make_operation() noexcept {
  ps_operation_descriptor_v1 operation{};
  operation.header =
      make_record_header(PS_OPERATION_DESCRIPTOR_V1_SIZE,
                         PS_OPERATION_RECORD_OPERATION_DESCRIPTOR_V1);
  operation.operation_identity = kOperationIdentity;
  operation.type = make_bytes("image_process");
  operation.subtype = make_bytes("resize");
  operation.display_name = make_bytes("Resize replacement fixture");
  operation.configuration_schema_identity = kConfigurationIdentity;
  operation.input_ports = make_array_ref(kPorts, 1U);
  operation.output_ports = make_array_ref(kPorts + 1U, 1U);
  return operation;
}

/** @brief Stable complete fixture definition. */
const Definition kDefinition{
    kPluginIdentity,
    "resize-replacement-abi1",
    make_operation(),
    kImplementations,
    1U,
    nullptr,
    nullptr,
};

}  // namespace

/** @copydoc plugin_definition */
const Definition& plugin_definition() noexcept {
  return kDefinition;
}

}  // namespace ps::operation_plugin

PS_DEFINE_OPERATION_PLUGIN_V1()
