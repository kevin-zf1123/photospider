#include "plugin/plugin_loader.hpp"  // NOLINT(build/include_subdir)

#include <algorithm>
#include <exception>
#include <limits>
#include <map>
#include <memory>
#include <new>
#include <optional>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#include "core/ps_types.hpp"  // NOLINT(build/include_subdir)
#include "photospider/plugin/operation_plugin_api.h"
#include "plugin/operation_host_adapter.hpp"
#include "plugin/plugin_trust.hpp"

#if defined(PHOTOSPIDER_INTERNAL_BAD_ALLOC_TESTING)
#include "core/op_registry_test_access.hpp"
#include "plugin/plugin_loader_test_access.hpp"
#endif

#ifdef _WIN32
#include <windows.h>
#else
#include <dlfcn.h>
#endif

namespace ps {
namespace {

/**
 * @brief Selects the next unload-order sequence for a candidate plugin.
 *
 * @param loaded_plugins Current retained plugin records.
 * @return One greater than the largest visible sequence, or one for the first
 * plugin.
 * @throws std::overflow_error if the sequence space is exhausted.
 * @note The scan allocates nothing and runs before candidate publication.
 */
std::uint64_t next_plugin_load_sequence(
    const LoadedOpPluginMap& loaded_plugins) {
  std::uint64_t largest = 0;
  for (const auto& [_, plugin] : loaded_plugins) {
    largest = std::max(largest, plugin.load_sequence);
  }
  if (largest == std::numeric_limits<std::uint64_t>::max()) {
    throw std::overflow_error("operation plugin load sequence exhausted");
  }
  return largest + 1;
}

#if defined(PHOTOSPIDER_INTERNAL_BAD_ALLOC_TESTING)
/** @brief Thread-local post-registration failure selected by the current test.
 */
using LoadFailpoint = testing::OperationPluginLoadFailpoint;
thread_local LoadFailpoint load_failpoint = LoadFailpoint::None;
/** @brief Number of actual visits to the selected test-only failure site. */
thread_local std::size_t operation_plugin_load_failpoint_hit_count = 0;

/**
 * @brief Throws at one selected post-registration bookkeeping boundary.
 *
 * @param failpoint Boundary currently being entered by the real loader.
 * @return Nothing.
 * @throws std::bad_alloc when `failpoint` is the armed boundary.
 * @note This helper and its mutable state are omitted from BUILD_TESTING=OFF
 * products. The hit count changes before throwing so tests can distinguish the
 * intended failure from an unrelated allocation failure.
 */
void maybe_fail_operation_plugin_bookkeeping(
    testing::OperationPluginLoadFailpoint failpoint) {
  if (load_failpoint != failpoint) {
    return;
  }
  ++operation_plugin_load_failpoint_hit_count;
  throw std::bad_alloc();
}
#endif

/** @brief Numeric discovery entry resolved before any ABI record is read. */
using GetOperationAbiVersion = ps_operation_plugin_get_abi_version_fn_v1;

/** @brief Exact operation ABI-v1 root discovery entry. */
using GetOperationApi = ps_operation_plugin_get_api_fn_v1;
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
 * @brief Closes one operation plugin native handle.
 * @param handle Nonnull `LoadLibrary` or `dlopen` result.
 * @return Nothing after the platform close attempt.
 * @throws Nothing; close failures are intentionally ignored during retirement.
 * @note `make_authorized_native_library_lease()` invokes this before releasing
 * the matching exact-object capability.
 */
void close_operation_library(void* handle) noexcept {
#ifdef _WIN32
  (void)FreeLibrary(static_cast<HMODULE>(handle));
#else
  (void)dlclose(handle);
#endif
}

/**
 * @brief RAII wrapper for one operation plugin dynamic library.
 *
 * The wrapper owns the native handle through `library_` and exposes typed
 * symbol lookup. Moving or copying the wrapper itself is intentionally
 * disabled; users pass around `shared_ptr<void>` lifetimes instead.
 *
 * @note This helper is local to the operation plugin loader and never loads
 * installed Policy ABI libraries, whose lifecycle belongs to PolicyRegistry.
 */
class DynamicLibrary final {
 public:
  /**
   * @brief Opens a dynamic library and reports platform loader failures.
   *
   * @param authorized Retained candidate object already approved for the
   * operation role. Success transfers it into the shared native-library lease.
   * @param error_message Output error text when opening fails.
   * @return A library wrapper on success, or `nullptr` on failure.
   * @throws std::bad_alloc if allocation of the wrapper or handle owner fails.
   * @throws PluginTrustError if the capability cannot expose an exact-object
   * native path on the active platform.
   * @throws std::invalid_argument if a successful platform handle is paired
   * with an inactive capability, which is an internal invariant violation.
   * @note The capability remains live through native mapping and then through
   * every callback/record owner. On failure the handle closes before the local
   * capability, and the caller remains responsible for structured path output.
   */
  static std::unique_ptr<DynamicLibrary> open(AuthorizedPluginFile authorized,
                                              std::string& error_message) {
    const std::string load_path = authorized.native_load_path();
#ifdef _WIN32
    HMODULE handle = LoadLibraryA(load_path.c_str());
    if (!handle) {
      error_message = "LoadLibrary failed";
      return nullptr;
    }
    auto library_lifetime = make_authorized_native_library_lease(
        static_cast<void*>(handle), std::move(authorized),
        close_operation_library);
    return std::unique_ptr<DynamicLibrary>(new DynamicLibrary(
        static_cast<void*>(handle), std::move(library_lifetime)));
#else
    void* handle = dlopen(load_path.c_str(), RTLD_NOW | RTLD_LOCAL);
    if (!handle) {
      const char* err = dlerror();
      error_message = err ? err : "dlopen failed";
      return nullptr;
    }
    auto library_lifetime = make_authorized_native_library_lease(
        handle, std::move(authorized), close_operation_library);
    return std::unique_ptr<DynamicLibrary>(
        new DynamicLibrary(handle, std::move(library_lifetime)));
#endif
  }

  /**
   * @brief Resolves a required C symbol as a typed function pointer.
   *
   * @tparam Function Function pointer type expected by the caller.
   * @param symbol_name Exported symbol name to locate.
   * @param error_message Output error detail when the symbol is missing.
   * @return Typed function pointer on success, or `nullptr` on failure.
   * @throws std::bad_alloc if writing the platform error into `error_message`
   * cannot allocate.
   * @note POSIX `dlsym` returns `void*`; the cast is isolated here so the call
   * sites stay simple and auditable. Platform symbol lookup errors themselves
   * are returned in `error_message`.
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
   * @brief Returns the combined native-library lifetime retained by owners.
   *
   * @return Shared native handle and exact-object capability owner.
   * @throws Nothing.
   * @note Holding this object keeps the library loaded even after the local
   * `DynamicLibrary` wrapper goes out of scope.
   */
  std::shared_ptr<void> lifetime() const noexcept { return library_; }

 private:
  /**
   * @brief Takes ownership of an already-opened combined native lease.
   *
   * @param handle Native platform handle returned by `LoadLibrary` or `dlopen`.
   * @param library_lifetime Prebuilt shared owner for handle and capability.
   * @throws Nothing; the shared owner is moved into place.
   * @note The caller constructs library_lifetime before allocating this wrapper
   * so wrapper allocation failure still closes the native handle before the
   * exact-object capability.
   */
  explicit DynamicLibrary(void* handle,
                          std::shared_ptr<void> library_lifetime) noexcept
      : handle_(handle), library_(std::move(library_lifetime)) {}

  /** @brief Native dynamic-library handle used for symbol lookup. */
  void* handle_ = nullptr;
  /**
   * @brief Shared owner that unloads the native library and then releases its
   * exact-object capability after the last plugin handle releases it.
   */
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
  /** @brief Directory root visited by the plugin scanner. */
  fs::path base_dir;
  /**
   * @brief True when the raw scan pattern requested recursive
   * slash-double-star walking.
   */
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
 * @throws std::filesystem::filesystem_error when directory iteration fails.
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
 * @brief Appends registered operation sources to the result and map.
 *
 * @param plugin_path Absolute plugin path used as the operation source.
 * @param registered_keys Operation keys touched during registration.
 * @param op_sources Destination `op key -> plugin path` map.
 * @param result Load result receiving registered operation keys.
 * @throws std::bad_alloc from map or vector growth.
 * @note Existing source entries are overwritten for keys replaced by this
 * plugin so unload can identify the plugin-owned callback.
 */
void record_registered_operation_sources(
    const std::string& plugin_path,
    const std::vector<std::string>& registered_keys,
    std::map<std::string, std::string>& op_sources, PluginLoadResult& result) {
  for (const auto& key : registered_keys) {
    op_sources[key] = plugin_path;
    result.new_op_keys.push_back(key);
  }
}

/**
 * @brief Captures previous source-map entries for keys a plugin registered.
 *
 * @param registered_keys Operation keys touched by the plugin.
 * @param op_sources Current operation source map before plugin source writes.
 * @return Map from key to previous source; nullopt means no prior source.
 * @throws std::bad_alloc from map growth or string copies.
 * @note The loader stores this beside registry snapshots so unload can restore
 * both lookup callbacks and frontend-visible source labels.
 */
std::map<std::string, std::optional<std::string>> collect_previous_sources(
    const std::vector<std::string>& registered_keys,
    const std::map<std::string, std::string>& op_sources) {
  std::map<std::string, std::optional<std::string>> previous_sources;
  for (const auto& key : registered_keys) {
    auto source_it = op_sources.find(key);
    if (source_it == op_sources.end()) {
      previous_sources.emplace(key, std::nullopt);
      continue;
    }
    previous_sources.emplace(key, source_it->second);
  }
  return previous_sources;
}

/**
 * @brief Preallocates callback retirement slots for one plugin publication.
 *
 * @param owned_entries Final per-key slot revisions written by the registrar.
 * @return Snapshot map containing empty callable placeholders and one default
 *         stable device-owner slot per plugin-owned appended element.
 * @throws std::bad_alloc if map, key, or device-placeholder storage allocation
 *         fails.
 * @note The returned storage contains no plugin callback yet. Allocation-free
 *       unload swaps live or dependent plugin callback state into it before
 *       releasing the registry lock.
 */
std::unordered_map<std::string, OpRegistry::RegistryEntrySnapshot>
make_retirement_registry_entries(
    const std::unordered_map<std::string, OpRegistry::RegistryEntryOwnership>&
        owned_entries) {
  std::unordered_map<std::string, OpRegistry::RegistryEntrySnapshot>
      retirement_entries;
  retirement_entries.reserve(owned_entries.size());
  for (const auto& [key, owned] : owned_entries) {
    OpRegistry::RegistryEntrySnapshot retirement;
    if (owned.legacy_op != 0) {
      retirement.legacy_op.emplace(MonolithicOpFunc{});
    }
    if (owned.metadata != 0) {
      retirement.metadata.emplace();
    }
    const bool owns_implementation_slot =
        owned.monolithic_hp != 0 || owned.tiled_hp != 0 ||
        owned.tiled_rt != 0 || owned.dirty_propagator != 0 ||
        owned.forward_propagator != 0 || owned.dependency_builder != 0 ||
        owned.data_dependent != 0 || !owned.device_impls.empty();
    if (owns_implementation_slot) {
      retirement.implementations.emplace();
      retirement.implementations->device_impl_slots.resize(
          owned.device_impls.size());
    }
    retirement_entries.emplace(key, std::move(retirement));
  }
  return retirement_entries;
}

/**
 * @brief Exchanges two structured plugin-load results without allocation.
 *
 * @param left First accumulated result.
 * @param right Second accumulated result.
 * @return Nothing.
 * @throws Nothing.
 * @note Integers and standard-allocator vectors are swapped independently so
 * operation-plugin transaction publication has a mechanically auditable
 * no-throw result step.
 */
void swap_plugin_load_result(PluginLoadResult& left,
                             PluginLoadResult& right) noexcept {
  static_assert(noexcept(left.errors.swap(right.errors)),
                "PluginLoadError vector swap must be noexcept");
  static_assert(noexcept(left.new_op_keys.swap(right.new_op_keys)),
                "operation-key vector swap must be noexcept");
  using std::swap;
  swap(left.attempted, right.attempted);
  swap(left.loaded, right.loaded);
  left.errors.swap(right.errors);
  left.new_op_keys.swap(right.new_op_keys);
}

/**
 * @brief Owns all unpublished state for one operation-plugin load attempt.
 *
 * Construction copies the current registry, source map, retained-handle map,
 * and accumulated result before the registrar runs. Registration and every
 * later allocating bookkeeping stage mutate only these shadow values. After
 * all staging succeeds, `commit_all` publishes them through no-throw swaps.
 *
 * Member declaration order is part of the safety proof: `library_lifetime_`
 * is declared first and therefore destroyed last. On every exceptional exit,
 * captured snapshots, staged results, copied/candidate leases, source
 * strings, and plugin callbacks in `staged_registry_` are destroyed while the
 * candidate library is still mapped.
 *
 * @note The transaction never invokes a throwing registry rollback. Failed
 * attempts leave caller-owned state byte-for-byte semantically unchanged;
 * ordinary registrar errors may commit only their structured result row after
 * staged callbacks have become cleanup-owned by this object.
 */
class OperationPluginLoadTransaction final {
 public:
  /**
   * @brief Copies all caller-owned state before entering plugin code.
   *
   * @param library_lifetime Candidate handle/capability lease retained through
   * rollback destruction.
   * @param registry Current host registry copied into the shadow registry.
   * @param op_sources Current frontend-visible source map.
   * @param loaded_plugins Existing retained plugin records and handles.
   * @param result Accumulated scan result, including the staged attempt count.
   * @throws std::bad_alloc if any shadow copy cannot allocate.
   * @note No plugin callback has run when construction throws, and the caller's
   * live registry/source/result/handle state remains unchanged.
   */
  OperationPluginLoadTransaction(
      std::shared_ptr<void> library_lifetime, const OpRegistry& registry,
      const std::map<std::string, std::string>& op_sources,
      const LoadedOpPluginMap& loaded_plugins, const PluginLoadResult& result)
      : library_lifetime_(std::move(library_lifetime)),
        staged_registry_(registry),
        staged_op_sources_(op_sources),
        staged_loaded_plugins_(loaded_plugins),
        staged_result_(result) {}

  OperationPluginLoadTransaction(const OperationPluginLoadTransaction&) =
      delete;
  OperationPluginLoadTransaction& operator=(
      const OperationPluginLoadTransaction&) = delete;

  /**
   * @brief Destroys unpublished state while retaining the library until last.
   *
   * @throws Nothing.
   * @note Default member destruction follows reverse declaration order, which
   * is the transaction rollback ordering documented on the class.
   */
  ~OperationPluginLoadTransaction() = default;

  /**
   * @brief Returns the shadow registry passed to the host registrar.
   *
   * @return Mutable unpublished registry.
   * @throws Nothing.
   * @note Callbacks stored here cannot be resolved through the singleton until
   * the final commit swaps complete.
   */
  OpRegistry& registry() noexcept { return staged_registry_; }

  /**
   * @brief Returns the mutable registration capture for unload snapshots.
   *
   * @return Transaction-owned capture.
   * @throws Nothing.
   * @note Its allocations and plugin callable copies are destroyed before the
   * retained candidate library on failure.
   */
  OpRegistry::RegistrationCapture& registration_capture() noexcept {
    return registration_capture_;
  }

  /**
   * @brief Returns the shadow operation source map.
   *
   * @return Mutable unpublished source map.
   * @throws Nothing.
   */
  std::map<std::string, std::string>& op_sources() noexcept {
    return staged_op_sources_;
  }

  /**
   * @brief Returns the shadow retained-handle map.
   *
   * @return Mutable unpublished handle/snapshot map.
   * @throws Nothing.
   */
  LoadedOpPluginMap& loaded_plugins() noexcept {
    return staged_loaded_plugins_;
  }

  /**
   * @brief Returns the shadow accumulated scan result.
   *
   * @return Mutable unpublished result.
   * @throws Nothing.
   */
  PluginLoadResult& result() noexcept { return staged_result_; }

  /**
   * @brief Returns the shared candidate lease for the published record.
   *
   * @return Shared owner whose copy operation cannot allocate.
   * @throws Nothing.
   * @note The returned copy shares the control block created before registrar
   * execution.
   */
  std::shared_ptr<void> library_lifetime() const noexcept {
    return library_lifetime_;
  }

  /**
   * @brief Publishes only a recoverable registrar error result.
   *
   * @param result Caller-owned accumulated scan result.
   * @return Nothing.
   * @throws Nothing.
   * @note Registry, sources, and handles remain unchanged. The transaction
   * destructor then removes unpublished plugin callbacks before the library is
   * released.
   */
  void commit_result_only(PluginLoadResult& result) noexcept {
    swap_plugin_load_result(result, staged_result_);
  }

  /**
   * @brief Atomically-by-noexcept-phase publishes every staged state surface.
   *
   * @param registry Live singleton registry.
   * @param op_sources Live frontend-visible source map.
   * @param loaded_plugins Live retained plugin map.
   * @param result Live accumulated scan result.
   * @return Nothing.
   * @throws Nothing.
   * @note The lease map is published before the registry, while the local
   * `DynamicLibrary` and this transaction still retain the candidate handle
   * and capability.
   * No observer can see a plugin callback whose library lacks an owner after
   * the final registry swap.
   */
  void commit_all(OpRegistry& registry,
                  std::map<std::string, std::string>& op_sources,
                  LoadedOpPluginMap& loaded_plugins,
                  PluginLoadResult& result) noexcept {
    static_assert(noexcept(loaded_plugins.swap(staged_loaded_plugins_)),
                  "loaded plugin map swap must be noexcept");
    static_assert(noexcept(op_sources.swap(staged_op_sources_)),
                  "operation source map swap must be noexcept");
    loaded_plugins.swap(staged_loaded_plugins_);
    op_sources.swap(staged_op_sources_);
    swap_plugin_load_result(result, staged_result_);
    registry.swap_state(staged_registry_);
  }

 private:
  /** @brief Candidate combined lease declared first and destroyed last. */
  std::shared_ptr<void> library_lifetime_;
  /** @brief Registry copy receiving every registrar mutation. */
  OpRegistry staged_registry_;
  /** @brief Source-map copy receiving candidate ownership labels. */
  std::map<std::string, std::string> staged_op_sources_;
  /** @brief Existing plus candidate retained-handle records. */
  LoadedOpPluginMap staged_loaded_plugins_;
  /** @brief Result copy receiving attempt/error/key/load bookkeeping. */
  PluginLoadResult staged_result_;
  /** @brief Prior registry snapshots captured inside the shadow registry. */
  OpRegistry::RegistrationCapture registration_capture_;
};

/**
 * @brief Captures plugin registration inside an unpublished shadow registry.
 *
 * @param registry Host registry that owns the transactional capture.
 * @param generation Validated candidate generation captured by every callback.
 * @param registration_capture Destination snapshot populated by the registry.
 * @return Nothing.
 * @throws Exceptions from registrar construction, plugin registration, or
 * capture storage to the owning loader while library_lifetime remains alive.
 * @note No catch/restore step is needed: `registry` is transaction-owned and
 * unpublished. Its callbacks and capture are destroyed before the candidate
 * library lifetime if any exception exits this function. Registrar/context
 * references remain stack-bounded and are not retained. The owning loader
 * copies ordinary diagnostics and replaces any plugin-origin bad_alloc with a
 * fresh host-owned instance before this lifetime can retire.
 */
void capture_plugin_registration(
    OpRegistry& registry,
    const std::shared_ptr<plugin_host::OperationPluginGeneration>& generation,
    OpRegistry::RegistrationCapture& registration_capture) {
  registry.capture_registration([&]() { generation->register_into(registry); },
                                registration_capture);
}

/**
 * @brief Loads one operation plugin file and retains its dynamic library.
 *
 * @param path Candidate shared library path.
 * @param op_sources Operation source map to update with registered or replaced
 * keys.
 * @param loaded_plugins Caller-owned handle map keyed by absolute plugin path.
 * @param result Accumulated load result.
 * @return Nothing.
 * @throws std::filesystem::filesystem_error from `fs::absolute`.
 * @throws std::bad_alloc from result, shadow registry/source/handle copies,
 * registrar execution, or post-registration staging. Plugin-origin resource
 * exhaustion is rethrown as a fresh host-owned standard bad_alloc.
 * @throws std::invalid_argument if a successful native open is paired with an
 * inactive exact-object capability.
 * @throws Any non-`PluginTrustError` exception cached by process trust-policy
 * initialization unchanged.
 * @note Signed content/role authorization opens and hashes the candidate before
 * native mapping. Successful mapping moves the retained exact-file capability
 * into the same shared lease as the native handle, so its descriptor remains
 * live through all callback and generation owners and closes only after final
 * native unload. Trust rejection cannot run an initializer. The live
 * singleton, source map, accumulated result, and handle map are changed only
 * by a final no-throw swap phase. On later failure the transaction's shadow
 * callback state is destroyed before its candidate lease, so no throwing
 * rollback, leaked handle, dangling callback, identity mismatch, or partial
 * result is exposed.
 */
void load_one_plugin(const fs::path& path,
                     std::map<std::string, std::string>& op_sources,
                     LoadedOpPluginMap& loaded_plugins,
                     PluginLoadResult& result) {
  const std::string absolute_path = fs::absolute(path).string();
  if (loaded_plugins.count(absolute_path) > 0) {
    return;
  }
  const std::uint64_t load_sequence = next_plugin_load_sequence(loaded_plugins);

  PluginLoadResult staged_result = result;
  ++staged_result.attempted;

  std::optional<AuthorizedPluginFile> authorized;
  try {
    authorized.emplace(
        authorize_process_plugin(absolute_path, PluginArtifactKind::Operation));
  } catch (const PluginTrustError& error) {
    staged_result.errors.push_back(
        {absolute_path, GraphErrc::InvalidParameter, error.what()});
    swap_plugin_load_result(result, staged_result);
    return;
  }

  std::string error_message;
  auto library = DynamicLibrary::open(std::move(*authorized), error_message);
  if (!library) {
    staged_result.errors.push_back(
        {absolute_path, GraphErrc::Io, std::move(error_message)});
    swap_plugin_load_result(result, staged_result);
    return;
  }

  auto get_abi_version = library->resolve<GetOperationAbiVersion>(
      PS_OPERATION_PLUGIN_GET_ABI_VERSION_SYMBOL, error_message);
  if (!get_abi_version) {
    staged_result.errors.push_back(
        {absolute_path, GraphErrc::InvalidParameter, error_message});
    swap_plugin_load_result(result, staged_result);
    return;
  }
  auto get_api = library->resolve<GetOperationApi>(
      PS_OPERATION_PLUGIN_GET_API_V1_SYMBOL, error_message);
  if (!get_api) {
    staged_result.errors.push_back(
        {absolute_path, GraphErrc::InvalidParameter, error_message});
    swap_plugin_load_result(result, staged_result);
    return;
  }

  std::shared_ptr<plugin_host::OperationPluginGeneration> generation;
  try {
    generation = plugin_host::OperationPluginGeneration::create(
        library->lifetime(), get_abi_version, get_api);
  } catch (const std::bad_alloc&) {
    throw std::bad_alloc();
  } catch (const GraphError& error) {
    staged_result.errors.push_back({absolute_path, error.code(), error.what()});
    swap_plugin_load_result(result, staged_result);
    return;
  } catch (const std::invalid_argument& error) {
    staged_result.errors.push_back(
        {absolute_path, GraphErrc::InvalidParameter, error.what()});
    swap_plugin_load_result(result, staged_result);
    return;
  } catch (const std::exception& error) {
    staged_result.errors.push_back(
        {absolute_path, GraphErrc::Unknown, error.what()});
    swap_plugin_load_result(result, staged_result);
    return;
  } catch (...) {
    staged_result.errors.push_back(
        {absolute_path, GraphErrc::Unknown,
         "operation ABI discovery failed with an unknown exception"});
    swap_plugin_load_result(result, staged_result);
    return;
  }

  auto& registry = OpRegistry::instance();
  OperationPluginLoadTransaction transaction(generation, registry, op_sources,
                                             loaded_plugins, staged_result);
  auto& registration_capture = transaction.registration_capture();
  try {
    capture_plugin_registration(transaction.registry(), generation,
                                registration_capture);
  } catch (const std::bad_alloc&) {
    throw std::bad_alloc();
  } catch (const GraphError& error) {
    transaction.result().errors.push_back(
        {absolute_path, error.code(), error.what()});
    transaction.commit_result_only(result);
    return;
  } catch (const std::invalid_argument& error) {
    transaction.result().errors.push_back(
        {absolute_path, GraphErrc::InvalidParameter, error.what()});
    transaction.commit_result_only(result);
    return;
  } catch (const std::exception& e) {
    transaction.result().errors.push_back(
        {absolute_path, GraphErrc::Unknown, e.what()});
    transaction.commit_result_only(result);
    return;
  } catch (...) {
    transaction.result().errors.push_back(
        {absolute_path, GraphErrc::Unknown,
         "operation ABI registration failed with an unknown exception"});
    transaction.commit_result_only(result);
    return;
  }

#if defined(PHOTOSPIDER_INTERNAL_BAD_ALLOC_TESTING)
  maybe_fail_operation_plugin_bookkeeping(
      testing::OperationPluginLoadFailpoint::PreviousSource);
#endif
  auto previous_sources = collect_previous_sources(
      registration_capture.registered_keys, transaction.op_sources());
  auto retirement_entries =
      make_retirement_registry_entries(registration_capture.owned_entries);
#if defined(PHOTOSPIDER_INTERNAL_BAD_ALLOC_TESTING)
  maybe_fail_operation_plugin_bookkeeping(
      testing::OperationPluginLoadFailpoint::SourceAndResult);
#endif
  record_registered_operation_sources(
      absolute_path, registration_capture.registered_keys,
      transaction.op_sources(), transaction.result());

#if defined(PHOTOSPIDER_INTERNAL_BAD_ALLOC_TESTING)
  maybe_fail_operation_plugin_bookkeeping(
      testing::OperationPluginLoadFailpoint::Snapshot);
#endif
  LoadedOpPlugin loaded;
  loaded.library = transaction.library_lifetime();
  loaded.load_sequence = load_sequence;
  loaded.registered_keys = std::move(registration_capture.registered_keys);
  loaded.previous_registry_entries =
      std::move(registration_capture.previous_entries);
  loaded.owned_registry_entries = std::move(registration_capture.owned_entries);
  loaded.retirement_registry_entries = std::move(retirement_entries);
  loaded.previous_sources = std::move(previous_sources);
#if defined(PHOTOSPIDER_INTERNAL_BAD_ALLOC_TESTING)
  maybe_fail_operation_plugin_bookkeeping(
      testing::OperationPluginLoadFailpoint::HandleCommit);
#endif
  const bool inserted = transaction.loaded_plugins()
                            .emplace(absolute_path, std::move(loaded))
                            .second;
  if (!inserted) {
    throw std::logic_error("operation plugin transaction path already loaded");
  }
  ++transaction.result().loaded;
  transaction.commit_all(registry, op_sources, loaded_plugins, result);
}

}  // namespace

/** @copydoc load_plugins_for_process_owner */
PluginLoadResult load_plugins_for_process_owner(
    ProcessPluginOwnerToken owner_token,
    const std::vector<std::string>& plugin_dir_paths,
    std::map<std::string, std::string>& op_sources,
    LoadedOpPluginMap& loaded_plugins) {
  (void)owner_token;
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

#if defined(PHOTOSPIDER_INTERNAL_BAD_ALLOC_TESTING)
namespace testing {

/** @copydoc set_operation_plugin_load_failpoint */
void set_operation_plugin_load_failpoint(
    OperationPluginLoadFailpoint failpoint) noexcept {
  load_failpoint = failpoint;
  operation_plugin_load_failpoint_hit_count = 0;
}

/** @copydoc operation_plugin_load_failpoint_hits */
std::size_t operation_plugin_load_failpoint_hits() noexcept {
  return operation_plugin_load_failpoint_hit_count;
}

/** @copydoc load_one_operation_plugin_for_testing */
void load_one_operation_plugin_for_testing(
    const std::filesystem::path& path,
    std::map<std::string, std::string>& op_sources,
    LoadedOpPluginMap& loaded_plugins, PluginLoadResult& result) {
  load_one_plugin(path, op_sources, loaded_plugins, result);
}

}  // namespace testing
#endif

}  // namespace ps
