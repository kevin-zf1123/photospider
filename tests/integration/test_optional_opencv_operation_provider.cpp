#include <gtest/gtest.h>

#include <algorithm>
#include <cstddef>
#include <filesystem>
#include <string>
#include <variant>
#include <vector>

#include "core/ps_types.hpp"  // NOLINT(build/include_subdir)
#include "graph/node.hpp"     // NOLINT(build/include_subdir)
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
 * @note A small dependency-neutral ImageBuffer is supplied so the built-in
 *       OpenCV provider and stdlib replacement are both executable.
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

  NodeOutput input;
  input.image_buffer =
      make_aligned_cpu_image_buffer(2, 2, 1, DataType::FLOAT32);
  auto* base = static_cast<std::byte*>(input.image_buffer.data.get());
  for (int row_index = 0; row_index < input.image_buffer.height; ++row_index) {
    auto* row = reinterpret_cast<float*>(
        base + static_cast<std::size_t>(row_index) * input.image_buffer.step);
    std::fill(row, row + input.image_buffer.width, 0.25F);
  }
  const std::vector<const NodeOutput*> inputs{&input};
  return std::get<MonolithicOpFunc>(resolved.value())(node, inputs);
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
 *       stdlib-only provider can supply and execute the absent operation.
 */
TEST(OptionalOpenCvOperationProvider, ReplacementExecutesAndRestores) {
  constexpr bool kExpectOpenCvProvider =
      PS_EXPECT_OPENCV_OPERATION_PROVIDER != 0;
  constexpr char kResizeKey[] = "image_process:resize";
  constexpr char kConstantKey[] = "image_generator:constant";

  PluginManager& manager = PluginManager::process_instance();
  EXPECT_EQ(manager.unload_all_plugins(), 0);
  manager.seed_builtins_from_registry();

  EXPECT_TRUE(registry_contains("analyzer:get_dimensions"));
  EXPECT_TRUE(registry_contains("math:divide"));
  EXPECT_EQ(registry_contains(kResizeKey), kExpectOpenCvProvider);
  EXPECT_EQ(registry_contains(kConstantKey), kExpectOpenCvProvider);

  if (kExpectOpenCvProvider) {
    const NodeOutput original = execute_active_resize();
    EXPECT_EQ(original.image_buffer.width, 4);
    EXPECT_EQ(original.image_buffer.height, 3);
    EXPECT_EQ(original.image_buffer.channels, 1);

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
  ASSERT_EQ(load_result.loaded, 1);
  ASSERT_TRUE(load_result.errors.empty());
  ASSERT_EQ(manager.op_sources().at(kResizeKey), plugin_path.string());

  const NodeOutput replacement = execute_active_resize();
  EXPECT_EQ(replacement.debug.compute_device, "STDLIB_RESIZE_REPLACEMENT");
  ASSERT_EQ(replacement.image_buffer.width, 3);
  ASSERT_EQ(replacement.image_buffer.height, 2);
  ASSERT_EQ(replacement.image_buffer.channels, 1);
  const auto* replacement_pixel =
      static_cast<const float*>(replacement.image_buffer.data.get());
  ASSERT_NE(replacement_pixel, nullptr);
  EXPECT_FLOAT_EQ(*replacement_pixel, 0.625F);

  EXPECT_GT(manager.unload_by_plugin_path(plugin_path.string()), 0);
  if (kExpectOpenCvProvider) {
    ASSERT_EQ(manager.op_sources().at(kResizeKey), "built-in");
    const NodeOutput restored = execute_active_resize();
    EXPECT_EQ(restored.image_buffer.width, 4);
    EXPECT_EQ(restored.image_buffer.height, 3);
  } else {
    EXPECT_FALSE(registry_contains(kResizeKey));
    EXPECT_EQ(manager.op_sources().count(kResizeKey), 0U);
  }
}

}  // namespace
}  // namespace ps
