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
  std::string type_name;    // 调度器类型名称
  std::string description;  // 描述
  std::string plugin_path;  // 插件文件路径
  std::string version;      // 插件版本
  bool is_builtin = false;  // 是否为内置调度器
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

  /**
   * @brief Loads one scheduler plugin as a strong registry transaction.
   *
   * The candidate library, discovered exports, type mappings, metadata,
   * retained handle, and diagnostics are staged while `mutex_` excludes other
   * registry operations. Successful staging is published with no-throw swaps.
   *
   * @param plugin_path Plugin library path normalized to an absolute key.
   * @return True when the plugin was already loaded or its complete staged
   * state committed; false when the library cannot open or required exports
   * are missing and a diagnostic was committed.
   * @throws std::bad_alloc unchanged when path normalization, metadata copy,
   * shadow-state construction, diagnostic staging, or handle insertion cannot
   * allocate.
   * @throws Any exception propagated by a plugin discovery or metadata
   * callback, preserving its original identity.
   * @note Every exceptional exit leaves registered types, type metadata,
   * retained handles, and the load-error prefix exactly as they were before
   * the call. The candidate library remains mapped until all staged
   * plugin-derived state has been destroyed, so the same path can be retried
   * immediately.
   */
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
  std::optional<SchedulerPluginInfo> get_info(
      const std::string& type_name) const;

  /// @brief 获取所有已加载的调度器信息
  /// @return 调度器信息列表
  std::vector<SchedulerPluginInfo> get_all_info() const;

  /// @brief 获取调度器类型的描述
  /// @param type_name 调度器类型名称
  /// @return 描述字符串
  std::string get_description(const std::string& type_name) const;

  /**
   * @brief Creates one built-in or plugin-provided scheduler instance.
   *
   * Plugin creation establishes a stack RAII guard immediately after the raw
   * create export returns. The guard spans runtime-interface validation,
   * wrapper allocation, and copied type-name construction, then transfers
   * destruction only after the complete owner exists.
   *
   * @param type_name Registered scheduler type name.
   * @param num_workers Requested worker count; zero selects implementation
   * defaults.
   * @return Owning scheduler pointer, or nullptr for an unknown type, a null
   * plugin result, or an instance lacking `SchedulerTaskRuntime`.
   * @throws std::bad_alloc unchanged if built-in creation, wrapper allocation,
   * or type-name copy exhausts memory.
   * @throws Any exception thrown by a built-in/plugin create function or owner
   * construction, preserving its original identity.
   * @note Every plugin raw instance created before an exceptional exit is
   * destroyed exactly once through its plugin destroy export while the library
   * remains loaded. Exceptions from that cleanup destroy export are suppressed
   * so they cannot replace an owner-construction exception. Once returned,
   * explicit scheduler lifecycle calls continue to propagate plugin exceptions;
   * only the owner's destructor uses best-effort no-throw fallback cleanup.
   */
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

  /**
   * @brief Implements one scheduler-plugin load while the caller holds mutex_.
   * @param plugin_path Candidate path supplied by direct load or directory
   * scan.
   * @return The same committed/already-loaded or recoverable-failure result as
   * `load_plugin()`.
   * @throws std::bad_alloc or any plugin callback exception unchanged.
   * @note All observable registry mutations use `RegistryState`; callers must
   * hold `mutex_` for the complete call.
   */
  bool load_plugin_internal_unlocked(const fs::path& plugin_path);

  /**
   * @brief Appends one recoverable load diagnostic with a strong guarantee.
   * @param error Fully constructed diagnostic to append.
   * @return Nothing.
   * @throws std::bad_alloc if error-vector shadow construction or growth fails.
   * @note The caller must hold `mutex_`. Allocation failure leaves
   * `load_errors_` unchanged.
   */
  void append_load_error_unlocked(std::string error);

  /**
   * @brief Retains one loaded scheduler library and its resolved ABI exports.
   * @note `library` is the authoritative RAII lifetime. `handle` is retained
   * only for platform symbol lookup and is never closed independently.
   */
  struct PluginHandle {
    /** @brief Native handle used for symbol lookup. */
    void* handle = nullptr;
    /**
     * @brief Shared lifetime that closes the native handle on final release.
     */
    std::shared_ptr<void> library;
    /** @brief Absolute registry key for the library. */
    std::string path;
    /** @brief Types successfully registered by this plugin. */
    std::vector<std::string> registered_types;

    /** @brief Required type-count export. */
    int (*get_count)() = nullptr;
    /** @brief Required indexed type-name export. */
    const char* (*get_name)(int) = nullptr;
    /** @brief Optional indexed description export. */
    const char* (*get_description)(int) = nullptr;
    /** @brief Required scheduler creation export. */
    IScheduler* (*create)(const char*, unsigned int) = nullptr;
    /** @brief Required scheduler destruction export. */
    void (*destroy)(IScheduler*) = nullptr;
    /** @brief Optional plugin version export. */
    const char* (*get_version)() = nullptr;
  };

  /**
   * @brief Allocation-owning shadow of every observable plugin registry.
   *
   * Construction copies the live state before candidate callbacks mutate any
   * registry. `commit()` swaps all completed containers while `mutex_` remains
   * held; standard allocator equality makes every swap non-throwing.
   *
   * @note The candidate `PluginHandle` is declared before this shadow in the
   * load routine. Failed shadow state is therefore destroyed before the
   * candidate shared-library lifetime is released.
   */
  struct RegistryState {
    /**
     * @brief Copies the loader's complete caller-visible plugin state.
     * @param loader Locked loader whose state is staged.
     * @throws std::bad_alloc if any container, key, metadata, or diagnostic
     * copy cannot allocate.
     */
    explicit RegistryState(const SchedulerPluginLoader& loader);

    /**
     * @brief Publishes this complete shadow into a locked loader.
     * @param loader Loader receiving the staged state.
     * @return Nothing.
     * @throws Nothing.
     * @note The retained-handle map is swapped first. The enclosing mutex keeps
     * the four swaps externally atomic, and no allocation or plugin callback
     * occurs during commit.
     */
    void commit(SchedulerPluginLoader& loader) noexcept;

    /** @brief Staged absolute-path to retained-handle registry. */
    std::map<std::string, PluginHandle> loaded_plugins;
    /** @brief Staged scheduler-type to plugin-path registry. */
    std::map<std::string, std::string> type_to_plugin;
    /** @brief Staged scheduler metadata registry. */
    std::map<std::string, SchedulerPluginInfo> type_info;
    /** @brief Staged recoverable diagnostic sequence. */
    std::vector<std::string> load_errors;
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
