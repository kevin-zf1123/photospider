#pragma once

#include <cstdint>

#include "execution/native_gpu.hpp"
#include "photospider/compiler/physical_program.hpp"

namespace ps::gpu_internal {
/** @brief Fixed byte-copy geometry, also the Metal constant-buffer layout. */
struct CopyGeometry final {
  std::uint64_t source_origin = 0, destination_origin = 0, bytes = 0, width = 0;
  std::uint64_t shape[8]{};
  std::int64_t strides[8]{};
  std::uint32_t rank = 0, threads = 0;
  explicit CopyGeometry(const physical::Layout& layout) noexcept;
  std::uint64_t contiguous_bytes() const noexcept;
  std::uint64_t source_offset(std::uint64_t sample) const noexcept;
};

/** @brief Native views and a bounded Host epilogue borrowed through finish.
 * Native completion performs the epilogue before reporting task completion.
 * It allocates nothing and invokes no user code. Failed native work never
 * copies staging bytes to the Host output.
 */
struct CopyBinding final {
  CopyGeometry geometry;
  BufferView source, destination;
  const void* download_source = nullptr;
  void* download_destination = nullptr;
  explicit CopyBinding(CopyGeometry value) : geometry(value) {}
};

/** @brief Resolve native owners and gather a Host input into declared Shared
 * scratch. The invocation is already structurally validated by the Run.
 * Rejection has submitted no native work and transfers no borrowing.
 */
Result<CopyBinding> bind_copy(Device& device, CopyGeometry geometry,
                              const ps_kernel_invocation_v8& invocation);
}  // namespace ps::gpu_internal
