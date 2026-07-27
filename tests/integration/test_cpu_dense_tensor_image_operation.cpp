#include <gtest/gtest.h>

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <stdexcept>
#include <utility>
#include <variant>
#include <vector>

#include "compute/node_executor.hpp"
#include "core/cpu_dense_image_operation.hpp"
#include "core/ops.hpp"
#include "graph/graph_model.hpp"  // NOLINT(build/include_subdir)
#include "graph/node.hpp"         // NOLINT(build/include_subdir)
#include "photospider/core/compute_intent.hpp"
#include "photospider/core/graph_error.hpp"
#include "photospider/core/image_buffer.hpp"
#include "photospider/data/image_view.hpp"

namespace ps {
namespace {

/**
 * @brief Creates one valid padded unsigned-8 HWC Value for test inspection.
 *
 * @param width Positive image width.
 * @param height Positive image height.
 * @param channels Positive channel count.
 * @param row_stride Positive row stride at least width times channels.
 * @return Immutable Value whose active bytes increase from one.
 * @throws std::invalid_argument from Value validation for invalid arguments.
 * @throws std::bad_alloc when shape, layout, or storage allocation fails.
 * @note Inter-row padding is initialized to 0xA5 and is never an active
 *       element.
 */
Value make_unsigned8_value(std::size_t width, std::size_t height,
                           std::size_t channels, std::size_t row_stride) {
  const std::size_t row_bytes = width * channels;
  const std::size_t storage_size = (height - 1U) * row_stride + row_bytes;
  std::vector<std::byte> storage(storage_size, std::byte{0xA5});
  std::uint8_t next = 1U;
  for (std::size_t y = 0U; y < height; ++y) {
    for (std::size_t x = 0U; x < width; ++x) {
      for (std::size_t channel = 0U; channel < channels; ++channel) {
        storage[y * row_stride + x * channels + channel] = std::byte{next++};
      }
    }
  }

  DenseTensorDescriptor descriptor{{height, width, channels},
                                   ElementSemantics::UnsignedInteger,
                                   StorageEncoding{8U}};
  ImageFacet image;
  image.x_axis = 1U;
  image.y_axis = 0U;
  image.channel_axis = 2U;
  StridedLayout layout{{static_cast<std::ptrdiff_t>(row_stride),
                        static_cast<std::ptrdiff_t>(channels), 1}};
  return Value::from_cpu_dense_tensor(std::move(descriptor), image,
                                      std::move(layout), std::move(storage));
}

/**
 * @brief Fills active bytes of a current ImageBuffer with a known sequence.
 *
 * @param buffer Valid writable unsigned-8 CPU buffer.
 * @return Flat active byte sequence in row-major interleaved order.
 * @throws std::bad_alloc when expected-byte storage allocation fails.
 * @note Every allocation byte is first set to 0xA5 so padding differs from all
 *       expected inverted active bytes.
 */
std::vector<std::uint8_t> fill_unsigned8_image(ImageBuffer* buffer) {
  std::memset(buffer->data.get(), 0xA5,
              buffer->step * static_cast<std::size_t>(buffer->height));
  std::vector<std::uint8_t> expected;
  expected.reserve(static_cast<std::size_t>(buffer->width) *
                   static_cast<std::size_t>(buffer->height) *
                   static_cast<std::size_t>(buffer->channels));
  std::uint8_t next = 0U;
  auto* base = static_cast<std::byte*>(buffer->data.get());
  for (int y = 0; y < buffer->height; ++y) {
    for (int x = 0; x < buffer->width; ++x) {
      for (int channel = 0; channel < buffer->channels; ++channel) {
        base[static_cast<std::size_t>(y) * buffer->step +
             static_cast<std::size_t>(x * buffer->channels + channel)] =
            std::byte{next};
        expected.push_back(next);
        next = static_cast<std::uint8_t>(next + 17U);
      }
    }
  }
  return expected;
}

/**
 * @brief Reads active unsigned-8 bytes from a validated current ImageBuffer.
 *
 * @param buffer Valid CPU UINT8 image.
 * @return Flat active byte sequence in row-major interleaved order.
 * @throws std::bad_alloc when result allocation fails.
 * @note Row padding is skipped through image_buffer_row_data and row_bytes.
 */
std::vector<std::uint8_t> read_unsigned8_image(const ImageBuffer& buffer) {
  std::vector<std::uint8_t> result;
  const std::size_t row_bytes = image_buffer_row_bytes(buffer);
  result.reserve(row_bytes * static_cast<std::size_t>(buffer.height));
  for (int y = 0; y < buffer.height; ++y) {
    const std::byte* row = image_buffer_row_data(buffer, y);
    for (std::size_t index = 0U; index < row_bytes; ++index) {
      result.push_back(std::to_integer<std::uint8_t>(row[index]));
    }
  }
  return result;
}

TEST(CpuDenseTensorImageOperation,
     ValueRejectsMalformedFacetStrideAndEnvelope) {
  DenseTensorDescriptor descriptor{{2U, 3U, 2U},
                                   ElementSemantics::UnsignedInteger,
                                   StorageEncoding{8U}};
  ImageFacet image;
  image.x_axis = 1U;
  image.y_axis = 0U;
  image.channel_axis = 2U;

  StridedLayout zero_stride{{8, 2, 0}};
  EXPECT_THROW(
      Value::from_cpu_dense_tensor(descriptor, image, zero_stride,
                                   std::vector<std::byte>(14U, std::byte{0})),
      std::invalid_argument);

  StridedLayout padded{{8, 2, 1}};
  EXPECT_THROW(
      Value::from_cpu_dense_tensor(descriptor, image, padded,
                                   std::vector<std::byte>(13U, std::byte{0})),
      std::invalid_argument);

  ImageFacet duplicate_axis = image;
  duplicate_axis.channel_axis = duplicate_axis.x_axis;
  EXPECT_THROW(
      Value::from_cpu_dense_tensor(descriptor, duplicate_axis, padded,
                                   std::vector<std::byte>(14U, std::byte{0})),
      std::invalid_argument);
}

TEST(CpuDenseTensorImageOperation,
     ValueCopiesShareBytesAndViewsRetainLifetime) {
  Value original = make_unsigned8_value(3U, 2U, 2U, 8U);
  Value shared = original;
  EXPECT_EQ(original.data(), shared.data());
  EXPECT_EQ(original.storage_size(), 14U);

  ImageView image = [&original]() {
    ImageView retaining(original);
    return retaining;
  }();
  original = Value{};
  shared = Value{};

  EXPECT_EQ(image.width(), 3U);
  EXPECT_EQ(image.height(), 2U);
  EXPECT_EQ(image.channels(), 2U);
  EXPECT_EQ(image.row_stride(), 8);
  EXPECT_EQ(std::to_integer<std::uint8_t>(*image.channel_data(2U, 1U, 1U)),
            12U);
  EXPECT_THROW(image.channel_data(3U, 0U, 0U), std::out_of_range);
}

TEST(CpuDenseTensorImageOperation,
     DenseInvertInferencePreservesExactLogicalDescriptor) {
  const ops::CpuDenseImageOperation operation =
      ops::make_dense_invert_operation();
  ops::DenseImageDescriptor input;
  input.tensor = DenseTensorDescriptor{{4U, 7U, 3U},
                                       ElementSemantics::UnsignedInteger,
                                       StorageEncoding{8U}};
  input.image.x_axis = 1U;
  input.image.y_axis = 0U;
  input.image.channel_axis = 2U;
  ops::CpuDenseImageConfiguration configuration;

  const ops::DenseImageDescriptor inferred =
      operation.infer(configuration, {input});
  EXPECT_EQ(inferred, input);
}

TEST(CpuDenseTensorImageOperation,
     ProductRegistryAndExecutorInvertPaddedMultiChannelInput) {
  ops::register_core_operations();
  const auto resolved = OpRegistry::instance().resolve_for_intent(
      "image_process", "invert_dense", ComputeIntent::GlobalHighPrecision);
  ASSERT_TRUE(resolved.has_value());
  ASSERT_TRUE(std::holds_alternative<MonolithicOpFunc>(*resolved));

  NodeOutput input;
  input.image_buffer = make_aligned_cpu_image_buffer(5, 3, 3, DataType::UINT8);
  const std::size_t input_row_bytes =
      image_buffer_row_bytes(input.image_buffer);
  ASSERT_GT(input.image_buffer.step, input_row_bytes);
  const std::vector<std::uint8_t> active_input =
      fill_unsigned8_image(&input.image_buffer);

  GraphModel graph("cache/cpu-dense-tensor-image-operation");
  Node node;
  node.id = 79;
  node.name = "cpu_dense_invert";
  node.type = "image_process";
  node.subtype = "invert_dense";
  NodeOutput output =
      compute::NodeExecutor::execute(graph, node, *resolved, {&input});

  EXPECT_EQ(output.image_buffer.width, input.image_buffer.width);
  EXPECT_EQ(output.image_buffer.height, input.image_buffer.height);
  EXPECT_EQ(output.image_buffer.channels, input.image_buffer.channels);
  EXPECT_EQ(output.image_buffer.type, DataType::UINT8);
  EXPECT_EQ(output.image_buffer.device, Device::CPU);
  EXPECT_GT(output.image_buffer.step,
            image_buffer_row_bytes(output.image_buffer));
  validate_image_buffer(output.image_buffer);

  const std::vector<std::uint8_t> active_output =
      read_unsigned8_image(output.image_buffer);
  ASSERT_EQ(active_output.size(), active_input.size());
  for (std::size_t index = 0U; index < active_input.size(); ++index) {
    EXPECT_EQ(active_output[index],
              static_cast<std::uint8_t>(255U - active_input[index]));
  }
}

TEST(CpuDenseTensorImageOperation,
     RunnerRejectsExecuteDescriptorMismatchAsComputeError) {
  NodeOutput input;
  input.image_buffer = make_aligned_cpu_image_buffer(2, 2, 1, DataType::UINT8);
  (void)fill_unsigned8_image(&input.image_buffer);

  ops::CpuDenseImageOperation operation;
  operation.infer = [](const ops::CpuDenseImageConfiguration& configuration,
                       const std::vector<ops::DenseImageDescriptor>& inputs) {
    (void)configuration;
    return inputs.front();
  };
  operation.execute = [](const ops::CpuDenseImageConfiguration& configuration,
                         const std::vector<ImageView>& inputs,
                         const ops::DenseImageDescriptor& inferred) {
    (void)configuration;
    (void)inputs;
    ops::DenseImageDescriptor mismatched = inferred;
    ++mismatched.tensor.shape[mismatched.image.x_axis];
    const std::size_t width = mismatched.tensor.shape[mismatched.image.x_axis];
    const std::size_t height = mismatched.tensor.shape[mismatched.image.y_axis];
    StridedLayout layout{{static_cast<std::ptrdiff_t>(width), 1, 1}};
    std::vector<std::byte> storage(width * height, std::byte{0});
    return Value::from_cpu_dense_tensor(std::move(mismatched.tensor),
                                        mismatched.image, std::move(layout),
                                        std::move(storage));
  };

  Node node;
  try {
    (void)ops::execute_cpu_dense_image_operation(node, {&input}, operation);
    FAIL() << "descriptor mismatch should fail before NodeOutput publication";
  } catch (const GraphError& error) {
    EXPECT_EQ(error.code(), GraphErrc::ComputeError);
  }
}

}  // namespace
}  // namespace ps
