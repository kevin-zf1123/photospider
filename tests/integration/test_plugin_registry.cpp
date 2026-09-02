#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <exception>
#include <limits>
#include <map>
#include <memory>
#include <new>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include "photospider/compiler/compiler.hpp"
#include "photospider/execution/execution.hpp"
#include "photospider/plugin/data_definition_registry.hpp"
#include "photospider/plugin/operation_registry.hpp"
#include "plugin/dense_layout_validation.hpp"
#include "plugin/library_test_hooks.hpp"
#include "support/test_support.hpp"

#if defined(_WIN32)
#include <windows.h>
#else
#include <dlfcn.h>
#endif

#ifndef PS_OPERATION_FIXTURE_PATH
#error "PS_OPERATION_FIXTURE_PATH must name the valid fixture"
#endif
#ifndef PS_OPERATION_BAD_FIXTURE_PATH
#error "PS_OPERATION_BAD_FIXTURE_PATH must name the invalid fixture"
#endif
#ifndef PS_OPERATION_DENSE_OVERFLOW_FIXTURE_PATH
#error "missing dense-overflow operation fixture path"
#endif
#ifndef PS_OPERATION_DENSE_RANK1_LIMIT_FIXTURE_PATH
#error "missing legal rank-one dense-limit fixture path"
#endif
#ifndef PS_OPERATION_DENSE_RANK1_OVERFLOW_FIXTURE_PATH
#error "missing invalid rank-one dense-limit fixture path"
#endif
#ifndef PS_OPERATION_DENSE_RANK2_LIMIT_FIXTURE_PATH
#error "missing legal rank-two dense-limit fixture path"
#endif
#ifndef PS_OPERATION_DENSE_RANK2_OVERFLOW_FIXTURE_PATH
#error "missing invalid rank-two dense-limit fixture path"
#endif
#ifndef PS_OPERATION_DENSE_MULTI_OVERFLOW_FIXTURE_PATH
#error "missing transactional dense-limit fixture path"
#endif
#ifndef PS_OPERATION_BAD_PARAMETER_POINTER_FIXTURE_PATH
#error "missing parameter-pointer ABI fixture path"
#endif
#ifndef PS_OPERATION_BAD_PARAMETER_SIZE_FIXTURE_PATH
#error "missing parameter-size ABI fixture path"
#endif
#ifndef PS_OPERATION_BAD_PARAMETER_COUNT_FIXTURE_PATH
#error "missing parameter-count ABI fixture path"
#endif
#ifndef PS_OPERATION_BAD_PARAMETER_BOUNDS_FIXTURE_PATH
#error "missing parameter-bounds ABI fixture path"
#endif
#ifndef PS_OPERATION_BAD_PARAMETER_ALIGNMENT_FIXTURE_PATH
#error "missing parameter-alignment ABI fixture path"
#endif
#ifndef PS_OPERATION_INVALID_UTF8_OVERLONG_FIXTURE_PATH
#error "missing overlong operation-key UTF-8 fixture path"
#endif
#ifndef PS_OPERATION_INVALID_UTF8_TRUNCATED_FIXTURE_PATH
#error "missing truncated parameter-key UTF-8 fixture path"
#endif
#ifndef PS_OPERATION_INVALID_UTF8_SURROGATE_FIXTURE_PATH
#error "missing surrogate parameter-key UTF-8 fixture path"
#endif
#ifndef PS_OPERATION_INVALID_UTF8_TOO_LARGE_FIXTURE_PATH
#error "missing above-U+10FFFF operation-key fixture path"
#endif
#ifndef PS_OPERATION_INVALID_UTF8_CONTINUATION_FIXTURE_PATH
#error "missing invalid-continuation parameter-key fixture path"
#endif
#ifndef PS_OPERATION_UNICODE_FIXTURE_PATH
#error "missing valid Unicode operation fixture path"
#endif
#ifndef PS_DATA_PROVIDER_FIXTURE_PATH
#error "PS_DATA_PROVIDER_FIXTURE_PATH must name the valid provider fixture"
#endif
#ifndef PS_DATA_PROVIDER_BAD_FIXTURE_PATH
#error \
    "PS_DATA_PROVIDER_BAD_FIXTURE_PATH must name the invalid provider fixture"
#endif
#ifndef PS_DATA_PROVIDER_INVALID_UTF8_OVERLONG_FIXTURE_PATH
#error "missing overlong provider-schema UTF-8 fixture path"
#endif
#ifndef PS_DATA_PROVIDER_UNICODE_FIXTURE_PATH
#error "missing valid Unicode provider fixture path"
#endif

namespace {

using ps::plugin_testing::LibraryKind;

/**
 * @brief Standard exception whose borrowed diagnostic pointer is null.
 *
 * @note The type deterministically exercises the C++ embedding callback
 * exception fence without allocating while the exception is raised.
 */
class NullDiagnosticException final : public std::exception {
 public:
  /**
   * @brief Returns no diagnostic bytes for the injected callback failure.
   * @return Null, intentionally.
   * @throws Nothing.
   */
  [[nodiscard]] const char* what() const noexcept override { return nullptr; }
};

/**
 * @brief Shared observations for one copy-aware embedding callback.
 *
 * @note Tests arm copy rejection only after rvalue registration completes, so
 * any later increment identifies a registry snapshot that copied user code.
 */
struct CopyAwareCallbackState final {
  /** @brief Whether a later callable copy must raise deterministically. */
  bool reject_copies = false;
  /** @brief Exact number of callable copy-constructor entries. */
  std::uint32_t copy_count = 0U;
  /** @brief Exact number of callback invocations. */
  std::uint32_t call_count = 0U;
};

/**
 * @brief Legal embedding callable that can reject copies after publication.
 *
 * @note Moving remains non-throwing so rvalue registration can transfer the
 * callable into immutable registry ownership without invoking its copy code.
 */
class CopyAwareCallback final {
 public:
  /**
   * @brief Creates one callable over shared deterministic observations.
   * @param state Nonnull observation state retained by the callable.
   * @throws std::invalid_argument If `state` is null.
   */
  explicit CopyAwareCallback(std::shared_ptr<CopyAwareCallbackState> state)
      : state_(std::move(state)) {
    if (!state_) {
      throw std::invalid_argument("copy-aware callback requires state");
    }
  }

  /**
   * @brief Copies one callable and raises when post-publication copies are
   * armed.
   * @param other Source callable sharing the same observation state.
   * @throws std::runtime_error When the shared state rejects later copies.
   * @note The counter increments before the deterministic exception.
   */
  CopyAwareCallback(const CopyAwareCallback& other) : state_(other.state_) {
    ++state_->copy_count;
    if (state_->reject_copies) {
      throw std::runtime_error("embedding callable copied after publication");
    }
  }

  /**
   * @brief Moves callable ownership without entering user copy code.
   * @param other Source callable left with unspecified shared ownership.
   * @throws Nothing.
   */
  CopyAwareCallback(CopyAwareCallback&& other) noexcept = default;

  /**
   * @brief Releases shared observation ownership.
   * @throws Nothing.
   */
  ~CopyAwareCallback() noexcept = default;

  /**
   * @brief Forbids copy assignment, which `std::function` does not require.
   * @param other Source callable that cannot be assigned.
   * @return No value; the operation is deleted.
   * @throws Nothing; the operation is deleted.
   */
  CopyAwareCallback& operator=(const CopyAwareCallback& other) = delete;

  /**
   * @brief Forbids move assignment of an already stored callback target.
   * @param other Source callable that cannot be assigned.
   * @return No value; the operation is deleted.
   * @throws Nothing; the operation is deleted.
   */
  CopyAwareCallback& operator=(CopyAwareCallback&& other) = delete;

  /**
   * @brief Publishes one deterministic scalar while counting callback entry.
   * @param invocation Validated zero-input invocation.
   * @return Float64 scalar value 41.
   * @throws std::bad_alloc If Value construction fails.
   */
  ps::Result<ps::Value> operator()(
      const ps::OperationInvocation& invocation) const {
    static_cast<void>(invocation);
    ++state_->call_count;
    return ps::Result<ps::Value>(ps::Value::from_float64(41.0));
  }

 private:
  /** @brief Shared deterministic copy/invocation observations. */
  std::shared_ptr<CopyAwareCallbackState> state_;
};

/** @brief Library kind expected by the currently installed test callback. */
LibraryKind g_expected_library_kind = LibraryKind::Operation;
/** @brief Exact owner-allocation callback invocation count. */
std::uint32_t g_owner_allocation_count = 0U;
/** @brief Exact native close callback invocation count. */
std::uint32_t g_native_close_count = 0U;
/** @brief Exact copied-provider-schema retirement callback count. */
std::uint32_t g_provider_schema_retirement_count = 0U;
/** @brief Exact number of provider schemas retired by the observed registry. */
std::size_t g_retired_provider_schema_count = 0U;
/** @brief Monotonic callback sequence within one installed hook scope. */
std::uint32_t g_lifecycle_sequence = 0U;
/** @brief Sequence position of copied-provider-schema retirement. */
std::uint32_t g_provider_schema_retirement_sequence = 0U;
/** @brief Sequence position of the expected native close callback. */
std::uint32_t g_native_close_sequence = 0U;

/**
 * @brief Raises deterministic allocation failure at one expected owner seam.
 * @param kind Library kind approaching owner allocation.
 * @throws std::bad_alloc For the exact expected library kind.
 * @note Unexpected kinds expose a test-ordering error through no exception.
 */
void fail_owner_allocation(ps::plugin_testing::LibraryKind kind) {
  if (kind == g_expected_library_kind) {
    ++g_owner_allocation_count;
    throw std::bad_alloc();
  }
}

/**
 * @brief Counts one native close call for the expected library kind.
 * @param kind Closed library kind.
 * @throws Nothing.
 * @note Unexpected kinds are ignored so each scoped assertion stays exact.
 */
void count_native_close(ps::plugin_testing::LibraryKind kind) noexcept {
  if (kind == g_expected_library_kind) {
    ++g_native_close_count;
    g_native_close_sequence = ++g_lifecycle_sequence;
  }
}

/**
 * @brief Records one copied-provider-schema retirement phase.
 * @param count Exact number of registry-owned schema records cleared.
 * @throws Nothing.
 * @note Native provider close must occur at a later sequence position.
 */
void count_provider_schemas_retired(std::size_t count) noexcept {
  ++g_provider_schema_retirement_count;
  g_retired_provider_schema_count = count;
  g_provider_schema_retirement_sequence = ++g_lifecycle_sequence;
}

/**
 * @brief Installs one deterministic native-library lifecycle hook scope.
 *
 * @note The single-threaded scope may inject owner allocation failure or only
 * observe native close, and clears callbacks before later lifecycle cases.
 */
class LibraryHookScope final {
 public:
  /**
   * @brief Installs callbacks for one exact library kind and behavior.
   * @param kind Operation or provider lifecycle to observe.
   * @param fail_allocation Whether to throw at the owner-allocation boundary.
   * @throws Nothing.
   */
  LibraryHookScope(ps::plugin_testing::LibraryKind kind,
                   bool fail_allocation) noexcept {
    g_expected_library_kind = kind;
    g_owner_allocation_count = 0U;
    g_native_close_count = 0U;
    g_provider_schema_retirement_count = 0U;
    g_retired_provider_schema_count = 0U;
    g_lifecycle_sequence = 0U;
    g_provider_schema_retirement_sequence = 0U;
    g_native_close_sequence = 0U;
    hooks_.before_owner_allocation =
        fail_allocation ? fail_owner_allocation : nullptr;
    hooks_.native_close = count_native_close;
    hooks_.provider_schemas_retired = count_provider_schemas_retired;
    ps::plugin_testing::install_library_test_hooks(&hooks_);
  }

  /**
   * @brief Clears borrowed callbacks before scope storage retires.
   * @throws Nothing.
   */
  ~LibraryHookScope() noexcept {
    ps::plugin_testing::install_library_test_hooks(nullptr);
  }

  /**
   * @brief Forbids duplicating one process-global callback installation.
   * @param other Source scope that cannot be copied.
   * @throws Nothing; the operation is deleted.
   * @note Exactly one scope owns callback installation at a time.
   */
  LibraryHookScope(const LibraryHookScope& other) = delete;
  /**
   * @brief Forbids assigning process-global callback installation.
   * @param other Source scope that cannot be assigned.
   * @return No value; the operation is deleted.
   * @throws Nothing; the operation is deleted.
   */
  LibraryHookScope& operator=(const LibraryHookScope& other) = delete;
  /**
   * @brief Forbids moving the address-stable borrowed callback table.
   * @param other Source scope that cannot be moved.
   * @throws Nothing; the operation is deleted.
   * @note The installed pointer must remain stable until scope destruction.
   */
  LibraryHookScope(LibraryHookScope&& other) = delete;
  /**
   * @brief Forbids move assignment of process-global callback installation.
   * @param other Source scope that cannot be assigned.
   * @return No value; the operation is deleted.
   * @throws Nothing; the operation is deleted.
   * @note Callback storage and installation always retire together.
   */
  LibraryHookScope& operator=(LibraryHookScope&& other) = delete;

  /**
   * @brief Returns exact injected owner-allocation callback count.
   * @return Current scoped count.
   * @throws Nothing.
   */
  [[nodiscard]] std::uint32_t owner_allocation_count() const noexcept {
    return g_owner_allocation_count;
  }

  /**
   * @brief Returns exact observed native close-call count.
   * @return Current scoped count.
   * @throws Nothing.
   */
  [[nodiscard]] std::uint32_t native_close_count() const noexcept {
    return g_native_close_count;
  }

  /**
   * @brief Returns exact copied-provider-schema retirement callback count.
   * @return Current scoped callback count.
   * @throws Nothing.
   */
  [[nodiscard]] std::uint32_t provider_schema_retirement_count()
      const noexcept {
    return g_provider_schema_retirement_count;
  }

  /**
   * @brief Returns exact number of copied schemas retired by destruction.
   * @return Last observed schema count.
   * @throws Nothing.
   */
  [[nodiscard]] std::size_t retired_provider_schema_count() const noexcept {
    return g_retired_provider_schema_count;
  }

  /**
   * @brief Reports whether schema retirement preceded native provider close.
   * @return True only for two observed callbacks in the required order.
   * @throws Nothing.
   */
  [[nodiscard]] bool provider_retired_before_close() const noexcept {
    return g_provider_schema_retirement_sequence != 0U &&
           g_native_close_sequence > g_provider_schema_retirement_sequence;
  }

 private:
  /** @brief Borrowed callback table installed for this scope. */
  ps::plugin_testing::LibraryTestHooks hooks_;
};

/**
 * @brief Keeps one test DSO image mapped while registry ownership is released.
 *
 * @note The observer performs no product admission; it exists only to read the
 * fixture's destroy counter after registry destruction.
 */
class LibraryObserver final {
 public:
  /**
   * @brief Opens one exact test fixture path.
   * @param path Build-generated fixture path.
   * @throws std::runtime_error If the fixture cannot be mapped.
   * @note The mapping is independent from registry ownership.
   */
  explicit LibraryObserver(const char* path) {
#if defined(_WIN32)
    handle_ = static_cast<void*>(LoadLibraryA(path));
#else
    handle_ = dlopen(path, RTLD_NOW | RTLD_LOCAL);
#endif
    if (!handle_) {
      throw std::runtime_error("could not open lifecycle fixture");
    }
  }

  /**
   * @brief Releases the observer's independent mapping reference.
   * @throws Nothing.
   * @note The fixture's registry-owned mapping has already retired in tests.
   */
  ~LibraryObserver() noexcept {
#if defined(_WIN32)
    if (handle_) {
      FreeLibrary(static_cast<HMODULE>(handle_));
    }
#else
    if (handle_) {
      dlclose(handle_);
    }
#endif
  }

  /**
   * @brief Forbids duplicating one native fixture mapping reference.
   * @param other Source observer that cannot be copied.
   * @throws Nothing; the operation is deleted.
   * @note Each observer closes exactly the mapping it opened.
   */
  LibraryObserver(const LibraryObserver& other) = delete;
  /**
   * @brief Forbids assigning native fixture mapping ownership.
   * @param other Source observer that cannot be assigned.
   * @return No value; the operation is deleted.
   * @throws Nothing; the operation is deleted.
   * @note Symbol observation lifetime remains tied to construction.
   */
  LibraryObserver& operator=(const LibraryObserver& other) = delete;

  /**
   * @brief Reads one exported zero-argument uint32 counter function.
   * @param name Exact fixture symbol.
   * @return Current counter value.
   * @throws std::runtime_error If the symbol is missing.
   * @note The mapped fixture remains alive for this observer's lifetime.
   */
  [[nodiscard]] std::uint32_t counter(const char* name) const {
#if defined(_WIN32)
    void* address = reinterpret_cast<void*>(
        GetProcAddress(static_cast<HMODULE>(handle_), name));
#else
    void* address = dlsym(handle_, name);
#endif
    using Function = std::uint32_t (*)();
    Function function = nullptr;
    static_assert(sizeof(function) == sizeof(address),
                  "fixture target requires equal pointer sizes");
    std::memcpy(&function, &address, sizeof(function));
    if (!function) {
      throw std::runtime_error("fixture counter symbol is missing");
    }
    return function();
  }

 private:
  /** @brief Native fixture mapping handle. */
  void* handle_ = nullptr;
};

/**
 * @brief Verifies dense byte-size arithmetic for 32-bit and native hosts.
 * @return Zero when every exact inclusive/exclusive boundary is preserved.
 * @throws Nothing.
 * @note The explicit `uint32_t` instantiation compiles and executes the 32-bit
 * `size_t` branch even when this test process uses 64-bit `size_t`.
 */
int verify_dense_byte_size_contract() noexcept {
  constexpr std::uint64_t kUint32Maximum = UINT64_C(4294967295);
  constexpr std::uint64_t kMaximumLegalSignedRange = UINT64_C(1) << 63U;
  static_assert(
      ps::plugin_internal::dense_byte_size_representable<std::uint32_t>(
          kUint32Maximum),
      "32-bit size maximum must remain inclusive");
  static_assert(
      !ps::plugin_internal::dense_byte_size_representable<std::uint32_t>(
          kUint32Maximum + UINT64_C(1)),
      "32-bit size overflow must be rejected");
  PS_CHECK(ps::plugin_internal::dense_byte_size_representable<std::uint64_t>(
      kMaximumLegalSignedRange));
  PS_CHECK(!ps::plugin_internal::dense_byte_size_representable<std::uint64_t>(
      kMaximumLegalSignedRange + UINT64_C(1)));
  PS_CHECK(!ps::plugin_internal::dense_byte_size_representable<std::uint32_t>(
      UINT64_C(0)));
  return 0;
}

/**
 * @brief Loads one real dense-boundary DSO and verifies exact lifecycle.
 * @param path Build-generated fixture path.
 * @param should_load Whether descriptor validation must accept the table.
 * @param expected_key Accepted operation key, empty for rejected tables.
 * @param expected_shape Accepted fixed shape, empty for rejected tables.
 * @return Zero when registration, transactionality, destroy, and close match.
 * @throws std::bad_alloc If registry staging allocation fails.
 * @throws std::runtime_error If the fixture cannot be mapped or inspected.
 * @note Rejected multi-descriptor tables must leave the registry completely
 * empty even when the first descriptor was legal.
 */
int verify_dense_dso_fixture(const char* path, bool should_load,
                             const std::string& expected_key,
                             const std::vector<std::uint64_t>& expected_shape) {
  LibraryObserver observer(path);
  PS_CHECK(observer.counter("ps_operation_dense_limit_fixture_destroy_count") ==
           0U);
  {
    LibraryHookScope lifecycle(ps::plugin_testing::LibraryKind::Operation,
                               false);
    {
      ps::OperationRegistry registry;
      const ps::Status loaded = registry.load_plugin(path);
      PS_CHECK(loaded.ok() == should_load);
      if (should_load) {
        PS_CHECK(registry.keys() == std::vector<std::string>({expected_key}));
        auto traits = registry.find_traits(expected_key);
        PS_CHECK(traits.ok());
        PS_CHECK(traits.value().output_element_type == ps::ElementType::UInt8);
        PS_CHECK(traits.value().shape_rule == ps::OperationShapeRule::Fixed);
        PS_CHECK(traits.value().fixed_output_shape == expected_shape);
      } else {
        PS_CHECK(registry.keys().empty());
      }
    }
    PS_CHECK(lifecycle.native_close_count() == 1U);
  }
  PS_CHECK(observer.counter("ps_operation_dense_limit_fixture_destroy_count") ==
           1U);
  return 0;
}

/**
 * @brief Verifies one C++ fixed descriptor with a broadcast-only huge shape.
 *
 * The helper registers and freezes an embedding callback, compiles the same
 * graph twice, executes the same plan twice, and proves descriptor identity,
 * admission accounting, and the eight-byte broadcast result stay stable
 * without evaluating the dense logical element product.
 *
 * @param shape Rank-1..8 nonzero fixed output shape.
 * @return Zero when registration, compilation, execution, and exact checks
 * pass; one on the first failed check.
 * @throws std::bad_alloc If test staging allocation fails.
 * @note The callback publishes a zero-stride whole-Region Value backed by one
 * Float64 scalar; this is a C++ embedding contract, not a C DSO layout.
 */
int verify_cpp_fixed_broadcast(const std::vector<std::uint64_t>& shape) {
  auto registry = std::make_shared<ps::OperationRegistry>();
  ps::OperationTraits traits;
  traits.estimated_bytes = sizeof(double);
  traits.output_element_type = ps::ElementType::Float64;
  traits.shape_rule = ps::OperationShapeRule::Fixed;
  traits.fixed_output_shape = shape;
  PS_CHECK(
      registry
          ->register_operation(ps::OperationDefinition{
              "fixture.fixed_broadcast", traits,
              [shape](const ps::OperationInvocation&) -> ps::Result<ps::Value> {
                const double scalar = 19.0;
                std::vector<std::uint8_t> bytes(sizeof(scalar));
                std::memcpy(bytes.data(), &scalar, sizeof(scalar));
                return ps::Value::create(
                    ps::ValueDescriptor{ps::ElementType::Float64, shape},
                    ps::Region::whole(shape),
                    ps::StridedLayout{
                        0U, std::vector<std::int64_t>(shape.size(), 0)},
                    std::move(bytes));
              }})
          .ok());
  auto copied_traits = registry->find_traits("fixture.fixed_broadcast");
  PS_CHECK(copied_traits.ok());
  PS_CHECK(copied_traits.value().estimated_bytes == sizeof(double));
  PS_CHECK(copied_traits.value().fixed_output_shape == shape);
  PS_CHECK(registry->freeze().ok());

  ps::WorkflowDocument document;
  document.nodes = {ps::WorkflowNode{1U, "fixture.fixed_broadcast", {}, {}}};
  document.outputs = {ps::WorkflowOutput{"value", 1U, "value"}};
  ps::GraphContext graph(std::move(document));
  ps::Compiler compiler(registry);
  auto first_compilation = compiler.compile(graph);
  auto second_compilation = compiler.compile(graph);
  PS_CHECK(first_compilation.ok());
  PS_CHECK(second_compilation.ok());
  PS_CHECK(first_compilation.value().semantic.digest().value ==
           second_compilation.value().semantic.digest().value);
  PS_CHECK(first_compilation.value().optimized.digest().value ==
           second_compilation.value().optimized.digest().value);
  PS_CHECK(first_compilation.value().plan.digest().value ==
           second_compilation.value().plan.digest().value);
  PS_CHECK(first_compilation.value().plan.cache_key().value ==
           second_compilation.value().plan.cache_key().value);
  PS_CHECK(first_compilation.value().plan.steps().size() == 1U);
  PS_CHECK(first_compilation.value().plan.steps().front().planned_bytes ==
           sizeof(double));
  PS_CHECK(
      first_compilation.value().plan.steps().front().output_descriptor.shape ==
      shape);

  ps::ExecutionContextConfig config;
  config.cpu_workers = 1U;
  config.maximum_queued_tasks = 1U;
  config.maximum_live_bytes = sizeof(double);
  ps::ExecutionContext execution(registry, config);
  auto first_result = execution.execute(first_compilation.value().plan);
  auto second_result = execution.execute(second_compilation.value().plan);
  PS_CHECK(first_result.ok());
  PS_CHECK(second_result.ok());
  PS_CHECK(first_result.value().diagnostics.peak_live_bytes == sizeof(double));
  PS_CHECK(second_result.value().diagnostics.peak_live_bytes == sizeof(double));
  PS_CHECK(first_result.value().diagnostics.result_digest ==
           second_result.value().diagnostics.result_digest);
  const auto value_entry = first_result.value().values.find("value");
  PS_CHECK(value_entry != first_result.value().values.end());
  const ps::Value& value = value_entry->second;
  PS_CHECK(value.valid());
  PS_CHECK(value.descriptor().shape == shape);
  PS_CHECK(value.bytes().size() == sizeof(double));
  PS_CHECK(value.layout().byte_strides.size() == shape.size());
  PS_CHECK(std::all_of(value.layout().byte_strides.begin(),
                       value.layout().byte_strides.end(),
                       [](std::int64_t stride) { return stride == 0; }));
  PS_CHECK(value.region().dimensions().size() == shape.size());
  for (std::size_t axis = 0U; axis < shape.size(); ++axis) {
    PS_CHECK(value.region().dimensions()[axis].offset == 0U);
    PS_CHECK(value.region().dimensions()[axis].extent == shape[axis]);
  }
  return 0;
}

/**
 * @brief Proves registry snapshots retain immutable callback handles.
 *
 * The first registry receives a copy-aware callable through rvalue
 * registration, arms copy rejection, freezes, and invokes it. The second arms
 * an independently registered callable before loading a valid DSO into an
 * unfrozen registry. Both paths must copy only owning handles after arm.
 *
 * @return Zero when invoke/load succeed without a callable copy or exception.
 * @throws std::bad_alloc If test or registry staging allocation fails.
 * @note The valid DSO load also preserves transactional publication and its
 * existing library lifetime anchor; the fixture is fully unloaded on return.
 */
int verify_immutable_callback_handles() {
  using ps::Backend;
  using ps::CancellationToken;
  using ps::ElementType;
  using ps::ErrorCode;
  using ps::OperationDefinition;
  using ps::OperationInvocation;
  using ps::OperationRegistry;
  using ps::OperationTraits;
  using ps::ParameterValue;
  using ps::Region;
  using ps::Value;

  const std::vector<Value> no_inputs;
  const std::vector<Region> no_demands;
  const std::map<std::string, ParameterValue> no_parameters;

  auto invoke_state = std::make_shared<CopyAwareCallbackState>();
  OperationRegistry invoke_registry;
  PS_CHECK(invoke_registry
               .register_operation(OperationDefinition{
                   "fixture.copy_aware_invoke", OperationTraits{},
                   CopyAwareCallback(invoke_state)})
               .ok());
  invoke_state->reject_copies = true;
  const std::uint32_t invoke_copies_before_arm = invoke_state->copy_count;
  PS_CHECK(invoke_registry.freeze().ok());
  bool invoke_escaped = false;
  bool invoke_succeeded = false;
  double invoked_value = 0.0;
  try {
    auto invoked = invoke_registry.invoke(
        "fixture.copy_aware_invoke",
        OperationInvocation{no_inputs, no_demands, no_parameters, Backend::Cpu,
                            CancellationToken()});
    invoke_succeeded = invoked.ok() && invoked.value().as_float64().ok();
    if (invoke_succeeded) {
      invoked_value = invoked.value().as_float64().value();
    }
  } catch (...) {
    invoke_escaped = true;
  }
  PS_CHECK(!invoke_escaped);
  PS_CHECK(invoke_succeeded);
  PS_CHECK(invoked_value == 41.0);
  PS_CHECK(invoke_state->call_count == 1U);
  PS_CHECK(invoke_state->copy_count == invoke_copies_before_arm);

  const Value unexpected_input = Value::from_float64(1.0);
  const std::vector<Value> invalid_inputs{unexpected_input};
  const std::vector<Region> invalid_demands{Region::whole({1U})};
  auto invalid_input = invoke_registry.invoke(
      "fixture.copy_aware_invoke",
      OperationInvocation{invalid_inputs, invalid_demands, no_parameters,
                          Backend::Cpu, CancellationToken()});
  PS_CHECK(!invalid_input.ok());
  PS_CHECK(invalid_input.status().code == ErrorCode::InvalidArgument);
  PS_CHECK(invoke_state->call_count == 1U);
  PS_CHECK(invoke_state->copy_count == invoke_copies_before_arm);

  auto load_state = std::make_shared<CopyAwareCallbackState>();
  OperationRegistry load_registry;
  PS_CHECK(load_registry
               .register_operation(OperationDefinition{
                   "fixture.copy_aware_load", OperationTraits{},
                   CopyAwareCallback(load_state)})
               .ok());
  load_state->reject_copies = true;
  const std::uint32_t load_copies_before_arm = load_state->copy_count;
  bool load_escaped = false;
  bool load_succeeded = false;
  try {
    load_succeeded = load_registry.load_plugin(PS_OPERATION_FIXTURE_PATH).ok();
  } catch (...) {
    load_escaped = true;
  }
  PS_CHECK(!load_escaped);
  PS_CHECK(load_succeeded);
  PS_CHECK(load_registry.find_traits("fixture.double").ok());
  PS_CHECK(load_state->call_count == 0U);
  PS_CHECK(load_state->copy_count == load_copies_before_arm);
  return 0;
}

}  // namespace

/**
 * @brief Exercises operation/provider ABI validation, callbacks, freezing, and
 * exact destroy-before-unload lifecycle.
 * @return Zero when all checks pass.
 * @throws std::bad_alloc If test setup allocation fails.
 * @throws std::runtime_error If a fixture cannot be inspected.
 * @note Failures otherwise return nonzero through `PS_CHECK`.
 */
int main() {
  using ps::Backend;
  using ps::CancellationToken;
  using ps::Compiler;
  using ps::DataDefinitionRegistry;
  using ps::DataSchemaDefinition;
  using ps::ElementType;
  using ps::ErrorCode;
  using ps::GraphContext;
  using ps::OperationDefinition;
  using ps::OperationInvocation;
  using ps::OperationParameterSpec;
  using ps::OperationParameterType;
  using ps::OperationRegistry;
  using ps::OperationShapeRule;
  using ps::OperationTraits;
  using ps::ParameterValue;
  using ps::Region;
  using ps::RegionDimension;
  using ps::Result;
  using ps::StridedLayout;
  using ps::Value;
  using ps::ValueDescriptor;
  using ps::ValueFacet;
  using ps::WorkflowDocument;
  using ps::WorkflowInput;
  using ps::WorkflowNode;
  using ps::WorkflowOutput;

  PS_CHECK(verify_immutable_callback_handles() == 0);

  const std::array<const char*, 5U> invalid_operation_utf8_fixtures{
      PS_OPERATION_INVALID_UTF8_OVERLONG_FIXTURE_PATH,
      PS_OPERATION_INVALID_UTF8_TRUNCATED_FIXTURE_PATH,
      PS_OPERATION_INVALID_UTF8_SURROGATE_FIXTURE_PATH,
      PS_OPERATION_INVALID_UTF8_TOO_LARGE_FIXTURE_PATH,
      PS_OPERATION_INVALID_UTF8_CONTINUATION_FIXTURE_PATH,
  };
  auto unicode_operation_registry = std::make_unique<OperationRegistry>();
  for (const char* fixture_path : invalid_operation_utf8_fixtures) {
    LibraryObserver observer(fixture_path);
    PS_CHECK(observer.counter("ps_operation_utf8_fixture_destroy_count") == 0U);
    {
      LibraryHookScope lifecycle(ps::plugin_testing::LibraryKind::Operation,
                                 false);
      PS_CHECK(!unicode_operation_registry->load_plugin(fixture_path).ok());
      PS_CHECK(lifecycle.owner_allocation_count() == 0U);
      PS_CHECK(lifecycle.native_close_count() == 1U);
    }
    PS_CHECK(unicode_operation_registry->keys().empty());
    PS_CHECK(observer.counter("ps_operation_utf8_fixture_destroy_count") == 1U);
  }

  LibraryObserver unicode_operation_observer(PS_OPERATION_UNICODE_FIXTURE_PATH);
  PS_CHECK(unicode_operation_observer.counter(
               "ps_operation_utf8_fixture_destroy_count") == 0U);
  PS_CHECK(
      unicode_operation_registry->load_plugin(PS_OPERATION_UNICODE_FIXTURE_PATH)
          .ok());
  const std::string unicode_operation_key = "fixture.\xe5\x80\x8d\xe7\x8e\x87";
  const std::string unicode_parameter_key = "\xe7\xbc\xa9\xe6\x94\xbe";
  auto unicode_traits =
      unicode_operation_registry->find_traits(unicode_operation_key);
  PS_CHECK(unicode_traits.ok());
  PS_CHECK(unicode_traits.value().parameter_schema.size() == 1U);
  PS_CHECK(unicode_traits.value().parameter_schema.front().key ==
           unicode_parameter_key);
  {
    LibraryHookScope lifecycle(ps::plugin_testing::LibraryKind::Operation,
                               false);
    unicode_operation_registry.reset();
    PS_CHECK(lifecycle.native_close_count() == 1U);
  }
  PS_CHECK(unicode_operation_observer.counter(
               "ps_operation_utf8_fixture_destroy_count") == 1U);

  LibraryObserver dense_overflow_observer(
      PS_OPERATION_DENSE_OVERFLOW_FIXTURE_PATH);
  PS_CHECK(dense_overflow_observer.counter(
               "ps_operation_dense_overflow_fixture_destroy_count") == 0U);
  {
    LibraryHookScope lifecycle(ps::plugin_testing::LibraryKind::Operation,
                               false);
    OperationRegistry registry;
    PS_CHECK(
        !registry.load_plugin(PS_OPERATION_DENSE_OVERFLOW_FIXTURE_PATH).ok());
    PS_CHECK(registry.keys().empty());
    PS_CHECK(lifecycle.native_close_count() == 1U);
  }
  PS_CHECK(dense_overflow_observer.counter(
               "ps_operation_dense_overflow_fixture_destroy_count") == 1U);

  PS_CHECK(verify_dense_byte_size_contract() == 0);
  const std::uint64_t maximum_legal_dense_bytes = UINT64_C(1) << 63U;
  PS_CHECK(verify_dense_dso_fixture(PS_OPERATION_DENSE_RANK1_LIMIT_FIXTURE_PATH,
                                    true, "fixture.dense_rank1_limit",
                                    {maximum_legal_dense_bytes}) == 0);
  PS_CHECK(verify_dense_dso_fixture(
               PS_OPERATION_DENSE_RANK1_OVERFLOW_FIXTURE_PATH, false, "", {}) ==
           0);
  PS_CHECK(verify_dense_dso_fixture(PS_OPERATION_DENSE_RANK2_LIMIT_FIXTURE_PATH,
                                    true, "fixture.dense_rank2_limit",
                                    {UINT64_C(2), UINT64_C(1) << 62U}) == 0);
  PS_CHECK(verify_dense_dso_fixture(
               PS_OPERATION_DENSE_RANK2_OVERFLOW_FIXTURE_PATH, false, "", {}) ==
           0);
  PS_CHECK(verify_dense_dso_fixture(
               PS_OPERATION_DENSE_MULTI_OVERFLOW_FIXTURE_PATH, false, "", {}) ==
           0);

  LibraryObserver operation_observer(PS_OPERATION_FIXTURE_PATH);
  PS_CHECK(operation_observer.counter("ps_operation_fixture_destroy_count") ==
           0U);
  {
    LibraryHookScope failure(ps::plugin_testing::LibraryKind::Operation, true);
    bool allocation_failed = false;
    try {
      OperationRegistry registry;
      static_cast<void>(registry.load_plugin(PS_OPERATION_FIXTURE_PATH));
    } catch (const std::bad_alloc&) {
      allocation_failed = true;
    }
    PS_CHECK(allocation_failed);
    PS_CHECK(failure.owner_allocation_count() == 1U);
    PS_CHECK(failure.native_close_count() == 1U);
    PS_CHECK(operation_observer.counter("ps_operation_fixture_destroy_count") ==
             1U);
  }
  {
    OperationRegistry registry;
    PS_CHECK(registry.load_plugin(PS_OPERATION_FIXTURE_PATH).ok());
    PS_CHECK(!registry.load_plugin(PS_OPERATION_BAD_FIXTURE_PATH).ok());
    PS_CHECK(
        !registry.load_plugin(PS_OPERATION_BAD_PARAMETER_POINTER_FIXTURE_PATH)
             .ok());
    PS_CHECK(!registry.load_plugin(PS_OPERATION_BAD_PARAMETER_SIZE_FIXTURE_PATH)
                  .ok());
    PS_CHECK(
        !registry.load_plugin(PS_OPERATION_BAD_PARAMETER_COUNT_FIXTURE_PATH)
             .ok());
    PS_CHECK(
        !registry.load_plugin(PS_OPERATION_BAD_PARAMETER_BOUNDS_FIXTURE_PATH)
             .ok());
    PS_CHECK(
        !registry.load_plugin(PS_OPERATION_BAD_PARAMETER_ALIGNMENT_FIXTURE_PATH)
             .ok());
    PS_CHECK(registry
                 .register_operation(OperationDefinition{
                     "fixture.source", OperationTraits{},
                     [](const OperationInvocation&) -> Result<Value> {
                       return Result<Value>(Value::from_float64(3.0));
                     }})
                 .ok());
    PS_CHECK(registry.freeze().ok());
    PS_CHECK(!registry.load_plugin(PS_OPERATION_FIXTURE_PATH).ok());
    auto traits = registry.find_traits("fixture.double");
    PS_CHECK(traits.ok());
    PS_CHECK(traits.value().input_count == 1U);
    PS_CHECK(traits.value().version == 2U);
    PS_CHECK(traits.value().parameter_schema.size() == 1U);
    PS_CHECK(traits.value().parameter_schema.front().key == "scale");
    PS_CHECK(traits.value().parameter_schema.front().type ==
             OperationParameterType::Float64);
    PS_CHECK(traits.value().parameter_schema.front().required);

    Compiler plugin_compiler(std::shared_ptr<OperationRegistry>(
        &registry, [](OperationRegistry*) {}));
    WorkflowDocument plugin_document;
    plugin_document.nodes = {
        WorkflowNode{1U, "fixture.source", {}, {}},
        WorkflowNode{2U,
                     "fixture.double",
                     {WorkflowInput{1U, "value"}},
                     {{"scale", 2.0}}},
    };
    plugin_document.outputs = {WorkflowOutput{"value", 2U, "value"}};
    GraphContext plugin_graph(plugin_document);
    PS_CHECK(plugin_compiler.compile(plugin_graph).ok());
    WorkflowDocument missing_plugin_parameter = plugin_document;
    missing_plugin_parameter.nodes.back().parameters.clear();
    GraphContext missing_plugin_graph(std::move(missing_plugin_parameter));
    PS_CHECK(!plugin_compiler.compile(missing_plugin_graph).ok());
    WorkflowDocument unknown_plugin_parameter = plugin_document;
    unknown_plugin_parameter.nodes.back().parameters = {{"factor", 2.0}};
    GraphContext unknown_plugin_graph(std::move(unknown_plugin_parameter));
    PS_CHECK(!plugin_compiler.compile(unknown_plugin_graph).ok());
    WorkflowDocument wrong_plugin_parameter = plugin_document;
    wrong_plugin_parameter.nodes.back().parameters = {{"scale", 2LL}};
    GraphContext wrong_plugin_graph(std::move(wrong_plugin_parameter));
    PS_CHECK(!plugin_compiler.compile(wrong_plugin_graph).ok());

    const Value scalar = Value::from_float64(3.0);
    auto faceted_input = Value::create(
        scalar.descriptor(), scalar.region(), scalar.layout(), scalar.bytes(),
        {ValueFacet{"test.semantic", 2U, {8U, 9U}}});
    PS_CHECK(faceted_input.ok());
    std::vector<Value> inputs{faceted_input.take_value()};
    const std::vector<Region> demands{Region::whole({1U})};
    const std::map<std::string, ParameterValue> parameters{{"scale", 2.0}};
    auto output =
        registry.invoke("fixture.double",
                        OperationInvocation{inputs, demands, parameters,
                                            Backend::Cpu, CancellationToken()});
    PS_CHECK(output.ok());
    PS_CHECK(output.value().as_float64().ok());
    PS_CHECK(output.value().as_float64().value() == 6.0);
    PS_CHECK(output.value().facets().size() == 1U);
    PS_CHECK(output.value().facets().front().key == "test.semantic");
    PS_CHECK(output.value().facets().front().payload ==
             std::vector<std::uint8_t>({8U, 9U}));
    auto trailing_input = Value::create(
        scalar.descriptor(), scalar.region(), scalar.layout(),
        std::vector<std::uint8_t>(scalar.bytes().size() + 1U, 0U));
    PS_CHECK(trailing_input.ok());
    std::vector<Value> trailing_inputs{trailing_input.take_value()};
    auto trailing_rejected = registry.invoke(
        "fixture.double",
        OperationInvocation{trailing_inputs, demands, parameters, Backend::Cpu,
                            CancellationToken()});
    PS_CHECK(!trailing_rejected.ok());
    PS_CHECK(trailing_rejected.status().code == ErrorCode::TypeMismatch);
    const std::map<std::string, ParameterValue> no_parameters;
    auto missing_parameter =
        registry.invoke("fixture.double",
                        OperationInvocation{inputs, demands, no_parameters,
                                            Backend::Cpu, CancellationToken()});
    PS_CHECK(!missing_parameter.ok());
    PS_CHECK(missing_parameter.status().code == ErrorCode::InvalidArgument);
    const std::map<std::string, ParameterValue> unknown_parameters{
        {"factor", 2.0}};
    auto unknown_parameter =
        registry.invoke("fixture.double",
                        OperationInvocation{inputs, demands, unknown_parameters,
                                            Backend::Cpu, CancellationToken()});
    PS_CHECK(!unknown_parameter.ok());
    PS_CHECK(unknown_parameter.status().code == ErrorCode::InvalidArgument);
    const std::map<std::string, ParameterValue> wrong_parameters{
        {"scale", std::int64_t{2}}};
    auto wrong_parameter =
        registry.invoke("fixture.double",
                        OperationInvocation{inputs, demands, wrong_parameters,
                                            Backend::Cpu, CancellationToken()});
    PS_CHECK(!wrong_parameter.ok());
    PS_CHECK(wrong_parameter.status().code == ErrorCode::InvalidArgument);
    auto bad_facet =
        registry.invoke("fixture.bad_facet",
                        OperationInvocation{inputs, demands, no_parameters,
                                            Backend::Cpu, CancellationToken()});
    PS_CHECK(!bad_facet.ok());
    PS_CHECK(bad_facet.status().code == ErrorCode::InvalidArgument);
  }
  PS_CHECK(operation_observer.counter("ps_operation_fixture_destroy_count") ==
           2U);

  auto unicode_provider_registry = std::make_unique<DataDefinitionRegistry>();
  const std::string invalid_provider_key("\xc0\xaf", 2U);
  const std::string unicode_provider_key = "fixture.\xe5\x9b\xbe\xe5\x83\x8f";
  {
    LibraryObserver observer(
        PS_DATA_PROVIDER_INVALID_UTF8_OVERLONG_FIXTURE_PATH);
    PS_CHECK(observer.counter("ps_data_provider_utf8_fixture_destroy_count") ==
             0U);
    {
      LibraryHookScope lifecycle(ps::plugin_testing::LibraryKind::Provider,
                                 false);
      PS_CHECK(!unicode_provider_registry
                    ->load_provider(
                        PS_DATA_PROVIDER_INVALID_UTF8_OVERLONG_FIXTURE_PATH)
                    .ok());
      PS_CHECK(lifecycle.owner_allocation_count() == 0U);
      PS_CHECK(lifecycle.native_close_count() == 1U);
    }
    PS_CHECK(!unicode_provider_registry->find(invalid_provider_key).ok());
    PS_CHECK(!unicode_provider_registry->find(unicode_provider_key).ok());
    PS_CHECK(observer.counter("ps_data_provider_utf8_fixture_destroy_count") ==
             1U);
  }

  LibraryObserver unicode_provider_observer(
      PS_DATA_PROVIDER_UNICODE_FIXTURE_PATH);
  PS_CHECK(unicode_provider_observer.counter(
               "ps_data_provider_utf8_fixture_destroy_count") == 0U);
  PS_CHECK(unicode_provider_registry
               ->load_provider(PS_DATA_PROVIDER_UNICODE_FIXTURE_PATH)
               .ok());
  auto unicode_schema = unicode_provider_registry->find(unicode_provider_key);
  PS_CHECK(unicode_schema.ok());
  DataSchemaDefinition retained_unicode_schema = unicode_schema.value();
  {
    LibraryHookScope lifecycle(ps::plugin_testing::LibraryKind::Provider,
                               false);
    unicode_provider_registry.reset();
    PS_CHECK(lifecycle.provider_schema_retirement_count() == 1U);
    PS_CHECK(lifecycle.retired_provider_schema_count() == 1U);
    PS_CHECK(lifecycle.provider_retired_before_close());
    PS_CHECK(lifecycle.native_close_count() == 1U);
  }
  PS_CHECK(unicode_provider_observer.counter(
               "ps_data_provider_utf8_fixture_destroy_count") == 1U);
  PS_CHECK(retained_unicode_schema.key == unicode_provider_key);
  PS_CHECK(retained_unicode_schema.element_type == ElementType::Float64);
  PS_CHECK(retained_unicode_schema.maximum_rank == 4U);

  LibraryObserver provider_observer(PS_DATA_PROVIDER_FIXTURE_PATH);
  PS_CHECK(provider_observer.counter(
               "ps_data_provider_fixture_destroy_count") == 0U);
  {
    LibraryHookScope failure(ps::plugin_testing::LibraryKind::Provider, true);
    bool allocation_failed = false;
    try {
      DataDefinitionRegistry registry;
      static_cast<void>(registry.load_provider(PS_DATA_PROVIDER_FIXTURE_PATH));
    } catch (const std::bad_alloc&) {
      allocation_failed = true;
    }
    PS_CHECK(allocation_failed);
    PS_CHECK(failure.owner_allocation_count() == 1U);
    PS_CHECK(failure.native_close_count() == 1U);
    PS_CHECK(provider_observer.counter(
                 "ps_data_provider_fixture_destroy_count") == 1U);
  }
  DataSchemaDefinition retained_provider_schema;
  auto provider_registry = std::make_unique<DataDefinitionRegistry>();
  PS_CHECK(
      provider_registry->load_provider(PS_DATA_PROVIDER_FIXTURE_PATH).ok());
  PS_CHECK(!provider_registry->load_provider(PS_DATA_PROVIDER_BAD_FIXTURE_PATH)
                .ok());
  PS_CHECK(provider_registry->freeze().ok());
  PS_CHECK(
      !provider_registry->load_provider(PS_DATA_PROVIDER_FIXTURE_PATH).ok());
  auto schema = provider_registry->find("fixture.float64");
  PS_CHECK(schema.ok());
  retained_provider_schema = schema.value();
  PS_CHECK(provider_observer.counter(
               "ps_data_provider_fixture_destroy_count") == 1U);
  {
    LibraryHookScope lifecycle(ps::plugin_testing::LibraryKind::Provider,
                               false);
    provider_registry.reset();
    PS_CHECK(lifecycle.provider_schema_retirement_count() == 1U);
    PS_CHECK(lifecycle.retired_provider_schema_count() == 1U);
    PS_CHECK(lifecycle.provider_retired_before_close());
    PS_CHECK(lifecycle.native_close_count() == 1U);
  }
  PS_CHECK(provider_observer.counter(
               "ps_data_provider_fixture_destroy_count") == 2U);
  PS_CHECK(retained_provider_schema.key == "fixture.float64");
  PS_CHECK(retained_provider_schema.element_type == ElementType::Float64);
  PS_CHECK(retained_provider_schema.maximum_rank == 4U);
  {
    DataDefinitionRegistry later_registry;
    PS_CHECK(later_registry.load_provider(PS_DATA_PROVIDER_FIXTURE_PATH).ok());
    PS_CHECK(later_registry.find("fixture.float64").ok());
  }
  PS_CHECK(provider_observer.counter(
               "ps_data_provider_fixture_destroy_count") == 3U);

  PS_CHECK(verify_cpp_fixed_broadcast(
               {std::numeric_limits<std::uint64_t>::max()}) == 0);
  PS_CHECK(verify_cpp_fixed_broadcast(
               {std::numeric_limits<std::uint64_t>::max(), 2U}) == 0);

  OperationRegistry fencing;
  OperationTraits conflicting_schema;
  conflicting_schema.parameter_schema = {
      OperationParameterSpec{"duplicate", OperationParameterType::Int64, true},
      OperationParameterSpec{"duplicate", OperationParameterType::Float64,
                             true},
  };
  PS_CHECK(!fencing
                .register_operation(OperationDefinition{
                    "fixture.conflicting_schema", conflicting_schema,
                    [](const OperationInvocation&) -> Result<Value> {
                      return Result<Value>(Value::from_float64(1.0));
                    }})
                .ok());
  PS_CHECK(fencing
               .register_operation(OperationDefinition{
                   "fixture.throw", OperationTraits{},
                   [](const OperationInvocation&) -> Result<Value> {
                     throw std::runtime_error("fixture exception");
                   }})
               .ok());
  PS_CHECK(fencing
               .register_operation(OperationDefinition{
                   "fixture.null_diagnostic", OperationTraits{},
                   [](const OperationInvocation&) -> Result<Value> {
                     throw NullDiagnosticException();
                   }})
               .ok());
  PS_CHECK(fencing
               .register_operation(OperationDefinition{
                   "fixture.bad_alloc", OperationTraits{},
                   [](const OperationInvocation&) -> Result<Value> {
                     throw std::bad_alloc();
                   }})
               .ok());
  PS_CHECK(fencing
               .register_operation(OperationDefinition{
                   "fixture.wrong_output", OperationTraits{},
                   [](const OperationInvocation&) -> Result<Value> {
                     auto output = Value::create(
                         ValueDescriptor{ElementType::UInt8, {1U}},
                         Region::whole({1U}), StridedLayout{0U, {1}},
                         std::vector<std::uint8_t>{7U});
                     return output;
                   }})
               .ok());
  PS_CHECK(fencing
               .register_operation(OperationDefinition{
                   "fixture.partial_output", OperationTraits{},
                   [](const OperationInvocation&) -> Result<Value> {
                     return Value::create(
                         ValueDescriptor{ElementType::Float64, {1U}},
                         Region({RegionDimension{0U, 0U}}),
                         StridedLayout{0U, {8}},
                         std::vector<std::uint8_t>(sizeof(double), 0U));
                   }})
               .ok());
  fencing.freeze();
  const std::vector<Value> no_inputs;
  const std::vector<Region> no_demands;
  const std::map<std::string, ParameterValue> no_parameters;
  auto exception = fencing.invoke(
      "fixture.throw", OperationInvocation{no_inputs, no_demands, no_parameters,
                                           Backend::Cpu, CancellationToken()});
  PS_CHECK(!exception.ok());
  PS_CHECK(exception.status().code == ErrorCode::OperationFailed);
  bool null_diagnostic_escaped = false;
  bool null_diagnostic_ok = true;
  ErrorCode null_diagnostic_code = ErrorCode::Ok;
  std::string null_diagnostic_message = "unobserved";
  try {
    auto null_diagnostic =
        fencing.invoke("fixture.null_diagnostic",
                       OperationInvocation{no_inputs, no_demands, no_parameters,
                                           Backend::Cpu, CancellationToken()});
    null_diagnostic_ok = null_diagnostic.ok();
    null_diagnostic_code = null_diagnostic.status().code;
    null_diagnostic_message = null_diagnostic.status().message;
  } catch (...) {
    null_diagnostic_escaped = true;
  }
  PS_CHECK(!null_diagnostic_escaped);
  PS_CHECK(!null_diagnostic_ok);
  PS_CHECK(null_diagnostic_code == ErrorCode::OperationFailed);
  PS_CHECK(null_diagnostic_message.empty());
  bool bad_alloc_propagated = false;
  try {
    static_cast<void>(
        fencing.invoke("fixture.bad_alloc",
                       OperationInvocation{no_inputs, no_demands, no_parameters,
                                           Backend::Cpu, CancellationToken()}));
  } catch (const std::bad_alloc&) {
    bad_alloc_propagated = true;
  }
  PS_CHECK(bad_alloc_propagated);
  auto wrong_output =
      fencing.invoke("fixture.wrong_output",
                     OperationInvocation{no_inputs, no_demands, no_parameters,
                                         Backend::Cpu, CancellationToken()});
  PS_CHECK(!wrong_output.ok());
  PS_CHECK(wrong_output.status().code == ErrorCode::TypeMismatch);
  auto partial_output =
      fencing.invoke("fixture.partial_output",
                     OperationInvocation{no_inputs, no_demands, no_parameters,
                                         Backend::Cpu, CancellationToken()});
  PS_CHECK(!partial_output.ok());
  PS_CHECK(partial_output.status().code == ErrorCode::TypeMismatch);
  return 0;
}
