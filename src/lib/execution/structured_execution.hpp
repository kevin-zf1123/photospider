#pragma once

#include <chrono>
#include <functional>
#include <future>
#include <map>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "photospider/execution/execution.hpp"

namespace ps::execution_internal {
class SharedResults;
class ResultCheckpoints;
class ResultCache;
/** @brief Uses the existing context worker and its completion/drain boundary.
 */
struct StructuredServices final {
  // A CPU start phase uses this fact to admit a GPU-targeted factory. It does
  // not grant native services to the CPU callback.
  bool native_gpu_available = false;
  const ps_cpu_parallel_service_v1* cpu_parallel = nullptr;
  const ps_cpu_tiles_service_v1* cpu_tiles = nullptr;
  const ps_gpu_service_v1* gpu = nullptr;
  std::function<Status()> gpu_status;
  std::function<std::uint64_t()> gpu_dispatches;
  BufferAllocator allocator;
  std::function<bool(const CpuStorage&)> native_storage;
  std::function<void(ExecutionDiagnostics&)> observe;
  std::function<Result<std::pair<Value, std::uint64_t>>(
      const Value&, const std::function<Status(std::uint64_t)>&)>
      native_input;
};
/** @brief Observes one structured submission through task retirement.
 * @details `retirement` keeps the completion state and its Root lease alive.
 * Use `ready`, `wait`, and `finish` to observe asynchronous work; `finish`
 * returns the task status. CPU staged-tile work runs inline and returns an
 * immediate status, rather than queueing on a CPU worker and waiting for that
 * same pool's tile workers.
 * @note For a queued task, the future becomes ready only after task captures
 * and callback services retire. Queue acceptance followed by dropping the
 * work resolves as Cancelled after its captures are released. Destruction of
 * this object does not wait. This reports task retirement, not producer
 * ownership or completion; a shared Result producer may publish incomplete
 * coverage and continue under its separate lease. The submission does not own
 * the coordinator or arbitrary state borrowed by its task. Normally the caller
 * drains retirement before those owners leave scope. The frozen-plan structured
 * path may instead transfer a cancelled coordinator to the shared-result
 * registry while a peer still needs its pending producer; the registry then
 * keeps that coordinator alive for peer-driven completion. A supplied
 * `plan_owner` separately pins the plan; without it, the coordinator borrows
 * the plan.
 */
struct StructuredSubmission final {
  std::shared_ptr<const void> retirement;
  std::shared_future<Result<int>> completion;
  Status immediate{ErrorCode::Internal, {}};
  std::shared_ptr<ExecutionDiagnostics> observations = {};
  bool ready() const {
    return !completion.valid() ||
           completion.wait_for(std::chrono::milliseconds(0)) ==
               std::future_status::ready;
  }
  void wait() const {
    if (completion.valid())
      completion.wait();
  }
  Status finish() const {
    return completion.valid() ? completion.get().status() : immediate;
  }
};
/** @brief Submits a structured task or completes it inline.
 * @note Submission errors are returned directly. Once accepted, task failures
 * are observed through `StructuredSubmission::finish`. The caller's dispatch
 * drain must wait for retirement before the borrowed coordinator or any
 * caller-owned state referenced by the task can leave scope. The coordinator
 * copies options and borrows `plan` unless `plan_owner` pins it.
 */
using StructuredDispatch = std::function<Result<StructuredSubmission>(
    Backend, bool, bool, bool, CancellationToken,
    std::function<Status(const StructuredServices&)>)>;
/** @brief Runs one structured coordinator with captured run inputs.
 * @details The coordinator is allocated with the supplied execution Root,
 * owns the binding vector, copies the options, and copies the snapshot identity
 * into Root-managed storage. When `plan_owner` is supplied, the
 * coordinator aliases its plan while retaining that owner; otherwise `plan`
 * is borrowed. The call drains pending callbacks before return, including
 * exception cleanup, unless a cancelled frozen-plan producer is adopted by the
 * shared-result registry for a live peer. Failed adoption or a peer that leaves
 * before adoption preserves synchronous draining.
 * @param requested Named output sample queries. In atom mode, the coordinator
 * preflights the CPU Result query set and total observation count before
 * preparing actors or invoking callbacks.
 * @param atom_outcomes Selects Result atom collection into
 * `ExecutionResult::atoms`; the public wrapper requires a managed structured
 * Result dependency plan for this mode.
 * @param maximum_parallelism Caps the number of queued callback tasks the
 * driver submits in one wave. The driver retires that wave before applying
 * phase results and submitting the next one; this does not schedule a Result
 * dependency graph independently of its coordinator.
 */
Result<ExecutionResult> execute_structured(
    const ExecutionPlan& plan, std::vector<ExecutionBinding> bindings,
    std::shared_ptr<OperationRegistry> operations, ResourceBudget resources,
    const ExecutionOptions& options, const CancellationToken& cancellation,
    const std::function<ErrorCode()>& stop, const StructuredDispatch& dispatch,
    const DemandQuery* requested = nullptr,
    const std::string& snapshot_identity = {},
    SharedResults* shared_results = nullptr,
    ResultCheckpoints* checkpoints = nullptr, ResultCache* blocks = nullptr,
    std::shared_ptr<const ExecutionPlan> plan_owner = {},
    bool atom_outcomes = false, std::uint32_t maximum_parallelism = 1);
}  // namespace ps::execution_internal
