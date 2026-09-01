#pragma once

#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <string>
#include <vector>

#include "photospider/compiler/workflow_document.hpp"
#include "photospider/core/status.hpp"
#include "photospider/data/value.hpp"
#include "photospider/execution/cancellation.hpp"

namespace ps {

/**
 * @brief Local physical backend selected for one plan step.
 *
 * @note GPU names one optional in-process lane, never a remote device.
 */
enum class Backend : std::uint32_t {
  Cpu = 1,
  Gpu = 2,
};

/** @brief Closed compile-time output-shape inference rule. */
enum class OperationShapeRule : std::uint32_t {
  /** @brief Output is one scalar with shape `{1}`. */
  Scalar = 1U,
  /** @brief Output descriptor equals the first input descriptor. */
  PreserveFirstInput = 2U,
  /** @brief All input descriptors must match and output preserves them. */
  MatchAllInputs = 3U,
};

/** @brief Closed compiler-visible Region propagation rule. */
enum class OperationRegionRule : std::uint32_t {
  /** @brief Operation requires and produces whole logical coverage. */
  Whole = 1U,
  /** @brief Output Region maps element-for-element from input Regions. */
  Elementwise = 2U,
  /** @brief Output Region reads an explicit symmetric input halo. */
  Halo = 3U,
};

/**
 * @brief Immutable compiler-visible facts for one operation implementation.
 *
 * @note Traits are copied into semantic IR; callback/DSO identities are not.
 */
struct PHOTOSPIDER_API OperationTraits final {
  /** @brief Exact ordered input count. */
  std::uint32_t input_count = 0;
  /** @brief Equal inputs/parameters produce equal output bytes. */
  bool deterministic = true;
  /** @brief Callback has no externally visible side effect. */
  bool side_effect_free = true;
  /** @brief Required CPU implementation is available. */
  bool supports_cpu = true;
  /** @brief Optional local GPU implementation is available. */
  bool supports_gpu = false;
  /** @brief Recoverable GPU failure may execute the CPU implementation. */
  bool allows_cpu_fallback = false;
  /** @brief Estimated peak invocation bytes for resource admission. */
  std::uint64_t estimated_bytes = 0;
  /** @brief Version of this complete semantic trait record. */
  std::uint32_t version = 1U;
  /** @brief Whether a derived result may enter a disposable local cache. */
  bool cacheable = true;
  /** @brief Static output type for scalar or descriptor validation. */
  ElementType output_element_type = ElementType::Float64;
  /** @brief Closed static output-shape inference behavior. */
  OperationShapeRule shape_rule = OperationShapeRule::Scalar;
  /** @brief Closed logical Region propagation behavior. */
  OperationRegionRule region_rule = OperationRegionRule::Whole;
  /** @brief Symmetric element halo, nonzero only for `Halo`. */
  std::uint32_t halo_radius = 0U;
};

/**
 * @brief Immutable invocation passed to a registered operation callback.
 *
 * @note Input Values, parameters, and token remain valid for the callback only.
 */
struct PHOTOSPIDER_API OperationInvocation final {
  /** @brief Ordered immutable input Values. */
  const std::vector<Value>& inputs;
  /** @brief Canonically ordered source parameters. */
  const std::map<std::string, ParameterValue>& parameters;
  /** @brief Physical backend selected by the validated plan. */
  Backend backend = Backend::Cpu;
  /** @brief Cooperative cancellation observation. */
  CancellationToken cancellation;
};

/** @brief C++ callback type for one synchronous operation invocation. */
using OperationCallback =
    std::function<Result<Value>(const OperationInvocation&)>;

/**
 * @brief One complete operation definition before registry publication.
 *
 * @note Registration deep-copies key/traits/callback into registry ownership.
 */
struct PHOTOSPIDER_API OperationDefinition final {
  /** @brief Nonempty unique operation key. */
  std::string key;
  /** @brief Immutable compiler-visible semantic traits. */
  OperationTraits traits;
  /** @brief Required synchronous implementation callback. */
  OperationCallback callback;
};

/**
 * @brief Startup-configured registry for trusted in-process operations.
 *
 * @note Mutation is serialized and forbidden after `freeze()` succeeds.
 */
class PHOTOSPIDER_API OperationRegistry final {
 public:
  /**
   * @brief Creates an empty mutable registry.
   * @throws std::bad_alloc If private state allocation fails.
   * @note Register operations before compilation begins.
   */
  OperationRegistry();

  /**
   * @brief Stops callbacks, destroys DSO records, and unloads libraries.
   * @throws Nothing.
   * @note Callers must ensure no invocation remains in flight at destruction.
   */
  ~OperationRegistry() noexcept;

  OperationRegistry(const OperationRegistry&) = delete;
  OperationRegistry& operator=(const OperationRegistry&) = delete;
  OperationRegistry(OperationRegistry&&) = delete;
  OperationRegistry& operator=(OperationRegistry&&) = delete;

  /**
   * @brief Registers one built-in or embedding-supplied operation.
   * @param definition Complete owned definition.
   * @return Success or validation/duplicate/frozen failure.
   * @throws std::bad_alloc If registry allocation fails without mutation.
   * @note CPU support and a nonempty callback are mandatory.
   */
  [[nodiscard]] Status register_operation(OperationDefinition definition);

  /**
   * @brief Loads and validates one trusted in-process operation DSO.
   * @param path Explicit startup-configured library path.
   * @return Success or complete load/ABI/descriptor validation failure.
   * @throws std::bad_alloc If staging allocation fails without publication.
   * @note No signature, trust-store, sandbox, or process isolation is applied.
   */
  [[nodiscard]] Status load_plugin(const std::string& path);

  /**
   * @brief Makes the operation set read-only.
   * @return Success; repeated calls are idempotent.
   * @throws Nothing.
   * @note Compilation requires a frozen registry.
   */
  Status freeze() noexcept;

  /**
   * @brief Reports whether registry mutation has ended.
   * @return True after `freeze()`.
   * @throws Nothing.
   * @note The state is safe for concurrent read observation.
   */
  [[nodiscard]] bool frozen() const noexcept;

  /**
   * @brief Finds copied semantic traits for one operation.
   * @param key Exact operation key.
   * @return Traits or `NotFound`.
   * @throws std::bad_alloc If a failure diagnostic allocation fails.
   * @note The returned value grants no callback or registry mutation access.
   */
  [[nodiscard]] Result<OperationTraits> find_traits(
      const std::string& key) const;

  /**
   * @brief Invokes one operation through its exception fence.
   * @param key Exact registered operation key.
   * @param invocation Immutable validated invocation.
   * @return Complete Value or typed callback failure.
   * @throws std::bad_alloc Only for process resource exhaustion before a
   * recoverable result can be constructed.
   * @note Callback exceptions other than bad_alloc become `OperationFailed`.
   */
  [[nodiscard]] Result<Value> invoke(
      const std::string& key, const OperationInvocation& invocation) const;

  /**
   * @brief Returns the sorted immutable operation-key inventory.
   * @return Exact key list.
   * @throws std::bad_alloc If result allocation fails.
   * @note Keys remain process configuration, not IPC values.
   */
  [[nodiscard]] std::vector<std::string> keys() const;

 private:
  /** @brief Opaque synchronized registry and DSO ownership state. */
  struct Impl;
  /** @brief Unique private state. */
  std::unique_ptr<Impl> impl_;
};

/**
 * @brief Creates the maintained built-in operation set and freezes it.
 * @return Shared read-only registry containing constant, identity, add, and
 * delay operations.
 * @throws std::bad_alloc If construction fails.
 * @note The caller may instead assemble a custom registry before freezing.
 */
[[nodiscard]] PHOTOSPIDER_API std::shared_ptr<OperationRegistry>
make_default_operation_registry();

}  // namespace ps
