#include "compute/run_group.hpp"

#include <atomic>
#include <limits>
#include <memory>
#include <new>
#include <optional>
#include <stdexcept>
#include <utility>

namespace ps::compute {
namespace {

/**
 * @brief Mints one non-reused process-lifetime RunGroup identity.
 * @return Fresh nonzero diagnostic value.
 * @throws std::overflow_error before wrap or reuse.
 * @note The allocator owns no Run, worker, Graph, or supersession authority.
 */
std::uint64_t mint_run_group_id() {
  static std::atomic<std::uint64_t> next_id{1};
  static std::atomic_flag maximum_claimed = ATOMIC_FLAG_INIT;
  std::uint64_t candidate = next_id.load(std::memory_order_relaxed);
  for (;;) {
    if (candidate == std::numeric_limits<std::uint64_t>::max()) {
      if (!maximum_claimed.test_and_set(std::memory_order_relaxed)) {
        return candidate;
      }
      throw std::overflow_error("RunGroupId sequence exhausted.");
    }
    if (next_id.compare_exchange_weak(candidate, candidate + 1,
                                      std::memory_order_relaxed,
                                      std::memory_order_relaxed)) {
      return candidate;
    }
  }
}

/**
 * @brief Tests whether an exact failure derives from resource exhaustion.
 * @param failure Candidate child exception identity.
 * @return True only when rethrow matches std::bad_alloc.
 * @throws Nothing.
 */
bool is_bad_alloc_failure(const std::exception_ptr& failure) noexcept {
  if (!failure) {
    return false;
  }
  try {
    std::rethrow_exception(failure);
  } catch (const std::bad_alloc&) {
    return true;
  } catch (...) {
    return false;
  }
}

/**
 * @brief Validates two submissions as one realtime child pair.
 * @param hp HP Full child submission.
 * @param rt RT Interactive child submission.
 * @return Nothing.
 * @throws std::invalid_argument for mismatched lineage/domain/quality.
 */
void validate_realtime_group(const ComputeRunSubmission& hp,
                             const ComputeRunSubmission& rt) {
  if (hp.graph_identity != rt.graph_identity ||
      hp.graph_instance_id != rt.graph_instance_id ||
      hp.revision != rt.revision || hp.target_node_id != rt.target_node_id ||
      !(hp.supersession.key == rt.supersession.key) ||
      !(hp.supersession.generation == rt.supersession.generation) ||
      hp.supersession.key.request_intent() != ComputeIntent::RealTimeUpdate ||
      hp.intent != ComputeIntent::GlobalHighPrecision ||
      hp.quality != ComputeRunQuality::Full ||
      rt.intent != ComputeIntent::RealTimeUpdate ||
      rt.quality != ComputeRunQuality::Interactive) {
    throw std::invalid_argument(
        "RunGroup requires one shared realtime lineage and distinct HP/RT "
        "children.");
  }
}

}  // namespace

/** @copydoc RunGroup::RunGroup */
RunGroup::RunGroup(
    ComputeRunSubmission hp_submission, ComputeRunSubmission rt_submission,
    std::shared_ptr<ComputeRequestCancellationSource> cancellation)
    : id_([&hp_submission, &rt_submission] {
        validate_realtime_group(hp_submission, rt_submission);
        return RunGroupId(mint_run_group_id());
      }()),  // NOLINT(whitespace/indent_namespace)
      supersession_(hp_submission.supersession),
      hp_run_(std::move(hp_submission)),
      rt_run_(std::move(rt_submission)),
      hp_lease_(hp_run_.acquire_lease()),
      rt_lease_(rt_run_.acquire_lease()),
      cancellation_(cancellation != nullptr
                        ? std::move(cancellation)
                        : std::make_shared<ComputeRequestCancellationSource>()),
      sibling_commit_gate_(std::make_shared<DirtySiblingCommitGate>()) {
}  // NOLINT(whitespace/indent_namespace)

/** @copydoc RunGroup::~RunGroup */
RunGroup::~RunGroup() noexcept = default;

/** @copydoc RunGroup::request_cancellation */
bool RunGroup::request_cancellation(ComputeRunCancellationReason reason) {
  sibling_commit_gate_->abort_hp_commit();
  return cancellation_->request_cancellation(reason);
}

/** @copydoc RunGroup::aggregate_terminal_outcome */
ComputeRunTerminalOutcome RunGroup::aggregate_terminal_outcome() const {
  const std::optional<ComputeRunTerminalOutcome> rt =
      rt_run_.terminal_outcome();
  const std::optional<ComputeRunTerminalOutcome> hp =
      hp_run_.terminal_outcome();
  if (!rt.has_value() || !hp.has_value()) {
    throw std::logic_error(
        "RunGroup aggregation requires both child terminal outcomes.");
  }

  if (rt->kind == ComputeRunTerminalKind::Failed &&
      is_bad_alloc_failure(rt->failure)) {
    return *rt;
  }
  if (hp->kind == ComputeRunTerminalKind::Failed &&
      is_bad_alloc_failure(hp->failure)) {
    return *hp;
  }
  if (rt->kind == ComputeRunTerminalKind::Failed) {
    return *rt;
  }
  if (hp->kind == ComputeRunTerminalKind::Failed) {
    return *hp;
  }

  const std::optional<ComputeRunCancellationReason> group_reason =
      cancellation_->accepted_child_cancellation_reason();
  if (group_reason.has_value()) {
    return ComputeRunTerminalOutcome{ComputeRunTerminalKind::Cancelled, nullptr,
                                     group_reason};
  }
  if (rt->kind == ComputeRunTerminalKind::Cancelled) {
    return *rt;
  }
  if (hp->kind == ComputeRunTerminalKind::Cancelled) {
    return *hp;
  }
  return ComputeRunTerminalOutcome{ComputeRunTerminalKind::Succeeded, nullptr,
                                   std::nullopt};
}

}  // namespace ps::compute
