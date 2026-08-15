/**
 * @file worker_manager.cpp
 * @brief Exposes the public WorkerManager pimpl boundary and test seams.
 */
#include "server/worker/worker_manager.hpp"

#include <limits>
#include <utility>

#include "server/worker/worker_manager_internal.hpp"

namespace ps::server {

using namespace worker_manager_detail;  // NOLINT(build/namespaces)

/** @copydoc ps::server::WorkerManagerTestAccess::reset_descriptor_for_test */
void WorkerManagerTestAccess::reset_descriptor_for_test(
    int* descriptor, int replacement, DescriptorCloseCall close_call,
    void* context) {
  if (descriptor == nullptr || close_call == nullptr) {
    throw std::invalid_argument("worker descriptor reset seam is incomplete");
  }
  reset_manager_descriptor(descriptor, replacement, close_call, context);
}

/** @copydoc ps::server::ScopedWorkerThreadStartFailure */
ScopedWorkerThreadStartFailure::ScopedWorkerThreadStartFailure() {
  arm_manager_thread_start_failure(&attempted_job_id_,
                                   &manager_record_inserted_before_failure_);
}

/** @copydoc
 * ps::server::ScopedWorkerThreadStartFailure::~ScopedWorkerThreadStartFailure
 */
ScopedWorkerThreadStartFailure::~ScopedWorkerThreadStartFailure() noexcept {
  disarm_manager_thread_start_failure(&attempted_job_id_,
                                      &manager_record_inserted_before_failure_);
}
/** @copydoc ps::server::WorkerManagerOwnershipSnapshot::total */
std::size_t WorkerManagerOwnershipSnapshot::total() const {
  if (active > std::numeric_limits<std::size_t>::max() - completed ||
      active + completed > std::numeric_limits<std::size_t>::max() - joining) {
    throw std::overflow_error("worker manager ownership snapshot overflowed");
  }
  return active + completed + joining;
}

/** @copydoc ps::server::WorkerManager::WorkerManager */
WorkerManager::WorkerManager(std::shared_ptr<JobAttemptWorkerFactory> factory,
                             WorkerManagerCallbacks callbacks,
                             WorkerManagerOptions options,
                             bool in_process_test_mode)
    : impl_(std::make_unique<Impl>(
          std::move(factory), std::move(callbacks),
          std::move(options),        // NOLINT(whitespace/indent_namespace)
          in_process_test_mode)) {}  // NOLINT(whitespace/indent_namespace)

/** @copydoc ps::server::WorkerManager::~WorkerManager */
WorkerManager::~WorkerManager() noexcept = default;

/** @copydoc ps::server::WorkerManager::start */
void WorkerManager::start(JobAssignment assignment) {
  impl_->start(std::move(assignment));
}

/** @copydoc ps::server::WorkerManager::request_cancel */
bool WorkerManager::request_cancel(const AttemptIdentity& identity) noexcept {
  return impl_->request_cancel(identity);
}

/** @copydoc ps::server::WorkerManager::owns_attempt */
bool WorkerManager::owns_attempt(
    const AttemptIdentity& identity) const noexcept {
  return impl_->owns_attempt(identity);
}

/** @copydoc ps::server::WorkerManager::shutdown */
void WorkerManager::shutdown() noexcept {
  impl_->shutdown();
}

/** @copydoc ps::server::WorkerManager::ownership_snapshot */
WorkerManagerOwnershipSnapshot WorkerManager::ownership_snapshot()
    const noexcept {
  return impl_->ownership_snapshot();
}

/** @copydoc ps::server::WorkerManager::wait_for_owned_count_at_most */
bool WorkerManager::wait_for_owned_count_at_most(
    std::size_t maximum_count, std::chrono::milliseconds timeout) const {
  return impl_->wait_for_owned_count_at_most(maximum_count, timeout);
}

}  // namespace ps::server
