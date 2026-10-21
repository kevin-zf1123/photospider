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

#include "core/dense_layout_validation.hpp"
#include "photospider/compiler/compiler.hpp"
#include "photospider/execution/execution.hpp"
#include "photospider/plugin/operation_registry.hpp"
#include "plugin/library_test_hooks.hpp"
#include "support/operation_result_fixture.hpp"
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

namespace {

using ps::plugin_testing::LibraryKind;

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

  /** @brief Count entry while lowering a fixed source task graph. */
  ps::Result<ps::OperationProgram> operator()(
      const ps::OperationLowering& query) const {
    ++state_->call_count;
    ps::OperationProgramBuilder builder(query);
    for (const auto& member : query.requests[0].members)
      for (auto tile : member.second) {
        auto task = builder.add_task("constant", "multi-results-v1");
        if (!task.ok())
          return ps::Result<ps::OperationProgram>(task.status());
        auto status = builder.write_output(task.value(), 0, member.first, tile);
        if (!status.ok())
          return ps::Result<ps::OperationProgram>(status);
      }
    return builder.finish();
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

  output.result_schema = multi_result::schema();
  output.output_schema.result_schema_id = std::string(output.result_schema->id);
  output.output_schema.result_schema_version = output.result_schema->version;
  multi_result::install_fixed(definition, {-1}, {41});
  definition.lower = CopyAwareCallback(state);
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
  }
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
   * @param kind Operation lifecycle to observe.
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
    hooks_.before_owner_allocation =
        fail_allocation
            ? fail_owner_allocation
            : (observe_path_boundaries ? count_owner_allocation : nullptr);
    hooks_.native_load = observe_path_boundaries ? count_native_load : nullptr;
    hooks_.native_close = count_native_close;
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
  static_assert(ps::core_internal::dense_byte_size_representable<std::uint32_t>(
                    kUint32Maximum),
                "32-bit size maximum must remain inclusive");
  static_assert(
      !ps::core_internal::dense_byte_size_representable<std::uint32_t>(
          kUint32Maximum + UINT64_C(1)),
      "32-bit size overflow must be rejected");
  PS_CHECK(ps::core_internal::dense_byte_size_representable<std::uint64_t>(
      kMaximumLegalSignedRange));
  PS_CHECK(!ps::core_internal::dense_byte_size_representable<std::uint64_t>(
      kMaximumLegalSignedRange + UINT64_C(1)));
  PS_CHECK(!ps::core_internal::dense_byte_size_representable<std::uint32_t>(
      UINT64_C(0)));
  return 0;
}

/** @brief Published callbacks retain identity through compile and module load.
 */
int verify_immutable_callback_handles() {
  auto state = std::make_shared<CopyAwareCallbackState>();
  auto registry = std::make_shared<ps::OperationRegistry>();
  PS_REQUIRE_OK(registry->register_operation(
      copy_aware_definition("fixture.copy_aware_invoke", state)));
  state->reject_copies = true;
  const auto copies = state->copy_count;
  PS_REQUIRE_OK(registry->freeze());
  PS_REQUIRE_OK(registry->find_traits("fixture.copy_aware_invoke"));
  ps::WorkflowDocument document;
  document.nodes = {{1, "fixture.copy_aware_invoke", {}, {}}};
  document.outputs = {{"value", 1, "value"}};
  ps::GraphContext graph(document);
  auto plan = ps::Compiler(registry).compile(graph);
  PS_REQUIRE_OK(plan);
  PS_CHECK(state->call_count > 0);
  const auto calls = state->call_count;
  ps::ExecutionContext context(registry);
  auto result = multi_result::run(context, plan.value().plan.value());
  PS_REQUIRE_OK(result);
  PS_CHECK(multi_result::number(result.value().results.at("value")) == 41);
  PS_CHECK(state->call_count == calls && state->copy_count == copies);
  auto invalid = registry->prepare_operation(
      "fixture.copy_aware_invoke",
      {{std::make_shared<const ps::SchemaTemplate>(multi_result::schema())}},
      {});
  PS_CHECK(!invalid.ok() &&
           invalid.status().code == ps::ErrorCode::InvalidArgument);
  PS_CHECK(state->call_count == calls && state->copy_count == copies);

  auto load_state = std::make_shared<CopyAwareCallbackState>();
  ps::OperationRegistry load_registry;
  PS_REQUIRE_OK(load_registry.register_operation(
      copy_aware_definition("fixture.copy_aware_load", load_state)));
  load_state->reject_copies = true;
  const auto load_copies = load_state->copy_count;
  PS_REQUIRE_OK(load_registry.load_plugin(PS_OPERATION_FIXTURE_PATH));
  PS_REQUIRE_OK(load_registry.find_traits("fixture.double"));
  PS_CHECK(load_state->call_count == 0 &&
           load_state->copy_count == load_copies);
  return 0;
}

ps::OperationDefinition prevalidation_definition(std::string key,
                                                 unsigned input_count,
                                                 unsigned* calls) {
  ps::OperationDefinition operation;
  operation.key = std::move(key);
  operation.traits.input_count = input_count;
  operation.traits.input_schema.resize(input_count);
  for (auto& port : operation.traits.input_schema) {
    port.result_schema_id = "test.multi_output";
    port.result_schema_version = 1;
  }
  operation.traits.requires_metadata_specialization = true;
  operation.traits.outputs = {multi_result::output("value")};
  operation.prepare_static = [](const auto& inputs, const auto&) {
    for (const auto& input : inputs) {
      if (!input.result_schema)
        return ps::Result<ps::OperationPreparation>(
            {ps::ErrorCode::TypeMismatch, "missing Result metadata"});
      const auto& tensors = input.result_schema->tensors;
      if (tensors.size() != 1 ||
          tensors[0].descriptor.element_type != ps::ElementType::Float64 ||
          tensors[0].sample_shape() != std::vector<std::uint64_t>{1})
        return ps::Result<ps::OperationPreparation>(
            {ps::ErrorCode::TypeMismatch, "expected Float64 scalar"});
    }
    ps::OperationPreparation preparation;
    preparation.outputs.resize(1);
    preparation.outputs[0].metadata.result_schema =
        std::make_shared<const ps::SchemaTemplate>(multi_result::schema());
    return ps::Result<ps::OperationPreparation>(std::move(preparation));
  };
  multi_result::install_fixed(operation, {-1}, {17});
  const auto lower = operation.lower;
  operation.lower = [calls, lower](const ps::OperationLowering& query) {
    ++*calls;
    return lower(query);
  };
  return operation;
}
int verify_cpp_invocation_prevalidation() {
  using namespace ps;  // NOLINT(build/namespaces)
  unsigned preserve_calls = 0, match_calls = 0, backend_calls = 0;
  auto registry = std::make_shared<OperationRegistry>();
  PS_REQUIRE_OK(registry->register_operation(
      prevalidation_definition("fixture.preserve", 1, &preserve_calls)));
  PS_REQUIRE_OK(registry->register_operation(
      prevalidation_definition("fixture.match", 2, &match_calls)));
  PS_REQUIRE_OK(registry->register_operation(
      prevalidation_definition("fixture.backend", 0, &backend_calls)));
  PS_REQUIRE_OK(registry->freeze());
  const auto metadata = [](ElementType type,
                           std::vector<std::uint64_t> shape = {1}) {
    return OperationMetadata{std::make_shared<const SchemaTemplate>(
        multi_result::schema(type, std::move(shape)))};
  };
  auto preserve = registry->prepare_operation(
      "fixture.preserve", {metadata(ElementType::UInt8)}, {});
  PS_CHECK(!preserve.ok() && preserve.status().code == ErrorCode::TypeMismatch);
  PS_CHECK(preserve_calls == 0);
  for (const auto& second :
       {metadata(ElementType::UInt8), metadata(ElementType::Float64, {2})}) {
    auto invalid = registry->prepare_operation(
        "fixture.match", {metadata(ElementType::Float64), second}, {});
    PS_CHECK(!invalid.ok() && invalid.status().code == ErrorCode::TypeMismatch);
  }
  for (unsigned port : {0U, 1U}) {
    std::vector<OperationMetadata> inputs(2, metadata(ElementType::Float64));
    inputs[port] = {};
    auto invalid = registry->prepare_operation("fixture.match", inputs, {});
    PS_CHECK(!invalid.ok() && invalid.status().code == ErrorCode::TypeMismatch);
  }
  PS_CHECK(match_calls == 0);
  auto prepared = registry->prepare_operation("fixture.backend", {}, {});
  PS_REQUIRE_OK(prepared);
  auto request = multi_result::take(TileRequest::all(
      multi_result::take(TileGrid::members(multi_result::schema())),
      ResourceBudget{}));
  auto unknown = prepared.value()->lower(
      {request}, 128, static_cast<physical::Backend>(99), {});
  PS_CHECK(!unknown.ok() &&
           unknown.status().code == ErrorCode::InvalidArgument);
  auto unsupported =
      prepared.value()->lower({request}, 128, physical::Backend::Gpu, {});
  PS_CHECK(!unsupported.ok() &&
           unsupported.status().code == ErrorCode::BackendUnavailable);
  PS_CHECK(backend_calls == 0);
  PS_REQUIRE_OK(
      prepared.value()->lower({request}, 128, physical::Backend::Cpu, {}));
  PS_CHECK(backend_calls == 1);
  WorkflowDocument document;
  document.nodes = {{1, "fixture.backend", {}, {}}};
  document.outputs = {{"value", 1, "value"}};
  GraphContext graph(document);
  auto compiled = Compiler(registry).compile(graph);
  PS_REQUIRE_OK(compiled);
  ExecutionContext context(registry);
  auto result = multi_result::run(context, compiled.value().plan.value());
  PS_REQUIRE_OK(result);
  PS_CHECK(multi_result::number(result.value().results.at("value")) == 17);
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
        PS_CHECK(output.result_schema.has_value());
        const auto& schema = *output.result_schema;
        PS_CHECK(schema.tensors.size() == expected.size());
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
 * @brief Exercises operation ABI validation, callbacks, freezing, and
 * exact destroy-before-unload lifecycle.
 * @return Zero when all checks pass.
 * @throws std::bad_alloc If test setup allocation fails.
 * @throws std::runtime_error If a fixture cannot be inspected.
 * @note Failures otherwise return nonzero through `PS_CHECK`.
 */
int main() {
  using ps::CancellationToken;
  using ps::Compiler;
  using ps::ElementType;
  using ps::ErrorCode;
  using ps::GraphContext;
  using ps::OperationDefinition;
  using ps::OperationParameterSpec;
  using ps::OperationParameterType;
  using ps::OperationRegistry;
  using ps::OperationTraits;
  using ps::ParameterValue;
  using ps::Region;
  using ps::RegionDimension;
  using ps::Result;
  using ps::StridedLayout;
  using ps::TensorDescriptor;
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

  PS_CHECK(!nul_operation_status.ok());
  PS_CHECK(nul_operation_status.code == ErrorCode::InvalidArgument);
  PS_CHECK(nul_operation_registry_empty);
  PS_CHECK(nul_operation_owner_count == 0U);
  PS_CHECK(nul_operation_load_count == 0U);
  PS_CHECK(nul_operation_close_count == 0U);
  PS_CHECK(nul_operation_observer.counter(
               "ps_operation_fixture_destroy_count") == 0U);
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

  PS_CHECK(verify_result_registration() == 0);

  PS_CHECK(verify_tensor_extent_dso() == 0);

  PS_CHECK(verify_dense_byte_size_contract() == 0);
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
    PS_CHECK(result.schema().tensors[0].atomic_trailing_axes == 1);
    const auto input_metadata =
        ps::OperationMetadata{std::make_shared<const ps::SchemaTemplate>(
            operation_result::schema(true))};
    for (const auto& bad : std::vector<std::map<std::string, ParameterValue>>{
             {},
             {{"factor", 2.0}},
             {{"scale", int64_t{2}}}}) {
      auto rejected =
          registry.prepare_operation("fixture.double", {input_metadata}, bad);
      PS_CHECK(!rejected.ok() &&
               rejected.status().code == ErrorCode::InvalidArgument);
    }
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
  return 0;
}
