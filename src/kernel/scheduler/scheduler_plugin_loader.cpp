// Photospider kernel: Scheduler Plugin Loader implementation
#include "kernel/scheduler/scheduler_plugin_loader.hpp"

#include <algorithm>
#include <iostream>
#include <sstream>

#include "kernel/scheduler/scheduler_plugin_api.hpp"

#ifdef _WIN32
#include <windows.h>
#else
#include <dlfcn.h>
#endif

namespace ps {

SchedulerPluginLoader& SchedulerPluginLoader::instance() {
  static SchedulerPluginLoader instance;
  return instance;
}

SchedulerPluginLoader::~SchedulerPluginLoader() {
  // 卸载所有插件
  for (auto& [path, handle] : loaded_plugins_) {
    if (handle.handle) {
#ifdef _WIN32
      FreeLibrary(static_cast<HMODULE>(handle.handle));
#else
      dlclose(handle.handle);
#endif
    }
  }
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
    if (dir_path.size() >= 3 &&
        dir_path.substr(dir_path.size() - 3) == "/**") {
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
      if (!entry.is_regular_file()) return;
      
      const auto& path = entry.path();
#if defined(_WIN32)
      const std::string extension = ".dll";
#elif defined(__APPLE__)
      const std::string extension = ".dylib";
#else
      const std::string extension = ".so";
#endif
      
      if (path.extension() != extension) return;
      
      // 跳过已加载的插件
      std::string abs_path = fs::absolute(path).string();
      if (loaded_plugins_.count(abs_path) > 0) return;
      
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

bool SchedulerPluginLoader::load_plugin(const fs::path& plugin_path) {
  std::lock_guard<std::mutex> lock(mutex_);
  return load_plugin_internal_unlocked(plugin_path);
}

bool SchedulerPluginLoader::load_plugin_internal_unlocked(const fs::path& plugin_path) {
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
    load_errors_.push_back("Failed to load plugin: " + abs_path +
                           " (LoadLibrary failed)");
    return false;
  }
  
  handle.get_count = reinterpret_cast<decltype(handle.get_count)>(
      GetProcAddress(static_cast<HMODULE>(handle.handle),
                     PS_SCHEDULER_PLUGIN_GET_COUNT));
  handle.get_name = reinterpret_cast<decltype(handle.get_name)>(
      GetProcAddress(static_cast<HMODULE>(handle.handle),
                     PS_SCHEDULER_PLUGIN_GET_NAME));
  handle.get_description = reinterpret_cast<decltype(handle.get_description)>(
      GetProcAddress(static_cast<HMODULE>(handle.handle),
                     PS_SCHEDULER_PLUGIN_GET_DESCRIPTION));
  handle.create = reinterpret_cast<decltype(handle.create)>(
      GetProcAddress(static_cast<HMODULE>(handle.handle),
                     PS_SCHEDULER_PLUGIN_CREATE));
  handle.destroy = reinterpret_cast<decltype(handle.destroy)>(
      GetProcAddress(static_cast<HMODULE>(handle.handle),
                     PS_SCHEDULER_PLUGIN_DESTROY));
  handle.get_version = reinterpret_cast<decltype(handle.get_version)>(
      GetProcAddress(static_cast<HMODULE>(handle.handle),
                     PS_SCHEDULER_PLUGIN_GET_VERSION));
#else
  handle.handle = dlopen(abs_path.c_str(), RTLD_LAZY);
  if (!handle.handle) {
    const char* err = dlerror();
    load_errors_.push_back("Failed to load plugin: " + abs_path + " (" +
                           (err ? err : "unknown error") + ")");
    return false;
  }
  
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
  if (!handle.get_count || !handle.get_name || !handle.create) {
    load_errors_.push_back(
        "Plugin missing required exports: " + abs_path +
        " (need " PS_SCHEDULER_PLUGIN_GET_COUNT ", " PS_SCHEDULER_PLUGIN_GET_NAME
        ", " PS_SCHEDULER_PLUGIN_CREATE ")");
#ifdef _WIN32
    FreeLibrary(static_cast<HMODULE>(handle.handle));
#else
    dlclose(handle.handle);
#endif
    return false;
  }
  
  // 获取版本
  std::string version = "unknown";
  if (handle.get_version) {
    const char* v = handle.get_version();
    if (v) version = v;
  }
  
  // 注册所有调度器类型
  int count = handle.get_count();
  for (int i = 0; i < count; ++i) {
    const char* name = handle.get_name(i);
    if (!name) continue;
    
    std::string type_name = name;
    
    // 检查是否与内置或其他插件冲突
    if (builtins_.count(type_name) > 0) {
      load_errors_.push_back("Scheduler type '" + type_name +
                             "' conflicts with builtin in plugin: " + abs_path);
      continue;
    }
    if (type_to_plugin_.count(type_name) > 0) {
      load_errors_.push_back(
          "Scheduler type '" + type_name + "' already registered by plugin: " +
          type_to_plugin_[type_name] + " (in " + abs_path + ")");
      continue;
    }
    
    // 注册类型
    type_to_plugin_[type_name] = abs_path;
    handle.registered_types.push_back(type_name);
    
    // 保存信息
    SchedulerPluginInfo info;
    info.type_name = type_name;
    info.plugin_path = abs_path;
    info.version = version;
    info.is_builtin = false;
    
    if (handle.get_description) {
      const char* desc = handle.get_description(i);
      if (desc) info.description = desc;
    }
    
    type_info_[type_name] = info;
  }
  
  // 保存句柄
  loaded_plugins_[abs_path] = std::move(handle);
  
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
  
  // 关闭动态库
  if (it->second.handle) {
#ifdef _WIN32
    FreeLibrary(static_cast<HMODULE>(it->second.handle));
#else
    dlclose(it->second.handle);
#endif
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
  
  // 注意：插件创建的调度器需要通过插件的 destroy 函数销毁
  // 但由于插件的生命周期由 SchedulerPluginLoader 管理，
  // 且 IScheduler 有虚析构函数，这里直接使用 default_delete
  // 确保 IScheduler 的析构函数正确实现释放资源
  return std::unique_ptr<IScheduler>(raw_ptr);
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
  
  // 卸载所有插件
  for (auto& [path, handle] : loaded_plugins_) {
    if (handle.handle) {
#ifdef _WIN32
      FreeLibrary(static_cast<HMODULE>(handle.handle));
#else
      dlclose(handle.handle);
#endif
    }
  }
  
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
        if (i > 0) info += ", ";
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
