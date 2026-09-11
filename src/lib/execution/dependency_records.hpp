#pragma once

#include <map>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <vector>

#include "photospider/compiler/compiler.hpp"
#include "photospider/execution/dependencies.hpp"
#include "photospider/plugin/dependency_program.hpp"

namespace ps::execution_internal {
/** @brief One immutable direct record shared independently of pixel owners.
 * @note Upstream links contain only structure. Their identity binds the
 * captured plan, snapshot and exact observation, including terminal full Q.
 */
struct DependencyRecord final {
  std::string identity;
  std::size_t step;
  Footprint samples;
  std::optional<DependencyCertificate> certificate;
  std::vector<DependencyNeed> manifest;
  std::vector<std::shared_ptr<const DependencyRecord>> upstream;
  /** @brief Intrusive retirement link, accessed only after the last owner. */
  DependencyRecord* retired_next = nullptr;
  /** @brief Iteratively retires arbitrarily deep structural DAGs without
   * allocating or recursing through shared_ptr child destructors. */
  static void retire(DependencyRecord* record) noexcept {
    thread_local DependencyRecord* pending = nullptr;
    thread_local bool draining = false;
    record->retired_next = pending;
    pending = record;
    if (draining)
      return;
    draining = true;
    while (pending) {
      auto* next = pending;
      pending = next->retired_next;
      delete next;
    }
    draining = false;
  }
};
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
  Status append_empty(std::size_t step, const Footprint& outputs);
  /** @brief Covered rows before a speculative backend attempt; no pixel owners.
   * @note Copies only bounded coverage, not nested dependency manifests.
   */
  using Checkpoint = std::vector<Footprint>;
  Result<Checkpoint> checkpoint(std::uint64_t* remaining_work) const;
  /** @brief Restores prior rows and indexes without mutating shared records.
   * @note Outputs cannot be added between checkpoint and rollback. Successful
   * retained Whole records must be reimported when their pixels are requested.
   */
  Status rollback(const Checkpoint& checkpoint, std::uint64_t* remaining_work);
  std::string observation_identity(std::size_t step,
                                   const Footprint& samples) const;
  Result<std::shared_ptr<const DependencyRecord>> capture(
      std::size_t step, const Footprint& samples,
      std::vector<std::shared_ptr<const DependencyRecord>> upstream);
  Status import(const std::shared_ptr<const DependencyRecord>& record);
  /** @brief Rebinds a verified content witness through corresponding ports.
   * Returns an optional cache miss for ambiguous/nonmatching graph routes.
   * Copies no pixel owners and never mutates this Run's records on failure.
   */
  Result<std::shared_ptr<const DependencyRecord>> rebind_cached(
      const std::shared_ptr<const DependencyRecord>& record, std::size_t step,
      const std::map<std::size_t, std::vector<PlanInput>>& routes,
      std::uint64_t* remaining_work) const;
  Status output(const std::string& name, std::size_t step,
                const Footprint& samples);
  ExecutionDependencies finish() &&;
  static std::uint64_t metadata_size(
      const ExecutionDependencies& evidence) noexcept;

 private:
  Status append_record(std::size_t step, Footprint outputs,
                       std::optional<DependencyCertificate> certificate,
                       std::vector<DependencyNeed> manifest);
  std::string certificate_identity(std::size_t step) const;
  std::shared_ptr<ExecutionDependencies::Impl> impl_;
  FootprintLimits limits_;
  const ExecutionPlan* plan_;
  std::string identity_;
  Status failure_;
  std::set<std::string> imported_;
};
}  // namespace ps::execution_internal
