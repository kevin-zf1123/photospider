// Photospider test: Scheduler Plugin Loader
// 测试调度器插件的动态加载功能

#include <gtest/gtest.h>

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <new>
#include <optional>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include "kernel/scheduler/scheduler_factory.hpp"
#include "kernel/scheduler/scheduler_plugin_api.hpp"
#include "kernel/scheduler/scheduler_plugin_loader.hpp"
#include "kernel/scheduler/scheduler_task_runtime.hpp"

#ifdef _WIN32
#include <windows.h>
#else
#include <dlfcn.h>
#endif

namespace scheduler_allocation_probe {

/** @brief Disabled allocation countdown sentinel. */
constexpr std::int64_t kDisabled = -1;
/** @brief Per-thread zero-based allocation countdown. */
thread_local std::int64_t countdown = kDisabled;
/** @brief Whether the armed one-shot allocation failure fired. */
thread_local bool fired = false;

/**
 * @brief Arms one deterministic allocation failure on the current thread.
 * @param allocation_index Zero-based allocation to reject.
 * @return Nothing.
 * @throws Nothing.
 */
void arm(std::int64_t allocation_index) noexcept {
  countdown = allocation_index;
  fired = false;
}

/**
 * @brief Disarms the current-thread allocation failure.
 * @return Nothing.
 * @throws Nothing.
 */
void disarm() noexcept {
  countdown = kDisabled;
}

/**
 * @brief Reports whether the most recently armed failure fired.
 * @return True after `maybe_fail()` rejected the selected allocation.
 * @throws Nothing.
 */
bool did_fire() noexcept {
  return fired;
}

/**
 * @brief Applies the current allocation failure decision.
 * @return Nothing when allocation may continue.
 * @throws std::bad_alloc at the selected allocation index.
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

}  // namespace scheduler_allocation_probe

/**
 * @brief Test-executable allocation operator for scheduler failure probes.
 * @param size Requested allocation size.
 * @return malloc-backed storage.
 * @throws std::bad_alloc when injected or malloc fails.
 */
void* operator new(std::size_t size) {
  scheduler_allocation_probe::maybe_fail();
  if (void* memory = std::malloc(size == 0 ? 1 : size)) {
    return memory;
  }
  throw std::bad_alloc{};
}

/** @copydoc operator new(std::size_t) */
void* operator new[](std::size_t size) {
  return ::operator new(size);
}

/** @brief Releases scalar storage allocated by the test operator. */
void operator delete(void* memory) noexcept {
  std::free(memory);
}
/** @brief Releases array storage allocated by the test operator. */
void operator delete[](void* memory) noexcept {
  std::free(memory);
}
/** @brief Releases sized scalar storage allocated by the test operator. */
void operator delete(void* memory, std::size_t) noexcept {
  std::free(memory);
}
/** @brief Releases sized array storage allocated by the test operator. */
void operator delete[](void* memory, std::size_t) noexcept {
  std::free(memory);
}

namespace ps {
namespace {

/** @brief Short destroy-count fixture scheduler type. */
constexpr const char* kDestroyCountSchedulerType = "destroy_count_test";
constexpr const char* kLongDestroyCountSchedulerType =                 // NOLINT
    "destroy_count_scheduler_type_long_enough_to_force_owned_string_"  // NOLINT
    "storage";                                                         // NOLINT
/** @brief Fixture environment key selecting lifecycle trace output. */
constexpr const char* kDestroyCountTraceEnvironment =
    "PS_DESTROY_COUNT_SCHEDULER_TRACE";  // NOLINT
/** @brief Fixture environment key selecting injected lifecycle failures. */
constexpr const char* kDestroyCountFailureEnvironment =
    "PS_DESTROY_COUNT_SCHEDULER_FAILURE";  // NOLINT
/** @brief Fixture environment key enabling one duplicate discovery entry. */
constexpr const char* kDestroyCountDuplicateTypeEnvironment =
    "PS_DESTROY_COUNT_SCHEDULER_DUPLICATE_TYPE";  // NOLINT
/** @brief Fixture environment key selecting a discovery callback exception. */
constexpr const char* kDestroyCountLoadFailureEnvironment =
    "PS_DESTROY_COUNT_SCHEDULER_LOAD_FAILURE";  // NOLINT

/**
 * @brief Mirrors the destroy-count plugin's fixture-only forwarding counters.
 * @note Values form a private test protocol consumed only through
 * `ps_test_scheduler_forwarding_count` while the fixture library is loaded.
 */
enum class SchedulerForwardingProbe : int {
  /** @brief Plugin runtime device-inventory calls. */
  AvailableDevices = 0,
  /** @brief Plugin runtime initial handle-batch calls. */
  InitialHandles,
  /** @brief Plugin runtime single worker-handle calls. */
  WorkerHandle,
  /** @brief Plugin runtime worker handle-batch calls. */
  WorkerHandles,
  /** @brief Plugin runtime single any-thread handle calls. */
  AnyThreadHandle,
  /** @brief Plugin runtime any-thread handle-batch calls. */
  AnyThreadHandles,
  /** @brief Closure submissions indicating an incorrect owner fallback. */
  ClosureFallback,
};

/**
 * @brief Mirrors fixture-only scheduler discovery callback counters.
 * @note Values form a private protocol consumed through
 * `ps_test_scheduler_load_probe_count` while the fixture remains loaded.
 */
enum class SchedulerLoadProbe : int {
  /** @brief Calls to the type-count export. */
  GetCount = 0,
  /** @brief Calls to the indexed type-name export. */
  GetName,
  /** @brief Calls to the optional indexed description export. */
  GetDescription,
  /** @brief Calls to the optional version export. */
  GetVersion,
};

/**
 * @brief Captures every caller-visible scheduler plugin registry container.
 * @note Exact equality before and after an injected load failure detects
 * phantom type mappings, partial metadata, leaked handles, and diagnostic
 * prefix changes.
 */
struct SchedulerPluginRegistrySnapshot {
  /** @brief Sorted built-in and plugin scheduler type names. */
  std::vector<std::string> registered_types;
  /** @brief Built-in and plugin metadata in loader iteration order. */
  std::vector<SchedulerPluginInfo> type_info;
  /** @brief Formatted retained-plugin handle records. */
  std::vector<std::string> loaded_plugins;
  /** @brief Recoverable load diagnostics in insertion order. */
  std::vector<std::string> load_errors;
};

/**
 * @brief Counts real TaskHandle callbacks executed by the plugin fixture.
 * @note The synchronous fixture invokes this executor on the calling test
 * thread, so plain integer fields are sufficient.
 */
class CountingTaskExecutor final : public TaskExecutor {
 public:
  /**
   * @brief Records one executed dense task id.
   * @param task_id Dense id forwarded by `TaskHandle::run()`.
   * @return Nothing.
   * @throws Nothing.
   * @note Count and sum together detect missing, duplicate, or unexpected test
   * callbacks without allocating trace storage.
   */
  void run_task(int task_id) override {
    ++callback_count_;
    task_id_sum_ += task_id;
  }

  /**
   * @brief Returns the number of callbacks observed so far.
   * @return Synchronous callback count.
   * @throws Nothing.
   */
  int callback_count() const noexcept { return callback_count_; }

  /**
   * @brief Returns the sum of executed dense task ids.
   * @return Accumulated task-id sum.
   * @throws Nothing.
   */
  int task_id_sum() const noexcept { return task_id_sum_; }

 private:
  /** @brief Number of real `run_task` calls. */
  int callback_count_ = 0;
  /** @brief Sum of real task ids used to detect duplicates or omissions. */
  int task_id_sum_ = 0;
};

/**
 * @brief Reads one named forwarding counter through the fixture C export.
 * @param read_count Resolved fixture counter function.
 * @param probe Counter slot to read.
 * @return Current fixture call count for `probe`.
 * @throws Nothing.
 */
int forwarding_probe_count(int (*read_count)(int),
                           SchedulerForwardingProbe probe) noexcept {
  return read_count(static_cast<int>(probe));
}

/**
 * @brief Reads one fixture scheduler-discovery callback counter.
 * @param read_count Resolved fixture counter export.
 * @param probe Discovery callback slot to read.
 * @return Current fixture call count for `probe`.
 * @throws Nothing.
 */
int load_probe_count(int (*read_count)(int),
                     SchedulerLoadProbe probe) noexcept {
  return read_count(static_cast<int>(probe));
}

/**
 * @brief Captures the loader's complete observable plugin registry state.
 * @param loader Loader queried while no allocation failpoint is armed.
 * @return Owned snapshot suitable for exact post-failure comparison.
 * @throws std::bad_alloc if result vectors or copied metadata cannot allocate.
 * @note Tests are single-threaded, so copying the diagnostic reference is safe
 * within the fixture's exclusive use of the singleton.
 */
SchedulerPluginRegistrySnapshot capture_plugin_registry(
    const SchedulerPluginLoader& loader) {
  SchedulerPluginRegistrySnapshot snapshot;
  snapshot.registered_types = loader.get_registered_types();
  snapshot.type_info = loader.get_all_info();
  snapshot.loaded_plugins = loader.list_loaded_plugins();
  snapshot.load_errors = loader.get_load_errors();
  return snapshot;
}

/**
 * @brief Compares every field in two scheduler plugin registry snapshots.
 * @param expected State captured before a transactional load attempt.
 * @param actual State observed after that attempt.
 * @return Nothing.
 * @throws std::bad_alloc if GoogleTest diagnostic formatting cannot allocate.
 * @note Metadata fields are compared individually because
 * `SchedulerPluginInfo` intentionally has no public equality operator.
 */
void expect_plugin_registry_equal(
    const SchedulerPluginRegistrySnapshot& expected,
    const SchedulerPluginRegistrySnapshot& actual) {
  EXPECT_EQ(actual.registered_types, expected.registered_types);
  EXPECT_EQ(actual.loaded_plugins, expected.loaded_plugins);
  EXPECT_EQ(actual.load_errors, expected.load_errors);
  ASSERT_EQ(actual.type_info.size(), expected.type_info.size());
  for (std::size_t index = 0; index < expected.type_info.size(); ++index) {
    EXPECT_EQ(actual.type_info[index].type_name,
              expected.type_info[index].type_name);
    EXPECT_EQ(actual.type_info[index].description,
              expected.type_info[index].description);
    EXPECT_EQ(actual.type_info[index].plugin_path,
              expected.type_info[index].plugin_path);
    EXPECT_EQ(actual.type_info[index].version,
              expected.type_info[index].version);
    EXPECT_EQ(actual.type_info[index].is_builtin,
              expected.type_info[index].is_builtin);
  }
}

/**
 * @brief Sets one process environment value for a bounded fixture scenario.
 *
 * @note Destruction restores the prior value or removes the key. Tests using
 * this guard are process-serial because environment variables are global.
 */
class ScopedSchedulerFixtureEnvironment final {
 public:
  /**
   * @brief Installs one fixture environment value and saves its predecessor.
   * @param name Environment key copied for the guard lifetime.
   * @param value New value visible to the dynamic scheduler plugin.
   * @throws std::runtime_error when the platform environment update fails.
   * @throws std::bad_alloc if owned key or previous-value storage cannot grow.
   */
  ScopedSchedulerFixtureEnvironment(const char* name, const std::string& value)
      : name_(name) {
    if (const char* previous = std::getenv(name)) {
      previous_ = std::string(previous);
    }
    set(value);
  }

  /**
   * @brief Restores the predecessor without throwing from test cleanup.
   * @throws Nothing; restoration failures are intentionally suppressed.
   */
  ~ScopedSchedulerFixtureEnvironment() noexcept {
    try {
      if (previous_) {
        set(*previous_);
      } else {
        clear();
      }
    } catch (...) {
      // Process-global test cleanup cannot safely surface restoration failure.
    }
  }

  ScopedSchedulerFixtureEnvironment(const ScopedSchedulerFixtureEnvironment&) =
      delete;
  ScopedSchedulerFixtureEnvironment& operator=(
      const ScopedSchedulerFixtureEnvironment&) = delete;

 private:
  /**
   * @brief Replaces the owned environment key with `value`.
   * @param value New process-global value.
   * @return Nothing.
   * @throws std::runtime_error when the platform update fails.
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
   * @return Nothing.
   * @throws std::runtime_error when the platform update fails.
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

  /** @brief Environment key retained for restoration. */
  std::string name_;
  /** @brief Previous value, or nullopt when the key was initially absent. */
  std::optional<std::string> previous_;
};

/**
 * @brief Returns a unique path for one scheduler-owner lifecycle trace.
 * @param label Stable scenario label included in the filename.
 * @return Path in GoogleTest's temporary directory.
 * @throws std::bad_alloc from path/string construction.
 */
std::filesystem::path scheduler_owner_trace_path(const std::string& label) {
  static std::atomic<unsigned int> sequence{0};
  return std::filesystem::path(::testing::TempDir()) /
         ("photospider-scheduler-owner-" + label + "-" +
          std::to_string(sequence.fetch_add(1, std::memory_order_relaxed)) +
          ".log");
}

/**
 * @brief Reads newline-delimited events emitted by the real scheduler fixture.
 * @param path Trace file selected through the fixture environment.
 * @return Events in actual lifecycle and final-unload order.
 * @throws std::bad_alloc if line/vector storage cannot allocate.
 * @note A missing trace returns an empty vector for a focused assertion.
 */
std::vector<std::string> read_scheduler_owner_trace(
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
 * @brief RAII owner for one current-thread allocation failpoint.
 * @note Assertions run after destruction so GoogleTest allocation is normal.
 */
class ScopedSchedulerAllocationFailure final {
 public:
  /**
   * @brief Arms a zero-based allocation failure.
   * @param allocation_index Allocation to reject.
   * @throws Nothing.
   */
  explicit ScopedSchedulerAllocationFailure(
      std::int64_t allocation_index) noexcept {
    scheduler_allocation_probe::arm(allocation_index);
  }

  /**
   * @brief Disarms the current-thread failpoint before assertions allocate.
   * @throws Nothing.
   */
  ~ScopedSchedulerAllocationFailure() noexcept {
    scheduler_allocation_probe::disarm();
  }

  ScopedSchedulerAllocationFailure(const ScopedSchedulerAllocationFailure&) =
      delete;
  ScopedSchedulerAllocationFailure& operator=(
      const ScopedSchedulerAllocationFailure&) = delete;
};

#ifndef PS_SCHEDULER_PLUGIN_DIR
#define PS_SCHEDULER_PLUGIN_DIR "build/schedulers"
#endif

#ifndef PS_TEST_SCHEDULER_PLUGIN_DIR
#define PS_TEST_SCHEDULER_PLUGIN_DIR "build/test_schedulers"
#endif

/**
 * @brief Returns the CMake output directory for scheduler plugins.
 *
 * @param test_fixture True selects the test-only scheduler fixture output
 * directory; false selects the production/demo scheduler plugin output
 * directory.
 * @return Filesystem path injected by CMake for this test target, or the
 * legacy source-root-relative `build/schedulers` tree when compiled outside
 * CMake.
 * @throws std::bad_alloc from path string construction.
 * @note CMake injects absolute paths during normal builds so the loader tests
 * can execute from the source tree, the binary tree, or nested CI build
 * directories without assuming a literal `build/` directory name.
 */
std::filesystem::path scheduler_plugin_dir(bool test_fixture = false) {
  return std::filesystem::path(test_fixture ? PS_TEST_SCHEDULER_PLUGIN_DIR
                                            : PS_SCHEDULER_PLUGIN_DIR);
}

/**
 * @brief Builds the expected shared library path for a scheduler plugin.
 *
 * @param stem Platform-independent library target stem without prefix or
 * extension.
 * @param test_fixture True when the plugin lives in the test-only scheduler
 * output directory.
 * @return Platform-specific plugin path inside `scheduler_plugin_dir()`.
 * @throws std::bad_alloc from path and filename string construction.
 * @note The helper mirrors CMake's target output directories instead of using
 * source-root-relative paths, preserving CI compatibility with arbitrary
 * `BUILD_DIR` values.
 */
std::filesystem::path scheduler_plugin_path(const std::string& stem,
                                            bool test_fixture = false) {
  const std::filesystem::path dir = scheduler_plugin_dir(test_fixture);
#if defined(_WIN32)
  return dir / (stem + ".dll");
#elif defined(__APPLE__)
  return dir / ("lib" + stem + ".dylib");
#else
  return dir / ("lib" + stem + ".so");
#endif
}

bool contains_type(const std::vector<std::string>& types,
                   const std::string& type) {
  return std::find(types.begin(), types.end(), type) != types.end();
}

}  // namespace

class SchedulerPluginLoaderTest : public ::testing::Test {
 protected:
  void SetUp() override {
    // 清除之前的状态
    auto& loader = SchedulerPluginLoader::instance();
    loader.clear_plugins();
    loader.clear_errors();
  }
};

// 测试单例模式
TEST_F(SchedulerPluginLoaderTest, SingletonInstance) {
  auto& loader1 = SchedulerPluginLoader::instance();
  auto& loader2 = SchedulerPluginLoader::instance();
  EXPECT_EQ(&loader1, &loader2);
}

// 测试初始状态
TEST_F(SchedulerPluginLoaderTest, InitialState) {
  auto& loader = SchedulerPluginLoader::instance();

  // 初始时应该没有已注册的类型（插件还未加载）
  auto types = loader.get_registered_types();
  EXPECT_TRUE(types.empty());

  // 加载错误列表应该为空
  auto errors = loader.get_load_errors();
  EXPECT_TRUE(errors.empty());
}

// 测试扫描不存在的目录
TEST_F(SchedulerPluginLoaderTest, ScanNonExistentDirectory) {
  auto& loader = SchedulerPluginLoader::instance();

  size_t count = loader.scan_and_load("/nonexistent/directory");
  EXPECT_EQ(count, 0);
}

// 测试扫描实际的调度器插件目录
TEST_F(SchedulerPluginLoaderTest, ScanSchedulerDirectory) {
  auto& loader = SchedulerPluginLoader::instance();

  const auto plugin_dir = scheduler_plugin_dir();
  size_t count = loader.scan_and_load(plugin_dir.string());

  EXPECT_GE(count, 3u);

  // 如果加载成功，验证类型
  if (count > 0) {
    auto types = loader.get_registered_types();
    EXPECT_FALSE(types.empty());
    EXPECT_TRUE(contains_type(types, "cpu_work_stealing_example"));
    EXPECT_TRUE(contains_type(types, "serial_debug_example"));
    EXPECT_TRUE(contains_type(types, "gpu_pipeline_example"));
    EXPECT_TRUE(contains_type(types, "heterogeneous_example"));
    EXPECT_FALSE(contains_type(types, "destroy_count_test"))
        << "test-only scheduler fixture must not be exposed in " << plugin_dir;

    // 打印已加载的类型
    std::cout << "Loaded scheduler types from plugins:\n";
    for (const auto& type : types) {
      std::cout << "  - " << type << "\n";
    }
  }
}

// 测试加载单个插件
TEST_F(SchedulerPluginLoaderTest, LoadSinglePlugin) {
  auto& loader = SchedulerPluginLoader::instance();

  bool result = loader.load_plugin(
      scheduler_plugin_path("cpu_work_stealing_example_plugin").string());

  if (result) {
    EXPECT_TRUE(loader.is_registered("cpu_work_stealing_example"));
  }
}

// 测试获取描述
TEST_F(SchedulerPluginLoaderTest, GetDescription) {
  auto& loader = SchedulerPluginLoader::instance();

  loader.scan_and_load(scheduler_plugin_dir().string());

  auto types = loader.get_registered_types();
  for (const auto& type : types) {
    auto desc = loader.get_description(type);
    // 描述应该非空
    EXPECT_FALSE(desc.empty()) << "Type: " << type;
  }
}

// 测试创建调度器实例
TEST_F(SchedulerPluginLoaderTest, CreateSchedulerFromPlugin) {
  auto& loader = SchedulerPluginLoader::instance();

  const auto plugin_dir = scheduler_plugin_dir();
  size_t count = loader.scan_and_load(plugin_dir.string());
  if (count == 0) {
    GTEST_SKIP() << "No plugins found in " << plugin_dir;
  }

  auto types = loader.get_registered_types();
  for (const auto& type : types) {
    auto scheduler = loader.create(type, 4);
    EXPECT_NE(scheduler, nullptr)
        << "Failed to create scheduler of type: " << type;

    if (scheduler) {
      // 验证基本功能
      scheduler->start();
      auto stats = scheduler->get_stats();
      EXPECT_FALSE(stats.empty());
      scheduler->shutdown();
    }
  }
}

// 测试列出已加载的插件
TEST_F(SchedulerPluginLoaderTest, ListLoadedPlugins) {
  auto& loader = SchedulerPluginLoader::instance();

  loader.scan_and_load(scheduler_plugin_dir().string());

  auto plugins = loader.list_loaded_plugins();
  std::cout << "Loaded plugins:\n";
  for (const auto& info : plugins) {
    std::cout << "  " << info << "\n";
  }
}

// 测试 SchedulerFactory 集成
TEST_F(SchedulerPluginLoaderTest, FactoryIntegration) {
  auto& loader = SchedulerPluginLoader::instance();

  size_t count = loader.scan_and_load(scheduler_plugin_dir().string());
  if (count == 0) {
    GTEST_SKIP() << "No plugins found";
  }

  // 通过 SchedulerFactory 创建插件调度器
  auto types = loader.get_registered_types();
  for (const auto& type : types) {
    // Factory 应该能够创建插件类型
    auto scheduler = SchedulerFactory::create(type, 2);
    EXPECT_NE(scheduler, nullptr) << "Factory failed to create: " << type;
  }
}

TEST_F(SchedulerPluginLoaderTest, GpuPipelineExampleCreateRejectsNullTypeName) {
  const auto plugin_path = scheduler_plugin_path("gpu_pipeline_example_plugin");

  if (!std::filesystem::exists(plugin_path)) {
    GTEST_SKIP() << "gpu pipeline scheduler plugin was not built";
  }

#ifdef _WIN32
  HMODULE test_handle = LoadLibrary(plugin_path.string().c_str());
  ASSERT_NE(test_handle, nullptr);
  auto create_scheduler = reinterpret_cast<SchedulerPluginCreateFunc>(
      GetProcAddress(test_handle, PS_SCHEDULER_PLUGIN_CREATE));
#else
  void* test_handle = dlopen(plugin_path.string().c_str(), RTLD_LAZY);
  ASSERT_NE(test_handle, nullptr) << dlerror();
  auto create_scheduler = reinterpret_cast<SchedulerPluginCreateFunc>(
      dlsym(test_handle, PS_SCHEDULER_PLUGIN_CREATE));
#endif
  ASSERT_NE(create_scheduler, nullptr);
  EXPECT_EQ(create_scheduler(nullptr, 0), nullptr);

#ifdef _WIN32
  FreeLibrary(test_handle);
#else
  dlclose(test_handle);
#endif
}

// 测试清除插件
TEST_F(SchedulerPluginLoaderTest, ClearPlugins) {
  auto& loader = SchedulerPluginLoader::instance();

  const auto plugin_dir = scheduler_plugin_dir();
  loader.scan_and_load(plugin_dir.string());

  auto before = loader.get_registered_types();
  size_t before_count = before.size();

  // 清除
  loader.clear_plugins();

  auto after = loader.get_registered_types();
  EXPECT_TRUE(after.empty());

  // 可以重新加载
  loader.scan_and_load(plugin_dir.string());
  auto reloaded = loader.get_registered_types();
  EXPECT_EQ(reloaded.size(), before_count);
}

TEST_F(SchedulerPluginLoaderTest, PluginSchedulerUsesPluginDestroyAfterClear) {
  auto& loader = SchedulerPluginLoader::instance();

  const auto plugin_path =
      scheduler_plugin_path("destroy_count_scheduler_plugin", true);

  if (!std::filesystem::exists(plugin_path)) {
    GTEST_SKIP() << "destroy-count scheduler plugin was not built";
  }

#ifdef _WIN32
  HMODULE test_handle = LoadLibrary(plugin_path.string().c_str());
  ASSERT_NE(test_handle, nullptr);
  auto reset_counts = reinterpret_cast<void (*)()>(
      GetProcAddress(test_handle, "ps_test_scheduler_reset_counts"));
  auto active_count = reinterpret_cast<int (*)()>(
      GetProcAddress(test_handle, "ps_test_scheduler_active_count"));
  auto destroy_count = reinterpret_cast<int (*)()>(
      GetProcAddress(test_handle, "ps_test_scheduler_destroy_count"));
#else
  void* test_handle = dlopen(plugin_path.string().c_str(), RTLD_LAZY);
  ASSERT_NE(test_handle, nullptr) << dlerror();
  auto reset_counts = reinterpret_cast<void (*)()>(
      dlsym(test_handle, "ps_test_scheduler_reset_counts"));
  auto active_count = reinterpret_cast<int (*)()>(
      dlsym(test_handle, "ps_test_scheduler_active_count"));
  auto destroy_count = reinterpret_cast<int (*)()>(
      dlsym(test_handle, "ps_test_scheduler_destroy_count"));
#endif
  ASSERT_NE(reset_counts, nullptr);
  ASSERT_NE(active_count, nullptr);
  ASSERT_NE(destroy_count, nullptr);
  reset_counts();

  ASSERT_TRUE(loader.load_plugin(plugin_path));
  auto scheduler = loader.create("destroy_count_test", 0);
  ASSERT_NE(scheduler, nullptr);
  EXPECT_EQ(active_count(), 1);
  EXPECT_EQ(destroy_count(), 0);

  loader.clear_plugins();
  EXPECT_EQ(active_count(), 1);
  scheduler.reset();
  EXPECT_EQ(active_count(), 0);
  EXPECT_EQ(destroy_count(), 1);

#ifdef _WIN32
  FreeLibrary(test_handle);
#else
  dlclose(test_handle);
#endif
}

/**
 * @brief Verifies the host owner forwards every current non-pure runtime API.
 * @return Nothing.
 * @throws Nothing when device discovery and all native TaskHandle calls reach
 * the real plugin overrides without closure fallback.
 * @note Fixture counters prove which dynamic virtual functions ran; callback
 * count and task-id sum prove the supplied handles executed exactly once.
 */
TEST_F(SchedulerPluginLoaderTest,
       OwnerForwardsDevicesAndEveryTaskHandleOverride) {
  auto& loader = SchedulerPluginLoader::instance();
  const auto plugin_path =
      scheduler_plugin_path("destroy_count_scheduler_plugin", true);
  ASSERT_TRUE(std::filesystem::exists(plugin_path));

#ifdef _WIN32
  HMODULE test_handle = LoadLibrary(plugin_path.string().c_str());
  ASSERT_NE(test_handle, nullptr);
  auto reset_counts = reinterpret_cast<void (*)()>(
      GetProcAddress(test_handle, "ps_test_scheduler_reset_counts"));
  auto read_forwarding_count = reinterpret_cast<int (*)(int)>(
      GetProcAddress(test_handle, "ps_test_scheduler_forwarding_count"));
#else
  void* test_handle = dlopen(plugin_path.string().c_str(), RTLD_LAZY);
  ASSERT_NE(test_handle, nullptr) << dlerror();
  auto reset_counts = reinterpret_cast<void (*)()>(
      dlsym(test_handle, "ps_test_scheduler_reset_counts"));
  auto read_forwarding_count = reinterpret_cast<int (*)(int)>(
      dlsym(test_handle, "ps_test_scheduler_forwarding_count"));
#endif
  ASSERT_NE(reset_counts, nullptr);
  ASSERT_NE(read_forwarding_count, nullptr);
  ASSERT_TRUE(loader.load_plugin(plugin_path));
  reset_counts();

  auto scheduler = loader.create("destroy_count_test", 0);
  ASSERT_NE(scheduler, nullptr);
  auto* runtime = dynamic_cast<SchedulerTaskRuntime*>(scheduler.get());
  ASSERT_NE(runtime, nullptr);

  EXPECT_EQ(runtime->available_devices(),
            (std::vector<Device>{Device::CPU, Device::GPU_METAL}));

  CountingTaskExecutor executor;
  std::vector<TaskHandle> initial_handles{TaskHandle{&executor, 1, 101},
                                          TaskHandle{&executor, 2, 102}};
  runtime->submit_initial_task_handles(std::move(initial_handles), 2,
                                       SchedulerTaskPriority::High);
  runtime->submit_ready_task_handle_from_worker(TaskHandle{&executor, 3, 103},
                                                SchedulerTaskPriority::Normal);

  std::vector<TaskHandle> worker_handles{TaskHandle{&executor, 4, 104},
                                         TaskHandle{&executor, 5, 105}};
  runtime->submit_ready_task_handles_from_worker(std::move(worker_handles),
                                                 SchedulerTaskPriority::High);
  runtime->submit_ready_task_handle_any_thread(TaskHandle{&executor, 6, 106},
                                               SchedulerTaskPriority::Normal,
                                               std::uint64_t{41});

  std::vector<TaskHandle> any_thread_handles{TaskHandle{&executor, 7, 107},
                                             TaskHandle{&executor, 8, 108}};
  runtime->submit_ready_task_handles_any_thread(std::move(any_thread_handles),
                                                SchedulerTaskPriority::High,
                                                std::uint64_t{42});

  EXPECT_EQ(executor.callback_count(), 8);
  EXPECT_EQ(executor.task_id_sum(), 36);
  EXPECT_EQ(forwarding_probe_count(read_forwarding_count,
                                   SchedulerForwardingProbe::AvailableDevices),
            1);
  EXPECT_EQ(forwarding_probe_count(read_forwarding_count,
                                   SchedulerForwardingProbe::InitialHandles),
            1);
  EXPECT_EQ(forwarding_probe_count(read_forwarding_count,
                                   SchedulerForwardingProbe::WorkerHandle),
            1);
  EXPECT_EQ(forwarding_probe_count(read_forwarding_count,
                                   SchedulerForwardingProbe::WorkerHandles),
            1);
  EXPECT_EQ(forwarding_probe_count(read_forwarding_count,
                                   SchedulerForwardingProbe::AnyThreadHandle),
            1);
  EXPECT_EQ(forwarding_probe_count(read_forwarding_count,
                                   SchedulerForwardingProbe::AnyThreadHandles),
            1);
  EXPECT_EQ(forwarding_probe_count(read_forwarding_count,
                                   SchedulerForwardingProbe::ClosureFallback),
            0);

  scheduler.reset();
  loader.clear_plugins();
#ifdef _WIN32
  FreeLibrary(test_handle);
#else
  dlclose(test_handle);
#endif
}

/**
 * @brief Verifies plugin batch bad_alloc crosses the owner before callbacks.
 * @return Nothing.
 * @throws Nothing when every native handle-batch override propagates
 * `std::bad_alloc` and executes zero borrowed handles.
 * @note Per-method fixture counters distinguish direct batch forwarding from
 * the base class's repeated single-handle fallback.
 */
TEST_F(SchedulerPluginLoaderTest,
       OwnerPreservesBatchBadAllocBeforeAnyTaskHandleCallback) {
  auto& loader = SchedulerPluginLoader::instance();
  const auto plugin_path =
      scheduler_plugin_path("destroy_count_scheduler_plugin", true);
  ASSERT_TRUE(std::filesystem::exists(plugin_path));

#ifdef _WIN32
  HMODULE test_handle = LoadLibrary(plugin_path.string().c_str());
  ASSERT_NE(test_handle, nullptr);
  auto reset_counts = reinterpret_cast<void (*)()>(
      GetProcAddress(test_handle, "ps_test_scheduler_reset_counts"));
  auto read_forwarding_count = reinterpret_cast<int (*)(int)>(
      GetProcAddress(test_handle, "ps_test_scheduler_forwarding_count"));
#else
  void* test_handle = dlopen(plugin_path.string().c_str(), RTLD_LAZY);
  ASSERT_NE(test_handle, nullptr) << dlerror();
  auto reset_counts = reinterpret_cast<void (*)()>(
      dlsym(test_handle, "ps_test_scheduler_reset_counts"));
  auto read_forwarding_count = reinterpret_cast<int (*)(int)>(
      dlsym(test_handle, "ps_test_scheduler_forwarding_count"));
#endif
  ASSERT_NE(reset_counts, nullptr);
  ASSERT_NE(read_forwarding_count, nullptr);
  ASSERT_TRUE(loader.load_plugin(plugin_path));
  reset_counts();

  auto scheduler = loader.create("destroy_count_test", 0);
  ASSERT_NE(scheduler, nullptr);
  auto* runtime = dynamic_cast<SchedulerTaskRuntime*>(scheduler.get());
  ASSERT_NE(runtime, nullptr);
  CountingTaskExecutor executor;

  {
    ScopedSchedulerFixtureEnvironment failures(kDestroyCountFailureEnvironment,
                                               "handle_batch_bad_alloc");
    std::vector<TaskHandle> initial_handles{TaskHandle{&executor, 1, 101},
                                            TaskHandle{&executor, 2, 102}};
    EXPECT_THROW(
        runtime->submit_initial_task_handles(std::move(initial_handles), 2,
                                             SchedulerTaskPriority::High),
        std::bad_alloc);

    std::vector<TaskHandle> worker_handles{TaskHandle{&executor, 3, 103},
                                           TaskHandle{&executor, 4, 104}};
    EXPECT_THROW(runtime->submit_ready_task_handles_from_worker(
                     std::move(worker_handles), SchedulerTaskPriority::Normal),
                 std::bad_alloc);

    std::vector<TaskHandle> any_thread_handles{TaskHandle{&executor, 5, 105},
                                               TaskHandle{&executor, 6, 106}};
    EXPECT_THROW(runtime->submit_ready_task_handles_any_thread(
                     std::move(any_thread_handles), SchedulerTaskPriority::High,
                     std::uint64_t{43}),
                 std::bad_alloc);
  }

  EXPECT_EQ(executor.callback_count(), 0);
  EXPECT_EQ(executor.task_id_sum(), 0);
  EXPECT_EQ(forwarding_probe_count(read_forwarding_count,
                                   SchedulerForwardingProbe::InitialHandles),
            1);
  EXPECT_EQ(forwarding_probe_count(read_forwarding_count,
                                   SchedulerForwardingProbe::WorkerHandles),
            1);
  EXPECT_EQ(forwarding_probe_count(read_forwarding_count,
                                   SchedulerForwardingProbe::AnyThreadHandles),
            1);
  EXPECT_EQ(forwarding_probe_count(read_forwarding_count,
                                   SchedulerForwardingProbe::WorkerHandle),
            0);
  EXPECT_EQ(forwarding_probe_count(read_forwarding_count,
                                   SchedulerForwardingProbe::AnyThreadHandle),
            0);
  EXPECT_EQ(forwarding_probe_count(read_forwarding_count,
                                   SchedulerForwardingProbe::ClosureFallback),
            0);

  scheduler.reset();
  loader.clear_plugins();
#ifdef _WIN32
  FreeLibrary(test_handle);
#else
  dlclose(test_handle);
#endif
}

/**
 * @brief Verifies explicit lifecycle calls preserve plugin exception identity.
 * @throws Nothing when the owner delegates without applying destructor fences.
 * @note The failure environment is restored before owner destruction so this
 * test isolates the ordinary caller-visible API path from fallback cleanup.
 */
TEST_F(SchedulerPluginLoaderTest,
       ExplicitLifecycleCallsPropagatePluginExceptions) {
  auto& loader = SchedulerPluginLoader::instance();
  const auto plugin_path =
      scheduler_plugin_path("destroy_count_scheduler_plugin", true);
  ASSERT_TRUE(std::filesystem::exists(plugin_path));
  ASSERT_TRUE(loader.load_plugin(plugin_path));
  auto scheduler = loader.create("destroy_count_test", 0);
  ASSERT_NE(scheduler, nullptr);

  {
    ScopedSchedulerFixtureEnvironment failures(kDestroyCountFailureEnvironment,
                                               "all");
    EXPECT_THROW(scheduler->shutdown(), std::runtime_error);
    EXPECT_THROW(scheduler->detach(), std::bad_alloc);
  }

  scheduler.reset();
  loader.clear_plugins();
}

/**
 * @brief Exercises hostile lifecycle and destroy exceptions during destruction.
 * @throws Nothing when fallback cleanup fences every plugin call independently.
 * @note The real library-unload probe must run only after shutdown, detach, and
 * exactly one destroy call, with no extra test-owned dynamic-library handle.
 */
TEST_F(SchedulerPluginLoaderTest,
       DestructorFallbackFencesEachExceptionAndUnloadsAfterDestroy) {
  auto& loader = SchedulerPluginLoader::instance();
  const auto plugin_path =
      scheduler_plugin_path("destroy_count_scheduler_plugin", true);
  ASSERT_TRUE(std::filesystem::exists(plugin_path));
  const auto trace_path = scheduler_owner_trace_path("fallback-exceptions");
  std::filesystem::remove(trace_path);

  {
    ScopedSchedulerFixtureEnvironment trace(kDestroyCountTraceEnvironment,
                                            trace_path.string());
    ScopedSchedulerFixtureEnvironment failures(kDestroyCountFailureEnvironment,
                                               "all");
    ASSERT_TRUE(loader.load_plugin(plugin_path));
    auto scheduler = loader.create("destroy_count_test", 0);
    ASSERT_NE(scheduler, nullptr);

    loader.clear_plugins();
    scheduler.reset();

    EXPECT_EQ(read_scheduler_owner_trace(trace_path),
              (std::vector<std::string>{"shutdown", "detach", "destroy",
                                        "library_unload"}));
  }

  std::filesystem::remove(trace_path);
}

/**
 * @brief Combines owner allocation failure with a throwing destroy export.
 * @throws Nothing when the raw guard preserves the original `std::bad_alloc`.
 * @note Active/destroy counts are read while the test-owned library handle is
 * still mapped; destroy must end the raw object exactly once before throwing.
 */
TEST_F(SchedulerPluginLoaderTest,
       OwnerAllocationFailureFencesDestroyAndPreservesBadAlloc) {
  auto& loader = SchedulerPluginLoader::instance();
  const auto plugin_path =
      scheduler_plugin_path("destroy_count_scheduler_plugin", true);
  ASSERT_TRUE(std::filesystem::exists(plugin_path));

#ifdef _WIN32
  HMODULE test_handle = LoadLibrary(plugin_path.string().c_str());
  ASSERT_NE(test_handle, nullptr);
  auto reset_counts = reinterpret_cast<void (*)()>(
      GetProcAddress(test_handle, "ps_test_scheduler_reset_counts"));
  auto active_count = reinterpret_cast<int (*)()>(
      GetProcAddress(test_handle, "ps_test_scheduler_active_count"));
  auto destroy_count = reinterpret_cast<int (*)()>(
      GetProcAddress(test_handle, "ps_test_scheduler_destroy_count"));
#else
  void* test_handle = dlopen(plugin_path.string().c_str(), RTLD_LAZY);
  ASSERT_NE(test_handle, nullptr) << dlerror();
  auto reset_counts = reinterpret_cast<void (*)()>(
      dlsym(test_handle, "ps_test_scheduler_reset_counts"));
  auto active_count = reinterpret_cast<int (*)()>(
      dlsym(test_handle, "ps_test_scheduler_active_count"));
  auto destroy_count = reinterpret_cast<int (*)()>(
      dlsym(test_handle, "ps_test_scheduler_destroy_count"));
#endif
  ASSERT_NE(reset_counts, nullptr);
  ASSERT_NE(active_count, nullptr);
  ASSERT_NE(destroy_count, nullptr);
  ASSERT_TRUE(loader.load_plugin(plugin_path));
  reset_counts();
  const std::string type_name = kLongDestroyCountSchedulerType;
  ScopedSchedulerFixtureEnvironment failures(kDestroyCountFailureEnvironment,
                                             "destroy_runtime_error");

  bool caught_bad_alloc = false;
  bool failpoint_fired = false;
  {
    ScopedSchedulerAllocationFailure failure(0);
    try {
      (void)loader.create(type_name, 0);
    } catch (const std::bad_alloc&) {
      caught_bad_alloc = true;
    }
    failpoint_fired = scheduler_allocation_probe::did_fire();
  }

  EXPECT_TRUE(failpoint_fired);
  EXPECT_TRUE(caught_bad_alloc);
  EXPECT_EQ(active_count(), 0);
  EXPECT_EQ(destroy_count(), 1);
  loader.clear_plugins();

#ifdef _WIN32
  FreeLibrary(test_handle);
#else
  dlclose(test_handle);
#endif
}

/**
 * @brief Injects failure into host owner allocation after plugin creation.
 * @throws Nothing when the raw guard propagates bad_alloc and destroys once.
 * @note The fixture create export itself performs no heap allocation.
 */
TEST_F(SchedulerPluginLoaderTest,
       OwnerAllocationFailureDestroysRawPluginInstanceExactlyOnce) {
  auto& loader = SchedulerPluginLoader::instance();
  const auto plugin_path =
      scheduler_plugin_path("destroy_count_scheduler_plugin", true);
  ASSERT_TRUE(std::filesystem::exists(plugin_path));

#ifdef _WIN32
  HMODULE test_handle = LoadLibrary(plugin_path.string().c_str());
  ASSERT_NE(test_handle, nullptr);
  auto reset_counts = reinterpret_cast<void (*)()>(
      GetProcAddress(test_handle, "ps_test_scheduler_reset_counts"));
  auto active_count = reinterpret_cast<int (*)()>(
      GetProcAddress(test_handle, "ps_test_scheduler_active_count"));
  auto destroy_count = reinterpret_cast<int (*)()>(
      GetProcAddress(test_handle, "ps_test_scheduler_destroy_count"));
#else
  void* test_handle = dlopen(plugin_path.string().c_str(), RTLD_LAZY);
  ASSERT_NE(test_handle, nullptr) << dlerror();
  auto reset_counts = reinterpret_cast<void (*)()>(
      dlsym(test_handle, "ps_test_scheduler_reset_counts"));
  auto active_count = reinterpret_cast<int (*)()>(
      dlsym(test_handle, "ps_test_scheduler_active_count"));
  auto destroy_count = reinterpret_cast<int (*)()>(
      dlsym(test_handle, "ps_test_scheduler_destroy_count"));
#endif
  ASSERT_NE(reset_counts, nullptr);
  ASSERT_NE(active_count, nullptr);
  ASSERT_NE(destroy_count, nullptr);
  ASSERT_TRUE(loader.load_plugin(plugin_path));
  reset_counts();
  const std::string type_name = kLongDestroyCountSchedulerType;

  bool caught_bad_alloc = false;
  bool failpoint_fired = false;
  {
    ScopedSchedulerAllocationFailure failure(0);
    try {
      (void)loader.create(type_name, 0);
    } catch (const std::bad_alloc&) {
      caught_bad_alloc = true;
    }
    failpoint_fired = scheduler_allocation_probe::did_fire();
  }

  EXPECT_TRUE(failpoint_fired);
  EXPECT_TRUE(caught_bad_alloc);
  EXPECT_EQ(active_count(), 0);
  EXPECT_EQ(destroy_count(), 1);
  loader.clear_plugins();

#ifdef _WIN32
  FreeLibrary(test_handle);
#else
  dlclose(test_handle);
#endif
}

/**
 * @brief Injects failure into the host owner's long type-name copy.
 * @throws Nothing when the raw guard destroys the instance exactly once.
 * @note Allocation index one follows successful owner storage allocation.
 */
TEST_F(SchedulerPluginLoaderTest,
       TypeNameCopyFailureDestroysRawPluginInstanceExactlyOnce) {
  auto& loader = SchedulerPluginLoader::instance();
  const auto plugin_path =
      scheduler_plugin_path("destroy_count_scheduler_plugin", true);
  ASSERT_TRUE(std::filesystem::exists(plugin_path));

#ifdef _WIN32
  HMODULE test_handle = LoadLibrary(plugin_path.string().c_str());
  ASSERT_NE(test_handle, nullptr);
  auto reset_counts = reinterpret_cast<void (*)()>(
      GetProcAddress(test_handle, "ps_test_scheduler_reset_counts"));
  auto active_count = reinterpret_cast<int (*)()>(
      GetProcAddress(test_handle, "ps_test_scheduler_active_count"));
  auto destroy_count = reinterpret_cast<int (*)()>(
      GetProcAddress(test_handle, "ps_test_scheduler_destroy_count"));
#else
  void* test_handle = dlopen(plugin_path.string().c_str(), RTLD_LAZY);
  ASSERT_NE(test_handle, nullptr) << dlerror();
  auto reset_counts = reinterpret_cast<void (*)()>(
      dlsym(test_handle, "ps_test_scheduler_reset_counts"));
  auto active_count = reinterpret_cast<int (*)()>(
      dlsym(test_handle, "ps_test_scheduler_active_count"));
  auto destroy_count = reinterpret_cast<int (*)()>(
      dlsym(test_handle, "ps_test_scheduler_destroy_count"));
#endif
  ASSERT_NE(reset_counts, nullptr);
  ASSERT_NE(active_count, nullptr);
  ASSERT_NE(destroy_count, nullptr);
  ASSERT_TRUE(loader.load_plugin(plugin_path));
  reset_counts();
  const std::string type_name = kLongDestroyCountSchedulerType;

  bool caught_bad_alloc = false;
  bool failpoint_fired = false;
  {
    ScopedSchedulerAllocationFailure failure(1);
    try {
      (void)loader.create(type_name, 0);
    } catch (const std::bad_alloc&) {
      caught_bad_alloc = true;
    }
    failpoint_fired = scheduler_allocation_probe::did_fire();
  }

  EXPECT_TRUE(failpoint_fired);
  EXPECT_TRUE(caught_bad_alloc);
  EXPECT_EQ(active_count(), 0);
  EXPECT_EQ(destroy_count(), 1);
  loader.clear_plugins();

#ifdef _WIN32
  FreeLibrary(test_handle);
#else
  dlclose(test_handle);
#endif
}

/**
 * @brief Exhausts every allocation along one post-open plugin load path.
 *
 * Each injected `std::bad_alloc` must leave registered types, metadata,
 * retained handles, and the pre-existing error prefix exactly unchanged. The
 * same candidate is then retried immediately without the failpoint and must
 * commit both real types plus its intentional duplicate-type diagnostic.
 *
 * @return Nothing.
 * @throws Nothing when every allocation failure preserves the strong
 * transaction guarantee and the first non-failing index completes the load.
 * @note Fixture callback counters prove the sweep reaches shadow construction,
 * type/metadata staging, conflict-error staging, and final handle bookkeeping
 * after the candidate library has opened. A fixed upper bound prevents a
 * broken allocation probe from hanging the test.
 */
TEST_F(SchedulerPluginLoaderTest,
       LoadAllocationFailuresPreserveRegistryAndAllowImmediateRetry) {
  auto& loader = SchedulerPluginLoader::instance();
  const auto baseline_plugin_path =
      scheduler_plugin_path("serial_debug_example_plugin");
  const auto candidate_plugin_path =
      scheduler_plugin_path("destroy_count_scheduler_plugin", true);
  const auto missing_plugin_path =
      scheduler_plugin_path("missing_load_transaction_plugin", true);
  ASSERT_TRUE(std::filesystem::exists(baseline_plugin_path));
  ASSERT_TRUE(std::filesystem::exists(candidate_plugin_path));
  ASSERT_FALSE(std::filesystem::exists(missing_plugin_path));
  ASSERT_TRUE(loader.load_plugin(baseline_plugin_path));

#ifdef _WIN32
  HMODULE test_handle = LoadLibrary(candidate_plugin_path.string().c_str());
  ASSERT_NE(test_handle, nullptr);
  auto reset_counts = reinterpret_cast<void (*)()>(
      GetProcAddress(test_handle, "ps_test_scheduler_reset_counts"));
  auto read_load_probe = reinterpret_cast<int (*)(int)>(
      GetProcAddress(test_handle, "ps_test_scheduler_load_probe_count"));
#else
  void* test_handle = dlopen(candidate_plugin_path.string().c_str(), RTLD_LAZY);
  ASSERT_NE(test_handle, nullptr) << dlerror();
  auto reset_counts = reinterpret_cast<void (*)()>(
      dlsym(test_handle, "ps_test_scheduler_reset_counts"));
  auto read_load_probe = reinterpret_cast<int (*)(int)>(
      dlsym(test_handle, "ps_test_scheduler_load_probe_count"));
#endif
  ASSERT_NE(reset_counts, nullptr);
  ASSERT_NE(read_load_probe, nullptr);

  ScopedSchedulerFixtureEnvironment duplicate_type(
      kDestroyCountDuplicateTypeEnvironment, "1");
  const std::string candidate_abs_path =
      std::filesystem::absolute(candidate_plugin_path).string();
  constexpr std::int64_t kMaximumAllocationIndex = 512;
  int injected_failures = 0;
  int failures_after_open = 0;
  int failures_during_shadow_copy = 0;
  int failures_during_type_staging = 0;
  int failures_after_complete_metadata = 0;
  int failures_after_duplicate_callback = 0;
  bool completed_without_failure = false;

  for (std::int64_t allocation_index = 0;
       allocation_index < kMaximumAllocationIndex; ++allocation_index) {
    SCOPED_TRACE(::testing::Message()
                 << "allocation_index=" << allocation_index);
    loader.clear_errors();
    ASSERT_FALSE(loader.load_plugin(missing_plugin_path));
    const SchedulerPluginRegistrySnapshot before =
        capture_plugin_registry(loader);
    ASSERT_EQ(before.load_errors.size(), 1u);
    reset_counts();

    bool load_result = false;
    bool caught_bad_alloc = false;
    bool failpoint_fired = false;
    {
      ScopedSchedulerAllocationFailure failure(allocation_index);
      try {
        load_result = loader.load_plugin(candidate_plugin_path);
      } catch (const std::bad_alloc&) {
        caught_bad_alloc = true;
      }
      failpoint_fired = scheduler_allocation_probe::did_fire();
    }

    const int count_calls =
        load_probe_count(read_load_probe, SchedulerLoadProbe::GetCount);
    const int name_calls =
        load_probe_count(read_load_probe, SchedulerLoadProbe::GetName);
    const int description_calls =
        load_probe_count(read_load_probe, SchedulerLoadProbe::GetDescription);
    const int version_calls =
        load_probe_count(read_load_probe, SchedulerLoadProbe::GetVersion);

    if (failpoint_fired) {
      ++injected_failures;
      failures_after_open += version_calls > 0 ? 1 : 0;
      failures_during_shadow_copy +=
          version_calls > 0 && count_calls == 0 ? 1 : 0;
      failures_during_type_staging +=
          count_calls > 0 && name_calls > 0 && description_calls < 2 ? 1 : 0;
      failures_after_complete_metadata += description_calls >= 2 ? 1 : 0;
      failures_after_duplicate_callback += name_calls >= 3 ? 1 : 0;

      EXPECT_TRUE(caught_bad_alloc);
      EXPECT_FALSE(load_result);
      expect_plugin_registry_equal(before, capture_plugin_registry(loader));

      ASSERT_TRUE(loader.load_plugin(candidate_plugin_path))
          << "same-path retry failed after allocation index "
          << allocation_index;
    } else {
      EXPECT_FALSE(caught_bad_alloc);
      ASSERT_TRUE(load_result);
      completed_without_failure = true;
    }

    const SchedulerPluginRegistrySnapshot committed =
        capture_plugin_registry(loader);
    EXPECT_EQ(committed.registered_types.size(),
              before.registered_types.size() + 2);
    EXPECT_TRUE(
        contains_type(committed.registered_types, kDestroyCountSchedulerType));
    EXPECT_TRUE(contains_type(committed.registered_types,
                              kLongDestroyCountSchedulerType));
    ASSERT_EQ(committed.loaded_plugins.size(),
              before.loaded_plugins.size() + 1);
    EXPECT_TRUE(std::any_of(
        committed.loaded_plugins.begin(), committed.loaded_plugins.end(),
        [&](const std::string& plugin) {
          return plugin.find(candidate_abs_path) != std::string::npos;
        }));
    ASSERT_EQ(committed.load_errors.size(), before.load_errors.size() + 1);
    EXPECT_TRUE(std::equal(before.load_errors.begin(), before.load_errors.end(),
                           committed.load_errors.begin()));
    EXPECT_NE(committed.load_errors.back().find("already registered by plugin"),
              std::string::npos);

    const auto short_info = loader.get_info(kDestroyCountSchedulerType);
    const auto long_info = loader.get_info(kLongDestroyCountSchedulerType);
    ASSERT_TRUE(short_info.has_value());
    ASSERT_TRUE(long_info.has_value());
    EXPECT_EQ(short_info->plugin_path, candidate_abs_path);
    EXPECT_EQ(long_info->plugin_path, candidate_abs_path);
    EXPECT_EQ(short_info->version, "test");
    EXPECT_EQ(long_info->version, "test");

    ASSERT_TRUE(loader.unload_plugin(candidate_plugin_path));
    SchedulerPluginRegistrySnapshot expected_after_unload = before;
    expected_after_unload.load_errors = committed.load_errors;
    expect_plugin_registry_equal(expected_after_unload,
                                 capture_plugin_registry(loader));

    if (completed_without_failure) {
      break;
    }
  }

  EXPECT_TRUE(completed_without_failure);
  EXPECT_GT(injected_failures, 0);
  EXPECT_GT(failures_after_open, 0);
  EXPECT_GT(failures_during_shadow_copy, 0);
  EXPECT_GT(failures_during_type_staging, 0);
  EXPECT_GT(failures_after_complete_metadata, 0);
  EXPECT_GT(failures_after_duplicate_callback, 0);

  loader.clear_plugins();
  loader.clear_errors();
#ifdef _WIN32
  FreeLibrary(test_handle);
#else
  dlclose(test_handle);
#endif
}

/**
 * @brief A discovery callback exception rolls back partial candidate metadata.
 * @return Nothing.
 * @throws Nothing when the original callback diagnostic propagates, the live
 * registry remains exact, and the same path succeeds immediately afterward.
 * @note The fixture throws from the second description callback, after its
 * first type has been fully staged but before any shadow state is committed.
 */
TEST_F(SchedulerPluginLoaderTest,
       LoadCallbackExceptionPreservesRegistryAndAllowsImmediateRetry) {
  auto& loader = SchedulerPluginLoader::instance();
  const auto baseline_plugin_path =
      scheduler_plugin_path("serial_debug_example_plugin");
  const auto candidate_plugin_path =
      scheduler_plugin_path("destroy_count_scheduler_plugin", true);
  const auto missing_plugin_path =
      scheduler_plugin_path("missing_callback_transaction_plugin", true);
  ASSERT_TRUE(std::filesystem::exists(baseline_plugin_path));
  ASSERT_TRUE(std::filesystem::exists(candidate_plugin_path));
  ASSERT_FALSE(std::filesystem::exists(missing_plugin_path));
  ASSERT_TRUE(loader.load_plugin(baseline_plugin_path));
  ASSERT_FALSE(loader.load_plugin(missing_plugin_path));
  const SchedulerPluginRegistrySnapshot before =
      capture_plugin_registry(loader);

  {
    ScopedSchedulerFixtureEnvironment failure(
        kDestroyCountLoadFailureEnvironment, "description_runtime_error");
    try {
      (void)loader.load_plugin(candidate_plugin_path);
      FAIL() << "fixture metadata callback failure did not propagate";
    } catch (const std::runtime_error& error) {
      EXPECT_STREQ(error.what(), "fixture scheduler description failure");
    }
  }

  expect_plugin_registry_equal(before, capture_plugin_registry(loader));
  EXPECT_FALSE(loader.is_registered(kDestroyCountSchedulerType));
  EXPECT_FALSE(loader.is_registered(kLongDestroyCountSchedulerType));

  ASSERT_TRUE(loader.load_plugin(candidate_plugin_path));
  EXPECT_TRUE(loader.is_registered(kDestroyCountSchedulerType));
  EXPECT_TRUE(loader.is_registered(kLongDestroyCountSchedulerType));
  ASSERT_TRUE(loader.unload_plugin(candidate_plugin_path));
  expect_plugin_registry_equal(before, capture_plugin_registry(loader));
  loader.clear_plugins();
  loader.clear_errors();
}

}  // namespace ps
