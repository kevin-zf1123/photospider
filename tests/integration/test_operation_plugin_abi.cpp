#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <future>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <unordered_map>
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
#include "graph/graph_model.hpp"               // NOLINT(build/include_subdir)
#include "graph/node.hpp"                      // NOLINT(build/include_subdir)
#include "graph/roi_propagation_service.hpp"   // NOLINT(build/include_subdir)
#include "photospider/plugin/operation_plugin.hpp"
#include "plugin/operation_host_adapter.hpp"
#include "plugin/operation_runtime_router.hpp"

namespace ps {
namespace {

/** @brief Environment variable selecting the lifecycle fixture trace file. */
constexpr const char* kTraceEnvironment = "PS_LIFECYCLE_PLUGIN_TRACE";
/** @brief Environment variable selecting the callback release file. */
constexpr const char* kReleaseEnvironment{
    "PS_LIFECYCLE_PLUGIN_CALLBACK_RELEASE_FILE",
};
/** @brief Environment variable selecting one conformance mutation. */
constexpr const char* kConformanceModeEnvironment{
    "PS_OPERATION_CONFORMANCE_MODE",
};
/** @brief Environment variable tracing forbidden direct execution. */
constexpr const char* kConformanceTraceEnvironment{
    "PS_OPERATION_CONFORMANCE_TRACE",
};
/** @brief Runtime package identity declared by the conformance fixture. */
constexpr ps_operation_identity_v1 kConformanceRuntimePackage{
    0x4142434445464748ULL,
    0x494A4B4C4D4E4F50ULL,
};

#if defined(__linux__) && defined(PS_TEST_ISOLATED_CPU_FIXTURE_PATH)
/**
 * @brief Creates one deterministic nonzero supervised identity component.
 * @param seed Final canonical byte, which must be nonzero in these tests.
 * @return Comparison-only opaque identity.
 * @throws Nothing.
 */
execution::IsolatedCpuOpaqueId conformance_supervised_id(
    std::uint8_t seed) noexcept {
  execution::IsolatedCpuOpaqueId identity;
  identity.bytes.back() = static_cast<std::byte>(seed);
  return identity;
}

/**
 * @brief Mints one complete fresh supervised invocation identity.
 * @return Nonzero caller/worker facts and a process-unique invocation id.
 * @throws Nothing.
 * @note The runtime router replaces package bytes and generation with the
 * signed executor facts before protocol validation.
 */
execution::IsolatedCpuInvocationIdentity
conformance_supervised_identity() noexcept {
  static std::atomic<std::uint8_t> next_invocation{80U};
  execution::IsolatedCpuInvocationIdentity identity;
  identity.tenant_id = conformance_supervised_id(1U);
  identity.job_id = conformance_supervised_id(2U);
  identity.attempt_id = conformance_supervised_id(3U);
  identity.worker_id = conformance_supervised_id(4U);
  identity.worker_lease_generation = 1U;
  identity.plugin_package_id = conformance_supervised_id(5U);
  identity.plugin_generation = 1U;
  identity.invocation_id =
      conformance_supervised_id(next_invocation.fetch_add(1U));
  return identity;
}

/**
 * @brief Creates isolated-runtime resource authority for conformance calls.
 * @return Fresh ledger sized for two sequential four-byte invocations.
 * @throws std::bad_alloc when ledger ownership cannot allocate.
 */
std::shared_ptr<ResourceLedger> conformance_resource_ledger() {
  return std::make_shared<ResourceLedger>(
      ResourceVector{}, std::vector<DeviceResourceLimit>{},
      PluginResourceVector{1U, 1U, 1ULL << 40U, 64ULL * 1024ULL * 1024ULL,
                           4096U});
}
#endif

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
 * @brief Removes one process-registry key when a focused test scope exits.
 * @throws Nothing.
 * @note The fixture owns only the named key and never clears unrelated
 * process-global registrations.
 */
class ScopedRegistryKeyCleanup final {
 public:
  /**
   * @brief Takes cleanup responsibility for one canonical registry key.
   * @param key Exact `type:subtype` key.
   * @throws std::bad_alloc when retaining the key cannot allocate.
   */
  explicit ScopedRegistryKeyCleanup(std::string key) : key_(std::move(key)) {}

  /** @brief Removes the owned key after all callback snapshots leave scope. */
  ~ScopedRegistryKeyCleanup() noexcept {
    static_cast<void>(OpRegistry::instance().unregister_key(key_));
  }

  ScopedRegistryKeyCleanup(const ScopedRegistryKeyCleanup&) = delete;
  ScopedRegistryKeyCleanup& operator=(const ScopedRegistryKeyCleanup&) = delete;

 private:
  /** @brief Canonical key removed at scope exit. */
  std::string key_;
};

/**
 * @brief Preallocates callback retirement storage for one captured key.
 * @param owned Exact slot revisions published by the captured generation.
 * @return Empty snapshot whose topology can receive every owned callback.
 * @throws std::bad_alloc when legacy or device placeholder storage allocates.
 * @note This focused helper mirrors the production loader's pre-publication
 * allocation so retirement and middle-generation splicing remain noexcept.
 */
OpRegistry::RegistryEntrySnapshot make_retirement_snapshot(
    const OpRegistry::RegistryEntryOwnership& owned) {
  OpRegistry::RegistryEntrySnapshot retirement;
  if (owned.legacy_op != 0U) {
    retirement.legacy_op.emplace(MonolithicOpFunc{});
  }
  if (owned.metadata != 0U) {
    retirement.metadata.emplace();
  }
  if (owned.monolithic_hp != 0U || owned.tiled_hp != 0U ||
      owned.tiled_rt != 0U || owned.dirty_propagator != 0U ||
      owned.forward_propagator != 0U || owned.dependency_builder != 0U ||
      owned.data_dependent != 0U || owned.device_impl_set != 0U ||
      !owned.device_impls.empty()) {
    retirement.implementations.emplace();
    if (owned.device_impl_set == 0U) {
      retirement.implementations->device_impl_slots.resize(
          owned.device_impls.size());
    }
  }
  return retirement;
}

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
 * @brief Invokes the active lifecycle callback in one private registry.
 * @param registry Registry containing exactly one selected lifecycle slot.
 * @return Diagnostic marker emitted by the selected fixture generation.
 * @throws std::runtime_error when no monolithic lifecycle callback is active.
 * @throws Callback or allocation failures unchanged.
 */
std::string invoke_lifecycle_marker(OpRegistry& registry) {
  auto resolved = registry.resolve_for_intent(
      "plugin_lifecycle", "op", ComputeIntent::GlobalHighPrecision);
  if (!resolved || !std::holds_alternative<MonolithicOpFunc>(*resolved)) {
    throw std::runtime_error("lifecycle generation is not monolithic");
  }
  Node node;
  node.id = 1;
  node.type = "plugin_lifecycle";
  node.subtype = "op";
  return std::get<MonolithicOpFunc>(*resolved)(node, {}).debug.compute_device;
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
 * @brief Publishes one ordinary fallback input for conformance invocations.
 * @return Ready two-by-two UINT8 image with no operation-specific metadata.
 * @throws Value validation, allocation, or publication failures unchanged.
 * @note Dedicated input-metadata tests replace this fallback with a preceding
 * conformance output; ordinary route tests only need a connected input slot.
 */
NodeOutput conformance_fallback_input() {
  DenseTensorDescriptor descriptor{{2U, 2U, 1U},
                                   ElementSemantics::UnsignedInteger,
                                   StorageEncoding{8U}};
  ImageFacet image = make_zero_origin_image_facet(descriptor, 1U, 0U, 2U);
  StridedLayout layout{{2, 1, 1}};
  NodeOutput output;
  output.publish_image_value(Value::from_cpu_dense_tensor(
      std::move(descriptor), std::move(image), std::move(layout),
      std::vector<std::byte>(4U, std::byte{0x11})));
  return output;
}

/**
 * @brief Publishes one whole-byte image for pass-through alignment admission.
 * @param semantics Supported integer or floating-point element semantics.
 * @param bit_width Supported whole-byte native-scalar physical width.
 * @return Ready two-by-two single-channel image with a tight positive layout.
 * @throws Descriptor, allocation, or publication failures unchanged.
 * @note The source binding may be more strongly aligned than its element;
 *       the output plan must request the descriptor's exact minimum alignment.
 */
NodeOutput conformance_passthrough_input(ElementSemantics semantics,
                                         std::uint32_t bit_width) {
  DenseTensorDescriptor descriptor{{2U, 2U, 1U},
                                   semantics,
                                   StorageEncoding{bit_width}};
  const std::size_t element_bytes = dense_tensor_element_bytes(descriptor);
  ImageFacet image = make_zero_origin_image_facet(descriptor, 1U, 0U, 2U);
  StridedLayout layout{{static_cast<std::ptrdiff_t>(2U * element_bytes),
                        static_cast<std::ptrdiff_t>(element_bytes),
                        static_cast<std::ptrdiff_t>(element_bytes)}};
  NodeOutput output;
  output.publish_image_value(Value::from_cpu_dense_tensor(
      std::move(descriptor), std::move(image), std::move(layout),
      std::vector<std::byte>(4U * element_bytes, std::byte{0x11})));
  return output;
}

/**
 * @brief Publishes one ordinary facet-free fallback DenseTensor input.
 * @return Ready two-by-three UINT8 tensor with no retained operation metadata.
 * @throws Value validation, allocation, or publication failures unchanged.
 * @note This ordinary publisher proves the adapter derives only the public
 * built-in Schema/Layout facts when no retained publisher record exists.
 */
NodeOutput conformance_generic_fallback_input() {
  DenseTensorDescriptor descriptor{{2U, 3U},
                                   ElementSemantics::UnsignedInteger,
                                   StorageEncoding{8U}};
  StridedLayout layout{{3, 1}};
  NodeOutput output;
  output.publish_named_value(
      "tensor", Value::from_cpu_dense_tensor(
                    std::move(descriptor), std::nullopt, std::move(layout),
                    std::vector<std::byte>(6U, std::byte{0x11})));
  return output;
}

/**
 * @brief Reports whether one fixture mode contains a stable feature token.
 * @param mode Nonnull fixture mode bytes.
 * @param token Nonnull token to search for.
 * @return True when token occurs in mode.
 * @throws Nothing.
 */
bool conformance_mode_contains(const char* mode, const char* token) noexcept {
  return mode != nullptr && token != nullptr &&
         std::strstr(mode, token) != nullptr;
}

/**
 * @brief Asserts one facet-free conformance output retained every publisher
 * fact.
 * @param output Operation result expected to contain exactly `tensor`.
 * @return Nothing; GoogleTest records representation or payload mismatches.
 * @throws DenseTensor view/access failures when the published Value is invalid.
 */
void expect_conformance_generic_output(const NodeOutput& output) {
  ASSERT_EQ(output.named_values.size(), 1U);
  const auto found = output.named_values.find("tensor");
  ASSERT_NE(found, output.named_values.end());
  const Value& value = found->second;
  EXPECT_FALSE(value.image_facet().has_value());
  EXPECT_EQ(value.dense_tensor_descriptor().shape,
            (std::vector<std::size_t>{2U, 3U}));
  EXPECT_EQ(value.strided_layout().byte_strides,
            (std::vector<std::ptrdiff_t>{3, 1}));
  const auto* metadata = DenseTensorValueDescriptorMetadataAccess::get(value);
  ASSERT_NE(metadata, nullptr);
  EXPECT_EQ(metadata->schema_identity,
            (ExtensionIdentity{
                PS_OPERATION_BUILTIN_DENSE_TENSOR_SCHEMA_IDENTITY_WORD0_V1,
                PS_OPERATION_BUILTIN_DENSE_TENSOR_SCHEMA_IDENTITY_WORD1_V1}));
  EXPECT_EQ(metadata->facet_identity, ExtensionIdentity{});
  EXPECT_EQ(metadata->layout_identity,
            (ExtensionIdentity{
                PS_OPERATION_BUILTIN_STRIDED_LAYOUT_IDENTITY_WORD0_V1,
                PS_OPERATION_BUILTIN_STRIDED_LAYOUT_IDENTITY_WORD1_V1}));
  EXPECT_EQ(metadata->descriptor_version, 7U);
  EXPECT_EQ(metadata->layout_version, 11U);
  EXPECT_EQ(metadata->descriptor_digest,
            (std::array<std::uint64_t, 4U>{0x0102030405060708ULL, 0U, 0U,
                                           0x1112131415161718ULL}));
  EXPECT_EQ(metadata->content_digest,
            (std::array<std::uint64_t, 4U>{0U, 0x2122232425262728ULL, 0U, 0U}));
  EXPECT_EQ(metadata->layout_digest,
            (std::array<std::uint64_t, 4U>{0U, 0U, 0x3132333435363738ULL, 0U}));
  const DenseTensorView view(value);
  ASSERT_EQ(view.storage_size(), 6U);
  for (std::size_t index = 0U; index < view.storage_size(); ++index) {
    EXPECT_EQ(view.data()[index], std::byte{0x5A});
  }
}

/**
 * @brief Executes one conformance implementation in monolithic shape.
 * @param mode Fixture mode selected before DSO discovery.
 * @param input Optional exact upstream output; null selects a fallback Value.
 * @param parameters Optional exact effective configuration copied into the
 * node.
 * @return Fresh output after complete Host retirement and publication.
 * @throws Discovery, inference, callback, validation, or publication failures
 * unchanged.
 */
NodeOutput execute_conformance_monolithic(
    const char* mode, const NodeOutput* input = nullptr,
    const plugin::ParameterMap* parameters = nullptr) {
  ScopedEnvironment selected_mode(kConformanceModeEnvironment, mode);
  auto generation = load_generation(PS_TEST_CONFORMANCE_OPERATION_PLUGIN);
  OpRegistry registry;
  generation->register_into(registry);
  auto selected =
      registry.resolve_for_intent("operation_conformance", "supervised_tile",
                                  ComputeIntent::GlobalHighPrecision);
  if (!selected.has_value() ||
      !std::holds_alternative<MonolithicOpFunc>(*selected)) {
    throw std::runtime_error(
        "conformance fixture did not publish a monolithic callback");
  }
  Node node;
  node.type = "operation_conformance";
  node.subtype = "supervised_tile";
  if (parameters != nullptr) {
    node.parameters = *parameters;
  }
  NodeOutput fallback;
  if (input == nullptr) {
    fallback = conformance_mode_contains(mode, "facet_free_input")
                   ? conformance_generic_fallback_input()
                   : conformance_fallback_input();
    input = &fallback;
  }
  return std::get<MonolithicOpFunc>(*selected)(node, {input});
}

/**
 * @brief Executes one conformance implementation against a whole-image tile.
 * @param mode Fixture mode selected before DSO discovery.
 * @param input Optional exact upstream output; null selects a fallback Value.
 * @param parameters Optional exact effective configuration copied into the
 * node.
 * @return Fresh sealed output after callback completion and grant retirement.
 * @throws Discovery, inference, callback, or validation failures unchanged.
 * @note Failure cleanup retires the still-active borrowed tile grant before
 * rethrowing so hostile callbacks cannot leak test-owned authority.
 */
NodeOutput execute_conformance_tiled(
    const char* mode, const NodeOutput* input = nullptr,
    const plugin::ParameterMap* parameters = nullptr) {
  ScopedEnvironment selected_mode(kConformanceModeEnvironment, mode);
  auto generation = load_generation(PS_TEST_CONFORMANCE_OPERATION_PLUGIN);
  OpRegistry registry;
  generation->register_into(registry);
  auto selected =
      registry.resolve_for_intent("operation_conformance", "supervised_tile",
                                  ComputeIntent::GlobalHighPrecision);
  if (!selected.has_value() || !std::holds_alternative<TileOpFunc>(*selected)) {
    throw std::runtime_error(
        "conformance fixture did not publish a tiled callback");
  }
  HostOutputBinding binding =
      HostOutputBinding::allocate(conformance_output_plan());
  HostOutputWriteGrant grant =
      binding.grant_tile({image_region_domain(), 0, 2, 0, 2});
  OutputTile output{&binding.plan(), &grant, PixelRect{0, 0, 2, 2}};
  Node node;
  node.type = "operation_conformance";
  node.subtype = "supervised_tile";
  if (parameters != nullptr) {
    node.parameters = *parameters;
  }
  NodeOutput fallback;
  if (input == nullptr) {
    fallback = conformance_mode_contains(mode, "facet_free_input")
                   ? conformance_generic_fallback_input()
                   : conformance_fallback_input();
    input = &fallback;
  }
  const Value* input_value = nullptr;
  PixelRect input_roi{};
  if (conformance_mode_contains(mode, "facet_free_input")) {
    const auto found = input->named_values.find("tensor");
    if (found == input->named_values.end()) {
      throw std::runtime_error("conformance generic input is absent");
    }
    input_value = &found->second;
  } else {
    input_value = &input->image_value();
    input_roi = PixelRect{0, 0, 2, 2};
  }
  InputTile input_tile{input_value, input_roi, nullptr};
  try {
    std::get<TileOpFunc> (*selected)(node, output, {input_tile});
    grant.retire_success();
  } catch (...) {
    if (grant.active()) {
      grant.retire_failure("expected conformance callback failure");
    }
    throw;
  }
  NodeOutput result;
  result.plugin_library_lifetime = generation;
  result.publish_image_value(binding.seal());
  return result;
}

/**
 * @brief Captures one invalid-argument diagnostic from a conformance call.
 * @tparam Callback Nullary callable expected to fail.
 * @param callback Test body.
 * @return Host-owned diagnostic text.
 * @throws Any non-invalid-argument failure unchanged.
 */
template <typename Callback>
std::string invalid_argument_message(Callback&& callback) {
  try {
    std::forward<Callback>(callback)();
  } catch (const std::invalid_argument& error) {
    return error.what();
  }
  throw std::runtime_error("expected std::invalid_argument was not thrown");
}

/**
 * @brief Proves empty C++ byte views are canonical before Host generation
 * validation.
 * @throws Nothing; GoogleTest captures an unexpected loader exception.
 * @note The fixture publishes an explicitly nonnull, zero-length
 * `implementation_version` through generated `get_api`; the real Host
 * generation loader then exercises strict `copy_bytes` validation.
 */
TEST(OperationPluginAbi, CanonicalizesEmptyCppViewsBeforeHostGenerationLoad) {
  constexpr std::uint8_t kNonNullByteStorage{0U};
  const auto pointer_view =
      operation_plugin::make_bytes(&kNonNullByteStorage, 0U);
  EXPECT_EQ(pointer_view.data, nullptr);
  EXPECT_EQ(pointer_view.size, 0U);

  constexpr char kNonNullTextStorage[] = "nonempty backing storage";
  const std::string_view empty_text{kNonNullTextStorage, 0U};
  ASSERT_NE(empty_text.data(), nullptr);
  const auto string_view = operation_plugin::make_bytes(empty_text);
  EXPECT_EQ(string_view.data, nullptr);
  EXPECT_EQ(string_view.size, 0U);

  const std::filesystem::path plugin_path = PS_TEST_LIFECYCLE_OPERATION_PLUGIN;
  ASSERT_TRUE(std::filesystem::exists(plugin_path));
  std::shared_ptr<plugin_host::OperationPluginGeneration> generation;
  EXPECT_NO_THROW(generation = load_generation(plugin_path));
  EXPECT_NE(generation, nullptr);
}

/**
 * @brief Proves the public pass-through helper derives whole-byte alignment
 * before real Host output-plan admission.
 * @throws Nothing when every descriptor crosses inference, allocation,
 * execution, retirement, and immutable publication successfully.
 * @note FP32 and FP64 are the P1 regression cases. Integer widths lock the
 * same storage-encoding derivation without replacing Host validation.
 */
TEST(OperationPluginAbi,
     PassthroughCppHelperDerivesWholeByteAlignmentBeforeHostAdmission) {
  /** @brief One supported logical-semantics and physical-width combination. */
  struct ElementCase final {
    /** @brief Logical interpretation used by Host descriptor validation. */
    ElementSemantics semantics;
    /** @brief Native-scalar physical width expressed in bits. */
    std::uint32_t bit_width;
  };
  constexpr std::array<ElementCase, 10U> kCases{{
      {ElementSemantics::FloatingPoint, 32U},
      {ElementSemantics::FloatingPoint, 64U},
      {ElementSemantics::UnsignedInteger, 8U},
      {ElementSemantics::UnsignedInteger, 16U},
      {ElementSemantics::UnsignedInteger, 32U},
      {ElementSemantics::UnsignedInteger, 64U},
      {ElementSemantics::SignedInteger, 8U},
      {ElementSemantics::SignedInteger, 16U},
      {ElementSemantics::SignedInteger, 32U},
      {ElementSemantics::SignedInteger, 64U},
  }};
  for (const ElementCase& element : kCases) {
    SCOPED_TRACE(element.bit_width);
    NodeOutput input =
        conformance_passthrough_input(element.semantics, element.bit_width);
    NodeOutput output;
    EXPECT_NO_THROW(output = execute_conformance_monolithic(
                        "trusted_monolithic_passthrough_alignment", &input));
    ASSERT_TRUE(output.has_image_value());
    const Value& value = output.image_value();
    EXPECT_EQ(value.dense_tensor_descriptor().element_semantics,
              element.semantics);
    EXPECT_EQ(value.dense_tensor_descriptor().storage_encoding.bit_width,
              element.bit_width);
    EXPECT_EQ(value.storage_binding().required_alignment,
              dense_tensor_element_bytes(value.dense_tensor_descriptor()));
  }
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
 * @brief Proves a later pure-C generation replaces one executable slot.
 * @throws Discovery, capture, callback, or restoration failures unchanged.
 * @note The direct seam is portable and bypasses production trust. Process-
 * owner middle-generation splice and lease retirement remain covered by the
 * Linux PluginManager integration suite.
 */
TEST(OperationPluginAbi, LaterGenerationReplacesAndRestoresExecutableSlot) {
  auto predecessor = load_generation(PS_TEST_LIFECYCLE_OPERATION_PLUGIN);
  auto replacement = load_generation(PS_TEST_OVERRIDE_OPERATION_PLUGIN);
  OpRegistry registry;
  OpRegistry::RegistrationCapture predecessor_capture;
  registry.capture_registration([&]() { predecessor->register_into(registry); },
                                predecessor_capture);
  EXPECT_EQ(invoke_lifecycle_marker(registry), "PLUGIN_LIFECYCLE_TEST");

  OpRegistry::RegistrationCapture replacement_capture;
  registry.capture_registration([&]() { replacement->register_into(registry); },
                                replacement_capture);
  EXPECT_EQ(invoke_lifecycle_marker(registry), "PLUGIN_OVERRIDE_TEST");

  registry.restore_registration_capture(replacement_capture);
  EXPECT_EQ(invoke_lifecycle_marker(registry), "PLUGIN_LIFECYCLE_TEST");
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
 * @brief Proves output descriptor identities must match the declared port in
 * every execution mode and shape before grants or supervised routing.
 * @throws Nothing when all four fail-closed boundaries reject the fixture.
 */
TEST(OperationPluginAbi,
     RejectsOutputDescriptorIdentityMismatchInEveryRouteAndShape) {
  EXPECT_THROW((void)execute_conformance_monolithic(
                   "trusted_monolithic_identity_mismatch"),
               std::invalid_argument);
  EXPECT_THROW(execute_conformance_tiled("trusted_tiled_identity_mismatch"),
               std::invalid_argument);

  const std::string supervised_monolithic = invalid_argument_message([]() {
    (void)execute_conformance_monolithic(
        "supervised_monolithic_identity_mismatch");
  });
  EXPECT_NE(supervised_monolithic.find("descriptor identity"),
            std::string::npos);
  const std::string supervised_tiled = invalid_argument_message([]() {
    execute_conformance_tiled("supervised_tiled_identity_mismatch");
  });
  EXPECT_NE(supervised_tiled.find("descriptor identity"), std::string::npos);
}

/**
 * @brief Proves trusted monolithic and tiled callbacks receive the exact
 * non-default versions and three digests accepted during inference.
 * @throws Nothing when both callbacks validate and fulfill their grants.
 */
TEST(OperationPluginAbi, TrustedExecutionEchoesExactDescriptorMetadata) {
  EXPECT_NO_THROW(
      (void)execute_conformance_monolithic("trusted_monolithic_metadata"));
  EXPECT_NO_THROW(execute_conformance_tiled("trusted_tiled_metadata"));
}

/**
 * @brief Proves generic descriptor authority survives publication and reuse.
 * @throws Discovery, execution, Value, and assertion failures unchanged.
 * @note The first call exercises the ordinary public built-in identity
 * fallback. The second consumes a retained generic output monolithically; the
 * third consumes the same Value through a tiled image-producing callback.
 */
TEST(OperationPluginAbi, TrustedGenericOutputBecomesMonolithicAndTiledInput) {
  const NodeOutput ordinary = execute_conformance_monolithic(
      "trusted_monolithic_facet_free_input_facet_free_output");
  expect_conformance_generic_output(ordinary);

  const NodeOutput source =
      execute_conformance_monolithic("trusted_monolithic_facet_free_output");
  expect_conformance_generic_output(source);
  const NodeOutput consumed = execute_conformance_monolithic(
      "trusted_monolithic_facet_free_input_facet_free_output_input_metadata",
      &source);
  expect_conformance_generic_output(consumed);
  EXPECT_NO_THROW(execute_conformance_tiled(
      "trusted_tiled_facet_free_input_input_metadata", &source));
}

/**
 * @brief Proves one public generation publishes and plans its full candidate
 * set.
 * @throws Discovery, registry, graph, callback, and assertion failures
 * unchanged.
 * @note The fixture expands five public rows into eight intent/shape
 * candidates. HP selects the data-dependent fast monolithic row; RT selects the
 * fast tiled row. Backward, forward, and dependency markers must come from
 * those exact selected rows rather than the definition's first implementation.
 */
TEST(OperationPluginAbi, CompleteCandidateSetDrivesIntentCostAndPlanning) {
  ScopedEnvironment selected_mode(kConformanceModeEnvironment, "candidate_set");
  auto generation = load_generation(PS_TEST_CONFORMANCE_OPERATION_PLUGIN);
  auto& registry = OpRegistry::instance();
  constexpr const char* kKey = "operation_conformance:supervised_tile";
  static_cast<void>(registry.unregister_key(kKey));
  ScopedRegistryKeyCleanup cleanup(kKey);
  registry.register_dependency_builder(
      "operation_conformance", "supervised_tile",
      [](const Node&, const GraphModel&, const std::vector<PixelSize>&,
         const PixelSize& output_extent, const plugin::ParameterMap&) {
        SpatialDependencyMap result;
        result.grid_size_x = output_extent.width;
        result.grid_size_y = output_extent.height;
        result.cols = 1;
        result.rows = 1;
        result.output_extent = output_extent;
        result.upstream_input_index = 0U;
        result.cell_to_upstream_roi.push_back(PixelRect{1, 1, 1, 1});
        return result;
      },
      false);
  generation->register_into(registry);

  const std::vector<OpImplementation> candidates =
      registry.get_all_implementations("operation_conformance",
                                       "supervised_tile");
  ASSERT_EQ(candidates.size(), 8U);
  EXPECT_EQ(std::count_if(candidates.begin(), candidates.end(),
                          [](const OpImplementation& candidate) {
                            return candidate.metadata.supports_high_precision;
                          }),
            4);
  EXPECT_EQ(std::count_if(candidates.begin(), candidates.end(),
                          [](const OpImplementation& candidate) {
                            return candidate.metadata.supports_realtime;
                          }),
            4);
  EXPECT_EQ(std::count_if(candidates.begin(), candidates.end(),
                          [](const OpImplementation& candidate) {
                            return candidate.is_monolithic();
                          }),
            4);
  EXPECT_EQ(std::count_if(candidates.begin(), candidates.end(),
                          [](const OpImplementation& candidate) {
                            return candidate.is_tiled();
                          }),
            4);

  const auto hp = registry.select_implementation(
      "operation_conformance", "supervised_tile", {DeviceBackend::CPU},
      ComputeIntent::GlobalHighPrecision);
  const auto rt = registry.select_implementation(
      "operation_conformance", "supervised_tile", {DeviceBackend::CPU},
      ComputeIntent::RealTimeUpdate);
  ASSERT_TRUE(hp.has_value());
  ASSERT_TRUE(rt.has_value());
  EXPECT_TRUE(hp->is_monolithic());
  EXPECT_EQ(hp->metadata.cost_score, 100);
  EXPECT_TRUE(hp->metadata.data_dependent);
  EXPECT_TRUE(hp->dirty_propagator.has_value());
  EXPECT_TRUE(hp->forward_propagator.has_value());
  EXPECT_TRUE(hp->dependency_builder.has_value());
  EXPECT_TRUE(rt->is_tiled());
  EXPECT_EQ(rt->metadata.cost_score, 100);
  EXPECT_FALSE(rt->metadata.data_dependent);
  EXPECT_NE(hp->implementation_identity, rt->implementation_identity);

  Node source;
  source.id = 1;
  source.type = "candidate_source";
  source.subtype = "cached";
  source.cached_output_high_precision = conformance_fallback_input();
  Node child;
  child.id = 2;
  child.type = "operation_conformance";
  child.subtype = "supervised_tile";
  child.image_inputs.push_back({1, "image"});
  GraphModel graph("");
  graph.add_node(std::move(source));
  graph.add_node(std::move(child));
  graph.validate_topology();

  std::unordered_map<int, PixelSize> hp_sizes;
  RoiPropagationService hp_propagation({DeviceBackend::CPU},
                                       ComputeIntent::GlobalHighPrecision);
  const UpstreamRoiProjection hp_upstream =
      hp_propagation.compute_upstream_projection(
          graph.node(2), PixelRect{0, 0, 2, 2}, graph, hp_sizes);
  EXPECT_EQ(hp_upstream.shared_roi, (PixelRect{1, 0, 1, 1}));
  ASSERT_TRUE(hp_upstream.dependency_input_index.has_value());
  EXPECT_EQ(*hp_upstream.dependency_input_index, 0U);
  EXPECT_EQ(hp_upstream.dependency_roi, (PixelRect{0, 1, 1, 1}));
  const auto hp_forward =
      hp_propagation.project_roi_forward(graph, 1, PixelRect{0, 0, 1, 1}, 2);
  ASSERT_TRUE(hp_forward.has_value());
  EXPECT_EQ(*hp_forward, (PixelRect{1, 0, 1, 1}));

  std::unordered_map<int, PixelSize> rt_sizes;
  RoiPropagationService rt_propagation({DeviceBackend::CPU},
                                       ComputeIntent::RealTimeUpdate);
  const UpstreamRoiProjection rt_upstream =
      rt_propagation.compute_upstream_projection(
          graph.node(2), PixelRect{0, 0, 2, 2}, graph, rt_sizes);
  EXPECT_EQ(rt_upstream.shared_roi, (PixelRect{0, 1, 1, 1}));
  EXPECT_FALSE(rt_upstream.dependency_input_index.has_value());
  const auto rt_forward =
      rt_propagation.project_roi_forward(graph, 1, PixelRect{0, 0, 1, 1}, 2);
  ASSERT_TRUE(rt_forward.has_value());
  EXPECT_EQ(*rt_forward, (PixelRect{0, 1, 1, 1}));
}

/**
 * @brief Proves whole candidate generations restore across middle removal.
 * @throws Discovery, capture, restoration, and assertion failures unchanged.
 * @note The test drives the same preallocated retire/splice primitives used by
 * PluginManager. Removing the middle one-row generation while the third
 * eight-row generation is active must splice its predecessor. Removing the
 * third then restores the first generation's exact eight identities, and final
 * reverse removal leaves no candidate behind.
 */
TEST(OperationPluginAbi, WholeCandidateGenerationsRestoreAcrossMiddleUnload) {
  std::shared_ptr<plugin_host::OperationPluginGeneration> first;
  std::shared_ptr<plugin_host::OperationPluginGeneration> middle;
  std::shared_ptr<plugin_host::OperationPluginGeneration> third;
  {
    ScopedEnvironment selected_mode(kConformanceModeEnvironment,
                                    "candidate_set");
    first = load_generation(PS_TEST_CONFORMANCE_OPERATION_PLUGIN);
  }
  {
    ScopedEnvironment selected_mode(kConformanceModeEnvironment, "valid");
    middle = load_generation(PS_TEST_CONFORMANCE_OPERATION_PLUGIN);
  }
  {
    ScopedEnvironment selected_mode(kConformanceModeEnvironment,
                                    "candidate_set");
    third = load_generation(PS_TEST_CONFORMANCE_OPERATION_PLUGIN);
  }

  OpRegistry registry;
  OpRegistry::RegistrationCapture first_capture;
  OpRegistry::RegistrationCapture middle_capture;
  OpRegistry::RegistrationCapture third_capture;
  registry.capture_registration([&]() { first->register_into(registry); },
                                first_capture);
  const std::vector<OpImplementation> first_candidates =
      registry.get_all_implementations("operation_conformance",
                                       "supervised_tile");
  ASSERT_EQ(first_candidates.size(), 8U);

  registry.capture_registration([&]() { middle->register_into(registry); },
                                middle_capture);
  ASSERT_EQ(
      registry
          .get_all_implementations("operation_conformance", "supervised_tile")
          .size(),
      1U);
  registry.capture_registration([&]() { third->register_into(registry); },
                                third_capture);
  const std::vector<OpImplementation> third_candidates =
      registry.get_all_implementations("operation_conformance",
                                       "supervised_tile");
  ASSERT_EQ(third_candidates.size(), 8U);
  EXPECT_NE(third_candidates.front().implementation_identity,
            first_candidates.front().implementation_identity);

  constexpr const char* kKey = "operation_conformance:supervised_tile";
  auto& middle_owned = middle_capture.owned_entries.at(kKey);
  auto& middle_previous = middle_capture.previous_entries.at(kKey);
  auto middle_retirement = make_retirement_snapshot(middle_owned);
  EXPECT_FALSE(registry.retire_owned_entry_noexcept(
      kKey, middle_owned, middle_previous, middle_retirement));
  auto& third_previous = third_capture.previous_entries.at(kKey);
  OpRegistry::splice_owned_snapshot_noexcept(
      third_previous, middle_owned, middle_previous, middle_retirement);
  const std::vector<OpImplementation> after_middle =
      registry.get_all_implementations("operation_conformance",
                                       "supervised_tile");
  ASSERT_EQ(after_middle.size(), 8U);
  EXPECT_EQ(after_middle.front().implementation_identity,
            third_candidates.front().implementation_identity);

  auto& third_owned = third_capture.owned_entries.at(kKey);
  auto third_retirement = make_retirement_snapshot(third_owned);
  EXPECT_TRUE(registry.retire_owned_entry_noexcept(
      kKey, third_owned, third_previous, third_retirement));
  const std::vector<OpImplementation> restored =
      registry.get_all_implementations("operation_conformance",
                                       "supervised_tile");
  ASSERT_EQ(restored.size(), 8U);
  for (std::size_t index = 0U; index < restored.size(); ++index) {
    EXPECT_EQ(restored[index].implementation_identity,
              first_candidates[index].implementation_identity);
  }

  auto& first_owned = first_capture.owned_entries.at(kKey);
  auto& first_previous = first_capture.previous_entries.at(kKey);
  auto first_retirement = make_retirement_snapshot(first_owned);
  EXPECT_TRUE(registry.retire_owned_entry_noexcept(
      kKey, first_owned, first_previous, first_retirement));
  EXPECT_TRUE(
      registry
          .get_all_implementations("operation_conformance", "supervised_tile")
          .empty());
}

/**
 * @brief Rejects consumer-declared custom publisher identity for ordinary
 * Values in every execution mode and shape.
 * @throws Nothing when all four routes fail at the shared input projection.
 * @note A Value with retained operation metadata may carry a matching custom
 * identity; this test deliberately supplies an ordinary Host-built image with
 * no retained publisher record.
 */
TEST(OperationPluginAbi,
     OrdinaryValuesRejectConsumerDeclaredIdentityInEveryRouteAndShape) {
  constexpr const char* kModes[]{
      "trusted_monolithic_custom_input_identity",
      "trusted_tiled_custom_input_identity",
      "supervised_monolithic_custom_input_identity",
      "supervised_tiled_custom_input_identity",
  };
  for (const char* mode : kModes) {
    SCOPED_TRACE(mode);
    const std::string diagnostic = invalid_argument_message([&]() {
      if (std::string_view(mode).find("monolithic") != std::string_view::npos) {
        (void)execute_conformance_monolithic(mode);
      } else {
        execute_conformance_tiled(mode);
      }
    });
    EXPECT_NE(diagnostic.find("built-in publisher identity"),
              std::string::npos);
  }
}

/**
 * @brief Proves one operation output preserves exact descriptor metadata when
 * consumed by the next trusted monolithic or tiled invocation.
 * @throws Nothing when both publication-to-input routes retain every field.
 */
TEST(OperationPluginAbi, TrustedValuesPreserveExactInputDescriptorMetadata) {
  const NodeOutput monolithic =
      execute_conformance_monolithic("trusted_monolithic_metadata");
  EXPECT_NO_THROW((void)execute_conformance_monolithic(
      "trusted_monolithic_input_metadata", &monolithic));

  const NodeOutput tiled = execute_conformance_tiled("trusted_tiled_metadata");
  EXPECT_NO_THROW(
      (void)execute_conformance_tiled("trusted_tiled_input_metadata", &tiled));
}

/**
 * @brief Proves supervised monolithic and tiled execution carry exact
 * non-default descriptor identities, versions, and digests through fresh exec.
 * @throws Standard trust, routing, protocol, execution, or assertion failures.
 * @note Exact-object positive execution is Linux-only; portable tests still
 * cover pre-route inference rejection and trusted exact echo on other hosts.
 * The supervised route also serializes both an empty root configuration and
 * nested empty object/array values, requiring canonical zero child offsets.
 */
TEST(OperationPluginAbi, SupervisedExecutionEchoesExactDescriptorMetadata) {
#if defined(__linux__) && defined(PS_TEST_ISOLATED_CPU_FIXTURE_PATH)
  EXPECT_FALSE(plugin_host::remove_supervised_operation_runtime_route(
      kConformanceRuntimePackage));
  auto executor = std::make_shared<execution::PluginInvocationExecutor>(
      std::filesystem::path(PS_TEST_ISOLATED_CPU_FIXTURE_PATH),
      conformance_resource_ledger());
  plugin_host::install_supervised_operation_runtime_route(
      kConformanceRuntimePackage, executor,
      []() { return conformance_supervised_identity(); });
  try {
    const NodeOutput output =
        execute_conformance_monolithic("supervised_monolithic_metadata");
    EXPECT_TRUE(output.has_image_value());
    EXPECT_NO_THROW((void)execute_conformance_monolithic(
        "supervised_monolithic_input_metadata", &output));
    const NodeOutput tiled =
        execute_conformance_tiled("supervised_tiled_metadata");
    EXPECT_NO_THROW((void)execute_conformance_tiled(
        "supervised_tiled_input_metadata", &tiled));
    const NodeOutput generic = execute_conformance_monolithic(
        "supervised_monolithic_facet_free_output");
    expect_conformance_generic_output(generic);
    const NodeOutput generic_consumed = execute_conformance_monolithic(
        "supervised_monolithic_facet_free_input_facet_free_output_"
        "input_metadata",
        &generic);
    expect_conformance_generic_output(generic_consumed);
    EXPECT_NO_THROW(execute_conformance_tiled(
        "supervised_tiled_facet_free_input_input_metadata", &generic));

    plugin::ParameterMap empty_containers;
    empty_containers.emplace(
        "empty_array", plugin::ParameterValue(plugin::ParameterValue::Array{}));
    empty_containers.emplace(
        "empty_object",
        plugin::ParameterValue(plugin::ParameterValue::Object{}));
    EXPECT_NO_THROW((void)execute_conformance_monolithic(
        "supervised_monolithic_metadata", nullptr, &empty_containers));
    EXPECT_NO_THROW(execute_conformance_tiled("supervised_tiled_metadata",
                                              nullptr, &empty_containers));
  } catch (...) {
    static_cast<void>(plugin_host::remove_supervised_operation_runtime_route(
        kConformanceRuntimePackage));
    throw;
  }
  EXPECT_TRUE(plugin_host::remove_supervised_operation_runtime_route(
      kConformanceRuntimePackage));
#else
  GTEST_SKIP() << "exact signed supervised execution is Linux-only";
#endif
}

/**
 * @brief Proves every reachable Host-owned output authority surface is
 * recursively revalidated after trusted callbacks in both execution shapes.
 * @throws Nothing when all hostile graph mutations fail closed.
 */
TEST(OperationPluginAbi,
     RejectsTrustedCallbackMutationAcrossCompleteOutputRecordGraph) {
  constexpr const char* kMutations[]{
      "mutate_binding",     "mutate_descriptor",   "mutate_buffer_plan",
      "mutate_full_region", "mutate_grant_region", "mutate_nested_extent",
  };
  for (const char* mutation : kMutations) {
    SCOPED_TRACE(mutation);
    const std::string monolithic =
        std::string("trusted_monolithic_") + mutation;
    EXPECT_THROW((void)execute_conformance_monolithic(monolithic.c_str()),
                 std::invalid_argument);
    const std::string tiled = std::string("trusted_tiled_") + mutation;
    EXPECT_THROW(execute_conformance_tiled(tiled.c_str()),
                 std::invalid_argument);
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
  NodeOutput input = conformance_fallback_input();
  InputTile input_tile{&input.image_value(), PixelRect{0, 0, 2, 2}, nullptr};
  EXPECT_THROW(std::get<TileOpFunc>(*selected)(node, output, {input_tile}),
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
