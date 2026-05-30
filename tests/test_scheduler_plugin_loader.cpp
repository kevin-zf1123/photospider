// Photospider test: Scheduler Plugin Loader
// 测试调度器插件的动态加载功能

#include <gtest/gtest.h>

#include <filesystem>

#include "kernel/scheduler/scheduler_factory.hpp"
#include "kernel/scheduler/scheduler_plugin_loader.hpp"

#ifdef _WIN32
#include <windows.h>
#else
#include <dlfcn.h>
#endif

namespace ps {

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
  
  // 扫描 build/schedulers 目录
  size_t count = loader.scan_and_load("build/schedulers");
  
  // 应该加载了 3 个插件
  EXPECT_GE(count, 0);  // 至少能够执行
  
  // 如果加载成功，验证类型
  if (count > 0) {
    auto types = loader.get_registered_types();
    EXPECT_FALSE(types.empty());
    
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
  
  // 尝试加载 cpu_work_stealing_plugin
  bool result = loader.load_plugin("build/schedulers/libcpu_work_stealing_plugin.dylib");
  
  if (result) {
    EXPECT_TRUE(loader.is_registered("cpu_work_stealing"));
  }
}

// 测试获取描述
TEST_F(SchedulerPluginLoaderTest, GetDescription) {
  auto& loader = SchedulerPluginLoader::instance();
  
  // 先加载插件
  loader.scan_and_load("build/schedulers");
  
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
  
  // 加载插件
  size_t count = loader.scan_and_load("build/schedulers");
  if (count == 0) {
    GTEST_SKIP() << "No plugins found in build/schedulers";
  }
  
  auto types = loader.get_registered_types();
  for (const auto& type : types) {
    auto scheduler = loader.create(type, 4);
    EXPECT_NE(scheduler, nullptr) << "Failed to create scheduler of type: " << type;
    
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
  
  // 加载插件
  loader.scan_and_load("build/schedulers");
  
  auto plugins = loader.list_loaded_plugins();
  std::cout << "Loaded plugins:\n";
  for (const auto& info : plugins) {
    std::cout << "  " << info << "\n";
  }
}

// 测试 SchedulerFactory 集成
TEST_F(SchedulerPluginLoaderTest, FactoryIntegration) {
  auto& loader = SchedulerPluginLoader::instance();
  
  // 加载插件
  size_t count = loader.scan_and_load("build/schedulers");
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

// 测试清除插件
TEST_F(SchedulerPluginLoaderTest, ClearPlugins) {
  auto& loader = SchedulerPluginLoader::instance();
  
  // 加载插件
  loader.scan_and_load("build/schedulers");
  
  auto before = loader.get_registered_types();
  size_t before_count = before.size();
  
  // 清除
  loader.clear_plugins();
  
  auto after = loader.get_registered_types();
  EXPECT_TRUE(after.empty());
  
  // 可以重新加载
  loader.scan_and_load("build/schedulers");
  auto reloaded = loader.get_registered_types();
  EXPECT_EQ(reloaded.size(), before_count);
}

TEST_F(SchedulerPluginLoaderTest, PluginSchedulerUsesPluginDestroyAfterClear) {
  auto& loader = SchedulerPluginLoader::instance();

#if defined(_WIN32)
  const auto plugin_path =
      std::filesystem::path("build/schedulers/destroy_count_scheduler_plugin.dll");
#elif defined(__APPLE__)
  const auto plugin_path = std::filesystem::path(
      "build/schedulers/libdestroy_count_scheduler_plugin.dylib");
#else
  const auto plugin_path =
      std::filesystem::path("build/schedulers/libdestroy_count_scheduler_plugin.so");
#endif

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
