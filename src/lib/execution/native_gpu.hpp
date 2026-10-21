#pragma once

#include <cstdint>
#include <functional>
#include <memory>
#include <string>

#include "data/device_storage.hpp"
#include "execution/native_gpu_metadata.hpp"
#include "photospider/compiler/physical_program.hpp"
#include "photospider/data/storage.hpp"
#include "photospider/plugin/native_kernel_api.h"

namespace ps::gpu_internal {
/** @brief A retained bounded binding, retained through native completion. */
struct BufferView final {
  std::shared_ptr<const CpuStorage> storage;
  std::uint64_t offset = 0;
  std::uint64_t size = 0;
  bool writable = false;
  std::shared_ptr<const DeviceStorage> device_storage;
};
/** @brief Returns native payload capacity, or zero on overflow. */
std::uint64_t allocation_capacity(std::uint64_t bytes) noexcept;

/** @brief Optional process-local GPU device with no exported native handles.
 * @note Prepared kernels submit asynchronously and report terminal device
 * completion through the invocation completion slot. Allocations and prepared
 * kernels retain their native owners independently. Creation returns null if
 * unavailable.
 */
class Device final : public std::enable_shared_from_this<Device> {
 public:
  static std::shared_ptr<Device> create(
      std::shared_ptr<ResourceBudget> budget = {});
  std::shared_ptr<MetadataAccount> metadata() const;
  void clear_pipeline_cache();
  void collect_expired_allocations();
  ~Device();
  bool available() const noexcept;
  std::uint32_t backend() const noexcept;
  std::uint64_t minimum_buffer_offset_alignment() const noexcept;
  /** @brief Device-resolved allocation capacity for this exact logical size. */
  Result<std::uint64_t> allocation_capacity(std::uint64_t bytes);
  /** @brief Resolve Device-only capacity without allocating device memory. */
  Result<std::uint64_t> private_allocation_capacity(std::uint64_t bytes);
  using AllocationCommit =
      std::function<void(const std::shared_ptr<void>&, std::uint64_t)>;
  /** @brief Reserve capacity before allocating unmapped Device storage.
   * reserve owns payload accounting and returns the retained capacity lease.
   * An empty reserve means unmanaged payload. Owner/native metadata uses this
   * device's explicit metadata root independently of the calling thread.
   * Bytes are uninitialized; a producer must initialize all declared outputs.
   * commit observes actual native allocation before bind/registration can
   * fail; the reservation must retain that observation until resource release.
   */
  Result<std::shared_ptr<DeviceStorage>> allocate_private(
      std::uint64_t bytes, const BufferAllocator::Reserve& reserve,
      const AllocationCommit& commit = {});
  bool owns(const DeviceStorage& storage) const noexcept;
  std::string identity() const;
  std::string description() const;
  BufferAllocator allocator(const BufferAllocator& host);
  bool owns(const CpuStorage& storage) const noexcept;
  Result<BufferView> view(const std::uint8_t* bytes, std::uint64_t size,
                          bool writable);
  /** @brief Resolve an exact Device handle without dereferencing caller data.
   * Rejects foreign, expired, oversized or sealed writable bindings.
   */
  Result<BufferView> private_view(const void* handle, std::uint64_t size,
                                  bool writable);
  /** @brief Borrowed preparation adapter. Returned kernels retain this device.
   */
  ps_native_gpu_service_v2 preparation_service() noexcept;
  Result<ps_prepared_kernel_v8> prepare_kernel(
      const ps_native_program_v2& program,
      const ps_kernel_preparation_v8& query);
  /** @brief Prepare a byte-exact COPY of a validated affine source layout.
   * Host endpoints require the Plan's explicit Shared scratch reservation.
   * Dense output bytes become visible before terminal completion.
   */
  Result<ps_prepared_kernel_v8> prepare_copy(const physical::Layout& source);

 private:
  struct Impl;
  struct FixedKernel;
  struct CopyKernel;
  static ps_kernel_submission_v8 submit_copy(
      const ps_kernel_invocation_v8* invocation,
      const ps_kernel_completion_v8* completion) noexcept;
  static ps_kernel_submission_v8 submit_fixed(
      const ps_kernel_invocation_v8* invocation,
      const ps_kernel_completion_v8* completion) noexcept;
  explicit Device(std::unique_ptr<Impl> impl);
  Result<MutableBuffer> allocate(
      std::uint64_t size, const BufferAllocator::Reserve& reserve,
      std::shared_ptr<const void> domain,
      const BufferAllocator::AllocationCommit& commit);
  std::unique_ptr<Impl> impl_;
};

}  // namespace ps::gpu_internal
