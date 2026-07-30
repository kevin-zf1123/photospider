#pragma once

#include <cstdint>
#include <stdexcept>

#include "photospider/core/compute_intent.hpp"
#include "photospider/data/value.hpp"

/**
 * @file device_completion.hpp
 * @brief Source-private exact identity for asynchronous device completion.
 */

namespace ps::execution {

/**
 * @brief Immutable Run/task lineage captured before native submission.
 *
 * The seed names the exact Graph instance, canonical request lineage, Run, and
 * Run-local task whose provider is about to enqueue native work. It contains no
 * allocation or producer facts; those are added only after source and
 * destination Values have been published.
 *
 * @throws std::invalid_argument for zero identities, a negative target, or an
 * unsupported request intent.
 * @note This source-private value grants no queue, payload, completion, cache,
 * or visible-commit authority.
 */
class DeviceCompletionSeed final {
 public:
  /**
   * @brief Constructs one validated native-submission lineage.
   * @param graph_instance_id Nonzero live Graph identity scalar.
   * @param target_node_id Nonnegative canonical request target.
   * @param request_intent Canonical request intent shared by sibling Runs.
   * @param supersession_generation Nonzero graph-wide request generation.
   * @param run_id Nonzero opaque Run identity scalar.
   * @param local_task_id Dense Run-local task identity.
   * @throws std::invalid_argument for invalid scalar or enum inputs.
   */
  DeviceCompletionSeed(std::uint64_t graph_instance_id, int target_node_id,
                       ComputeIntent request_intent,
                       std::uint64_t supersession_generation,
                       std::uint64_t run_id, std::uint64_t local_task_id);

  /**
   * @brief Returns the nonzero live Graph identity scalar.
   * @return Exact GraphInstanceId scalar captured before submission.
   * @throws Nothing.
   */
  std::uint64_t graph_instance_id() const noexcept {
    return graph_instance_id_;
  }

  /**
   * @brief Returns the canonical nonnegative request target.
   * @return Target node from the supersession lineage.
   * @throws Nothing.
   */
  int target_node_id() const noexcept { return target_node_id_; }

  /**
   * @brief Returns the canonical request intent.
   * @return GlobalHighPrecision or RealTimeUpdate lineage intent.
   * @throws Nothing.
   */
  ComputeIntent request_intent() const noexcept { return request_intent_; }

  /**
   * @brief Returns the nonzero graph-wide supersession generation.
   * @return Exact currentness generation captured for this submission.
   * @throws Nothing.
   */
  std::uint64_t supersession_generation() const noexcept {
    return supersession_generation_;
  }

  /**
   * @brief Returns the nonzero opaque Run identity scalar.
   * @return Exact ComputeRunId scalar.
   * @throws Nothing.
   */
  std::uint64_t run_id() const noexcept { return run_id_; }

  /**
   * @brief Returns the dense Run-local task identity scalar.
   * @return Exact local task id, including permitted zero.
   * @throws Nothing.
   */
  std::uint64_t local_task_id() const noexcept { return local_task_id_; }

  /**
   * @brief Compares every Run/task lineage component.
   * @param other Seed to compare.
   * @return True only when every component matches.
   * @throws Nothing.
   */
  bool operator==(const DeviceCompletionSeed& other) const noexcept;

 private:
  /** @brief Nonzero live Graph identity scalar. */
  std::uint64_t graph_instance_id_ = 0U;
  /** @brief Canonical request target. */
  int target_node_id_ = -1;
  /** @brief Canonical request intent. */
  ComputeIntent request_intent_ = ComputeIntent::GlobalHighPrecision;
  /** @brief Nonzero graph-wide request generation. */
  std::uint64_t supersession_generation_ = 0U;
  /** @brief Nonzero opaque Run identity scalar. */
  std::uint64_t run_id_ = 0U;
  /** @brief Dense Run-local task identity scalar. */
  std::uint64_t local_task_id_ = 0U;
};

/**
 * @brief Exact immutable identity of one submitted replica production.
 *
 * @throws std::invalid_argument when either Value is invalid, revisions differ,
 * or source and destination bindings are identical.
 * @note Completion acceptance compares this complete value. Neither Run
 * cancellation nor a matching allocation alone can substitute for the exact
 * source/destination publication facts.
 */
class DeviceCompletionIdentity final {
 public:
  /**
   * @brief Binds a submission seed to source and destination Values.
   * @param seed Exact Run/task lineage captured before native submission.
   * @param source Immutable source publication.
   * @param destination Pending destination replica publication.
   * @throws std::invalid_argument for invalid or inconsistent publications.
   */
  DeviceCompletionIdentity(DeviceCompletionSeed seed, const Value& source,
                           const Value& destination);

  /**
   * @brief Returns the exact Run/task lineage.
   * @return Borrowed immutable seed retained by this identity.
   * @throws Nothing.
   */
  const DeviceCompletionSeed& seed() const noexcept { return seed_; }

  /**
   * @brief Returns the source logical revision.
   * @return Nonzero revision captured at construction.
   * @throws Nothing.
   */
  ValueRevisionId source_revision() const noexcept { return source_revision_; }

  /**
   * @brief Returns the destination logical revision.
   * @return Revision-preserving destination token.
   * @throws Nothing.
   */
  ValueRevisionId destination_revision() const noexcept {
    return destination_revision_;
  }

  /**
   * @brief Returns the exact source producer identity.
   * @return Nonzero source publication producer token.
   * @throws Nothing.
   */
  ProducerIdentity source_producer() const noexcept { return source_producer_; }

  /**
   * @brief Returns the exact destination producer identity.
   * @return Nonzero destination publication producer token.
   * @throws Nothing.
   */
  ProducerIdentity destination_producer() const noexcept {
    return destination_producer_;
  }

  /**
   * @brief Returns the exact source physical binding.
   * @return Complete immutable source allocation facts.
   * @throws Nothing.
   */
  StorageBinding source_binding() const noexcept { return source_binding_; }

  /**
   * @brief Returns the exact destination physical binding.
   * @return Complete immutable destination allocation facts.
   * @throws Nothing.
   */
  StorageBinding destination_binding() const noexcept {
    return destination_binding_;
  }

  /**
   * @brief Compares every lineage, revision, producer, and binding fact.
   * @param other Identity to compare.
   * @return True only for the exact same submitted replica production.
   * @throws Nothing.
   */
  bool operator==(const DeviceCompletionIdentity& other) const noexcept;

 private:
  /** @brief Exact Run/task lineage. */
  DeviceCompletionSeed seed_;
  /** @brief Source logical revision. */
  ValueRevisionId source_revision_;
  /** @brief Destination logical revision. */
  ValueRevisionId destination_revision_;
  /** @brief Exact source producer identity. */
  ProducerIdentity source_producer_;
  /** @brief Exact destination producer identity. */
  ProducerIdentity destination_producer_;
  /** @brief Exact source physical binding. */
  StorageBinding source_binding_;
  /** @brief Exact destination physical binding. */
  StorageBinding destination_binding_;
};

}  // namespace ps::execution
