#include <gtest/gtest.h>

#include <algorithm>
#include <filesystem>
#include <sstream>
#include <string>
#include <vector>

#include "kernel/plugin_manager.hpp"
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
 * @return True when `new_op_keys` contains `plugin_lifecycle:op`.
 * @throws Nothing.
 * @note The lifecycle plugin registers through `impl_table_`, so this also
 * proves loader key discovery observes multi-implementation registrations.
 */
bool result_contains_lifecycle_key(const PluginLoadResult& result) {
  return std::find(result.new_op_keys.begin(), result.new_op_keys.end(),
                   kLifecycleKey) != result.new_op_keys.end();
}

/**
 * @brief Checks whether a load result reports a standard plugin operation key.
 *
 * @param result Plugin load result to inspect.
 * @param key Canonical `type:subtype` operation key.
 * @return True when `new_op_keys` contains `key`.
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
