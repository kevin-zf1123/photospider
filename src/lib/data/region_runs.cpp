#include "photospider/data/region_runs.hpp"

#include <algorithm>
#include <cstring>
#include <limits>

namespace ps {
Status copy_value_region(
    const Value& source, const Region& region, const Region& destination,
    std::uint8_t* data, std::uint64_t bytes,
    const CancellationToken& cancellation,
    const std::function<Status(std::uint64_t)>& consume_samples) {
  if (!source.valid() || !data)
    return Status{ErrorCode::InvalidArgument, "invalid region copy buffer"};
  const auto width = Value::element_size(source.descriptor().element_type);
  auto count = destination.element_count();
  if (!count.ok())
    return count.status();
  if (count.value() > UINT64_MAX / width || bytes != count.value() * width ||
      bytes > static_cast<std::uint64_t>(PTRDIFF_MAX))
    return Status{ErrorCode::InvalidArgument,
                  "region copy byte count mismatch"};
  const auto from = reinterpret_cast<std::uintptr_t>(source.bytes().data());
  const auto to = reinterpret_cast<std::uintptr_t>(data);
  if (bytes > UINTPTR_MAX - to || source.bytes().size() > UINTPTR_MAX - from ||
      (to < from + source.bytes().size() && from < to + bytes))
    return Status{ErrorCode::InvalidArgument, "region copy buffers overlap"};
  return visit_value_runs(
      source, region, destination, 1024, {}, [&](const ValueReadRun& run) {
        if (cancellation.cancelled())
          return Status{ErrorCode::Cancelled, "region copy cancelled"};
        if (consume_samples) {
          auto status = consume_samples(run.samples);
          if (!status.ok())
            return status;
        }
        auto* out = data + run.destination_element * width;
        if (run.stride_bytes == static_cast<std::int64_t>(width)) {
          std::memcpy(out, run.data, run.samples * width);
        } else if (run.stride_bytes == 0) {
          std::memcpy(out, run.data, width);
          for (std::uint64_t filled = 1; filled < run.samples;) {
            const auto n = std::min(filled, run.samples - filled);
            std::memcpy(out + filled * width, out, n * width);
            filled += n;
          }
        } else {
          for (std::uint64_t i = 0; i < run.samples; ++i)
            std::memcpy(
                out + i * width,
                run.data + static_cast<std::int64_t>(static_cast<__int128>(i) *
                                                     run.stride_bytes),
                width);
        }
        return Status::success();
      });
}
}  // namespace ps
