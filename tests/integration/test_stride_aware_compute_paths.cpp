#include <gtest/gtest.h>

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

#include "compute/compute_metrics_recorder.hpp"
#include "compute/node_executor.hpp"
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
  base.image_buffer = make_aligned_cpu_image_buffer(5, 4, 1, DataType::FLOAT32);
  ASSERT_GT(base.image_buffer.step, image_buffer_row_bytes(base.image_buffer));

  NodeOutput secondary;
  secondary.image_buffer =
      make_aligned_cpu_image_buffer(3, 2, 1, DataType::FLOAT32);
  const std::size_t secondary_row_bytes =
      image_buffer_row_bytes(secondary.image_buffer);
  ASSERT_GT(secondary.image_buffer.step, secondary_row_bytes);
  std::memset(secondary.image_buffer.data.get(), 0xFF,
              secondary.image_buffer.step * secondary.image_buffer.height);
  const float secondary_pixels[2][3] = {{1.0F, 2.0F, 3.0F}, {4.0F, 5.0F, 6.0F}};
  auto* secondary_base =
      static_cast<std::byte*>(secondary.image_buffer.data.get());
  for (int row = 0; row < secondary.image_buffer.height; ++row) {
    std::memcpy(secondary_base +
                    static_cast<std::size_t>(row) * secondary.image_buffer.step,
                secondary_pixels[row], secondary_row_bytes);
  }

  int tile_calls = 0;
  bool saw_normalized_padded_input = false;
  OpRegistry::OpVariant operation =
      TileOpFunc([&](const Node&, const OutputTile& output_tile,
                     const std::vector<InputTile>& input_tiles) {
        ASSERT_EQ(input_tiles.size(), 2U);
        ASSERT_NE(input_tiles[1].buffer, nullptr);
        ASSERT_NE(output_tile.buffer, nullptr);
        ++tile_calls;
        const ImageBuffer& normalized = *input_tiles[1].buffer;
        saw_normalized_padded_input =
            saw_normalized_padded_input ||
            (normalized.width == base.image_buffer.width &&
             normalized.height == base.image_buffer.height &&
             normalized.step > image_buffer_row_bytes(normalized) &&
             input_tiles[1].buffer != &secondary.image_buffer);
        EXPECT_EQ(input_tiles[1].roi, output_tile.roi);
        copy_image_buffer_region(
            InputTileView{input_tiles[1].buffer, input_tiles[1].roi},
            OutputTileView{output_tile.buffer, output_tile.roi});
      });

  compute::TiledExecutionConfig config;
  config.tile_size = 2;
  NodeOutput output = compute::NodeExecutor::execute(
      graph, node, operation, {&base, &secondary}, config);

  EXPECT_EQ(tile_calls, 6);
  EXPECT_TRUE(saw_normalized_padded_input);
  EXPECT_EQ(output.image_buffer.width, 5);
  EXPECT_EQ(output.image_buffer.height, 4);
  EXPECT_EQ(output.image_buffer.channels, 1);
  const std::size_t output_row_bytes =
      image_buffer_row_bytes(output.image_buffer);
  ASSERT_GT(output.image_buffer.step, output_row_bytes);

  const float expected[4][5] = {
      {1.0F, 2.0F, 3.0F, 0.0F, 0.0F},
      {4.0F, 5.0F, 6.0F, 0.0F, 0.0F},
      {0.0F, 0.0F, 0.0F, 0.0F, 0.0F},
      {0.0F, 0.0F, 0.0F, 0.0F, 0.0F},
  };
  auto* output_base = static_cast<std::byte*>(output.image_buffer.data.get());
  for (int row = 0; row < output.image_buffer.height; ++row) {
    const std::byte* active = image_buffer_row_data(output.image_buffer, row);
    for (int column = 0; column < output.image_buffer.width; ++column) {
      float value = 0.0F;
      std::memcpy(&value,
                  active + static_cast<std::size_t>(column) * sizeof(float),
                  sizeof(value));
      EXPECT_FLOAT_EQ(value, expected[row][column]);
    }
    std::memset(output_base +
                    static_cast<std::size_t>(row) * output.image_buffer.step +
                    output_row_bytes,
                0xFF, output.image_buffer.step - output_row_bytes);
  }

  compute::ComputeMetricsRecorder::finalize_output_metadata(
      output, {&base, &secondary}, true, 2.4);
  EXPECT_DOUBLE_EQ(output.debug.min_val, 0.0);
  EXPECT_DOUBLE_EQ(output.debug.max_val, 6.0);
  EXPECT_FALSE(output.debug.has_nan)
      << "0xFF padding encodes NaN float payloads but is not active pixels";
  EXPECT_EQ(output.debug.compute_device, "CPU");

  const float active_nan = std::numeric_limits<float>::quiet_NaN();
  std::memcpy(output_base, &active_nan, sizeof(active_nan));
  compute::ComputeMetricsRecorder::finalize_output_metadata(
      output, {&base, &secondary}, true, 2.4);
  EXPECT_TRUE(output.debug.has_nan);
  EXPECT_DOUBLE_EQ(output.debug.min_val, 0.0);
  EXPECT_DOUBLE_EQ(output.debug.max_val, 6.0);
}

TEST(StrideAwareComputePaths,
     MetricsScanMultiChannelIntegerRowsWithoutPaddingBytes) {
  NodeOutput output;
  output.image_buffer =
      make_aligned_cpu_image_buffer(2, 2, 3, DataType::UINT16);
  const std::size_t row_bytes = image_buffer_row_bytes(output.image_buffer);
  ASSERT_GT(output.image_buffer.step, row_bytes);
  std::memset(output.image_buffer.data.get(), 0xFF,
              output.image_buffer.step * output.image_buffer.height);
  const std::uint16_t pixels[2][6] = {
      {1U, 2U, 3U, 4U, 5U, 6U},
      {7U, 8U, 9U, 10U, 11U, 12U},
  };
  auto* base = static_cast<std::byte*>(output.image_buffer.data.get());
  for (int row = 0; row < output.image_buffer.height; ++row) {
    std::memcpy(base + static_cast<std::size_t>(row) * output.image_buffer.step,
                pixels[row], row_bytes);
  }

  compute::ComputeMetricsRecorder::finalize_output_metadata(output, {}, true,
                                                            0.0);
  EXPECT_DOUBLE_EQ(output.debug.min_val, 1.0);
  EXPECT_DOUBLE_EQ(output.debug.max_val, 12.0);
  EXPECT_FALSE(output.debug.has_nan);
}

TEST(StrideAwareComputePaths, MetricsRetainAllNanEmptyRangeSentinels) {
  NodeOutput output;
  output.image_buffer =
      make_aligned_cpu_image_buffer(1, 1, 1, DataType::FLOAT32);
  const float active_nan = std::numeric_limits<float>::quiet_NaN();
  std::memcpy(output.image_buffer.data.get(), &active_nan, sizeof(active_nan));

  compute::ComputeMetricsRecorder::finalize_output_metadata(output, {}, true,
                                                            0.0);
  EXPECT_TRUE(output.debug.has_nan);
  EXPECT_EQ(output.debug.min_val, std::numeric_limits<double>::infinity());
  EXPECT_EQ(output.debug.max_val, -std::numeric_limits<double>::infinity());
}

TEST(StrideAwareComputePaths,
     MetricsRejectMalformedCpuDescriptorBeforePixelInspection) {
  NodeOutput output;
  output.image_buffer.width = 2;
  output.image_buffer.height = 2;
  output.image_buffer.channels = 1;
  output.image_buffer.context = std::make_shared<int>(57);

  EXPECT_THROW(compute::ComputeMetricsRecorder::finalize_output_metadata(
                   output, {}, true, 0.0),
               std::invalid_argument);
}

}  // namespace
}  // namespace ps
