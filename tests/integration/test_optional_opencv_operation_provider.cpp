#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <string>
#include <utility>
#include <variant>
#include <vector>

#include "core/ps_types.hpp"  // NOLINT(build/include_subdir)
#include "core/value_image_adapter.hpp"
#include "graph/node.hpp"  // NOLINT(build/include_subdir)
#include "photospider/core/graph_error.hpp"
#include "photospider/core/image_buffer.hpp"
#include "plugin/plugin_manager.hpp"

#ifndef PS_RESIZE_REPLACEMENT_PLUGIN_PATH
#error "PS_RESIZE_REPLACEMENT_PLUGIN_PATH must name the replacement fixture"
#endif

#ifndef PS_EXPECT_OPENCV_OPERATION_PROVIDER
#error "PS_EXPECT_OPENCV_OPERATION_PROVIDER must describe this build profile"
#endif

namespace ps {
namespace {

/**
 * @brief Checks whether the process registry currently contains one key.
 *
 * @param key Canonical `type:subtype` key.
 * @return True when the sorted registry snapshot contains the key.
 * @throws std::bad_alloc if registry snapshot allocation fails.
 * @note The returned Boolean carries no callback or provider lifetime.
 */
bool registry_contains(const std::string& key) {
  const std::vector<std::string> keys = OpRegistry::instance().get_keys();
  return std::binary_search(keys.begin(), keys.end(), key);
}

/**
 * @brief Resolves and executes the active resize callback.
 *
 * @return Complete operation output from the active provider.
 * @throws std::bad_alloc if input, callback snapshot, or output allocation
 *         fails.
 * @throws GraphError or another active-provider exception unchanged.
 * @note A small canonical Value is supplied so the built-in OpenCV provider
 *       and stdlib replacement are both executable.
 */
NodeOutput execute_active_resize() {
  auto resolved = OpRegistry::instance().resolve_for_intent(
      "image_process", "resize", ComputeIntent::GlobalHighPrecision);
  if (!resolved ||
      !std::holds_alternative<MonolithicOpFunc>(resolved.value())) {
    throw GraphError(GraphErrc::NoOperation,
                     "image_process:resize is not monolithic");
  }

  Node node;
  node.id = 58;
  node.type = "image_process";
  node.subtype = "resize";
  node.runtime_parameters["width"] = 4;
  node.runtime_parameters["height"] = 3;
  node.runtime_parameters["interpolation"] = "nearest";

  ImageBuffer input_image =
      make_aligned_cpu_image_buffer(2, 2, 1, DataType::FLOAT32);
  auto* base = static_cast<std::byte*>(input_image.data.get());
  for (int row_index = 0; row_index < input_image.height; ++row_index) {
    auto* row = reinterpret_cast<float*>(
        base + static_cast<std::size_t>(row_index) * input_image.step);
    std::fill(row, row + input_image.width, 0.25F);
  }
  NodeOutput input;
  input.publish_image_value(
      value_image_adapter::snapshot_cpu_image_value(input_image));
  const std::vector<const NodeOutput*> inputs{&input};
  return std::get<MonolithicOpFunc>(resolved.value())(node, inputs);
}

/**
 * @brief Projects one canonical provider output to a callback-compatible CPU
 * snapshot for exact test inspection.
 *
 * @param output Provider result carrying the required image Value.
 * @return Independent CPU ImageBuffer snapshot.
 * @throws std::logic_error when the canonical image output is absent.
 * @throws Adapter validation, overflow, access, and allocation exceptions
 * unchanged.
 * @note The snapshot is read-only test observation and never becomes runtime,
 * revision, allocation, or cache authority.
 */
ImageBuffer snapshot_output_image(const NodeOutput& output) {
  return value_image_adapter::snapshot_cpu_image_buffer(output.image_value());
}

/**
 * @brief Resolves and executes the frozen coordinate-pattern generator.
 *
 * @return Complete `2x2` RGBA FP32 output for seed zero.
 * @throws GraphError when the active registry does not expose the required
 *         monolithic callback or the provider rejects the request.
 * @throws std::bad_alloc if callback snapshot or output allocation fails.
 * @note The compact fixture retains every term in the frozen coordinate
 *       formula while keeping exact-bit assertions inexpensive.
 */
NodeOutput execute_coordinate_pattern() {
  auto resolved = OpRegistry::instance().resolve_for_intent(
      "image_generator", "coordinate_pattern",
      ComputeIntent::GlobalHighPrecision);
  if (!resolved ||
      !std::holds_alternative<MonolithicOpFunc>(resolved.value())) {
    throw GraphError(GraphErrc::NoOperation,
                     "image_generator:coordinate_pattern is not monolithic");
  }

  Node node;
  node.id = 60;
  node.type = "image_generator";
  node.subtype = "coordinate_pattern";
  node.runtime_parameters["width"] = 2;
  node.runtime_parameters["height"] = 2;
  node.runtime_parameters["channels"] = 4;
  node.runtime_parameters["seed"] = 0;
  return std::get<MonolithicOpFunc>(resolved.value())(node, {});
}

/**
 * @brief Reads one FP32 image sample as its exact IEEE 754 bit pattern.
 *
 * @param image Provider output with CPU-backed FLOAT32 storage.
 * @param x Zero-based horizontal coordinate.
 * @param y Zero-based vertical coordinate.
 * @param channel Zero-based interleaved channel coordinate.
 * @return Raw 32-bit encoding of the selected sample.
 * @throws Nothing.
 * @note The caller owns bounds and image-format validation. `memcpy` avoids
 *       aliasing undefined behavior and preserves signed-zero distinctions.
 */
std::uint32_t sample_bits(const ImageBuffer& image, int x, int y,
                          int channel) noexcept {
  const auto* base = static_cast<const std::byte*>(image.data.get());
  const auto* sample =
      base + static_cast<std::size_t>(y) * image.step +
      (static_cast<std::size_t>(x) * static_cast<std::size_t>(image.channels) +
       static_cast<std::size_t>(channel)) *
          sizeof(float);
  std::uint32_t bits = 0U;
  std::memcpy(&bits, sample, sizeof(bits));
  return bits;
}

/**
 * @brief Forces a deterministic OpenCV construction failure through constant.
 *
 * @return Nothing when the active provider unexpectedly accepts the invalid
 *         image shape.
 * @throws GraphError translated by the OpenCV provider error fence.
 * @throws std::bad_alloc if callback snapshot or error storage allocation
 *         fails.
 * @note Negative OpenCV matrix dimensions reliably raise `cv::Exception`
 *       inside the provider without including OpenCV in this test.
 */
void execute_invalid_opencv_constant() {
  auto resolved = OpRegistry::instance().resolve_for_intent(
      "image_generator", "constant", ComputeIntent::GlobalHighPrecision);
  if (!resolved ||
      !std::holds_alternative<MonolithicOpFunc>(resolved.value())) {
    throw GraphError(GraphErrc::NoOperation,
                     "image_generator:constant is not monolithic");
  }
  Node node;
  node.id = 59;
  node.type = "image_generator";
  node.subtype = "constant";
  node.runtime_parameters["width"] = -1;
  node.runtime_parameters["height"] = 1;
  node.runtime_parameters["channels"] = 1;
  (void)std::get<MonolithicOpFunc>(resolved.value())(node, {});
}

/**
 * @brief Validates optional publication, real replacement execution, and
 *        predecessor restoration.
 * @throws Nothing when all GTest assertions pass.
 * @note The same source is built twice: the normal profile proves replacement
 *       of the OpenCV provider; the provider-disabled profile proves a
 *       stdlib-only provider can supply and execute the absent operation. The
 *       enabled profile also requires every exact OpenCV scalar candidate to
 *       retain its own backward and forward planning callbacks.
 */
TEST(OptionalOpenCvOperationProvider, ReplacementExecutesAndRestores) {
  constexpr bool kExpectOpenCvProvider =
      PS_EXPECT_OPENCV_OPERATION_PROVIDER != 0;
  constexpr char kResizeKey[] = "image_process:resize";
  constexpr char kConstantKey[] = "image_generator:constant";
  constexpr char kCoordinatePatternKey[] = "image_generator:coordinate_pattern";

  PluginManager& manager = PluginManager::process_instance();
  EXPECT_EQ(manager.unload_all_plugins(), 0);
  manager.seed_builtins_from_registry();

  EXPECT_TRUE(registry_contains("analyzer:get_dimensions"));
  EXPECT_TRUE(registry_contains("math:divide"));
  EXPECT_EQ(registry_contains(kResizeKey), kExpectOpenCvProvider);
  EXPECT_EQ(registry_contains(kConstantKey), kExpectOpenCvProvider);
  EXPECT_EQ(registry_contains(kCoordinatePatternKey), kExpectOpenCvProvider);

  if (kExpectOpenCvProvider) {
    constexpr std::array<std::pair<const char*, const char*>, 12U>
        kMonolithicCandidates{{
            {"image_source", "path"},
            {"image_generator", "constant"},
            {"image_generator", "coordinate_pattern"},
            {"image_generator", "perlin_noise"},
            {"image_process", "convolve"},
            {"image_process", "resize"},
            {"image_process", "crop"},
            {"image_process", "extract_channel"},
            {"image_process", "gaussian_blur"},
            {"image_mixing", "add_weighted"},
            {"image_mixing", "diff"},
            {"image_mixing", "multiply"},
        }};
    constexpr std::array<std::pair<const char*, const char*>, 5U>
        kHighPrecisionTiledCandidates{{
            {"image_process", "gaussian_blur"},
            {"image_process", "curve_transform"},
            {"image_mixing", "add_weighted"},
            {"image_mixing", "diff"},
            {"image_mixing", "multiply"},
        }};
    constexpr std::array<std::pair<const char*, const char*>, 2U>
        kRealtimeTiledCandidates{{
            {"image_process", "gaussian_blur"},
            {"image_mixing", "add_weighted"},
        }};

    for (const auto& [type, subtype] : kMonolithicCandidates) {
      SCOPED_TRACE(make_key(type, subtype));
      const auto implementations =
          OpRegistry::instance().get_implementations(type, subtype);
      ASSERT_TRUE(implementations.has_value());
      ASSERT_TRUE(implementations->monolithic_hp.has_value());
      EXPECT_TRUE(implementations->monolithic_hp->dirty_propagator.has_value());
      EXPECT_TRUE(
          implementations->monolithic_hp->forward_propagator.has_value());
    }
    for (const auto& [type, subtype] : kHighPrecisionTiledCandidates) {
      SCOPED_TRACE(make_key(type, subtype));
      const auto implementations =
          OpRegistry::instance().get_implementations(type, subtype);
      ASSERT_TRUE(implementations.has_value());
      ASSERT_TRUE(implementations->tiled_hp.has_value());
      EXPECT_TRUE(implementations->tiled_hp->dirty_propagator.has_value());
      EXPECT_TRUE(implementations->tiled_hp->forward_propagator.has_value());
    }
    for (const auto& [type, subtype] : kRealtimeTiledCandidates) {
      SCOPED_TRACE(make_key(type, subtype));
      const auto implementations =
          OpRegistry::instance().get_implementations(type, subtype);
      ASSERT_TRUE(implementations.has_value());
      ASSERT_TRUE(implementations->tiled_rt.has_value());
      EXPECT_TRUE(implementations->tiled_rt->dirty_propagator.has_value());
      EXPECT_TRUE(implementations->tiled_rt->forward_propagator.has_value());
    }

    const NodeOutput original = execute_active_resize();
    const ImageBuffer original_image = snapshot_output_image(original);
    EXPECT_EQ(original_image.width, 4);
    EXPECT_EQ(original_image.height, 3);
    EXPECT_EQ(original_image.channels, 1);

    const NodeOutput pattern = execute_coordinate_pattern();
    const ImageBuffer pattern_image = snapshot_output_image(pattern);
    ASSERT_EQ(pattern_image.width, 2);
    ASSERT_EQ(pattern_image.height, 2);
    ASSERT_EQ(pattern_image.channels, 4);
    ASSERT_EQ(pattern_image.type, DataType::FLOAT32);
    ASSERT_NE(pattern_image.data, nullptr);
    EXPECT_EQ(sample_bits(pattern_image, 0, 0, 0), 0x00000000U);
    EXPECT_EQ(sample_bits(pattern_image, 0, 0, 1), 0x3e3cbcbdU);
    EXPECT_EQ(sample_bits(pattern_image, 1, 0, 0), 0x3d888889U);
    EXPECT_EQ(sample_bits(pattern_image, 0, 1, 0), 0x3df8f8f9U);
    EXPECT_EQ(sample_bits(pattern_image, 1, 1, 1), 0x3ebebebfU);

    try {
      execute_invalid_opencv_constant();
      FAIL() << "invalid OpenCV dimensions unexpectedly succeeded";
    } catch (const GraphError& error) {
      EXPECT_EQ(error.code(), GraphErrc::ComputeError);
      EXPECT_NE(std::string(error.what()).find("image_generator:constant"),
                std::string::npos);
    }
  }

  const std::filesystem::path plugin_path =
      std::filesystem::absolute(PS_RESIZE_REPLACEMENT_PLUGIN_PATH);
  ASSERT_TRUE(std::filesystem::exists(plugin_path));
  const PluginLoadResult load_result =
      manager.load_from_dirs_report({plugin_path.parent_path().string()});
  ASSERT_EQ(load_result.loaded, 1)
      << (load_result.errors.empty() ? std::string{}
                                     : load_result.errors.front().message);
  ASSERT_TRUE(load_result.errors.empty());
  const std::string expected_replacement_source =
      kExpectOpenCvProvider ? "mixed" : plugin_path.string();
  ASSERT_EQ(manager.op_sources().at(kResizeKey), expected_replacement_source);
  ASSERT_EQ(manager.combined_sources().at(kResizeKey),
            expected_replacement_source);

  const NodeOutput replacement = execute_active_resize();
  EXPECT_EQ(replacement.debug.compute_device, "STDLIB_RESIZE_REPLACEMENT");
  const ImageBuffer replacement_image = snapshot_output_image(replacement);
  ASSERT_EQ(replacement_image.width, 3);
  ASSERT_EQ(replacement_image.height, 2);
  ASSERT_EQ(replacement_image.channels, 1);
  const auto* replacement_pixel =
      static_cast<const float*>(replacement_image.data.get());
  ASSERT_NE(replacement_pixel, nullptr);
  EXPECT_FLOAT_EQ(*replacement_pixel, 0.625F);

  EXPECT_GT(manager.unload_by_plugin_path(plugin_path.string()), 0);
  if (kExpectOpenCvProvider) {
    ASSERT_EQ(manager.op_sources().at(kResizeKey), "built-in");
    ASSERT_EQ(manager.combined_sources().at(kResizeKey), "built-in");
    const NodeOutput restored = execute_active_resize();
    const ImageBuffer restored_image = snapshot_output_image(restored);
    EXPECT_EQ(restored_image.width, 4);
    EXPECT_EQ(restored_image.height, 3);
  } else {
    EXPECT_FALSE(registry_contains(kResizeKey));
    EXPECT_EQ(manager.op_sources().count(kResizeKey), 0U);
    EXPECT_EQ(manager.combined_sources().count(kResizeKey), 0U);
  }
}

}  // namespace
}  // namespace ps
