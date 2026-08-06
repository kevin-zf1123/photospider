/**
 * @file b1_evidence.cpp
 * @brief Implements fixed B1 observations and fail-closed row evaluation.
 */
#include "benchmark/b1_evidence.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <limits>
#include <map>
#include <memory>
#include <set>
#include <stdexcept>
#include <string>
#include <tuple>
#include <utility>
#include <vector>

namespace ps::benchmark {
namespace {

/** @brief One retained cancellation is sufficient to fail fault-free B1. */
constexpr std::size_t kB1CancellationCapacity = 1U;

/** @brief Exactly one current-generation publication is permitted per job. */
constexpr std::size_t kB1CurrentGenerationCapacity = 1U;

/** @brief Exact number of actual task-ready observations per B1 job. */
constexpr std::size_t kB1TaskReadyCapacity = kB1TasksPerJob;

/** @brief Exact number of actual task-terminal observations per B1 job. */
constexpr std::size_t kB1TaskTerminalCapacity = kB1TasksPerJob;

/** @brief Exactly one terminal publication is permitted per job. */
constexpr std::size_t kB1TerminalCapacity = 1U;

/** @brief Exactly one visible publication is permitted per successful job. */
constexpr std::size_t kB1VisibleCapacity = 1U;

/** @brief Exactly one Run-quiescence transition is permitted per job. */
constexpr std::size_t kB1QuiescenceCapacity = 1U;

/** @brief Exactly one root-settlement transition is permitted per job. */
constexpr std::size_t kB1ResourceSettlementCapacity = 1U;

/** @brief Frozen B1 Host resource limits from the execution-profile contract.
 */
constexpr ResourceVector kB1HostResourceLimits{
    32U, 1073741824U, 536870912U, 65536U,
    268435456U};  // NOLINT(whitespace/indent_namespace)

/** @brief Frozen configured-Metal memory and scratch limits. */
constexpr DeviceResourceVector kB1MetalResourceLimits{536870912U, 268435456U};

/**
 * @brief Fixed callback-written slot with release/acquire publication.
 * @tparam Record Complete observation record type.
 * @throws Nothing when `Record` is no-throw default constructible.
 * @note One callback owns each slot before release publication; snapshot code
 * reads it only after the matching acquire load and after caller settlement.
 */
template <typename Record>
struct PublishedB1Slot final {
  /** @brief True only after the complete record has been stored. */
  std::atomic<bool> published{false};
  /** @brief Callback-owned record storage. */
  Record value;
};

/**
 * @brief Internal exactly-once terminal record.
 * @throws Nothing for value construction and copying.
 */
struct B1TerminalRecord final {
  /** @brief Exact terminal category. */
  compute::ComputeRunTerminalKind kind =
      compute::ComputeRunTerminalKind::Failed;
  /** @brief Run identity and causal coordinate. */
  B1ObservedRunTransition transition;
};

/**
 * @brief Internal visible transition retaining the immutable product Value.
 * @throws Nothing for default construction and movement.
 * @note Digest traversal occurs only on the harness thread in `snapshot()`.
 */
struct B1VisibleRecord final {
  /** @brief Run identity and causal coordinate. */
  B1ObservedRunTransition transition;
  /** @brief Exact immutable Value published by the product contender. */
  Value output;
};

/**
 * @brief Appends every release-published fixed slot to caller-owned storage.
 * @tparam Record Complete observation record type.
 * @tparam Capacity Compile-time category capacity.
 * @param slots Fixed callback-written source slots.
 * @param output Destination vector.
 * @return Nothing after copying the current release-published cut.
 * @throws std::bad_alloc when destination storage cannot allocate.
 */
template <typename Record, std::size_t Capacity>
void append_b1_slots(const std::array<PublishedB1Slot<Record>, Capacity>& slots,
                     std::vector<Record>* output) {
  output->reserve(Capacity);
  for (const PublishedB1Slot<Record>& slot : slots) {
    if (slot.published.load(std::memory_order_acquire)) {
      output->push_back(slot.value);
    }
  }
}

/**
 * @brief Adds one human-readable structural invalidation reason.
 * @param reasons Destination diagnostic sequence.
 * @param reason Complete stable reason.
 * @return Nothing.
 * @throws std::bad_alloc when diagnostic ownership cannot allocate.
 */
void invalidate_b1(std::vector<std::string>* reasons, std::string reason) {
  reasons->push_back(std::move(reason));
}

/**
 * @brief Checked-adds one unsigned evidence charge.
 * @param total In/out aggregate.
 * @param value Nonnegative charge.
 * @return True after exact addition, false without mutation on overflow.
 * @throws Nothing.
 */
bool add_b1_charge(std::uint64_t* total, std::uint64_t value) noexcept {
  if (value > std::numeric_limits<std::uint64_t>::max() - *total) {
    return false;
  }
  *total += value;
  return true;
}

/**
 * @brief Tests exact B1 Throughput QoS including the row cap.
 * @param qos Immutable observed Run QoS.
 * @param run_cap Exact expected cap one or eight.
 * @return True only for Throughput, no deadline, weight one, and matching cap.
 * @throws Nothing.
 */
bool valid_b1_qos(const compute::ComputeRunQos& qos,
                  std::uint64_t run_cap) noexcept {
  return qos.service_class == compute::ComputeRunQosClass::Throughput &&
         !qos.deadline.has_value() && qos.weight == 1U &&
         qos.maximum_parallelism ==
             std::optional<std::uint32_t>{static_cast<std::uint32_t>(run_cap)};
}

/**
 * @brief Tests component-wise Host resource monotonicity.
 * @param lower Earlier values.
 * @param upper Later values.
 * @return True when no component decreases.
 * @throws Nothing.
 */
bool b1_resources_not_less(const ResourceVector& lower,
                           const ResourceVector& upper) noexcept {
  return lower.cpu_slots <= upper.cpu_slots &&
         lower.retained_memory_bytes <= upper.retained_memory_bytes &&
         lower.scratch_bytes <= upper.scratch_bytes &&
         lower.ready_entries <= upper.ready_entries &&
         lower.ready_bytes <= upper.ready_bytes;
}

/**
 * @brief Tests whether a Host resource vector fits its absolute limits.
 * @param value Complete current or high-water values.
 * @param limits Complete configured limits.
 * @return True when every component fits.
 * @throws Nothing.
 */
bool b1_resources_fit(const ResourceVector& value,
                      const ResourceVector& limits) noexcept {
  return value.cpu_slots <= limits.cpu_slots &&
         value.retained_memory_bytes <= limits.retained_memory_bytes &&
         value.scratch_bytes <= limits.scratch_bytes &&
         value.ready_entries <= limits.ready_entries &&
         value.ready_bytes <= limits.ready_bytes;
}

/**
 * @brief Tests component-wise device resource monotonicity.
 * @param lower Earlier values.
 * @param upper Later values.
 * @return True when neither byte dimension decreases.
 * @throws Nothing.
 */
bool b1_device_resources_not_less(const DeviceResourceVector& lower,
                                  const DeviceResourceVector& upper) noexcept {
  return lower.device_memory_bytes <= upper.device_memory_bytes &&
         lower.device_scratch_bytes <= upper.device_scratch_bytes;
}

/**
 * @brief Tests whether a device resource vector fits its limits.
 * @param value Complete current or high-water values.
 * @param limits Complete configured limits.
 * @return True when both byte dimensions fit.
 * @throws Nothing.
 */
bool b1_device_resources_fit(const DeviceResourceVector& value,
                             const DeviceResourceVector& limits) noexcept {
  return value.device_memory_bytes <= limits.device_memory_bytes &&
         value.device_scratch_bytes <= limits.device_scratch_bytes;
}

/**
 * @brief Validates a device snapshot's checked available values.
 * @param snapshot Complete device snapshot.
 * @return True when reserved fits and available is exact subtraction.
 * @throws Nothing.
 */
bool valid_b1_device_available(
    const ResourceLedger::DeviceSnapshot& snapshot) noexcept {
  return b1_device_resources_fit(snapshot.reserved, snapshot.limits) &&
         snapshot.available.device_memory_bytes ==
             snapshot.limits.device_memory_bytes -
                 snapshot.reserved.device_memory_bytes &&
         snapshot.available.device_scratch_bytes ==
             snapshot.limits.device_scratch_bytes -
                 snapshot.reserved.device_scratch_bytes;
}

/**
 * @brief Compares every execution-lifecycle counter exactly.
 * @param lhs First counter cut.
 * @param rhs Second counter cut.
 * @return True only when every current ownership count is equal.
 * @throws Nothing.
 */
bool equal_b1_lifecycle_counters(
    const compute::ExecutionLifecycleCounters& lhs,
    const compute::ExecutionLifecycleCounters& rhs) noexcept {
  return lhs.registered_graph_count == rhs.registered_graph_count &&
         lhs.open_graph_count == rhs.open_graph_count &&
         lhs.closing_graph_count == rhs.closing_graph_count &&
         lhs.pending_candidate_count == rhs.pending_candidate_count &&
         lhs.admitted_standalone_run_count ==
             rhs.admitted_standalone_run_count &&
         lhs.admitted_run_group_count == rhs.admitted_run_group_count &&
         lhs.admitted_child_run_count == rhs.admitted_child_run_count &&
         lhs.terminal_not_quiescent_run_count ==
             rhs.terminal_not_quiescent_run_count &&
         lhs.finalizing_run_count == rhs.finalizing_run_count &&
         lhs.ready_entry_count == rhs.ready_entry_count &&
         lhs.entered_callback_count == rhs.entered_callback_count &&
         lhs.live_root_reservation_count == rhs.live_root_reservation_count &&
         lhs.live_child_grant_count == rhs.live_child_grant_count &&
         lhs.live_policy_invocation_count == rhs.live_policy_invocation_count &&
         lhs.live_policy_binding_count == rhs.live_policy_binding_count;
}

/**
 * @brief Tests whether every graph-independent active lifecycle count settled.
 * @param counters Complete final process lifecycle cut.
 * @return True when no row-owned work or authority remains.
 * @throws Nothing.
 * @note Reusable registered/open Graphs and policy bindings may equal baseline.
 */
bool b1_lifecycle_work_settled(
    const compute::ExecutionLifecycleCounters& counters) noexcept {
  return counters.closing_graph_count == 0U &&
         counters.pending_candidate_count == 0U &&
         counters.admitted_standalone_run_count == 0U &&
         counters.admitted_run_group_count == 0U &&
         counters.admitted_child_run_count == 0U &&
         counters.terminal_not_quiescent_run_count == 0U &&
         counters.finalizing_run_count == 0U &&
         counters.ready_entry_count == 0U &&
         counters.entered_callback_count == 0U &&
         counters.live_root_reservation_count == 0U &&
         counters.live_child_grant_count == 0U &&
         counters.live_policy_invocation_count == 0U;
}

/**
 * @brief Validates one authority-free Compute I/O snapshot.
 * @param snapshot Candidate event-aligned state.
 * @return True for exact limits, bounded totals, and coherent phase counts.
 * @throws Nothing.
 */
bool valid_b1_io_snapshot(
    const execution::ComputeIoExecutorSnapshot& snapshot) noexcept {
  if (snapshot.task_limit != kB1ComputeIoTaskLimit ||
      snapshot.planned_bytes_limit != kB1ComputeIoPlannedByteLimit ||
      snapshot.active_tasks > snapshot.task_limit ||
      snapshot.active_planned_bytes > snapshot.planned_bytes_limit ||
      snapshot.constructing_tasks > snapshot.active_tasks ||
      snapshot.queued_tasks > snapshot.active_tasks ||
      snapshot.running_tasks > snapshot.active_tasks) {
    return false;
  }
  const std::uint64_t phase_sum = snapshot.constructing_tasks +
                                  snapshot.queued_tasks +
                                  snapshot.running_tasks;
  return phase_sum >= snapshot.constructing_tasks &&
         phase_sum >= snapshot.queued_tasks &&
         phase_sum >= snapshot.running_tasks &&
         phase_sum <= snapshot.active_tasks;
}

/**
 * @brief Compares every counter/flag in two executor snapshot cuts.
 * @param lhs First snapshot.
 * @param rhs Second snapshot.
 * @return True only when all limits, counts, and flags match exactly.
 * @throws Nothing.
 */
bool equal_b1_io_snapshot(
    const execution::ComputeIoExecutorSnapshot& lhs,
    const execution::ComputeIoExecutorSnapshot& rhs) noexcept {
  return lhs.task_limit == rhs.task_limit &&
         lhs.planned_bytes_limit == rhs.planned_bytes_limit &&
         lhs.active_tasks == rhs.active_tasks &&
         lhs.active_planned_bytes == rhs.active_planned_bytes &&
         lhs.constructing_tasks == rhs.constructing_tasks &&
         lhs.queued_tasks == rhs.queued_tasks &&
         lhs.running_tasks == rhs.running_tasks &&
         lhs.accepting == rhs.accepting &&
         lhs.shutdown_complete == rhs.shutdown_complete;
}

/**
 * @brief Compares one repeated executor-authored admission proof exactly.
 * @param lhs First event.
 * @param rhs Second event.
 * @return True only for identical sequence/decision/charge/snapshot evidence.
 * @throws Nothing.
 */
bool equal_b1_io_admission_event(
    const execution::ComputeIoAdmissionEvent& lhs,
    const execution::ComputeIoAdmissionEvent& rhs) noexcept {
  return lhs.sequence == rhs.sequence && lhs.status == rhs.status &&
         lhs.offered_planned_bytes == rhs.offered_planned_bytes &&
         lhs.charged_tasks == rhs.charged_tasks &&
         lhs.charged_planned_bytes == rhs.charged_planned_bytes &&
         equal_b1_io_snapshot(lhs.snapshot_after, rhs.snapshot_after);
}

/**
 * @brief Returns the exact planned-byte charge for one B1 I/O stage.
 * @param task Valid complete task identity.
 * @return Payload bytes or the job-specific manifest length.
 * @throws As `validate_b1_io_task_identity` and `b1_manifest_length`.
 */
std::uint64_t expected_b1_io_charge(const B1IoTaskIdentity& task) {
  validate_b1_io_task_identity(task);
  return task.stage == B1IoStage::PayloadStage
             ? kB1PayloadBytes
             : b1_manifest_length(task.job.job_index);
}

/**
 * @brief Computes the stable commit identifier used by `B1OutputStore`.
 * @param job Valid immutable occurrence.
 * @return Lowercase SHA-256 commit identifier.
 * @throws Identity, digest, and allocation errors unchanged.
 */
std::string expected_b1_commit_id(const B1JobInstance& job) {
  B1Sha256 hash;
  hash.update("execution-profile-output-commit-id-v1\n");
  hash.update(encode_b1_job_instance(job));
  return b1_digest_hex(hash.finish());
}

/**
 * @brief Returns whether a rooted slot is a single relative safe component.
 * @param slot Candidate root-relative path.
 * @return True for one nonempty ordinary component.
 * @throws Nothing.
 */
bool valid_b1_rooted_slot(const std::filesystem::path& slot) noexcept {
  return !slot.empty() && !slot.is_absolute() && slot.filename() == slot &&
         slot != "." && slot != "..";
}

/**
 * @brief Internal physical-trace evaluation for one occurrence.
 * @throws Nothing for value construction.
 */
struct B1PhysicalEvaluation final {
  /** @brief True when raw callback evidence is lossless and self-consistent. */
  bool structurally_valid = true;
  /** @brief True when all 257 distinct plan tasks started exactly once. */
  bool complete_plan = false;
  /** @brief True when terminal and current-visible success agree. */
  bool successful_visible_run = false;
  /** @brief Complete service charge for this job. */
  std::uint64_t all_service = 0U;
  /** @brief Charge of starts following an accepted cancellation. */
  std::uint64_t post_cancellation_service = 0U;
  /** @brief Later start count for duplicate Run-local task identities. */
  std::size_t duplicate_starts = 0U;
};

/**
 * @brief Evaluates the complete allocation-free callback trace for one job.
 * @param evidence Job occurrence and raw physical snapshot.
 * @param reasons Row-level structural diagnostics.
 * @return Physical validity, plan completeness, and service accounting.
 * @throws std::bad_alloc when set/diagnostic storage allocates.
 */
B1PhysicalEvaluation evaluate_b1_physical_trace(
    const B1JobEvidence& evidence, std::vector<std::string>* reasons) {
  B1PhysicalEvaluation result;
  const B1RunObservationSnapshot& trace = evidence.physical_trace;
  const auto structural_failure = [&](const std::string& detail) {
    result.structurally_valid = false;
    invalidate_b1(reasons, "job " + std::to_string(evidence.job.job_index) +
                               " physical trace: " + detail);
  };
  if (!(trace.job == evidence.job)) {
    structural_failure("occurrence identity mismatch");
  }
  if (trace.overflowed) {
    structural_failure("fixed observation capacity overflowed");
  }
  if (trace.current_generations.size() != 1U ||
      trace.current_generations.front().generation == 0U) {
    structural_failure("current-generation publication is not exactly one");
  }

  std::set<std::uint64_t> causal_sequences;
  const auto reserve_sequence = [&](std::uint64_t sequence) {
    if (sequence == 0U || !causal_sequences.insert(sequence).second) {
      structural_failure("causal coordinate is zero or duplicated");
    }
  };
  for (const B1ObservedCurrentGeneration& generation :
       trace.current_generations) {
    reserve_sequence(generation.coordinate.causal_sequence);
  }
  for (const B1ObservedCancellation& cancellation : trace.cancellations) {
    if (cancellation.run_id == 0U) {
      structural_failure("cancellation has zero Run identity");
    }
    reserve_sequence(cancellation.coordinate.causal_sequence);
  }
  for (const B1ObservedTaskReady& ready : trace.task_readies) {
    if (ready.run_id == 0U) {
      structural_failure("task ready has zero Run identity");
    }
    reserve_sequence(ready.coordinate.causal_sequence);
  }
  for (const B1ObservedTaskTerminal& terminal : trace.task_terminals) {
    if (terminal.run_id == 0U) {
      structural_failure("task terminal has zero Run identity");
    }
    reserve_sequence(terminal.coordinate.causal_sequence);
  }

  std::set<std::uint64_t> local_tasks;
  std::uint64_t expected_run_id = 0U;
  for (const B1ObservedServiceStart& start : trace.service_starts) {
    reserve_sequence(start.coordinate.causal_sequence);
    if (start.run_id == 0U || start.service_charge == 0U ||
        !valid_b1_qos(start.qos, evidence.job.run_cap)) {
      structural_failure("service start identity, charge, or QoS is invalid");
    }
    if (expected_run_id == 0U) {
      expected_run_id = start.run_id;
    } else if (start.run_id != expected_run_id) {
      structural_failure("service starts span multiple Runs");
    }
    if (!add_b1_charge(&result.all_service, start.service_charge)) {
      structural_failure("service charge sum overflowed");
    }
    if (!local_tasks.insert(start.local_task_id).second) {
      ++result.duplicate_starts;
    }
    for (const B1ObservedCancellation& cancellation : trace.cancellations) {
      if (cancellation.run_id == start.run_id &&
          start.coordinate.causal_sequence >
              cancellation.coordinate.causal_sequence &&
          !add_b1_charge(&result.post_cancellation_service,
                         start.service_charge)) {
        structural_failure("post-cancellation service sum overflowed");
      }
    }
  }
  result.complete_plan = trace.service_starts.size() == kB1TasksPerJob &&
                         local_tasks.size() == kB1TasksPerJob;
  if (result.complete_plan) {
    std::uint64_t expected_local_id = 0U;
    for (const std::uint64_t local_id : local_tasks) {
      if (local_id != expected_local_id) {
        result.complete_plan = false;
        break;
      }
      ++expected_local_id;
    }
  }

  if (!trace.terminal.has_value() || !trace.terminal_kind.has_value() ||
      !trace.quiescent.has_value() || !trace.resource_settled.has_value()) {
    structural_failure("terminal/quiescence/resource settlement is missing");
    return result;
  }
  reserve_sequence(trace.terminal->coordinate.causal_sequence);
  reserve_sequence(trace.quiescent->coordinate.causal_sequence);
  reserve_sequence(trace.resource_settled->coordinate.causal_sequence);
  if (trace.visible.has_value()) {
    reserve_sequence(trace.visible->coordinate.causal_sequence);
  }
  const std::uint64_t run_id = trace.terminal->run_id;
  if (run_id == 0U || trace.quiescent->run_id != run_id ||
      trace.resource_settled->run_id != run_id ||
      (expected_run_id != 0U && expected_run_id != run_id) ||
      (trace.visible.has_value() && trace.visible->run_id != run_id)) {
    structural_failure("Run lifecycle join identity is inconsistent");
  }
  if (!(trace.terminal->coordinate.causal_sequence <
            trace.quiescent->coordinate.causal_sequence &&
        trace.quiescent->coordinate.causal_sequence <
            trace.resource_settled->coordinate.causal_sequence)) {
    structural_failure("terminal/quiescence/resource order is invalid");
  }
  for (const B1ObservedServiceStart& start : trace.service_starts) {
    if (start.coordinate.causal_sequence >=
        trace.terminal->coordinate.causal_sequence) {
      structural_failure("service start does not precede terminal");
      break;
    }
  }
  if (trace.terminal_kind == compute::ComputeRunTerminalKind::Succeeded) {
    if (!trace.visible.has_value() ||
        trace.visible->coordinate.causal_sequence >=
            trace.terminal->coordinate.causal_sequence ||
        trace.visible_content_digest.state != ContentDigestState::Available ||
        !trace.visible_content_digest.digest.has_value()) {
      structural_failure("successful Run lacks a valid earlier visible digest");
    } else {
      result.successful_visible_run = evidence.run_succeeded;
    }
  } else if (trace.visible.has_value() || evidence.run_succeeded) {
    structural_failure("non-success terminal contradicts visible/Host result");
  }
  return result;
}

/**
 * @brief Internal output/trace/golden evaluation for one occurrence.
 * @throws Nothing for scalar construction.
 */
struct B1DeterministicEvaluation final {
  /** @brief True when canonical trace bytes and digest are well formed. */
  bool trace_valid = false;
  /** @brief True when a successful receipt is structurally complete. */
  bool artifact_valid = false;
  /** @brief True when candidate logical identity equals its golden. */
  bool logical_match = false;
  /** @brief True when candidate raw payload identity equals its golden. */
  bool raw_match = false;
  /** @brief True when the manifest digest matches exact canonical bytes. */
  bool manifest_match = false;
};

/**
 * @brief Evaluates canonical semantic and artifact identity domains.
 * @param evidence Complete job evidence.
 * @param reasons Row-level structural diagnostics.
 * @return Independent trace/artifact/golden results.
 * @throws std::bad_alloc when parsing or diagnostics allocate.
 */
B1DeterministicEvaluation evaluate_b1_deterministic_evidence(
    const B1JobEvidence& evidence, std::vector<std::string>* reasons) {
  B1DeterministicEvaluation result;
  try {
    const std::vector<B1SemanticRecord> parsed =
        parse_b1_semantic_trace(evidence.semantic_trace);
    const std::string observed_trace = encode_b1_semantic_trace(
        make_b1_observed_semantic_records(evidence.physical_trace));
    result.trace_valid =
        parsed.size() == kB1TasksPerJob * 3U &&
        parsed.front().task.job_index == evidence.job.job_index &&
        parsed.back().task.job_index == evidence.job.job_index &&
        encode_b1_semantic_trace(parsed) == evidence.semantic_trace &&
        observed_trace == evidence.semantic_trace &&
        b1_sha256(evidence.semantic_trace) == evidence.semantic_trace_digest;
  } catch (const std::exception&) {
    result.trace_valid = false;
  }

  if (!evidence.output.succeeded()) {
    if (evidence.output.status == B1OutputCommitStatus::Succeeded ||
        evidence.output.receipt.has_value()) {
      invalidate_b1(reasons,
                    "B1 output status and receipt presence are inconsistent");
    }
    return result;
  }
  const B1OutputCommitReceipt& receipt = *evidence.output.receipt;
  try {
    const B1JobGolden frozen = b1_frozen_job_golden(evidence.job.job_index);
    const std::string commit_id = expected_b1_commit_id(evidence.job);
    const std::string manifest =
        b1_artifact_manifest(evidence.job.job_index, receipt.payload_digest);
    result.artifact_valid =
        receipt.commit_id == commit_id && receipt.job == evidence.job &&
        receipt.logical_descriptor == "dense-tensor-hwc-fp32-rgba-2048x2048" &&
        receipt.committed_generation == 1U &&
        receipt.payload_name == "output.rgba32le" &&
        receipt.manifest_name == "manifest.txt" &&
        receipt.payload_length == kB1PayloadBytes &&
        receipt.manifest_length == b1_manifest_length(evidence.job.job_index) &&
        receipt.requested_durability == B1OutputDurability::CrashDurable &&
        receipt.achieved_durability == B1OutputDurability::CrashDurable &&
        !receipt.published_manifest_identity.empty() &&
        valid_b1_rooted_slot(receipt.rooted_slot) &&
        receipt.rooted_slot ==
            std::filesystem::path("occurrence-" + commit_id) &&
        receipt.resolved_root.is_absolute();
    result.manifest_match = receipt.manifest_length == manifest.size() &&
                            receipt.manifest_digest == b1_sha256(manifest);
    result.logical_match =
        evidence.golden.job_index == evidence.job.job_index &&
        evidence.golden.logical_digest == frozen.logical_digest &&
        receipt.logical_content_digest == frozen.logical_digest;
    result.raw_match =
        evidence.golden.job_index == evidence.job.job_index &&
        evidence.golden.raw_payload_digest == frozen.raw_payload_digest &&
        receipt.payload_digest == frozen.raw_payload_digest;
  } catch (const std::exception&) {
    result.artifact_valid = false;
  }
  return result;
}

/**
 * @brief Internal Compute I/O evidence evaluation for one occurrence.
 * @throws Nothing for scalar construction.
 */
struct B1IoEvaluation final {
  /** @brief True when every snapshot/status/identity relation is coherent. */
  bool structurally_valid = true;
  /** @brief True when exactly two attempt-zero tasks accepted and succeeded. */
  bool fault_free_complete = false;
  /** @brief Duplicate accepted admissions for one stage/attempt. */
  std::size_t duplicate_admissions = 0U;
  /** @brief Accepted or offered task records using attempts above zero. */
  std::size_t retry_records = 0U;
  /** @brief Maximum observed active task count. */
  std::uint64_t task_high_water = 0U;
  /** @brief Maximum observed active planned bytes. */
  std::uint64_t planned_byte_high_water = 0U;
};

/**
 * @brief Evaluates event-aligned Compute I/O observations for one job.
 * @param evidence Complete job occurrence and output evidence.
 * @param reasons Row-level structural diagnostics.
 * @return I/O structural, fault-free, retry, and high-water facts.
 * @throws std::bad_alloc when maps or diagnostics allocate.
 */
B1IoEvaluation evaluate_b1_io_evidence(const B1JobEvidence& evidence,
                                       std::vector<std::string>* reasons) {
  B1IoEvaluation result;
  const auto structural_failure = [&](const std::string& detail) {
    result.structurally_valid = false;
    invalidate_b1(reasons, "job " + std::to_string(evidence.job.job_index) +
                               " Compute I/O: " + detail);
  };

  /** @brief Exact ordered state of the two-stage observation protocol. */
  enum class StreamState : std::uint8_t {
    /** @brief The first row boundary has not been consumed. */
    Initial,
    /** @brief Payload attempt-zero offer or typed rejection is next. */
    PayloadOffer,
    /** @brief The accepted payload task must settle next. */
    PayloadSettlement,
    /** @brief Manifest attempt-zero offer or typed rejection is next. */
    ManifestOffer,
    /** @brief The accepted manifest task must settle next. */
    ManifestSettlement,
    /** @brief The row-level final boundary must appear next. */
    Final,
    /** @brief The final boundary was consumed; no rows remain legal. */
    Complete,
  };

  /** @brief Exact product path that made the next boundary Final. */
  enum class TerminalPath : std::uint8_t {
    /** @brief No structurally valid terminal path has completed yet. */
    None,
    /** @brief A terminal rejection or bounded capacity exhaustion occurred. */
    AdmissionRejected,
    /** @brief One accepted payload or manifest task failed or cancelled. */
    SettlementFailed,
    /** @brief Both payload and manifest tasks settled successfully. */
    IoCompleted,
  };

  StreamState state = StreamState::Initial;
  TerminalPath terminal_path = TerminalPath::None;
  std::map<B1IoStage, std::size_t> accepted;
  std::map<B1IoStage, std::size_t> settled;
  std::map<B1IoStage, std::size_t> capacity_rejections;
  std::map<B1IoStage, execution::ComputeIoAdmissionEvent> accepted_events;
  std::uint64_t last_accounting_event_sequence = 0U;
  const auto boundary_fields_empty = [](const B1ComputeIoObservation& value) {
    return !value.task.has_value() && value.planned_bytes == 0U &&
           !value.admission.has_value() && !value.completion.has_value() &&
           !value.admission_event.has_value() &&
           !value.settlement_event.has_value();
  };

  for (const B1ComputeIoObservation& observation :
       evidence.output.io_observations) {
    if (!valid_b1_io_snapshot(observation.snapshot)) {
      structural_failure("snapshot limits or phase totals are invalid");
    }
    result.task_high_water =
        std::max(result.task_high_water, observation.snapshot.active_tasks);
    result.planned_byte_high_water =
        std::max(result.planned_byte_high_water,
                 observation.snapshot.active_planned_bytes);

    if (state == StreamState::Complete) {
      structural_failure("observation appears after the final boundary");
      continue;
    }
    if (state == StreamState::Initial) {
      if (observation.point != B1IoObservationPoint::Initial ||
          !boundary_fields_empty(observation)) {
        structural_failure("Initial is not the first row boundary");
      } else {
        state = StreamState::PayloadOffer;
      }
      continue;
    }

    if (observation.point == B1IoObservationPoint::Final) {
      if (state != StreamState::Final || !boundary_fields_empty(observation)) {
        structural_failure("Final is misplaced or contains task fields");
      }
      state = StreamState::Complete;
      continue;
    }

    const bool payload_stage = state == StreamState::PayloadOffer ||
                               state == StreamState::PayloadSettlement;
    const B1IoStage expected_stage =
        payload_stage ? B1IoStage::PayloadStage : B1IoStage::ManifestCommit;
    if (!observation.task.has_value() ||
        !(observation.task->job == evidence.job) ||
        observation.task->stage != expected_stage) {
      structural_failure("task job or stage does not match stream state");
      continue;
    }
    if (observation.task->attempt != 0U) {
      ++result.retry_records;
      structural_failure("task attempt is not the frozen attempt zero");
    }
    try {
      if (observation.planned_bytes !=
          expected_b1_io_charge(*observation.task)) {
        structural_failure("task planned-byte charge changed");
      }
    } catch (const std::exception&) {
      structural_failure("task identity is invalid");
      continue;
    }
    if (!observation.admission.has_value() ||
        !observation.admission_event.has_value() ||
        observation.admission_event->status != *observation.admission ||
        observation.admission_event->offered_planned_bytes !=
            observation.planned_bytes ||
        observation.admission_event->sequence == 0U) {
      structural_failure(
          "task lacks an exact executor-authored admission event");
      continue;
    }

    if (state == StreamState::PayloadOffer ||
        state == StreamState::ManifestOffer) {
      if (observation.settlement_event.has_value() ||
          !equal_b1_io_snapshot(observation.snapshot,
                                observation.admission_event->snapshot_after) ||
          observation.admission_event->sequence <=
              last_accounting_event_sequence) {
        structural_failure(
            "offer event sequence or same-lock snapshot is invalid");
        continue;
      }
      last_accounting_event_sequence = observation.admission_event->sequence;
      if (observation.point == B1IoObservationPoint::OfferRejected) {
        if (*observation.admission ==
                execution::ComputeIoAdmissionStatus::Accepted ||
            observation.completion.has_value() ||
            observation.admission_event->charged_tasks != 0U ||
            observation.admission_event->charged_planned_bytes != 0U) {
          structural_failure("offer rejection status is invalid");
          continue;
        }
        if (*observation.admission ==
                execution::ComputeIoAdmissionStatus::TaskLimit ||
            *observation.admission ==
                execution::ComputeIoAdmissionStatus::PlannedByteLimit) {
          const std::size_t rejection_count =
              ++capacity_rejections[expected_stage];
          if (rejection_count == kB1CapacityAdmissionAttemptLimit) {
            state = StreamState::Final;
            terminal_path = TerminalPath::AdmissionRejected;
          }
        } else {
          state = StreamState::Final;
          terminal_path = TerminalPath::AdmissionRejected;
        }
        continue;
      }
      if (observation.point != B1IoObservationPoint::AcceptedAdmission ||
          observation.admission !=
              execution::ComputeIoAdmissionStatus::Accepted ||
          observation.completion.has_value() ||
          observation.admission_event->charged_tasks != 1U ||
          observation.admission_event->charged_planned_bytes !=
              observation.planned_bytes ||
          observation.snapshot.active_tasks < 1U ||
          observation.snapshot.active_planned_bytes <
              observation.planned_bytes) {
        structural_failure("offer state did not contain a valid admission");
        continue;
      }
      if (++accepted[expected_stage] > 1U) {
        ++result.duplicate_admissions;
        structural_failure("task stage was admitted more than once");
      }
      accepted_events[expected_stage] = *observation.admission_event;
      state = payload_stage ? StreamState::PayloadSettlement
                            : StreamState::ManifestSettlement;
      continue;
    }

    if (state == StreamState::PayloadSettlement ||
        state == StreamState::ManifestSettlement) {
      if (observation.point != B1IoObservationPoint::Settlement ||
          observation.admission !=
              execution::ComputeIoAdmissionStatus::Accepted ||
          !observation.completion.has_value() ||
          !observation.settlement_event.has_value()) {
        structural_failure("settlement state or status is invalid");
        continue;
      }
      const auto accepted_event = accepted_events.find(expected_stage);
      if (accepted_event == accepted_events.end() ||
          !equal_b1_io_admission_event(accepted_event->second,
                                       *observation.admission_event) ||
          observation.settlement_event->sequence <=
              last_accounting_event_sequence ||
          observation.settlement_event->admission_sequence !=
              accepted_event->second.sequence ||
          observation.settlement_event->status != *observation.completion ||
          observation.settlement_event->released_tasks != 1U ||
          observation.settlement_event->released_planned_bytes !=
              observation.planned_bytes ||
          !equal_b1_io_snapshot(observation.snapshot,
                                observation.settlement_event->snapshot_after)) {
        structural_failure(
            "settlement is not bound to the exact admitted task charge");
        continue;
      }
      last_accounting_event_sequence = observation.settlement_event->sequence;
      if (++settled[expected_stage] > 1U) {
        structural_failure("task stage settled more than once");
      }
      if (*observation.completion !=
          execution::ComputeIoCompletionStatus::Succeeded) {
        state = StreamState::Final;
        terminal_path = TerminalPath::SettlementFailed;
      } else if (payload_stage) {
        state = StreamState::ManifestOffer;
      } else {
        state = StreamState::Final;
        terminal_path = TerminalPath::IoCompleted;
      }
      continue;
    }

    structural_failure("observation point does not match the current state");
  }

  if (state != StreamState::Complete) {
    structural_failure("observation stream ended before the final boundary");
  }
  const bool payload_succeeded = accepted[B1IoStage::PayloadStage] == 1U &&
                                 settled[B1IoStage::PayloadStage] == 1U;
  const bool manifest_succeeded = accepted[B1IoStage::ManifestCommit] == 1U &&
                                  settled[B1IoStage::ManifestCommit] == 1U;
  result.fault_free_complete =
      state == StreamState::Complete &&
      terminal_path == TerminalPath::IoCompleted && payload_succeeded &&
      manifest_succeeded && result.duplicate_admissions == 0U &&
      result.retry_records == 0U &&
      capacity_rejections[B1IoStage::PayloadStage] == 0U &&
      capacity_rejections[B1IoStage::ManifestCommit] == 0U;
  if ((evidence.output.status == B1OutputCommitStatus::Succeeded) !=
      evidence.output.receipt.has_value()) {
    structural_failure("output status and receipt presence disagree");
  }
  const bool failed_task_status =
      evidence.output.status == B1OutputCommitStatus::TaskFailed ||
      evidence.output.status == B1OutputCommitStatus::RootUnavailable ||
      evidence.output.status == B1OutputCommitStatus::DurabilityUnsupported ||
      evidence.output.status == B1OutputCommitStatus::RevalidationFailed;
  if ((terminal_path == TerminalPath::AdmissionRejected &&
       (evidence.output.status != B1OutputCommitStatus::AdmissionFailed ||
        evidence.output.receipt.has_value())) ||
      (terminal_path == TerminalPath::SettlementFailed &&
       (!failed_task_status || evidence.output.receipt.has_value())) ||
      (terminal_path == TerminalPath::IoCompleted &&
       !((evidence.output.status == B1OutputCommitStatus::Succeeded &&
          evidence.output.receipt.has_value()) ||
         (evidence.output.status == B1OutputCommitStatus::RevalidationFailed &&
          !evidence.output.receipt.has_value())))) {
    structural_failure("terminal I/O path disagrees with output status");
  }
  if (evidence.output.succeeded()) {
    for (const B1ComputeIoObservation& observation :
         evidence.output.io_observations) {
      if (observation.point == B1IoObservationPoint::Settlement &&
          observation.completion !=
              execution::ComputeIoCompletionStatus::Succeeded) {
        result.fault_free_complete = false;
      }
    }
    if (!result.fault_free_complete) {
      structural_failure("successful receipt lacks the exact two-task FSM");
    }
  }
  return result;
}

/**
 * @brief Builds the exact expected isolated-row occurrence set.
 * @param replicate_ordinal Fresh-process replicate in `[1,3]`.
 * @param run_cap Exact cap one or eight.
 * @return Cold, three warmup, and thirty measured identities.
 * @throws std::bad_alloc when result storage allocates.
 */
std::vector<B1JobInstance> expected_b1_jobs(std::uint64_t replicate_ordinal,
                                            std::uint64_t run_cap) {
  std::vector<B1JobInstance> jobs;
  jobs.reserve(1U + kB1WarmupJobCount + kB1MeasuredJobCount);
  jobs.push_back(B1JobInstance{kB1WorkloadId, replicate_ordinal,
                               B1JobPhase::Cold, 0U, kB1ColdJobIndex, run_cap});
  for (const std::uint64_t job_index : kB1WarmupJobIndices) {
    jobs.push_back(B1JobInstance{kB1WorkloadId, replicate_ordinal,
                                 B1JobPhase::Warmup, 0U, job_index, run_cap});
  }
  for (std::size_t job_index = 0U; job_index < kB1MeasuredJobCount;
       ++job_index) {
    jobs.push_back(
        B1JobInstance{kB1WorkloadId, replicate_ordinal, B1JobPhase::Measured,
                      0U, static_cast<std::uint64_t>(job_index), run_cap});
  }
  return jobs;
}

/**
 * @brief Returns the exact contiguous graph-local offer ordinal for one job.
 * @param job Frozen isolated-row occurrence.
 * @return Zero-based ordinal within Graph A or Graph B across the complete row.
 * @throws std::invalid_argument for an unsupported occurrence.
 */
std::uint64_t expected_b1_offer_ordinal(const B1JobInstance& job) {
  if (job.phase == B1JobPhase::Cold && job.job_index == 252U) {
    return 0U;
  }
  if (job.phase == B1JobPhase::Warmup) {
    if (job.job_index == 253U) {
      return 0U;
    }
    if (job.job_index == 254U || job.job_index == 255U) {
      return 1U;
    }
  }
  if (job.phase == B1JobPhase::Measured && job.job_index < 30U) {
    return 2U + job.job_index / 2U;
  }
  throw std::invalid_argument("B1 offer ordinal received an unknown job.");
}

/**
 * @brief Validates exact baseline/final resource and lifecycle closure.
 * @param input Complete row snapshots.
 * @param reasons Row-level structural diagnostics.
 * @return True when limits, high-water, losslessness, and settlement cohere.
 * @throws std::bad_alloc when diagnostics allocate.
 */
bool validate_b1_memory_evidence(const B1InnerRowInput& input,
                                 std::vector<std::string>* reasons) {
  bool valid = true;
  const auto fail = [&](const std::string& reason) {
    valid = false;
    invalidate_b1(reasons, "B1 memory evidence: " + reason);
  };
  const ResourceLedger::Snapshot& initial =
      input.initial_snapshot.host_resources;
  const ResourceLedger::Snapshot& final = input.final_snapshot.host_resources;
  if (initial.limits != kB1HostResourceLimits ||
      final.limits != kB1HostResourceLimits) {
    fail("Host limits do not equal the frozen configuration");
  }
  if (initial.reserved != final.reserved ||
      !b1_resources_not_less(initial.high_water, final.high_water) ||
      !b1_resources_fit(initial.reserved, initial.limits) ||
      !b1_resources_fit(final.reserved, final.limits) ||
      !b1_resources_fit(initial.high_water, initial.limits) ||
      !b1_resources_fit(final.high_water, final.limits)) {
    fail("Host settlement, monotonic high-water, or limits are inconsistent");
  }

  if (input.initial_snapshot.device_resources.size() !=
      input.final_snapshot.device_resources.size()) {
    fail("configured device inventory changed during the row");
  } else {
    for (std::size_t index = 0U;
         index < input.initial_snapshot.device_resources.size(); ++index) {
      const ResourceLedger::DeviceSnapshot& before =
          input.initial_snapshot.device_resources[index];
      const ResourceLedger::DeviceSnapshot& after =
          input.final_snapshot.device_resources[index];
      if (before.device != after.device || before.limits != after.limits ||
          before.reserved != after.reserved ||
          !b1_device_resources_not_less(before.high_water, after.high_water) ||
          !b1_device_resources_fit(before.high_water, before.limits) ||
          !b1_device_resources_fit(after.high_water, after.limits) ||
          !valid_b1_device_available(before) ||
          !valid_b1_device_available(after)) {
        fail("device identity, settlement, high-water, or available drifted");
      }
      if (before.device.backend() == DeviceBackend::Metal &&
          before.limits != kB1MetalResourceLimits) {
        fail("configured Metal limits do not equal the frozen configuration");
      }
    }
  }

  const compute::ExecutionLifecyclePage& initial_lifecycle =
      input.initial_snapshot.lifecycle;
  const compute::ExecutionLifecyclePage& final_lifecycle =
      input.final_snapshot.lifecycle;
  if (initial_lifecycle.schema_version !=
          compute::kExecutionLifecycleTelemetrySchemaVersion ||
      final_lifecycle.schema_version !=
          compute::kExecutionLifecycleTelemetrySchemaVersion ||
      initial_lifecycle.service_instance_id == 0U ||
      initial_lifecycle.service_instance_id !=
          final_lifecycle.service_instance_id ||
      initial_lifecycle.telemetry_epoch == 0U ||
      initial_lifecycle.telemetry_epoch != final_lifecycle.telemetry_epoch ||
      initial_lifecycle.global_dropped_total !=
          final_lifecycle.global_dropped_total ||
      initial_lifecycle.global_dropped_saturated ||
      final_lifecycle.global_dropped_saturated ||
      final_lifecycle.cursor_gap != 0U || final_lifecycle.has_more ||
      !equal_b1_lifecycle_counters(initial_lifecycle.counters,
                                   final_lifecycle.counters) ||
      !b1_lifecycle_work_settled(final_lifecycle.counters)) {
    fail("lifecycle identity, losslessness, or final counters are invalid");
  }

  if (!valid_b1_io_snapshot(input.initial_snapshot.compute_io) ||
      !valid_b1_io_snapshot(input.final_snapshot.compute_io) ||
      input.initial_snapshot.compute_io.active_tasks != 0U ||
      input.initial_snapshot.compute_io.active_planned_bytes != 0U ||
      input.final_snapshot.compute_io.active_tasks != 0U ||
      input.final_snapshot.compute_io.active_planned_bytes != 0U ||
      input.final_snapshot.compute_io.constructing_tasks != 0U ||
      input.final_snapshot.compute_io.queued_tasks != 0U ||
      input.final_snapshot.compute_io.running_tasks != 0U) {
    fail("Compute I/O baseline or final state is not exactly quiescent");
  }
  return valid;
}

/**
 * @brief Validates one job's before/after authority-free snapshot cuts.
 * @param evidence Complete job carrying both event cuts.
 * @param reasons Row-level structural diagnostics.
 * @return True when identities, limits, bounds, and high-water are coherent.
 * @throws std::bad_alloc when diagnostics allocate.
 * @note Other Graph/I/O work may remain active at a job endpoint, so only the
 * final row snapshot—not this helper—requires exact zero settlement.
 */
bool validate_b1_job_execution_evidence(const B1JobEvidence& evidence,
                                        std::vector<std::string>* reasons) {
  bool valid = true;
  const auto fail = [&](const std::string& detail) {
    valid = false;
    invalidate_b1(reasons, "job " + std::to_string(evidence.job.job_index) +
                               " execution snapshot: " + detail);
  };
  const B1ExecutionSnapshot& before = evidence.execution_before;
  const B1ExecutionSnapshot& after = evidence.execution_after;
  if (before.host_resources.limits != kB1HostResourceLimits ||
      after.host_resources.limits != kB1HostResourceLimits ||
      !b1_resources_fit(before.host_resources.reserved,
                        before.host_resources.limits) ||
      !b1_resources_fit(after.host_resources.reserved,
                        after.host_resources.limits) ||
      !b1_resources_fit(before.host_resources.high_water,
                        before.host_resources.limits) ||
      !b1_resources_fit(after.host_resources.high_water,
                        after.host_resources.limits) ||
      !b1_resources_not_less(before.host_resources.high_water,
                             after.host_resources.high_water)) {
    fail("Host limits, bounds, or high-water are inconsistent");
  }
  if (before.device_resources.size() != after.device_resources.size()) {
    fail("configured device inventory changed");
  } else {
    for (std::size_t index = 0U; index < before.device_resources.size();
         ++index) {
      const ResourceLedger::DeviceSnapshot& first =
          before.device_resources[index];
      const ResourceLedger::DeviceSnapshot& second =
          after.device_resources[index];
      if (first.device != second.device || first.limits != second.limits ||
          !valid_b1_device_available(first) ||
          !valid_b1_device_available(second) ||
          !b1_device_resources_not_less(first.high_water, second.high_water) ||
          !b1_device_resources_fit(first.high_water, first.limits) ||
          !b1_device_resources_fit(second.high_water, second.limits)) {
        fail("device identity, bounds, or high-water are inconsistent");
      }
    }
  }
  if (before.lifecycle.schema_version !=
          compute::kExecutionLifecycleTelemetrySchemaVersion ||
      after.lifecycle.schema_version !=
          compute::kExecutionLifecycleTelemetrySchemaVersion ||
      before.lifecycle.service_instance_id == 0U ||
      before.lifecycle.service_instance_id !=
          after.lifecycle.service_instance_id ||
      before.lifecycle.telemetry_epoch == 0U ||
      before.lifecycle.telemetry_epoch != after.lifecycle.telemetry_epoch ||
      before.lifecycle.global_dropped_total !=
          after.lifecycle.global_dropped_total ||
      before.lifecycle.global_dropped_saturated ||
      after.lifecycle.global_dropped_saturated ||
      after.lifecycle.cursor_gap != 0U || after.lifecycle.has_more) {
    fail("lifecycle identity or losslessness is invalid");
  }
  if (!valid_b1_io_snapshot(before.compute_io) ||
      !valid_b1_io_snapshot(after.compute_io)) {
    fail("Compute I/O limits or phase totals are invalid");
  }
  return valid;
}

/**
 * @brief Extracts one deterministic comparison identity from a successful job.
 * @param job Complete verified job evidence.
 * @return Logical, payload, manifest, trace, and golden identity tuple.
 * @throws std::invalid_argument when a receipt is absent.
 */
auto b1_comparison_identity(const B1JobEvidence& job) {
  if (!job.output.receipt.has_value()) {
    throw std::invalid_argument("B1 comparison job lacks an output receipt.");
  }
  const B1OutputCommitReceipt& receipt = *job.output.receipt;
  return std::tuple(receipt.logical_content_digest, receipt.payload_digest,
                    receipt.manifest_digest, job.semantic_trace_digest,
                    job.golden.logical_digest, job.golden.raw_payload_digest);
}

}  // namespace

/**
 * @brief Shared fixed-capacity sink implementation for one B1 request.
 * @throws Nothing from product callbacks after successful construction.
 */
class B1RunObservationCollector::Impl final
    : public compute::ComputeRunObservationSink {
 public:
  /**
   * @brief Binds the complete occurrence before product admission.
   * @param job Valid immutable job identity.
   * @throws std::bad_alloc when occurrence strings cannot be copied.
   */
  explicit Impl(B1JobInstance job) : job_(std::move(job)) {}

  /** @copydoc compute::ComputeRunObservationSink::reserve_causal_coordinate */
  compute::ComputeRunObservationCoordinate reserve_causal_coordinate() noexcept
      override {
    const std::chrono::steady_clock::time_point observed_at =
        std::chrono::steady_clock::now();
    const std::uint64_t sequence =
        next_causal_sequence_.fetch_add(1U, std::memory_order_relaxed);
    if (sequence == 0U ||
        sequence == std::numeric_limits<std::uint64_t>::max()) {
      overflowed_.store(true, std::memory_order_release);
    }
    return compute::ComputeRunObservationCoordinate{observed_at, sequence};
  }

  /** @copydoc compute::ComputeRunObservationSink::on_current_generation */
  void on_current_generation(
      const compute::SupersessionIdentity& identity,
      compute::ComputeRunObservationCoordinate coordinate) noexcept override {
    publish(
        current_generations_, next_current_generation_,
        B1ObservedCurrentGeneration{identity.generation.value(), coordinate});
  }

  /** @copydoc compute::ComputeRunObservationSink::observes_task_semantics */
  bool observes_task_semantics() const noexcept override { return true; }

  /** @copydoc compute::ComputeRunObservationSink::on_task_ready */
  void on_task_ready(
      const compute::ComputeRunDescriptor& descriptor,
      compute::ComputeRunTaskIdentity task_identity,
      const compute::ComputeRunTaskReadyObservation& observation,
      compute::ComputeRunObservationCoordinate coordinate) noexcept override {
    B1ObservedTaskReady ready;
    ready.run_id = descriptor.id().value();
    ready.local_task_id = task_identity.local_task_id().value();
    ready.coordinate = coordinate;
    if ((observation.dependency_task_count != 0U &&
         observation.dependency_task_ids == nullptr) ||
        observation.dependency_task_count > ready.dependencies.size()) {
      overflowed_.store(true, std::memory_order_release);
      return;
    }
    ready.dependency_count = observation.dependency_task_count;
    for (std::size_t index = 0U; index < ready.dependency_count; ++index) {
      const int dependency = observation.dependency_task_ids[index];
      if (dependency < 0) {
        overflowed_.store(true, std::memory_order_release);
        return;
      }
      ready.dependencies[index] = static_cast<std::uint64_t>(dependency);
    }

    std::uint64_t logical_ready_bytes = 0U;
    if (observation.tiled) {
      if (observation.output_width <= 0 || observation.output_height <= 0) {
        overflowed_.store(true, std::memory_order_release);
        return;
      }
      constexpr std::uint64_t kBytesPerB1Pixel =
          kB1ChannelCount * sizeof(float);
      const std::uint64_t width =
          static_cast<std::uint64_t>(observation.output_width);
      const std::uint64_t height =
          static_cast<std::uint64_t>(observation.output_height);
      if (width > std::numeric_limits<std::uint64_t>::max() / height ||
          width * height >
              std::numeric_limits<std::uint64_t>::max() / kBytesPerB1Pixel) {
        overflowed_.store(true, std::memory_order_release);
        return;
      }
      logical_ready_bytes = width * height * kBytesPerB1Pixel;
    }
    ready.resources =
        B1SemanticResourceVector{observation.work_units,
                                 1U,
                                 logical_ready_bytes,
                                 observation.device == Device::CPU ? 1U : 0U,
                                 observation.retained_memory_bytes,
                                 observation.scratch_bytes,
                                 0U,
                                 0U};
    publish(task_readies_, next_task_ready_, std::move(ready));
  }

  /** @copydoc compute::ComputeRunObservationSink::on_service_start */
  void on_service_start(
      const compute::ComputeRunDescriptor& descriptor,
      compute::ComputeRunTaskIdentity task_identity,
      std::uint64_t service_charge,
      compute::ComputeRunObservationCoordinate coordinate) noexcept override {
    publish(service_starts_, next_service_start_,
            B1ObservedServiceStart{
                descriptor.id().value(), task_identity.local_task_id().value(),
                service_charge, descriptor.qos(), coordinate});
  }

  /** @copydoc compute::ComputeRunObservationSink::on_task_terminal */
  void on_task_terminal(
      const compute::ComputeRunDescriptor& descriptor,
      compute::ComputeRunTaskIdentity task_identity,
      compute::ComputeRunTaskTerminalKind kind,
      compute::ComputeRunObservationCoordinate coordinate) noexcept override {
    publish(task_terminals_, next_task_terminal_,
            B1ObservedTaskTerminal{descriptor.id().value(),
                                   task_identity.local_task_id().value(), kind,
                                   coordinate});
  }

  /** @copydoc compute::ComputeRunObservationSink::on_cancellation */
  void on_cancellation(
      const compute::ComputeRunDescriptor& descriptor,
      compute::ComputeRunCancellationReason reason,
      compute::ComputeRunObservationCoordinate coordinate) noexcept override {
    publish(
        cancellations_, next_cancellation_,
        B1ObservedCancellation{descriptor.id().value(), reason, coordinate});
  }

  /** @copydoc compute::ComputeRunObservationSink::on_terminal */
  void on_terminal(
      const compute::ComputeRunDescriptor& descriptor,
      compute::ComputeRunTerminalKind kind,
      compute::ComputeRunObservationCoordinate coordinate) noexcept override {
    publish(terminals_, next_terminal_,
            B1TerminalRecord{kind, B1ObservedRunTransition{
                                       descriptor.id().value(), coordinate}});
  }

  /** @copydoc compute::ComputeRunObservationSink::on_current_visible */
  void on_current_visible(
      const compute::ComputeRunDescriptor& descriptor, Value output,
      compute::ComputeRunObservationCoordinate coordinate) noexcept override {
    publish(visible_, next_visible_,
            B1VisibleRecord{
                B1ObservedRunTransition{descriptor.id().value(), coordinate},
                std::move(output)});
  }

  /** @copydoc compute::ComputeRunObservationSink::on_run_quiescent */
  void on_run_quiescent(
      const compute::ComputeRunDescriptor& descriptor,
      compute::ComputeRunObservationCoordinate coordinate) noexcept override {
    publish(quiescent_, next_quiescent_,
            B1ObservedRunTransition{descriptor.id().value(), coordinate});
  }

  /** @copydoc compute::ComputeRunObservationSink::on_run_resource_settled */
  void on_run_resource_settled(
      const compute::ComputeRunDescriptor& descriptor,
      compute::ComputeRunObservationCoordinate coordinate) noexcept override {
    publish(resource_settled_, next_resource_settled_,
            B1ObservedRunTransition{descriptor.id().value(), coordinate});
  }

  /** @copydoc compute::ComputeRunObservationSink::on_host_settled */
  void on_host_settled(
      compute::ComputeRunObservationCoordinate coordinate) noexcept override {
    (void)coordinate;
  }

  /**
   * @brief Publishes one complete callback record into a unique fixed slot.
   * @tparam Record Complete record type.
   * @tparam Capacity Fixed category capacity.
   * @param slots Category storage.
   * @param next Atomic unique slot allocator.
   * @param record Complete callback-local record.
   * @return True after release publication, false on capacity exhaustion.
   * @throws Nothing; exhaustion marks the complete trace overflowed.
   */
  template <typename Record, std::size_t Capacity>
  bool publish(std::array<PublishedB1Slot<Record>, Capacity>& slots,
               std::atomic<std::size_t>& next, Record record) noexcept {
    const std::size_t index = next.fetch_add(1U, std::memory_order_relaxed);
    if (index >= Capacity) {
      overflowed_.store(true, std::memory_order_release);
      return false;
    }
    slots[index].value = std::move(record);
    slots[index].published.store(true, std::memory_order_release);
    return true;
  }

  /** @brief Complete immutable occurrence bound before admission. */
  B1JobInstance job_;
  /** @brief Nonzero collector-local causal sequence allocator. */
  std::atomic<std::uint64_t> next_causal_sequence_{1U};
  /** @brief Sticky capacity or sequence-overflow marker. */
  std::atomic<bool> overflowed_{false};
  /** @brief Unique current-generation slot allocator. */
  std::atomic<std::size_t> next_current_generation_{0U};
  /** @brief Unique actual task-ready slot allocator. */
  std::atomic<std::size_t> next_task_ready_{0U};
  /** @brief Unique physical service-start slot allocator. */
  std::atomic<std::size_t> next_service_start_{0U};
  /** @brief Unique actual task-terminal slot allocator. */
  std::atomic<std::size_t> next_task_terminal_{0U};
  /** @brief Unique accepted-cancellation slot allocator. */
  std::atomic<std::size_t> next_cancellation_{0U};
  /** @brief Unique terminal slot allocator. */
  std::atomic<std::size_t> next_terminal_{0U};
  /** @brief Unique visible-publication slot allocator. */
  std::atomic<std::size_t> next_visible_{0U};
  /** @brief Unique quiescence slot allocator. */
  std::atomic<std::size_t> next_quiescent_{0U};
  /** @brief Unique root-settlement slot allocator. */
  std::atomic<std::size_t> next_resource_settled_{0U};
  /** @brief Fixed current-generation storage. */
  std::array<PublishedB1Slot<B1ObservedCurrentGeneration>,
             kB1CurrentGenerationCapacity>
      current_generations_;
  /** @brief Fixed actual task-ready storage for the frozen plan. */
  std::array<PublishedB1Slot<B1ObservedTaskReady>, kB1TaskReadyCapacity>
      task_readies_;
  /** @brief Fixed service-start storage for the exact frozen plan. */
  std::array<PublishedB1Slot<B1ObservedServiceStart>, kB1TasksPerJob>
      service_starts_;
  /** @brief Fixed actual task-terminal storage for the frozen plan. */
  std::array<PublishedB1Slot<B1ObservedTaskTerminal>, kB1TaskTerminalCapacity>
      task_terminals_;
  /** @brief Fixed cancellation storage sufficient for fault-free failure. */
  std::array<PublishedB1Slot<B1ObservedCancellation>, kB1CancellationCapacity>
      cancellations_;
  /** @brief Fixed exactly-once terminal storage. */
  std::array<PublishedB1Slot<B1TerminalRecord>, kB1TerminalCapacity> terminals_;
  /** @brief Fixed exactly-once current-visible Value storage. */
  std::array<PublishedB1Slot<B1VisibleRecord>, kB1VisibleCapacity> visible_;
  /** @brief Fixed exactly-once physical quiescence storage. */
  std::array<PublishedB1Slot<B1ObservedRunTransition>, kB1QuiescenceCapacity>
      quiescent_;
  /** @brief Fixed exactly-once resource-settlement storage. */
  std::array<PublishedB1Slot<B1ObservedRunTransition>,
             kB1ResourceSettlementCapacity>
      resource_settled_;
};

B1RunObservationCollector::B1RunObservationCollector(B1JobInstance job) {
  validate_b1_job_instance(job);
  impl_ = std::make_shared<Impl>(std::move(job));
}

B1RunObservationCollector::~B1RunObservationCollector() noexcept = default;

std::shared_ptr<compute::ComputeRunObservationSink>
B1RunObservationCollector::sink() const {
  return impl_;
}

B1RunObservationSnapshot B1RunObservationCollector::snapshot() const {
  B1RunObservationSnapshot result;
  result.job = impl_->job_;
  result.overflowed = impl_->overflowed_.load(std::memory_order_acquire);
  append_b1_slots(impl_->current_generations_, &result.current_generations);
  append_b1_slots(impl_->task_readies_, &result.task_readies);
  append_b1_slots(impl_->service_starts_, &result.service_starts);
  append_b1_slots(impl_->task_terminals_, &result.task_terminals);
  append_b1_slots(impl_->cancellations_, &result.cancellations);
  if (impl_->terminals_.front().published.load(std::memory_order_acquire)) {
    result.terminal_kind = impl_->terminals_.front().value.kind;
    result.terminal = impl_->terminals_.front().value.transition;
  }
  if (impl_->visible_.front().published.load(std::memory_order_acquire)) {
    result.visible = impl_->visible_.front().value.transition;
    result.visible_content_digest =
        compute_content_digest(impl_->visible_.front().value.output);
  }
  if (impl_->quiescent_.front().published.load(std::memory_order_acquire)) {
    result.quiescent = impl_->quiescent_.front().value;
  }
  if (impl_->resource_settled_.front().published.load(
          std::memory_order_acquire)) {
    result.resource_settled = impl_->resource_settled_.front().value;
  }
  return result;
}

std::vector<B1SemanticRecord> make_b1_observed_semantic_records(
    const B1RunObservationSnapshot& snapshot) {
  validate_b1_job_instance(snapshot.job);
  if (snapshot.overflowed) {
    throw std::invalid_argument(
        "B1 semantic observation capacity or coordinate overflowed.");
  }
  if (snapshot.task_readies.size() != kB1TasksPerJob ||
      snapshot.service_starts.size() != kB1TasksPerJob ||
      snapshot.task_terminals.size() != kB1TasksPerJob) {
    throw std::invalid_argument(
        "B1 semantic observation triplets are missing or duplicated.");
  }

  std::vector<std::optional<B1ObservedTaskReady>> readies(kB1TasksPerJob);
  std::vector<std::optional<B1ObservedServiceStart>> starts(kB1TasksPerJob);
  std::vector<std::optional<B1ObservedTaskTerminal>> terminals(kB1TasksPerJob);
  std::set<std::uint64_t> causal_sequences;
  std::uint64_t expected_run_id = 0U;
  const auto validate_identity = [&](std::uint64_t run_id,
                                     std::uint64_t local_task_id,
                                     std::uint64_t sequence) {
    if (run_id == 0U || local_task_id >= kB1TasksPerJob || sequence == 0U ||
        !causal_sequences.insert(sequence).second) {
      throw std::invalid_argument(
          "B1 semantic observation identity or sequence is invalid.");
    }
    if (expected_run_id == 0U) {
      expected_run_id = run_id;
    } else if (expected_run_id != run_id) {
      throw std::invalid_argument(
          "B1 semantic observations span multiple Runs.");
    }
  };

  for (const B1ObservedTaskReady& ready : snapshot.task_readies) {
    validate_identity(ready.run_id, ready.local_task_id,
                      ready.coordinate.causal_sequence);
    std::optional<B1ObservedTaskReady>& slot =
        readies[static_cast<std::size_t>(ready.local_task_id)];
    if (slot.has_value()) {
      throw std::invalid_argument(
          "B1 semantic task-ready identity is duplicated.");
    }
    if (ready.dependency_count > ready.dependencies.size()) {
      throw std::invalid_argument(
          "B1 semantic dependency count exceeds retained storage.");
    }
    for (std::size_t index = 0U; index < ready.dependency_count; ++index) {
      if (ready.dependencies[index] >= kB1TasksPerJob ||
          (index != 0U &&
           ready.dependencies[index - 1U] >= ready.dependencies[index])) {
        throw std::invalid_argument(
            "B1 semantic dependencies are invalid or unsorted.");
      }
    }
    slot = ready;
  }
  for (const B1ObservedServiceStart& start : snapshot.service_starts) {
    validate_identity(start.run_id, start.local_task_id,
                      start.coordinate.causal_sequence);
    std::optional<B1ObservedServiceStart>& slot =
        starts[static_cast<std::size_t>(start.local_task_id)];
    if (slot.has_value() || start.service_charge == 0U) {
      throw std::invalid_argument(
          "B1 semantic task-start identity or charge is invalid.");
    }
    slot = start;
  }
  for (const B1ObservedTaskTerminal& terminal : snapshot.task_terminals) {
    validate_identity(terminal.run_id, terminal.local_task_id,
                      terminal.coordinate.causal_sequence);
    std::optional<B1ObservedTaskTerminal>& slot =
        terminals[static_cast<std::size_t>(terminal.local_task_id)];
    if (slot.has_value()) {
      throw std::invalid_argument(
          "B1 semantic task-terminal identity is duplicated.");
    }
    slot = terminal;
  }

  std::vector<B1SemanticRecord> records;
  records.reserve(kB1TasksPerJob * 3U);
  const B1GraphRole graph = b1_graph_for_job(snapshot.job.job_index);
  for (std::size_t index = 0U; index < kB1TasksPerJob; ++index) {
    if (!readies[index].has_value() || !starts[index].has_value() ||
        !terminals[index].has_value()) {
      throw std::invalid_argument(
          "B1 semantic task identities contain a contiguous-plan gap.");
    }
    const B1ObservedTaskReady& ready = *readies[index];
    const B1ObservedServiceStart& start = *starts[index];
    const B1ObservedTaskTerminal& terminal = *terminals[index];
    if (!(ready.coordinate.causal_sequence < start.coordinate.causal_sequence &&
          start.coordinate.causal_sequence <
              terminal.coordinate.causal_sequence)) {
      throw std::invalid_argument(
          "B1 semantic ready/start/terminal order is invalid.");
    }
    if (terminal.kind != compute::ComputeRunTaskTerminalKind::Succeeded) {
      throw std::invalid_argument(
          "B1 semantic task terminal outcome drifted from success.");
    }

    std::vector<std::uint64_t> dependencies;
    dependencies.reserve(ready.dependency_count);
    for (std::size_t dependency = 0U; dependency < ready.dependency_count;
         ++dependency) {
      dependencies.push_back(ready.dependencies[dependency]);
    }
    B1SemanticTask task{snapshot.job.job_index, graph,
                        static_cast<std::uint64_t>(index),
                        std::move(dependencies), ready.resources};
    records.push_back(B1SemanticRecord{task, B1SemanticAction::Ready,
                                       B1SemanticOutcome::NotApplicable});
    records.push_back(B1SemanticRecord{task, B1SemanticAction::Start,
                                       B1SemanticOutcome::NotApplicable});
    records.push_back(B1SemanticRecord{std::move(task),
                                       B1SemanticAction::Terminal,
                                       B1SemanticOutcome::Succeeded});
  }
  return records;
}

B1InnerRow evaluate_b1_inner_row(B1InnerRowInput input) {
  B1InnerRow row;
  row.evidence = std::move(input);
  bool throughput_invalid = false;
  bool determinism_invalid = false;
  bool waste_invalid = false;
  bool memory_invalid = false;

  if (row.evidence.replicate_ordinal == 0U ||
      row.evidence.replicate_ordinal > kB1ReplicateCount ||
      (row.evidence.run_cap != 1U && row.evidence.run_cap != 8U)) {
    invalidate_b1(&row.validity_reasons,
                  "B1 row replicate or Run cap is outside the frozen domain");
    throughput_invalid = determinism_invalid = waste_invalid = memory_invalid =
        true;
  }
  if (row.evidence.environment.workload_id != kB1WorkloadId ||
      row.evidence.environment.replicate_ordinal !=
          row.evidence.replicate_ordinal ||
      row.evidence.environment.run_cap != row.evidence.run_cap ||
      !row.evidence.environment.storage_eligibility.has_value() ||
      !row.evidence.environment.storage_eligibility->eligible ||
      !row.evidence.environment.storage_eligibility->reasons.empty() ||
      !compatible_b1_environments(row.evidence.environment,
                                  row.evidence.environment,
                                  B1EnvironmentRelation::CandidateReference)) {
    invalidate_b1(&row.validity_reasons,
                  "B1 row environment is malformed, ineligible, or mismatched");
    throughput_invalid = determinism_invalid = memory_invalid = true;
  }
  if (row.evidence.measurement_end <= row.evidence.measurement_start) {
    invalidate_b1(&row.validity_reasons,
                  "B1 measurement interval is empty or reversed");
    throughput_invalid = true;
  }

  const std::vector<B1JobInstance> expected =
      expected_b1_jobs(row.evidence.replicate_ordinal, row.evidence.run_cap);
  std::map<B1JobInstance, const B1JobEvidence*> indexed_jobs;
  for (const B1JobEvidence& evidence : row.evidence.jobs) {
    try {
      validate_b1_job_instance(evidence.job);
    } catch (const std::exception&) {
      invalidate_b1(&row.validity_reasons,
                    "B1 row contains an invalid job occurrence");
      throughput_invalid = determinism_invalid = waste_invalid = true;
      continue;
    }
    if (!indexed_jobs.emplace(evidence.job, &evidence).second) {
      invalidate_b1(&row.validity_reasons,
                    "B1 row contains a duplicate job occurrence");
      throughput_invalid = determinism_invalid = waste_invalid = true;
    }
  }
  if (row.evidence.jobs.size() != expected.size() ||
      indexed_jobs.size() != expected.size()) {
    invalidate_b1(&row.validity_reasons,
                  "B1 row does not contain exactly 34 unique occurrences");
    throughput_invalid = determinism_invalid = waste_invalid = true;
  }

  for (const B1JobInstance& expected_job : expected) {
    const auto found = indexed_jobs.find(expected_job);
    if (found == indexed_jobs.end()) {
      invalidate_b1(&row.validity_reasons,
                    "B1 row is missing required job " +
                        std::to_string(expected_job.job_index));
      throughput_invalid = determinism_invalid = waste_invalid = true;
      continue;
    }
    const B1JobEvidence& evidence = *found->second;
    try {
      if (evidence.producer_offer_ordinal !=
          expected_b1_offer_ordinal(evidence.job)) {
        invalidate_b1(&row.validity_reasons,
                      "B1 graph-local producer offer ordinal drifted");
        throughput_invalid = true;
      }
    } catch (const std::exception&) {
      invalidate_b1(&row.validity_reasons,
                    "B1 graph-local producer identity is invalid");
      throughput_invalid = true;
    }
    if (evidence.endpoint_at < evidence.offered_at) {
      invalidate_b1(&row.validity_reasons,
                    "B1 job endpoint precedes its offer");
      throughput_invalid = true;
    }

    const B1PhysicalEvaluation physical =
        evaluate_b1_physical_trace(evidence, &row.validity_reasons);
    const B1DeterministicEvaluation deterministic =
        evaluate_b1_deterministic_evidence(evidence, &row.validity_reasons);
    const B1IoEvaluation io =
        evaluate_b1_io_evidence(evidence, &row.validity_reasons);
    const bool execution_valid =
        validate_b1_job_execution_evidence(evidence, &row.validity_reasons);
    if (!physical.structurally_valid) {
      throughput_invalid = determinism_invalid = waste_invalid = true;
    }
    if (!io.structurally_valid) {
      throughput_invalid = determinism_invalid = waste_invalid =
          memory_invalid = true;
    }
    if (!execution_valid) {
      throughput_invalid = memory_invalid = true;
    }
    if (!physical.complete_plan) {
      determinism_invalid = true;
    }
    if (!deterministic.trace_valid) {
      ++row.semantic_trace_mismatches;
      determinism_invalid = true;
    }
    if (!deterministic.artifact_valid || !deterministic.manifest_match) {
      ++row.artifact_mismatches;
      if (evidence.output.succeeded()) {
        determinism_invalid = true;
      }
    }
    if (evidence.output.succeeded() && !deterministic.logical_match) {
      ++row.logical_golden_mismatches;
    }
    if (evidence.output.succeeded() && !deterministic.raw_match) {
      ++row.raw_golden_mismatches;
    }

    if (!add_b1_charge(&row.all_started_service, physical.all_service) ||
        !add_b1_charge(&row.post_cancellation_started_service,
                       physical.post_cancellation_service)) {
      invalidate_b1(&row.validity_reasons,
                    "B1 row service aggregate overflowed");
      waste_invalid = true;
    }
    row.duplicate_service_starts += physical.duplicate_starts;
    row.retry_service_starts += io.retry_records;
    row.duplicate_service_starts += io.duplicate_admissions;
    row.compute_io_task_high_water =
        std::max(row.compute_io_task_high_water, io.task_high_water);
    row.compute_io_planned_byte_high_water = std::max(
        row.compute_io_planned_byte_high_water, io.planned_byte_high_water);

    const bool visible_digest_match =
        evidence.output.receipt.has_value() &&
        evidence.physical_trace.visible_content_digest.state ==
            ContentDigestState::Available &&
        evidence.physical_trace.visible_content_digest.digest.has_value() &&
        *evidence.physical_trace.visible_content_digest.digest ==
            evidence.output.receipt->logical_content_digest;
    const bool verified =
        physical.structurally_valid && physical.complete_plan &&
        physical.successful_visible_run && deterministic.trace_valid &&
        deterministic.artifact_valid && deterministic.manifest_match &&
        deterministic.logical_match && deterministic.raw_match &&
        visible_digest_match && io.structurally_valid &&
        io.fault_free_complete && evidence.output.succeeded();
    if (evidence.job.phase == B1JobPhase::Measured && verified) {
      ++row.verified_measured_jobs;
    }

    std::uint64_t discarded = 0U;
    if (!verified) {
      discarded = physical.all_service;
    } else if (physical.duplicate_starts != 0U) {
      std::set<std::uint64_t> seen;
      for (const B1ObservedServiceStart& start :
           evidence.physical_trace.service_starts) {
        if (!seen.insert(start.local_task_id).second &&
            !add_b1_charge(&discarded, start.service_charge)) {
          invalidate_b1(&row.validity_reasons,
                        "B1 duplicate service sum overflowed");
          waste_invalid = true;
        }
      }
    }
    if (!add_b1_charge(&row.discarded_started_service, discarded)) {
      invalidate_b1(&row.validity_reasons,
                    "B1 discarded service aggregate overflowed");
      waste_invalid = true;
    }
  }

  const bool memory_valid =
      validate_b1_memory_evidence(row.evidence, &row.validity_reasons);
  if (!memory_valid) {
    memory_invalid = true;
  }
  row.compute_io_task_high_water =
      std::max({row.compute_io_task_high_water,
                row.evidence.initial_snapshot.compute_io.active_tasks,
                row.evidence.final_snapshot.compute_io.active_tasks});
  row.compute_io_planned_byte_high_water =
      std::max({row.compute_io_planned_byte_high_water,
                row.evidence.initial_snapshot.compute_io.active_planned_bytes,
                row.evidence.final_snapshot.compute_io.active_planned_bytes});

  if (row.verified_measured_jobs >
      std::numeric_limits<std::uint64_t>::max() / kB1SiteOperationsPerJob) {
    invalidate_b1(&row.validity_reasons,
                  "B1 successful site-operation total overflowed");
    throughput_invalid = true;
  } else {
    row.successful_site_operations =
        static_cast<std::uint64_t>(row.verified_measured_jobs) *
        kB1SiteOperationsPerJob;
  }
  if (!throughput_invalid) {
    const double seconds =
        std::chrono::duration<double>(row.evidence.measurement_end -
                                      row.evidence.measurement_start)
            .count();
    if (!(seconds > 0.0) || !std::isfinite(seconds)) {
      throughput_invalid = true;
    } else {
      row.throughput_mpix_ops_per_second =
          static_cast<double>(row.successful_site_operations) / 1000000.0 /
          seconds;
    }
  }
  if (row.all_started_service != 0U) {
    row.discarded_started_service_ratio =
        static_cast<double>(row.discarded_started_service) /
        static_cast<double>(row.all_started_service);
  }

  row.throughput_verdict =
      throughput_invalid ? I1Verdict::Invalid
                         : (row.verified_measured_jobs == kB1MeasuredJobCount
                                ? I1Verdict::Pass
                                : I1Verdict::Fail);
  row.determinism_verdict =
      determinism_invalid ? I1Verdict::Invalid
                          : (row.logical_golden_mismatches == 0U &&
                                     row.raw_golden_mismatches == 0U &&
                                     row.semantic_trace_mismatches == 0U &&
                                     row.artifact_mismatches == 0U
                                 ? I1Verdict::Pass
                                 : I1Verdict::Fail);
  row.waste_verdict =
      waste_invalid ? I1Verdict::Invalid
                    : (row.discarded_started_service == 0U &&
                               row.post_cancellation_started_service == 0U &&
                               row.duplicate_service_starts == 0U &&
                               row.retry_service_starts == 0U
                           ? I1Verdict::Pass
                           : I1Verdict::Fail);
  row.memory_verdict = memory_invalid ? I1Verdict::Invalid : I1Verdict::Pass;
  return row;
}

B1DeterminismSummary evaluate_b1_cross_row_determinism(
    const std::vector<B1InnerRow>& rows) {
  B1DeterminismSummary summary;
  summary.row_count = rows.size();
  if (rows.size() != kB1RunCaps.size() * kB1ReplicateCount) {
    return summary;
  }

  std::map<std::pair<std::uint64_t, std::uint64_t>, const B1InnerRow*> indexed;
  for (const B1InnerRow& row : rows) {
    if (row.schema != kB1InnerRowSchema ||
        row.schema_version != kB1InnerRowSchemaVersion ||
        row.workload_id != kB1WorkloadId ||
        row.determinism_verdict != I1Verdict::Pass ||
        row.evidence.jobs.size() !=
            1U + kB1WarmupJobCount + kB1MeasuredJobCount ||
        !indexed
             .emplace(std::make_pair(row.evidence.run_cap,
                                     row.evidence.replicate_ordinal),
                      &row)
             .second) {
      return summary;
    }
  }
  for (const std::uint64_t replicate : {1U, 2U, 3U}) {
    const auto cap_one = indexed.find({1U, replicate});
    const auto cap_eight = indexed.find({8U, replicate});
    if (cap_one == indexed.end() || cap_eight == indexed.end() ||
        !compatible_b1_environments(cap_one->second->evidence.environment,
                                    cap_eight->second->evidence.environment,
                                    B1EnvironmentRelation::CapOneCapEight)) {
      return summary;
    }
  }

  const B1InnerRow& baseline = rows.front();
  for (const B1InnerRow& row : rows) {
    if (row.evidence.environment.base_manifest !=
            baseline.evidence.environment.base_manifest ||
        row.evidence.environment.storage_manifest !=
            baseline.evidence.environment.storage_manifest ||
        row.evidence.environment.environment_class_manifest !=
            baseline.evidence.environment.environment_class_manifest ||
        row.evidence.environment.fixture_digest !=
            baseline.evidence.environment.fixture_digest ||
        row.evidence.environment.resource_identity !=
            baseline.evidence.environment.resource_identity) {
      return summary;
    }
  }

  std::map<std::uint64_t, decltype(b1_comparison_identity(
                              std::declval<const B1JobEvidence&>()))>
      baseline_identities;
  try {
    for (const B1JobEvidence& job : baseline.evidence.jobs) {
      baseline_identities.emplace(job.job.job_index,
                                  b1_comparison_identity(job));
    }
    if (baseline_identities.size() !=
        1U + kB1WarmupJobCount + kB1MeasuredJobCount) {
      return summary;
    }
    for (const B1InnerRow& row : rows) {
      for (const B1JobEvidence& job : row.evidence.jobs) {
        const auto expected = baseline_identities.find(job.job.job_index);
        if (expected == baseline_identities.end() ||
            expected->second != b1_comparison_identity(job)) {
          ++summary.mismatch_count;
        }
      }
    }
  } catch (const std::exception&) {
    return summary;
  }
  summary.verdict =
      summary.mismatch_count == 0U ? I1Verdict::Pass : I1Verdict::Fail;
  return summary;
}

B1ReferenceThroughputSummary evaluate_b1_reference_throughput(
    const std::vector<B1InnerRow>& candidate,
    const std::vector<B1InnerRow>& reference) {
  B1ReferenceThroughputSummary summary;
  if (candidate.size() != kB1ReplicateCount ||
      reference.size() != kB1ReplicateCount) {
    return summary;
  }
  std::map<std::uint64_t, const B1InnerRow*> candidate_by_replicate;
  std::map<std::uint64_t, const B1InnerRow*> reference_by_replicate;
  for (const B1InnerRow& row : candidate) {
    if (!candidate_by_replicate.emplace(row.evidence.replicate_ordinal, &row)
             .second) {
      return summary;
    }
  }
  for (const B1InnerRow& row : reference) {
    if (!reference_by_replicate.emplace(row.evidence.replicate_ordinal, &row)
             .second) {
      return summary;
    }
  }

  summary.replicate_ratios.reserve(kB1ReplicateCount);
  for (std::uint64_t replicate = 1U; replicate <= kB1ReplicateCount;
       ++replicate) {
    const auto candidate_row = candidate_by_replicate.find(replicate);
    const auto reference_row = reference_by_replicate.find(replicate);
    if (candidate_row == candidate_by_replicate.end() ||
        reference_row == reference_by_replicate.end() ||
        candidate_row->second->schema != kB1InnerRowSchema ||
        reference_row->second->schema != kB1InnerRowSchema ||
        candidate_row->second->throughput_verdict != I1Verdict::Pass ||
        reference_row->second->throughput_verdict != I1Verdict::Pass ||
        !candidate_row->second->throughput_mpix_ops_per_second.has_value() ||
        !reference_row->second->throughput_mpix_ops_per_second.has_value() ||
        *reference_row->second->throughput_mpix_ops_per_second <= 0.0 ||
        !compatible_b1_environments(
            candidate_row->second->evidence.environment,
            reference_row->second->evidence.environment,
            B1EnvironmentRelation::CandidateReference)) {
      summary.replicate_ratios.clear();
      return summary;
    }
    const double ratio =
        *candidate_row->second->throughput_mpix_ops_per_second /
        *reference_row->second->throughput_mpix_ops_per_second;
    if (!std::isfinite(ratio) || ratio < 0.0) {
      summary.replicate_ratios.clear();
      return summary;
    }
    summary.replicate_ratios.push_back(ratio);
  }

  std::vector<double> sorted = summary.replicate_ratios;
  std::sort(sorted.begin(), sorted.end());
  summary.median_ratio = sorted[1U];
  const bool every_passes = std::all_of(
      summary.replicate_ratios.begin(), summary.replicate_ratios.end(),
      [](double ratio) { return ratio >= kB1MinimumThroughputRatioLimit; });
  summary.verdict =
      every_passes && *summary.median_ratio >= kB1MedianThroughputRatioLimit
          ? I1Verdict::Pass
          : I1Verdict::Fail;
  return summary;
}

}  // namespace ps::benchmark
