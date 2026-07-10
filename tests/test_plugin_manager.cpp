#include <gtest/gtest.h>

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <map>
#include <memory>
#include <optional>
#include <sstream>
#include <string>
#include <utility>
#include <variant>
#include <vector>

#include "kernel/plugin_manager.hpp"
#if defined(PHOTOSPIDER_INTERNAL_BAD_ALLOC_TESTING)
#include "kernel/plugin_manage_module/plugin_loader_test_access.hpp"
#endif
#include "node.hpp"      // NOLINT(build/include_subdir)
#include "ps_types.hpp"  // NOLINT(build/include_subdir)

namespace plugin_cleanup_allocation_probe {

/** @brief Disabled current-thread allocation failure sentinel. */
constexpr std::int64_t kDisabled = -1;
/** @brief Zero-based allocation countdown for the current test thread. */
thread_local std::int64_t countdown = kDisabled;
/** @brief Whether an allocation was rejected after the most recent arm. */
thread_local bool fired = false;

/** @brief Arms one allocation failure at `allocation_index`. */
void arm(std::int64_t allocation_index) noexcept {
  countdown = allocation_index;
  fired = false;
}

/** @brief Disarms current-thread allocation injection. */
void disarm() noexcept {
  countdown = kDisabled;
}

/** @brief Returns whether the armed failure has fired. */
bool did_fire() noexcept {
  return fired;
}

/**
 * @brief Rejects the selected allocation.
 * @return Nothing when allocation may continue.
 * @throws std::bad_alloc at the armed allocation index.
 */
void maybe_fail() {
  if (countdown < 0) {
    return;
  }
  if (countdown == 0) {
    countdown = kDisabled;
    fired = true;
    throw std::bad_alloc{};
  }
  --countdown;
}

}  // namespace plugin_cleanup_allocation_probe

/**
 * @brief Test-executable allocator used to audit no-allocation plugin cleanup.
 * @param size Requested allocation size.
 * @return malloc-backed storage.
 * @throws std::bad_alloc when injected or malloc fails.
 */
void* operator new(std::size_t size) {
  plugin_cleanup_allocation_probe::maybe_fail();
  if (void* memory = std::malloc(size == 0 ? 1 : size)) {
    return memory;
  }
  throw std::bad_alloc{};
}

/** @copydoc operator new(std::size_t) */
void* operator new[](std::size_t size) {
  return ::operator new(size);
}
/** @brief Releases scalar storage allocated by the test allocator. */
void operator delete(void* memory) noexcept {
  std::free(memory);
}
/** @brief Releases array storage allocated by the test allocator. */
void operator delete[](void* memory) noexcept {
  std::free(memory);
}
/** @brief Releases sized scalar storage allocated by the test allocator. */
void operator delete(void* memory, std::size_t) noexcept {
  std::free(memory);
}
/** @brief Releases sized array storage allocated by the test allocator. */
void operator delete[](void* memory, std::size_t) noexcept {
  std::free(memory);
}

namespace ps {
namespace {

constexpr const char* kLifecycleType = "plugin_lifecycle";
constexpr const char* kLifecycleSubtype = "op";
constexpr const char* kLifecycleKey = "plugin_lifecycle:op";
constexpr const char* kLifecycleTraceEnvironment = "PS_LIFECYCLE_PLUGIN_TRACE";
constexpr const char* kLifecycleThrowEnvironment =
    "PS_LIFECYCLE_PLUGIN_REGISTRAR_THROW";  // NOLINT(whitespace/indent_namespace)

#ifndef PS_STANDARD_OP_PLUGIN_DIR
#define PS_STANDARD_OP_PLUGIN_DIR "build/plugins"
#endif

#ifndef PS_TEST_OP_PLUGIN_DIR
#define PS_TEST_OP_PLUGIN_DIR "build/test_plugins"
#endif

/**
 * @brief Returns the CMake output root for test-only operation plugins.
 *
 * @return Filesystem path injected by CMake for this test target, or the legacy
 * `build/test_plugins` fallback when the source is compiled outside CMake.
 * @throws std::bad_alloc from path string construction.
 * @note The value is an absolute path in normal CMake builds, which lets the
 * test run from any working directory and from nested build trees such as
 * `build/ci`.
 */
std::filesystem::path test_op_plugin_dir() {
  return std::filesystem::path(PS_TEST_OP_PLUGIN_DIR);
}

/**
 * @brief Returns the CMake output directory for standard operation plugins.
 *
 * @return Filesystem path injected by CMake for standard plugin shared
 * libraries, or the legacy `build/plugins` fallback outside CMake.
 * @throws std::bad_alloc from path string construction.
 * @note Plugin manager lifecycle tests intentionally consume the build output
 * directory instead of a source-relative `build/` path so CI jobs can choose
 * any binary directory.
 */
std::filesystem::path standard_plugin_dir() {
  return std::filesystem::path(PS_STANDARD_OP_PLUGIN_DIR);
}

/**
 * @brief Returns the build output path for the lifecycle op plugin fixture.
 *
 * @return Platform-specific shared library path under `test_op_plugin_dir()`.
 * @throws std::bad_alloc from path string construction.
 * @note The plugin directory comes from the test target's CMake definitions so
 * the fixture follows the active binary directory rather than a hard-coded
 * `build/` tree.
 */
std::filesystem::path lifecycle_plugin_path() {
  const std::filesystem::path dir = test_op_plugin_dir() / "lifecycle";
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
 * @return Platform-specific shared library path under `test_op_plugin_dir()`.
 * @throws std::bad_alloc from path string construction.
 * @note The replacement plugin intentionally registers the same operation key
 * as `lifecycle_op_plugin`, and its directory is injected by CMake for the
 * active build tree.
 */
std::filesystem::path override_lifecycle_plugin_path() {
  const std::filesystem::path dir = test_op_plugin_dir() / "override";
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
 * @brief Sets one process environment variable for a bounded fixture load.
 *
 * @note Destruction restores the prior value or removes the key. Tests are
 * process-serial because environment variables are process-global.
 */
class ScopedEnvironmentVariable final {
 public:
  /**
   * @brief Installs a process environment value and preserves its predecessor.
   * @param name Environment key copied for the guard lifetime.
   * @param value New value visible to the dynamic plugin.
   * @throws std::runtime_error if the platform environment update fails.
   * @throws std::bad_alloc if key or previous-value storage cannot allocate.
   * @note Construction completes only after the replacement is installed.
   */
  ScopedEnvironmentVariable(const char* name, const std::string& value)
      : name_(name) {
    if (const char* previous = std::getenv(name)) {
      previous_ = std::string(previous);
    }
    set(value);
  }

  /**
   * @brief Restores the prior process environment without throwing.
   * @throws Nothing; platform restoration failures are suppressed.
   * @note The destructor never changes plugin/registry ownership state.
   */
  ~ScopedEnvironmentVariable() {
    try {
      if (previous_) {
        set(*previous_);
      } else {
        clear();
      }
    } catch (...) {
      // Test cleanup cannot safely surface an environment restoration failure.
    }
  }

  ScopedEnvironmentVariable(const ScopedEnvironmentVariable&) = delete;
  ScopedEnvironmentVariable& operator=(const ScopedEnvironmentVariable&) =
      delete;

 private:
  /**
   * @brief Replaces the owned environment key.
   *
   * @param value New value.
   * @return Nothing.
   * @throws std::runtime_error when the platform call fails.
   * @note The process-global key is the one copied during construction.
   */
  void set(const std::string& value) {
#if defined(_WIN32)
    if (_putenv_s(name_.c_str(), value.c_str()) != 0) {
      throw std::runtime_error("_putenv_s failed");
    }
#else
    if (setenv(name_.c_str(), value.c_str(), 1) != 0) {
      throw std::runtime_error("setenv failed");
    }
#endif
  }

  /**
   * @brief Removes the owned environment key.
   *
   * @return Nothing.
   * @throws std::runtime_error when the platform call fails.
   * @note Removal affects process-global state and is used only by this guard.
   */
  void clear() {
#if defined(_WIN32)
    if (_putenv_s(name_.c_str(), "") != 0) {
      throw std::runtime_error("_putenv_s clear failed");
    }
#else
    if (unsetenv(name_.c_str()) != 0) {
      throw std::runtime_error("unsetenv failed");
    }
#endif
  }

  /** @brief Environment key owned for the guard lifetime. */
  std::string name_;
  /** @brief Previous value, or nullopt when the key was absent. */
  std::optional<std::string> previous_;
};

/**
 * @brief RAII owner for one current-thread plugin-cleanup allocation failure.
 * @note The scope owns no heap storage and always disarms before assertions.
 */
class ScopedPluginCleanupAllocationFailure final {
 public:
  /**
   * @brief Arms a zero-based allocation failure.
   * @param allocation_index Allocation to reject.
   * @throws Nothing.
   */
  explicit ScopedPluginCleanupAllocationFailure(
      std::int64_t allocation_index) noexcept {
    plugin_cleanup_allocation_probe::arm(allocation_index);
  }

  /** @brief Disarms allocation injection. */
  ~ScopedPluginCleanupAllocationFailure() {
    plugin_cleanup_allocation_probe::disarm();
  }

  ScopedPluginCleanupAllocationFailure(
      const ScopedPluginCleanupAllocationFailure&) = delete;
  ScopedPluginCleanupAllocationFailure& operator=(
      const ScopedPluginCleanupAllocationFailure&) = delete;
};

/**
 * @brief Returns a unique trace path for one lifecycle transaction scenario.
 *
 * @param label Stable scenario label used in the filename.
 * @return Path in GTest's temporary directory.
 * @throws std::bad_alloc from path/string construction.
 * @note Existing files are removed by the caller before installing the path in
 * the dynamic plugin's environment.
 */
std::filesystem::path lifecycle_trace_path(const std::string& label) {
  static std::atomic<unsigned int> sequence{0};
  return std::filesystem::path(::testing::TempDir()) /
         ("photospider-lifecycle-" + label + "-" +
          std::to_string(sequence.fetch_add(1, std::memory_order_relaxed)) +
          ".log");
}

/**
 * @brief Reads newline-delimited lifecycle events emitted by the real plugin.
 *
 * @param path Trace file selected through the fixture environment.
 * @return Events in actual destructor/registrar order.
 * @throws std::bad_alloc if line/vector storage cannot allocate.
 * @note A missing file yields an empty vector so the caller reports a focused
 * assertion rather than an unrelated stream exception.
 */
std::vector<std::string> read_lifecycle_trace(
    const std::filesystem::path& path) {
  std::vector<std::string> events;
  std::ifstream input(path);
  std::string line;
  while (std::getline(input, line)) {
    events.push_back(line);
  }
  return events;
}

/**
 * @brief Checks global registry key presence without copying plugin callbacks.
 *
 * @return True when the lifecycle key remains in the canonical key inventory.
 * @throws std::bad_alloc if registry key enumeration allocation fails.
 * @note Avoiding `resolve_for_intent` is intentional: a defective loader may
 * already have unloaded the library behind the stale callable.
 */
bool lifecycle_key_is_registered() {
  const auto keys = OpRegistry::instance().get_combined_keys();
  return std::find(keys.begin(), keys.end(), kLifecycleKey) != keys.end();
}

/**
 * @brief Consumes the currently armed allocation failure after cleanup.
 *
 * @return True when the probe still rejected this explicit allocation.
 * @throws Nothing; `std::bad_alloc` is converted to the boolean result.
 * @note A true result proves the preceding cleanup performed no C++ dynamic
 * allocation and therefore did not consume the zero-count failpoint.
 */
bool consume_armed_cleanup_allocation_failure() noexcept {
  try {
    void* memory = ::operator new(1);
    ::operator delete(memory);
  } catch (const std::bad_alloc&) {
    return true;
  }
  return false;
}

/**
 * @brief Installs a host-owned callback at the lifecycle plugin's target key.
 *
 * @return Nothing.
 * @throws std::bad_alloc if registry callback or metadata storage cannot grow.
 * @note A failed candidate load must preserve this exact active callback,
 * proving strong guarantee for replacement as well as insertion registration.
 */
void register_host_lifecycle_sentinel() {
  OpMetadata metadata;
  metadata.cost_score = 99;
  OpRegistry::instance().register_op_hp_monolithic(
      kLifecycleType, kLifecycleSubtype,
      [](const Node&, const std::vector<const NodeOutput*>&) {
        NodeOutput output;
        output.debug.compute_device = "HOST_LIFECYCLE_SENTINEL";
        return output;
      },
      metadata);
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
 * @note Standard plugins are loaded from `standard_plugin_dir()`, and this
 * helper keeps test assertions independent from platform-specific shared
 * library suffixes.
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

/**
 * @brief Proves unload-all unwinds a two-plugin override chain in load order.
 * @throws Nothing when the built-in callback/source are restored exactly.
 * @note Plugin filenames intentionally sort differently from dependency order.
 */
TEST_F(PluginManagerLifecycleTest,
       UnloadAllRestoresBuiltinThroughReverseLoadOrder) {
  const auto plugin_path = lifecycle_plugin_path();
  const auto override_path = override_lifecycle_plugin_path();
  ASSERT_TRUE(std::filesystem::exists(plugin_path));
  ASSERT_TRUE(std::filesystem::exists(override_path));

  register_host_lifecycle_sentinel();
  PluginManager manager;
  manager.seed_builtins_from_registry();

  const auto first_result =
      manager.load_from_dirs_report({plugin_path.parent_path().string()});
  ASSERT_EQ(first_result.loaded, 1) << describe_errors(first_result.errors);
  ASSERT_EQ(current_lifecycle_compute_device(), "PLUGIN_LIFECYCLE_TEST");

  const auto second_result =
      manager.load_from_dirs_report({override_path.parent_path().string()});
  ASSERT_EQ(second_result.loaded, 1) << describe_errors(second_result.errors);
  ASSERT_EQ(current_lifecycle_compute_device(), "PLUGIN_OVERRIDE_TEST");

  EXPECT_EQ(manager.unload_all_plugins(), 2);
  EXPECT_EQ(manager.loaded_plugin_count(), 0u);
  ASSERT_EQ(manager.op_sources().at(kLifecycleKey), "built-in");
  EXPECT_EQ(current_lifecycle_compute_device(), "HOST_LIFECYCLE_SENTINEL");
}

/**
 * @brief Proves explicit unload performs no allocation and preserves lifetime.
 * @throws Nothing when the armed allocation remains unconsumed.
 * @note The trace must destroy plugin callable state before library unload.
 */
TEST_F(PluginManagerLifecycleTest,
       UnloadAllAllocatesNothingAndDestroysCallbacksBeforeLibrary) {
  const auto plugin_path = lifecycle_plugin_path();
  ASSERT_TRUE(std::filesystem::exists(plugin_path));
  const auto trace_path = lifecycle_trace_path("unload-all-no-allocation");
  std::filesystem::remove(trace_path);
  ScopedEnvironmentVariable trace_environment(kLifecycleTraceEnvironment,
                                              trace_path.string());
  PluginManager manager;
  const auto result =
      manager.load_from_dirs_report({plugin_path.parent_path().string()});
  ASSERT_EQ(result.loaded, 1) << describe_errors(result.errors);

  int removed = -1;
  bool fired_during_cleanup = false;
  bool failure_remained_armed = false;
  {
    ScopedPluginCleanupAllocationFailure failure(0);
    removed = manager.unload_all_plugins();
    fired_during_cleanup = plugin_cleanup_allocation_probe::did_fire();
    failure_remained_armed = consume_armed_cleanup_allocation_failure();
  }

  EXPECT_EQ(removed, 1);
  EXPECT_FALSE(fired_during_cleanup);
  EXPECT_TRUE(failure_remained_armed);
  EXPECT_EQ(manager.loaded_plugin_count(), 0u);
  EXPECT_FALSE(lifecycle_key_is_registered());
  EXPECT_EQ(read_lifecycle_trace(trace_path),
            (std::vector<std::string>{"registrar_return", "callback_destroy",
                                      "library_unload"}));
  std::filesystem::remove(trace_path);
}

/**
 * @brief Proves exact-key single-plugin unload is allocation-independent.
 * @throws Nothing when cleanup leaves the allocation failpoint armed.
 * @note The exact absolute key is computed before injection and the lifecycle
 * trace must destroy callable state before releasing the dynamic library.
 */
TEST_F(PluginManagerLifecycleTest,
       ExactKeyUnloadAllocatesNothingAndDestroysCallbacksBeforeLibrary) {
  const auto plugin_path = lifecycle_plugin_path();
  ASSERT_TRUE(std::filesystem::exists(plugin_path));
  const std::string load_key = std::filesystem::absolute(plugin_path).string();
  const auto trace_path = lifecycle_trace_path("exact-key-no-allocation");
  std::filesystem::remove(trace_path);
  ScopedEnvironmentVariable trace_environment(kLifecycleTraceEnvironment,
                                              trace_path.string());
  PluginManager manager;
  const auto result =
      manager.load_from_dirs_report({plugin_path.parent_path().string()});
  ASSERT_EQ(result.loaded, 1) << describe_errors(result.errors);

  int removed = -1;
  bool fired_during_cleanup = false;
  bool failure_remained_armed = false;
  {
    ScopedPluginCleanupAllocationFailure failure(0);
    removed = manager.unload_by_plugin_path(load_key);
    fired_during_cleanup = plugin_cleanup_allocation_probe::did_fire();
    failure_remained_armed = consume_armed_cleanup_allocation_failure();
  }

  EXPECT_EQ(removed, 1);
  EXPECT_FALSE(fired_during_cleanup);
  EXPECT_TRUE(failure_remained_armed);
  EXPECT_EQ(manager.loaded_plugin_count(), 0u);
  EXPECT_FALSE(lifecycle_key_is_registered());
  EXPECT_EQ(read_lifecycle_trace(trace_path),
            (std::vector<std::string>{"registrar_return", "callback_destroy",
                                      "library_unload"}));
  std::filesystem::remove(trace_path);
}

/**
 * @brief Proves relative-path normalization failure has a strong guarantee.
 * @throws Nothing when the injected allocation failure is propagated exactly.
 * @note The allocation is injected before normalization; registry, source,
 * result, and retained handle state must remain unchanged.
 */
TEST_F(PluginManagerLifecycleTest,
       RelativePathNormalizationBadAllocLeavesPluginStateUnchanged) {
  const auto plugin_path = lifecycle_plugin_path();
  ASSERT_TRUE(std::filesystem::exists(plugin_path));
  const std::string load_key = std::filesystem::absolute(plugin_path).string();
  const std::string relative_path =
      std::filesystem::relative(plugin_path).string();
  ASSERT_NE(relative_path, load_key);
  PluginManager manager;
  const auto result =
      manager.load_from_dirs_report({plugin_path.parent_path().string()});
  ASSERT_EQ(result.loaded, 1) << describe_errors(result.errors);
  ASSERT_EQ(manager.op_sources().at(kLifecycleKey), load_key);

  bool caught_bad_alloc = false;
  bool allocation_failed = false;
  {
    ScopedPluginCleanupAllocationFailure failure(0);
    try {
      (void)manager.unload_by_plugin_path(relative_path);
    } catch (const std::bad_alloc&) {
      caught_bad_alloc = true;
    }
    allocation_failed = plugin_cleanup_allocation_probe::did_fire();
  }

  EXPECT_TRUE(caught_bad_alloc);
  EXPECT_TRUE(allocation_failed);
  EXPECT_EQ(manager.loaded_plugin_count(), 1u);
  ASSERT_EQ(manager.op_sources().at(kLifecycleKey), load_key);
  EXPECT_EQ(current_lifecycle_compute_device(), "PLUGIN_LIFECYCLE_TEST");
  EXPECT_EQ(manager.unload_by_plugin_path(load_key), 1);
  EXPECT_EQ(manager.loaded_plugin_count(), 0u);
  EXPECT_FALSE(lifecycle_key_is_registered());
}

/**
 * @brief Proves manager destruction is allocation-independent and ordered.
 * @throws Nothing when cleanup leaves the failpoint armed.
 * @note This exercises the real `PluginManager` noexcept destructor.
 */
TEST_F(PluginManagerLifecycleTest,
       DestructorAllocatesNothingAndDestroysCallbacksBeforeLibrary) {
  const auto plugin_path = lifecycle_plugin_path();
  ASSERT_TRUE(std::filesystem::exists(plugin_path));
  const auto trace_path = lifecycle_trace_path("destructor-no-allocation");
  std::filesystem::remove(trace_path);
  ScopedEnvironmentVariable trace_environment(kLifecycleTraceEnvironment,
                                              trace_path.string());
  auto manager = std::make_unique<PluginManager>();
  const auto result =
      manager->load_from_dirs_report({plugin_path.parent_path().string()});
  ASSERT_EQ(result.loaded, 1) << describe_errors(result.errors);

  bool fired_during_cleanup = false;
  bool failure_remained_armed = false;
  {
    ScopedPluginCleanupAllocationFailure failure(0);
    manager.reset();
    fired_during_cleanup = plugin_cleanup_allocation_probe::did_fire();
    failure_remained_armed = consume_armed_cleanup_allocation_failure();
  }

  EXPECT_FALSE(fired_during_cleanup);
  EXPECT_TRUE(failure_remained_armed);
  EXPECT_FALSE(lifecycle_key_is_registered());
  EXPECT_EQ(read_lifecycle_trace(trace_path),
            (std::vector<std::string>{"registrar_return", "callback_destroy",
                                      "library_unload"}));
  std::filesystem::remove(trace_path);
}

/**
 * @brief Reuses one manager across repeated real dynamic load/unload cycles.
 * @throws Nothing when registry/source/handle state returns to baseline.
 * @note Twenty iterations expose retained-handle and stale-source leakage.
 */
TEST_F(PluginManagerLifecycleTest, RepeatedLoadUnloadPreservesLifecycleState) {
  constexpr int kIterations = 20;
  const auto plugin_path = lifecycle_plugin_path();
  ASSERT_TRUE(std::filesystem::exists(plugin_path));
  PluginManager manager;

  for (int iteration = 0; iteration < kIterations; ++iteration) {
    SCOPED_TRACE(iteration);
    const auto result =
        manager.load_from_dirs_report({plugin_path.parent_path().string()});
    ASSERT_EQ(result.loaded, 1) << describe_errors(result.errors);
    EXPECT_EQ(manager.loaded_plugin_count(), 1u);
    EXPECT_EQ(current_lifecycle_compute_device(), "PLUGIN_LIFECYCLE_TEST");
    EXPECT_EQ(manager.unload_all_plugins(), 1);
    EXPECT_EQ(manager.loaded_plugin_count(), 0u);
    EXPECT_FALSE(lifecycle_key_is_registered());
  }
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

  const auto plugin_dir = standard_plugin_dir();
  ASSERT_TRUE(std::filesystem::exists(plugin_dir))
      << "standard operation plugin directory was not built: " << plugin_dir;

  PluginManager manager;
  const auto result = manager.load_from_dirs_report({plugin_dir.string()});

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

#if defined(PHOTOSPIDER_INTERNAL_BAD_ALLOC_TESTING)
TEST_F(PluginManagerLifecycleTest,
       PostRegistrationBadAllocHasStrongTransactionGuarantee) {
  using Failpoint = testing::OperationPluginLoadFailpoint;
  const std::vector<std::pair<Failpoint, std::string>> cases = {
      {Failpoint::PreviousSource, "previous-source"},
      {Failpoint::SourceAndResult, "source-result"},
      {Failpoint::Snapshot, "snapshot"},
      {Failpoint::HandleCommit, "handle-commit"},
  };
  const auto plugin_path = lifecycle_plugin_path();
  ASSERT_TRUE(std::filesystem::exists(plugin_path));

  for (const auto& [failpoint, label] : cases) {
    SCOPED_TRACE(label);
    OpRegistry::instance().unregister_key(kLifecycleKey);
    register_host_lifecycle_sentinel();
    std::map<std::string, std::string> op_sources = {
        {"sentinel:key", "sentinel-source"}};
    LoadedOpPluginMap loaded_plugins;
    LoadedOpPlugin sentinel_plugin;
    sentinel_plugin.registered_keys.push_back("sentinel:key");
    loaded_plugins.emplace("sentinel-plugin", std::move(sentinel_plugin));
    PluginLoadResult result;
    result.attempted = 7;
    result.loaded = 3;
    result.errors.push_back(
        {"sentinel-path", GraphErrc::Unknown, "sentinel-error"});
    result.new_op_keys.push_back("sentinel:key");

    const auto trace_path = lifecycle_trace_path(label);
    std::filesystem::remove(trace_path);
    ScopedEnvironmentVariable trace_environment(kLifecycleTraceEnvironment,
                                                trace_path.string());
    testing::set_operation_plugin_load_failpoint(failpoint);

    bool caught_bad_alloc = false;
    try {
      testing::load_one_operation_plugin_for_testing(plugin_path, op_sources,
                                                     loaded_plugins, result);
    } catch (const std::bad_alloc&) {
      caught_bad_alloc = true;
    }
    const auto failpoint_hits = testing::operation_plugin_load_failpoint_hits();
    testing::set_operation_plugin_load_failpoint(Failpoint::None);

    const auto trace = read_lifecycle_trace(trace_path);
    EXPECT_TRUE(caught_bad_alloc);
    EXPECT_EQ(failpoint_hits, 1u);
    EXPECT_EQ(op_sources, (std::map<std::string, std::string>{
                              {"sentinel:key", "sentinel-source"}}));
    ASSERT_EQ(loaded_plugins.size(), 1u);
    ASSERT_EQ(loaded_plugins.count("sentinel-plugin"), 1u);
    EXPECT_EQ(loaded_plugins.at("sentinel-plugin").registered_keys,
              (std::vector<std::string>{"sentinel:key"}));
    EXPECT_EQ(result.attempted, 7);
    EXPECT_EQ(result.loaded, 3);
    ASSERT_EQ(result.errors.size(), 1u);
    EXPECT_EQ(result.errors[0].path, "sentinel-path");
    EXPECT_EQ(result.errors[0].message, "sentinel-error");
    EXPECT_EQ(result.new_op_keys, (std::vector<std::string>{"sentinel:key"}));
    EXPECT_TRUE(lifecycle_key_is_registered());
    EXPECT_EQ(current_lifecycle_compute_device(), "HOST_LIFECYCLE_SENTINEL");
    EXPECT_EQ(trace,
              (std::vector<std::string>{"registrar_return", "callback_destroy",
                                        "library_unload"}));
    std::cout << "plugin_transaction_trace failpoint=" << label
              << " bad_alloc=" << caught_bad_alloc << " hits=" << failpoint_hits
              << " attempted=" << result.attempted
              << " sources=" << op_sources.size()
              << " handles=" << loaded_plugins.size()
              << " active_callback=" << current_lifecycle_compute_device()
              << " lifecycle=";
    for (const auto& event : trace) {
      std::cout << event << ',';
    }
    std::cout << '\n';
    std::filesystem::remove(trace_path);
    OpRegistry::instance().unregister_key(kLifecycleKey);
  }
}

TEST_F(PluginManagerLifecycleTest,
       ManagerBadAllocDoesNotRetainOrPublishCandidatePlugin) {
  using Failpoint = testing::OperationPluginLoadFailpoint;
  const auto plugin_path = lifecycle_plugin_path();
  ASSERT_TRUE(std::filesystem::exists(plugin_path));
  const auto trace_path = lifecycle_trace_path("manager-handle-commit");
  std::filesystem::remove(trace_path);
  ScopedEnvironmentVariable trace_environment(kLifecycleTraceEnvironment,
                                              trace_path.string());
  testing::set_operation_plugin_load_failpoint(Failpoint::HandleCommit);

  PluginManager manager;
  EXPECT_THROW(
      manager.load_from_dirs_report({plugin_path.parent_path().string()}),
      std::bad_alloc);
  const auto hits = testing::operation_plugin_load_failpoint_hits();
  testing::set_operation_plugin_load_failpoint(Failpoint::None);

  EXPECT_EQ(hits, 1u);
  EXPECT_EQ(manager.loaded_plugin_count(), 0u);
  EXPECT_EQ(manager.op_sources().count(kLifecycleKey), 0u);
  EXPECT_FALSE(lifecycle_key_is_registered());
  EXPECT_EQ(read_lifecycle_trace(trace_path),
            (std::vector<std::string>{"registrar_return", "callback_destroy",
                                      "library_unload"}));
  std::cout << "plugin_manager_transaction_trace failpoint=handle-commit"
            << " hits=" << hits
            << " retained_handles=" << manager.loaded_plugin_count()
            << " active_key=" << lifecycle_key_is_registered() << '\n';
  std::filesystem::remove(trace_path);
}

TEST_F(PluginManagerLifecycleTest,
       RegistrarFailuresPreserveOriginalPolicyAndSafeUnloadOrder) {
  const auto plugin_path = lifecycle_plugin_path();
  ASSERT_TRUE(std::filesystem::exists(plugin_path));

  {
    const auto trace_path = lifecycle_trace_path("registrar-runtime");
    std::filesystem::remove(trace_path);
    ScopedEnvironmentVariable trace_environment(kLifecycleTraceEnvironment,
                                                trace_path.string());
    ScopedEnvironmentVariable throw_environment(kLifecycleThrowEnvironment,
                                                "runtime_error");
    PluginManager manager;
    const auto result =
        manager.load_from_dirs_report({plugin_path.parent_path().string()});
    ASSERT_EQ(result.errors.size(), 1u);
    EXPECT_EQ(result.errors[0].message, "lifecycle registrar runtime failure");
    EXPECT_EQ(result.loaded, 0);
    EXPECT_EQ(manager.loaded_plugin_count(), 0u);
    EXPECT_FALSE(lifecycle_key_is_registered());
    EXPECT_EQ(read_lifecycle_trace(trace_path),
              (std::vector<std::string>{"registrar_throw_runtime_error",
                                        "callback_destroy", "library_unload"}));
    std::cout << "plugin_registrar_failure_trace mode=runtime_error"
              << " errors=" << result.errors.size()
              << " retained_handles=" << manager.loaded_plugin_count()
              << " active_key=" << lifecycle_key_is_registered() << '\n';
    std::filesystem::remove(trace_path);
  }

  {
    const auto trace_path = lifecycle_trace_path("registrar-bad-alloc");
    std::filesystem::remove(trace_path);
    ScopedEnvironmentVariable trace_environment(kLifecycleTraceEnvironment,
                                                trace_path.string());
    ScopedEnvironmentVariable throw_environment(kLifecycleThrowEnvironment,
                                                "bad_alloc");
    PluginManager manager;
    EXPECT_THROW(
        manager.load_from_dirs_report({plugin_path.parent_path().string()}),
        std::bad_alloc);
    EXPECT_EQ(manager.loaded_plugin_count(), 0u);
    EXPECT_FALSE(lifecycle_key_is_registered());
    EXPECT_EQ(read_lifecycle_trace(trace_path),
              (std::vector<std::string>{"registrar_throw_bad_alloc",
                                        "callback_destroy", "library_unload"}));
    std::cout << "plugin_registrar_failure_trace mode=bad_alloc"
              << " retained_handles=" << manager.loaded_plugin_count()
              << " active_key=" << lifecycle_key_is_registered() << '\n';
    std::filesystem::remove(trace_path);
  }
}
#endif

}  // namespace ps
