#include <cstddef>
#include <cstdint>
#include <limits>
#include <opencv2/core.hpp>
#include <opencv2/imgcodecs.hpp>
#include <string>
#include <string_view>

#include "photospider/plugin/operation_plugin.hpp"

namespace ps::operation_plugin {
namespace {

/** @brief Permanent save plugin identity. */
constexpr auto kPluginIdentity =
    make_identity(0x5053534156454F50ULL, 0x0001ULL);
/** @brief Permanent save operation identity. */
constexpr auto kOperationIdentity =
    make_identity(0x5053534156454F50ULL, 0x1001ULL);
/** @brief Permanent trusted CPU implementation identity. */
constexpr auto kImplementationIdentity =
    make_identity(0x5053534156454F50ULL, 0x2001ULL);
/** @brief Permanent save configuration-schema identity. */
constexpr auto kConfigurationIdentity =
    make_identity(0x5053534156454F50ULL, 0x3001ULL);
/** @brief Permanent input-port identity. */
constexpr auto kInputIdentity = make_identity(0x5053534156454F50ULL, 0x4001ULL);

/**
 * @brief Creates the stable ordinary DenseImage input port.
 * @return Complete exact input descriptor.
 * @throws Nothing.
 */
ps_operation_port_descriptor_v1 make_input_port() noexcept {
  ps_operation_port_descriptor_v1 port{};
  port.header = make_record_header(PS_OPERATION_PORT_DESCRIPTOR_V1_SIZE,
                                   PS_OPERATION_RECORD_PORT_DESCRIPTOR_V1);
  port.port_identity = kInputIdentity;
  port.index = 0U;
  port.direction = PS_OPERATION_PORT_INPUT_V1;
  port.name = make_bytes("image");
  port.schema_identity = make_identity(0x50534449U, 0x1001U);
  port.facet_identity = make_identity(0x50534449U, 0x1002U);
  port.layout_identity = make_identity(0x50534449U, 0x1003U);
  return port;
}

/** @brief Stable single input port. */
const ps_operation_port_descriptor_v1 kInputPort = make_input_port();

/**
 * @brief Maps one ABI scalar encoding to an OpenCV depth.
 * @param dense Complete DenseTensor descriptor.
 * @return OpenCV depth constant, or negative one when unsupported.
 * @throws Nothing.
 */
int opencv_depth(
    const ps_operation_dense_tensor_descriptor_v1& dense) noexcept {
  if (dense.element_semantics == PS_OPERATION_ELEMENT_UNSIGNED_INTEGER_V1) {
    if (dense.bit_width == 8U) {
      return CV_8U;
    }
    if (dense.bit_width == 16U) {
      return CV_16U;
    }
  }
  if (dense.element_semantics == PS_OPERATION_ELEMENT_SIGNED_INTEGER_V1) {
    if (dense.bit_width == 8U) {
      return CV_8S;
    }
    if (dense.bit_width == 16U) {
      return CV_16S;
    }
    if (dense.bit_width == 32U) {
      return CV_32S;
    }
  }
  if (dense.element_semantics == PS_OPERATION_ELEMENT_FLOATING_POINT_V1) {
    if (dense.bit_width == 32U) {
      return CV_32F;
    }
    if (dense.bit_width == 64U) {
      return CV_64F;
    }
  }
  return -1;
}

/**
 * @brief Returns OK because save declares no Value outputs to plan.
 * @return Stable OK status.
 * @throws Nothing.
 * @note Side-effect execution still receives its complete immutable input and
 * configuration; the Host correctly allocates no output binding.
 */
ps_operation_status_v1 PS_OPERATION_CALL infer_save(
    void*, const ps_operation_invocation_v1*,
    const ps_operation_configuration_view_v1*, const ps_operation_array_ref_v1*,
    const ps_operation_output_sink_v1*) noexcept {
  return PS_OPERATION_STATUS_OK_V1;
}

/**
 * @brief Writes one canonical CPU DenseImage to the configured path.
 * @param configuration Immutable flattened configuration containing `path`.
 * @param inputs Exact payload-available input bindings.
 * @param outputs Must be an exact empty output-binding array.
 * @param sink Host diagnostic sink used by the exception fence.
 * @return Stable ABI status.
 * @throws Nothing across the pure-C callback boundary.
 * @note The callback accepts only current interleaved positive-stride OpenCV
 * layouts. It retains no borrowed pointer and performs no Host publication.
 */
ps_operation_status_v1 PS_OPERATION_CALL
execute_save(void*, const ps_operation_invocation_v1*,
             const ps_operation_configuration_view_v1* configuration,
             const ps_operation_array_ref_v1* inputs,
             const ps_operation_array_ref_v1* outputs,
             const ps_operation_output_sink_v1* sink) noexcept {
  return fence(sink, [&]() -> ps_operation_status_v1 {
    const std::string_view path_view =
        configuration_string(configuration, "path", {});
    if (path_view.empty() || outputs == nullptr || outputs->data != nullptr ||
        outputs->count != 0U || outputs->stride != 0U) {
      return PS_OPERATION_STATUS_INVALID_ARGUMENT_V1;
    }
    const auto* input = array_element<ps_operation_input_binding_v1>(
        inputs, 0U, PS_OPERATION_INPUT_BINDING_V1_SIZE);
    if (input == nullptr || input->value == nullptr ||
        input->value->descriptor == nullptr ||
        input->value->descriptor->dense_tensor == nullptr ||
        input->value->descriptor->image_facet == nullptr ||
        input->value->descriptor->strided_layout == nullptr ||
        input->value->buffers.count != 1U ||
        input->value->buffers.stride != PS_OPERATION_BUFFER_VIEW_V1_SIZE) {
      return PS_OPERATION_STATUS_INVALID_DESCRIPTOR_V1;
    }
    const auto& dense = *input->value->descriptor->dense_tensor;
    const auto& image = *input->value->descriptor->image_facet;
    const auto& layout = *input->value->descriptor->strided_layout;
    const auto& buffer = *static_cast<const ps_operation_buffer_view_v1*>(
        input->value->buffers.data);
    if (dense.rank < 2U || dense.rank > 3U || dense.extents.data == nullptr ||
        dense.extents.count != dense.rank ||
        layout.byte_strides.data == nullptr ||
        layout.byte_strides.count != dense.rank || image.x_axis >= dense.rank ||
        image.y_axis >= dense.rank || image.x_axis == image.y_axis ||
        buffer.cpu_data == nullptr || layout.byte_offset >= buffer.size) {
      return PS_OPERATION_STATUS_INVALID_DESCRIPTOR_V1;
    }
    const auto* extents = static_cast<const std::uint64_t*>(dense.extents.data);
    const auto* strides =
        static_cast<const std::int64_t*>(layout.byte_strides.data);
    std::uint64_t channels = 1U;
    if ((image.presence_mask & PS_OPERATION_IMAGE_HAS_CHANNEL_AXIS_V1) != 0U) {
      if (image.channel_axis >= dense.rank ||
          image.channel_axis == image.x_axis ||
          image.channel_axis == image.y_axis) {
        return PS_OPERATION_STATUS_INVALID_DESCRIPTOR_V1;
      }
      channels = extents[image.channel_axis];
    }
    if (channels == 0U || channels > CV_CN_MAX || extents[image.x_axis] == 0U ||
        extents[image.y_axis] == 0U ||
        extents[image.x_axis] >
            static_cast<std::uint64_t>(std::numeric_limits<int>::max()) ||
        extents[image.y_axis] >
            static_cast<std::uint64_t>(std::numeric_limits<int>::max())) {
      return PS_OPERATION_STATUS_INVALID_DESCRIPTOR_V1;
    }
    const int depth = opencv_depth(dense);
    const std::uint64_t element_bytes = dense.bit_width / 8U;
    if (depth < 0 || element_bytes == 0U ||
        strides[image.x_axis] !=
            static_cast<std::int64_t>(element_bytes * channels) ||
        strides[image.y_axis] <= 0 ||
        (channels > 1U && strides[image.channel_axis] !=
                              static_cast<std::int64_t>(element_bytes))) {
      return PS_OPERATION_STATUS_UNSUPPORTED_V1;
    }
    const std::uint64_t rows = extents[image.y_axis];
    const std::uint64_t row_bytes =
        extents[image.x_axis] * element_bytes * channels;
    const std::uint64_t row_stride =
        static_cast<std::uint64_t>(strides[image.y_axis]);
    if (row_stride < row_bytes ||
        row_bytes > buffer.size - layout.byte_offset ||
        rows - 1U >
            (buffer.size - layout.byte_offset - row_bytes) / row_stride) {
      return PS_OPERATION_STATUS_INVALID_DESCRIPTOR_V1;
    }

    cv::Mat source(static_cast<int>(rows),
                   static_cast<int>(extents[image.x_axis]),
                   CV_MAKETYPE(depth, static_cast<int>(channels)),
                   buffer.cpu_data + layout.byte_offset,
                   static_cast<std::size_t>(row_stride));
    cv::Mat converted;
    source.convertTo(converted, CV_MAKETYPE(CV_16U, source.channels()),
                     65535.0);
    const std::string path(path_view);
    if (!cv::imwrite(path, converted)) {
      return PS_OPERATION_STATUS_INTERNAL_ERROR_V1;
    }
    return PS_OPERATION_STATUS_OK_V1;
  });
}

/** @brief Creates the trusted monolithic side-effect implementation. */
Implementation make_implementation() noexcept {
  Implementation implementation;
  auto& descriptor = implementation.descriptor;
  descriptor.header =
      make_record_header(PS_OPERATION_IMPLEMENTATION_DESCRIPTOR_V1_SIZE,
                         PS_OPERATION_RECORD_IMPLEMENTATION_DESCRIPTOR_V1);
  descriptor.implementation_identity = kImplementationIdentity;
  descriptor.operation_identity = kOperationIdentity;
  descriptor.name = make_bytes("cpu-side-effect");
  descriptor.intent_mask = PS_OPERATION_INTENT_HP_V1;
  descriptor.execution_shape_mask = PS_OPERATION_EXECUTION_MONOLITHIC_V1;
  descriptor.device_kind = PS_OPERATION_DEVICE_CPU_V1;
  descriptor.behavior_mask = PS_OPERATION_BEHAVIOR_SIDE_EFFECT_V1;
  descriptor.input_access_mask = PS_OPERATION_ACCESS_READ_V1;
  descriptor.reentrant = 1U;
  descriptor.relative_cost_binary64_bits = 0x3FF0000000000000ULL;
  descriptor.execution_mode = PS_OPERATION_EXECUTION_TRUSTED_IN_PROCESS_V1;
  implementation.infer = infer_save;
  implementation.execute_monolithic = execute_save;
  return implementation;
}

/** @brief Stable trusted implementation row. */
const Implementation kImplementations[]{make_implementation()};

/** @brief Creates the complete immutable save operation definition. */
ps_operation_descriptor_v1 make_operation() noexcept {
  ps_operation_descriptor_v1 operation{};
  operation.header =
      make_record_header(PS_OPERATION_DESCRIPTOR_V1_SIZE,
                         PS_OPERATION_RECORD_OPERATION_DESCRIPTOR_V1);
  operation.operation_identity = kOperationIdentity;
  operation.type = make_bytes("io");
  operation.subtype = make_bytes("save");
  operation.display_name = make_bytes("Save image");
  operation.configuration_schema_identity = kConfigurationIdentity;
  operation.input_ports = make_array_ref(&kInputPort, 1U);
  operation.output_ports = empty_array_ref();
  return operation;
}

/** @brief Stable complete save plugin definition. */
const Definition kDefinition{kPluginIdentity,
                             "repository-save-abi1",
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
