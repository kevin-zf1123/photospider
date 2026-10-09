#include <algorithm>
#include <cstring>
#include <utility>
#include <vector>

#include "core/checked_math.hpp"
#include "core/status_helpers.hpp"
#include "data/fragment_atlas_work.hpp"
#include "data/result_window_access.hpp"
#include "photospider/data/fragment_atlas.hpp"
#include "photospider/plugin/result_program.hpp"

namespace ps {
namespace {
using Work = data_internal::FragmentAtlasWork;
}  // namespace
Result<FragmentAtlasPlan> FragmentAtlasPlan::prepare(
    const ResultTensorInput& input, std::vector<std::uint64_t> geometry,
    const FootprintLimits& limits) {
  if (!input.result_.valid())
    return Result<FragmentAtlasPlan>(data_internal::invalid_fragment_atlas());
  return prepare_regions(
      {input.spec().descriptor.element_type, input.spec().sample_shape()},
      input.coverage(), std::move(geometry), limits);
}
Result<FragmentAtlas> FragmentAtlasPlan::materialize(
    const ResultTensorInput& input, const BufferAllocator& allocator,
    const FootprintLimits& limits) const {
  using Answer = Result<FragmentAtlas>;
  if (!impl_ || !input.result_.valid())
    return Answer(data_internal::invalid_fragment_atlas());
  if (!input.payload_authorized_)
    return Answer(input.read({}, nullptr, 0, limits.cancellation));
  Work work{limits.maximum_work, limits.cancellation, limits.consume_work};
  const auto rank = input.coverage().shape().size();
  const auto boxes = input.coverage().boxes().size();
  const auto per_box = 1 + 2 * rank;
  if (boxes > limits.maximum_boxes || boxes == UINT64_MAX ||
      !core_internal::can_multiply(boxes + 1, per_box) ||
      !core_internal::can_multiply_add(boxes, (boxes + 1) * per_box, 1))
    return Answer(core_internal::resource_exhausted());
  auto status = work.consume(1 + boxes * (boxes + 1) * per_box);
  if (!status.ok())
    return Answer(status);
  ResourceVector<ResultTensorReadWindow> windows;
  std::uint64_t max_read_work = 0;
  for (const auto& box : input.coverage().boxes()) {
    auto window = input.acquire(box, limits.cancellation);
    if (!window.ok())
      return Answer(window.status());
    auto read_work =
        execution_internal::ResultWindowAccess::read_work(window.value());
    if (!read_work.ok())
      return Answer(read_work.status());
    max_read_work = std::max(max_read_work, read_work.value());
    windows.push_back(window.take_value());
  }
  const auto count = input.coverage().element_count();
  if (!count.ok() ||
      !core_internal::can_multiply_add(boxes, rank + 1, max_read_work))
    return Answer(core_internal::resource_exhausted());
  const auto cost = max_read_work + boxes * (rank + 1);
  if (!core_internal::can_multiply(count.value(), cost))
    return Answer(core_internal::resource_exhausted());
  status = work.consume(count.value() * cost);
  if (!status.ok())
    return Answer(status);
  auto remaining = limits;
  remaining.maximum_work = work.left;
  return materialize_regions(
      {input.spec().descriptor.element_type, input.spec().sample_shape()},
      input.coverage(),
      [&](const auto& at, void* destination, std::size_t bytes) {
        for (const auto& window : windows) {
          bool contains = true;
          for (std::size_t axis = 0; axis < at.size(); ++axis) {
            const auto& span = window.region().dimensions()[axis];
            contains = contains && at[axis] >= span.offset &&
                       at[axis] - span.offset < span.extent;
          }
          if (!contains)
            continue;
          auto row = window.row_run(at);
          if (!row.ok())
            return row.status();
          std::memcpy(destination, row.value().data, bytes);
          return Status::success();
        }
        return data_internal::invalid_fragment_atlas();
      },
      allocator, remaining);
}
}  // namespace ps
