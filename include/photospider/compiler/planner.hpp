#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "photospider/compiler/plan.hpp"

namespace ps {
/** @brief Callback-local construction of complete physical tables.
 * The compiler seeds call/value/CFG metadata from validated IR. A named planner
 * reads static call bindings and builds the entire task graph, including
 * intermediate values and explicit transfers. Borrowed references cannot
 * escape the callback. Only the compiler validates and publishes the result.
 */
class PHOTOSPIDER_API PlanBuilder final {
 public:
  PlanBuilder(const PlanBuilder&) = delete;
  PlanBuilder& operator=(const PlanBuilder&) = delete;
  physical::Program& program() { return plan_.edit().program_; }
  const physical::Program& program() const noexcept {
    return plan_.data().program_;
  }
  const std::vector<PlanCallBinding>& calls() const noexcept {
    return plan_.data().calls_;
  }
  const std::vector<WorkflowInputDeclaration>& input_declarations()
      const noexcept {
    return plan_.data().inputs_;
  }
  const std::vector<PlanOutput>& return_signature() const noexcept {
    return plan_.data().returns_;
  }
  const std::map<std::string, std::size_t>& outputs() const noexcept {
    return plan_.data().outputs_;
  }
  const std::map<std::string, TileRequest>& output_requests() const noexcept {
    return plan_.data().requests_;
  }
  const ResourceSet& resources() const noexcept {
    return plan_.data().resources_;
  }

 private:
  friend class Compiler;
  explicit PlanBuilder(ExecutionPlan plan) : plan_(std::move(plan)) {}
  ExecutionPlan plan_;
};

/** @brief Named trusted in-process physical planning backend.
 * The callback reads the original immutable IR and linked builder declarations,
 * then emits tasks. Return a failure to discard the builder. Successful
 * output is checked for coverage, DAG structure, ownership and resource bounds;
 * invalid output is Internal; validation exhaustion is ResourceExhausted.
 * Structural checks do not prove semantic mapping
 * which remains the backend's responsibility.
 * Callback failures and exceptions identify this planner's name and version
 * in their diagnostic message; operational status codes are preserved.
 * Callbacks must support concurrent invocations, retain no builder references,
 * and invoke only pure static operation hooks, never runtime callbacks.
 */
struct PHOTOSPIDER_API PlannerDefinition final {
  std::string name, version, description;
  std::function<Status(const ValidatedIR&, PlanBuilder&,
                       const PlanningOptions&)>
      plan;
  std::vector<IRForm> supported_ir_forms = {IRForm::DependencyPaths,
                                            IRForm::ControlFlow};
};

/** @brief Registration-order catalog, immutable after freeze.
 * Register and freeze on one thread before sharing. Frozen reads are
 * concurrent. Returned definition pointers remain valid until mutation or
 * destruction.
 */
class PHOTOSPIDER_API PlannerRegistry final {
 public:
  /** @brief Stable catalog identity; share ownership instead of copying or
   * moving a registry, so published definitions remain valid after freeze.
   */
  PlannerRegistry() = default;
  PlannerRegistry(const PlannerRegistry&) = delete;
  PlannerRegistry& operator=(const PlannerRegistry&) = delete;
  PlannerRegistry(PlannerRegistry&&) = delete;
  PlannerRegistry& operator=(PlannerRegistry&&) = delete;

  /** @brief Add a unique backend before freeze. InvalidArgument rejects an
   * empty/oversized name or version, missing callback, duplicate name, invalid
   * or repeated IR form, or frozen registry. Allocation failures may throw.
   */
  Status add(PlannerDefinition definition);
  void freeze() noexcept { frozen_ = true; }
  bool frozen() const noexcept { return frozen_; }
  const std::vector<PlannerDefinition>& definitions() const noexcept {
    return definitions_;
  }
  const PlannerDefinition* find(const std::string& name) const noexcept;

 private:
  bool frozen_ = false;
  std::vector<PlannerDefinition> definitions_;
};

/** @brief Shared frozen catalog containing static-tiles@2.
 * Initialization is thread-safe and may throw on allocation failure.
 */
PHOTOSPIDER_API std::shared_ptr<const PlannerRegistry>
make_default_planner_registry();
}  // namespace ps
