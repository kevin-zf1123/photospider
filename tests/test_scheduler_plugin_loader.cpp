// Photospider test: Scheduler Plugin Loader
// 测试调度器插件的动态加载功能

#include <gtest/gtest.h>

#include <algorithm>
#include <filesystem>
#include <iostream>
#include <string>
#include <vector>

#include "kernel/scheduler/scheduler_factory.hpp"
#include "kernel/scheduler/scheduler_plugin_api.hpp"
#include "kernel/scheduler/scheduler_plugin_loader.hpp"

#ifdef _WIN32
#include <windows.h>
#else
#include <dlfcn.h>
#endif

namespace ps {
namespace {

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

}  // namespace ps
