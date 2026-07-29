#include "execution/value_transfer_task.hpp"

#include <cstring>
#include <memory>
#include <stdexcept>
#include <string>
#include <utility>

#include "core/pending_value.hpp"

namespace ps {

/**
 * @brief Complete prepared state for one explicit CPU Value-copy transfer.
 *
 * @note Field order cancels the wait registration before destroying the
 *       unresolved destination producer.
 */
struct ValueTransferTask::Impl final {
  /** @brief Immutable source retained through transfer settlement. */
  Value source;

  /** @brief Public immutable destination publication. */
  Value destination;

  /** @brief Only mutable capability for destination bytes and fence. */
  PendingValueProducer producer;

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
   * @throws Nothing under member move contracts.
   */
  Impl(Value source_in, PendingValuePublication publication) noexcept
      : source(std::move(source_in)),
        destination(std::move(publication.value)),
        producer(std::move(publication.producer)) {}
};

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
  PendingValuePublication publication =
      PendingValuePublisher::allocate_cpu_dense_tensor(
          source.dense_tensor_descriptor(), source.image_facet(),
          source.strided_layout(), source.storage_size());
  return ValueTransferTask(
      std::make_shared<Impl>(std::move(source), std::move(publication)));
}

/** @copydoc ValueTransferTask::destination */
Value ValueTransferTask::destination() const {
  if (!impl_) {
    throw std::logic_error("Moved-from ValueTransferTask has no destination.");
  }
  return impl_->destination;
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
        const ReadLease source_read =
            impl->source.buffer_handle().acquire_read();
        if (source_read.size() != impl->producer.size()) {
          throw std::logic_error(
              "Value transfer source and destination envelopes differ.");
        }
        std::memcpy(impl->producer.data(), source_read.data(),
                    source_read.size());
        (void)impl->producer.complete_ready();
        return;
      }
      case ReadyFenceState::Failed: {
        const ReadyFenceFailure* failure = source_snapshot.failure();
        if (failure == nullptr) {
          throw std::logic_error(
              "Failed source fence omitted its typed failure.");
        }
        (void)impl->producer.complete_failed(*failure);
        return;
      }
      case ReadyFenceState::ProducerCancelled:
        (void)impl->producer.cancel();
        return;
      case ReadyFenceState::Pending:
        throw std::logic_error(
            "Value transfer callback received a Pending source fence.");
    }
  } catch (const std::exception& error) {
    try {
      (void)impl->producer.complete_failed(ReadyFenceFailure(
          ReadyFenceFailureDomain::Transfer, 1, error.what()));
    } catch (...) {
      (void)impl->producer.cancel();
    }
  } catch (...) {
    try {
      (void)impl->producer.complete_failed(
          ReadyFenceFailure(ReadyFenceFailureDomain::Transfer, 2,
                            "Value transfer failed with an unknown error."));
    } catch (...) {
      (void)impl->producer.cancel();
    }
  }
}

}  // namespace ps
