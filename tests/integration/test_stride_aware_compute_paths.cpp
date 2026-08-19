#include <gtest/gtest.h>

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>
#include <memory>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include "compute/dirty/node_executor.hpp"
#include "compute/request/compute_metrics_recorder.hpp"
#include "graph/graph_model.hpp"  // NOLINT(build/include_subdir)
#include "graph/node.hpp"         // NOLINT(build/include_subdir)
#include "photospider/data/image_view.hpp"

namespace ps {
namespace {

/**
 * @brief Publishes one padded zero-origin ordinary image Value.
 * @tparam Sample Trivially copyable scalar matching `semantics` and
 * `bit_width`.
 * @param width Positive image width.
 * @param height Positive image height.
 * @param channels Positive channel count.
 * @param row_stride Positive row stride no smaller than the active row.
 * @param semantics Exact stored-element semantics.
 * @param bit_width Exact stored-element bit width.
 * @param samples Exact row-major interleaved active samples.
 * @param padding Byte copied into every inactive row-padding position.
 * @return Fresh immutable CPU DenseTensor image Value.
 * @throws std::invalid_argument for inconsistent shape, encoding, or payload.
 * @throws std::overflow_error when size arithmetic overflows in Value
 * validation.
 * @throws std::bad_alloc when metadata or payload allocation fails.
 * @note Padding is retained in the immutable allocation but is not a logical
 * image element and therefore must not affect metrics.
 */
template <typename Sample>
Value make_padded_image_value(std::size_t width, std::size_t height,
                              std::size_t channels, std::size_t row_stride,
                              ElementSemantics semantics,
                              std::uint16_t bit_width,
                              const std::vector<Sample>& samples,
                              std::byte padding = std::byte{0xFF}) {
  const std::size_t row_bytes = width * channels * sizeof(Sample);
  if (width == 0U || height == 0U || channels == 0U || row_stride < row_bytes ||
      samples.size() != width * height * channels ||
      bit_width != sizeof(Sample) * 8U) {
    throw std::invalid_argument("padded image fixture is inconsistent");
  }
  std::vector<std::byte> storage((height - 1U) * row_stride + row_bytes,
                                 padding);
  for (std::size_t row = 0U; row < height; ++row) {
    std::memcpy(storage.data() + row * row_stride,
                samples.data() + row * width * channels, row_bytes);
  }
  DenseTensorDescriptor descriptor{{height, width, channels},
                                   semantics,
                                   StorageEncoding{bit_width}};
  ImageFacet facet = make_zero_origin_image_facet(descriptor, 1U, 0U, 2U);
  return Value::from_cpu_dense_tensor(
      std::move(descriptor), std::move(facet),
      StridedLayout{{static_cast<std::ptrdiff_t>(row_stride),
                     static_cast<std::ptrdiff_t>(channels * sizeof(Sample)),
                     static_cast<std::ptrdiff_t>(sizeof(Sample))}},
      std::move(storage));
}

/**
 * @brief Reads one FP32 channel through a checked immutable image view.
 * @param view Valid FP32 image view.
 * @param x Zero-based column.
 * @param y Zero-based row.
 * @param channel Zero-based channel.
 * @return Stored binary32 sample.
 * @throws std::out_of_range when a coordinate is invalid.
 * @throws std::bad_alloc when checked address calculation allocates.
 */
float read_float(const ImageView& view, std::size_t x, std::size_t y,
                 std::size_t channel = 0U) {
  float value = 0.0F;
  std::memcpy(&value, view.channel_data(x, y, channel), sizeof(value));
  return value;
}

TEST(StrideAwareComputePaths,
     TiledCropNormalizationAndMetricsIgnorePaddedRows) {
  GraphModel graph("cache/stride-aware-compute-paths");
  Node node;
  node.id = 57;
  node.name = "padded_mixing";
  node.type = "image_mixing";
  node.subtype = "copy_secondary";
  node.runtime_parameters["merge_strategy"] = std::string("crop");

  NodeOutput base;
  constexpr std::size_t kBaseRowStride = 64U;
  base.publish_image_value(make_padded_image_value<float>(
      5U, 4U, 1U, kBaseRowStride, ElementSemantics::FloatingPoint, 32U,
      std::vector<float>(20U, 0.0F)));

  NodeOutput secondary;
  constexpr std::size_t kSecondaryRowStride = 32U;
  secondary.publish_image_value(make_padded_image_value<float>(
      3U, 2U, 1U, kSecondaryRowStride, ElementSemantics::FloatingPoint, 32U,
      std::vector<float>{1.0F, 2.0F, 3.0F, 4.0F, 5.0F, 6.0F}));

  int tile_calls = 0;
  bool saw_normalized_fresh_input = false;
  OpRegistry::OpVariant operation =
      TileOpFunc([&](const Node&, const OutputTile& output_tile,
                     const std::vector<InputTile>& input_tiles) {
        ASSERT_EQ(input_tiles.size(), 2U);
        ASSERT_NE(input_tiles[1].value, nullptr);
        ASSERT_NE(output_tile.plan, nullptr);
        ASSERT_NE(output_tile.grant, nullptr);
        ASSERT_EQ(output_tile.grant->span_count(),
                  static_cast<std::size_t>(output_tile.roi.height));
        ++tile_calls;
        const ImageView normalized(*input_tiles[1].value);
        saw_normalized_fresh_input =
            saw_normalized_fresh_input ||
            (normalized.width() == 5U && normalized.height() == 4U &&
             input_tiles[1].value != &secondary.image_value());
        EXPECT_EQ(input_tiles[1].roi, output_tile.roi);
        for (int row = 0; row < output_tile.roi.height; ++row) {
          ASSERT_EQ(
              output_tile.grant->span(static_cast<std::size_t>(row)).byte_size,
              static_cast<std::size_t>(output_tile.roi.width) * sizeof(float));
          for (int column = 0; column < output_tile.roi.width; ++column) {
            const float sample =
                read_float(normalized,
                           static_cast<std::size_t>(output_tile.roi.x + column),
                           static_cast<std::size_t>(output_tile.roi.y + row));
            std::memcpy(output_tile.grant->data(static_cast<std::size_t>(row)) +
                            static_cast<std::size_t>(column) * sizeof(float),
                        &sample, sizeof(sample));
          }
        }
      });

  compute::TiledExecutionConfig config;
  config.tile_size = 2;
  NodeOutput output = compute::NodeExecutor::execute(
      graph, node, operation, {&base, &secondary}, config);

  EXPECT_EQ(tile_calls, 6);
  EXPECT_TRUE(saw_normalized_fresh_input);
  EXPECT_TRUE(output.has_image_value());
  const ImageView output_view(output.image_value());
  EXPECT_EQ(output_view.width(), 5U);
  EXPECT_EQ(output_view.height(), 4U);
  EXPECT_EQ(output_view.channels(), 1U);
  ASSERT_GT(output_view.row_stride(),
            static_cast<std::ptrdiff_t>(output_view.width() * sizeof(float)));

  const float expected[4][5] = {
      {1.0F, 2.0F, 3.0F, 0.0F, 0.0F},
      {4.0F, 5.0F, 6.0F, 0.0F, 0.0F},
      {0.0F, 0.0F, 0.0F, 0.0F, 0.0F},
      {0.0F, 0.0F, 0.0F, 0.0F, 0.0F},
  };
  std::vector<float> output_samples;
  output_samples.reserve(20U);
  for (std::size_t row = 0U; row < output_view.height(); ++row) {
    for (std::size_t column = 0U; column < output_view.width(); ++column) {
      const float value = read_float(output_view, column, row);
      output_samples.push_back(value);
      EXPECT_FLOAT_EQ(value, expected[row][column]);
    }
  }

  NodeOutput metrics_output;
  metrics_output.publish_image_value(make_padded_image_value<float>(
      5U, 4U, 1U, 64U, ElementSemantics::FloatingPoint, 32U, output_samples));
  compute::ComputeMetricsRecorder::finalize_output_metadata(
      metrics_output, {&base, &secondary}, true, 2.4);
  EXPECT_DOUBLE_EQ(metrics_output.debug.min_val, 0.0);
  EXPECT_DOUBLE_EQ(metrics_output.debug.max_val, 6.0);
  EXPECT_FALSE(metrics_output.debug.has_nan)
      << "0xFF padding encodes NaN float payloads but is not active pixels";
  EXPECT_EQ(metrics_output.debug.compute_device, "CPU");

  const float active_nan = std::numeric_limits<float>::quiet_NaN();
  output_samples.front() = active_nan;
  NodeOutput nan_output;
  nan_output.publish_image_value(make_padded_image_value<float>(
      5U, 4U, 1U, 64U, ElementSemantics::FloatingPoint, 32U, output_samples));
  compute::ComputeMetricsRecorder::finalize_output_metadata(
      nan_output, {&base, &secondary}, true, 2.4);
  EXPECT_TRUE(nan_output.debug.has_nan);
  EXPECT_DOUBLE_EQ(nan_output.debug.min_val, 0.0);
  EXPECT_DOUBLE_EQ(nan_output.debug.max_val, 6.0);
}

TEST(StrideAwareComputePaths,
     MetricsScanMultiChannelIntegerRowsWithoutPaddingBytes) {
  NodeOutput output;
  output.publish_image_value(make_padded_image_value<std::uint16_t>(
      2U, 2U, 3U, 32U, ElementSemantics::UnsignedInteger, 16U,
      std::vector<std::uint16_t>{1U, 2U, 3U, 4U, 5U, 6U, 7U, 8U, 9U, 10U, 11U,
                                 12U}));

  compute::ComputeMetricsRecorder::finalize_output_metadata(output, {}, true,
                                                            0.0);
  EXPECT_DOUBLE_EQ(output.debug.min_val, 1.0);
  EXPECT_DOUBLE_EQ(output.debug.max_val, 12.0);
  EXPECT_FALSE(output.debug.has_nan);
}

TEST(StrideAwareComputePaths, MetricsRetainAllNanEmptyRangeSentinels) {
  NodeOutput output;
  const float active_nan = std::numeric_limits<float>::quiet_NaN();
  output.publish_image_value(make_padded_image_value<float>(
      1U, 1U, 1U, 16U, ElementSemantics::FloatingPoint, 32U,
      std::vector<float>{active_nan}));

  compute::ComputeMetricsRecorder::finalize_output_metadata(output, {}, true,
                                                            0.0);
  EXPECT_TRUE(output.debug.has_nan);
  EXPECT_EQ(output.debug.min_val, std::numeric_limits<double>::infinity());
  EXPECT_EQ(output.debug.max_val, -std::numeric_limits<double>::infinity());
}

TEST(StrideAwareComputePaths, MetricsSkipAbsentImageBeforePixelInspection) {
  NodeOutput output;
  output.debug.min_val = 123.0;
  output.debug.max_val = 456.0;
  output.debug.has_nan = true;

  compute::ComputeMetricsRecorder::finalize_output_metadata(output, {}, true,
                                                            0.0);
  EXPECT_DOUBLE_EQ(output.debug.min_val, 123.0);
  EXPECT_DOUBLE_EQ(output.debug.max_val, 456.0);
  EXPECT_TRUE(output.debug.has_nan);
}

}  // namespace
}  // namespace ps
