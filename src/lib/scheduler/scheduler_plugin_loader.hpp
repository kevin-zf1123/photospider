// Photospider kernel: scheduler plugin discovery and ownership boundary.
#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

#include "photospider/scheduler/scheduler_plugin_api.hpp"

namespace ps {

/** @brief Filesystem namespace used by scheduler plugin path APIs. */
namespace fs = std::filesystem;

/**
 * @brief Host-owned metadata snapshot for one registered scheduler type.
 *
 * All strings are copied while the loader mutex and plugin library lifetime
 * are held. Returned snapshots therefore remain valid after registry changes
 * and never expose plugin-owned character storage.
 *
 * @note Built-in entries use the synthetic path and version `"(builtin)"`
 *       and `"builtin"`; plugin entries retain their absolute DSO path.
 */
struct SchedulerPluginInfo {
  /** @brief Stable scheduler type used by `SchedulerPluginLoader::create`. */
  std::string type_name;
  /** @brief Human-readable scheduler description copied during discovery. */
  std::string description;
  /** @brief Absolute plugin path, or `"(builtin)"` for host implementations. */
  std::string plugin_path;
  /** @brief Cached human-readable plugin version or built-in marker. */
  std::string version;
  /** @brief True when creation uses a host factory instead of a DSO export. */
  bool is_builtin = false;
};

/**
 * @brief Thread-safe registry and lifetime owner for scheduler plugins.
 *
 * Discovery validates the numeric ABI handshake before resolving or invoking
 * any metadata/create/destroy export. Complete plugin metadata is staged in a
 * shadow registry and published atomically. Created scheduler owners retain a
 * shared DSO lifetime through their matching destroy export. Plugin-origin
 * exceptions are normalized while that lease is live; host task exceptions
 * use preallocated owner-side `exception_ptr` identity slots and preserve their
 * exact type, identity, and plugin-visible behavior across first-exception
 * storage.
 *
 * @note Registry methods serialize through `mutex_`. Plugin callbacks execute
 *       while that mutex is held, so plugins must not re-enter this loader.
 */
class SchedulerPluginLoader {
 public:
  /**
   * @brief Returns the process-wide scheduler plugin registry.
   * @return Loader singleton reference valid until process teardown.
   * @throws std::system_error only if static initialization synchronization
   * fails.
   * @note The singleton is non-copyable and owns loaded-library registrations.
   */
  static SchedulerPluginLoader& instance();

  /**
   * @brief Scans configured directories and loads candidate scheduler DSOs.
   * @param dir_paths Directory expressions; trailing slash-plus-two-stars
   * requests recursive scanning and slash-plus-one-star requests one level.
   * @return Number of newly committed plugin libraries.
   * @throws std::filesystem::filesystem_error for filesystem iteration errors.
   * @throws std::bad_alloc for host allocation or as a fresh host-owned copy of
   * plugin resource exhaustion.
   * @throws GraphError after normalizing any other plugin discovery exception.
   * @note Already-loaded absolute paths are skipped and do not increment the
   * result. The numeric handshake type is `noexcept`; a plugin that violates
   * that ABI contract terminates rather than propagating an exception.
   */
  std::size_t scan_and_load(const std::vector<std::string>& dir_paths);

  /**
   * @brief Scans one directory expression for scheduler DSOs.
   * @param dir_path Directory expression accepted by the vector overload.
   * @return Number of newly committed plugin libraries.
   * @throws The same exceptions as the vector overload.
   * @note This convenience overload performs one vector allocation.
   */
  std::size_t scan_and_load(const std::string& dir_path);

  /**
   * @brief Loads one scheduler plugin as a strong registry transaction.
   *
   * The candidate library, discovered exports, type mappings, metadata,
   * retained handle, and diagnostics are staged while `mutex_` excludes other
   * registry operations. Successful staging is published with no-throw swaps.
   *
   * @param plugin_path Plugin library path normalized to an absolute key.
   * @return True when the plugin was already loaded or its complete staged
   * state committed; false for a recoverable open/ABI/export/type-identity
   * failure or when no non-conflicting type remains.
   * @throws std::bad_alloc when host staging cannot allocate or a plugin
   * discovery callback reports resource exhaustion.
   * @throws GraphError after normalizing any other plugin discovery exception
   * while the candidate lease remains alive.
   * @note Every exceptional exit preserves all live registries and diagnostic
   * prefixes. A compatible candidate must report at least one type, return a
   * non-null non-empty name at every in-range index, and retain at least one
   * non-conflicting type. Missing/mismatched handshakes invoke no later plugin
   * export. The handshake itself is `noexcept`; violating plugins terminate by
   * C++ ABI rules rather than entering rollback.
   */
  bool load_plugin(const fs::path& plugin_path);

  /**
   * @brief Removes one plugin's registry entries and loader-owned DSO lifetime.
   * @param plugin_path Plugin path normalized to the same absolute key as load.
   * @return True when a loaded registration was removed; false if absent.
   * @throws std::filesystem::filesystem_error if absolute normalization fails.
   * @throws std::bad_alloc if path normalization cannot allocate.
   * @note Existing scheduler owners retain a shared library lifetime until
   *       their destroy export returns; new creation becomes unavailable.
   */
  bool unload_plugin(const fs::path& plugin_path);

  /**
   * @brief Returns all built-in and plugin scheduler type names.
   * @return Alphabetically sorted host-owned type-name snapshot.
   * @throws std::bad_alloc if snapshot construction or sorting storage fails.
   * @note The snapshot remains valid after concurrent registry changes.
   */
  std::vector<std::string> get_registered_types() const;

  /**
   * @brief Tests whether a built-in or plugin scheduler type is registered.
   * @param type_name Stable scheduler type to query.
   * @return True when creation can resolve the type under the current registry.
   * @throws std::system_error only if locking a valid mutex fails.
   */
  bool is_registered(const std::string& type_name) const;

  /**
   * @brief Copies metadata for one scheduler type.
   * @param type_name Stable scheduler type to query.
   * @return Host-owned metadata, or `std::nullopt` when absent.
   * @throws std::bad_alloc if metadata snapshot construction fails.
   * @note No returned string aliases plugin memory or loader containers.
   */
  std::optional<SchedulerPluginInfo> get_info(
      const std::string& type_name) const;

  /**
   * @brief Copies metadata for every built-in and plugin scheduler type.
   * @return Host-owned metadata snapshot in registry iteration order.
   * @throws std::bad_alloc if result growth or metadata copying fails.
   * @note The human version export is not re-entered; cached strings are used.
   */
  std::vector<SchedulerPluginInfo> get_all_info() const;

  /**
   * @brief Returns one registered scheduler's cached description.
   * @param type_name Stable scheduler type to query.
   * @return Copied description, or `"Unknown scheduler type"` when absent.
   * @throws std::bad_alloc if metadata or return-value copying fails.
   */
  std::string get_description(const std::string& type_name) const;

  /**
   * @brief Creates one built-in or plugin-provided scheduler instance.
   *
   * A raw plugin result is guarded immediately, remains protected through
   * owner allocation/type-name copying, and transfers only after the complete
   * owner exists.
   *
   * @param type_name Registered scheduler type name.
   * @param num_workers Requested worker count; zero selects implementation
   * defaults and is narrowed to the fixed-width plugin ABI after validation.
   * @return Owning scheduler pointer, or nullptr for an unknown type or null
   * plugin result.
   * @throws std::overflow_error when `num_workers` exceeds `std::uint32_t`.
   * @throws std::bad_alloc for built-in creation, wrapper allocation, copied
   * owner state, or normalized plugin resource exhaustion.
   * @throws GraphError after normalizing a plugin create exception.
   * @throws Any built-in factory exception unchanged because built-ins do not
   * cross an unloadable DSO boundary.
   * @note Raw instances on exceptional exits are destroyed exactly once while
   *       their DSO remains mapped. Cleanup exceptions are suppressed only to
   *       preserve the original construction exception.
   */
  std::unique_ptr<IScheduler> create(const std::string& type_name,
                                     unsigned int num_workers = 0);

  /**
   * @brief Registers or replaces one host-owned scheduler factory.
   * @param type_name Stable built-in type name.
   * @param description Human-readable description copied into the registry.
   * @param factory Factory receiving a requested worker count.
   * @return Nothing.
   * @throws std::bad_alloc if key, description, or callable storage allocation
   * fails.
   * @note A built-in type takes precedence over conflicting plugin discovery.
   */
  void register_builtin(
      const std::string& type_name, const std::string& description,
      std::function<std::unique_ptr<IScheduler>(unsigned int)> factory);

  /**
   * @brief Removes all plugin registrations while preserving built-ins.
   * @return Nothing.
   * @throws std::system_error only if locking a valid mutex fails.
   * @note Existing created scheduler owners retain their DSO lifetime.
   */
  void clear_plugins();

  /**
   * @brief Formats every retained plugin path, types, and cached version.
   * @return Host-owned diagnostic strings, one per loaded plugin.
   * @throws std::bad_alloc if result or formatting growth fails.
   * @note This method never calls a plugin export after load-time caching.
   */
  std::vector<std::string> list_loaded_plugins() const;

  /**
   * @brief Copies recoverable plugin-load diagnostics under the registry lock.
   * @return Host-owned diagnostic snapshot.
   * @throws std::bad_alloc if copying the diagnostic sequence fails.
   * @note Returning by value prevents references from outliving the lock.
   */
  std::vector<std::string> get_load_errors() const;

  /**
   * @brief Clears all recoverable plugin-load diagnostics.
   * @return Nothing.
   * @throws std::system_error only if locking a valid mutex fails.
   */
  void clear_errors();

 private:
  /**
   * @brief Constructs an empty process registry.
   * @throws std::system_error if mutex initialization fails.
   */
  SchedulerPluginLoader() = default;

  /**
   * @brief Releases loader-owned DSO registrations at process teardown.
   * @throws Nothing.
   * @note Created scheduler owners must obey ordinary static-lifetime ordering.
   */
  ~SchedulerPluginLoader();

  /** @brief Prevents copying registry and DSO ownership. */
  SchedulerPluginLoader(const SchedulerPluginLoader&) = delete;

  /** @brief Prevents copy assignment of registry and DSO ownership. */
  SchedulerPluginLoader& operator=(const SchedulerPluginLoader&) = delete;

  /**
   * @brief Implements one plugin load while the caller holds `mutex_`.
   * @param plugin_path Candidate path supplied by direct load or scanning.
   * @return Same committed/already-loaded or recoverable result as load_plugin.
   * @throws std::bad_alloc from host allocation or normalized plugin resource
   * exhaustion.
   * @throws GraphError after normalizing any other metadata/discovery callback
   * exception.
   * @note All successful candidate mutations use `RegistryState`; recoverable
   * rejection diagnostics use the strong append helper. Caller holds `mutex_`
   * for the complete call. Numeric handshake calls are non-throwing by ABI.
   */
  bool load_plugin_internal_unlocked(const fs::path& plugin_path);

  /**
   * @brief Appends one recoverable load diagnostic with a strong guarantee.
   * @param error Fully constructed diagnostic to append.
   * @return Nothing.
   * @throws std::bad_alloc if shadow construction or growth fails.
   * @note Caller holds `mutex_`; allocation failure preserves `load_errors_`.
   */
  void append_load_error_unlocked(std::string error);

  /**
   * @brief Retains one loaded DSO and its validated scheduler ABI exports.
   *
   * `library` is authoritative RAII lifetime. `handle` is a non-owning native
   * lookup token and is never closed independently.
   *
   * @note Cached strings and registered types are destroyed before `library`,
   *       so no plugin-derived state survives DSO unmapping.
   */
  struct PluginHandle {
    /** @brief Non-owning native handle used only for symbol lookup. */
    void* handle = nullptr;
    /** @brief Shared lifetime that closes the DSO on final release. */
    std::shared_ptr<void> library;
    /** @brief Absolute registry key for the library. */
    std::string path;
    /** @brief Types successfully registered by this plugin. */
    std::vector<std::string> registered_types;
    /** @brief Human-readable version copied exactly once during loading. */
    std::string version;
    /** @brief Required numeric ABI handshake export. */
    SchedulerPluginGetAbiVersionFunc get_abi_version = nullptr;
    /** @brief Required fixed-width type-count export. */
    SchedulerPluginGetCountFunc get_count = nullptr;
    /** @brief Required fixed-width indexed type-name export. */
    SchedulerPluginGetNameFunc get_name = nullptr;
    /** @brief Required fixed-width indexed description export. */
    SchedulerPluginGetDescriptionFunc get_description = nullptr;
    /** @brief Required scheduler creation export. */
    SchedulerPluginCreateFunc create = nullptr;
    /** @brief Required scheduler destruction export. */
    SchedulerPluginDestroyFunc destroy = nullptr;
    /** @brief Required human-readable version export used only at load time. */
    SchedulerPluginGetVersionFunc get_version = nullptr;
  };

  /**
   * @brief Allocation-owning shadow of every observable plugin registry.
   *
   * Construction copies live state before candidate callbacks publish any
   * registration. `commit()` swaps completed containers while `mutex_` remains
   * held; compile-time assertions prove each swap is non-throwing.
   *
   * @note Candidate plugin state is destroyed before its shared DSO lifetime on
   *       every failed transaction.
   */
  struct RegistryState {
    /**
     * @brief Copies the loader's complete caller-visible plugin state.
     * @param loader Locked loader whose state is staged.
     * @throws std::bad_alloc if any container or element copy cannot allocate.
     */
    explicit RegistryState(const SchedulerPluginLoader& loader);

    /**
     * @brief Publishes this complete shadow into a locked loader.
     * @param loader Loader receiving staged state.
     * @return Nothing.
     * @throws Nothing.
     * @note The retained-handle map swaps first; the enclosing mutex makes all
     *       four no-throw swaps externally atomic.
     */
    void commit(SchedulerPluginLoader& loader) noexcept;

    /** @brief Staged absolute-path to retained-DSO registry. */
    std::map<std::string, PluginHandle> loaded_plugins;
    /** @brief Staged scheduler-type to plugin-path registry. */
    std::map<std::string, std::string> type_to_plugin;
    /** @brief Staged scheduler metadata registry. */
    std::map<std::string, SchedulerPluginInfo> type_info;
    /** @brief Staged recoverable diagnostic sequence. */
    std::vector<std::string> load_errors;
  };

  /**
   * @brief Host-owned description and factory for one built-in scheduler.
   * @note Factory calls occur while `mutex_` is held and must not re-enter the
   *       loader.
   */
  struct BuiltinScheduler {
    /** @brief Human-readable built-in description. */
    std::string description;
    /** @brief Host factory receiving requested worker count. */
    std::function<std::unique_ptr<IScheduler>(unsigned int)> factory;
  };

  /** @brief Absolute path to validated retained plugin state. */
  std::map<std::string, PluginHandle> loaded_plugins_;
  /** @brief Plugin scheduler type to its absolute owning DSO path. */
  std::map<std::string, std::string> type_to_plugin_;
  /** @brief Plugin scheduler type to host-owned cached metadata. */
  std::map<std::string, SchedulerPluginInfo> type_info_;
  /** @brief Built-in scheduler type to host-owned factory state. */
  std::map<std::string, BuiltinScheduler> builtins_;
  /** @brief Recoverable load diagnostics in observation order. */
  std::vector<std::string> load_errors_;
  /** @brief Serializes every registry, diagnostic, and DSO-lifetime mutation.
   */
  mutable std::mutex mutex_;
};

}  // namespace ps
