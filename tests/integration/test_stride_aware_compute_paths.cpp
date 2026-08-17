#include <gtest/gtest.h>

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

#include "compute/dirty/node_executor.hpp"
#include "compute/request/compute_metrics_recorder.hpp"
#include "core/value_image_adapter.hpp"
#include "graph/graph_model.hpp"  // NOLINT(build/include_subdir)
#include "graph/node.hpp"         // NOLINT(build/include_subdir)
#include "photospider/core/image_buffer.hpp"

namespace ps {
namespace {

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
  ImageBuffer base_buffer =
      make_aligned_cpu_image_buffer(5, 4, 1, DataType::FLOAT32);
  ASSERT_GT(base_buffer.step, image_buffer_row_bytes(base_buffer));
  base.publish_image_value(
      value_image_adapter::snapshot_cpu_image_value(base_buffer));

  NodeOutput secondary;
  ImageBuffer secondary_buffer =
      make_aligned_cpu_image_buffer(3, 2, 1, DataType::FLOAT32);
  const std::size_t secondary_row_bytes =
      image_buffer_row_bytes(secondary_buffer);
  ASSERT_GT(secondary_buffer.step, secondary_row_bytes);
  std::memset(secondary_buffer.data.get(), 0xFF,
              secondary_buffer.step * secondary_buffer.height);
  const float secondary_pixels[2][3] = {{1.0F, 2.0F, 3.0F}, {4.0F, 5.0F, 6.0F}};
  auto* secondary_base = static_cast<std::byte*>(secondary_buffer.data.get());
  for (int row = 0; row < secondary_buffer.height; ++row) {
    std::memcpy(
        secondary_base + static_cast<std::size_t>(row) * secondary_buffer.step,
        secondary_pixels[row], secondary_row_bytes);
  }
  secondary.publish_image_value(
      value_image_adapter::snapshot_cpu_image_value(secondary_buffer));

  int tile_calls = 0;
  bool saw_normalized_padded_input = false;
  OpRegistry::OpVariant operation =
      TileOpFunc([&](const Node&, const OutputTile& output_tile,
                     const std::vector<InputTile>& input_tiles) {
        ASSERT_EQ(input_tiles.size(), 2U);
        ASSERT_NE(input_tiles[1].buffer, nullptr);
        ASSERT_NE(output_tile.plan, nullptr);
        ASSERT_NE(output_tile.grant, nullptr);
        ASSERT_EQ(output_tile.grant->span_count(),
                  static_cast<std::size_t>(output_tile.roi.height));
        ++tile_calls;
        const ImageBuffer& normalized = *input_tiles[1].buffer;
        saw_normalized_padded_input =
            saw_normalized_padded_input ||
            (normalized.width == base_buffer.width &&
             normalized.height == base_buffer.height &&
             normalized.step > image_buffer_row_bytes(normalized) &&
             input_tiles[1].buffer != &secondary_buffer);
        EXPECT_EQ(input_tiles[1].roi, output_tile.roi);
        for (int row = 0; row < output_tile.roi.height; ++row) {
          const std::byte* input_row =
              image_buffer_row_data(normalized, output_tile.roi.y + row);
          const std::size_t input_offset =
              static_cast<std::size_t>(output_tile.roi.x) * sizeof(float);
          ASSERT_EQ(
              output_tile.grant->span(static_cast<std::size_t>(row)).byte_size,
              static_cast<std::size_t>(output_tile.roi.width) * sizeof(float));
          std::memcpy(
              output_tile.grant->data(static_cast<std::size_t>(row)),
              input_row + input_offset,
              static_cast<std::size_t>(output_tile.roi.width) * sizeof(float));
        }
      });

  compute::TiledExecutionConfig config;
  config.tile_size = 2;
  NodeOutput output = compute::NodeExecutor::execute(
      graph, node, operation, {&base, &secondary}, config);

  EXPECT_EQ(tile_calls, 6);
  EXPECT_TRUE(saw_normalized_padded_input);
  EXPECT_TRUE(output.has_image_value());
  EXPECT_FALSE(output.has_compatibility_image());
  ImageBuffer output_buffer =
      value_image_adapter::snapshot_cpu_image_buffer(output.image_value());
  EXPECT_EQ(output_buffer.width, 5);
  EXPECT_EQ(output_buffer.height, 4);
  EXPECT_EQ(output_buffer.channels, 1);
  const std::size_t output_row_bytes = image_buffer_row_bytes(output_buffer);
  ASSERT_GT(output_buffer.step, output_row_bytes);

  const float expected[4][5] = {
      {1.0F, 2.0F, 3.0F, 0.0F, 0.0F},
      {4.0F, 5.0F, 6.0F, 0.0F, 0.0F},
      {0.0F, 0.0F, 0.0F, 0.0F, 0.0F},
      {0.0F, 0.0F, 0.0F, 0.0F, 0.0F},
  };
  auto* output_base = static_cast<std::byte*>(output_buffer.data.get());
  for (int row = 0; row < output_buffer.height; ++row) {
    const std::byte* active = image_buffer_row_data(output_buffer, row);
    for (int column = 0; column < output_buffer.width; ++column) {
      float value = 0.0F;
      std::memcpy(&value,
                  active + static_cast<std::size_t>(column) * sizeof(float),
                  sizeof(value));
      EXPECT_FLOAT_EQ(value, expected[row][column]);
    }
    std::memset(output_base +
                    static_cast<std::size_t>(row) * output_buffer.step +
                    output_row_bytes,
                0xFF, output_buffer.step - output_row_bytes);
  }

  NodeOutput metrics_output;
  metrics_output.publish_image_value(
      value_image_adapter::snapshot_cpu_image_value(output_buffer));
  compute::ComputeMetricsRecorder::finalize_output_metadata(
      metrics_output, {&base, &secondary}, true, 2.4);
  EXPECT_DOUBLE_EQ(metrics_output.debug.min_val, 0.0);
  EXPECT_DOUBLE_EQ(metrics_output.debug.max_val, 6.0);
  EXPECT_FALSE(metrics_output.debug.has_nan)
      << "0xFF padding encodes NaN float payloads but is not active pixels";
  EXPECT_EQ(metrics_output.debug.compute_device, "CPU");

  const float active_nan = std::numeric_limits<float>::quiet_NaN();
  std::memcpy(output_base, &active_nan, sizeof(active_nan));
  NodeOutput nan_output;
  nan_output.publish_image_value(
      value_image_adapter::snapshot_cpu_image_value(output_buffer));
  compute::ComputeMetricsRecorder::finalize_output_metadata(
      nan_output, {&base, &secondary}, true, 2.4);
  EXPECT_TRUE(nan_output.debug.has_nan);
  EXPECT_DOUBLE_EQ(nan_output.debug.min_val, 0.0);
  EXPECT_DOUBLE_EQ(nan_output.debug.max_val, 6.0);
}

TEST(StrideAwareComputePaths,
     MetricsScanMultiChannelIntegerRowsWithoutPaddingBytes) {
  NodeOutput output;
  ImageBuffer output_buffer =
      make_aligned_cpu_image_buffer(2, 2, 3, DataType::UINT16);
  const std::size_t row_bytes = image_buffer_row_bytes(output_buffer);
  ASSERT_GT(output_buffer.step, row_bytes);
  std::memset(output_buffer.data.get(), 0xFF,
              output_buffer.step * output_buffer.height);
  const std::uint16_t pixels[2][6] = {
      {1U, 2U, 3U, 4U, 5U, 6U},
      {7U, 8U, 9U, 10U, 11U, 12U},
  };
  auto* base = static_cast<std::byte*>(output_buffer.data.get());
  for (int row = 0; row < output_buffer.height; ++row) {
    std::memcpy(base + static_cast<std::size_t>(row) * output_buffer.step,
                pixels[row], row_bytes);
  }
  output.publish_image_value(
      value_image_adapter::snapshot_cpu_image_value(output_buffer));

  compute::ComputeMetricsRecorder::finalize_output_metadata(output, {}, true,
                                                            0.0);
  EXPECT_DOUBLE_EQ(output.debug.min_val, 1.0);
  EXPECT_DOUBLE_EQ(output.debug.max_val, 12.0);
  EXPECT_FALSE(output.debug.has_nan);
}

TEST(StrideAwareComputePaths, MetricsRetainAllNanEmptyRangeSentinels) {
  NodeOutput output;
  ImageBuffer output_buffer =
      make_aligned_cpu_image_buffer(1, 1, 1, DataType::FLOAT32);
  const float active_nan = std::numeric_limits<float>::quiet_NaN();
  std::memcpy(output_buffer.data.get(), &active_nan, sizeof(active_nan));
  output.publish_image_value(
      value_image_adapter::snapshot_cpu_image_value(output_buffer));

  compute::ComputeMetricsRecorder::finalize_output_metadata(output, {}, true,
                                                            0.0);
  EXPECT_TRUE(output.debug.has_nan);
  EXPECT_EQ(output.debug.min_val, std::numeric_limits<double>::infinity());
  EXPECT_EQ(output.debug.max_val, -std::numeric_limits<double>::infinity());
}

TEST(StrideAwareComputePaths,
     MetricsSkipCompatibilityStagingBeforePixelInspection) {
  NodeOutput output;
  output.compatibility_image.width = 2;
  output.compatibility_image.height = 2;
  output.compatibility_image.channels = 1;
  output.compatibility_image.context = std::make_shared<int>(57);
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
