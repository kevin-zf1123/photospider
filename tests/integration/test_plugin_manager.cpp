#include <gtest/gtest.h>

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <future>
#include <optional>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>
#include <variant>
#include <vector>

#include "core/ps_types.hpp"          // NOLINT(build/include_subdir)
#include "graph/node.hpp"             // NOLINT(build/include_subdir)
#include "plugin/plugin_manager.hpp"  // NOLINT(build/include_subdir)

#ifndef PS_TEST_LIFECYCLE_OPERATION_PLUGIN
#error "PS_TEST_LIFECYCLE_OPERATION_PLUGIN must name the lifecycle fixture"
#endif

#ifndef PS_TEST_OVERRIDE_OPERATION_PLUGIN
#error "PS_TEST_OVERRIDE_OPERATION_PLUGIN must name the override fixture"
#endif

#ifndef PS_TEST_UNSUPPORTED_OPERATION_PLUGIN
#error "PS_TEST_UNSUPPORTED_OPERATION_PLUGIN must name the version fixture"
#endif

namespace ps {
namespace {

/** @brief Canonical key shared by the predecessor test generations. */
constexpr const char* kLifecycleKey = "plugin_lifecycle:op";
/** @brief Operation type shared by the predecessor test generations. */
constexpr const char* kLifecycleType = "plugin_lifecycle";
/** @brief Operation subtype shared by the predecessor test generations. */
constexpr const char* kLifecycleSubtype = "op";
/** @brief Lifecycle trace environment owned by serial integration tests. */
constexpr const char* kTraceEnvironment = "PS_LIFECYCLE_PLUGIN_TRACE";
/** @brief In-flight callback release-file environment. */
constexpr const char* kReleaseEnvironment{
    "PS_LIFECYCLE_PLUGIN_CALLBACK_RELEASE_FILE",
};
/** @brief Unsupported-root invocation marker environment. */
constexpr const char* kUnsupportedMarkerEnvironment{
    "PS_UNSUPPORTED_OPERATION_ABI_MARKER",
};

/**
 * @brief Restores one process environment variable after a test scope.
 * @throws std::bad_alloc when preserving the previous value allocates.
 * @note Tests using this helper are registered serially on Linux because
 * process environment mutation is global.
 */
class ScopedEnvironment final {
 public:
  /**
   * @brief Replaces one process environment variable.
   * @param name Stable nonempty environment name.
   * @param value Replacement value.
   * @throws std::runtime_error when the platform rejects the update.
   */
  ScopedEnvironment(const char* name, std::string value)
      : name_(name), previous_(read(name)) {
    write(name_.c_str(), value.c_str());
  }

  /** @brief Restores the previous environment state without throwing. */
  ~ScopedEnvironment() noexcept {
    if (previous_) {
      write_noexcept(name_.c_str(), previous_->c_str());
    } else {
      erase_noexcept(name_.c_str());
    }
  }

  ScopedEnvironment(const ScopedEnvironment&) = delete;
  ScopedEnvironment& operator=(const ScopedEnvironment&) = delete;

 private:
  /**
   * @brief Copies one current environment value.
   * @param name Environment name.
   * @return Owned value or nullopt when absent.
   * @throws std::bad_alloc when value copying allocates.
   */
  static std::optional<std::string> read(const char* name) {
    const char* value = std::getenv(name);
    return value == nullptr ? std::nullopt : std::optional<std::string>(value);
  }

  /**
   * @brief Writes one environment value or throws.
   * @param name Environment name.
   * @param value Replacement value.
   * @return Nothing.
   * @throws std::runtime_error on platform failure.
   */
  static void write(const char* name, const char* value) {
#if defined(_WIN32)
    if (_putenv_s(name, value) != 0) {
#else
    if (setenv(name, value, 1) != 0) {
#endif
      throw std::runtime_error("failed to set plugin-manager test environment");
    }
  }

  /**
   * @brief Best-effort environment replacement during destruction.
   * @param name Environment name.
   * @param value Replacement value.
   * @return Nothing.
   * @throws Nothing.
   */
  static void write_noexcept(const char* name, const char* value) noexcept {
#if defined(_WIN32)
    (void)_putenv_s(name, value);
#else
    (void)setenv(name, value, 1);
#endif
  }

  /**
   * @brief Best-effort environment removal during destruction.
   * @param name Environment name.
   * @return Nothing.
   * @throws Nothing.
   */
  static void erase_noexcept(const char* name) noexcept {
#if defined(_WIN32)
    (void)_putenv_s(name, "");
#else
    (void)unsetenv(name);
#endif
  }

  /** @brief Owned environment name. */
  std::string name_;
  /** @brief Prior value, absent when the variable did not exist. */
  std::optional<std::string> previous_;
};  // NOLINT(readability/braces)

/**
 * @brief Returns one collision-resistant temporary test path.
 * @param suffix Human-readable file suffix.
 * @return Path below the platform temporary directory.
 * @throws Filesystem or allocation failures unchanged.
 */
std::filesystem::path temporary_path(const char* suffix) {
  const auto stamp =
      std::chrono::steady_clock::now().time_since_epoch().count();
  return std::filesystem::temp_directory_path() /
         (std::string("photospider-plugin-manager-") + std::to_string(stamp) +
          "-" + suffix);
}

/**
 * @brief Reads every complete lifecycle trace event.
 * @param path Trace file path.
 * @return Events in append order.
 * @throws Stream or allocation failures unchanged.
 */
std::vector<std::string> read_trace(const std::filesystem::path& path) {
  std::ifstream input(path);
  std::vector<std::string> events;
  for (std::string line; std::getline(input, line);) {
    events.push_back(std::move(line));
  }
  return events;
}

/**
 * @brief Waits for one lifecycle event with a bounded deadline.
 * @param path Trace file path.
 * @param event Exact event to observe.
 * @return True when observed before the deadline.
 * @throws Stream or allocation failures unchanged.
 */
bool wait_for_event(const std::filesystem::path& path,
                    const std::string& event) {
  const auto deadline =
      std::chrono::steady_clock::now() + std::chrono::seconds(3);
  while (std::chrono::steady_clock::now() < deadline) {
    const auto events = read_trace(path);
    if (std::find(events.begin(), events.end(), event) != events.end()) {
      return true;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(2));
  }
  return false;
}

/**
 * @brief Loads the only candidate in one fixture directory.
 * @param manager Process operation-plugin owner.
 * @param plugin_path Exact fixture library path.
 * @return Structured single-candidate load report.
 * @throws Loader, filesystem, or allocation failures unchanged.
 */
PluginLoadResult load_fixture(PluginManager& manager,
                              const std::filesystem::path& plugin_path) {
  return manager.load_from_dirs_report({plugin_path.parent_path().string()});
}

/**
 * @brief Registers one direct predecessor for the lifecycle plugin key.
 * @return Selected nonzero implementation revision.
 * @throws Registry validation or allocation failures unchanged.
 */
std::uint64_t register_direct_predecessor() {
  OpMetadata metadata;
  metadata.produces_image = false;
  metadata.cost_score = 50;
  OpRegistry::instance().register_op_hp_monolithic(
      kLifecycleType, kLifecycleSubtype,
      [](const Node&, const std::vector<const NodeOutput*>&) {
        NodeOutput output;
        output.debug.compute_device = "DIRECT_PREDECESSOR";
        return output;
      },
      metadata);
  const auto selected = OpRegistry::instance().select_implementation(
      kLifecycleType, kLifecycleSubtype, {Device::CPU},
      ComputeIntent::GlobalHighPrecision);
  if (!selected || selected->implementation_identity == 0U) {
    throw std::runtime_error("direct predecessor revision was not published");
  }
  return selected->implementation_identity;
}

/**
 * @brief Returns the active lifecycle marker and implementation revision.
 * @return Pair of callback marker and nonzero selected revision.
 * @throws std::runtime_error when the expected monolithic slot is absent.
 * @throws Callback, registry-copy, or allocation failures unchanged.
 */
std::pair<std::string, std::uint64_t> active_lifecycle_generation() {
  const auto callback = OpRegistry::instance().resolve_for_intent(
      kLifecycleType, kLifecycleSubtype, ComputeIntent::GlobalHighPrecision);
  const auto selected = OpRegistry::instance().select_implementation(
      kLifecycleType, kLifecycleSubtype, {Device::CPU},
      ComputeIntent::GlobalHighPrecision);
  if (!callback || !std::holds_alternative<MonolithicOpFunc>(*callback) ||
      !selected || selected->implementation_identity == 0U) {
    throw std::runtime_error("lifecycle generation is not monolithic");
  }
  Node node;
  node.id = 1;
  node.type = kLifecycleType;
  node.subtype = kLifecycleSubtype;
  NodeOutput output = std::get<MonolithicOpFunc>(*callback)(node, {});
  return {output.debug.compute_device, selected->implementation_identity};
}

/**
 * @brief Isolates the process-global manager and lifecycle registry key.
 * @note Every test removes dynamic generations and the direct predecessor.
 */
class PluginManagerPureCAbiTest : public ::testing::Test {
 protected:
  /** @brief Clears stale state before a test without retaining callbacks. */
  void SetUp() override {
    (void)PluginManager::process_instance().unload_all_plugins();
    (void)OpRegistry::instance().unregister_key(kLifecycleKey);
  }

  /** @brief Clears all state after a test without throwing. */
  void TearDown() override {
    (void)PluginManager::process_instance().unload_all_plugins();
    (void)OpRegistry::instance().unregister_key(kLifecycleKey);
  }
};

/**
 * @brief Proves numeric discovery rejects a foreign generation atomically.
 * @throws Nothing when the report and process state remain unchanged.
 */
TEST_F(PluginManagerPureCAbiTest,
       UnsupportedNumericVersionNeverNegotiatesOrPublishes) {
  const std::filesystem::path plugin_path =
      PS_TEST_UNSUPPORTED_OPERATION_PLUGIN;
  ASSERT_TRUE(std::filesystem::exists(plugin_path));
  const auto marker_path = temporary_path("unsupported-api.txt");
  std::filesystem::remove(marker_path);
  ScopedEnvironment marker_environment(kUnsupportedMarkerEnvironment,
                                       marker_path.string());

  auto& manager = PluginManager::process_instance();
  manager.seed_builtins_from_registry();
  const auto keys_before = manager.combined_keys();
  const auto sources_before = manager.op_sources();
  const std::size_t handles_before = manager.loaded_plugin_count();
  const PluginLoadResult result = load_fixture(manager, plugin_path);

  EXPECT_EQ(result.attempted, 1);
  EXPECT_EQ(result.loaded, 0);
  ASSERT_EQ(result.errors.size(), 1U);
  EXPECT_NE(result.errors.front().message.find("numeric ABI version"),
            std::string::npos);
  EXPECT_TRUE(result.new_op_keys.empty());
  EXPECT_EQ(manager.combined_keys(), keys_before);
  EXPECT_EQ(manager.op_sources(), sources_before);
  EXPECT_EQ(manager.loaded_plugin_count(), handles_before);
  EXPECT_FALSE(std::filesystem::exists(marker_path));
}

/**
 * @brief Proves middle-generation removal splices the exact predecessor.
 * @throws Nothing when identities, visibility, and DSO retirement agree.
 */
TEST_F(PluginManagerPureCAbiTest,
       MiddleGenerationSplicePreservesNewestThenRestoresPredecessor) {
  const std::filesystem::path lifecycle_path =
      PS_TEST_LIFECYCLE_OPERATION_PLUGIN;
  const std::filesystem::path override_path = PS_TEST_OVERRIDE_OPERATION_PLUGIN;
  ASSERT_TRUE(std::filesystem::exists(lifecycle_path));
  ASSERT_TRUE(std::filesystem::exists(override_path));
  const auto trace_path = temporary_path("middle-trace.txt");
  std::filesystem::remove(trace_path);
  ScopedEnvironment trace_environment(kTraceEnvironment, trace_path.string());

  auto& manager = PluginManager::process_instance();
  manager.seed_builtins_from_registry();
  const std::uint64_t predecessor_revision = register_direct_predecessor();
  ASSERT_EQ(active_lifecycle_generation().first, "DIRECT_PREDECESSOR");

  const PluginLoadResult first = load_fixture(manager, lifecycle_path);
  ASSERT_EQ(first.loaded, 1)
      << (first.errors.empty() ? std::string() : first.errors.front().message);
  const auto lifecycle = active_lifecycle_generation();
  EXPECT_EQ(lifecycle.first, "PLUGIN_LIFECYCLE_TEST");
  EXPECT_NE(lifecycle.second, predecessor_revision);

  const PluginLoadResult second = load_fixture(manager, override_path);
  ASSERT_EQ(second.loaded, 1)
      << (second.errors.empty() ? std::string()
                                : second.errors.front().message);
  const auto replacement = active_lifecycle_generation();
  EXPECT_EQ(replacement.first, "PLUGIN_OVERRIDE_TEST");
  EXPECT_NE(replacement.second, lifecycle.second);

  EXPECT_EQ(manager.unload_by_plugin_path(
                std::filesystem::absolute(lifecycle_path).string()),
            0);
  const auto after_middle_unload = active_lifecycle_generation();
  EXPECT_EQ(after_middle_unload, replacement);
  EXPECT_EQ(manager.loaded_plugin_count(), 1U);
  ASSERT_TRUE(wait_for_event(trace_path, "library_unload"));

  EXPECT_EQ(manager.unload_by_plugin_path(
                std::filesystem::absolute(override_path).string()),
            1);
  const auto restored = active_lifecycle_generation();
  EXPECT_EQ(restored.first, "DIRECT_PREDECESSOR");
  EXPECT_EQ(restored.second, predecessor_revision);
  EXPECT_EQ(manager.loaded_plugin_count(), 0U);
  ASSERT_TRUE(wait_for_event(trace_path, "override_library_unload"));
}

/**
 * @brief Proves unload-all retires successful generations in reverse order.
 * @throws Nothing when trace order and predecessor revision are exact.
 */
TEST_F(PluginManagerPureCAbiTest,
       UnloadAllUsesReverseSuccessOrderAndRestoresDirectGeneration) {
  const std::filesystem::path lifecycle_path =
      PS_TEST_LIFECYCLE_OPERATION_PLUGIN;
  const std::filesystem::path override_path = PS_TEST_OVERRIDE_OPERATION_PLUGIN;
  const auto trace_path = temporary_path("reverse-trace.txt");
  std::filesystem::remove(trace_path);
  ScopedEnvironment trace_environment(kTraceEnvironment, trace_path.string());

  auto& manager = PluginManager::process_instance();
  manager.seed_builtins_from_registry();
  const std::uint64_t predecessor_revision = register_direct_predecessor();
  ASSERT_EQ(load_fixture(manager, lifecycle_path).loaded, 1);
  ASSERT_EQ(load_fixture(manager, override_path).loaded, 1);
  EXPECT_EQ(manager.loaded_plugin_count(), 2U);

  EXPECT_EQ(manager.unload_all_plugins(), 2);
  EXPECT_EQ(manager.loaded_plugin_count(), 0U);
  const auto restored = active_lifecycle_generation();
  EXPECT_EQ(restored.first, "DIRECT_PREDECESSOR");
  EXPECT_EQ(restored.second, predecessor_revision);
  ASSERT_TRUE(wait_for_event(trace_path, "library_unload"));

  const auto events = read_trace(trace_path);
  const auto override_destroy =
      std::find(events.begin(), events.end(), "override_callback_destroy");
  const auto override_unload =
      std::find(events.begin(), events.end(), "override_library_unload");
  const auto predecessor_destroy =
      std::find(events.begin(), events.end(), "plugin_destroy");
  const auto predecessor_unload =
      std::find(events.begin(), events.end(), "library_unload");
  ASSERT_NE(override_destroy, events.end());
  ASSERT_NE(override_unload, events.end());
  ASSERT_NE(predecessor_destroy, events.end());
  ASSERT_NE(predecessor_unload, events.end());
  EXPECT_LT(override_destroy, override_unload);
  EXPECT_LT(override_unload, predecessor_destroy);
  EXPECT_LT(predecessor_destroy, predecessor_unload);
  EXPECT_EQ(std::count(events.begin(), events.end(), "plugin_destroy"), 1);
  EXPECT_EQ(
      std::count(events.begin(), events.end(), "override_callback_destroy"), 1);
}

/**
 * @brief Proves manager unload cannot unmap an in-flight pure-C callback.
 * @throws Nothing when callback/result leases delay exact-once destruction.
 */
TEST_F(PluginManagerPureCAbiTest,
       InFlightCallbackAndReturnedValueRetainGenerationLease) {
  const std::filesystem::path lifecycle_path =
      PS_TEST_LIFECYCLE_OPERATION_PLUGIN;
  const auto trace_path = temporary_path("inflight-trace.txt");
  const auto release_path = temporary_path("release.txt");
  std::filesystem::remove(trace_path);
  std::filesystem::remove(release_path);
  ScopedEnvironment trace_environment(kTraceEnvironment, trace_path.string());
  ScopedEnvironment release_environment(kReleaseEnvironment,
                                        release_path.string());

  auto& manager = PluginManager::process_instance();
  manager.seed_builtins_from_registry();
  ASSERT_EQ(load_fixture(manager, lifecycle_path).loaded, 1);
  auto resolved = OpRegistry::instance().resolve_for_intent(
      kLifecycleType, kLifecycleSubtype, ComputeIntent::GlobalHighPrecision);
  ASSERT_TRUE(resolved.has_value());
  ASSERT_TRUE(std::holds_alternative<MonolithicOpFunc>(*resolved));
  MonolithicOpFunc callback = std::move(std::get<MonolithicOpFunc>(*resolved));
  resolved.reset();

  auto invocation = std::async(std::launch::async,
                               [callback = std::move(callback)]() mutable {
                                 Node node;
                                 node.id = 1;
                                 node.type = kLifecycleType;
                                 node.subtype = kLifecycleSubtype;
                                 return callback(node, {});
                               });
  callback = MonolithicOpFunc{};
  if (!wait_for_event(trace_path, "callback_enter")) {
    std::ofstream(release_path).put('\n');
    invocation.wait();
    FAIL() << "lifecycle callback did not enter release barrier";
  }

  EXPECT_EQ(manager.unload_by_plugin_path(
                std::filesystem::absolute(lifecycle_path).string()),
            1);
  EXPECT_EQ(manager.loaded_plugin_count(), 0U);
  auto events = read_trace(trace_path);
  EXPECT_EQ(std::find(events.begin(), events.end(), "library_unload"),
            events.end());
  std::ofstream(release_path).put('\n');
  NodeOutput output = invocation.get();
  EXPECT_EQ(output.debug.compute_device, "PLUGIN_LIFECYCLE_TEST");
  events = read_trace(trace_path);
  EXPECT_EQ(std::find(events.begin(), events.end(), "library_unload"),
            events.end());

  output = NodeOutput{};
  ASSERT_TRUE(wait_for_event(trace_path, "library_unload"));
  events = read_trace(trace_path);
  EXPECT_EQ(std::count(events.begin(), events.end(), "callback_destroy"), 1);
  EXPECT_EQ(std::count(events.begin(), events.end(), "plugin_destroy"), 1);
  EXPECT_EQ(std::count(events.begin(), events.end(), "library_unload"), 1);
  std::filesystem::remove(release_path);
}

}  // namespace
}  // namespace ps
