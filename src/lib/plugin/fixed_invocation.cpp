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
// Direct enum conversions below require identical public C and C++ codes.
static_assert(static_cast<std::uint32_t>(ErrorCode::Ok) == PS_KERNEL_OK_V8);
static_assert(static_cast<std::uint32_t>(ErrorCode::InvalidArgument) ==
              PS_KERNEL_INVALID_ARGUMENT_V8);
static_assert(static_cast<std::uint32_t>(ErrorCode::NotFound) ==
              PS_KERNEL_NOT_FOUND_V8);
static_assert(static_cast<std::uint32_t>(ErrorCode::Cycle) ==
              PS_KERNEL_CYCLE_V8);
static_assert(static_cast<std::uint32_t>(ErrorCode::TypeMismatch) ==
              PS_KERNEL_TYPE_MISMATCH_V8);
static_assert(static_cast<std::uint32_t>(ErrorCode::ResourceExhausted) ==
              PS_KERNEL_RESOURCE_EXHAUSTED_V8);
static_assert(static_cast<std::uint32_t>(ErrorCode::Cancelled) ==
              PS_KERNEL_CANCELLED_V8);
static_assert(static_cast<std::uint32_t>(ErrorCode::Stale) ==
              PS_KERNEL_STALE_V8);
static_assert(static_cast<std::uint32_t>(ErrorCode::BackendUnavailable) ==
              PS_KERNEL_BACKEND_UNAVAILABLE_V8);
static_assert(static_cast<std::uint32_t>(ErrorCode::OperationFailed) ==
              PS_KERNEL_OPERATION_FAILED_V8);
static_assert(static_cast<std::uint32_t>(ErrorCode::Internal) ==
              PS_KERNEL_INTERNAL_V8);
static_assert(static_cast<std::uint32_t>(FailureReason::None) ==
              PS_KERNEL_REASON_NONE_V8);
static_assert(static_cast<std::uint32_t>(FailureReason::ArithmeticOverflow) ==
              PS_KERNEL_REASON_ARITHMETIC_OVERFLOW_V8);
static_assert(static_cast<std::uint32_t>(FailureReason::InvalidDomain) ==
              PS_KERNEL_REASON_INVALID_DOMAIN_V8);
static_assert(static_cast<std::uint32_t>(FailureReason::MalformedEnvelope) ==
              PS_KERNEL_REASON_MALFORMED_ENVELOPE_V8);
static_assert(static_cast<std::uint32_t>(FailureReason::UnauthorizedRead) ==
              PS_KERNEL_REASON_UNAUTHORIZED_READ_V8);
static_assert(static_cast<std::uint32_t>(FailureReason::StaleHandle) ==
              PS_KERNEL_REASON_STALE_HANDLE_V8);
static_assert(static_cast<std::uint32_t>(FailureReason::HostException) ==
              PS_KERNEL_REASON_HOST_EXCEPTION_V8);
static_assert(static_cast<std::uint32_t>(FailureReason::ShortIo) ==
              PS_KERNEL_REASON_SHORT_IO_V8);
static_assert(static_cast<std::uint32_t>(FailureReason::WorkLimit) ==
              PS_KERNEL_REASON_WORK_LIMIT_V8);
static_assert(static_cast<std::uint32_t>(FailureReason::CapacityLimit) ==
              PS_KERNEL_REASON_CAPACITY_LIMIT_V8);
static_assert(static_cast<std::uint32_t>(FailureReason::StageLimit) ==
              PS_KERNEL_REASON_STAGE_LIMIT_V8);
static_assert(static_cast<std::uint32_t>(FailureReason::Cancelled) ==
              PS_KERNEL_REASON_CANCELLED_V8);
static_assert(static_cast<std::uint32_t>(FailureReason::StaleVersion) ==
              PS_KERNEL_REASON_STALE_VERSION_V8);
static_assert(static_cast<std::uint32_t>(FailureReason::ViewUnavailable) ==
              PS_KERNEL_REASON_VIEW_UNAVAILABLE_V8);
static_assert(static_cast<std::uint32_t>(ElementType::UInt8) ==
              PS_ELEMENT_UINT8_V8);
static_assert(static_cast<std::uint32_t>(ElementType::Int64) ==
              PS_ELEMENT_INT64_V8);
static_assert(static_cast<std::uint32_t>(ElementType::Float64) ==
              PS_ELEMENT_FLOAT64_V8);
static_assert(static_cast<std::uint32_t>(ElementType::Float32) ==
              PS_ELEMENT_FLOAT32_V8);
static_assert(static_cast<std::uint32_t>(ElementType::Int8) ==
              PS_ELEMENT_INT8_V8);
static_assert(static_cast<std::uint32_t>(ElementType::UInt16) ==
              PS_ELEMENT_UINT16_V8);
static_assert(static_cast<std::uint32_t>(ElementType::Int16) ==
              PS_ELEMENT_INT16_V8);
static_assert(static_cast<std::uint32_t>(physical::Domain::Host) ==
              PS_KERNEL_DOMAIN_HOST_V8);
static_assert(static_cast<std::uint32_t>(physical::Domain::Device) ==
              PS_KERNEL_DOMAIN_DEVICE_V8);
static_assert(static_cast<std::uint32_t>(physical::Domain::Shared) ==
              PS_KERNEL_DOMAIN_SHARED_V8);

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
      status.reason > PS_KERNEL_REASON_VIEW_UNAVAILABLE_V8 ||
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
  ps_prepared_kernel_v9 work_kernel{};
  bool adopted = false;
  ~State() noexcept {
    if (adopted && work_kernel.release) {
      core_internal::CallbackEnvironment environment;
      work_kernel.release(work_kernel.context);
    }
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
Result<PreparedKernel> PreparedKernel::adopt(ps_prepared_kernel_v9 kernel,
                                             std::shared_ptr<const void> owners,
                                             const ResourceBudget& budget) {
  if (kernel.struct_size != sizeof(kernel) ||
      kernel.abi_version != PS_FIXED_INVOCATION_ABI_VERSION_9 ||
      !kernel.invoke_cpu || (kernel.context && !kernel.release))
    return Result<PreparedKernel>(
        Status{ErrorCode::InvalidArgument, "invalid prepared kernel v9 ABI"});
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
    state->work_kernel = kernel;
    state->adopted = true;
    return Result<PreparedKernel>(PreparedKernel(std::move(state)));
  } catch (const std::bad_alloc&) {
    return Result<PreparedKernel>(Status{ErrorCode::ResourceExhausted, {}});
  }
}
bool PreparedKernel::uses_work_v9() const noexcept {
  return state_ && state_->work_kernel.invoke_cpu;
}
void PreparedKernel::bind(ps_kernel_invocation_v9* invocation) const noexcept {
  invocation->struct_size = sizeof(*invocation);
  invocation->abi_version = PS_FIXED_INVOCATION_ABI_VERSION_9;
  bind(&invocation->views);
  if (uses_work_v9())
    invocation->views.prepared_context = state_->work_kernel.context;
}
ps_kernel_status_v8 PreparedKernel::invoke(
    const ps_kernel_invocation_v9& invocation) const noexcept {
  if (invocation.struct_size != sizeof(invocation) ||
      invocation.abi_version != PS_FIXED_INVOCATION_ABI_VERSION_9 ||
      invocation.views.struct_size != sizeof(invocation.views) ||
      invocation.views.abi_version != PS_FIXED_INVOCATION_ABI_VERSION_8)
    return kernel_failure(ErrorCode::InvalidArgument,
                          "invalid invocation v9 ABI",
                          FailureReason::MalformedEnvelope);
  if (!uses_work_v9())
    return invoke(invocation.views);
  if (!invocation.work.consume)
    return kernel_failure(ErrorCode::InvalidArgument,
                          "missing Run work service");
  core_internal::CallbackEnvironment environment;
  if (!environment.active())
    return kernel_failure(ErrorCode::OperationFailed,
                          "cannot save floating environment");
  auto status = kernel_failure(ErrorCode::OperationFailed,
                               "kernel did not report status");
  try {
    state_->work_kernel.invoke_cpu(&invocation, &status);
  } catch (const std::bad_alloc&) {
    return kernel_failure(ErrorCode::ResourceExhausted,
                          "kernel allocation failed");
  } catch (const std::exception& error) {
    return kernel_failure(ErrorCode::OperationFailed, error.what(),
                          FailureReason::HostException);
  } catch (...) {
    return kernel_failure(ErrorCode::OperationFailed,
                          "v9 kernel threw an exception",
                          FailureReason::HostException);
  }
  return status;
}
bool PreparedKernel::supports(physical::Backend backend) const noexcept {
  return state_ &&
         ((backend == physical::Backend::Cpu &&
           (state_->kernel.invoke_cpu || state_->work_kernel.invoke_cpu)) ||
          (backend == physical::Backend::Gpu && state_->kernel.submit_gpu));
}
const void* PreparedKernel::context() const noexcept {
  return !state_          ? nullptr
         : uses_work_v9() ? state_->work_kernel.context
                          : state_->kernel.context;
}
void PreparedKernel::bind(ps_kernel_invocation_v8* invocation) const noexcept {
  invocation->struct_size = sizeof(*invocation);
  invocation->abi_version = PS_FIXED_INVOCATION_ABI_VERSION_8;
  invocation->prepared_context = context();
}
ps_kernel_status_v8 PreparedKernel::invoke(
    const ps_kernel_invocation_v8& invocation) const noexcept {
  if (uses_work_v9())
    return kernel_failure(ErrorCode::InvalidArgument,
                          "v9 preparation requires a v9 invocation",
                          FailureReason::MalformedEnvelope);
  if (!state_ || !state_->kernel.invoke_cpu)
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
