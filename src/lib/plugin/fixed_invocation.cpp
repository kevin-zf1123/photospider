#include "photospider/plugin/fixed_invocation.hpp"

#include <algorithm>
#include <cstring>
#include <exception>
#include <memory>
#include <new>
#include <string>
#include <utility>

#include "core/callback_environment.hpp"
#include "core/resource_observation.hpp"

namespace ps {
ps_kernel_status_v8 kernel_failure(ErrorCode code, const char* message,
                                   FailureReason reason) noexcept {
  ps_kernel_status_v8 result{};
  result.code = static_cast<std::uint32_t>(code);
  result.reason = static_cast<std::uint32_t>(reason);
  if (message) {
    std::size_t i = 0;
    for (; i + 1 < PS_KERNEL_MESSAGE_CAPACITY_V8 && message[i]; ++i)
      result.message[i] = message[i];
    result.message[i] = 0;
  }
  return result;
}
void assign_kernel_status(Status* target, const ps_kernel_status_v8& status) {
  target->detail = {};
  if (status.code > PS_KERNEL_INTERNAL_V8 ||
      status.reason > static_cast<unsigned>(FailureReason::ViewUnavailable) ||
      !std::memchr(status.message, 0, sizeof(status.message))) {
    target->code = ErrorCode::Internal;
    target->reason = FailureReason::MalformedEnvelope;
    target->message.assign("malformed kernel completion status");
    return;
  }
  target->code = static_cast<ErrorCode>(status.code);
  target->reason = static_cast<FailureReason>(status.reason);
  target->message.assign(status.message);
  target->detail.domain_code = status.domain_code;
}
Status kernel_status(const ps_kernel_status_v8& status) {
  Status result;
  assign_kernel_status(&result, status);
  return result;
}
ps_kernel_layout_v8 kernel_layout(const physical::Layout& layout) noexcept {
  ps_kernel_layout_v8 result{};
  result.rank = layout.rank;
  result.element_type = static_cast<std::uint32_t>(layout.element_type);
  std::copy(layout.shape.begin(), layout.shape.end(), result.shape);
  std::copy(layout.origin.begin(), layout.origin.end(), result.origin);
  std::copy(layout.strides.begin(), layout.strides.end(), result.strides);
  return result;
}

struct PreparedKernel::State final {
  ResourceLease metadata;
  // Reverse destruction order keeps code/device/resource owners until after
  // the operation-defined release function has finished.
  std::shared_ptr<const void> owners;
  ps_prepared_kernel_v8 kernel{};
  bool adopted = false;
  ~State() noexcept {
    if (adopted && kernel.release) {
      core_internal::CallbackEnvironment environment;
      kernel.release(kernel.context);
    }
  }
};

Result<PreparedKernel> PreparedKernel::adopt(ps_prepared_kernel_v8 kernel,
                                             std::shared_ptr<const void> owners,
                                             const ResourceBudget& budget) {
  if (kernel.struct_size != sizeof(kernel) ||
      kernel.abi_version != PS_FIXED_INVOCATION_ABI_VERSION_8 ||
      (!kernel.invoke_cpu && !kernel.submit_gpu) ||
      (kernel.context && !kernel.release))
    return Result<PreparedKernel>(
        Status{ErrorCode::InvalidArgument, "invalid prepared kernel ABI"});
  try {
    auto metadata = budget.reserve(
        ResourceCapacity::host(sizeof(State) + 64, sizeof(State) + 64));
    if (!metadata.ok())
      return Result<PreparedKernel>(metadata.status());
    auto state = std::make_shared<State>();
    state->metadata = metadata.take_value();
    core_internal::ResourceMetadataAccess::commit(state->metadata,
                                                  sizeof(State));
    state->owners = std::move(owners);
    state->kernel = kernel;
    state->adopted = true;
    return Result<PreparedKernel>(PreparedKernel(std::move(state)));
  } catch (const std::bad_alloc&) {
    return Result<PreparedKernel>(Status{ErrorCode::ResourceExhausted, {}});
  }
}
bool PreparedKernel::supports(physical::Backend backend) const noexcept {
  return state_ &&
         ((backend == physical::Backend::Cpu && state_->kernel.invoke_cpu) ||
          (backend == physical::Backend::Gpu && state_->kernel.submit_gpu));
}
const void* PreparedKernel::context() const noexcept {
  return state_ ? state_->kernel.context : nullptr;
}
void PreparedKernel::bind(ps_kernel_invocation_v8* invocation) const noexcept {
  invocation->struct_size = sizeof(*invocation);
  invocation->abi_version = PS_FIXED_INVOCATION_ABI_VERSION_8;
  invocation->prepared_context = context();
}
ps_kernel_status_v8 PreparedKernel::invoke(
    const ps_kernel_invocation_v8& invocation) const noexcept {
  if (!supports(physical::Backend::Cpu))
    return kernel_failure(ErrorCode::BackendUnavailable,
                          "CPU kernel unavailable");
  core_internal::CallbackEnvironment environment;
  if (!environment.active())
    return kernel_failure(ErrorCode::OperationFailed,
                          "cannot save floating environment");
  auto status = kernel_failure(ErrorCode::OperationFailed,
                               "kernel did not report status");
  try {
    state_->kernel.invoke_cpu(&invocation, &status);
  } catch (const std::bad_alloc&) {
    return kernel_failure(ErrorCode::ResourceExhausted,
                          "kernel allocation failed");
  } catch (const std::exception& error) {
    return kernel_failure(ErrorCode::OperationFailed, error.what(),
                          FailureReason::HostException);
  } catch (...) {
    return kernel_failure(ErrorCode::OperationFailed,
                          "kernel threw an exception",
                          FailureReason::HostException);
  }
  return status;
}
ps_kernel_submission_v8 PreparedKernel::submit(
    const ps_kernel_invocation_v8& invocation,
    const ps_kernel_completion_v8& completion) const noexcept {
  if (!supports(physical::Backend::Gpu)) {
    ps_kernel_submission_v8 rejected{};
    rejected.status =
        kernel_failure(ErrorCode::BackendUnavailable, "GPU kernel unavailable");
    return rejected;
  }
  core_internal::CallbackEnvironment environment;
  if (!environment.active()) {
    ps_kernel_submission_v8 rejected{};
    rejected.status = kernel_failure(ErrorCode::OperationFailed,
                                     "cannot save floating environment");
    return rejected;
  }
  return state_->kernel.submit_gpu(&invocation, &completion);
}
}  // namespace ps
