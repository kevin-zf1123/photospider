#include "execution/result_fallback.hpp"

#include "plugin/failure_latch.hpp"

namespace ps::execution_internal {
bool ResultFallbackState::can_retry_start(
    const OperationTraits& traits, Backend backend,
    const StructuredStartPhase& started,
    const std::atomic<ErrorCode>& operation_failure,
    const plugin_internal::FailureLatch& host_latch) {
  return started.dispatched.ok() && !started.result.ok() &&
         plugin_internal::FailureLatch::retryable_backend_failure(
             started.result.status()) &&
         backend == Backend::Gpu && traits.supports_cpu &&
         traits.allows_cpu_fallback && traits.deterministic &&
         traits.side_effect_free && started.sticky == ErrorCode::Ok &&
         operation_failure.load() == ErrorCode::Ok &&
         host_latch.snapshot().ok();
}
bool ResultFallbackState::can_retry_poll(
    const OperationTraits& traits, Backend backend, const Status& failed,
    const Status& host_failure, const plugin_internal::FailureLatch& host_latch,
    std::uint64_t native_dispatches, bool published, ErrorCode sticky,
    ErrorCode operation_failure) const {
  const bool retryable_host_failure =
      host_failure.ok() ||
      plugin_internal::FailureLatch::retryable_backend_failure(host_failure);
  return plugin_internal::FailureLatch::retryable_backend_failure(failed) &&
         backend == Backend::Gpu && traits.supports_cpu &&
         traits.allows_cpu_fallback && traits.deterministic &&
         traits.side_effect_free && retry_safe && !native_dispatches &&
         !published && sticky == ErrorCode::Ok &&
         (operation_failure == ErrorCode::Ok ||
          operation_failure == ErrorCode::BackendUnavailable) &&
         retryable_host_failure && host_latch.backend_retry_allowed();
}
bool ResultFallbackState::optional_joint_failure(const Status& failure) {
  return failure.code != ErrorCode::Cancelled &&
         failure.code != ErrorCode::Stale &&
         failure.detail.origin != FailureOrigin::Protocol &&
         failure.detail.origin != FailureOrigin::Domain &&
         failure.detail.origin != FailureOrigin::Schema &&
         failure.detail.scope == FailureScope::Unspecified;
}
}  // namespace ps::execution_internal
