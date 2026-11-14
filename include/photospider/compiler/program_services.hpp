#pragma once

#include <memory>
#include <utility>

#include "photospider/compiler/plan.hpp"

namespace ps {
/** @brief Registry-bound IR binding and physical Plan construction services.
 * The registry must be frozen. Binding retains selected operation and resource
 * owners. Selection only narrows compiled coverage. Callers may share this
 * service across threads when their editable construction objects are separate.
 */
class PHOTOSPIDER_API ProgramServices final {
 public:
  explicit ProgramServices(std::shared_ptr<OperationRegistry> operations)
      : operations_(std::move(operations)) {}
  Result<ValidatedIR> bind_ir(IRProgram program,
                              ResourceSet resources = {}) const;

 private:
  friend class Compiler;
  friend class ExecutionPlan;
  friend class CompilerArtifact;
  Result<ExecutionPlan> link_plan(const ValidatedIR& ir,
                                  const PlanningOptions& options) const;
  Status validate_plan(const ExecutionPlan& plan) const;
  Status select_plan(ExecutionPlan* plan) const;

  Status finalize_plan(ExecutionPlan* plan) const;
  std::shared_ptr<OperationRegistry> operations_;
};
}  // namespace ps
