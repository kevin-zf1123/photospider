#pragma once

#include <memory>

#include "photospider/data/value.hpp"
#include "photospider/memory/ready_fence.hpp"

/**
 * @file value_transfer_task.hpp
 * @brief Source-private explicit asynchronous CPU Value-copy transfer task.
 */

namespace ps {

/**
 * @brief Move-owned explicit transfer from one CPU Value to a pending copy.
 *
 * Preparation allocates a distinct pending destination but copies no payload.
 * `enqueue()` registers source readiness with the injected physical execution
 * mechanism. The queued callback performs the complete checked-envelope copy,
 * revokes destination producer access, and only then publishes Ready.
 *
 * @throws Nothing for movement and destruction.
 * @note This V-6 source-private task proves asynchronous transfer mechanics. It
 *       is not a general AccessPlan, device binding, residency replica, native
 *       command, or ComputeRun submission.
 * @note One task object is externally serialized. Its queued callback retains
 *       shared state independently and may race only with owner destruction.
 */
class ValueTransferTask final {
 public:
  /** @brief Copy construction is forbidden for one transfer ownership path. */
  ValueTransferTask(const ValueTransferTask&) = delete;

  /** @brief Copy assignment is forbidden for one transfer ownership path. */
  ValueTransferTask& operator=(const ValueTransferTask&) = delete;

  /**
   * @brief Transfers the complete unresolved or settled task state.
   *
   * @param other Task to consume.
   * @throws Nothing.
   */
  ValueTransferTask(ValueTransferTask&& other) noexcept = default;

  /**
   * @brief Replaces this task through exact ownership transfer.
   *
   * @param other Task to consume.
   * @return This task after transfer.
   * @throws Nothing.
   * @note Destroying any unresolved state previously held by this object
   *       cancels its waiter and pending destination producer.
   */
  ValueTransferTask& operator=(ValueTransferTask&& other) noexcept = default;

  /**
   * @brief Cancels unresolved destination production during destruction.
   *
   * @throws Nothing.
   */
  ~ValueTransferTask() noexcept;

  /**
   * @brief Prepares one explicit positive-layout CPU envelope copy.
   *
   * @param source Valid CPU DenseTensor Value retained by the task.
   * @return Move-only task with a distinct sealed Pending destination.
   * @throws std::invalid_argument when source is invalid or its current layout
   *         cannot be used as a positive exact producer layout.
   * @throws std::overflow_error for address or identity overflow.
   * @throws std::bad_alloc when destination or task state cannot allocate.
   * @note Preparation performs no source payload access and enqueues no work.
   */
  static ValueTransferTask prepare_cpu_copy(Value source);

  /**
   * @brief Reports whether this object retains transfer state.
   *
   * @return True before movement, including after transfer settlement.
   * @throws Nothing.
   */
  bool valid() const noexcept { return impl_ != nullptr; }

  /**
   * @brief Returns the distinct destination publication.
   *
   * @return Copyable Value whose fence is Pending until task settlement.
   * @throws std::logic_error when this task is moved-from.
   * @note Metadata and identities may be inspected before Ready; payload access
   *       remains fence-gated.
   */
  Value destination() const;

  /**
   * @brief Enqueues this transfer through source-fence readiness.
   *
   * @param executor Shared physical mechanism that asynchronously queues the
   *        transfer continuation.
   * @return Nothing.
   * @throws std::logic_error when moved-from or already enqueued.
   * @throws std::invalid_argument when executor is null.
   * @throws std::overflow_error when source-fence waiter identity is exhausted.
   * @throws std::bad_alloc when callback or waiter state cannot allocate.
   * @note A Pending source queues no runnable copy until terminal. A Ready
   *       source queues exactly one callback, which the executor must not run
   *       inline.
   */
  void enqueue(std::shared_ptr<ReadyFenceExecutor> executor);

  /**
   * @brief Reports whether explicit enqueue has succeeded.
   *
   * @return True after one wait registration is installed.
   * @throws std::logic_error when this task is moved-from.
   */
  bool enqueued() const;

 private:
  /** @brief Shared state retained weakly by the asynchronous callback. */
  struct Impl;

  /**
   * @brief Creates one task from complete prepared state.
   *
   * @param impl Shared prepared transfer state.
   * @throws Nothing.
   */
  explicit ValueTransferTask(std::shared_ptr<Impl> impl) noexcept;

  /**
   * @brief Settles one destination from a source terminal snapshot.
   *
   * Ready copies the complete envelope before producer completion. Failed and
   * ProducerCancelled propagate without source payload access. Every internal
   * exception becomes typed destination failure when allocation permits,
   * otherwise producer destruction publishes cancellation.
   *
   * @param impl Retained task state locked by the queued callback.
   * @param source_snapshot Exact terminal source observation.
   * @return Nothing.
   * @throws Nothing; callback failures settle destination state.
   */
  static void settle_from_source(const std::shared_ptr<Impl>& impl,
                                 ReadyFenceSnapshot source_snapshot) noexcept;

  /** @brief Prepared task state, or null after movement. */
  std::shared_ptr<Impl> impl_;
};

}  // namespace ps
