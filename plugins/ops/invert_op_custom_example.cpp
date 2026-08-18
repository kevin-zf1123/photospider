#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>
#include <stdexcept>

#include "photospider/plugin/operation_plugin.hpp"

namespace ps::operation_plugin {
namespace {

constexpr ps_operation_identity_v1 kPluginIdentity{0x5053494E56455254ULL, 1U};
constexpr ps_operation_identity_v1 kOperationIdentity{0x50534F50494E5654ULL,
                                                      1U};
constexpr ps_operation_identity_v1 kImplementationIdentity{
    0x5053494D50494E56ULL, 1U};
constexpr ps_operation_identity_v1 kConfigurationIdentity{0x5053434647494E56ULL,
                                                          1U};
constexpr ps_operation_identity_v1 kInputIdentity{0x5053504F5254494EULL, 1U};
constexpr ps_operation_identity_v1 kOutputIdentity{0x5053504F52544F55ULL, 1U};
constexpr ps_operation_identity_v1 kDenseSchema{0x50534449U, 0x1001U};
constexpr ps_operation_identity_v1 kImageFacet{0x50534449U, 0x1002U};
constexpr ps_operation_identity_v1 kStridedLayout{0x50534449U, 0x1003U};

/**
 * @brief Creates one stable image port descriptor.
 * @param identity Permanent port identity.
 * @param direction Input or output direction.
 * @return Complete ABI-v1 port row.
 * @throws Nothing.
 */
ps_operation_port_descriptor_v1 make_image_port(
    ps_operation_identity_v1 identity,
    ps_operation_port_direction_v1 direction) noexcept {
  ps_operation_port_descriptor_v1 port{};
  port.header = make_record_header(PS_OPERATION_PORT_DESCRIPTOR_V1_SIZE,
                                   PS_OPERATION_RECORD_PORT_DESCRIPTOR_V1);
  port.port_identity = identity;
  port.index = 0U;
  port.direction = direction;
  port.name = make_bytes("image");
  port.schema_identity = kDenseSchema;
  port.facet_identity = kImageFacet;
  port.layout_identity = kStridedLayout;
  return port;
}

/** @brief Stable one-input/one-output port records returned by Definition. */
const ps_operation_port_descriptor_v1 kPorts[] = {
    make_image_port(kInputIdentity, PS_OPERATION_PORT_INPUT_V1),
    make_image_port(kOutputIdentity, PS_OPERATION_PORT_OUTPUT_V1)};

/**
 * @brief Emits an immutable pass-through plan for pointwise inversion.
 * @param inputs Payload-free input records.
 * @param sink Host plan sink.
 * @return Stable ABI status.
 * @throws Nothing.
 */
ps_operation_status_v1 PS_OPERATION_CALL
infer_invert(void*, const ps_operation_invocation_v1*,
             const ps_operation_configuration_view_v1*,
             const ps_operation_array_ref_v1* inputs,
             const ps_operation_output_sink_v1* sink) noexcept {
  return emit_passthrough_image_plan(inputs, sink);
}

/**
 * @brief Computes a checked signed byte offset for one logical ordinal.
 * @param dense Complete tensor descriptor.
 * @param layout Complete signed Strided layout.
 * @param ordinal Dense row-major element ordinal.
 * @return Signed byte offset from the allocation base.
 * @throws std::invalid_argument or std::overflow_error for malformed facts.
 */
std::int64_t logical_offset(
    const ps_operation_dense_tensor_descriptor_v1& dense,
    const ps_operation_strided_layout_v1& layout, std::uint64_t ordinal) {
  if (dense.extents.count != dense.rank ||
      layout.byte_strides.count != dense.rank ||
      dense.extents.data == nullptr || layout.byte_strides.data == nullptr ||
      layout.byte_offset > static_cast<std::uint64_t>(
                               std::numeric_limits<std::int64_t>::max())) {
    throw std::invalid_argument("invert descriptor rank mismatch");
  }
  const auto* extents = static_cast<const std::uint64_t*>(dense.extents.data);
  const auto* strides =
      static_cast<const std::int64_t*>(layout.byte_strides.data);
  std::int64_t offset = static_cast<std::int64_t>(layout.byte_offset);
  for (std::size_t axis = dense.rank; axis-- > 0U;) {
    if (extents[axis] == 0U) {
      throw std::invalid_argument("invert descriptor has zero extent");
    }
    const std::uint64_t coordinate = ordinal % extents[axis];
    ordinal /= extents[axis];
    if (coordinate >
        static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max())) {
      throw std::overflow_error("invert coordinate exceeds int64");
    }
    const std::int64_t signed_coordinate =
        static_cast<std::int64_t>(coordinate);
    const std::int64_t stride = strides[axis];
    if ((stride > 0 && signed_coordinate >
                           std::numeric_limits<std::int64_t>::max() / stride) ||
        (stride < 0 &&
         (stride == std::numeric_limits<std::int64_t>::min() ||
          signed_coordinate >
              std::numeric_limits<std::int64_t>::max() / -stride))) {
      throw std::overflow_error("invert stride multiplication overflow");
    }
    const std::int64_t term = signed_coordinate * stride;
    if ((term > 0 &&
         offset > std::numeric_limits<std::int64_t>::max() - term) ||
        (term < 0 &&
         offset < std::numeric_limits<std::int64_t>::min() - term)) {
      throw std::overflow_error("invert stride addition overflow");
    }
    offset += term;
  }
  return offset;
}

/**
 * @brief Executes stride-aware pointwise inversion through ABI records.
 * @param inputs Exact payload-available input bindings.
 * @param outputs Exact Host-owned mutable output bindings.
 * @param sink Host diagnostic sink used by the exception fence.
 * @return Stable ABI status.
 * @throws Nothing across the callback boundary.
 */
ps_operation_status_v1 PS_OPERATION_CALL
execute_invert(void*, const ps_operation_invocation_v1*,
               const ps_operation_configuration_view_v1*,
               const ps_operation_array_ref_v1* inputs,
               const ps_operation_array_ref_v1* outputs,
               const ps_operation_output_sink_v1* sink) noexcept {
  return fence(sink, [&]() -> ps_operation_status_v1 {
    const auto* input = array_element<ps_operation_input_binding_v1>(
        inputs, 0U, PS_OPERATION_INPUT_BINDING_V1_SIZE);
    const auto* output = array_element<ps_operation_mutable_output_binding_v1>(
        outputs, 0U, PS_OPERATION_MUTABLE_OUTPUT_BINDING_V1_SIZE);
    if (input == nullptr || input->value == nullptr || output == nullptr ||
        input->value->descriptor == nullptr ||
        input->value->descriptor->dense_tensor == nullptr ||
        input->value->descriptor->strided_layout == nullptr ||
        input->value->buffers.count != 1U || output->spans.count != 1U) {
      return PS_OPERATION_STATUS_INVALID_DESCRIPTOR_V1;
    }
    const auto* input_buffer = static_cast<const ps_operation_buffer_view_v1*>(
        input->value->buffers.data);
    const auto* output_span =
        static_cast<const ps_operation_output_grant_span_v1*>(
            output->spans.data);
    if (input_buffer->cpu_data == nullptr ||
        output_span->bytes.data == nullptr ||
        input_buffer->size != output_span->bytes.size) {
      return PS_OPERATION_STATUS_INVALID_DESCRIPTOR_V1;
    }
    std::memcpy(output_span->bytes.data, input_buffer->cpu_data,
                static_cast<std::size_t>(input_buffer->size));
    const auto& dense = *input->value->descriptor->dense_tensor;
    const auto& layout = *input->value->descriptor->strided_layout;
    const auto* extents = static_cast<const std::uint64_t*>(dense.extents.data);
    std::uint64_t count = 1U;
    for (std::uint32_t axis = 0U; axis < dense.rank; ++axis) {
      if (extents[axis] == 0U ||
          count > std::numeric_limits<std::uint64_t>::max() / extents[axis]) {
        return PS_OPERATION_STATUS_INVALID_DESCRIPTOR_V1;
      }
      count *= extents[axis];
    }
    for (std::uint64_t ordinal = 0U; ordinal < count; ++ordinal) {
      const std::int64_t offset = logical_offset(dense, layout, ordinal);
      const std::uint64_t width = dense.bit_width / 8U;
      if (offset < 0 || width == 0U || input_buffer->size < width ||
          static_cast<std::uint64_t>(offset) > input_buffer->size - width) {
        return PS_OPERATION_STATUS_INVALID_DESCRIPTOR_V1;
      }
      std::uint8_t* destination = output_span->bytes.data + offset;
      const std::uint8_t* source = input_buffer->cpu_data + offset;
      if (dense.element_semantics == PS_OPERATION_ELEMENT_UNSIGNED_INTEGER_V1 &&
          dense.bit_width == 8U) {
        *destination = static_cast<std::uint8_t>(255U - *source);
      } else if (dense.element_semantics ==
                     PS_OPERATION_ELEMENT_FLOATING_POINT_V1 &&
                 dense.bit_width == 32U) {
        float value = 0.0F;
        std::memcpy(&value, source, sizeof(value));
        value = 1.0F - value;
        std::memcpy(destination, &value, sizeof(value));
      } else if (dense.element_semantics ==
                     PS_OPERATION_ELEMENT_FLOATING_POINT_V1 &&
                 dense.bit_width == 64U) {
        double value = 0.0;
        std::memcpy(&value, source, sizeof(value));
        value = 1.0 - value;
        std::memcpy(destination, &value, sizeof(value));
      } else {
        return PS_OPERATION_STATUS_UNSUPPORTED_V1;
      }
    }
    return PS_OPERATION_STATUS_OK_V1;
  });
}

/** @brief Creates the single trusted CPU implementation row. */
Implementation make_implementation() noexcept {
  Implementation implementation;
  auto& descriptor = implementation.descriptor;
  descriptor.header =
      make_record_header(PS_OPERATION_IMPLEMENTATION_DESCRIPTOR_V1_SIZE,
                         PS_OPERATION_RECORD_IMPLEMENTATION_DESCRIPTOR_V1);
  descriptor.implementation_identity = kImplementationIdentity;
  descriptor.operation_identity = kOperationIdentity;
  descriptor.name = make_bytes("cpu");
  descriptor.intent_mask = PS_OPERATION_INTENT_HP_V1;
  descriptor.execution_shape_mask = PS_OPERATION_EXECUTION_MONOLITHIC_V1;
  descriptor.device_kind = PS_OPERATION_DEVICE_CPU_V1;
  descriptor.input_access_mask = PS_OPERATION_ACCESS_READ_V1;
  descriptor.output_access_mask = PS_OPERATION_ACCESS_WRITE_V1;
  descriptor.reentrant = 1U;
  descriptor.relative_cost_binary64_bits = 0x3FF0000000000000ULL;
  descriptor.execution_mode = PS_OPERATION_EXECUTION_TRUSTED_IN_PROCESS_V1;
  implementation.infer = infer_invert;
  implementation.execute_monolithic = execute_invert;
  return implementation;
}

/** @brief Stable single implementation array. */
const Implementation kImplementations[] = {make_implementation()};

/** @brief Creates the complete immutable operation descriptor. */
ps_operation_descriptor_v1 make_operation() noexcept {
  ps_operation_descriptor_v1 operation{};
  operation.header =
      make_record_header(PS_OPERATION_DESCRIPTOR_V1_SIZE,
                         PS_OPERATION_RECORD_OPERATION_DESCRIPTOR_V1);
  operation.operation_identity = kOperationIdentity;
  operation.type = make_bytes("image_process");
  operation.subtype = make_bytes("invert");
  operation.display_name = make_bytes("Invert");
  operation.configuration_schema_identity = kConfigurationIdentity;
  operation.input_ports = make_array_ref(kPorts, 1U);
  operation.output_ports = make_array_ref(kPorts + 1U, 1U);
  return operation;
}

/** @brief Stable complete plugin definition. */
const Definition kDefinition{kPluginIdentity,
                             "repository-invert-abi1",
                             make_operation(),
                             kImplementations,
                             1U,
                             nullptr,
                             nullptr};

}  // namespace

/** @copydoc plugin_definition */
const Definition& plugin_definition() noexcept {
  return kDefinition;
}

}  // namespace ps::operation_plugin

PS_DEFINE_OPERATION_PLUGIN_V1()
