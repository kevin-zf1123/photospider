#include "execution/value_transfer_task.hpp"

#include <cstring>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <string>
#include <utility>

#include "core/value_validation.hpp"

namespace ps {

/**
 * @brief Complete prepared state for one explicit Value replica transfer.
 *
 * @note Field order cancels the wait registration before destroying the
 * unresolved CPU producer or external completion authority.
 */
struct ValueTransferTask::Impl final {
  /** @brief Immutable source retained through transfer settlement. */
  Value source;

  /** @brief Public immutable destination publication. */
  Value destination;

  /** @brief Immutable explicit Transfer plan used by this task. */
  AccessPlan plan;

  /** @brief Built-in CPU destination producer, or null for external transfer.
   */
  std::optional<PendingValueProducer> cpu_producer;

  /** @brief External destination terminal authority, or null for CPU copy. */
  std::shared_ptr<DeviceTransferCompletion> device_completion;

  /** @brief External physical provider invoked after source Ready. */
  ExternalTransferOperation external_operation;

  /** @brief Observer-local source wait cancelled before producer destruction.
   */
  ReadyFenceWaitRegistration wait;

  /** @brief True after one source wait has been installed. */
  bool enqueued = false;

  /**
   * @brief Stores one fully prepared transfer.
   *
   * @param source_in Source Value retained through execution.
   * @param publication Distinct pending destination and private producer.
   * @param plan_in Explicit distinct-binding CPU Transfer plan.
   * @throws Nothing under member move contracts.
   */
  Impl(Value source_in, PendingValuePublication publication,
       AccessPlan plan_in) noexcept
      : source(std::move(source_in)),
        destination(std::move(publication.value)),
        plan(std::move(plan_in)),
        cpu_producer(std::move(publication.producer)) {}

  /**
   * @brief Stores one fully prepared external/device transfer.
   * @param source_in Source Value retained through execution.
   * @param publication Revision-preserving external destination.
   * @param plan_in Explicit target Transfer plan.
   * @param operation_in Nonempty physical provider operation.
   * @throws std::bad_alloc when shared completion ownership cannot allocate.
   */
  Impl(Value source_in, PendingDeviceValuePublication publication,
       AccessPlan plan_in, ExternalTransferOperation operation_in)
      : source(std::move(source_in)),
        destination(std::move(publication.value)),
        plan(std::move(plan_in)),
        device_completion(std::shared_ptr<DeviceTransferCompletion>(
            new DeviceTransferCompletion(std::move(publication.producer)))),
        external_operation(std::move(operation_in)) {}
};

/** @copydoc DeviceTransferCompletion::DeviceTransferCompletion */
DeviceTransferCompletion::DeviceTransferCompletion(
    PendingDeviceValueProducer producer) noexcept
    : producer_(std::move(producer)) {}

/** @copydoc DeviceTransferCompletion::~DeviceTransferCompletion */
DeviceTransferCompletion::~DeviceTransferCompletion() noexcept {
  (void)cancel();
}

/** @copydoc DeviceTransferCompletion::valid */
bool DeviceTransferCompletion::valid() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return producer_.has_value() && producer_->valid();
}

/** @copydoc DeviceTransferCompletion::complete_ready */
bool DeviceTransferCompletion::complete_ready() noexcept {
  try {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!producer_.has_value()) {
      return false;
    }
    const bool published = producer_->complete_ready();
    producer_.reset();
    return published;
  } catch (...) {
    std::terminate();
  }
}

/** @copydoc DeviceTransferCompletion::complete_failed */
bool DeviceTransferCompletion::complete_failed(ReadyFenceFailure failure) {
  std::lock_guard<std::mutex> lock(mutex_);
  if (!producer_.has_value()) {
    return false;
  }
  const bool published = producer_->complete_failed(std::move(failure));
  producer_.reset();
  return published;
}

/** @copydoc DeviceTransferCompletion::cancel */
bool DeviceTransferCompletion::cancel() noexcept {
  try {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!producer_.has_value()) {
      return false;
    }
    const bool published = producer_->cancel();
    producer_.reset();
    return published;
  } catch (...) {
    std::terminate();
  }
}

/** @copydoc ValueTransferTask::ValueTransferTask */
ValueTransferTask::ValueTransferTask(std::shared_ptr<Impl> impl) noexcept
    : impl_(std::move(impl)) {}

/** @copydoc ValueTransferTask::~ValueTransferTask */
ValueTransferTask::~ValueTransferTask() noexcept = default;

/** @copydoc ValueTransferTask::prepare_cpu_copy */
ValueTransferTask ValueTransferTask::prepare_cpu_copy(Value source) {
  if (!source.valid()) {
    throw std::invalid_argument(
        "ValueTransferTask requires a valid source Value.");
  }
  const StorageBinding source_binding = source.storage_binding();
  if (source_binding.device.backend() != DeviceBackend::CPU ||
      !source_binding.host_visible) {
    throw std::invalid_argument(
        "CPU Value copy requires a host-visible CPU source binding.");
  }
  AccessPlan plan = source.plan_access(AccessTarget{
      DeviceId(DeviceBackend::CPU), MemoryDomain::Host, true, true});
  if (plan.kind() != AccessPlanKind::Transfer) {
    throw std::logic_error(
        "Distinct CPU copy did not produce a Transfer access plan.");
  }
  PendingValuePublication publication =
      PendingValuePublisher::allocate_cpu_dense_tensor(
          source.dense_tensor_descriptor(), source.image_facet(),
          source.strided_layout(), source.storage_size(), source.revision_id());
  return ValueTransferTask(std::make_shared<Impl>(
      std::move(source), std::move(publication), std::move(plan)));
}

/** @copydoc ValueTransferTask::prepare_external_transfer */
ValueTransferTask ValueTransferTask::prepare_external_transfer(
    Value source, AccessTarget target, std::shared_ptr<void> owner,
    void* native_handle, std::byte* host_pointer,
    ExternalTransferOperation operation) {
  if (!source.valid()) {
    throw std::invalid_argument(
        "External Value transfer requires a valid source.");
  }
  if (!operation) {
    throw std::invalid_argument(
        "External Value transfer requires a physical provider.");
  }
  if (target.host_read && host_pointer == nullptr) {
    throw std::invalid_argument(
        "Host-readable transfer target requires a host pointer.");
  }
  validate_dense_tensor_producer_envelope(source.dense_tensor_descriptor(),
                                          source.strided_layout(),
                                          source.storage_size());
  AccessPlan plan = source.plan_access(target);
  if (plan.kind() != AccessPlanKind::Transfer) {
    throw std::invalid_argument(
        "External Value transfer requires a Transfer access plan.");
  }
  PendingDeviceValuePublication publication =
      PendingDeviceValuePublisher::publish_dense_tensor(
          source.dense_tensor_descriptor(), source.image_facet(),
          source.strided_layout(), std::move(owner), native_handle,
          host_pointer, source.storage_size(), target.device,
          target.memory_domain, source.revision_id());
  return ValueTransferTask(
      std::make_shared<Impl>(std::move(source), std::move(publication),
                             std::move(plan), std::move(operation)));
}

/** @copydoc ValueTransferTask::destination */
Value ValueTransferTask::destination() const {
  if (!impl_) {
    throw std::logic_error("Moved-from ValueTransferTask has no destination.");
  }
  return impl_->destination;
}

/** @copydoc ValueTransferTask::access_plan */
const AccessPlan& ValueTransferTask::access_plan() const {
  if (!impl_) {
    throw std::logic_error("Moved-from ValueTransferTask has no AccessPlan.");
  }
  return impl_->plan;
}

/** @copydoc ValueTransferTask::enqueue */
void ValueTransferTask::enqueue(std::shared_ptr<ReadyFenceExecutor> executor) {
  if (!impl_) {
    throw std::logic_error("Moved-from ValueTransferTask cannot enqueue.");
  }
  if (impl_->enqueued) {
    throw std::logic_error("ValueTransferTask is already enqueued.");
  }
  if (!executor) {
    throw std::invalid_argument(
        "ValueTransferTask requires a ReadyFenceExecutor.");
  }

  const std::weak_ptr<Impl> weak_impl = impl_;
  ReadyFenceWaitRegistration wait = impl_->source.ready_fence().async_wait(
      std::move(executor),
      [weak_impl](ReadyFenceSnapshot source_snapshot) noexcept {
        if (const std::shared_ptr<Impl> retained = weak_impl.lock()) {
          settle_from_source(retained, std::move(source_snapshot));
        }
      });
  impl_->wait = std::move(wait);
  impl_->enqueued = true;
}

/** @copydoc ValueTransferTask::enqueued */
bool ValueTransferTask::enqueued() const {
  if (!impl_) {
    throw std::logic_error(
        "Moved-from ValueTransferTask has no enqueue state.");
  }
  return impl_->enqueued;
}

/** @copydoc ValueTransferTask::settle_from_source */
void ValueTransferTask::settle_from_source(
    const std::shared_ptr<Impl>& impl,
    ReadyFenceSnapshot source_snapshot) noexcept {
  try {
    switch (source_snapshot.state()) {
      case ReadyFenceState::Ready: {
        if (impl->cpu_producer.has_value()) {
          const ReadLease source_read =
              impl->source.buffer_handle().acquire_read();
          if (source_read.size() != impl->cpu_producer->size()) {
            throw std::logic_error(
                "Value transfer source and destination envelopes differ.");
          }
          std::memcpy(impl->cpu_producer->data(), source_read.data(),
                      source_read.size());
          (void)impl->cpu_producer->complete_ready();
          impl->cpu_producer.reset();
          return;
        }
        if (!impl->device_completion || !impl->external_operation) {
          throw std::logic_error(
              "External Value transfer lost its provider state.");
        }
        std::shared_ptr<DeviceTransferCompletion> completion =
            impl->device_completion;
        impl->external_operation(impl->source, completion);
        impl->device_completion.reset();
        impl->external_operation = {};
        return;
      }
      case ReadyFenceState::Failed: {
        const ReadyFenceFailure* failure = source_snapshot.failure();
        if (failure == nullptr) {
          throw std::logic_error(
              "Failed source fence omitted its typed failure.");
        }
        if (impl->cpu_producer.has_value()) {
          (void)impl->cpu_producer->complete_failed(*failure);
          impl->cpu_producer.reset();
        } else if (impl->device_completion) {
          (void)impl->device_completion->complete_failed(*failure);
          impl->device_completion.reset();
        }
        return;
      }
      case ReadyFenceState::ProducerCancelled:
        if (impl->cpu_producer.has_value()) {
          (void)impl->cpu_producer->cancel();
          impl->cpu_producer.reset();
        } else if (impl->device_completion) {
          (void)impl->device_completion->cancel();
          impl->device_completion.reset();
        }
        return;
      case ReadyFenceState::Pending:
        throw std::logic_error(
            "Value transfer callback received a Pending source fence.");
    }
  } catch (const std::exception& error) {
    try {
      const ReadyFenceFailure failure(ReadyFenceFailureDomain::Transfer, 1,
                                      error.what());
      if (impl->cpu_producer.has_value()) {
        (void)impl->cpu_producer->complete_failed(failure);
        impl->cpu_producer.reset();
      } else if (impl->device_completion) {
        (void)impl->device_completion->complete_failed(failure);
        impl->device_completion.reset();
      }
    } catch (...) {
      if (impl->cpu_producer.has_value()) {
        (void)impl->cpu_producer->cancel();
        impl->cpu_producer.reset();
      } else if (impl->device_completion) {
        (void)impl->device_completion->cancel();
        impl->device_completion.reset();
      }
    }
  } catch (...) {
    try {
      const ReadyFenceFailure failure(
          ReadyFenceFailureDomain::Transfer, 2,
          "Value transfer failed with an unknown error.");
      if (impl->cpu_producer.has_value()) {
        (void)impl->cpu_producer->complete_failed(failure);
        impl->cpu_producer.reset();
      } else if (impl->device_completion) {
        (void)impl->device_completion->complete_failed(failure);
        impl->device_completion.reset();
      }
    } catch (...) {
      if (impl->cpu_producer.has_value()) {
        (void)impl->cpu_producer->cancel();
        impl->cpu_producer.reset();
      } else if (impl->device_completion) {
        (void)impl->device_completion->cancel();
        impl->device_completion.reset();
      }
    }
  }
}

}  // namespace ps
