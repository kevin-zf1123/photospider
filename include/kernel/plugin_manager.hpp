#pragma once

#include <atomic>
#include <cstddef>
#include <map>
#include <string>
#include <vector>

#include "plugin_loader.hpp"  // NOLINT(build/include_subdir)

namespace ps {

/**
 * @brief Owns process-global operation plugin state and library handles.
 *
 * `PluginManager` is the unique process-level owner for operation plugins.
 * Every Kernel and embedded Host reaches the same manager, source map, retained
 * handle map, restoration snapshots, and successful-load ordering through
 * process_instance(). Host destruction never destroys this owner.
 *
 * Unload operations first remove plugin-owned keys from `OpRegistry`, restore
 * any previous implementation replaced by the plugin, then release the
 * retained library handle. Built-in operations are tracked as `"built-in"`
 * sources and are restored when an overriding plugin is unloaded.
 *
 * @note The manager serializes every load, unload, source/key inspection, and
 *       built-in seed operation. It is intentionally separate from scheduler
 *       plugin loading because operation plugins register process-global
 *       callbacks into `OpRegistry`, while scheduler plugins own live
 *       `IScheduler` instances and destroy functions.
 */
class PluginManager {
 public:
  /**
   * @brief Returns the unique process-level operation plugin owner.
   *
   * @return Process-lifetime manager shared by every Kernel and embedded Host.
   * @throws std::bad_alloc if first-use manager allocation fails.
   * @note Intentional process-lifetime residency avoids static-destruction
   *       ordering against `OpRegistry`. Plugins are released only by explicit
   *       unload, never by Host or Kernel destruction.
   */
  static PluginManager& process_instance();

  /**
   * @brief Prevents creation of a second process plugin owner by copying.
   *
   * @param other Process owner that must not be copied.
   * @note Deletion preserves one source/handle/restoration state machine for
   * the process-global registry.
   */
  PluginManager(const PluginManager& other) = delete;

  /**
   * @brief Prevents replacement of process ownership state by assignment.
   *
   * @param other Process owner that must not be assigned.
   * @return No value because this operation is deleted.
   * @note Manager identity and lock/container addresses remain process-stable.
   */
  PluginManager& operator=(const PluginManager& other) = delete;

  /**
   * @brief Loads operation plugins from directory patterns.
   *
   * @param dir_patterns Directories or trailing-star patterns to scan for
   * platform shared libraries.
   * @return Nothing.
   * @throws std::bad_alloc if path, result, registry, or plugin handle storage
   *         exhausts memory.
   * @throws std::overflow_error if successful-load sequence space is exhausted.
   * @throws std::filesystem::filesystem_error when iteration fails for an
   *         existing directory.
   * @note Errors are discarded by this convenience wrapper; callers needing
   * diagnostics should use `load_from_dirs_report`.
   */
  void load_from_dirs(const std::vector<std::string>& dir_patterns);

  /**
   * @brief Loads operation plugins and returns structured diagnostics.
   *
   * @param dir_patterns Directories or trailing-star patterns to scan.
   * @return Load result containing attempted files, successful libraries,
   * keys registered or replaced by plugins, and per-plugin errors.
   * @throws std::bad_alloc if path, result, registry, or plugin handle storage
   *         exhausts memory.
   * @throws std::overflow_error if successful-load sequence space is exhausted.
   * @throws std::filesystem::filesystem_error when iteration fails for an
   *         existing directory.
   * @note Successful plugin handles are retained by the process manager until
   * explicit unload. A candidate allocation
   * failure leaves registry, source map, report prefix, and handle map
   * unchanged for that candidate; previously committed candidates remain
   * loaded.
   */
  PluginLoadResult load_from_dirs_report(
      const std::vector<std::string>& dir_patterns);

  /**
   * @brief Records current built-in operation keys as `"built-in"` sources.
   *
   * @return Nothing.
   * @throws std::bad_alloc if built-in registration or source-map population
   *         exhausts memory.
   * @throws Exceptions from built-in registration or source-map reconciliation
   *         unchanged.
   * @note Built-in callbacks initialize at most once through this process
   * owner. Later calls reconcile copied source labels without replaying
   * built-in registration over an active plugin replacement.
   */
  void seed_builtins_from_registry();

  /**
   * @brief Unloads operation keys registered or replaced by one plugin path.
   *
   * @param absolute_plugin_path Plugin path to unload. Relative inputs are
   * normalized with `std::filesystem::absolute` for convenience.
   * @return Number of operation keys removed or restored in `OpRegistry`.
   * @throws std::bad_alloc if a relative or non-normalized path requires
   * convenience normalization and that normalization cannot allocate. No
   * manager state has changed when this exception propagates.
   * @throws std::filesystem::filesystem_error when path normalization fails.
   * @note Passing the exact absolute key recorded at load time takes a direct,
   * allocation-free lookup and cleanup path. The retained dynamic library
   * handle is released only after registry callbacks from that plugin have
   * been removed and previous callbacks, if any, have been restored. If the
   * plugin was already shadowed by a later plugin and owns no active keys, its
   * predecessor snapshots are spliced into the later plugin before the middle
   * handle is released.
   */
  int unload_by_plugin_path(const std::string& absolute_plugin_path);

  /**
   * @brief Unloads all dynamic operation plugins while preserving built-ins.
   *
   * @return Number of operation keys removed or restored in `OpRegistry`.
   * @throws Nothing. Unload consumes keys, source snapshots, registry
   * snapshots, and load sequences allocated during successful load.
   * @note Plugins are unwound in reverse successful-load order. Each plugin's
   * callbacks and metadata are removed or restored before its retained handle
   * is released, including during global allocation failure. Callback and
   * returned-value leases may defer final library unmapping after registry
   * visibility has been removed.
   */
  int unload_all_plugins() noexcept;

  /**
   * @brief Returns registered operation keys and their source labels.
   *
   * @return Copied map from operation key to `"built-in"`, one absolute plugin
   *         path, or `"mixed"` when active slots have different owners.
   * @throws std::bad_alloc if snapshot copying exhausts memory.
   * @note The snapshot is coherent with one serialized process-manager state
   *       and remains valid after later Host mutations.
   */
  std::map<std::string, std::string> op_sources() const;

  /**
   * @brief Returns a coherent process-global combined operation key snapshot.
   *
   * @return Combined operation keys copied from the shared registry.
   * @throws std::bad_alloc if registry snapshot construction exhausts memory.
   * @note Inspection is serialized with operation plugin load and unload.
   */
  std::vector<std::string> combined_keys() const;

  /**
   * @brief Returns coherent process-global combined operation source labels.
   *
   * @return Combined operation keys mapped to plugin paths, `"built-in"`, or
   *         `"mixed"` for slot-wise mixed ownership.
   * @throws std::bad_alloc if registry/source snapshot construction exhausts
   *         memory.
   * @note Inspection observes the same serialized source and registry state;
   *       unknown aliases preserve the historical `"built-in"` fallback.
   */
  std::map<std::string, std::string> combined_sources() const;

  /**
   * @brief Returns the number of retained dynamic operation plugin handles.
   *
   * @return Count of entries in the manager-owned handle map.
   * @throws Nothing.
   * @note This is a narrow inspection API used by tests and diagnostics to
   * verify handle ownership.
   */
  size_t loaded_plugin_count() const noexcept;

 private:
  /**
   * @brief Creates the one process-level operation plugin owner in empty state.
   *
   * @throws Nothing; containers and lock state begin empty and unlocked.
   * @note Construction is private so only `process_instance()` can establish
   *       ownership for the process-global registry.
   */
  PluginManager() = default;

  /**
   * @brief Private non-running destructor for the process-resident owner.
   *
   * @throws Nothing.
   * @note process_instance() intentionally never destroys its allocation;
   *       explicit unload owns callback and handle cleanup semantics.
   */
  ~PluginManager() = default;

  /**
   * @brief Allocation-free guard that serializes complete manager state.
   *
   * @throws Nothing.
   * @note The recursive token cannot consume allocation failpoints used to
   *       validate noexcept explicit unload. Same-thread callback/DSO
   *       destructor diagnostics cannot self-deadlock on the retiring owner.
   */
  class StateLockGuard {
   public:
    /**
     * @brief Acquires one recursive level of the complete manager state lock.
     *
     * @param manager Process owner whose state remains serialized for this
     * guard lifetime.
     * @throws Nothing.
     * @note Re-entry on the same thread increments a non-atomic owner-only
     * depth; competing threads yield until the outermost guard releases it.
     */
    explicit StateLockGuard(const PluginManager& manager) noexcept;

    /**
     * @brief Releases the recursive manager lock level acquired at
     * construction.
     *
     * @throws Nothing.
     * @note The owner token becomes available to another thread only when this
     *       guard releases the outermost recursive level.
     */
    ~StateLockGuard();

    /**
     * @brief Prevents duplicating ownership of one acquired lock level.
     *
     * @param other Guard that must remain the sole release owner.
     * @note Deletion prevents two destructors from decrementing one lock depth.
     */
    StateLockGuard(const StateLockGuard& other) = delete;

    /**
     * @brief Prevents retargeting an active manager lock guard.
     *
     * @param other Guard that must not transfer its release obligation.
     * @return No value because this operation is deleted.
     * @note Guard lifetime remains lexically paired with one acquisition.
     */
    StateLockGuard& operator=(const StateLockGuard& other) = delete;

   private:
    /**
     * @brief Manager whose state remains locked for this guard lifetime.
     *
     * @note The reference is borrowed and valid because the process owner is
     *       intentionally never destroyed.
     */
    const PluginManager& manager_;
  };

  /**
   * @brief Acquires the allocation-free recursive manager state lock.
   *
   * @return Nothing.
   * @throws Nothing.
   * @note The method uses a thread-local address as identity and performs no
   *       dynamic allocation, allowing it inside no-throw unload paths.
   */
  void lock_state() const noexcept;

  /**
   * @brief Releases one recursive manager state-lock level.
   *
   * @return Nothing.
   * @throws Nothing.
   * @note Only the owning thread calls this method; the outermost release uses
   *       release ordering before publishing a null owner token.
   */
  void unlock_state() const noexcept;

  /**
   * @brief Initializes built-ins once and reconciles their source labels.
   *
   * @return Nothing.
   * @throws std::bad_alloc if built-in registration, registry snapshotting, or
   *         source-map reconciliation exhausts memory.
   * @note Caller holds both the manager and process-registry state locks. Once
   *       initialized, later Host seed calls never replay built-in registration
   *       over an active plugin override.
   */
  void synchronize_builtins_locked();

  /**
   * @brief Builds source labels from active per-slot registry ownership.
   *
   * @return Coherent copied source map using a plugin path only when that
   *         plugin owns every active slot, `"mixed"` when plugin/direct or
   *         multiple-plugin slots coexist, and `"built-in"` otherwise.
   * @throws std::bad_alloc if key, map, or source-string copying allocates.
   * @note Caller holds both manager and process-registry state locks. Stored
   *       `op_sources_` remains restoration bookkeeping; this view never trusts
   *       a stale key-level label after a direct same-key mutation.
   */
  std::map<std::string, std::string> effective_sources_locked() const;

  /**
   * @brief Operation source map shown to frontends and unload code.
   *
   * Keys use `type:subtype`; stored values are either `"built-in"` or absolute
   * plugin paths. Public inspection derives `"mixed"` from active slot tokens;
   * the map itself does not own dynamic libraries.
   *
   * @note Access is serialized by `StateLockGuard`; public inspection returns a
   *       copy rather than exposing references across later mutations.
   */
  std::map<std::string, std::string> op_sources_;

  /**
   * @brief Manager-owned dynamic library handles keyed by absolute path.
   *
   * Each value also records the operation keys discovered during registration
   * so unload can remove callbacks before releasing the final handle reference.
   *
   * @note Records additionally own predecessor snapshots and load sequence;
   *       they are extracted only after registry/source visibility changes.
   */
  LoadedOpPluginMap loaded_plugins_;

  /**
   * @brief Names the thread serializing process plugin state.
   *
   * The recursive owner token lets plugin callable or dynamic-library
   * destructors perform same-thread diagnostic inspection without spinning on
   * the manager that is retiring them.
   *
   * @note A null token means unlocked. Acquire/release ordering publishes all
   *       manager container mutations across owning threads.
   */
  mutable std::atomic<const void*> state_lock_owner_{nullptr};

  /**
   * @brief Recursive hold count owned only by `state_lock_owner_`.
   *
   * @note Competing threads never access this non-atomic value until they own
   * the token, so it cannot race with the current owner's updates.
   */
  mutable std::size_t state_lock_depth_ = 0;

  /**
   * @brief Whether built-in registration has run through the process owner.
   *
   * Source labels are reconciled on every explicit seed and before plugin load,
   * but the callback registration body runs at most once so it cannot overwrite
   * a later plugin replacement.
   */
  bool builtins_seeded_ = false;
};

}  // namespace ps
