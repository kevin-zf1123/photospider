#include <cstdint>
#include <cstring>
#include <limits>
#include <map>
#include <new>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include "photospider/compiler/compiler.hpp"
#include "photospider/plugin/data_definition_registry.hpp"
#include "photospider/plugin/operation_registry.hpp"
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
#ifndef PS_DATA_PROVIDER_FIXTURE_PATH
#error "PS_DATA_PROVIDER_FIXTURE_PATH must name the valid provider fixture"
#endif
#ifndef PS_DATA_PROVIDER_BAD_FIXTURE_PATH
#error \
    "PS_DATA_PROVIDER_BAD_FIXTURE_PATH must name the invalid provider fixture"
#endif

namespace {

using ps::plugin_testing::LibraryKind;

/** @brief Library kind expected by the currently installed test callback. */
LibraryKind g_expected_library_kind = LibraryKind::Operation;
/** @brief Exact owner-allocation callback invocation count. */
std::uint32_t g_owner_allocation_count = 0U;
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
 * @brief Installs one deterministic owner-allocation failure scope.
 *
 * @note The scope is single-threaded and clears callbacks before fixture
 * observers or later lifecycle cases are destroyed.
 */
class LibraryFailureScope final {
 public:
  /**
   * @brief Installs callbacks for one exact library kind.
   * @param kind Operation or provider owner boundary to fail.
   * @throws Nothing.
   */
  explicit LibraryFailureScope(ps::plugin_testing::LibraryKind kind) noexcept {
    g_expected_library_kind = kind;
    g_owner_allocation_count = 0U;
    g_native_close_count = 0U;
    hooks_.before_owner_allocation = fail_owner_allocation;
    hooks_.native_close = count_native_close;
    ps::plugin_testing::install_library_test_hooks(&hooks_);
  }

  /**
   * @brief Clears borrowed callbacks before scope storage retires.
   * @throws Nothing.
   */
  ~LibraryFailureScope() noexcept {
    ps::plugin_testing::install_library_test_hooks(nullptr);
  }

  /**
   * @brief Forbids duplicating one process-global callback installation.
   * @param other Source scope that cannot be copied.
   * @throws Nothing; the operation is deleted.
   * @note Exactly one scope owns callback installation at a time.
   */
  LibraryFailureScope(const LibraryFailureScope& other) = delete;
  /**
   * @brief Forbids assigning process-global callback installation.
   * @param other Source scope that cannot be assigned.
   * @return No value; the operation is deleted.
   * @throws Nothing; the operation is deleted.
   */
  LibraryFailureScope& operator=(const LibraryFailureScope& other) = delete;
  /**
   * @brief Forbids moving the address-stable borrowed callback table.
   * @param other Source scope that cannot be moved.
   * @throws Nothing; the operation is deleted.
   * @note The installed pointer must remain stable until scope destruction.
   */
  LibraryFailureScope(LibraryFailureScope&& other) = delete;
  /**
   * @brief Forbids move assignment of process-global callback installation.
   * @param other Source scope that cannot be assigned.
   * @return No value; the operation is deleted.
   * @throws Nothing; the operation is deleted.
   * @note Callback storage and installation always retire together.
   */
  LibraryFailureScope& operator=(LibraryFailureScope&& other) = delete;

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

  LibraryObserver operation_observer(PS_OPERATION_FIXTURE_PATH);
  PS_CHECK(operation_observer.counter("ps_operation_fixture_destroy_count") ==
           0U);
  {
    LibraryFailureScope failure(ps::plugin_testing::LibraryKind::Operation);
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

  LibraryObserver provider_observer(PS_DATA_PROVIDER_FIXTURE_PATH);
  PS_CHECK(provider_observer.counter(
               "ps_data_provider_fixture_destroy_count") == 0U);
  {
    LibraryFailureScope failure(ps::plugin_testing::LibraryKind::Provider);
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
  {
    DataDefinitionRegistry registry;
    PS_CHECK(registry.load_provider(PS_DATA_PROVIDER_FIXTURE_PATH).ok());
    PS_CHECK(!registry.load_provider(PS_DATA_PROVIDER_BAD_FIXTURE_PATH).ok());
    PS_CHECK(registry.freeze().ok());
    PS_CHECK(!registry.load_provider(PS_DATA_PROVIDER_FIXTURE_PATH).ok());
    auto schema = registry.find("fixture.float64");
    PS_CHECK(schema.ok());
    PS_CHECK(schema.value().element_type == ElementType::Float64);
    PS_CHECK(schema.value().maximum_rank == 4U);
  }
  PS_CHECK(provider_observer.counter(
               "ps_data_provider_fixture_destroy_count") == 2U);

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
  OperationTraits overflowing_fixed_shape;
  overflowing_fixed_shape.shape_rule = OperationShapeRule::Fixed;
  overflowing_fixed_shape.fixed_output_shape = {
      std::numeric_limits<std::uint64_t>::max()};
  PS_CHECK(!fencing
                .register_operation(OperationDefinition{
                    "fixture.overflowing_fixed_shape", overflowing_fixed_shape,
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
