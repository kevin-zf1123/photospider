// Photospider kernel: Scheduler Plugin Loader implementation
#include "scheduler/scheduler_plugin_loader.hpp"

#include <algorithm>
#include <iostream>
#include <memory>
#include <sstream>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include "kernel/scheduler/scheduler_plugin_api.hpp"
#include "kernel/scheduler/scheduler_task_runtime.hpp"

#ifdef _WIN32
#include <windows.h>
#else
#include <dlfcn.h>
#endif

namespace ps {

namespace {

/**
 * @brief Wraps one native dynamic-library handle in shared lifetime ownership.
 *
 * @param handle Non-null handle returned by `LoadLibrary` or `dlopen`.
 * @return Shared lifetime whose final release unloads `handle`.
 * @throws std::bad_alloc if the shared ownership control block cannot allocate.
 * @note Scheduler owners retain a copy through their plugin destroy call; the
 * platform unload function is invoked only when the final copy is released.
 */
std::shared_ptr<void> make_library_lifetime(void* handle) {
#ifdef _WIN32
  return std::shared_ptr<void>(handle, [](void* ptr) {
    if (ptr) {
      FreeLibrary(static_cast<HMODULE>(ptr));
    }
  });
#else
  return std::shared_ptr<void>(handle, [](void* ptr) {
    if (ptr) {
      dlclose(ptr);
    }
  });
#endif
}

/**
 * @brief Invokes one plugin destroy export behind a no-throw ABI fence.
 *
 * @param scheduler Raw plugin scheduler whose ownership is being released.
 * @param destroy Plugin export paired with `scheduler`.
 * @return Nothing.
 * @throws Nothing; every plugin exception, including `std::bad_alloc`, is
 * suppressed because cleanup callers are `noexcept`.
 * @note The export is invoked exactly once. A hostile export that throws before
 * ending the object lifetime is not retried because a second ABI call could
 * double-destroy partially released plugin state. The caller must retain the
 * dynamic-library lifetime until this function returns.
 */
void destroy_plugin_scheduler_noexcept(IScheduler* scheduler,
                                       void (*destroy)(IScheduler*)) noexcept {
  try {
    destroy(scheduler);
  } catch (...) {
    // Plugin exceptions cannot cross a host cleanup/destructor boundary.
  }
}

/**
 * @brief Runs best-effort scheduler lifecycle fallback during owner
 * destruction.
 *
 * The fallback first attempts `shutdown()` and then independently attempts
 * `detach()`. Each call has its own exception fence so failure in one stage
 * cannot prevent the next stage or the subsequent plugin destroy export.
 *
 * @param scheduler Live plugin scheduler retained by the host owner.
 * @return Nothing.
 * @throws Nothing; ordinary and resource-exhaustion exceptions are suppressed.
 * @note This helper is only for `noexcept` destruction. Explicit public owner
 * calls delegate directly and preserve the plugin exception identity.
 */
void run_scheduler_destructor_fallback_noexcept(
    IScheduler* scheduler) noexcept {
  try {
    scheduler->shutdown();
  } catch (...) {
    // Continue to detach and destroy even when shutdown is hostile.
  }
  try {
    scheduler->detach();
  } catch (...) {
    // Continue to destroy even when detach is hostile.
  }
}

/**
 * @brief Host-side owner/delegator for one plugin-created scheduler.
 *
 * The wrapper delegates both required C++ runtime interfaces, destroys the raw
 * scheduler through the plugin ABI, and retains the dynamic library until that
 * destroy call has completed.
 *
 * @note `library_` is declared after the raw pointers and therefore remains
 * alive throughout the destructor body. Construction may allocate for the
 * wrapper and copied type name; `RawPluginSchedulerGuard` owns the raw instance
 * until construction succeeds.
 */
class PluginSchedulerOwner final : public IScheduler,
                                   public SchedulerTaskRuntime {
 public:
  /**
   * @brief Takes ownership of a validated plugin scheduler.
   * @param scheduler Raw instance implementing both scheduler interfaces.
   * @param destroy Plugin ABI destroy function paired with `scheduler`.
   * @param library Shared library lifetime retained through destruction.
   * @param type_name Registered scheduler type copied for `name()`.
   * @throws std::bad_alloc if `type_name` copy allocation fails.
   * @note The caller retains a temporary raw-instance guard until this
   * constructor and owner allocation both succeed.
   */
  PluginSchedulerOwner(IScheduler* scheduler, void (*destroy)(IScheduler*),
                       std::shared_ptr<void> library, std::string type_name)
      : type_name_(std::move(type_name)),
        scheduler_(scheduler),
        task_runtime_(dynamic_cast<SchedulerTaskRuntime*>(scheduler)),
        destroy_(destroy),
        library_(std::move(library)) {}

  /**
   * @brief Best-effort stops, detaches, and plugin-destroys the owned instance.
   *
   * Lifecycle fallback stages are fenced independently. Ownership is cleared
   * before plugin calls begin, and the destroy export is invoked exactly once
   * even when shutdown or detach fails.
   *
   * @throws Nothing; all plugin lifecycle and destroy exceptions are
   * suppressed.
   * @note `library_` remains alive throughout this destructor body and is
   * released only after the single destroy attempt returns.
   */
  ~PluginSchedulerOwner() noexcept override {
    IScheduler* scheduler = std::exchange(scheduler_, nullptr);
    task_runtime_ = nullptr;
    if (scheduler) {
      run_scheduler_destructor_fallback_noexcept(scheduler);
      destroy_plugin_scheduler_noexcept(scheduler, destroy_);
    }
  }

  /**
   * @brief Explicitly attaches the plugin scheduler to a graph runtime.
   * @param runtime Borrowed graph runtime forwarded unchanged.
   * @return Nothing.
   * @throws Any plugin exception unchanged.
   * @note This caller-visible path intentionally does not use destructor
   * fences.
   */
  void attach(GraphRuntime* runtime) override { scheduler_->attach(runtime); }
  /**
   * @brief Explicitly detaches the plugin scheduler.
   * @return Nothing.
   * @throws Any plugin exception, including `std::bad_alloc`, unchanged.
   * @note Destruction uses an independent no-throw fallback instead.
   */
  void detach() override { scheduler_->detach(); }
  /**
   * @brief Explicitly starts the plugin scheduler.
   * @return Nothing.
   * @throws Any plugin exception unchanged.
   */
  void start() override { scheduler_->start(); }
  /**
   * @brief Explicitly shuts down the plugin scheduler.
   * @return Nothing.
   * @throws Any plugin exception, including resource exhaustion, unchanged.
   * @note Destruction uses an independent no-throw fallback instead.
   */
  void shutdown() override { scheduler_->shutdown(); }

  /**
   * @brief Returns the host-owned registered scheduler type.
   * @return Copied type name retained independently of plugin metadata storage.
   * @throws std::bad_alloc if return-value construction cannot allocate.
   */
  std::string name() const override { return type_name_; }
  /**
   * @brief Delegates runtime statistics to the plugin scheduler.
   * @return Plugin-provided statistics string.
   * @throws Any plugin exception unchanged.
   */
  std::string get_stats() const override { return scheduler_->get_stats(); }
  /**
   * @brief Queries the plugin scheduler lifecycle state.
   * @return Plugin-reported running flag.
   * @throws Any plugin exception unchanged.
   */
  bool is_running() const override { return scheduler_->is_running(); }

  /**
   * @brief Queries the validated plugin task-runtime state.
   * @return Plugin-reported task-runtime running flag.
   * @throws Any plugin exception unchanged.
   */
  bool task_runtime_running() const override {
    return require_task_runtime().task_runtime_running();
  }

  /**
   * @brief Delegates runtime device discovery to the plugin scheduler.
   * @return Plugin-provided device list in its native preference order.
   * @throws Any plugin exception, including allocation failure, unchanged.
   * @note Forwarding is required because the base implementation reports CPU
   * only and would otherwise hide accelerator capabilities from compute
   * planning.
   */
  std::vector<Device> available_devices() const override {
    return require_task_runtime().available_devices();
  }

  /**
   * @brief Delegates one initial callback batch to the plugin task runtime.
   * @param tasks Initial ready callbacks transferred to the plugin.
   * @param total_task_count Total completion count for the batch.
   * @param priority Scheduler-supported priority hint.
   * @return Nothing.
   * @throws Any plugin exception unchanged.
   */
  void submit_initial_tasks(
      std::vector<Task>&& tasks, int total_task_count,
      SchedulerTaskPriority priority = SchedulerTaskPriority::Normal) override {
    require_task_runtime().submit_initial_tasks(std::move(tasks),
                                                total_task_count, priority);
  }

  /**
   * @brief Delegates one initial borrowed-handle batch to the plugin runtime.
   * @param handles Initial ready task handles transferred to the plugin.
   * @param total_task_count Total completion count for the new batch.
   * @param priority Scheduler-supported priority hint.
   * @return Nothing.
   * @throws Any plugin exception unchanged.
   * @note Direct delegation preserves the plugin runtime's transactional batch
   * semantics instead of using the base closure-conversion fallback.
   */
  void submit_initial_task_handles(
      std::vector<TaskHandle>&& handles, int total_task_count,
      SchedulerTaskPriority priority = SchedulerTaskPriority::Normal) override {
    require_task_runtime().submit_initial_task_handles(
        std::move(handles), total_task_count, priority);
  }

  /**
   * @brief Delegates one worker-origin ready callback to the plugin runtime.
   * @param task Ready callback transferred to the plugin.
   * @param priority Scheduler-supported priority hint.
   * @return Nothing.
   * @throws Any plugin exception unchanged.
   */
  void submit_ready_task_from_worker(
      Task&& task,
      SchedulerTaskPriority priority = SchedulerTaskPriority::Normal) override {
    require_task_runtime().submit_ready_task_from_worker(std::move(task),
                                                         priority);
  }

  /**
   * @brief Delegates one worker-origin borrowed task handle to the plugin.
   * @param handle Ready task handle borrowed from the active dispatcher.
   * @param priority Scheduler-supported priority hint.
   * @return Nothing.
   * @throws Any plugin exception unchanged.
   * @note The plugin receives the native handle API without an allocating
   * closure wrapper.
   */
  void submit_ready_task_handle_from_worker(
      TaskHandle handle,
      SchedulerTaskPriority priority = SchedulerTaskPriority::Normal) override {
    require_task_runtime().submit_ready_task_handle_from_worker(handle,
                                                                priority);
  }

  /**
   * @brief Delegates a worker-origin borrowed-handle batch to the plugin.
   * @param handles Ready task handles transferred as one batch.
   * @param priority Scheduler-supported priority hint.
   * @return Nothing.
   * @throws Any plugin exception unchanged.
   * @note Direct delegation prevents the base implementation from publishing a
   * prefix through repeated single-handle submissions.
   */
  void submit_ready_task_handles_from_worker(
      std::vector<TaskHandle>&& handles,
      SchedulerTaskPriority priority = SchedulerTaskPriority::Normal) override {
    require_task_runtime().submit_ready_task_handles_from_worker(
        std::move(handles), priority);
  }

  /**
   * @brief Delegates one caller-thread ready callback to the plugin runtime.
   * @param task Ready callback transferred to the plugin.
   * @param priority Scheduler-supported priority hint.
   * @param epoch Optional plugin batch epoch.
   * @return Nothing.
   * @throws Any plugin exception unchanged.
   */
  void submit_ready_task_any_thread(
      Task&& task,
      SchedulerTaskPriority priority = SchedulerTaskPriority::Normal,
      std::optional<uint64_t> epoch = std::nullopt) override {
    require_task_runtime().submit_ready_task_any_thread(std::move(task),
                                                        priority, epoch);
  }

  /**
   * @brief Delegates one caller-thread borrowed task handle to the plugin.
   * @param handle Ready task handle borrowed from the active dispatcher.
   * @param priority Scheduler-supported priority hint.
   * @param epoch Optional plugin batch epoch.
   * @return Nothing.
   * @throws Any plugin exception unchanged.
   * @note The plugin's native handle overload remains the exception boundary.
   */
  void submit_ready_task_handle_any_thread(
      TaskHandle handle,
      SchedulerTaskPriority priority = SchedulerTaskPriority::Normal,
      std::optional<uint64_t> epoch = std::nullopt) override {
    require_task_runtime().submit_ready_task_handle_any_thread(handle, priority,
                                                               epoch);
  }

  /**
   * @brief Delegates a caller-thread borrowed-handle batch to the plugin.
   * @param handles Ready task handles transferred as one batch.
   * @param priority Scheduler-supported priority hint.
   * @param epoch Optional plugin batch epoch shared by every handle.
   * @return Nothing.
   * @throws Any plugin exception unchanged.
   * @note Direct delegation preserves the plugin runtime's all-or-nothing queue
   * publication and original exception identity.
   */
  void submit_ready_task_handles_any_thread(
      std::vector<TaskHandle>&& handles,
      SchedulerTaskPriority priority = SchedulerTaskPriority::Normal,
      std::optional<uint64_t> epoch = std::nullopt) override {
    require_task_runtime().submit_ready_task_handles_any_thread(
        std::move(handles), priority, epoch);
  }

  /**
   * @brief Waits for the plugin task runtime to complete its active batch.
   * @return Nothing.
   * @throws Any plugin exception unchanged.
   */
  void wait_for_completion() override {
    require_task_runtime().wait_for_completion();
  }

  /**
   * @brief Publishes one task exception to the plugin runtime.
   * @param e Original exception identity forwarded unchanged.
   * @return Nothing.
   * @throws Any plugin exception unchanged.
   */
  void set_exception(std::exception_ptr e) override {
    require_task_runtime().set_exception(e);
  }

  /**
   * @brief Increases plugin runtime completion accounting.
   * @param delta Count forwarded unchanged.
   * @return Nothing.
   * @throws Any plugin exception unchanged.
   */
  void inc_tasks_to_complete(int delta) override {
    require_task_runtime().inc_tasks_to_complete(delta);
  }

  /**
   * @brief Decrements plugin runtime completion accounting once.
   * @return Nothing.
   * @throws Any plugin exception unchanged.
   */
  void dec_tasks_to_complete() override {
    require_task_runtime().dec_tasks_to_complete();
  }

  /**
   * @brief Forwards one scheduler trace event to the plugin runtime.
   * @param action Scheduler trace action.
   * @param node_id Associated graph node id.
   * @return Nothing.
   * @throws Any plugin exception unchanged.
   */
  void log_event(SchedulerTraceAction action, int node_id) override {
    require_task_runtime().log_event(action, node_id);
  }

 private:
  /**
   * @brief Returns the validated task-runtime side of the plugin instance.
   * @return Borrowed runtime reference valid for this owner lifetime.
   * @throws std::runtime_error only if construction invariants were violated.
   * @note Creation rejects an instance without this interface before ownership
   * transfer, so production calls do not normally throw here.
   */
  SchedulerTaskRuntime& require_task_runtime() const {
    if (!task_runtime_) {
      throw std::runtime_error(
          "Plugin scheduler does not implement SchedulerTaskRuntime");
    }
    return *task_runtime_;
  }

  std::string type_name_;
  IScheduler* scheduler_ = nullptr;
  SchedulerTaskRuntime* task_runtime_ = nullptr;
  void (*destroy_)(IScheduler*) = nullptr;
  std::shared_ptr<void> library_;
};

/**
 * @brief Stack owner for a raw scheduler returned by plugin create.
 *
 * The guard is established immediately after a non-null plugin instance is
 * returned and remains active through runtime-interface validation, heap
 * allocation of `PluginSchedulerOwner`, and its copied type-name construction.
 * It performs no dynamic allocation of its own.
 *
 * @note `library_` retains the plugin until after `destroy_` completes. Calling
 * `release()` transfers instance destruction to the fully constructed owner;
 * every exceptional exit before that point destroys the raw instance exactly
 * once through the plugin ABI.
 */
class RawPluginSchedulerGuard final {
 public:
  /**
   * @brief Starts guarding one plugin-created scheduler instance.
   *
   * @param scheduler Raw instance returned by the plugin create export.
   * @param destroy Plugin destroy export paired with `scheduler`.
   * @param library Shared dynamic-library lifetime already allocated at load.
   * @throws Nothing; copying `std::shared_ptr` only increments its control
   * block reference count.
   * @note `scheduler` and `destroy` must both be non-null.
   */
  RawPluginSchedulerGuard(IScheduler* scheduler, void (*destroy)(IScheduler*),
                          const std::shared_ptr<void>& library) noexcept
      : scheduler_(scheduler), destroy_(destroy), library_(library) {}

  /**
   * @brief Destroys an untransferred instance while its library is mapped.
   *
   * @throws Nothing; a hostile destroy-export exception is suppressed so the
   * original owner-construction exception continues unchanged.
   * @note The destroy export is attempted exactly once. The `library_` member
   * is released only after this destructor body and the fenced call complete.
   */
  ~RawPluginSchedulerGuard() noexcept {
    IScheduler* scheduler = std::exchange(scheduler_, nullptr);
    if (scheduler) {
      destroy_plugin_scheduler_noexcept(scheduler, destroy_);
    }
  }

  RawPluginSchedulerGuard(const RawPluginSchedulerGuard&) = delete;
  RawPluginSchedulerGuard& operator=(const RawPluginSchedulerGuard&) = delete;

  /**
   * @brief Transfers instance destruction to a completed owner.
   *
   * @return The guarded raw pointer.
   * @throws Nothing.
   * @note Call only after `PluginSchedulerOwner` construction succeeds.
   */
  IScheduler* release() noexcept {
    IScheduler* released = scheduler_;
    scheduler_ = nullptr;
    return released;
  }

 private:
  /** @brief Plugin instance destroyed unless ownership is released. */
  IScheduler* scheduler_ = nullptr;
  /** @brief Plugin ABI destroy function paired with `scheduler_`. */
  void (*destroy_)(IScheduler*) = nullptr;
  /** @brief Library lifetime retained through instance destruction. */
  std::shared_ptr<void> library_;
};

}  // namespace

SchedulerPluginLoader& SchedulerPluginLoader::instance() {
  static SchedulerPluginLoader instance;
  return instance;
}

SchedulerPluginLoader::~SchedulerPluginLoader() {
  loaded_plugins_.clear();
}

/** @copydoc SchedulerPluginLoader::RegistryState::RegistryState */
SchedulerPluginLoader::RegistryState::RegistryState(
    const SchedulerPluginLoader& loader)
    : loaded_plugins(  // NOLINT(whitespace/indent_namespace)
          loader.loaded_plugins_),
      type_to_plugin(  // NOLINT(whitespace/indent_namespace)
          loader.type_to_plugin_),
      type_info(  // NOLINT(whitespace/indent_namespace)
          loader.type_info_),
      load_errors(  // NOLINT(whitespace/indent_namespace)
          loader.load_errors_) {}

/** @copydoc SchedulerPluginLoader::RegistryState::commit */
void SchedulerPluginLoader::RegistryState::commit(
    SchedulerPluginLoader& loader) noexcept {
  loader.loaded_plugins_.swap(loaded_plugins);
  loader.type_to_plugin_.swap(type_to_plugin);
  loader.type_info_.swap(type_info);
  loader.load_errors_.swap(load_errors);
}

/** @copydoc SchedulerPluginLoader::append_load_error_unlocked */
void SchedulerPluginLoader::append_load_error_unlocked(std::string error) {
  std::vector<std::string> staged_errors = load_errors_;
  staged_errors.push_back(std::move(error));
  load_errors_.swap(staged_errors);
}

size_t SchedulerPluginLoader::scan_and_load(const std::string& dir_path) {
  return scan_and_load(std::vector<std::string>{dir_path});
}

size_t SchedulerPluginLoader::scan_and_load(
    const std::vector<std::string>& dir_paths) {
  std::lock_guard<std::mutex> lock(mutex_);

  size_t loaded_count = 0;

  for (const auto& dir_path : dir_paths) {
    bool recursive = false;
    std::string actual_path = dir_path;

    // 检查是否递归扫描
    if (dir_path.size() >= 3 && dir_path.substr(dir_path.size() - 3) == "/**") {
      recursive = true;
      actual_path = dir_path.substr(0, dir_path.size() - 3);
    } else if (dir_path.size() >= 2 &&
               dir_path.substr(dir_path.size() - 2) == "/*") {
      actual_path = dir_path.substr(0, dir_path.size() - 2);
    }

    fs::path base_dir(actual_path);
    if (!fs::exists(base_dir) || !fs::is_directory(base_dir)) {
      continue;
    }

    auto process_entry = [&](const fs::directory_entry& entry) {
      if (!entry.is_regular_file())
        return;

      const auto& path = entry.path();
#if defined(_WIN32)
      const std::string extension = ".dll";
#elif defined(__APPLE__)
      const std::string extension = ".dylib";
#else
      const std::string extension = ".so";
#endif

      if (path.extension() != extension)
        return;

      // 跳过已加载的插件
      std::string abs_path = fs::absolute(path).string();
      if (loaded_plugins_.count(abs_path) > 0)
        return;

      if (load_plugin_internal_unlocked(path)) {
        ++loaded_count;
      }
    };

    if (recursive) {
      for (const auto& entry : fs::recursive_directory_iterator(base_dir)) {
        process_entry(entry);
      }
    } else {
      for (const auto& entry : fs::directory_iterator(base_dir)) {
        process_entry(entry);
      }
    }
  }

  return loaded_count;
}

/** @copydoc SchedulerPluginLoader::load_plugin */
bool SchedulerPluginLoader::load_plugin(const fs::path& plugin_path) {
  std::lock_guard<std::mutex> lock(mutex_);
  return load_plugin_internal_unlocked(plugin_path);
}

/** @copydoc SchedulerPluginLoader::load_plugin_internal_unlocked */
bool SchedulerPluginLoader::load_plugin_internal_unlocked(
    const fs::path& plugin_path) {
  std::string abs_path = fs::absolute(plugin_path).string();

  // 检查是否已加载
  if (loaded_plugins_.count(abs_path) > 0) {
    return true;
  }

  PluginHandle handle;
  handle.path = abs_path;

#ifdef _WIN32
  handle.handle = LoadLibrary(abs_path.c_str());
  if (!handle.handle) {
    append_load_error_unlocked("Failed to load plugin: " + abs_path +
                               " (LoadLibrary failed)");
    return false;
  }
  handle.library = make_library_lifetime(handle.handle);

  handle.get_count =
      reinterpret_cast<decltype(handle.get_count)>(GetProcAddress(
          static_cast<HMODULE>(handle.handle), PS_SCHEDULER_PLUGIN_GET_COUNT));
  handle.get_name = reinterpret_cast<decltype(handle.get_name)>(GetProcAddress(
      static_cast<HMODULE>(handle.handle), PS_SCHEDULER_PLUGIN_GET_NAME));
  handle.get_description = reinterpret_cast<decltype(handle.get_description)>(
      GetProcAddress(static_cast<HMODULE>(handle.handle),
                     PS_SCHEDULER_PLUGIN_GET_DESCRIPTION));
  handle.create = reinterpret_cast<decltype(handle.create)>(GetProcAddress(
      static_cast<HMODULE>(handle.handle), PS_SCHEDULER_PLUGIN_CREATE));
  handle.destroy = reinterpret_cast<decltype(handle.destroy)>(GetProcAddress(
      static_cast<HMODULE>(handle.handle), PS_SCHEDULER_PLUGIN_DESTROY));
  handle.get_version = reinterpret_cast<decltype(handle.get_version)>(
      GetProcAddress(static_cast<HMODULE>(handle.handle),
                     PS_SCHEDULER_PLUGIN_GET_VERSION));
#else
  handle.handle = dlopen(abs_path.c_str(), RTLD_LAZY);
  if (!handle.handle) {
    const char* err = dlerror();
    append_load_error_unlocked("Failed to load plugin: " + abs_path + " (" +
                               (err ? err : "unknown error") + ")");
    return false;
  }
  handle.library = make_library_lifetime(handle.handle);

  // 清除之前的错误
  dlerror();

  handle.get_count = reinterpret_cast<decltype(handle.get_count)>(
      dlsym(handle.handle, PS_SCHEDULER_PLUGIN_GET_COUNT));
  handle.get_name = reinterpret_cast<decltype(handle.get_name)>(
      dlsym(handle.handle, PS_SCHEDULER_PLUGIN_GET_NAME));
  handle.get_description = reinterpret_cast<decltype(handle.get_description)>(
      dlsym(handle.handle, PS_SCHEDULER_PLUGIN_GET_DESCRIPTION));
  handle.create = reinterpret_cast<decltype(handle.create)>(
      dlsym(handle.handle, PS_SCHEDULER_PLUGIN_CREATE));
  handle.destroy = reinterpret_cast<decltype(handle.destroy)>(
      dlsym(handle.handle, PS_SCHEDULER_PLUGIN_DESTROY));
  handle.get_version = reinterpret_cast<decltype(handle.get_version)>(
      dlsym(handle.handle, PS_SCHEDULER_PLUGIN_GET_VERSION));
#endif

  // 验证必要的函数
  if (!handle.get_count || !handle.get_name || !handle.create ||
      !handle.destroy) {
    RegistryState staged(*this);
    staged.load_errors.push_back(
        "Plugin missing required exports: " + abs_path +
        " (need " PS_SCHEDULER_PLUGIN_GET_COUNT
        ", " PS_SCHEDULER_PLUGIN_GET_NAME ", " PS_SCHEDULER_PLUGIN_CREATE
        ", " PS_SCHEDULER_PLUGIN_DESTROY ")");
    staged.commit(*this);
    return false;
  }

  // 获取版本
  std::string version = "unknown";
  if (handle.get_version) {
    const char* v = handle.get_version();
    if (v)
      version = v;
  }

  RegistryState staged(*this);

  // 注册所有调度器类型
  int count = handle.get_count();
  for (int i = 0; i < count; ++i) {
    const char* name = handle.get_name(i);
    if (!name)
      continue;

    std::string type_name = name;

    // 检查是否与内置或其他插件冲突
    if (builtins_.count(type_name) > 0) {
      staged.load_errors.push_back(
          "Scheduler type '" + type_name +
          "' conflicts with builtin in plugin: " + abs_path);
      continue;
    }
    const auto existing_type = staged.type_to_plugin.find(type_name);
    if (existing_type != staged.type_to_plugin.end()) {
      staged.load_errors.push_back(
          "Scheduler type '" + type_name + "' already registered by plugin: " +
          existing_type->second + " (in " + abs_path + ")");
      continue;
    }

    // 注册类型
    staged.type_to_plugin[type_name] = abs_path;
    handle.registered_types.push_back(type_name);

    // 保存信息
    SchedulerPluginInfo info;
    info.type_name = type_name;
    info.plugin_path = abs_path;
    info.version = version;
    info.is_builtin = false;

    if (handle.get_description) {
      const char* desc = handle.get_description(i);
      if (desc)
        info.description = desc;
    }

    staged.type_info[type_name] = std::move(info);
  }

  // 保存句柄
  staged.loaded_plugins[abs_path] = std::move(handle);
  staged.commit(*this);

  return true;
}

bool SchedulerPluginLoader::unload_plugin(const fs::path& plugin_path) {
  std::lock_guard<std::mutex> lock(mutex_);

  std::string abs_path = fs::absolute(plugin_path).string();
  auto it = loaded_plugins_.find(abs_path);
  if (it == loaded_plugins_.end()) {
    return false;
  }

  // 移除该插件注册的所有类型
  for (const auto& type_name : it->second.registered_types) {
    type_to_plugin_.erase(type_name);
    type_info_.erase(type_name);
  }

  loaded_plugins_.erase(it);
  return true;
}

std::vector<std::string> SchedulerPluginLoader::get_registered_types() const {
  std::lock_guard<std::mutex> lock(mutex_);

  std::vector<std::string> types;

  // 内置类型
  for (const auto& [name, _] : builtins_) {
    types.push_back(name);
  }

  // 插件类型
  for (const auto& [name, _] : type_to_plugin_) {
    types.push_back(name);
  }

  std::sort(types.begin(), types.end());
  return types;
}

bool SchedulerPluginLoader::is_registered(const std::string& type_name) const {
  std::lock_guard<std::mutex> lock(mutex_);
  return builtins_.count(type_name) > 0 || type_to_plugin_.count(type_name) > 0;
}

std::optional<SchedulerPluginInfo> SchedulerPluginLoader::get_info(
    const std::string& type_name) const {
  std::lock_guard<std::mutex> lock(mutex_);

  // 检查内置
  auto builtin_it = builtins_.find(type_name);
  if (builtin_it != builtins_.end()) {
    SchedulerPluginInfo info;
    info.type_name = type_name;
    info.description = builtin_it->second.description;
    info.plugin_path = "(builtin)";
    info.version = "builtin";
    info.is_builtin = true;
    return info;
  }

  // 检查插件
  auto it = type_info_.find(type_name);
  if (it != type_info_.end()) {
    return it->second;
  }

  return std::nullopt;
}

std::vector<SchedulerPluginInfo> SchedulerPluginLoader::get_all_info() const {
  std::lock_guard<std::mutex> lock(mutex_);

  std::vector<SchedulerPluginInfo> result;

  // 内置类型
  for (const auto& [name, builtin] : builtins_) {
    SchedulerPluginInfo info;
    info.type_name = name;
    info.description = builtin.description;
    info.plugin_path = "(builtin)";
    info.version = "builtin";
    info.is_builtin = true;
    result.push_back(info);
  }

  // 插件类型
  for (const auto& [_, info] : type_info_) {
    result.push_back(info);
  }

  return result;
}

std::string SchedulerPluginLoader::get_description(
    const std::string& type_name) const {
  auto info = get_info(type_name);
  return info ? info->description : "Unknown scheduler type";
}

/** @copydoc SchedulerPluginLoader::create */
std::unique_ptr<IScheduler> SchedulerPluginLoader::create(
    const std::string& type_name, unsigned int num_workers) {
  std::lock_guard<std::mutex> lock(mutex_);

  // 检查内置
  auto builtin_it = builtins_.find(type_name);
  if (builtin_it != builtins_.end()) {
    return builtin_it->second.factory(num_workers);
  }

  // 检查插件
  auto plugin_it = type_to_plugin_.find(type_name);
  if (plugin_it == type_to_plugin_.end()) {
    return nullptr;
  }

  auto handle_it = loaded_plugins_.find(plugin_it->second);
  if (handle_it == loaded_plugins_.end()) {
    return nullptr;
  }

  const auto& handle = handle_it->second;
  if (!handle.create) {
    return nullptr;
  }

  // 创建调度器
  IScheduler* raw_ptr = handle.create(type_name.c_str(), num_workers);
  if (!raw_ptr) {
    return nullptr;
  }
  RawPluginSchedulerGuard raw_owner(raw_ptr, handle.destroy, handle.library);
  if (!dynamic_cast<SchedulerTaskRuntime*>(raw_ptr)) {
    return nullptr;
  }

  auto owner = std::make_unique<PluginSchedulerOwner>(
      raw_ptr, handle.destroy, handle.library, type_name);
  (void)raw_owner.release();
  return owner;
}

void SchedulerPluginLoader::register_builtin(
    const std::string& type_name, const std::string& description,
    std::function<std::unique_ptr<IScheduler>(unsigned int)> factory) {
  std::lock_guard<std::mutex> lock(mutex_);

  BuiltinScheduler builtin;
  builtin.description = description;
  builtin.factory = std::move(factory);
  builtins_[type_name] = std::move(builtin);
}

void SchedulerPluginLoader::clear_plugins() {
  std::lock_guard<std::mutex> lock(mutex_);

  loaded_plugins_.clear();
  type_to_plugin_.clear();
  type_info_.clear();
  // 保留内置
}

std::vector<std::string> SchedulerPluginLoader::list_loaded_plugins() const {
  std::lock_guard<std::mutex> lock(mutex_);
  std::vector<std::string> result;

  for (const auto& [path, handle] : loaded_plugins_) {
    std::string info = path;
    if (!handle.registered_types.empty()) {
      info += " (";
      for (size_t i = 0; i < handle.registered_types.size(); ++i) {
        if (i > 0)
          info += ", ";
        info += handle.registered_types[i];
      }
      info += ")";
    }
    if (handle.get_version) {
      const char* ver = handle.get_version();
      if (ver) {
        info += " v";
        info += ver;
      }
    }
    result.push_back(info);
  }

  return result;
}

const std::vector<std::string>& SchedulerPluginLoader::get_load_errors() const {
  return load_errors_;
}

void SchedulerPluginLoader::clear_errors() {
  std::lock_guard<std::mutex> lock(mutex_);
  load_errors_.clear();
}

}  // namespace ps
