#pragma once

#include <cstdint>

#include "execution/result_checkpoints.hpp"

namespace ps::execution_internal {
// Borrowed only until validate() returns. The host owns admission and current
// cancellation state; the validator cannot schedule or publish through this
// interface, and does not retain the interface or its returned work closures.
class PublicationValidationServices {
 public:
  virtual ~PublicationValidationServices() = default;
  virtual Status consume(std::uint64_t) = 0;
  virtual Status operation_work(std::uint64_t) = 0;
  virtual Status run_work(std::uint64_t) = 0;
  virtual ErrorCode stop() = 0;
  virtual CancellationToken cancellation() = 0;
  virtual FootprintLimits set_limits() = 0;
};

// All capabilities and callbacks are borrowed for one synchronous validation.
// The coordinator has retired the producer callback before constructing this
// view and owns the plan/query/history. Validation may bind producer and
// association facts; it does not publish, freeze ancestry, retire or notify.
struct PublicationValidationView final {
  const PlanStep& step;
  const ResultProgramQuery& query;
  const ResultRef& previous;
  std::uint64_t previous_revision, node_id;
  const ResultInputFacts& input_facts;
  const ResultNeedHistory& history;
  const ResourceBudget& resources;
  std::uint64_t maximum_window;
  PublicationValidationServices& services;
};
class ResultPublicationValidator final {
 public:
  // Returns an owning failure. Only the coordinator acts on retire_actor.
  static Status validate(const PublicationValidationView&,
                         const ResultPublication&, bool& retire_actor);
};
}  // namespace ps::execution_internal
