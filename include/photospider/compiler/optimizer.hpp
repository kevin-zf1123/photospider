#pragma once

#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "photospider/compiler/ir.hpp"

namespace ps {
/** @brief One ordered invocation; repeated names are independent stages. */
struct PHOTOSPIDER_API OptimizerInvocation final {
  std::string name;
  std::map<std::string, ParameterValue> options;
};

/** @brief Declarative, typed optimizer option. Numeric bounds are inclusive.
 * Bounds must have the default's numeric type. Choices, when present, have
 * that same type. Nonfinite floating values and strings over 8192 bytes fail.
 * All constraints are checked before invoking any transformation callback.
 */
struct PHOTOSPIDER_API OptimizerOption final {
  ParameterValue default_value = std::int64_t{0};
  std::optional<ParameterValue> minimum;
  std::optional<ParameterValue> maximum;
  std::vector<ParameterValue> choices;
  std::string description;
};

/** @brief Pure transformation over immutable input with a checked contract.
 * The compiler validates the returned IR and public input/output signature
 * before publication. No Run callbacks or mutable input storage may be used.
 * Structural validation does not prove numerical or effect equivalence;
 * each transform remains responsible for those semantic properties.
 */
struct PHOTOSPIDER_API OptimizerDefinition final {
  std::string name, version, description;
  std::map<std::string, OptimizerOption> option_schema;
  std::function<Result<IRProgram>(const ValidatedIR&,
                                  const std::map<std::string, ParameterValue>&)>
      transform;
  std::vector<IRForm> supported_ir_forms = {IRForm::DependencyPaths,
                                            IRForm::ControlFlow};

  /** @brief Resolve defaults and validate typed overrides before
   * transformation. Unknown names/types or values outside the schema return
   * InvalidArgument. Immutable calls are safe concurrently; allocation failures
   * may throw.
   */
  Result<std::map<std::string, ParameterValue>> resolve_options(
      const std::map<std::string, ParameterValue>& overrides) const;
};

/** @brief Registration-order catalog. Register before freeze; frozen reads
 * are thread-safe if registered transforms obey the pure callback contract.
 */
class PHOTOSPIDER_API OptimizerRegistry final {
 public:
  /** @brief Stable catalog identity; share ownership instead of copying or
   * moving a registry, so published definitions remain valid after freeze.
   */
  OptimizerRegistry() = default;
  OptimizerRegistry(const OptimizerRegistry&) = delete;
  OptimizerRegistry& operator=(const OptimizerRegistry&) = delete;
  OptimizerRegistry(OptimizerRegistry&&) = delete;
  OptimizerRegistry& operator=(OptimizerRegistry&&) = delete;

  Status add(OptimizerDefinition definition);
  void freeze() noexcept { frozen_ = true; }
  bool frozen() const noexcept { return frozen_; }
  const std::vector<OptimizerDefinition>& definitions() const noexcept {
    return definitions_;
  }
  const OptimizerDefinition* find(const std::string& name) const noexcept;

 private:
  bool frozen_ = false;
  std::vector<OptimizerDefinition> definitions_;
};

/** @brief Frozen catalog containing the optional example 'optimize' pass. */
PHOTOSPIDER_API std::shared_ptr<const OptimizerRegistry>
make_default_optimizer_registry();
}  // namespace ps
