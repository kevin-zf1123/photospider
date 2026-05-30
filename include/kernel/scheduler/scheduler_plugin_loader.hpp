// Photospider kernel: Scheduler Plugin Loader
// 动态加载调度器插件
#pragma once

#include <filesystem>
#include <map>
#include <memory>
#include <string>
#include <vector>

#include "kernel/scheduler/i_scheduler.hpp"

namespace ps {

namespace fs = std::filesystem;

// =============================================================================
// SchedulerPluginInfo: 调度器插件信息
// =============================================================================
struct SchedulerPluginInfo {
  std::string type_name;       // 调度器类型名称
  std::string description;     // 描述
  std::string plugin_path;     // 插件文件路径
  std::string version;         // 插件版本
  bool is_builtin = false;     // 是否为内置调度器
};

// =============================================================================
// SchedulerPluginLoader: 调度器插件加载器
// 负责扫描、加载和管理调度器插件
// =============================================================================
class SchedulerPluginLoader {
 public:
  /// @brief 获取单例实例
  static SchedulerPluginLoader& instance();

  /// @brief 扫描指定目录加载调度器插件
  /// @param dir_paths 目录路径列表，支持 "path/**" 递归扫描
  /// @return 成功加载的插件数量
  size_t scan_and_load(const std::vector<std::string>& dir_paths);
  
  /// @brief 扫描单个目录加载调度器插件
  /// @param dir_path 目录路径
  /// @return 成功加载的插件数量
  size_t scan_and_load(const std::string& dir_path);

  /// @brief 加载单个插件文件
  /// @param plugin_path 插件文件路径
  /// @return 是否加载成功
  bool load_plugin(const fs::path& plugin_path);

  /// @brief 卸载指定路径的插件
  /// @param plugin_path 插件文件路径
  /// @return 是否卸载成功
  bool unload_plugin(const fs::path& plugin_path);

  /// @brief 获取所有已注册的调度器类型
  /// @return 调度器类型名称列表
  std::vector<std::string> get_registered_types() const;

  /// @brief 检查指定类型是否已注册
  /// @param type_name 调度器类型名称
  /// @return 是否已注册
  bool is_registered(const std::string& type_name) const;

  /// @brief 获取指定类型的调度器信息
  /// @param type_name 调度器类型名称
  /// @return 调度器信息，如果不存在则返回 nullopt
  std::optional<SchedulerPluginInfo> get_info(const std::string& type_name) const;

  /// @brief 获取所有已加载的调度器信息
  /// @return 调度器信息列表
  std::vector<SchedulerPluginInfo> get_all_info() const;

  /// @brief 获取调度器类型的描述
  /// @param type_name 调度器类型名称
  /// @return 描述字符串
  std::string get_description(const std::string& type_name) const;

  /// @brief 创建指定类型的调度器实例
  /// @param type_name 调度器类型名称
  /// @param num_workers 工作线程数（0 表示自动）
  /// @return 调度器实例，如果类型不存在则返回 nullptr
  std::unique_ptr<IScheduler> create(const std::string& type_name,
                                     unsigned int num_workers = 0);

  /// @brief 注册内置调度器（不通过插件加载）
  /// @param type_name 调度器类型名称
  /// @param description 描述
  /// @param factory 工厂函数
  void register_builtin(
      const std::string& type_name, const std::string& description,
      std::function<std::unique_ptr<IScheduler>(unsigned int)> factory);

  /// @brief 清除所有已加载的插件（保留内置）
  void clear_plugins();

  /// @brief 获取已加载的插件路径列表
  /// @return 插件信息字符串列表（格式：path (types)）
  std::vector<std::string> list_loaded_plugins() const;

  /// @brief 获取加载错误列表
  /// @return 错误信息列表
  const std::vector<std::string>& get_load_errors() const;

  /// @brief 清除错误列表
  void clear_errors();

 private:
  SchedulerPluginLoader() = default;
  ~SchedulerPluginLoader();

  SchedulerPluginLoader(const SchedulerPluginLoader&) = delete;
  SchedulerPluginLoader& operator=(const SchedulerPluginLoader&) = delete;

  // 内部加载方法（无锁版本）
  bool load_plugin_internal_unlocked(const fs::path& plugin_path);

  // 内部插件句柄
  struct PluginHandle {
    void* handle = nullptr;                    // 动态库句柄
    std::shared_ptr<void> library;             // Keeps library loaded for live instances
    std::string path;                          // 文件路径
    std::vector<std::string> registered_types; // 该插件注册的类型
    
    // 函数指针
    int (*get_count)() = nullptr;
    const char* (*get_name)(int) = nullptr;
    const char* (*get_description)(int) = nullptr;
    IScheduler* (*create)(const char*, unsigned int) = nullptr;
    void (*destroy)(IScheduler*) = nullptr;
    const char* (*get_version)() = nullptr;
  };

  // 内置调度器工厂
  struct BuiltinScheduler {
    std::string description;
    std::function<std::unique_ptr<IScheduler>(unsigned int)> factory;
  };

  // 已加载的插件
  std::map<std::string, PluginHandle> loaded_plugins_;  // path -> handle
  
  // 类型到插件路径的映射
  std::map<std::string, std::string> type_to_plugin_;
  
  // 类型到信息的映射
  std::map<std::string, SchedulerPluginInfo> type_info_;
  
  // 内置调度器
  std::map<std::string, BuiltinScheduler> builtins_;
  
  // 加载错误
  std::vector<std::string> load_errors_;
  
  // 互斥锁
  mutable std::mutex mutex_;
};

}  // namespace ps
