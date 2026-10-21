#include "execution/native_copy.hpp"

#include <algorithm>
#include <cstring>
#include <utility>

#include "photospider/data/tensor.hpp"

namespace ps::gpu_internal {
CopyGeometry::CopyGeometry(const physical::Layout& layout) noexcept
    : width(TensorSpan::element_size(layout.element_type)), rank(layout.rank) {
  bytes = width;
  for (unsigned axis = 0; axis < rank; ++axis) {
    shape[axis] = layout.shape[axis];
    strides[axis] = layout.strides[axis];
    bytes *= shape[axis];
  }
  threads =
      static_cast<std::uint32_t>(std::min<std::uint64_t>(bytes, UINT32_MAX));
}
std::uint64_t CopyGeometry::source_offset(std::uint64_t sample) const noexcept {
  auto offset = source_origin;
  for (unsigned axis = rank; axis-- > 0;) {
    const auto coordinate = sample % shape[axis];
    sample /= shape[axis];
    const auto stride = strides[axis];
    if (stride < 0)
      offset -= coordinate * (static_cast<std::uint64_t>(-(stride + 1)) + 1);
    else
      offset += coordinate * static_cast<std::uint64_t>(stride);
  }
  return offset;
}
std::uint64_t CopyGeometry::contiguous_bytes() const noexcept {
  auto contiguous = width;
  for (unsigned axis = rank; axis-- > 0;) {
    if (shape[axis] > 1 &&
        (strides[axis] < 0 ||
         static_cast<std::uint64_t>(strides[axis]) != contiguous))
      break;
    contiguous *= shape[axis];
  }
  return contiguous;
}
Result<CopyBinding> bind_copy(Device& device, CopyGeometry geometry,
                              const ps_kernel_invocation_v8& invocation) {
  using Answer = Result<CopyBinding>;
  const auto cancelled = [&] {
    return invocation.cancellation.is_cancelled &&
           invocation.cancellation.is_cancelled(
               invocation.cancellation.context);
  };
  if (cancelled())
    return Answer({ErrorCode::Cancelled, "native COPY cancelled"});
  CopyBinding result(geometry);
  const auto& input = invocation.inputs[0];
  const auto& output = invocation.outputs[0];
  const auto host_source = input.domain == PS_KERNEL_DOMAIN_HOST_V8;
  const auto host_destination = output.domain == PS_KERNEL_DOMAIN_HOST_V8;
  std::uint8_t* staging = nullptr;
  if (host_source || host_destination) {
    const auto required = geometry.bytes * (host_source + host_destination);
    for (unsigned i = 0; i < invocation.scratch_count; ++i)
      if (invocation.scratch[i].domain == PS_KERNEL_DOMAIN_SHARED_V8 &&
          invocation.scratch[i].bytes >= required)
        staging = static_cast<std::uint8_t*>(invocation.scratch[i].allocation);
    if (!staging)
      return Answer({ErrorCode::InvalidArgument, "native COPY Shared staging"});
  }
  const auto resolve = [&](const void* data, std::uint64_t bytes,
                           std::uint32_t domain, bool writable) {
    return domain == PS_KERNEL_DOMAIN_DEVICE_V8
               ? device.private_view(data, bytes, writable)
               : device.view(static_cast<const std::uint8_t*>(data), bytes,
                             writable);
  };
  auto source =
      host_source ? device.view(staging, geometry.bytes, true)
                  : resolve(input.allocation, input.bytes, input.domain, false);
  if (!source.ok())
    return Answer(source.status());
  result.source = source.take_value();
  auto* output_staging =
      staging ? staging + (host_source ? geometry.bytes : 0) : nullptr;
  auto destination =
      host_destination
          ? device.view(output_staging, geometry.bytes, true)
          : resolve(output.allocation, output.bytes, output.domain, true);
  if (!destination.ok())
    return Answer(destination.status());
  result.destination = destination.take_value();
  if (host_source) {
    geometry.source_origin = input.byte_offset;
    const auto contiguous = geometry.contiguous_bytes();
    std::uint64_t chunk = 0;
    for (std::uint64_t offset = 0; offset < geometry.bytes;
         offset += contiguous, ++chunk) {
      if ((chunk & 4095) == 0 && cancelled())
        return Answer({ErrorCode::Cancelled, "native COPY gather cancelled"});
      std::memcpy(staging + offset,
                  static_cast<const std::uint8_t*>(input.allocation) +
                      geometry.source_offset(offset / geometry.width),
                  contiguous);
    }
    auto stride = geometry.width;
    for (unsigned axis = geometry.rank; axis-- > 0;) {
      result.geometry.strides[axis] = static_cast<std::int64_t>(stride);
      stride *= geometry.shape[axis];
    }
  }
  result.geometry.source_origin =
      result.source.offset + (host_source ? 0 : input.byte_offset);
  result.geometry.destination_origin =
      result.destination.offset + (host_destination ? 0 : output.byte_offset);
  if (host_destination) {
    result.download_source = output_staging;
    result.download_destination =
        static_cast<std::uint8_t*>(output.allocation) + output.byte_offset;
  }
  return Answer(std::move(result));
}
}  // namespace ps::gpu_internal
