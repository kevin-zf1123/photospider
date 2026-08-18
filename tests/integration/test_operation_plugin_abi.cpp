#include <gtest/gtest.h>

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <future>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>
#include <variant>
#include <vector>

#if defined(_WIN32)
#include <windows.h>
#else
#include <dlfcn.h>
#endif

#include "core/host_output_authorization.hpp"  // NOLINT(build/include_subdir)
#include "core/ps_types.hpp"                   // NOLINT(build/include_subdir)
#include "graph/node.hpp"                      // NOLINT(build/include_subdir)
#include "plugin/operation_host_adapter.hpp"
#include "plugin/operation_runtime_router.hpp"

namespace ps {
namespace {

/** @brief Environment variable selecting the lifecycle fixture trace file. */
constexpr const char* kTraceEnvironment = "PS_LIFECYCLE_PLUGIN_TRACE";
/** @brief Environment variable selecting the callback release file. */
constexpr const char* kReleaseEnvironment =
    "PS_LIFECYCLE_PLUGIN_CALLBACK_RELEASE_FILE";
/** @brief Environment variable selecting one conformance mutation. */
constexpr const char* kConformanceModeEnvironment =
    "PS_OPERATION_CONFORMANCE_MODE";
/** @brief Environment variable tracing forbidden direct execution. */
constexpr const char* kConformanceTraceEnvironment =
    "PS_OPERATION_CONFORMANCE_TRACE";
/** @brief Runtime package identity declared by the conformance fixture. */
constexpr ps_operation_identity_v1 kConformanceRuntimePackage{
    0x5053434F4E465254ULL, 0x0001ULL};

/**
 * @brief Restores one process environment variable after a test scope.
 * @throws std::bad_alloc when preserving the previous string allocates.
 * @note Tests using this helper are registered serially because environment
 * mutation is process-global.
 */
class ScopedEnvironment final {
 public:
  /**
   * @brief Replaces one environment variable for the current process.
   * @param name Stable environment key.
   * @param value Replacement value.
   * @throws std::runtime_error when the platform environment update fails.
   */
  ScopedEnvironment(const char* name, std::string value)
      : name_(name),
        previous_(read(name)),
        had_previous_(previous_.has_value()) {
    write(name_.c_str(), value.c_str());
  }

  /** @brief Restores the previous value without surfacing cleanup failures. */
  ~ScopedEnvironment() noexcept {
    if (had_previous_) {
      write_noexcept(name_.c_str(), previous_->c_str());
    } else {
      erase_noexcept(name_.c_str());
    }
  }

  ScopedEnvironment(const ScopedEnvironment&) = delete;
  ScopedEnvironment& operator=(const ScopedEnvironment&) = delete;

 private:
  /**
   * @brief Reads one environment variable.
   * @param name Environment key.
   * @return Owned value or nullopt when absent.
   * @throws std::bad_alloc when copying the value fails.
   */
  static std::optional<std::string> read(const char* name) {
    const char* value = std::getenv(name);
    return value == nullptr ? std::nullopt : std::optional<std::string>(value);
  }

  /**
   * @brief Writes one environment variable or throws.
   * @param name Environment key.
   * @param value Replacement value.
   * @throws std::runtime_error when the platform call fails.
   */
  static void write(const char* name, const char* value) {
#if defined(_WIN32)
    if (_putenv_s(name, value) != 0) {
#else
    if (setenv(name, value, 1) != 0) {
#endif
      throw std::runtime_error("failed to set operation ABI test environment");
    }
  }

  /**
   * @brief Best-effort environment replacement during destruction.
   * @param name Environment key.
   * @param value Replacement value.
   * @return Nothing.
   * @throws Nothing.
   */
  static void write_noexcept(const char* name, const char* value) noexcept {
#if defined(_WIN32)
    (void)_putenv_s(name, value);
#else
    (void)setenv(name, value, 1);
#endif
  }

  /**
   * @brief Best-effort environment removal during destruction.
   * @param name Environment key.
   * @return Nothing.
   * @throws Nothing.
   */
  static void erase_noexcept(const char* name) noexcept {
#if defined(_WIN32)
    (void)_putenv_s(name, "");
#else
    (void)unsetenv(name);
#endif
  }

  /** @brief Owned environment key. */
  std::string name_;
  /** @brief Previous value when the key existed. */
  std::optional<std::string> previous_;
  /** @brief Whether `previous_` represents an existing variable. */
  bool had_previous_ = false;
};  // NOLINT(readability/braces)

/**
 * @brief Closes one direct-test native library handle.
 * @param handle Nonnull platform handle.
 * @return Nothing.
 * @throws Nothing; close errors cannot affect test-process cleanup.
 */
void close_library(void* handle) noexcept {
#if defined(_WIN32)
  (void)FreeLibrary(static_cast<HMODULE>(handle));
#else
  (void)dlclose(handle);
#endif
}

/**
 * @brief Opens the lifecycle DSO without invoking production trust admission.
 * @param path Exact build-tree fixture path.
 * @return Shared native handle whose final owner closes the library.
 * @throws std::runtime_error when the platform loader rejects the fixture.
 * @note This seam isolates ABI/lifecycle behavior on Darwin, where production
 * exact-object trust intentionally fails closed before `dlopen`.
 */
std::shared_ptr<void> open_library(const std::filesystem::path& path) {
#if defined(_WIN32)
  void* handle = static_cast<void*>(LoadLibraryA(path.string().c_str()));
#else
  void* handle = dlopen(path.c_str(), RTLD_NOW | RTLD_LOCAL);
#endif
  if (handle == nullptr) {
    throw std::runtime_error("failed to open lifecycle operation ABI fixture");
  }
  return std::shared_ptr<void>(handle, close_library);
}

/**
 * @brief Resolves one required exported discovery symbol.
 * @tparam Function Exact pure-C function-pointer type.
 * @param handle Live native-library handle.
 * @param name Exact exported symbol name.
 * @return Resolved function pointer.
 * @throws std::runtime_error when the symbol is absent.
 */
template <typename Function>
Function resolve_symbol(void* handle, const char* name) {
#if defined(_WIN32)
  auto* symbol = reinterpret_cast<void*>(
      GetProcAddress(static_cast<HMODULE>(handle), name));
#else
  void* symbol = dlsym(handle, name);
#endif
  if (symbol == nullptr) {
    throw std::runtime_error("missing operation ABI discovery symbol");
  }
  return reinterpret_cast<Function>(symbol);
}

/**
 * @brief Reads complete newline-delimited lifecycle events.
 * @param path Trace file path.
 * @return Events in observed order.
 * @throws std::bad_alloc or stream failures from owned storage.
 */
std::vector<std::string> read_trace(const std::filesystem::path& path) {
  std::ifstream input(path);
  std::vector<std::string> events;
  for (std::string line; std::getline(input, line);) {
    events.push_back(std::move(line));
  }
  return events;
}

/**
 * @brief Waits for one lifecycle event with a bounded deadline.
 * @param path Trace file path.
 * @param event Exact expected event.
 * @return True if observed before the deadline.
 * @throws std::bad_alloc or stream failures from trace reading.
 */
bool wait_for_event(const std::filesystem::path& path,
                    const std::string& event) {
  const auto deadline =
      std::chrono::steady_clock::now() + std::chrono::seconds(3);
  while (std::chrono::steady_clock::now() < deadline) {
    const auto events = read_trace(path);
    if (std::find(events.begin(), events.end(), event) != events.end()) {
      return true;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(2));
  }
  return false;
}

/**
 * @brief Returns a collision-resistant temporary path for one test artifact.
 * @param suffix Human-readable artifact suffix.
 * @return Path below the platform temporary directory.
 * @throws std::bad_alloc or filesystem errors.
 */
std::filesystem::path temporary_path(const char* suffix) {
  const auto stamp =
      std::chrono::steady_clock::now().time_since_epoch().count();
  return std::filesystem::temp_directory_path() /
         (std::string("photospider-operation-abi-") + std::to_string(stamp) +
          "-" + suffix);
}

/**
 * @brief Opens and validates one operation-v1 generation for focused tests.
 * @param path Exact build-tree DSO path.
 * @return Validated immutable generation retaining the native-library lease.
 * @throws Native loader, symbol-resolution, root, suite, or record-validation
 * failures unchanged.
 * @note This direct seam deliberately bypasses production trust only so Darwin
 * can execute portable ABI validation without claiming exact-object admission.
 */
std::shared_ptr<plugin_host::OperationPluginGeneration> load_generation(
    const std::filesystem::path& path) {
  std::shared_ptr<void> native = open_library(path);
  const auto get_version =
      resolve_symbol<ps_operation_plugin_get_abi_version_fn_v1>(
          native.get(), PS_OPERATION_PLUGIN_GET_ABI_VERSION_SYMBOL);
  const auto get_api = resolve_symbol<ps_operation_plugin_get_api_fn_v1>(
      native.get(), PS_OPERATION_PLUGIN_GET_API_V1_SYMBOL);
  return plugin_host::OperationPluginGeneration::create(std::move(native),
                                                        get_version, get_api);
}

/**
 * @brief Builds one 2-by-2 single-channel UINT8 output plan for tile routing.
 * @return Complete validated immutable plan named `image`.
 * @throws Plan metadata validation or allocation failures unchanged.
 * @note The helper creates metadata only; the caller separately owns the Host
 * binding and exact tile grant.
 */
DenseImageOutputPlan conformance_output_plan() {
  DenseTensorDescriptor descriptor{{2U, 2U, 1U},
                                   ElementSemantics::UnsignedInteger,
                                   StorageEncoding{8U}};
  ImageFacet image = make_zero_origin_image_facet(descriptor, 1U, 0U, 2U);
  StridedLayout layout{{2, 1, 1}};
  return DenseImageOutputPlan::create("image", std::move(descriptor),
                                      std::move(image), std::move(layout), 4U,
                                      64U);
}

/**
 * @brief Proves callback snapshots retain the DSO through in-flight unload.
 * @throws Nothing when discovery, execution, retirement, and assertions pass.
 * @note Production trust admission is intentionally outside this focused test;
 * Linux `PluginManager` tests cover that boundary with an immutable fd path.
 */
TEST(OperationPluginAbi, InFlightCallbackRetainsDsoAndDestroysOnce) {
  const std::filesystem::path plugin_path = PS_TEST_LIFECYCLE_OPERATION_PLUGIN;
  ASSERT_TRUE(std::filesystem::exists(plugin_path));
  const auto trace_path = temporary_path("trace.txt");
  const auto release_path = temporary_path("release.txt");
  std::filesystem::remove(trace_path);
  std::filesystem::remove(release_path);
  ScopedEnvironment trace_environment(kTraceEnvironment, trace_path.string());
  ScopedEnvironment release_environment(kReleaseEnvironment,
                                        release_path.string());

  std::shared_ptr<void> native = open_library(plugin_path);
  const auto get_version =
      resolve_symbol<ps_operation_plugin_get_abi_version_fn_v1>(
          native.get(), PS_OPERATION_PLUGIN_GET_ABI_VERSION_SYMBOL);
  const auto get_api = resolve_symbol<ps_operation_plugin_get_api_fn_v1>(
      native.get(), PS_OPERATION_PLUGIN_GET_API_V1_SYMBOL);
  auto generation = plugin_host::OperationPluginGeneration::create(
      native, get_version, get_api);
  native.reset();

  OpRegistry registry;
  generation->register_into(registry);
  auto resolved = registry.resolve_for_intent(
      "plugin_lifecycle", "op", ComputeIntent::GlobalHighPrecision);
  ASSERT_TRUE(resolved.has_value());
  ASSERT_TRUE(std::holds_alternative<MonolithicOpFunc>(*resolved));
  MonolithicOpFunc callback = std::move(std::get<MonolithicOpFunc>(*resolved));
  resolved.reset();
  ASSERT_TRUE(registry.unregister_key("plugin_lifecycle:op"));
  generation.reset();

  NodeOutput output;
  {
    auto invocation = std::async(std::launch::async,
                                 [callback = std::move(callback)]() mutable {
                                   Node node;
                                   node.id = 1;
                                   node.type = "plugin_lifecycle";
                                   node.subtype = "op";
                                   return callback(node, {});
                                 });
    callback = MonolithicOpFunc{};
    if (!wait_for_event(trace_path, "callback_enter")) {
      std::ofstream(release_path).put('\n');
      invocation.wait();
      FAIL() << "operation ABI callback did not enter the release barrier";
    }
    const auto before_release = read_trace(trace_path);
    EXPECT_EQ(std::find(before_release.begin(), before_release.end(),
                        "plugin_destroy"),
              before_release.end());
    EXPECT_EQ(std::find(before_release.begin(), before_release.end(),
                        "library_unload"),
              before_release.end());

    std::ofstream(release_path).put('\n');
    output = invocation.get();
  }
  EXPECT_EQ(output.debug.compute_device, "PLUGIN_LIFECYCLE_TEST");
  const auto before_output_release = read_trace(trace_path);
  EXPECT_EQ(std::find(before_output_release.begin(),
                      before_output_release.end(), "plugin_destroy"),
            before_output_release.end());
  output = NodeOutput{};
  ASSERT_TRUE(wait_for_event(trace_path, "library_unload"));
  const auto events = read_trace(trace_path);
  EXPECT_EQ(std::count(events.begin(), events.end(), "callback_destroy"), 1);
  EXPECT_EQ(std::count(events.begin(), events.end(), "plugin_destroy"), 1);
  EXPECT_EQ(std::count(events.begin(), events.end(), "library_unload"), 1);
  const auto callback_return =
      std::find(events.begin(), events.end(), "callback_return");
  const auto context_destroy =
      std::find(events.begin(), events.end(), "callback_destroy");
  const auto plugin_destroy =
      std::find(events.begin(), events.end(), "plugin_destroy");
  const auto library_unload =
      std::find(events.begin(), events.end(), "library_unload");
  ASSERT_NE(callback_return, events.end());
  ASSERT_NE(context_destroy, events.end());
  ASSERT_NE(plugin_destroy, events.end());
  ASSERT_NE(library_unload, events.end());
  EXPECT_LT(callback_return, context_destroy);
  EXPECT_LT(context_destroy, plugin_destroy);
  EXPECT_LT(plugin_destroy, library_unload);
  std::filesystem::remove(trace_path);
  std::filesystem::remove(release_path);
}

/**
 * @brief Proves a supervised operation has no direct-execution fallback.
 * @throws Nothing when the unavailable exact package route is rejected.
 * @note Route lookup precedes request validation deliberately: an absent
 * signed package cannot cause any callback, mapping, allocation, or worker
 * activity regardless of hostile or incomplete invocation contents.
 */
TEST(OperationPluginAbi, MissingSupervisedRuntimeRouteFailsClosed) {
  constexpr ps_operation_identity_v1 kMissingPackage{0x50534D495353494EULL,
                                                     0x47524F5554450001ULL};
  EXPECT_FALSE(
      plugin_host::remove_supervised_operation_runtime_route(kMissingPackage));
  execution::IsolatedCpuHostInvocation invocation;
  EXPECT_THROW((void)plugin_host::invoke_supervised_operation_runtime(
                   kMissingPackage, std::move(invocation)),
               std::invalid_argument);
}

/**
 * @brief Proves exact root, suite, count, tail, stride, and reserved
 * validation.
 * @throws Nothing when every hostile mode is rejected before publication.
 * @note Each mode is emitted by one real DSO through otherwise valid pure-C
 * discovery callbacks; no source scan substitutes for loader behavior.
 */
TEST(OperationPluginAbi, RejectsMalformedRootSuiteAndDefinitionRecords) {
  const std::filesystem::path plugin_path =
      PS_TEST_CONFORMANCE_OPERATION_PLUGIN;
  ASSERT_TRUE(std::filesystem::exists(plugin_path));
  constexpr const char* kModes[]{"root_reserved",          "suite_tail",
                                 "suite_reserved",         "count_bound",
                                 "operation_tail",         "output_stride",
                                 "implementation_reserved"};
  for (const char* mode : kModes) {
    SCOPED_TRACE(mode);
    ScopedEnvironment selected_mode(kConformanceModeEnvironment, mode);
    EXPECT_THROW((void)load_generation(plugin_path), std::invalid_argument);
  }
}

/**
 * @brief Proves a supervised tiled descriptor reaches the runtime router and
 * never its DSO execution callback when the signed route is absent.
 * @throws Nothing when registration, resolution, grant cleanup, and assertions
 * complete successfully.
 * @note Darwin exercises this pre-process fail-closed boundary without claiming
 * exact-object runtime construction; supported Linux gates cover positive
 * authenticated child execution and recovery.
 */
TEST(OperationPluginAbi, SupervisedTiledSelectionFailsClosedWithoutFallback) {
  const std::filesystem::path plugin_path =
      PS_TEST_CONFORMANCE_OPERATION_PLUGIN;
  ASSERT_TRUE(std::filesystem::exists(plugin_path));
  const std::filesystem::path trace_path = temporary_path("route-trace.txt");
  std::filesystem::remove(trace_path);
  ScopedEnvironment selected_mode(kConformanceModeEnvironment, "valid");
  ScopedEnvironment trace_environment(kConformanceTraceEnvironment,
                                      trace_path.string());
  EXPECT_FALSE(plugin_host::remove_supervised_operation_runtime_route(
      kConformanceRuntimePackage));

  auto generation = load_generation(plugin_path);
  OpRegistry registry;
  generation->register_into(registry);
  auto selected =
      registry.resolve_for_intent("operation_conformance", "supervised_tile",
                                  ComputeIntent::GlobalHighPrecision);
  ASSERT_TRUE(selected.has_value());
  ASSERT_TRUE(std::holds_alternative<TileOpFunc>(*selected));

  HostOutputBinding binding =
      HostOutputBinding::allocate(conformance_output_plan());
  HostOutputWriteGrant grant =
      binding.grant_tile({image_region_domain(), 0, 2, 0, 2});
  OutputTile output{&binding.plan(), &grant, PixelRect{0, 0, 2, 2}};
  Node node;
  node.type = "operation_conformance";
  node.subtype = "supervised_tile";
  EXPECT_THROW(std::get<TileOpFunc>(*selected)(node, output, {}),
               std::invalid_argument);
  ASSERT_TRUE(grant.active());
  grant.retire_failure("expected missing supervised runtime route");
  EXPECT_TRUE(read_trace(trace_path).empty());

  selected.reset();
  EXPECT_TRUE(registry.unregister_key("operation_conformance:supervised_tile"));
  generation.reset();
  std::filesystem::remove(trace_path);
}

}  // namespace
}  // namespace ps
