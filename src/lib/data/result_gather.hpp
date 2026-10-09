#pragma once

#include <algorithm>
#include <array>
#include <functional>
#include <memory>
#include <utility>
#include <vector>

#include "core/checked_math.hpp"
#include "core/radix_sort.hpp"
#include "data/footprint_index.hpp"
#include "photospider/data/result_relation.hpp"

namespace ps::gather_internal {
using Coordinate = std::array<std::uint64_t, 8>;

// The index is local to the witnessed rectangle. Neither tensor's full
// cardinality needs to fit uint64, and grouped output channels share one row.
struct Table final {
  ResourceBudget budget;
  ResourceLease lease;
  ResourceVector<std::uint64_t> output_shape, input_shape, coordinates, present;
  ResourceVector<ResultMappedAxis> axes;
  Footprint outputs;
  ResourceVector<std::uint64_t> first;
  std::uint32_t tuple_axes = 0, indexed_axes = 0, columns = 0, taps = 0;
  std::uint32_t validation_axes = 0;
  Coordinate fixed_coordinates{};
  std::uint32_t fixed_axes = 0;
  std::array<unsigned, 8> key_width{}, key_shift{};
  std::array<unsigned, 8> coordinate_width{}, coordinate_shift{};
  unsigned coordinate_bits = 0;
  bool packed_keys = false;
  Footprint full_support, full_validation;
  Coordinate lower{}, upper{};
  std::uint64_t sample_count = 0;

  explicit Table(ResourceBudget root) : budget(std::move(root)) {}

  void prepare_keys() {
    unsigned bits = 0;
    for (std::size_t axis = input_shape.size(); axis-- > 0;) {
      const auto maximum = input_shape[axis] - 1;
      const auto width =
          maximum ? 64U - static_cast<unsigned>(__builtin_clzll(maximum)) : 0U;
      if (indexed_axes & (1U << axis)) {
        coordinate_width[axis] = width;
        coordinate_shift[axis] = coordinate_bits;
        coordinate_bits += width;
      }
      if (fixed_axes & (1U << axis))
        continue;
      key_width[axis] = width;
      key_shift[axis] = bits;
      bits += width;
    }
    packed_keys = bits <= 64;
  }

  static std::uint64_t mask(unsigned width) {
    return width == 64 ? UINT64_MAX : (UINT64_C(1) << width) - 1;
  }

  std::uint64_t read_bits(std::uint64_t bit, unsigned width) const {
    if (!width)
      return 0;
    const auto word = bit / 64;
    const auto shift = static_cast<unsigned>(bit % 64);
    auto value = coordinates[word] >> shift;
    if (shift && width > 64 - shift)
      value |= coordinates[word + 1] << (64 - shift);
    return value & mask(width);
  }

  void write_bits(std::uint64_t bit, unsigned width, std::uint64_t value) {
    if (!width)
      return;
    const auto word = bit / 64;
    const auto shift = static_cast<unsigned>(bit % 64);
    coordinates[word] |= value << shift;
    if (shift && width > 64 - shift)
      coordinates[word + 1] |= value >> (64 - shift);
  }

  bool has(std::uint64_t row, std::uint32_t tap) const {
    const auto reference = row * taps + tap;
    return (present[reference / 64] >> (reference % 64)) & 1U;
  }

  void store(std::uint64_t row, std::uint32_t tap, const Coordinate& at) {
    const auto reference = row * taps + tap;
    present[reference / 64] |= UINT64_C(1) << (reference % 64);
    const auto first_bit = reference * coordinate_bits;
    std::uint64_t packed = 0;
    for (std::size_t axis = 0; axis < input_shape.size(); ++axis) {
      if (!(indexed_axes & (1U << axis)))
        continue;
      lower[axis] = sample_count ? std::min(lower[axis], at[axis]) : at[axis];
      upper[axis] = sample_count ? std::max(upper[axis], at[axis]) : at[axis];
      if (!coordinate_width[axis])
        continue;
      if (coordinate_bits <= 64)
        packed |= at[axis] << coordinate_shift[axis];
      else
        write_bits(first_bit + coordinate_shift[axis], coordinate_width[axis],
                   at[axis]);
    }
    if (coordinate_bits <= 64)
      write_bits(first_bit, coordinate_bits, packed);
    ++sample_count;
  }

  // Indexed fields have their own layout: mapped coordinates do not occupy
  // the retained tape. Wide tuples still use bounded, at-most-two-word reads
  // per axis; only normalization needs its reference-key fallback.
  Status allocate(std::uint64_t rows, const FootprintLimits& limits) {
    if (!core_internal::can_multiply(rows, taps))
      return {ErrorCode::ResourceExhausted, "gather table size"};
    const auto entries = rows * taps;
    if (!core_internal::can_multiply(entries, coordinate_bits))
      return {ErrorCode::ResourceExhausted, "gather coordinate bits"};
    const auto bits = entries * coordinate_bits;
    const auto presence_words = entries / 64 + (entries % 64 != 0);
    const auto coordinate_words = bits / 64 + (bits % 64 != 0);
    auto charged = charge(presence_words + coordinate_words, limits);
    if (!charged.ok())
      return charged;
    present =
        ResourceVector<std::uint64_t>(ResourceAllocator<std::uint64_t>(budget));
    coordinates =
        ResourceVector<std::uint64_t>(ResourceAllocator<std::uint64_t>(budget));
    const auto clear = [&](auto& tape, std::uint64_t count) {
      tape.reserve(count);
      while (tape.size() < count) {
        if (limits.cancellation.cancelled())
          return Status{ErrorCode::Cancelled, {}};
        tape.resize(std::min(count, tape.size() + UINT64_C(32768)));
      }
      return limits.cancellation.cancelled() ? Status{ErrorCode::Cancelled, {}}
                                             : Status::success();
    };
    auto status = clear(present, presence_words);
    return status.ok() ? clear(coordinates, coordinate_words) : status;
  }

  std::uint64_t source_work() const {
    return input_shape.size() + columns +
           (coordinate_bits <= 64 ? 2 : 2 * columns);
  }

  std::uint64_t pack(const Coordinate& at) const {
    std::uint64_t key = 0;
    for (std::size_t axis = 0; axis < input_shape.size(); ++axis)
      if (key_width[axis])
        key |= at[axis] << key_shift[axis];
    return key;
  }

  // All phases of one query share one local work allowance. Nested Footprint
  // operations call this same adapter, so normalization cannot reset the limit.
  struct QueryWork {
    FootprintLimits limits;
    std::uint64_t remaining;
    QueryWork(const Table& table, const FootprintLimits& outer)
        : limits(outer), remaining(outer.maximum_work) {
      limits.consume_work = [this, &table, &outer](std::uint64_t n) {
        if (outer.cancellation.cancelled())
          return Status{ErrorCode::Cancelled, {}};
        if (n > remaining)
          return Status{ErrorCode::ResourceExhausted,
                        "gather query work limit"};
        remaining -= n;
        return outer.consume_work ? outer.consume_work(n)
                                  : table.budget.consume({n});
      };
    }
  };

  bool whole_request(const Footprint& requested) const {
    return requested == outputs;
  }

  std::size_t output_box(std::uint64_t row) const {
    return std::upper_bound(first.begin(), first.end(), row) - first.begin() -
           1;
  }

  Status charge(std::uint64_t n, const FootprintLimits& limits) const {
    if (limits.cancellation.cancelled())
      return {ErrorCode::Cancelled, {}};
    if (n > limits.maximum_work)
      return {ErrorCode::ResourceExhausted, "gather work limit"};
    auto status =
        limits.consume_work ? limits.consume_work(n) : budget.consume({n});
    if (!status.ok())
      return status;
    return limits.cancellation.cancelled() ? Status{ErrorCode::Cancelled, {}}
                                           : Status::success();
  }

  bool closed_axis(std::size_t axis) const {
    return axis >= input_shape.size() - validation_axes ||
           (!(indexed_axes & (1U << axis)) &&
            (axes[axis].output_axis < 0 || !axes[axis].step) &&
            axes[axis].source_origin == 0 &&
            axes[axis].extent >= input_shape[axis]);
  }

  bool data_closed() const {
    for (std::size_t axis = input_shape.size() - validation_axes;
         axis < input_shape.size(); ++axis)
      if ((indexed_axes & (1U << axis)) ||
          (axes[axis].output_axis >= 0 && axes[axis].step) ||
          axes[axis].source_origin || axes[axis].extent < input_shape[axis])
        return false;
    return true;
  }

  std::uint64_t ordinal(const Coordinate& at, std::size_t box) const {
    const auto& dims = outputs.boxes()[box].dimensions();
    std::uint64_t row = 0;
    for (std::size_t axis = 0; axis < output_shape.size() - tuple_axes; ++axis)
      row = row * dims[axis].extent + at[axis] - dims[axis].offset;
    return first[box] + row;
  }

  Coordinate source(const Coordinate& output, std::uint64_t row,
                    std::uint32_t tap) const {
    Coordinate at = fixed_coordinates;
    const auto first_bit = (row * taps + tap) * coordinate_bits;
    const auto packed =
        coordinate_bits <= 64 ? read_bits(first_bit, coordinate_bits) : 0;
    for (std::size_t axis = 0; axis < input_shape.size(); ++axis) {
      if (coordinate_width[axis]) {
        at[axis] = coordinate_bits <= 64
                       ? (packed >> coordinate_shift[axis]) &
                             mask(coordinate_width[axis])
                       : read_bits(first_bit + coordinate_shift[axis],
                                   coordinate_width[axis]);
      } else if (!(indexed_axes & (1U << axis)) &&
                 !(fixed_axes & (1U << axis))) {
        const auto m = axes[axis];
        at[axis] = m.source_coordinate(output[m.output_axis]).value();
      }
    }
    return at;
  }

  std::uint64_t input_coordinate(std::uint64_t reference,
                                 std::size_t axis) const {
    if (fixed_axes & (1U << axis))
      return fixed_coordinates[axis];
    if (indexed_axes & (1U << axis))
      return read_bits(reference * coordinate_bits + coordinate_shift[axis],
                       coordinate_width[axis]);
    const auto m = axes[axis];
    if (m.output_axis < 0 || !m.step)
      return m.source_origin;
    auto row = reference / taps;
    const auto box = first.size() == 1 ? 0 : output_box(row);
    row -= first[box];
    const auto& dims = outputs.boxes()[box].dimensions();
    for (std::size_t a = output_shape.size() - tuple_axes;
         a-- > static_cast<std::size_t>(m.output_axis) + 1;)
      row /= dims[a].extent;
    const auto d = dims[m.output_axis];
    return m.source_coordinate(d.offset + row % d.extent).value();
  }

  Coordinate input(std::uint64_t reference) const {
    Coordinate at{};
    for (std::size_t axis = 0; axis < input_shape.size(); ++axis) {
      if (!packed_keys) {
        at[axis] = input_coordinate(reference, axis);
      } else if (fixed_axes & (1U << axis)) {
        at[axis] = fixed_coordinates[axis];
      } else if (key_width[axis]) {
        const auto mask = key_width[axis] == 64
                              ? UINT64_MAX
                              : (UINT64_C(1) << key_width[axis]) - 1;
        at[axis] = (reference >> key_shift[axis]) & mask;
      }
    }
    return at;
  }

  template <class Visitor>
  Status each_tuple(const Footprint& requested, const FootprintLimits& limits,
                    Visitor visitor, std::uint64_t per_tap = 0) const {
    const auto prefix = output_shape.size() - tuple_axes;
    std::uint64_t lookup = 1;
    for (auto n = outputs.boxes().size(); n; n >>= 1)
      lookup += 2 * output_shape.size();
    for (const auto& box : requested.boxes()) {
      Coordinate at{};
      std::uint64_t count = 1;
      for (std::size_t axis = 0; axis < output_shape.size(); ++axis) {
        const auto d = box.dimensions()[axis];
        at[axis] = d.offset;
        if (axis < prefix) {
          if (!core_internal::can_multiply(count, d.extent))
            return {ErrorCode::ResourceExhausted, "gather tuple count"};
          count *= d.extent;
        }
      }
      const auto initial = footprint_internal::containing(
          outputs.boxes(), at.data(), output_shape.size());
      if (initial == outputs.boxes().size())
        return {ErrorCode::InvalidArgument, "outside gather witness"};
      bool contained = true;
      for (std::size_t axis = 0; axis < output_shape.size(); ++axis) {
        const auto a = box.dimensions()[axis],
                   b = outputs.boxes()[initial].dimensions()[axis];
        contained &= a.extent <= b.extent - (a.offset - b.offset);
      }
      const auto unit =
          1 + taps * (source_work() + 1 + per_tap) + (contained ? 0 : lookup);
      if (!core_internal::can_multiply_add(count, unit, lookup))
        return {ErrorCode::ResourceExhausted, "gather work overflow"};
      auto charged = charge(count * unit + lookup, limits);
      if (!charged.ok())
        return charged;
      for (std::uint64_t i = 0; i < count; ++i) {
        if (!(i & 255U) && limits.cancellation.cancelled())
          return {ErrorCode::Cancelled, {}};
        const auto selected =
            contained ? initial
                      : footprint_internal::containing(
                            outputs.boxes(), at.data(), output_shape.size());
        if (selected == outputs.boxes().size())
          return {ErrorCode::InvalidArgument, "outside gather witness"};
        auto status = visitor(at, ordinal(at, selected), box);
        if (!status.ok())
          return status;
        for (std::size_t axis = prefix; axis-- > 0;) {
          const auto d = box.dimensions()[axis];
          if (++at[axis] < d.offset + d.extent)
            break;
          at[axis] = d.offset;
        }
      }
    }
    return Status::success();
  }

  // Both enumerators produce lexicographically ordered rectangle origins.
  // Extents and boundary clipping stay here, including non-point Gather taps.
  template <class Enumerate>
  Result<Footprint> normalize_ordered(Enumerate enumerate,
                                      const FootprintLimits& limits) const {
    using Answer = Result<Footprint>;
    const auto rank = input_shape.size();
    auto merge_axis = rank - 1;
    while (merge_axis && !(indexed_axes & (1U << merge_axis)) &&
           (axes[merge_axis].output_axis < 0 || !axes[merge_axis].step) &&
           !axes[merge_axis].source_origin &&
           axes[merge_axis].extent >= input_shape[merge_axis])
      --merge_axis;
    ResourceVector<Region> boxes{ResourceAllocator<Region>(budget)};
    ResourceVector<ResourceLease> owners{
        ResourceAllocator<ResourceLease>(budget)};
    Coordinate previous{};
    bool have_previous = false;
    Coordinate run_origin{}, run_lengths{};
    bool have_run = false;
    const auto flush = [&]() -> Status {
      if (!have_run)
        return Status::success();
      if (boxes.size() >= limits.maximum_boxes)
        return {ErrorCode::ResourceExhausted, "gather box limit"};
      const auto bytes = rank * sizeof(RegionDimension);
      auto admitted = budget.reserve(ResourceCapacity::host(bytes, bytes));
      if (!admitted.ok())
        return admitted.status();
      owners.push_back(admitted.take_value());
      std::vector<RegionDimension> dimensions;
      dimensions.reserve(rank);
      for (std::size_t axis = 0; axis < rank; ++axis)
        dimensions.push_back({run_origin[axis], run_lengths[axis]});
      boxes.emplace_back(std::move(dimensions));
      return Status::success();
    };
    const auto accept = [&](const Coordinate& point) -> Status {
      if (have_previous && point == previous)
        return Status::success();
      previous = point;
      have_previous = true;
      Coordinate lengths{};
      for (std::size_t axis = 0; axis < rank; ++axis)
        lengths[axis] =
            std::min(axes[axis].extent, input_shape[axis] - point[axis]);
      bool adjacent = have_run;
      if (adjacent) {
        for (std::size_t axis = 0; axis < rank; ++axis) {
          if (axis == merge_axis)
            adjacent &= point[axis] <= run_origin[axis] + run_lengths[axis];
          else
            adjacent &= point[axis] == run_origin[axis] &&
                        lengths[axis] == run_lengths[axis];
        }
      }
      if (adjacent) {
        run_lengths[merge_axis] =
            std::max(run_origin[merge_axis] + run_lengths[merge_axis],
                     point[merge_axis] + lengths[merge_axis]) -
            run_origin[merge_axis];
      } else {
        auto status = flush();
        if (!status.ok())
          return status;
        run_origin = point;
        run_lengths = lengths;
        have_run = true;
      }
      return Status::success();
    };
    auto enumerated = enumerate(accept);
    if (!enumerated.ok())
      return Answer(enumerated);
    auto flushed = flush();
    if (!flushed.ok())
      return Answer(flushed);
    const auto bytes = boxes.size() * sizeof(Region);
    auto admitted = budget.reserve(ResourceCapacity::host(bytes, bytes));
    if (!admitted.ok())
      return Answer(admitted.status());
    std::vector<Region> normalized;
    normalized.reserve(boxes.size());
    for (auto& box : boxes)
      normalized.push_back(std::move(box));
    return Footprint::from_regions({input_shape.begin(), input_shape.end()},
                                   normalized, limits);
  }

  Result<Footprint> normalize(ResourceVector<std::uint64_t> points,
                              const FootprintLimits& limits) const {
    using Answer = Result<Footprint>;
    const auto rank = input_shape.size();
    std::array<std::size_t, 8> sort_axes{};
    std::size_t sort_rank = 0;
    for (std::size_t axis = 0; axis < rank; ++axis)
      if (!(fixed_axes & (1U << axis)))
        sort_axes[sort_rank++] = axis;
    std::uint64_t coordinate_cost = packed_keys ? 1 : 3;
    if (!packed_keys && outputs.boxes().size() > 1) {
      for (std::size_t axis = 0; axis < rank; ++axis) {
        if (!(fixed_axes & (1U << axis)) && axes[axis].output_axis >= 0 &&
            axes[axis].step) {
          for (auto n = outputs.boxes().size(); n; n >>= 1)
            ++coordinate_cost;
        }
      }
    }
    auto charged = radix_internal::sort(
        &points, packed_keys ? 1 : sort_rank,
        [&](auto reference, auto axis) {
          return packed_keys ? reference
                             : input_coordinate(reference, sort_axes[axis]);
        },
        [&](auto n) {
          if (!core_internal::can_multiply(n, coordinate_cost))
            return Status{ErrorCode::ResourceExhausted,
                          "gather radix work overflow"};
          return charge(n * coordinate_cost, limits);
        },
        limits.cancellation);
    if (!charged.ok())
      return Answer(charged);
    const auto unit = 3 * rank + source_work() + 1;
    if (!core_internal::can_multiply(points.size(), unit))
      return Answer(
          Status{ErrorCode::ResourceExhausted, "gather normalization work"});
    charged = charge(points.size() * unit, limits);
    if (!charged.ok())
      return Answer(charged);
    return normalize_ordered(
        [&](const auto& accept) {
          for (std::size_t i = 0; i < points.size(); ++i) {
            if (!(i & 255U) && limits.cancellation.cancelled())
              return Status{ErrorCode::Cancelled, {}};
            auto status = accept(input(points[i]));
            if (!status.ok())
              return status;
          }
          return Status::success();
        },
        limits);
  }

  std::uint64_t bitmap_area(std::uint64_t capacity) const {
    // The stored bound may be conservative for mapped axes or a partial Q.
    // Its admission uses this query's sample bound, never the full table alone.
    const auto count = std::min(capacity, sample_count);
    if (!count)
      return 0;
    const auto maximum = core_internal::saturating_multiply(count, 8);
    std::uint64_t area = 1;
    for (std::size_t axis = 0; axis < input_shape.size(); ++axis) {
      const auto extent = upper[axis] - lower[axis] + 1;
      if (!core_internal::can_multiply(extent, area, maximum))
        return 0;
      area *= extent;
    }
    return area;
  }

  Result<Footprint> normalize_bitmap(const Footprint& requested,
                                     std::uint64_t area,
                                     const FootprintLimits& limits) const {
    using Answer = Result<Footprint>;
    const auto rank = input_shape.size();
    Coordinate extents{}, strides{};
    std::array<std::size_t, 8> varying{};
    std::size_t varying_count = 0;
    std::uint64_t stride = 1;
    for (std::size_t axis = rank; axis-- > 0;) {
      extents[axis] = upper[axis] - lower[axis] + 1;
      strides[axis] = stride;
      stride *= extents[axis];
      if (extents[axis] > 1)
        varying[varying_count++] = axis;
    }
    const auto words = area / 64 + (area % 64 != 0);
    auto status = charge(words, limits);
    if (!status.ok())
      return Answer(status);
    // The allocator admits Host/Metadata before the bitmap is zero-filled.
    ResourceVector<std::uint64_t> bitmap{
        ResourceAllocator<std::uint64_t>(budget)};
    bitmap.reserve(words);
    while (bitmap.size() < words) {
      if (limits.cancellation.cancelled())
        return Answer(Status{ErrorCode::Cancelled, {}});
      bitmap.resize(std::min(words, bitmap.size() + UINT64_C(32768)));
    }
    status = each_tuple(
        requested, limits,
        [&](const auto& at, auto row, const auto&) {
          for (std::uint32_t tap = 0; tap < taps; ++tap) {
            if (!has(row, tap))
              continue;
            const auto point = source(at, row, tap);
            std::uint64_t index = 0;
            for (std::size_t i = 0; i < varying_count; ++i) {
              const auto axis = varying[i];
              index += (point[axis] - lower[axis]) * strides[axis];
            }
            bitmap[index / 64] |= UINT64_C(1) << (index % 64);
          }
          return Status::success();
        },
        2 * varying_count + 1);
    if (!status.ok())
      return Answer(status);
    return normalize_ordered(
        [&](const auto& accept) {
          for (std::uint64_t first_word = 0; first_word < words;) {
            const auto last_word = std::min(words, first_word + 64);
            auto charged = charge(2 * (last_word - first_word), limits);
            if (!charged.ok())
              return charged;
            std::uint64_t count = 0;
            for (auto word = first_word; word < last_word; ++word)
              count += __builtin_popcountll(bitmap[word]);
            charged = charge(count * (3 * rank + varying_count + 1), limits);
            if (!charged.ok())
              return charged;
            for (; first_word < last_word; ++first_word) {
              if (limits.cancellation.cancelled())
                return Status{ErrorCode::Cancelled, {}};
              auto bits = bitmap[first_word];
              while (bits) {
                auto index = first_word * 64 +
                             static_cast<unsigned>(__builtin_ctzll(bits));
                bits &= bits - 1;
                auto point = lower;
                for (std::size_t i = 0; i < varying_count; ++i) {
                  const auto axis = varying[i];
                  point[axis] += index % extents[axis];
                  index /= extents[axis];
                }
                auto accepted = accept(point);
                if (!accepted.ok())
                  return accepted;
              }
            }
          }
          return Status::success();
        },
        limits);
  }

  Result<Footprint> project(const Footprint& requested,
                            const FootprintLimits& bounds) const {
    QueryWork work(*this, bounds);
    const auto& limits = work.limits;
    auto checked = charge(
        requested.boxes().size() * (output_shape.size() + 1) + 1, limits);
    if (!checked.ok())
      return Result<Footprint>(checked);
    if (full_support.valid() && whole_request(requested)) {
      if (full_support.boxes().size() > limits.maximum_boxes)
        return Result<Footprint>(
            Status{ErrorCode::ResourceExhausted, "gather box limit"});
      auto charged = charge(input_shape.size() + 1, limits);
      return charged.ok() ? Result<Footprint>(full_support)
                          : Result<Footprint>(charged);
    }
    std::uint64_t capacity = 0;
    for (const auto& box : requested.boxes()) {
      std::uint64_t count = taps;
      for (std::size_t axis = 0; axis < output_shape.size() - tuple_axes;
           ++axis) {
        const auto extent = box.dimensions()[axis].extent;
        if (!core_internal::can_multiply(count, extent))
          return Result<Footprint>(
              Status{ErrorCode::ResourceExhausted, "gather tuple count"});
        count *= extent;
      }
      if (!core_internal::can_add(count, capacity))
        return Result<Footprint>(
            Status{ErrorCode::ResourceExhausted, "gather tuple count"});
      capacity += count;
    }
    if (!core_internal::can_multiply(capacity, input_shape.size() + 1,
                                     work.remaining))
      return Result<Footprint>(
          Status{ErrorCode::ResourceExhausted, "gather query work limit"});
    if (limits.cancellation.cancelled())
      return Result<Footprint>(Status{ErrorCode::Cancelled, {}});
    const auto area = bitmap_area(capacity);
    if (area)
      return normalize_bitmap(requested, area, limits);
    ResourceVector<std::uint64_t> points{
        ResourceAllocator<std::uint64_t>(budget)};
    points.reserve(std::min(capacity, sample_count));
    auto status = each_tuple(
        requested, limits,
        [&](const auto& at, auto row, const auto&) {
          for (std::uint32_t tap = 0; tap < taps; ++tap)
            if (has(row, tap))
              points.push_back(packed_keys ? pack(source(at, row, tap))
                                           : row * taps + tap);
          return Status::success();
        },
        packed_keys ? input_shape.size() : 0);
    return status.ok() ? normalize(std::move(points), limits)
                       : Result<Footprint>(status);
  }

  Result<Footprint> close(const Footprint& samples,
                          const FootprintLimits& bounds) const {
    QueryWork work(*this, bounds);
    const auto& limits = work.limits;
    if (!validation_axes || data_closed())
      return Result<Footprint>(samples);
    const auto bytes =
        samples.boxes().size() *
        (sizeof(Region) + input_shape.size() * sizeof(RegionDimension));
    auto admitted = budget.reserve(ResourceCapacity::host(bytes, bytes));
    if (!admitted.ok())
      return Result<Footprint>(admitted.status());
    std::vector<Region> boxes;
    auto charged =
        charge(samples.boxes().size() * (input_shape.size() + 1), limits);
    if (!charged.ok())
      return Result<Footprint>(charged);
    for (const auto& box : samples.boxes()) {
      auto dims = box.dimensions();
      for (std::size_t axis = input_shape.size() - validation_axes;
           axis < input_shape.size(); ++axis)
        dims[axis] = {0, input_shape[axis]};
      boxes.emplace_back(std::move(dims));
    }
    return Footprint::from_regions(samples.shape(), boxes, limits);
  }

  Status project(
      const Footprint& requested, ResultSupport support,
      const std::function<Status(ResultSupport, const Footprint*)>& visitor,
      const FootprintLimits& bounds) const {
    QueryWork work(*this, bounds);
    const auto& limits = work.limits;
    auto samples = project(requested, limits);
    if (!samples.ok())
      return samples.status();
    if (samples.value().empty())
      return Status::success();
    if (!validation_axes || data_closed()) {
      if (validation_axes)
        support.roles |= 4U;
      if (limits.cancellation.cancelled())
        return {ErrorCode::Cancelled, {}};
      return visitor(support, &samples.value());
    }
    auto validation = whole_request(requested)
                          ? Result<Footprint>(full_validation)
                          : close(samples.value(), limits);
    if (!validation.ok())
      return validation.status();
    if (validation.value().boxes().size() > limits.maximum_boxes)
      return {ErrorCode::ResourceExhausted, "gather validation box limit"};
    if (limits.cancellation.cancelled())
      return {ErrorCode::Cancelled, {}};
    auto status = visitor(support, &samples.value());
    if (!status.ok())
      return status;
    support.roles = 4;
    if (limits.cancellation.cancelled())
      return {ErrorCode::Cancelled, {}};
    return visitor(support, &validation.value());
  }

  Status visit(std::uint64_t output, ResultSupport support,
               std::uint64_t* remaining,
               const std::function<Status(ResultSupport)>& visitor) const {
    Coordinate at{};
    for (std::size_t axis = output_shape.size(); axis-- > 0;) {
      at[axis] = output % output_shape[axis];
      output /= output_shape[axis];
    }
    const auto box = footprint_internal::containing(outputs.boxes(), at.data(),
                                                    output_shape.size());
    if (box == outputs.boxes().size())
      return {ErrorCode::NotFound, "sample outside gather witness"};
    const auto row = ordinal(at, box);
    const auto emit = [&](Coordinate origin, bool validation) -> Status {
      Coordinate extent{}, coordinate = origin;
      std::size_t prefix = input_shape.size();
      std::uint64_t contiguous = 1;
      for (std::size_t axis = 0; axis < input_shape.size(); ++axis) {
        extent[axis] =
            std::min(axes[axis].extent, input_shape[axis] - origin[axis]);
        if (validation && axis >= input_shape.size() - validation_axes) {
          coordinate[axis] = origin[axis] = 0;
          extent[axis] = input_shape[axis];
        }
      }
      for (std::size_t axis = input_shape.size(); axis-- > 0;) {
        if (!core_internal::can_multiply(contiguous, extent[axis]))
          return {ErrorCode::ResourceExhausted, "gather span overflow"};
        contiguous *= extent[axis];
        prefix = axis;
        if (origin[axis] || extent[axis] != input_shape[axis])
          break;
      }
      for (;;) {
        if (!*remaining)
          return {ErrorCode::ResourceExhausted, "gather visit work limit"};
        --*remaining;
        auto charged = budget.consume({1});
        if (!charged.ok())
          return charged;
        std::uint64_t first = 0;
        for (std::size_t axis = 0; axis < input_shape.size(); ++axis) {
          if (!core_internal::can_multiply_add(first, input_shape[axis],
                                               coordinate[axis]))
            return {ErrorCode::ResourceExhausted, "gather position overflow"};
          first = first * input_shape[axis] + coordinate[axis];
        }
        if (!core_internal::can_add(contiguous, first))
          return {ErrorCode::ResourceExhausted, "gather span overflow"};
        auto span = support;
        span.first = first;
        span.count = contiguous;
        if (validation)
          span.roles = 4;
        else if (validation_axes && data_closed())
          span.roles |= 4;
        auto status = visitor(span);
        if (!status.ok())
          return status;
        std::size_t axis = prefix;
        while (axis) {
          --axis;
          if (++coordinate[axis] < origin[axis] + extent[axis])
            break;
          coordinate[axis] = origin[axis];
        }
        if (!axis && (!prefix || coordinate[0] == origin[0]))
          break;
      }
      return Status::success();
    };
    for (std::uint32_t tap = 0; tap < taps; ++tap) {
      if (!has(row, tap))
        continue;
      const auto origin = source(at, row, tap);
      auto status = emit(origin, false);
      if (status.ok() && validation_axes && !data_closed())
        status = emit(origin, true);
      if (!status.ok())
        return status;
    }
    return Status::success();
  }

  Result<Footprint> preimage(const Footprint& requested,
                             const Footprint& changed, bool validation,
                             const FootprintLimits& bounds) const {
    QueryWork work(*this, bounds);
    const auto& limits = work.limits;
    using Answer = Result<Footprint>;
    // Expand changed coordinates along the full tuple axes. This is a point
    // membership query even when the edit names only one channel of a tuple.
    std::array<bool, 8> full{};
    bool points = true;
    for (std::size_t axis = 0; axis < input_shape.size(); ++axis) {
      full[axis] =
          (validation && axis >= input_shape.size() - validation_axes) ||
          (!(indexed_axes & (1U << axis)) &&
           (axes[axis].output_axis < 0 || !axes[axis].step) &&
           !axes[axis].source_origin && axes[axis].extent >= input_shape[axis]);
      points &= full[axis] || axes[axis].extent == 1;
    }
    const auto bytes =
        changed.boxes().size() *
        (sizeof(Region) + input_shape.size() * sizeof(RegionDimension));
    auto admission = budget.reserve(ResourceCapacity::host(bytes, bytes));
    if (!admission.ok())
      return Answer(admission.status());
    std::vector<Region> expanded;
    if (points) {
      auto charged =
          charge(changed.boxes().size() * (input_shape.size() + 1), limits);
      if (!charged.ok())
        return Answer(charged);
      for (const auto& box : changed.boxes()) {
        auto dims = box.dimensions();
        for (std::size_t axis = 0; axis < dims.size(); ++axis)
          if (full[axis])
            dims[axis] = {0, input_shape[axis]};
        expanded.emplace_back(std::move(dims));
      }
    }
    auto edits =
        points ? Footprint::from_regions(changed.shape(), expanded, limits)
               : Answer(changed);
    if (!edits.ok())
      return edits;
    ResourceVector<Region> hits{ResourceAllocator<Region>(budget)};
    ResourceVector<ResourceLease> owners{
        ResourceAllocator<ResourceLease>(budget)};
    std::vector<std::uint64_t> point(input_shape.size());
    const auto prefix = output_shape.size() - tuple_axes;
    std::uint64_t lookup_work = 1;
    for (auto n = edits.value().boxes().size(); n; n >>= 1)
      lookup_work += 2 * input_shape.size();
    auto status = each_tuple(
        requested, limits, [&](const auto& at, auto row, const auto& box) {
          bool hit = false;
          for (std::uint32_t tap = 0; tap < taps && !hit; ++tap) {
            if (!has(row, tap))
              continue;
            const auto origin = source(at, row, tap);
            if (points) {
              auto charged = charge(lookup_work, limits);
              if (!charged.ok())
                return charged;
              for (std::size_t axis = 0; axis < point.size(); ++axis)
                point[axis] = full[axis] ? 0 : origin[axis];
              hit = edits.value().contains(point);
            } else {
              for (const auto& edit : changed.boxes()) {
                auto charged = charge(input_shape.size() + 1, limits);
                if (!charged.ok())
                  return charged;
                bool common = true;
                for (std::size_t axis = 0; axis < point.size(); ++axis) {
                  const auto first = full[axis] ? 0 : origin[axis];
                  const auto count = full[axis]
                                         ? input_shape[axis]
                                         : std::min(axes[axis].extent,
                                                    input_shape[axis] - first);
                  const auto d = edit.dimensions()[axis];
                  common &=
                      first < d.offset + d.extent && d.offset < first + count;
                }
                hit |= common;
                if (hit)
                  break;
              }
            }
          }
          if (!hit)
            return Status::success();
          auto dims = box.dimensions();
          for (std::size_t axis = 0; axis < prefix; ++axis)
            dims[axis] = {at[axis], 1};
          bool adjacent = prefix && !hits.empty();
          if (adjacent) {
            const auto& old = hits.back().dimensions();
            for (std::size_t axis = 0; axis < dims.size(); ++axis)
              adjacent &=
                  axis == prefix - 1
                      ? old[axis].offset + old[axis].extent == dims[axis].offset
                      : old[axis].offset == dims[axis].offset &&
                            old[axis].extent == dims[axis].extent;
          }
          if (adjacent) {
            const auto old = hits.back().dimensions()[prefix - 1];
            dims[prefix - 1] = {old.offset, old.extent + 1};
            hits.back() = Region(std::move(dims));
          } else {
            if (hits.size() >= limits.maximum_boxes)
              return Status{ErrorCode::ResourceExhausted,
                            "gather inverse box limit"};
            const auto bytes = dims.size() * sizeof(RegionDimension);
            auto admitted =
                budget.reserve(ResourceCapacity::host(bytes, bytes));
            if (!admitted.ok())
              return admitted.status();
            owners.push_back(admitted.take_value());
            hits.emplace_back(std::move(dims));
          }
          return Status::success();
        });
    if (!status.ok())
      return Answer(status);
    const auto copied = hits.size() * sizeof(Region);
    auto admitted = budget.reserve(ResourceCapacity::host(copied, copied));
    if (!admitted.ok())
      return Answer(admitted.status());
    std::vector<Region> normalized;
    normalized.reserve(hits.size());
    for (auto& hit : hits)
      normalized.push_back(std::move(hit));
    return Footprint::from_regions(requested.shape(), normalized, limits);
  }
};
}  // namespace ps::gather_internal
