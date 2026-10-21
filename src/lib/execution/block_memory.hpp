#pragma once

#include <array>
#include <cstdint>
#include <memory>
#include <vector>

#include "photospider/compiler/physical_program.hpp"
#include "photospider/core/resources.hpp"
#include "photospider/data/storage.hpp"

namespace ps::gpu_internal {
class DeviceStorage;
}
namespace ps::execution_internal {
struct MemoryObservation;
/** @brief Device-resolved native capacities for one immutable physical program.
 * Preparation queries capacity without allocating payload. The device and the
 * exact program must remain alive; the program cannot change after preparation.
 */
struct NativeAllocations final {
  NativeAllocations() = default;
  NativeAllocations(NativeAllocations&&) noexcept = default;
  NativeAllocations& operator=(NativeAllocations&& other) noexcept;
  NativeAllocations(const NativeAllocations&) = delete;
  NativeAllocations& operator=(const NativeAllocations&) = delete;
  std::shared_ptr<gpu_internal::Device> device;
  ResourceLease metadata;
  const physical::Program* program = nullptr;
  std::vector<std::uint64_t> tiles;
  using Scratch = std::array<std::array<std::uint64_t, 3>, 2>;
  std::vector<Scratch> scratch;
};
Result<NativeAllocations> prepare_native_allocations(
    const physical::Program& program,
    std::shared_ptr<gpu_internal::Device> device);

/** @brief Exclusive allocations admitted before one block can issue work.
 * Inputs/arguments borrow their existing owners and do not allocate here.
 * Each output tile has separate aligned storage. Scratch is one reusable
 * allocation per backend lane, sized to that backend's block maximum. A lane
 * cannot reuse its scratch until the previous invocation has completed. All
 * methods are coordinator-only. Workers borrow pointers through completion.
 */
class BlockMemory final {
 public:
  BlockMemory() = default;
  BlockMemory(BlockMemory&&) noexcept = default;
  BlockMemory& operator=(BlockMemory&& other) noexcept;
  BlockMemory(const BlockMemory&) = delete;
  BlockMemory& operator=(const BlockMemory&) = delete;
  /** @brief Reserve the entire block before allocating any payload.
   * Requires validated physical tables. Unsupported domains are rejected;
   * this function never changes a placement or inserts a transfer.
   * Failure returns every reservation and publishes no mutable pointers.
   */
  static Result<BlockMemory> allocate(
      const physical::Program& program, physical::Id block, std::uint32_t lanes,
      const ResourceBudget& budget, std::uint32_t gpu_lanes = 0,
      const NativeAllocations* native = nullptr,
      ResourceAdmissionFailure* failure = nullptr,
      std::uint64_t* allocation_count = nullptr,
      std::shared_ptr<MemoryObservation> observation = {});
  MutableBuffer& tile(physical::Id id) { return tiles_.at(id); }
  gpu_internal::DeviceStorage* device_tile(physical::Id id) {
    return device_tiles_.at(id).get();
  }
  gpu_internal::DeviceStorage* device_scratch(std::uint32_t lane) {
    return device_scratch_.at(lane).get();
  }
  MutableBuffer& scratch(std::uint32_t lane,
                         physical::Domain domain = physical::Domain::Host) {
    return scratch_.at(lane).at(static_cast<unsigned>(domain));
  }
  /** @brief Transfer the completed producer's owner without copying bytes.
   * Call only after its invocation has stopped accessing the allocation.
   * The block protects its full reservation until the boundary. Thereafter
   * this owner retains its allocation charges and may outlive the block, Run
   * and ExecutionContext.
   */
  std::shared_ptr<const CpuStorage> publish(physical::Id id);
  std::shared_ptr<const gpu_internal::DeviceStorage> publish_device(
      physical::Id id);
  /** @brief Internal owner for preallocating an unpublished Result envelope.
   * Admission may build structural descriptors around this owner, but must
   * keep every envelope private until producer success and retirement/RET.
   * No samples may be read through this alias before producer completion.
   */
  std::shared_ptr<const CpuStorage> unpublished(physical::Id id) const;
  std::shared_ptr<const gpu_internal::DeviceStorage> unpublished_device(
      physical::Id id) const;
  /** @brief Abandon an unsubmitted/skipped or quiescent failed output. */
  void discard(physical::Id id);

 private:
  // Reservation closes after vectors, scratch and local output owners retire.
  // Exported storage slices retain their own post-block charges.
  std::shared_ptr<void> reservation_, metadata_;
  std::vector<MutableBuffer> tiles_;
  std::vector<std::array<MutableBuffer, 3>> scratch_;
  std::vector<std::shared_ptr<gpu_internal::DeviceStorage>> device_tiles_;
  std::vector<std::shared_ptr<gpu_internal::DeviceStorage>> device_scratch_;
};
}  // namespace ps::execution_internal
