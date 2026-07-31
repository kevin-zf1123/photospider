#pragma once

#include <functional>
#include <memory>
#include <mutex>
#include <optional>

#include "core/pending_value.hpp"
#include "photospider/data/value.hpp"
#include "photospider/memory/ready_fence.hpp"

/**
 * @file value_transfer_task.hpp
 * @brief Source-private explicit asynchronous Value replica transfer tasks.
 */

namespace ps {

/**
 * @brief Thread-safe terminal authority for one external transfer destination.
 *
 * @throws Nothing for destruction; typed failure publication can allocate.
 * @note A physical provider may retain a shared owner through native callback
 * completion. Exactly one Ready, Failed, or ProducerCancelled transition wins;
 * destroying the last unresolved owner publishes ProducerCancelled.
 */
class DeviceTransferCompletion final {
 public:
  /**
   * @brief Prevents copying one concrete terminal authority object.
   * @param other Unused source because copying is forbidden.
   * @throws Nothing because this operation is deleted.
   */
  DeviceTransferCompletion(const DeviceTransferCompletion&) = delete;
  /**
   * @brief Prevents replacing one concrete terminal authority object.
   * @param other Unused source because assignment is forbidden.
   * @return No value because this operation is deleted.
   * @throws Nothing because this operation is deleted.
   */
  DeviceTransferCompletion& operator=(const DeviceTransferCompletion&) = delete;

  /**
   * @brief Cancels an unresolved destination on final owner destruction.
   * @throws Nothing.
   */
  ~DeviceTransferCompletion() noexcept;

  /**
   * @brief Reports whether terminal authority remains unresolved.
   * @return True before the unique terminal transition.
   * @throws std::system_error when synchronization fails.
   */
  bool valid() const;

  /**
   * @brief Publishes Ready after provider visibility work is complete.
   * @return True only for the unique terminal transition.
   * @throws Nothing; synchronization failure terminates.
   */
  bool complete_ready() noexcept;

  /**
   * @brief Publishes a typed physical transfer failure.
   * @param failure Complete immutable failure diagnostic.
   * @return True only for the unique terminal transition.
   * @throws std::bad_alloc when retained diagnostic storage cannot allocate.
   * @throws std::system_error when synchronization fails.
   */
  bool complete_failed(ReadyFenceFailure failure);

  /**
   * @brief Publishes ProducerCancelled for unresolved physical work.
   * @return True only for the unique terminal transition.
   * @throws Nothing; synchronization failure terminates.
   */
  bool cancel() noexcept;

 private:
  /**
   * @brief Takes one unique pending-device producer capability.
   * @param producer Destination terminal capability.
   * @throws Nothing after argument evaluation.
   */
  explicit DeviceTransferCompletion(
      PendingDeviceValueProducer producer) noexcept;

  /** @brief Serializes provider callbacks and fallback failure settlement. */
  mutable std::mutex mutex_;
  /** @brief Unique unresolved producer, reset after terminal publication. */
  std::optional<PendingDeviceValueProducer> producer_;

  friend class ValueTransferTask;
};

/**
 * @brief Move-owned explicit transfer from one Value to a pending replica.
 *
 * Preparation allocates a distinct pending destination but copies no payload.
 * `enqueue()` registers source readiness with the injected physical execution
 * mechanism. The queued callback performs a built-in CPU envelope copy or
 * hands a retained terminal authority to an injected external provider.
 *
 * @throws Nothing for movement and destruction.
 * @note This source-private task executes one prepared AccessPlan. Residency,
 * exact Run completion identity, and stale commit remain owned by the caller's
 * physical provider/ResidencyManager.
 * @note One task object is externally serialized. Its queued callback retains
 *       shared state independently and may race only with owner destruction.
 */
class ValueTransferTask final {
 public:
  /**
   * @brief External provider entry after source readiness.
   * @param source Ready immutable source Value.
   * @param completion Shared destination terminal authority that the provider
   * may retain through native completion.
   * @return Nothing after settling or retaining completion ownership.
   * @throws Provider validation, allocation, or native submission failures.
   * @note Returning without settling or retaining completion cancels the
   * destination. The operation must never wait synchronously for a device.
   */
  using ExternalTransferOperation = std::function<void(
      const Value& source,
      const std::shared_ptr<DeviceTransferCompletion>& completion)>;

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
   * @return Move-only task with a distinct revision-preserving Pending
   * destination.
   * @throws std::invalid_argument when source is invalid, not a host-visible
   * CPU binding, or its current layout cannot be used as a positive exact
   * producer layout.
   * @throws std::overflow_error for address or identity overflow.
   * @throws std::bad_alloc when destination or task state cannot allocate.
   * @note Preparation performs no source payload access and enqueues no work.
   * The plan explicitly requires a distinct CPU Host binding.
   */
  static ValueTransferTask prepare_cpu_copy(Value source);

  /**
   * @brief Prepares one explicit external/device destination transfer.
   * @param source Valid immutable source Value retained by the task.
   * @param target Target device/domain/capability requiring Transfer.
   * @param owner Non-null complete destination allocation owner.
   * @param native_handle Non-null opaque destination native handle.
   * @param host_pointer Optional host-visible destination allocation start.
   * @param operation Nonempty provider invoked only after source Ready.
   * @return Move-only task with a distinct revision-preserving destination.
   * @throws std::invalid_argument when source/binding/provider is invalid,
   * requested host access lacks a host pointer, the source is not a positive
   * zero-offset exact non-overlapping producer layout, or access planning does
   * not select Transfer.
   * @throws std::overflow_error for envelope or identity exhaustion.
   * @throws std::bad_alloc when destination/task ownership cannot allocate.
   * @note Producer-layout validation completes before retaining `owner`,
   * minting destination identities, creating a fence, or publishing a Pending
   * destination. The destination envelope equals source.storage_size().
   * Preparation performs no payload read and submits no native work.
   */
  static ValueTransferTask prepare_external_transfer(
      Value source, AccessTarget target, std::shared_ptr<void> owner,
      void* native_handle, std::byte* host_pointer,
      ExternalTransferOperation operation);

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
   * @brief Returns the immutable access plan executed by this task.
   * @return Directly borrowed prepared Transfer plan.
   * @throws std::logic_error when this task is moved-from.
   */
  const AccessPlan& access_plan() const;

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
