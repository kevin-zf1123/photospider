#pragma once

#include <algorithm>
#include <cstdint>
#include <functional>
#include <optional>
#include <vector>

#include "photospider/data/value.hpp"
#include "photospider/execution/cancellation.hpp"

namespace ps {

/** @brief One borrowed, logically ordered source run and its dense destination.
 * Data is valid only while the input Value lives. A negative/zero stride is
 * intentional. destination_element is relative to the complete destination
 * region, whereas logical_element is relative to the visited subregion.
 */
struct ValueReadRun final {
  const std::uint8_t* data = nullptr;
  std::uint64_t samples = 0;
  std::int64_t stride_bytes = 0;
  std::uint64_t destination_element = 0;
  std::uint64_t logical_element = 0;
};

/** @brief Decode a subregion-relative row-major element into global
 * coordinates. Preconditions: region is valid/nonempty and index < its element
 * count. The caller may reuse coordinate capacity across calls.
 */
inline void region_run_coordinate(const Region& region, std::uint64_t index,
                                  std::vector<std::uint64_t>* coordinate) {
  coordinate->resize(region.rank());
  for (std::size_t axis = region.rank(); axis-- > 0;) {
    const auto d = region.dimensions()[axis];
    (*coordinate)[axis] = d.offset + index % d.extent;
    index /= d.extent;
  }
}

/** @brief Visit bounded runs of a valid Value in row-major logical order.
 * Both regions use the source descriptor axes. The visited region must be
 * covered by the source and contained in destination. Contiguous trailing
 * axes are merged only when BOTH source and dense destination permit it.
 * A fixed_axis, when present, prevents a run crossing that coordinate (useful
 * for per-channel parameters). Negative/broadcast innermost strides are kept.
 * No output is written or published by the visitor machinery. The callback
 * owns cancellation/work accounting and may stop traversal with any failure.
 * maximum_samples must be in [1, 65536]; at most one bounded callback is made
 * before the caller can poll again. Allocations may throw std::bad_alloc.
 */
template <class Visitor>
Status visit_value_runs(const Value& source, const Region& region,
                        const Region& destination,
                        std::uint64_t maximum_samples,
                        std::optional<std::uint32_t> fixed_axis,
                        Visitor&& visitor) {
  const auto invalid = [] {
    return Status{ErrorCode::InvalidArgument, "invalid Value run traversal"};
  };
  if (!source.valid() || source.region().empty() ||
      source.region().rank() != region.rank() || region.empty() ||
      destination.empty() || maximum_samples == 0 || maximum_samples > 65536 ||
      (fixed_axis && *fixed_axis >= region.rank()))
    return invalid();
  const auto& shape = source.descriptor().shape;
  if (!region.validate(shape).ok() || !destination.validate(shape).ok() ||
      region.rank() > 8)
    return invalid();
  const auto& dims = region.dimensions();
  const auto& target = destination.dimensions();
  const auto& admitted = source.region().dimensions();
  for (std::size_t a = 0; a < dims.size(); ++a) {
    for (const auto outer : {target[a], admitted[a]})
      if (dims[a].offset < outer.offset ||
          dims[a].offset - outer.offset > outer.extent ||
          dims[a].extent > outer.extent - (dims[a].offset - outer.offset))
        return invalid();
  }
  auto count = region.element_count();
  if (!count.ok())
    return count.status();
  auto target_count = destination.element_count();
  if (!target_count.ok())
    return target_count.status();
  const auto width = Value::element_size(source.descriptor().element_type);
  const auto& strides = source.layout().byte_strides;
  std::size_t first = dims.size();
  std::uint64_t run = 1, target_stride = 1;
  for (std::size_t a = dims.size(); a-- > 0;) {
    if (fixed_axis && a == *fixed_axis)
      break;
    if (dims[a].extent != 1 && (static_cast<__int128>(strides[a]) !=
                                    static_cast<__int128>(run) * width ||
                                target_stride != run))
      break;
    run *= dims[a].extent;  // Bounded by the checked region element count.
    target_stride *= target[a].extent;
    first = a;
  }
  std::int64_t stride = static_cast<std::int64_t>(width);
  if (first == dims.size() && (!fixed_axis || *fixed_axis != dims.size() - 1)) {
    first = dims.size() - 1;
    run = dims.back().extent;
    stride = strides.back();
  }
  std::vector<std::uint64_t> at;
  at.reserve(dims.size());
  for (const auto& d : dims)
    at.push_back(d.offset);
  for (std::uint64_t logical = 0; logical < count.value(); logical += run) {
    auto address = source.byte_address(at);
    if (!address.ok())
      return address.status();
    std::uint64_t to = 0;
    for (std::size_t a = 0; a < dims.size(); ++a)
      to = to * target[a].extent + at[a] - target[a].offset;
    const auto* data = source.bytes().data() + address.value();
    for (std::uint64_t x = 0; x < run;) {
      const auto n = std::min(maximum_samples, run - x);
      // Validated Value addresses and the run proof bound this signed offset.
      const auto delta =
          static_cast<std::int64_t>(static_cast<__int128>(x) * stride);
      auto status =
          visitor(ValueReadRun{data + delta, n, stride, to + x, logical + x});
      if (!status.ok())
        return status;
      x += n;
    }
    for (std::size_t a = first; a-- > 0;) {
      if (++at[a] < dims[a].offset + dims[a].extent)
        break;
      at[a] = dims[a].offset;
    }
  }
  return Status::success();
}

/** @brief Copy samples, not padding, into an unpublished dense destination.
 * The buffer must hold exactly destination.element_count()*element_size bytes
 * and must not overlap the source ByteView. No bytes outside region are
 * touched. consume_samples, when supplied, is called before each <=1024-sample
 * chunk. Failure/cancellation can leave an unpublished prefix; the host owns
 * commit. A raw pointer remains a trusted C++ buffer contract, not a memory
 * sandbox.
 */
PHOTOSPIDER_API Status copy_value_region(
    const Value& source, const Region& region, const Region& destination,
    std::uint8_t* data, std::uint64_t bytes,
    const CancellationToken& cancellation = {},
    const std::function<Status(std::uint64_t)>& consume_samples = {});

}  // namespace ps
