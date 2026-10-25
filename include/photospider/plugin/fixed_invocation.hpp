#pragma once

#include <memory>
#include <utility>

#include "photospider/compiler/physical_program.hpp"
#include "photospider/plugin/fixed_invocation_api.h"
#include "photospider/plugin/fixed_invocation_v9_api.h"

struct ps_native_gpu_service_v2;

namespace ps {
/** @brief Convert a bounded kernel status to the owning host diagnostic.
 * Invalid codes or unterminated messages become Internal. May allocate the
 * host string; use only on the coordinator, never inside completion callbacks.
 */
PHOTOSPIDER_API Status kernel_status(const ps_kernel_status_v8& status);
/** @brief Assign into reusable host status storage on the coordinator.
 * Reserving PS_KERNEL_MESSAGE_CAPACITY_V8 bytes in target.message before issue
 * makes conversion allocation-free. The target must be nonnull.
 */
PHOTOSPIDER_API void assign_kernel_status(Status* target,
                                          const ps_kernel_status_v8& status);
/** @brief Allocation-free status construction for kernels and completion slots.
 * The bounded message is copied and always terminated. Null is an empty text.
 */
PHOTOSPIDER_API ps_kernel_status_v8
kernel_failure(ErrorCode code, const char* message = nullptr,
               FailureReason reason = FailureReason::None) noexcept;
/** @brief Resolve the fixed C layout from a checked physical layout. */
PHOTOSPIDER_API ps_kernel_layout_v8
kernel_layout(const physical::Layout& layout) noexcept;

/** @brief Immutable direct entry points with module/device/resource ownership.
 * Each invocation borrows this object through completion. Copies share one
 * context owner. Destruction releases the context before code/device leases.
 * Numerical loops run without registry lookups or publication validation.
 */
class PHOTOSPIDER_API PreparedKernel final {
 public:
  PreparedKernel() = default;
  /** @brief Adopt exactly one prepared context on success.
   * InvalidArgument rejects malformed ABI records without taking ownership.
   * Allocation failure returns ResourceExhausted and also leaves ownership
   * with the caller. owners retains the module, device and immutable resources.
   * budget admits host wrapper metadata until the last copy is destroyed;
   * provider-owned opaque context allocations are outside this accounting.
   * Omitting budget constructs an independent default root and may throw
   * std::bad_alloc before the call; retained leases keep that root alive.
   */
  static Result<PreparedKernel> adopt(
      ps_prepared_kernel_v8 kernel, std::shared_ptr<const void> owners,
      const ResourceBudget& budget = ResourceBudget{});
  /** @brief Adopt an explicit CPU v9 preparation with exact-size validation.
   * Ownership, allocation failures and thread rules match the v8 overload.
   */
  static Result<PreparedKernel> adopt(
      ps_prepared_kernel_v9 kernel, std::shared_ptr<const void> owners,
      const ResourceBudget& budget = ResourceBudget{});
  /** @brief True only for a preparation explicitly negotiated as v9. */
  bool uses_work_v9() const noexcept;
  /** @brief Bind/invoke a host-owned complete v9 envelope. The service and
   * every borrowed owner must remain alive until synchronous CPU return.
   * Invalid envelope size/version is rejected before provider entry.
   */
  void bind(ps_kernel_invocation_v9* invocation) const noexcept;
  ps_kernel_status_v8 invoke(
      const ps_kernel_invocation_v9& invocation) const noexcept;
  bool valid() const noexcept { return static_cast<bool>(state_); }
  bool supports(physical::Backend backend) const noexcept;
  const void* context() const noexcept;
  /** @brief Bind the prepared context to a host-owned invocation record.
   * The record and its view/parameter arrays remain alive through completion.
   * Calling requires a valid preparation and exclusive access to this record.
   */
  void bind(ps_kernel_invocation_v8* invocation) const noexcept;
  /** @brief Direct CPU invocation with exception/floating-environment fencing.
   * All accesses have ended on return. The invocation must already be bound
   * and validated; per-view structural checks belong to prepare/preflight.
   */
  ps_kernel_status_v8 invoke(
      const ps_kernel_invocation_v8& invocation) const noexcept;
  /** @brief Submit through the bound GPU entry point. Invocation and slot must
   * already be armed and retained by the Run. GPU entry points must not throw:
   * an exception cannot retract a possibly accepted native submission.
   * This method deliberately preserves that trusted noexcept obligation.
   */
  ps_kernel_submission_v8 submit(
      const ps_kernel_invocation_v8& invocation,
      const ps_kernel_completion_v8& completion) const noexcept;

 private:
  struct State;
  explicit PreparedKernel(std::shared_ptr<const State> state)
      : state_(std::move(state)) {}
  std::shared_ptr<const State> state_;
};
}  // namespace ps
