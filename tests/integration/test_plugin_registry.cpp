#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <exception>
#include <iostream>
#include <limits>
#include <map>
#include <memory>
#include <new>
#include <stdexcept>
#include <string>
#include <utility>
#include <variant>
#include <vector>

#include "photospider/compiler/compiler.hpp"
#include "photospider/execution/execution.hpp"
#include "photospider/plugin/data_definition_registry.hpp"
#include "photospider/plugin/operation_registry.hpp"
#include "plugin/dense_layout_validation.hpp"
#include "plugin/library_test_hooks.hpp"
#include "support/operation_result_fixture.hpp"
#include "support/result_diagnostics_fixture.hpp"
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
using result_diagnostics::verify_cpp_fixed_broadcast;

/** @brief Fixture mode for output-free GPU backend unavailability. */
constexpr std::uint32_t kGpuBackendUnavailable = 1U;
/** @brief Fixture mode for accepted output followed by duplicate success. */
constexpr std::uint32_t kDuplicateValidThenSuccess = 6U;
/** @brief Fixture mode for rejected output followed by duplicate success. */
constexpr std::uint32_t kDuplicateInvalidThenSuccess = 7U;
/** @brief Fixture mode for null-context rejection followed by valid output. */
constexpr std::uint32_t kNullContextThenValid = 11U;

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

struct CopyAwareOutput final {
  ps::Result<ps::ResultProgramPoll> poll(
      const ps::ResultProgramPhase& phase) try {
    using multi_result::check;
    using multi_result::take;
    auto builder = take(ps::ResultBuilder::start(
        phase.resources, *phase.query.output.result_schema,
        phase.query.semantic_key));
    check(builder.bind_descriptor_relation(
        take(ps::ResultRelation::cartesian(phase.resources, 1, {}))));
    const double number = 41;
    check(builder.publish_tensor(
        0, ps::Region::whole({1}),
        ps::ByteView(reinterpret_cast<const std::uint8_t*>(&number),
                     sizeof(number)),
        take(ps::ResultRelation::cartesian(phase.resources, 1, {})),
        {true, true, true, true}));
    return ps::Result<ps::ResultProgramPoll>(
        ps::ResultPublication{take(builder.seal()), true});
  } catch (const multi_result::Failure& failure) {
    return ps::Result<ps::ResultProgramPoll>(failure.status);
  }
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
   * @brief Starts a deterministic Result producer while counting entry.
   * @param query Validated zero-input query.
   * @param allocator Host continuation allocator.
   * @return Continuation publishing a Float64 Result sample equal to 41.
   */
  ps::Result<ps::ResultContinuation> operator()(
      const ps::ResultProgramQuery& query,
      const ps::BufferAllocator& allocator) const {
    static_cast<void>(query);
    ++state_->call_count;
    return ps::ResultContinuation::make<CopyAwareOutput>(allocator);
  }

 private:
  /** @brief Shared deterministic copy/invocation observations. */
  std::shared_ptr<CopyAwareCallbackState> state_;
};

ps::OperationDefinition copy_aware_definition(
    std::string key, const std::shared_ptr<CopyAwareCallbackState>& state) {
  ps::OperationDefinition definition;
  definition.key = std::move(key);
  auto& output = definition.traits.outputs[0];
  output.output_schema.kind = ps::OperationPortKind::Result;
  output.result_schema = multi_result::schema();
  output.output_schema.result_schema_id = std::string(output.result_schema->id);
  output.output_schema.result_schema_version = output.result_schema->version;
  output.region_rule = ps::OperationRegionRule::Whole;
  output.dependency_version = 2;
  output.continuation_bytes = sizeof(CopyAwareOutput);
  output.maximum_dependency_stages = 1;
  definition.traits.workspace_bytes = 8;
  definition.start_result = CopyAwareCallback(state);
  return definition;
}

/** @brief Library kind expected by the currently installed test callback. */
LibraryKind g_expected_library_kind = LibraryKind::Operation;
/** @brief Exact owner-allocation callback invocation count. */
std::uint32_t g_owner_allocation_count = 0U;
/** @brief Exact native-load attempt callback invocation count. */
std::uint32_t g_native_load_count = 0U;
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
 * @brief Counts one owner-allocation boundary without injecting failure.
 * @param kind Library kind approaching heap owner allocation.
 * @return No value.
 * @throws Nothing.
 */
void count_owner_allocation(ps::plugin_testing::LibraryKind kind) noexcept {
  if (kind == g_expected_library_kind) {
    ++g_owner_allocation_count;
  }
}

/**
 * @brief Counts one platform native-load attempt for the expected kind.
 * @param kind Library kind about to reach the platform loader.
 * @return No value.
 * @throws Nothing.
 */
void count_native_load(ps::plugin_testing::LibraryKind kind) noexcept {
  if (kind == g_expected_library_kind) {
    ++g_native_load_count;
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
   * @param observe_path_boundaries Whether to count native load and owner
   * allocation boundaries without injecting failure.
   * @throws Nothing.
   */
  LibraryHookScope(ps::plugin_testing::LibraryKind kind, bool fail_allocation,
                   bool observe_path_boundaries = false) noexcept {
    g_expected_library_kind = kind;
    g_owner_allocation_count = 0U;
    g_native_load_count = 0U;
    g_native_close_count = 0U;
    g_provider_schema_retirement_count = 0U;
    g_retired_provider_schema_count = 0U;
    g_lifecycle_sequence = 0U;
    g_provider_schema_retirement_sequence = 0U;
    g_native_close_sequence = 0U;
    hooks_.before_owner_allocation =
        fail_allocation
            ? fail_owner_allocation
            : (observe_path_boundaries ? count_owner_allocation : nullptr);
    hooks_.native_load = observe_path_boundaries ? count_native_load : nullptr;
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
   * @brief Returns exact observed native-load attempt count.
   * @return Current scoped count.
   * @throws Nothing.
   */
  [[nodiscard]] std::uint32_t native_load_count() const noexcept {
    return g_native_load_count;
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

  /**
   * @brief Reads one exported mode-indexed uint32 observation function.
   * @param name Exact fixture observation symbol.
   * @param mode Closed callback-result fixture mode.
   * @return Current exported observation value for `mode`; depending on
   * `name`, it may encode a bit field, instantaneous state, or counter.
   * @throws std::runtime_error If the symbol is missing.
   * @note The observation is not necessarily monotonic. The mapped fixture
   * remains alive for this observer's lifetime.
   */
  [[nodiscard]] std::uint32_t counter(const char* name,
                                      std::uint32_t mode) const {
#if defined(_WIN32)
    void* address = reinterpret_cast<void*>(
        GetProcAddress(static_cast<HMODULE>(handle_), name));
#else
    void* address = dlsym(handle_, name);
#endif
    using Function = std::uint32_t (*)(std::uint32_t);
    Function function = nullptr;
    static_assert(sizeof(function) == sizeof(address),
                  "fixture target requires equal pointer sizes");
    std::memcpy(&function, &address, sizeof(function));
    if (!function) {
      throw std::runtime_error("fixture indexed counter symbol is missing");
    }
    return function(mode);
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
        const auto& output = traits.value().outputs[0];
        PS_CHECK(output.output_schema.kind == ps::OperationPortKind::Result);
        PS_CHECK(output.result_schema.has_value());
        const auto& schema = *output.result_schema;
        PS_CHECK(schema.tensors.empty() && schema.fields.size() == 1);
        PS_CHECK(schema.fields[0].element_type == ps::ElementType::UInt8);
        const auto& shape = schema.fields[0].record_shape;
        PS_CHECK(std::vector<std::uint64_t>(shape.begin(), shape.end()) ==
                 expected_shape);
        const auto bytes = schema.row_bytes(0);
        PS_CHECK(bytes.ok());
        PS_CHECK(bytes.value() == (expected_shape.size() == 1
                                       ? UINT64_C(9223372036854775807)
                                       : UINT64_C(9223372036854775806)));
      } else {
        PS_CHECK(loaded.code == ps::ErrorCode::TypeMismatch);
        PS_CHECK(registry.keys().empty());
      }
    }
    PS_CHECK(lifecycle.native_close_count() == 1U);
  }
  PS_CHECK(
      observer.counter("ps_operation_dense_limit_fixture_callback_count") == 0);
  PS_CHECK(observer.counter("ps_operation_dense_limit_fixture_destroy_count") ==
           1U);
  return 0;
}

/**
 * @brief Proves registry snapshots retain immutable callback handles.
 *
 * The first registry receives a copy-aware callable through rvalue
 * registration, arms copy rejection, freezes, and starts/polls its Result. The
 * second arms
 * an independently registered callable before loading a valid DSO into an
 * unfrozen registry. Both paths must copy only owning handles after arm.
 *
 * @return Zero when start/poll/load succeed without callable copies.
 * @throws std::bad_alloc If test or registry staging allocation fails.
 * @note The valid DSO load also preserves transactional publication and its
 * existing library lifetime anchor; the fixture is fully unloaded on return.
 */
int verify_immutable_callback_handles() {
  using ps::ErrorCode;
  using ps::OperationRegistry;
  using ps::ParameterValue;

  const std::map<std::string, ParameterValue> no_parameters;
  ps::ResourceBudget root;
  ps::ResourceAllocationScope scope(root);
  auto invoke_state = std::make_shared<CopyAwareCallbackState>();
  OperationRegistry invoke_registry;
  PS_CHECK(invoke_registry
               .register_operation(copy_aware_definition(
                   "fixture.copy_aware_invoke", invoke_state))
               .ok());
  invoke_state->reject_copies = true;
  const std::uint32_t invoke_copies_before_arm = invoke_state->copy_count;
  PS_CHECK(invoke_registry.freeze().ok());
  ps::ResultProgramMetadata metadata;
  metadata.output.result_schema =
      std::make_shared<const ps::SchemaTemplate>(multi_result::schema());
  ps::ResultProgramQuery query(metadata, no_parameters);
  query.semantic_key = "fixture.copy_aware_invoke";
  const auto allocator = root.allocator();
  ps::ResultObjectInputs objects;
  ps::ResourceVector<ps::ResultIoReply> io;
  ps::ResultProgramPhase phase{
      query, objects,
      io,    allocator,
      root,  [&](std::uint64_t work) { return root.consume({work}); },
      {}};
  bool invoke_escaped = false;
  bool invoke_succeeded = false;
  double invoked_value = 0.0;
  try {
    auto started = invoke_registry.start_result("fixture.copy_aware_invoke",
                                                query, allocator);
    if (started.ok()) {
      auto continuation = started.take_value();
      auto published = continuation.poll(phase);
      invoke_succeeded =
          published.ok() &&
          std::holds_alternative<ps::ResultPublication>(published.value());
      if (invoke_succeeded) {
        invoked_value = multi_result::number(
            std::get<ps::ResultPublication>(published.value()).result);
      }
    }
  } catch (...) {
    invoke_escaped = true;
  }
  PS_CHECK(!invoke_escaped);
  PS_CHECK(invoke_succeeded);
  PS_CHECK(invoked_value == 41.0);
  PS_CHECK(invoke_state->call_count == 1U);
  PS_CHECK(invoke_state->copy_count == invoke_copies_before_arm);

  auto invalid_metadata = metadata;
  invalid_metadata.inputs = {metadata.output};
  ps::ResultProgramQuery invalid_query(invalid_metadata, no_parameters);
  invalid_query.semantic_key = query.semantic_key;
  auto invalid_input = invoke_registry.start_result("fixture.copy_aware_invoke",
                                                    invalid_query, allocator);
  PS_CHECK(!invalid_input.ok());
  PS_CHECK(invalid_input.status().code == ErrorCode::InvalidArgument);
  PS_CHECK(invoke_state->call_count == 1U);
  PS_CHECK(invoke_state->copy_count == invoke_copies_before_arm);

  auto load_state = std::make_shared<CopyAwareCallbackState>();
  OperationRegistry load_registry;
  PS_CHECK(load_registry
               .register_operation(
                   copy_aware_definition("fixture.copy_aware_load", load_state))
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

int verify_duplicate_sink_invocation(ps::OperationRegistry& registry,
                                     const LibraryObserver& observer,
                                     const char* key, std::uint32_t mode,
                                     std::uint32_t expected_publish_bits) {
  const auto cpu_before =
      observer.counter("ps_operation_fixture_cpu_invocation_count", mode);
  const auto gpu_before =
      observer.counter("ps_operation_fixture_gpu_invocation_count", mode);
  auto result = operation_result::execute(registry, key);
  PS_CHECK(!result.ok());
  PS_CHECK(result.status().code == (mode == kDuplicateInvalidThenSuccess
                                        ? ps::ErrorCode::InvalidArgument
                                        : ps::ErrorCode::OperationFailed));
  PS_CHECK(observer.counter("ps_operation_fixture_publish_result_bits", mode) ==
           expected_publish_bits);
  PS_CHECK(observer.counter("ps_operation_fixture_cpu_invocation_count",
                            mode) == cpu_before + 1);
  PS_CHECK(observer.counter("ps_operation_fixture_gpu_invocation_count",
                            mode) == gpu_before);
  return 0;
}
int verify_null_sink_context(ps::OperationRegistry& registry,
                             const LibraryObserver& observer) {
  const auto cpu_before = observer.counter(
      "ps_operation_fixture_cpu_invocation_count", kNullContextThenValid);
  auto result =
      operation_result::execute(registry, "fixture.null_context_then_valid");
  PS_CHECK(result.ok());
  PS_CHECK(multi_result::number(result.value().results.at("value")) == 3);
  PS_CHECK(observer.counter("ps_operation_fixture_publish_result_bits",
                            kNullContextThenValid) == 1);
  PS_CHECK(observer.counter("ps_operation_fixture_cpu_invocation_count",
                            kNullContextThenValid) == cpu_before + 1);
  return 0;
}

struct PrevalidatedOutput final {
  unsigned mode;
  explicit PrevalidatedOutput(unsigned mode = 0) : mode(mode) {}
  ps::Result<ps::ResultProgramPoll> poll(const ps::ResultProgramPhase& phase) {
    using multi_result::check;
    using multi_result::take;
    if (mode == 1)
      return ps::Result<ps::ResultProgramPoll>(ps::ResultPublication{{}, true});
    auto schema = *phase.query.output.result_schema;
    if (mode == 2)
      schema.tensors[0].descriptor.element_type = ps::ElementType::UInt8;
    auto builder = take(ps::ResultBuilder::start(phase.resources, schema,
                                                 phase.query.semantic_key));
    check(builder.bind_descriptor_relation(
        take(ps::ResultRelation::cartesian(phase.resources, 1, {}))));
    const double number = 17;
    const std::uint8_t byte = 17;
    if (mode != 3)
      check(builder.publish_tensor(
          0, ps::Region::whole({1}),
          mode == 2
              ? ps::ByteView(&byte, 1)
              : ps::ByteView(reinterpret_cast<const std::uint8_t*>(&number),
                             sizeof(number)),
          take(ps::ResultRelation::cartesian(phase.resources, 1, {})),
          {true, true, true, true}));
    return ps::Result<ps::ResultProgramPoll>(
        ps::ResultPublication{take(builder.seal()), true});
  }
};
ps::OperationDefinition prevalidation_definition(std::string key,
                                                 unsigned input_count,
                                                 unsigned* starts,
                                                 unsigned mode = 0) {
  ps::OperationDefinition operation;
  operation.key = std::move(key);
  operation.traits.input_count = input_count;
  operation.traits.input_schema.resize(input_count);
  for (auto& port : operation.traits.input_schema) {
    port.kind = ps::OperationPortKind::Result;
    port.result_schema_id = "test.multi_output";
    port.result_schema_version = 1;
  }
  operation.traits.requires_metadata_specialization = true;
  operation.traits.outputs = {multi_result::output("value")};
  operation.traits.outputs[0].region_rule = ps::OperationRegionRule::Whole;
  operation.prepare_static = [](const auto& inputs, const auto&) {
    for (const auto& input : inputs) {
      if (!input.result_schema)
        return ps::Result<ps::OperationPreparation>(ps::Status{
            ps::ErrorCode::InvalidArgument, "missing Result metadata"});
      const auto& tensors = input.result_schema->tensors;
      if (tensors.size() != 1 ||
          tensors[0].descriptor.element_type != ps::ElementType::Float64 ||
          tensors[0].sample_shape() != std::vector<std::uint64_t>{1})
        return ps::Result<ps::OperationPreparation>(
            ps::Status{ps::ErrorCode::TypeMismatch, "expected Float64 scalar"});
    }
    ps::OperationPreparation preparation;
    preparation.outputs.resize(1);
    preparation.outputs[0].metadata.result_schema =
        std::make_shared<const ps::SchemaTemplate>(multi_result::schema());
    return ps::Result<ps::OperationPreparation>(std::move(preparation));
  };
  operation.start_result = [starts, mode](const auto&, const auto& allocator) {
    ++*starts;
    return ps::ResultContinuation::make<PrevalidatedOutput>(allocator, mode);
  };
  return operation;
}
int verify_cpp_invocation_prevalidation() {
  using namespace ps;  // NOLINT(build/namespaces)
  unsigned preserve_starts = 0, match_starts = 0, backend_starts = 0;
  unsigned default_starts = 0;
  auto registry = std::make_shared<OperationRegistry>();
  PS_CHECK(registry
               ->register_operation(prevalidation_definition(
                   "fixture.preserve_prevalidation", 1, &preserve_starts))
               .ok());
  PS_CHECK(registry
               ->register_operation(prevalidation_definition(
                   "fixture.match_prevalidation", 2, &match_starts))
               .ok());
  PS_CHECK(registry
               ->register_operation(prevalidation_definition(
                   "fixture.backend_prevalidation", 0, &backend_starts))
               .ok());
  PS_CHECK(registry
               ->register_operation(prevalidation_definition(
                   "fixture.default_output", 0, &default_starts, 1))
               .ok());
  PS_CHECK(registry->freeze().ok());
  ResourceBudget root;
  const auto allocator = root.allocator();
  const std::map<std::string, ParameterValue> parameters;
  const auto metadata = [](ElementType type,
                           std::vector<std::uint64_t> shape = {1}) {
    OperationMetadata result;
    result.result_schema = std::make_shared<const SchemaTemplate>(
        multi_result::schema(type, std::move(shape)));
    return result;
  };
  ResultProgramMetadata info;
  info.output = metadata(ElementType::Float64);
  info.inputs = {metadata(ElementType::UInt8)};
  ResultProgramQuery query(info, parameters);
  query.semantic_key = "prevalidation";
  query.backend = Backend::Gpu;
  auto gpu = registry->start_result("fixture.preserve_prevalidation", query,
                                    allocator);
  PS_CHECK(!gpu.ok() && gpu.status().code == ErrorCode::BackendUnavailable);
  query.backend = Backend::Cpu;
  auto preserve = registry->start_result("fixture.preserve_prevalidation",
                                         query, allocator);
  PS_CHECK(!preserve.ok() && preserve.status().code == ErrorCode::TypeMismatch);
  PS_CHECK(preserve_starts == 0);
  info.inputs = {metadata(ElementType::Float64), metadata(ElementType::UInt8)};
  auto type =
      registry->start_result("fixture.match_prevalidation", query, allocator);
  PS_CHECK(!type.ok() && type.status().code == ErrorCode::TypeMismatch);
  info.inputs[1] = metadata(ElementType::Float64, {2});
  auto shape =
      registry->start_result("fixture.match_prevalidation", query, allocator);
  PS_CHECK(!shape.ok() && shape.status().code == ErrorCode::TypeMismatch);
  for (unsigned invalid : {0U, 1U}) {
    info.inputs = {metadata(ElementType::Float64),
                   metadata(ElementType::Float64)};
    info.inputs[invalid] = {};
    bool escaped = false;
    Status status;
    try {
      auto result = registry->start_result("fixture.match_prevalidation", query,
                                           allocator);
      status = result.status();
    } catch (...) {
      escaped = true;
    }
    PS_CHECK(!escaped && status.code == ErrorCode::TypeMismatch);
  }
  PS_CHECK(match_starts == 0);
  info.inputs.clear();
  query.backend = static_cast<Backend>(99);
  auto unknown =
      registry->start_result("fixture.backend_prevalidation", query, allocator);
  PS_CHECK(!unknown.ok() &&
           unknown.status().code == ErrorCode::InvalidArgument);
  query.backend = Backend::Gpu;
  auto unsupported =
      registry->start_result("fixture.backend_prevalidation", query, allocator);
  PS_CHECK(!unsupported.ok() &&
           unsupported.status().code == ErrorCode::BackendUnavailable);
  PS_CHECK(backend_starts == 0);
  query.backend = Backend::Cpu;
  auto valid =
      registry->start_result("fixture.backend_prevalidation", query, allocator);
  PS_CHECK(valid.ok() && backend_starts == 1);
  ResultObjectInputs objects;
  ResourceVector<ResultIoReply> io;
  ResultProgramPhase phase{query, objects,
                           io,    allocator,
                           root,  [&](auto n) { return root.consume({n}); },
                           {}};
  auto continuation = valid.take_value();
  auto published = continuation.poll(phase);
  PS_CHECK(published.ok() &&
           std::holds_alternative<ResultPublication>(published.value()));
  PS_CHECK(multi_result::number(
               std::get<ResultPublication>(published.value()).result) == 17);
  WorkflowDocument document;
  document.nodes = {{1, "fixture.default_output", {}, {}}};
  document.outputs = {{"value", 1, "value"}};
  GraphContext graph(document);
  auto compiled = Compiler(registry).compile(graph);
  PS_CHECK(compiled.ok());
  ExecutionContextConfig config;
  config.managed_resources = ResourceLimits{};
  ExecutionContext context(registry, config);
  auto invalid_result = context.execute(compiled.value().plan);
  PS_CHECK(!invalid_result.ok() &&
           invalid_result.status().code == ErrorCode::InvalidArgument);
  PS_CHECK(default_starts == 1);
  return 0;
}

int verify_tensor_extent_dso() {
  const std::array<const char*, 3> paths{
      PS_OPERATION_TENSOR_EXTENT_FIXTURE_PATH,
      PS_OPERATION_TENSOR_ZERO_FIXTURE_PATH,
      PS_OPERATION_TENSOR_RANK_FIXTURE_PATH};
  const std::vector<std::vector<std::uint64_t>> expected{
      {UINT64_C(1) << 63U},
      {(UINT64_C(1) << 63U) + 1},
      {2, UINT64_C(1) << 62U},
      {2, (UINT64_C(1) << 62U) + 1},
      {UINT64_MAX},
      {UINT64_MAX, UINT64_MAX}};
  for (std::size_t i = 0; i < paths.size(); ++i) {
    LibraryObserver observer(paths[i]);
    PS_CHECK(observer.counter(
                 "ps_operation_tensor_extent_fixture_destroy_count") == 0);
    LibraryHookScope lifecycle(LibraryKind::Operation, false, true);
    {
      ps::OperationRegistry registry;
      const auto status = registry.load_plugin(paths[i]);
      if (i == 0) {
        PS_CHECK(status.ok());
        auto traits = registry.find_traits("fixture.tensor.extents");
        PS_CHECK(traits.ok() && traits.value().outputs.size() == 1);
        const auto& output = traits.value().outputs[0];
        PS_CHECK(output.output_schema.kind == ps::OperationPortKind::Result);
        PS_CHECK(output.result_schema.has_value());
        const auto& schema = *output.result_schema;
        PS_CHECK(schema.fields.empty() &&
                 schema.tensors.size() == expected.size());
        for (std::size_t tensor = 0; tensor < expected.size(); ++tensor) {
          const auto& shape = schema.tensors[tensor].descriptor.shape;
          PS_CHECK(std::vector<std::uint64_t>(shape.begin(), shape.end()) ==
                   expected[tensor]);
          PS_CHECK(schema.tensors[tensor].descriptor.element_type ==
                   (tensor == 4 ? ps::ElementType::Float64
                                : ps::ElementType::UInt8));
        }
      } else {
        PS_CHECK(status.code == (i == 1 ? ps::ErrorCode::TypeMismatch
                                        : ps::ErrorCode::InvalidArgument));
        PS_CHECK(registry.keys().empty());
      }
    }
    PS_CHECK(lifecycle.owner_allocation_count() == 1);
    PS_CHECK(lifecycle.native_load_count() == 1);
    PS_CHECK(lifecycle.native_close_count() == 1);
    PS_CHECK(observer.counter(
                 "ps_operation_tensor_extent_fixture_callback_count") == 0);
    PS_CHECK(observer.counter(
                 "ps_operation_tensor_extent_fixture_destroy_count") == 1);
  }
  return 0;
}

int verify_result_registration() {
  using ps::OperationRegistry;
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
                                 false, true);
      const auto status = unicode_operation_registry->load_plugin(fixture_path);
      PS_CHECK(status.code == ps::ErrorCode::InvalidArgument);
      PS_CHECK(lifecycle.owner_allocation_count() == 1U);
      PS_CHECK(lifecycle.native_load_count() == 1U);
      PS_CHECK(lifecycle.native_close_count() == 1U);
    }
    PS_CHECK(unicode_operation_registry->keys().empty());
    PS_CHECK(observer.counter("ps_operation_utf8_fixture_destroy_count") == 1U);
  }

  LibraryObserver unicode_operation_observer(PS_OPERATION_UNICODE_FIXTURE_PATH);
  PS_CHECK(unicode_operation_observer.counter(
               "ps_operation_utf8_fixture_destroy_count") == 0U);
  {
    LibraryHookScope failure(LibraryKind::Operation, true);
    bool allocation_failed = false;
    try {
      static_cast<void>(unicode_operation_registry->load_plugin(
          PS_OPERATION_UNICODE_FIXTURE_PATH));
    } catch (const std::bad_alloc&) {
      allocation_failed = true;
    }
    PS_CHECK(allocation_failed);
    PS_CHECK(failure.owner_allocation_count() == 1U);
    PS_CHECK(failure.native_close_count() == 1U);
    PS_CHECK(unicode_operation_registry->keys().empty());
    PS_CHECK(unicode_operation_observer.counter(
                 "ps_operation_utf8_fixture_destroy_count") == 1U);
  }
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
  PS_CHECK(unicode_traits.value().outputs.size() == 1U);
  PS_CHECK(unicode_traits.value().outputs.front().output_schema.kind ==
           ps::OperationPortKind::Result);
  PS_CHECK(unicode_traits.value().outputs.front().result_schema.has_value());
  const auto keys = unicode_operation_registry->keys();
  const std::array<const char*, 5> invalid_parameters{
      PS_OPERATION_BAD_PARAMETER_POINTER_FIXTURE_PATH,
      PS_OPERATION_BAD_PARAMETER_SIZE_FIXTURE_PATH,
      PS_OPERATION_BAD_PARAMETER_COUNT_FIXTURE_PATH,
      PS_OPERATION_BAD_PARAMETER_BOUNDS_FIXTURE_PATH,
      PS_OPERATION_BAD_PARAMETER_ALIGNMENT_FIXTURE_PATH};
  for (const auto* path : invalid_parameters) {
    LibraryHookScope lifecycle(LibraryKind::Operation, false);
    const auto status = unicode_operation_registry->load_plugin(path);
    PS_CHECK(status.code == ps::ErrorCode::InvalidArgument);
    PS_CHECK(unicode_operation_registry->keys() == keys);
    PS_CHECK(lifecycle.native_close_count() == 1U);
  }
  {
    LibraryHookScope lifecycle(LibraryKind::Operation, false);
    const auto duplicate = unicode_operation_registry->load_plugin(
        PS_OPERATION_UNICODE_FIXTURE_PATH);
    PS_CHECK(duplicate.code == ps::ErrorCode::InvalidArgument);
    PS_CHECK(unicode_operation_registry->keys() == keys);
    PS_CHECK(lifecycle.native_close_count() == 1U);
    PS_CHECK(unicode_operation_observer.counter(
                 "ps_operation_utf8_fixture_destroy_count") == 2U);
  }
  {
    LibraryHookScope lifecycle(ps::plugin_testing::LibraryKind::Operation,
                               false);
    unicode_operation_registry.reset();
    PS_CHECK(lifecycle.native_close_count() == 1U);
  }
  PS_CHECK(unicode_operation_observer.counter(
               "ps_operation_utf8_fixture_destroy_count") == 3U);

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
  using ps::ValueDescriptor;
  using ps::ValueFacet;
  using ps::WorkflowDocument;
  using ps::WorkflowNode;
  using ps::WorkflowNodeOutput;
  using ps::WorkflowOutput;

  PS_CHECK(verify_immutable_callback_handles() == 0);
  PS_CHECK(verify_cpp_invocation_prevalidation() == 0);

  std::string nul_operation_path(PS_OPERATION_FIXTURE_PATH,
                                 std::strlen(PS_OPERATION_FIXTURE_PATH));
  nul_operation_path.append("\0suffix", 7U);
  ps::Status nul_operation_status;
  bool nul_operation_registry_empty = false;
  std::uint32_t nul_operation_owner_count = 0U;
  std::uint32_t nul_operation_load_count = 0U;
  std::uint32_t nul_operation_close_count = 0U;
  LibraryObserver nul_operation_observer(PS_OPERATION_FIXTURE_PATH);
  {
    LibraryHookScope lifecycle(ps::plugin_testing::LibraryKind::Operation,
                               false, true);
    {
      OperationRegistry registry;
      nul_operation_status = registry.load_plugin(nul_operation_path);
      nul_operation_registry_empty = registry.keys().empty();
    }
    nul_operation_owner_count = lifecycle.owner_allocation_count();
    nul_operation_load_count = lifecycle.native_load_count();
    nul_operation_close_count = lifecycle.native_close_count();
  }

  std::string nul_provider_path(PS_DATA_PROVIDER_FIXTURE_PATH,
                                std::strlen(PS_DATA_PROVIDER_FIXTURE_PATH));
  nul_provider_path.append("\0suffix", 7U);
  ps::Status nul_provider_status;
  bool nul_provider_schema_absent = false;
  std::uint32_t nul_provider_owner_count = 0U;
  std::uint32_t nul_provider_load_count = 0U;
  std::uint32_t nul_provider_close_count = 0U;
  std::size_t nul_provider_retired_schema_count = 0U;
  LibraryObserver nul_provider_observer(PS_DATA_PROVIDER_FIXTURE_PATH);
  {
    LibraryHookScope lifecycle(ps::plugin_testing::LibraryKind::Provider, false,
                               true);
    {
      DataDefinitionRegistry registry;
      nul_provider_status = registry.load_provider(nul_provider_path);
      nul_provider_schema_absent = !registry.find("fixture.float64").ok();
    }
    nul_provider_owner_count = lifecycle.owner_allocation_count();
    nul_provider_load_count = lifecycle.native_load_count();
    nul_provider_close_count = lifecycle.native_close_count();
    nul_provider_retired_schema_count =
        lifecycle.retired_provider_schema_count();
  }

  PS_CHECK(!nul_operation_status.ok());
  PS_CHECK(nul_operation_status.code == ErrorCode::InvalidArgument);
  PS_CHECK(nul_operation_registry_empty);
  PS_CHECK(nul_operation_owner_count == 0U);
  PS_CHECK(nul_operation_load_count == 0U);
  PS_CHECK(nul_operation_close_count == 0U);
  PS_CHECK(nul_operation_observer.counter(
               "ps_operation_fixture_destroy_count") == 0U);
  PS_CHECK(!nul_provider_status.ok());
  PS_CHECK(nul_provider_status.code == ErrorCode::InvalidArgument);
  PS_CHECK(nul_provider_schema_absent);
  PS_CHECK(nul_provider_owner_count == 0U);
  PS_CHECK(nul_provider_load_count == 0U);
  PS_CHECK(nul_provider_close_count == 0U);
  PS_CHECK(nul_provider_retired_schema_count == 0U);
  PS_CHECK(nul_provider_observer.counter(
               "ps_data_provider_fixture_destroy_count") == 0U);

  std::string missing_operation_path(PS_OPERATION_FIXTURE_PATH,
                                     std::strlen(PS_OPERATION_FIXTURE_PATH));
  missing_operation_path += ".photospider-missing";
  {
    LibraryHookScope lifecycle(ps::plugin_testing::LibraryKind::Operation,
                               false, true);
    OperationRegistry registry;
    const ps::Status missing = registry.load_plugin(missing_operation_path);
    PS_CHECK(!missing.ok());
    PS_CHECK(missing.code == ErrorCode::NotFound);
    PS_CHECK(registry.keys().empty());
    PS_CHECK(lifecycle.owner_allocation_count() == 0U);
    PS_CHECK(lifecycle.native_load_count() == 1U);
    PS_CHECK(lifecycle.native_close_count() == 0U);
  }

  std::string missing_provider_path(PS_DATA_PROVIDER_FIXTURE_PATH,
                                    std::strlen(PS_DATA_PROVIDER_FIXTURE_PATH));
  missing_provider_path += ".photospider-missing";
  {
    LibraryHookScope lifecycle(ps::plugin_testing::LibraryKind::Provider, false,
                               true);
    DataDefinitionRegistry registry;
    const ps::Status missing = registry.load_provider(missing_provider_path);
    PS_CHECK(!missing.ok());
    PS_CHECK(missing.code == ErrorCode::NotFound);
    PS_CHECK(!registry.find("fixture.float64").ok());
    PS_CHECK(lifecycle.owner_allocation_count() == 0U);
    PS_CHECK(lifecycle.native_load_count() == 1U);
    PS_CHECK(lifecycle.native_close_count() == 0U);
  }

  PS_CHECK(verify_result_registration() == 0);

  LibraryObserver dense_overflow_observer(
      PS_OPERATION_DENSE_OVERFLOW_FIXTURE_PATH);
  PS_CHECK(dense_overflow_observer.counter(
               "ps_operation_dense_overflow_fixture_destroy_count") == 0U);
  {
    LibraryHookScope lifecycle(ps::plugin_testing::LibraryKind::Operation,
                               false);
    OperationRegistry registry;
    PS_CHECK(
        registry.load_plugin(PS_OPERATION_DENSE_OVERFLOW_FIXTURE_PATH).code ==
        ErrorCode::TypeMismatch);
    PS_CHECK(registry.keys().empty());
    PS_CHECK(lifecycle.native_close_count() == 1U);
  }
  PS_CHECK(dense_overflow_observer.counter(
               "ps_operation_dense_overflow_fixture_destroy_count") == 1U);
  PS_CHECK(dense_overflow_observer.counter(
               "ps_operation_dense_overflow_fixture_callback_count") == 0U);
  PS_CHECK(verify_tensor_extent_dso() == 0);

  PS_CHECK(verify_dense_byte_size_contract() == 0);
  const std::uint64_t maximum_legal_dense_bytes = INT64_MAX;
  PS_CHECK(verify_dense_dso_fixture(PS_OPERATION_DENSE_RANK1_LIMIT_FIXTURE_PATH,
                                    true, "fixture.dense_rank1_limit",
                                    {maximum_legal_dense_bytes}) == 0);
  PS_CHECK(verify_dense_dso_fixture(
               PS_OPERATION_DENSE_RANK1_OVERFLOW_FIXTURE_PATH, false, "", {}) ==
           0);
  PS_CHECK(verify_dense_dso_fixture(PS_OPERATION_DENSE_RANK2_LIMIT_FIXTURE_PATH,
                                    true, "fixture.dense_rank2_limit",
                                    {UINT64_C(2), (UINT64_C(1) << 62U) - 1}) ==
           0);
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
    LibraryObserver bad_version(PS_OPERATION_BAD_FIXTURE_PATH);
    PS_CHECK(bad_version.counter("ps_bad_operation_api_calls") == 0);
    PS_CHECK(!registry.load_plugin(PS_OPERATION_BAD_FIXTURE_PATH).ok());
    PS_CHECK(bad_version.counter("ps_bad_operation_api_calls") == 1);
    PS_CHECK(registry.freeze().ok());
    PS_CHECK(!registry.load_plugin(PS_OPERATION_FIXTURE_PATH).ok());
    auto traits = registry.find_traits("fixture.double");
    PS_CHECK(traits.ok());
    PS_CHECK(traits.value().input_count == 1U);
    PS_CHECK(traits.value().version == OperationTraits{}.version);
    PS_CHECK(traits.value().parameter_schema.size() == 1U);
    PS_CHECK(traits.value().parameter_schema.front().key == "scale");
    PS_CHECK(traits.value().parameter_schema.front().type ==
             OperationParameterType::Float64);
    PS_CHECK(traits.value().parameter_schema.front().required);

    Compiler plugin_compiler(std::shared_ptr<OperationRegistry>(
        &registry, [](OperationRegistry*) {}));
    auto plugin_document =
        operation_result::document("fixture.double", {{"scale", 2.0}}, true);
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

    const std::map<std::string, ParameterValue> parameters{{"scale", 2.0}};
    auto output =
        operation_result::execute(registry, "fixture.double", parameters, true);
    PS_CHECK(output.ok());
    const auto& result = output.value().results.at("value");
    PS_CHECK(multi_result::number(result) == 6);
    const auto& facets = result.schema().tensors[0].facets;
    PS_CHECK(facets.size() == 1 && facets[0].key == "test.semantic" &&
             facets[0].version == 2);
    PS_CHECK(facets[0].payload == std::vector<std::uint8_t>({8, 9}));
    auto trailing_rejected =
        operation_result::execute(registry, "fixture.bad_bytes");
    PS_CHECK(!trailing_rejected.ok() &&
             trailing_rejected.status().code == ErrorCode::TypeMismatch);
    const std::map<std::string, ParameterValue> no_parameters;
    const auto gpu_before = operation_observer.counter(
        "ps_operation_fixture_gpu_invocation_count", kGpuBackendUnavailable);
    const auto cpu_before = operation_observer.counter(
        "ps_operation_fixture_cpu_invocation_count", kGpuBackendUnavailable);
    auto invalid_input = operation_result::start(
        registry, "fixture.gpu_fallback", no_parameters, Backend::Gpu, true);
    PS_CHECK(!invalid_input.ok() &&
             invalid_input.status().code == ErrorCode::TypeMismatch);
    auto invalid_backend =
        operation_result::start(registry, "fixture.gpu_fallback", no_parameters,
                                static_cast<Backend>(99));
    PS_CHECK(!invalid_backend.ok() &&
             invalid_backend.status().code == ErrorCode::InvalidArgument);
    PS_CHECK(
        operation_observer.counter("ps_operation_fixture_gpu_invocation_count",
                                   kGpuBackendUnavailable) == gpu_before);
    PS_CHECK(
        operation_observer.counter("ps_operation_fixture_cpu_invocation_count",
                                   kGpuBackendUnavailable) == cpu_before);
    auto unsupported = operation_result::start(registry, "fixture.double",
                                               parameters, Backend::Gpu);
    PS_CHECK(!unsupported.ok() &&
             unsupported.status().code == ErrorCode::BackendUnavailable);
    for (const auto& bad : std::vector<std::map<std::string, ParameterValue>>{
             {},
             {{"factor", 2.0}},
             {{"scale", int64_t{2}}}}) {
      auto rejected = operation_result::start(registry, "fixture.double", bad,
                                              Backend::Cpu);
      PS_CHECK(!rejected.ok() &&
               rejected.status().code == ErrorCode::InvalidArgument);
    }
    auto bad_facet = operation_result::execute(registry, "fixture.bad_facet");
    PS_CHECK(!bad_facet.ok() &&
             bad_facet.status().code == ErrorCode::InvalidArgument);
    PS_CHECK(verify_duplicate_sink_invocation(
                 registry, operation_observer, "fixture.duplicate_success",
                 kDuplicateValidThenSuccess, 2U) == 0);
    PS_CHECK(verify_duplicate_sink_invocation(
                 registry, operation_observer,
                 "fixture.duplicate_invalid_success",
                 kDuplicateInvalidThenSuccess, 0U) == 0);
    PS_CHECK(verify_null_sink_context(registry, operation_observer) == 0);
  }
  PS_CHECK(operation_observer.counter("ps_operation_fixture_destroy_count") ==
           2U);
  const std::uint32_t destroy_before_retirement =
      operation_observer.counter("ps_operation_fixture_destroy_count");
  {
    LibraryHookScope lifecycle(ps::plugin_testing::LibraryKind::Operation,
                               false);
    {
      OperationRegistry retirement_registry;
      PS_CHECK(retirement_registry.load_plugin(PS_OPERATION_FIXTURE_PATH).ok());
    }
    PS_CHECK(lifecycle.native_close_count() == 1U);
  }
  PS_CHECK(operation_observer.counter("ps_operation_fixture_destroy_count") ==
           destroy_before_retirement + 1U);

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

  PS_CHECK(verify_cpp_fixed_broadcast({4}) == 0);
  PS_CHECK(verify_cpp_fixed_broadcast({UINT64_MAX}, true) == 0);
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
  auto conflicting = copy_aware_definition(
      "fixture.conflicting_schema", std::make_shared<CopyAwareCallbackState>());
  conflicting.traits.parameter_schema = conflicting_schema.parameter_schema;
  PS_CHECK(!fencing.register_operation(std::move(conflicting)).ok());
  unsigned wrong_starts = 0, partial_starts = 0;
  PS_CHECK(fencing
               .register_operation(prevalidation_definition(
                   "fixture.wrong_output", 0, &wrong_starts, 2))
               .ok());
  PS_CHECK(fencing
               .register_operation(prevalidation_definition(
                   "fixture.partial_output", 0, &partial_starts, 3))
               .ok());
  PS_CHECK(fencing.freeze().ok());
  auto borrowed =
      std::shared_ptr<OperationRegistry>(&fencing, [](OperationRegistry*) {});
  ps::ExecutionContextConfig config;
  config.managed_resources = ps::ResourceLimits{};
  ps::ExecutionContext context(borrowed, config);
  for (const char* key : {"fixture.wrong_output", "fixture.partial_output"}) {
    WorkflowDocument document;
    document.nodes = {{1, key, {}, {}}};
    document.outputs = {{"value", 1, "value"}};
    GraphContext graph(document);
    auto compiled = Compiler(borrowed).compile(graph);
    PS_CHECK(compiled.ok());
    auto failed = context.execute(compiled.value().plan);
    PS_CHECK(!failed.ok() &&
             failed.status().code == ErrorCode::InvalidArgument);
    PS_CHECK(failed.status().detail.origin == ps::FailureOrigin::Protocol);
  }
  PS_CHECK(wrong_starts == 1 && partial_starts == 1);
  return 0;
}
