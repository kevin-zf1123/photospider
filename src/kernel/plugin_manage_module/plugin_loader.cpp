#include "plugin_loader.hpp"  // NOLINT(build/include_subdir)

#include <algorithm>
#include <exception>
#include <iterator>
#include <map>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "ps_types.hpp"  // NOLINT(build/include_subdir)

#ifdef _WIN32
#include <windows.h>
#else
#include <dlfcn.h>
#endif

namespace ps {
namespace {

using RegisterOpsFunc = void (*)();

/**
 * @brief Returns the platform shared-library suffix for operation plugins.
 *
 * The loader scans regular files only and compares their extension against this
 * value before attempting to open them. This keeps scan behavior consistent
 * with the historical loader while isolating platform branching in one place.
 *
 * @return `.dll` on Windows, `.dylib` on macOS, and `.so` on other Unix-like
 * platforms.
 * @throws Nothing.
 * @note The returned string is a static literal and has process lifetime.
 */
const char* platform_plugin_extension() noexcept {
#if defined(_WIN32)
  return ".dll";
#elif defined(__APPLE__)
  return ".dylib";
#else
  return ".so";
#endif
}

/**
 * @brief Creates a shared lifetime wrapper around an opened dynamic library.
 *
 * The returned `shared_ptr` does not own an object allocation; it owns the
 * platform handle through a custom deleter. Copying the pointer keeps the
 * library mapped until the final holder releases it.
 *
 * @param handle Native `LoadLibrary` or `dlopen` handle. A null handle produces
 * an empty lifetime object with a no-op deleter path.
 * @return Shared lifetime object that closes the library at final release.
 * @throws std::bad_alloc if the shared control block cannot be allocated.
 * @note Registered operation callbacks must be removed from `OpRegistry` before
 * this lifetime object is destroyed.
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
 * @brief RAII wrapper for one operation plugin dynamic library.
 *
 * The wrapper owns the native handle through `library_` and exposes typed
 * symbol lookup. Moving or copying the wrapper itself is intentionally
 * disabled; users pass around `shared_ptr<void>` lifetimes instead.
 *
 * @note This helper is local to the operation plugin loader because the
 * scheduler plugin loader has additional instance-destruction requirements.
 */
class DynamicLibrary final {
 public:
  /**
   * @brief Opens a dynamic library and reports platform loader failures.
   *
   * @param path Absolute or relative path to a candidate shared library.
   * @param error_message Output error text when opening fails.
   * @return A library wrapper on success, or `nullptr` on failure.
   * @throws std::bad_alloc if allocation of the wrapper or handle owner fails.
   * @note The caller is responsible for adding `path` to structured load
   * results; this function only returns the platform error detail.
   */
  static std::unique_ptr<DynamicLibrary> open(const fs::path& path,
                                              std::string& error_message) {
#ifdef _WIN32
    HMODULE handle = LoadLibrary(path.string().c_str());
    if (!handle) {
      error_message = "LoadLibrary failed";
      return nullptr;
    }
    return std::unique_ptr<DynamicLibrary>(
        new DynamicLibrary(static_cast<void*>(handle)));
#else
    void* handle = dlopen(path.c_str(), RTLD_LAZY);
    if (!handle) {
      const char* err = dlerror();
      error_message = err ? err : "dlopen failed";
      return nullptr;
    }
    return std::unique_ptr<DynamicLibrary>(new DynamicLibrary(handle));
#endif
  }

  /**
   * @brief Resolves a required C symbol as a typed function pointer.
   *
   * @tparam Function Function pointer type expected by the caller.
   * @param symbol_name Exported symbol name to locate.
   * @param error_message Output error detail when the symbol is missing.
   * @return Typed function pointer on success, or `nullptr` on failure.
   * @throws Nothing directly; platform symbol lookup errors are returned in
   * `error_message`.
   * @note POSIX `dlsym` returns `void*`; the cast is isolated here so the call
   * sites stay simple and auditable.
   */
  template <typename Function>
  Function resolve(const char* symbol_name, std::string& error_message) const {
#ifdef _WIN32
    auto* symbol = GetProcAddress(static_cast<HMODULE>(handle_), symbol_name);
    if (!symbol) {
      error_message = std::string("Missing ") + symbol_name;
      return nullptr;
    }
    return reinterpret_cast<Function>(symbol);
#else
    dlerror();
    void* symbol = dlsym(handle_, symbol_name);
    const char* err = dlerror();
    if (err) {
      error_message = err;
      return nullptr;
    }
    return reinterpret_cast<Function>(symbol);
#endif
  }

  /**
   * @brief Returns the shared handle lifetime retained by plugin owners.
   *
   * @return Shared native handle owner.
   * @throws Nothing.
   * @note Holding this object keeps the library loaded even after the local
   * `DynamicLibrary` wrapper goes out of scope.
   */
  std::shared_ptr<void> lifetime() const noexcept { return library_; }

 private:
  /**
   * @brief Takes ownership of an already-opened native library handle.
   *
   * @param handle Native platform handle returned by `LoadLibrary` or `dlopen`.
   * @throws std::bad_alloc if the shared lifetime control block cannot be
   * allocated.
   * @note Construction is private so callers cannot create wrappers around
   * invalid handles.
   */
  explicit DynamicLibrary(void* handle)
      : handle_(handle), library_(make_library_lifetime(handle)) {}

  void* handle_ = nullptr;
  std::shared_ptr<void> library_;
};

/**
 * @brief Parsed plugin directory scan request.
 *
 * `base_dir` is the directory to iterate and `recursive` records whether the
 * user requested the recursive double-star suffix. Plain paths and the
 * explicit single-star suffix both scan one directory level.
 */
struct ScanPattern {
  fs::path base_dir;
  bool recursive = false;
};

/**
 * @brief Converts one raw directory pattern into a scan directory and mode.
 *
 * @param raw_path User or config supplied path pattern.
 * @return Parsed base directory plus recursion flag.
 * @throws std::bad_alloc from string/path allocation.
 * @note Empty raw paths are filtered by the caller before parsing.
 */
ScanPattern parse_scan_pattern(const std::string& raw_path) {
  ScanPattern pattern;
  std::string path = raw_path;
  if (path.size() >= 3 && path.substr(path.size() - 3) == "/**") {
    pattern.recursive = true;
    path = path.substr(0, path.size() - 3);
  } else if (path.size() >= 2 && path.substr(path.size() - 2) == "/*") {
    path = path.substr(0, path.size() - 2);
  }
  pattern.base_dir = fs::path(path);
  return pattern;
}

/**
 * @brief Checks whether a regular file path has the platform plugin suffix.
 *
 * @param path Candidate filesystem path.
 * @return True when `path.extension()` matches the platform shared-library
 * extension.
 * @throws Nothing.
 * @note The caller is responsible for checking that the path is a regular file.
 */
bool is_operation_plugin_path(const fs::path& path) {
  return path.extension() == platform_plugin_extension();
}

/**
 * @brief Visits every platform plugin candidate in one directory scan pattern.
 *
 * @tparam Callback Callable accepting `const fs::path&`.
 * @param pattern Parsed directory and recursion mode.
 * @param callback Invoked for each regular file with the platform plugin
 * extension.
 * @throws std::filesystem_error when directory iteration fails.
 * @note Non-existent and non-directory paths are ignored to preserve legacy
 * plugin-dir behavior.
 */
template <typename Callback>
void visit_plugin_candidates(const ScanPattern& pattern, Callback&& callback) {
  if (!fs::exists(pattern.base_dir) || !fs::is_directory(pattern.base_dir)) {
    return;
  }

  auto visit_entry = [&](const fs::directory_entry& entry) {
    if (entry.is_regular_file() && is_operation_plugin_path(entry.path())) {
      callback(entry.path());
    }
  };

  if (pattern.recursive) {
    for (const auto& entry :
         fs::recursive_directory_iterator(pattern.base_dir)) {
      visit_entry(entry);
    }
    return;
  }

  for (const auto& entry : fs::directory_iterator(pattern.base_dir)) {
    visit_entry(entry);
  }
}

/**
 * @brief Computes newly registered operation keys after plugin registration.
 *
 * @param keys_before Sorted registry keys captured before registration.
 * @param keys_after Sorted registry keys captured after registration.
 * @return Keys present in `keys_after` but absent from `keys_before`.
 * @throws std::bad_alloc if result allocation fails.
 * @note `OpRegistry::get_keys()` returns sorted unique keys, so set difference
 * can be applied directly.
 */
std::vector<std::string> collect_new_keys(
    const std::vector<std::string>& keys_before,
    const std::vector<std::string>& keys_after) {
  std::vector<std::string> new_keys;
  std::set_difference(keys_after.begin(), keys_after.end(), keys_before.begin(),
                      keys_before.end(), std::back_inserter(new_keys));
  return new_keys;
}

/**
 * @brief Removes operation keys from the global registry after a failed load.
 *
 * @param keys Operation keys to unregister.
 * @return Count of registry entries that were removed.
 * @throws Nothing from the loop body under current `OpRegistry` behavior.
 * @note Rollback prevents callbacks from a closing plugin library from being
 * left in `OpRegistry` when `register_photospider_ops` throws.
 */
int unregister_keys(const std::vector<std::string>& keys) {
  auto& registry = OpRegistry::instance();
  int removed = 0;
  for (const auto& key : keys) {
    removed += registry.unregister_key(key) ? 1 : 0;
  }
  return removed;
}

/**
 * @brief Appends newly registered operation sources to the result and map.
 *
 * @param plugin_path Absolute plugin path used as the operation source.
 * @param new_keys Operation keys discovered during registration.
 * @param op_sources Destination `op key -> plugin path` map.
 * @param result Load result receiving `new_op_keys`.
 * @throws std::bad_alloc from map or vector growth.
 * @note Existing source entries are overwritten only for keys that appeared as
 * new during this load, matching the legacy loader behavior.
 */
void record_new_operation_sources(
    const std::string& plugin_path, const std::vector<std::string>& new_keys,
    std::map<std::string, std::string>& op_sources, PluginLoadResult& result) {
  for (const auto& key : new_keys) {
    op_sources[key] = plugin_path;
    result.new_op_keys.push_back(key);
  }
}

/**
 * @brief Loads one operation plugin file and retains its dynamic library.
 *
 * @param path Candidate shared library path.
 * @param op_sources Operation source map to update with newly registered keys.
 * @param loaded_plugins Caller-owned handle map keyed by absolute plugin path.
 * @param result Accumulated load result.
 * @throws std::filesystem_error from `fs::absolute`.
 * @throws std::bad_alloc from result, map, vector, or library owner allocation.
 * @note The handle is stored only after `register_photospider_ops` completes.
 * Failure paths close the library and roll back keys registered before an
 * exception.
 */
void load_one_plugin(const fs::path& path,
                     std::map<std::string, std::string>& op_sources,
                     LoadedOpPluginMap& loaded_plugins,
                     PluginLoadResult& result) {
  const std::string absolute_path = fs::absolute(path).string();
  if (loaded_plugins.count(absolute_path) > 0) {
    return;
  }

  ++result.attempted;

  std::string error_message;
  auto library = DynamicLibrary::open(path, error_message);
  if (!library) {
    result.errors.push_back(
        {absolute_path, GraphErrc::Io, std::move(error_message)});
    return;
  }

  auto register_ops = library->resolve<RegisterOpsFunc>(
      "register_photospider_ops", error_message);
  if (!register_ops) {
    result.errors.push_back(
        {absolute_path, GraphErrc::InvalidParameter, error_message});
    return;
  }

  auto& registry = OpRegistry::instance();
  const auto keys_before = registry.get_keys();
  try {
    register_ops();
  } catch (const std::exception& e) {
    const auto new_keys = collect_new_keys(keys_before, registry.get_keys());
    unregister_keys(new_keys);
    result.errors.push_back({absolute_path, GraphErrc::Unknown, e.what()});
    return;
  } catch (...) {
    const auto new_keys = collect_new_keys(keys_before, registry.get_keys());
    unregister_keys(new_keys);
    result.errors.push_back(
        {absolute_path, GraphErrc::Unknown, "register_photospider_ops threw"});
    return;
  }

  const auto new_keys = collect_new_keys(keys_before, registry.get_keys());
  record_new_operation_sources(absolute_path, new_keys, op_sources, result);

  LoadedOpPlugin loaded;
  loaded.library = library->lifetime();
  loaded.registered_keys = new_keys;
  loaded_plugins[absolute_path] = std::move(loaded);
  ++result.loaded;
}

/**
 * @brief Process-lifetime handle store for legacy `load_plugins` callers.
 *
 * @return Mutable map retaining successful plugin handles until process exit.
 * @throws Nothing after static initialization succeeds.
 * @note `PluginManager` does not use this store; it passes its own map so
 * unload operations can release handles explicitly.
 */
LoadedOpPluginMap& process_resident_op_plugins() {
  static LoadedOpPluginMap loaded_plugins;
  return loaded_plugins;
}

}  // namespace

PluginLoadResult load_plugins(const std::vector<std::string>& plugin_dir_paths,
                              std::map<std::string, std::string>& op_sources) {
  auto& loaded_plugins = process_resident_op_plugins();
  return load_plugins_retaining_handles(plugin_dir_paths, op_sources,
                                        loaded_plugins);
}

PluginLoadResult load_plugins_retaining_handles(
    const std::vector<std::string>& plugin_dir_paths,
    std::map<std::string, std::string>& op_sources,
    LoadedOpPluginMap& loaded_plugins) {
  PluginLoadResult result;
  for (const auto& raw_path : plugin_dir_paths) {
    if (raw_path.empty()) {
      continue;
    }
    const ScanPattern pattern = parse_scan_pattern(raw_path);
    visit_plugin_candidates(pattern, [&](const fs::path& path) {
      load_one_plugin(path, op_sources, loaded_plugins, result);
    });
  }
  return result;
}

}  // namespace ps
