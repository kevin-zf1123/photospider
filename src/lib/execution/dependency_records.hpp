#pragma once

#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "photospider/compiler/compiler.hpp"
#include "photospider/execution/dependencies.hpp"
#include "photospider/plugin/dependency_program.hpp"

namespace ps::execution_internal {
/** @brief Single-Run builder; publication makes all structural state immutable.
 * @note Owns no Values. Rows merge only for the same node/contract/snapshot;
 * Whole and terminal manifests remain complete indivisible records.
 */
class DependencyRecords final {
 public:
  DependencyRecords(const ExecutionPlan& plan, std::string identity,
                     FootprintLimits limits);
  DependencyRecords(const DependencyRecords&) = delete;
  DependencyRecords& operator=(const DependencyRecords&) = delete;
  const Status& status() const noexcept { return failure_; }
  Status append(std::size_t step, const DependencyResult& result);
  Status append_legacy(std::size_t step, const Footprint& outputs,
                       const std::vector<Footprint>& inputs);
  Status output(const std::string& name, std::size_t step,
                const Footprint& samples);
  ExecutionDependencies finish() &&;

 private:
  Status append_record(std::size_t step, Footprint outputs,
                       std::optional<DependencyCertificate> certificate,
                       std::vector<DependencyNeed> manifest);
  std::shared_ptr<ExecutionDependencies::Impl> impl_;
  FootprintLimits limits_;
  const ExecutionPlan* plan_;
  std::string identity_;
  Status failure_;
};
}  // namespace ps::execution_internal
