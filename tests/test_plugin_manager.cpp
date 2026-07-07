#include <gtest/gtest.h>

#include <algorithm>
#include <filesystem>
#include <sstream>
#include <string>
#include <variant>
#include <vector>

#include "kernel/plugin_manager.hpp"
#include "node.hpp"      // NOLINT(build/include_subdir)
#include "ps_types.hpp"  // NOLINT(build/include_subdir)

namespace ps {
namespace {

constexpr const char* kLifecycleType = "plugin_lifecycle";
constexpr const char* kLifecycleSubtype = "op";
constexpr const char* kLifecycleKey = "plugin_lifecycle:op";
constexpr const char* kStandardPluginDir = "build/plugins";

/**
 * @brief Returns the build output path for the lifecycle op plugin fixture.
 *
 * @return Platform-specific shared library path under `build/test_plugins`.
 * @throws std::bad_alloc from path string construction.
 * @note Tests run with the repository root as working directory, matching the
 * existing scheduler plugin loader tests.
 */
std::filesystem::path lifecycle_plugin_path() {
  const std::filesystem::path dir = "build/test_plugins/lifecycle";
#if defined(_WIN32)
  return dir / "lifecycle_op_plugin.dll";
#elif defined(__APPLE__)
  return dir / "liblifecycle_op_plugin.dylib";
#else
  return dir / "liblifecycle_op_plugin.so";
#endif
}

/**
 * @brief Returns the build output path for the replacement lifecycle plugin.
 *
 * @return Platform-specific shared library path under `build/test_plugins`.
 * @throws std::bad_alloc from path string construction.
 * @note The replacement plugin intentionally registers the same operation key
 * as `lifecycle_op_plugin`.
 */
std::filesystem::path override_lifecycle_plugin_path() {
  const std::filesystem::path dir = "build/test_plugins/override";
#if defined(_WIN32)
  return dir / "override_lifecycle_op_plugin.dll";
#elif defined(__APPLE__)
  return dir / "liboverride_lifecycle_op_plugin.dylib";
#else
  return dir / "liboverride_lifecycle_op_plugin.so";
#endif
}

/**
 * @brief Formats plugin load errors for assertion messages.
 *
 * @param errors Structured plugin load errors returned by the manager.
 * @return Single-line summary suitable for GTest failure output.
 * @throws std::bad_alloc from string stream growth.
 * @note The helper keeps assertions readable without changing loader behavior.
 */
std::string describe_errors(const std::vector<PluginLoadError>& errors) {
  std::ostringstream out;
  for (const auto& error : errors) {
    out << "[" << error.path << "] " << error.message << "; ";
  }
  return out.str();
}

/**
 * @brief Checks whether a load result reports the lifecycle operation key.
 *
 * @param result Plugin load result to inspect.
 * @return True when reported keys contain `plugin_lifecycle:op`.
 * @throws Nothing.
 * @note The lifecycle plugin registers through `impl_table_`, so this also
 * proves loader key discovery observes multi-implementation registrations.
 */
bool result_contains_lifecycle_key(const PluginLoadResult& result) {
  return std::find(result.new_op_keys.begin(), result.new_op_keys.end(),
                   kLifecycleKey) != result.new_op_keys.end();
}

/**
 * @brief Executes the currently registered lifecycle operation marker.
 *
 * @return `NodeOutput::debug.compute_device` emitted by the resolved
 * implementation.
 * @throws GTest assertion failures when the key is missing or not monolithic.
 * @note Invoking the callback proves the active registry entry points to the
 * expected loaded library implementation.
 */
std::string current_lifecycle_compute_device() {
  auto op = OpRegistry::instance().resolve_for_intent(
      kLifecycleType, kLifecycleSubtype, ComputeIntent::GlobalHighPrecision);
  EXPECT_TRUE(op.has_value());
  if (!op || !std::holds_alternative<MonolithicOpFunc>(*op)) {
    return {};
  }

  Node node;
  node.id = 1;
  node.type = kLifecycleType;
  node.subtype = kLifecycleSubtype;
  const std::vector<const NodeOutput*> inputs;
  NodeOutput output = std::get<MonolithicOpFunc>(*op)(node, inputs);
  return output.debug.compute_device;
}

/**
 * @brief Checks whether a load result reports a standard plugin operation key.
 *
 * @param result Plugin load result to inspect.
 * @param key Canonical `type:subtype` operation key.
 * @return True when reported keys contain `key`.
 * @throws Nothing.
 * @note Standard plugins are loaded from `build/plugins`, and this helper keeps
 * test assertions independent from platform-specific shared library suffixes.
 */
bool result_contains_key(const PluginLoadResult& result,
                         const std::string& key) {
  return std::find(result.new_op_keys.begin(), result.new_op_keys.end(), key) !=
         result.new_op_keys.end();
}

/**
 * @brief Asserts that one operation uses explicit dirty and forward contracts.
 *
 * @param type Operation type registered in OpRegistry.
 * @param subtype Operation subtype registered in OpRegistry.
 * @return Nothing.
 * @throws GTest assertion failures when either contract is missing.
 * @note The helper checks contract status rather than callback return values so
 * side-effecting plugins such as `io:save` can document explicit pass-through
 * behavior without pretending to produce an image output.
 */
void expect_explicit_roi_contract(const std::string& type,
                                  const std::string& subtype) {
  auto& registry = OpRegistry::instance();
  EXPECT_EQ(registry.dirty_propagation_contract_status(type, subtype),
            PropagationContractStatus::Explicit)
      << type << ":" << subtype << " dirty ROI contract";
  EXPECT_EQ(registry.forward_propagation_contract_status(type, subtype),
            PropagationContractStatus::Explicit)
      << type << ":" << subtype << " forward ROI contract";
}

/**
 * @brief Test fixture that isolates the global operation registry key.
 *
 * Each test removes the lifecycle key before and after execution because
 * `OpRegistry` is a process-global singleton shared by all plugin manager
 * instances.
 */
class PluginManagerLifecycleTest : public ::testing::Test {
 protected:
  /**
   * @brief Removes stale lifecycle operation state before each test.
   *
   * @throws Nothing under current registry behavior.
   * @note This protects the tests from previous failed runs in the same
   * process.
   */
  void SetUp() override {
    OpRegistry::instance().unregister_key(kLifecycleKey);
  }

  /**
   * @brief Removes lifecycle operation state after each test.
   *
   * @throws Nothing under current registry behavior.
   * @note The fixture does not own plugin handles; each test unloads through
   * its local `PluginManager` before teardown.
   */
  void TearDown() override {
    OpRegistry::instance().unregister_key(kLifecycleKey);
  }
};

}  // namespace

TEST_F(PluginManagerLifecycleTest,
       LoadRetainsHandleAndUnloadRemovesMultiImplKey) {
  const auto plugin_path = lifecycle_plugin_path();
  ASSERT_TRUE(std::filesystem::exists(plugin_path))
      << "lifecycle op plugin was not built: " << plugin_path;

  PluginManager manager;
  const auto result =
      manager.load_from_dirs_report({plugin_path.parent_path().string()});

  EXPECT_EQ(result.attempted, 1);
  EXPECT_EQ(result.loaded, 1) << describe_errors(result.errors);
  EXPECT_TRUE(result.errors.empty()) << describe_errors(result.errors);
  EXPECT_TRUE(result_contains_lifecycle_key(result));
  EXPECT_EQ(manager.loaded_plugin_count(), 1u);

  const std::string absolute_path =
      std::filesystem::absolute(plugin_path).string();
  auto source_it = manager.op_sources().find(kLifecycleKey);
  ASSERT_NE(source_it, manager.op_sources().end());
  EXPECT_EQ(source_it->second, absolute_path);

  EXPECT_TRUE(OpRegistry::instance()
                  .resolve_for_intent(kLifecycleType, kLifecycleSubtype,
                                      ComputeIntent::GlobalHighPrecision)
                  .has_value());

  EXPECT_EQ(manager.unload_by_plugin_path(absolute_path), 1);
  EXPECT_EQ(manager.loaded_plugin_count(), 0u);
  EXPECT_EQ(manager.op_sources().count(kLifecycleKey), 0u);
  EXPECT_FALSE(OpRegistry::instance()
                   .resolve_for_intent(kLifecycleType, kLifecycleSubtype,
                                       ComputeIntent::GlobalHighPrecision)
                   .has_value());
}

TEST_F(PluginManagerLifecycleTest, UnloadAllPluginsReleasesRetainedHandles) {
  const auto plugin_path = lifecycle_plugin_path();
  ASSERT_TRUE(std::filesystem::exists(plugin_path))
      << "lifecycle op plugin was not built: " << plugin_path;

  PluginManager manager;
  const auto result =
      manager.load_from_dirs_report({plugin_path.parent_path().string()});

  ASSERT_EQ(result.loaded, 1) << describe_errors(result.errors);
  ASSERT_TRUE(result.errors.empty()) << describe_errors(result.errors);
  ASSERT_EQ(manager.loaded_plugin_count(), 1u);

  EXPECT_EQ(manager.unload_all_plugins(), 1);
  EXPECT_EQ(manager.loaded_plugin_count(), 0u);
  EXPECT_EQ(manager.op_sources().count(kLifecycleKey), 0u);
  EXPECT_FALSE(OpRegistry::instance()
                   .resolve_for_intent(kLifecycleType, kLifecycleSubtype,
                                       ComputeIntent::GlobalHighPrecision)
                   .has_value());
}

TEST_F(PluginManagerLifecycleTest,
       UnloadReplacementPluginRestoresOverwrittenOperation) {
  const auto plugin_path = lifecycle_plugin_path();
  const auto override_path = override_lifecycle_plugin_path();
  ASSERT_TRUE(std::filesystem::exists(plugin_path))
      << "lifecycle op plugin was not built: " << plugin_path;
  ASSERT_TRUE(std::filesystem::exists(override_path))
      << "override lifecycle op plugin was not built: " << override_path;

  PluginManager manager;
  const auto first_result =
      manager.load_from_dirs_report({plugin_path.parent_path().string()});
  ASSERT_EQ(first_result.loaded, 1) << describe_errors(first_result.errors);
  ASSERT_TRUE(first_result.errors.empty())
      << describe_errors(first_result.errors);
  EXPECT_TRUE(result_contains_lifecycle_key(first_result));
  EXPECT_EQ(current_lifecycle_compute_device(), "PLUGIN_LIFECYCLE_TEST");

  const std::string original_path =
      std::filesystem::absolute(plugin_path).string();
  ASSERT_EQ(manager.op_sources().at(kLifecycleKey), original_path);

  const auto replacement_result =
      manager.load_from_dirs_report({override_path.parent_path().string()});
  ASSERT_EQ(replacement_result.loaded, 1)
      << describe_errors(replacement_result.errors);
  ASSERT_TRUE(replacement_result.errors.empty())
      << describe_errors(replacement_result.errors);
  EXPECT_TRUE(result_contains_lifecycle_key(replacement_result));
  ASSERT_EQ(manager.loaded_plugin_count(), 2u);
  EXPECT_EQ(current_lifecycle_compute_device(), "PLUGIN_OVERRIDE_TEST");

  const std::string replacement_path =
      std::filesystem::absolute(override_path).string();
  ASSERT_EQ(manager.op_sources().at(kLifecycleKey), replacement_path);

  EXPECT_EQ(manager.unload_by_plugin_path(replacement_path), 1);
  EXPECT_EQ(manager.loaded_plugin_count(), 1u);
  ASSERT_EQ(manager.op_sources().at(kLifecycleKey), original_path);
  EXPECT_EQ(current_lifecycle_compute_device(), "PLUGIN_LIFECYCLE_TEST");

  EXPECT_EQ(manager.unload_by_plugin_path(original_path), 1);
  EXPECT_EQ(manager.loaded_plugin_count(), 0u);
  EXPECT_EQ(manager.op_sources().count(kLifecycleKey), 0u);
  EXPECT_FALSE(OpRegistry::instance()
                   .resolve_for_intent(kLifecycleType, kLifecycleSubtype,
                                       ComputeIntent::GlobalHighPrecision)
                   .has_value());
}

TEST_F(PluginManagerLifecycleTest,
       UnloadShadowedPluginDropsDependentRestorationSnapshot) {
  const auto plugin_path = lifecycle_plugin_path();
  const auto override_path = override_lifecycle_plugin_path();
  ASSERT_TRUE(std::filesystem::exists(plugin_path))
      << "lifecycle op plugin was not built: " << plugin_path;
  ASSERT_TRUE(std::filesystem::exists(override_path))
      << "override lifecycle op plugin was not built: " << override_path;

  PluginManager manager;
  const auto first_result =
      manager.load_from_dirs_report({plugin_path.parent_path().string()});
  ASSERT_EQ(first_result.loaded, 1) << describe_errors(first_result.errors);
  ASSERT_TRUE(first_result.errors.empty())
      << describe_errors(first_result.errors);

  const std::string original_path =
      std::filesystem::absolute(plugin_path).string();
  ASSERT_EQ(manager.op_sources().at(kLifecycleKey), original_path);
  EXPECT_EQ(current_lifecycle_compute_device(), "PLUGIN_LIFECYCLE_TEST");

  const auto replacement_result =
      manager.load_from_dirs_report({override_path.parent_path().string()});
  ASSERT_EQ(replacement_result.loaded, 1)
      << describe_errors(replacement_result.errors);
  ASSERT_TRUE(replacement_result.errors.empty())
      << describe_errors(replacement_result.errors);

  const std::string replacement_path =
      std::filesystem::absolute(override_path).string();
  ASSERT_EQ(manager.loaded_plugin_count(), 2u);
  ASSERT_EQ(manager.op_sources().at(kLifecycleKey), replacement_path);
  EXPECT_EQ(current_lifecycle_compute_device(), "PLUGIN_OVERRIDE_TEST");

  EXPECT_EQ(manager.unload_by_plugin_path(original_path), 0);
  EXPECT_EQ(manager.loaded_plugin_count(), 1u);
  ASSERT_EQ(manager.op_sources().at(kLifecycleKey), replacement_path);
  EXPECT_EQ(current_lifecycle_compute_device(), "PLUGIN_OVERRIDE_TEST");

  EXPECT_EQ(manager.unload_by_plugin_path(replacement_path), 1);
  EXPECT_EQ(manager.loaded_plugin_count(), 0u);
  EXPECT_EQ(manager.op_sources().count(kLifecycleKey), 0u);
  EXPECT_FALSE(OpRegistry::instance()
                   .resolve_for_intent(kLifecycleType, kLifecycleSubtype,
                                       ComputeIntent::GlobalHighPrecision)
                   .has_value());
}

TEST_F(PluginManagerLifecycleTest,
       StandardOperationPluginsRegisterExplicitRoiContracts) {
  OpRegistry::instance().unregister_key("image_process:invert");
  OpRegistry::instance().unregister_key("image_process:threshold");
  OpRegistry::instance().unregister_key("io:save");
  OpRegistry::instance().unregister_key("image_generator:perlin_noise_metal");

  ASSERT_TRUE(std::filesystem::exists(kStandardPluginDir))
      << "standard operation plugin directory was not built: "
      << kStandardPluginDir;

  PluginManager manager;
  const auto result = manager.load_from_dirs_report({kStandardPluginDir});

  EXPECT_GE(result.loaded, 3) << describe_errors(result.errors);
  EXPECT_TRUE(result.errors.empty()) << describe_errors(result.errors);
  EXPECT_TRUE(result_contains_key(result, "image_process:invert"));
  EXPECT_TRUE(result_contains_key(result, "image_process:threshold"));
  EXPECT_TRUE(result_contains_key(result, "io:save"));
  const bool metal_perlin_loaded =
      result_contains_key(result, "image_generator:perlin_noise_metal");

  expect_explicit_roi_contract("image_process", "invert");
  expect_explicit_roi_contract("image_process", "threshold");
  expect_explicit_roi_contract("io", "save");
  if (metal_perlin_loaded) {
    expect_explicit_roi_contract("image_generator", "perlin_noise_metal");
  }

  EXPECT_GE(manager.unload_all_plugins(), 3);
  EXPECT_EQ(OpRegistry::instance().dirty_propagation_contract_status(
                "image_process", "invert"),
            PropagationContractStatus::LegacyIdentityFallback);
}

}  // namespace ps
