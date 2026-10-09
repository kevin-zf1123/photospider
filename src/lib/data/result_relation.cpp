#include "photospider/data/result_relation.hpp"

#include <algorithm>
#include <array>
#include <cstring>
#include <functional>
#include <map>
#include <memory>
#include <set>
#include <tuple>
#include <utility>
#include <vector>

#include "core/checked_math.hpp"
#include "data/result_gather.hpp"
#include "data/result_neighborhood.hpp"
#include "data/result_reshape.hpp"
#include "photospider/core/resource_allocator.hpp"

namespace ps {
namespace {
bool valid_support(ResultSupport support) {
  return support.roles && !(support.roles & ~15U) && support.input < 1024 &&
         static_cast<std::uint32_t>(support.target) <= 3 && support.slot < 16 &&
         core_internal::can_add(support.count, support.first);
}
bool valid_guarantee(DependencyGuarantee guarantee) {
  return guarantee == DependencyGuarantee::Exact ||
         guarantee == DependencyGuarantee::Conservative ||
         guarantee == DependencyGuarantee::Unknown;
}
Status invalid_relation() {
  return Status{ErrorCode::InvalidArgument, "invalid result support relation"};
}
DependencyGuarantee combine_guarantee(DependencyGuarantee a,
                                      DependencyGuarantee b) {
  return static_cast<DependencyGuarantee>(
      std::max(static_cast<unsigned>(a), static_cast<unsigned>(b)));
}
}  // namespace
Result<std::uint64_t> ResultMappedAxis::source_coordinate(
    std::uint64_t output) const {
  if (output_axis < 0 || !step)
    return Result<std::uint64_t>(source_origin);
  const auto distance =
      output >= output_origin ? output - output_origin : output_origin - output;
  const auto magnitude = step < 0 ? static_cast<std::uint64_t>(-(step + 1)) + 1
                                  : static_cast<std::uint64_t>(step);
  const bool subtract = (output < output_origin) != (step < 0);
  const auto available = subtract ? source_origin : UINT64_MAX - source_origin;
  if (!core_internal::can_multiply(distance, magnitude, available))
    return Result<std::uint64_t>(invalid_relation());
  const auto delta = distance * magnitude;
  return Result<std::uint64_t>(subtract ? source_origin - delta
                                        : source_origin + delta);
}
namespace {
__int128 floor_div(__int128 numerator, __int128 denominator) {
  const auto quotient = numerator / denominator;
  return quotient - (numerator % denominator < 0 ? 1 : 0);
}
__int128 ceil_div(__int128 numerator, __int128 denominator) {
  return -floor_div(-numerator, denominator);
}
std::uint64_t step_magnitude(std::int64_t step) {
  return step < 0 ? static_cast<std::uint64_t>(-(step + 1)) + 1
                  : static_cast<std::uint64_t>(step);
}
}  // namespace
struct ResultRelation::Impl {
  enum class Kind {
    Cartesian,
    Mapped,
    Gather,
    Neighborhood,
    Reshape,
    Identity,
    Prefix,
    Rows,
    Samples,
    Union,
    Restricted,
    Compose,
    Unknown
  };
  explicit Impl(ResourceBudget value) : budget(std::move(value)) {}
  ResourceBudget budget;
  ResourceLease lease;
  Kind kind = Kind::Unknown;
  DependencyGuarantee guarantee = DependencyGuarantee::Unknown;
  std::uint64_t outputs = 0, row_count = 0;
  std::uint32_t depth = 1, children_count = 0;
  ResultSupport support;
  ResourceVector<std::uint64_t> output_shape, input_shape;
  ResourceVector<ResultMappedAxis> mapping;
  ResourceVector<std::uint64_t> radii;
  bool periodic = false;
  Region mapped_outputs, reshape_source;
  Footprint restricted_outputs;
  ResourceLease mapped_region_lease;
  TemporaryStorage rows;
  ResourceVector<ResultRelationRow> samples;
  std::shared_ptr<const gather_internal::Table> gather;
  std::array<std::shared_ptr<const Impl>, 16> children;
  Result<const Impl*> output_geometry() const {
    auto charged = budget.consume({1});
    if (!charged.ok())
      return Result<const Impl*>(charged);
    if (!output_shape.empty())
      return Result<const Impl*>(this);
    if (kind == Kind::Union || kind == Kind::Compose) {
      for (std::uint32_t i = 0; i < (kind == Kind::Union ? children_count : 1U);
           ++i) {
        auto geometry = children[i]->output_geometry();
        if (!geometry.ok() || geometry.value())
          return geometry;
      }
    }
    return Result<const Impl*>(nullptr);
  }
  Result<Footprint> clip(const Footprint& requested,
                         const FootprintLimits& limits) const;
  Result<Footprint> mask(const FootprintLimits& limits) const {
    const auto* value = gather                       ? &gather->outputs
                        : restricted_outputs.valid() ? &restricted_outputs
                                                     : nullptr;
    if (value) {
      if (limits.cancellation.cancelled())
        return Result<Footprint>(Status{ErrorCode::Cancelled, {}});
      if (value->boxes().size() > limits.maximum_boxes)
        return Result<Footprint>(
            Status{ErrorCode::ResourceExhausted, "relation mask box limit"});
      return Result<Footprint>(*value);
    }
    return Footprint::from_regions({output_shape.begin(), output_shape.end()},
                                   {mapped_outputs}, limits);
  }
  bool inside_mask(std::uint64_t output) const noexcept {
    if (gather || restricted_outputs.valid()) {
      std::array<std::uint64_t, 8> at{};
      for (std::size_t axis = output_shape.size(); axis-- > 0;) {
        at[axis] = output % output_shape[axis];
        output /= output_shape[axis];
      }
      const auto& boxes =
          gather ? gather->outputs.boxes() : restricted_outputs.boxes();
      return footprint_internal::containing(
                 boxes, at.data(), output_shape.size()) != boxes.size();
    }
    for (std::size_t axis = output_shape.size(); axis-- > 0;) {
      const auto at = output % output_shape[axis];
      output /= output_shape[axis];
      const auto d = mapped_outputs.dimensions()[axis];
      if (at < d.offset || at - d.offset >= d.extent)
        return false;
    }
    return true;
  }
  Status validate_output_shape(const std::vector<std::uint64_t>& shape) const {
    auto charged = budget.consume({shape.size() + 1});
    if (!charged.ok())
      return charged;
    if (kind == Kind::Mapped || kind == Kind::Gather || kind == Kind::Reshape ||
        kind == Kind::Neighborhood || kind == Kind::Prefix ||
        kind == Kind::Restricted) {
      if (!std::equal(output_shape.begin(), output_shape.end(), shape.begin(),
                      shape.end()))
        return invalid_relation();
    }
    if (kind == Kind::Union || kind == Kind::Restricted ||
        kind == Kind::Compose) {
      for (std::uint32_t i = 0; i < (kind == Kind::Union ? children_count : 1U);
           ++i) {
        auto valid = children[i]->validate_output_shape(shape);
        if (!valid.ok())
          return valid;
      }
    }
    return Status::success();
  }
  static Result<std::shared_ptr<Impl>> make(ResourceBudget budget,
                                            std::uint64_t outputs) {
    auto capacity = ResourceCapacity::host(sizeof(Impl), sizeof(Impl));
    capacity[ResourceKind::Entries] = 1;
    auto lease = budget.reserve(capacity);
    if (!lease.ok())
      return Result<std::shared_ptr<Impl>>(lease.status());
    try {
      auto impl = std::make_shared<Impl>(std::move(budget));
      impl->lease = lease.take_value();
      impl->outputs = outputs;
      return Result<std::shared_ptr<Impl>>(std::move(impl));
    } catch (const std::bad_alloc&) {
      return Result<std::shared_ptr<Impl>>(
          Status{ErrorCode::ResourceExhausted, {}});
    }
  }
  Status walk(std::uint64_t output, std::uint64_t* remaining,
              const std::function<Status(ResultSupport)>& visitor,
              bool declared = false, bool* outside = nullptr) const {
    if (outside)
      *outside = false;
    if (output >= outputs)
      return invalid_relation();
    auto tick = [&]() {
      if (!*remaining)
        return Status{ErrorCode::ResourceExhausted,
                      "result relation work exhausted"};
      auto status = budget.consume({1});
      if (status.ok())
        --*remaining;
      return status;
    };
    auto status = tick();
    if (!status.ok())
      return status;
    switch (kind) {
      case Kind::Unknown:
        if (declared)
          return Status::success();
        return Status{ErrorCode::NotFound, "Unresolved result support"};
      case Kind::Cartesian:
        return support.count ? visitor(support) : Status::success();
      case Kind::Identity:
        return visitor({support.input, support.roles, output, 1, support.target,
                        support.slot});
      case Kind::Prefix:
        return visitor({support.input, support.roles, 0, output + 1,
                        support.target, support.slot});
      case Kind::Gather:
        if (!inside_mask(output)) {
          if (outside)
            *outside = true;
          return {ErrorCode::NotFound, "sample outside gather witness"};
        }
        return gather->visit(output, support, remaining, visitor);
      case Kind::Reshape: {
        std::array<uint64_t, 8> output_at{}, source_at{};
        auto index = output;
        for (size_t axis = output_shape.size(); axis-- > 0;) {
          output_at[axis] = index % output_shape[axis];
          index /= output_shape[axis];
          const auto d = mapped_outputs.dimensions()[axis];
          if (output_at[axis] < d.offset ||
              output_at[axis] - d.offset >= d.extent)
            return {ErrorCode::NotFound, "sample outside reshape witness"};
        }
        reshape_internal::Ordinal ordinal(output);
        for (size_t axis = input_shape.size(); axis-- > 0;) {
          const auto d = reshape_source.dimensions()[axis];
          source_at[axis] = d.offset + ordinal.divide(d.extent);
        }
        uint64_t first = 0;
        for (size_t axis = 0; axis < input_shape.size(); ++axis) {
          if (!core_internal::can_multiply_add(first, input_shape[axis],
                                               source_at[axis]))
            return {ErrorCode::ResourceExhausted,
                    "reshape support cannot be flattened"};
          first = first * input_shape[axis] + source_at[axis];
        }
        if (first == UINT64_MAX)
          return {ErrorCode::ResourceExhausted,
                  "reshape support cannot be flattened"};
        auto point = support;
        point.first = first;
        point.count = 1;
        return visitor(point);
      }
      case Kind::Neighborhood: {
        ResourceAllocationScope scope(budget);
        auto scratch = budget.reserve(ResourceCapacity::host(512, 512));
        if (!scratch.ok())
          return scratch.status();
        std::vector<RegionDimension> dims(output_shape.size());
        auto ordinal = output;
        for (std::size_t axis = dims.size(); axis-- > 0;) {
          dims[axis] = {ordinal % output_shape[axis], 1};
          ordinal /= output_shape[axis];
        }
        FootprintLimits limits;
        limits.maximum_work = *remaining;
        limits.consume_work = [&](std::uint64_t amount) {
          if (amount > *remaining)
            return Status{ErrorCode::ResourceExhausted, {}};
          *remaining -= amount;
          return budget.consume({amount});
        };
        auto centers = Footprint::from_regions(
            std::vector<std::uint64_t>(output_shape.begin(),
                                       output_shape.end()),
            {Region(std::move(dims))}, limits);
        if (!centers.ok())
          return centers.status();
        auto expanded = neighborhood_internal::expand(budget, centers.value(),
                                                      radii, periodic, limits);
        if (!expanded.ok())
          return expanded.status();
        for (const auto& box : expanded.value().boxes()) {
          std::array<std::uint64_t, 8> at{};
          for (std::size_t axis = 0; axis < output_shape.size(); ++axis)
            at[axis] = box.dimensions()[axis].offset;
          const auto last = output_shape.size() - 1;
          for (;;) {
            auto status = tick();
            if (!status.ok())
              return status;
            std::uint64_t first = 0;
            for (std::size_t axis = 0; axis < output_shape.size(); ++axis) {
              if (!core_internal::can_multiply_add(first, output_shape[axis],
                                                   at[axis]))
                return {ErrorCode::ResourceExhausted,
                        "neighborhood support cannot be flattened"};
              first = first * output_shape[axis] + at[axis];
            }
            auto span = support;
            span.first = first;
            span.count = box.dimensions()[last].extent;
            if (!core_internal::can_add(span.count, first))
              return {ErrorCode::ResourceExhausted, {}};
            status = visitor(span);
            if (!status.ok())
              return status;
            std::size_t axis = last;
            while (axis) {
              --axis;
              const auto d = box.dimensions()[axis];
              if (++at[axis] < d.offset + d.extent)
                break;
              at[axis] = d.offset;
            }
            if (!axis && (!last || at[0] == box.dimensions()[0].offset))
              break;
          }
        }
        return Status::success();
      }
      case Kind::Mapped: {
        std::array<std::uint64_t, 8> output_at{}, at{}, begin{}, extent{},
            stride{};
        auto index = output;
        for (std::size_t axis = output_shape.size(); axis-- > 0;) {
          output_at[axis] = index % output_shape[axis];
          index /= output_shape[axis];
          const auto d = mapped_outputs.dimensions()[axis];
          if (output_at[axis] < d.offset ||
              output_at[axis] - d.offset >= d.extent)
            return Status{ErrorCode::NotFound, "sample outside mapped witness"};
        }
        std::uint64_t contiguous = 1;
        auto prefix = input_shape.size();
        for (std::size_t axis = input_shape.size(); axis-- > 0;) {
          const auto m = mapping[axis];
          auto start = m.source_coordinate(
              m.output_axis < 0 ? 0 : output_at[m.output_axis]);
          if (!start.ok())
            return start.status();
          begin[axis] = start.value();
          extent[axis] = std::min(m.extent, input_shape[axis] - begin[axis]);
          at[axis] = begin[axis];
          if (axis + 1 == input_shape.size()) {
            stride[axis] = 1;
          } else {
            if (!core_internal::can_multiply(stride[axis + 1],
                                             input_shape[axis + 1]))
              return {ErrorCode::ResourceExhausted,
                      "tensor support cannot be flattened"};
            stride[axis] = stride[axis + 1] * input_shape[axis + 1];
          }
        }
        for (std::size_t axis = input_shape.size(); axis-- > 0;) {
          if (!core_internal::can_multiply(contiguous, extent[axis]))
            return {ErrorCode::ResourceExhausted,
                    "tensor support count cannot be flattened"};
          contiguous *= extent[axis];
          prefix = axis;
          if (begin[axis] || extent[axis] != input_shape[axis])
            break;
        }
        for (;;) {
          status = tick();
          if (!status.ok())
            return status;
          std::uint64_t first = 0;
          for (std::size_t axis = 0; axis < input_shape.size(); ++axis) {
            if (!core_internal::can_multiply_add(at[axis], stride[axis], first))
              return {ErrorCode::ResourceExhausted,
                      "tensor support position cannot be flattened"};
            first += at[axis] * stride[axis];
          }
          if (!core_internal::can_add(contiguous, first))
            return {ErrorCode::ResourceExhausted,
                    "tensor support span cannot be flattened"};
          auto span = support;
          span.first = first;
          span.count = contiguous;
          status = visitor(span);
          if (!status.ok())
            return status;
          std::size_t axis = prefix;
          while (axis) {
            --axis;
            if (++at[axis] < begin[axis] + extent[axis])
              break;
            at[axis] = begin[axis];
          }
          if (!axis && (prefix == 0 || at[0] == begin[0]))
            return Status::success();
        }
      }
      case Kind::Samples: {
        auto first = std::lower_bound(
            samples.begin(), samples.end(), output,
            [](const auto& row, auto index) { return row.output < index; });
        if (first == samples.end() || first->output != output)
          return Status{ErrorCode::NotFound,
                        "missing Result observation witness"};
        for (; first != samples.end() && first->output == output; ++first) {
          status = tick();
          if (!status.ok())
            return status;
          if (first->support.count) {
            status = visitor(first->support);
            if (!status.ok())
              return status;
          }
        }
        return Status::success();
      }
      case Kind::Rows:
        for (std::uint64_t i = 0; i < row_count; ++i) {
          status = tick();
          if (!status.ok())
            return status;
          auto page =
              rows.read(i * sizeof(ResultRelationRow),
                        sizeof(ResultRelationRow), sizeof(ResultRelationRow));
          if (!page.ok())
            return page.status();
          ResultRelationRow row;
          std::memcpy(&row, page.value()->bytes().data(), sizeof(row));
          if (row.output == output && row.support.count) {
            status = visitor(row.support);
            if (!status.ok())
              return status;
          }
        }
        return Status::success();
      case Kind::Union: {
        bool resolved = false;
        for (std::uint32_t i = 0; i < children_count; ++i) {
          bool child_outside = false;
          status = children[i]->walk(output, remaining, visitor, declared,
                                     &child_outside);
          if (status.code == ErrorCode::NotFound &&
              (children[i]->guarantee != DependencyGuarantee::Unknown ||
               child_outside))
            continue;
          if (!status.ok())
            return status;
          resolved = true;
        }
        if (resolved)
          return Status::success();
        if (outside)
          *outside = true;
        return {ErrorCode::NotFound, "missing union observation witness"};
      }
      case Kind::Restricted: {
        if (!inside_mask(output)) {
          if (outside)
            *outside = true;
          return {ErrorCode::NotFound, "sample outside restricted witness"};
        }
        return children[0]->walk(output, remaining, visitor, declared, outside);
      }
      case Kind::Compose:
        return children[0]->walk(
            output, remaining,
            [&](ResultSupport middle) {
              for (std::uint64_t i = 0; i < middle.count; ++i) {
                auto walked = children[1]->walk(
                    middle.first + i, remaining,
                    [&](ResultSupport leaf) {
                      leaf.roles |= middle.roles;
                      return visitor(leaf);
                    },
                    declared);
                if (!walked.ok())
                  return walked;
              }
              return Status::success();
            },
            declared, outside);
    }
    return invalid_relation();
  }
};
Result<std::uint64_t> ResultRelation::cache_metadata(
    ResourceVector<const void*>& owners, std::uint64_t maximum,
    const std::function<Status(std::uint64_t)>& work) const try {
  if (!impl_)
    return Result<std::uint64_t>(0);
  ResourceAllocationScope scope(impl_->budget);
  ResourceVector<const Impl*> pending{impl_.get()};
  std::uint64_t total = 0;
  while (!pending.empty()) {
    auto charged = work(1 + owners.size());
    if (!charged.ok())
      return Result<std::uint64_t>(charged);
    auto* node = pending.back();
    pending.pop_back();
    if (std::find(owners.begin(), owners.end(), node) != owners.end())
      continue;
    std::uint64_t count = 1;
    const auto add = [&](std::uint64_t n) {
      return core_internal::checked_add(count, n, &count, maximum);
    };
    if (!core_internal::can_multiply(node->row_count, 7, maximum) ||
        !core_internal::can_multiply(node->samples.capacity(), 7, maximum) ||
        !core_internal::can_multiply(node->mapping.capacity(), 5, maximum) ||
        !add(node->output_shape.capacity()) ||
        !add(node->input_shape.capacity()) || !add(node->radii.capacity()) ||
        !add(node->mapping.capacity() * 5) ||
        !add(2 * node->mapped_outputs.rank()) ||
        !add(node->restricted_outputs.boxes().size() *
             (1 + 2 * node->output_shape.size())) ||
        !add(2 * node->reshape_source.rank()) ||
        !add(node->samples.capacity() * 7) || !add(node->row_count * 7) ||
        !add(node->children_count) ||
        (node->gather &&
         (!add(node->gather->output_shape.capacity()) ||
          !add(node->gather->input_shape.capacity()) ||
          !add(node->gather->axes.capacity() * 5) ||
          !add(node->gather->outputs.boxes().size() *
               (1 + 2 * node->output_shape.size())) ||
          !add(node->gather->first.capacity()) ||
          !add(node->gather->coordinates.capacity()) ||
          !add(node->gather->present.capacity()) ||
          !add(node->gather->fixed_coordinates.size() +
               node->gather->key_width.size() + node->gather->key_shift.size() +
               node->gather->coordinate_width.size() +
               node->gather->coordinate_shift.size() +
               node->gather->lower.size() + node->gather->upper.size() + 12) ||
          !add(node->gather->full_support.boxes().size() *
               (1 + 2 * node->input_shape.size())) ||
          !add(node->gather->full_validation.boxes().size() *
               (1 + 2 * node->input_shape.size())))) ||
        !core_internal::can_add(total, count, maximum))
      return Result<std::uint64_t>(Status{ErrorCode::ResourceExhausted, {}});
    charged = work(count);
    if (!charged.ok())
      return Result<std::uint64_t>(charged);
    total += count;
    owners.push_back(node);
    for (std::uint32_t i = 0; i < node->children_count; ++i)
      pending.push_back(node->children[i].get());
  }
  return Result<std::uint64_t>(total);
} catch (const std::bad_alloc&) {
  return Result<std::uint64_t>(Status{ErrorCode::ResourceExhausted, {}});
}
bool ResultRelation::owned_by(const ResourceBudget& budget) const noexcept {
  return impl_ && impl_->budget.same_owner(budget);
}
DependencyGuarantee ResultRelation::guarantee() const noexcept {
  return impl_ ? impl_->guarantee : DependencyGuarantee::Unknown;
}
std::uint64_t ResultRelation::coverage() const noexcept {
  return impl_ ? impl_->outputs : 0;
}
Result<ResultRelation> ResultRelation::restrict_to(
    const std::vector<std::uint64_t>& shape, const Region& region) const try {
  using Answer = Result<ResultRelation>;
  if (!impl_ || shape.empty() || shape.size() > 8 ||
      !region.validate(shape).ok())
    return Answer(invalid_relation());
  std::uint64_t count = 1;
  for (auto n : shape) {
    if (!n)
      return Answer(invalid_relation());
    count = core_internal::saturating_multiply(count, n);
  }
  if (count != coverage())
    return Answer(invalid_relation());
  auto valid_shape = impl_->validate_output_shape(shape);
  if (!valid_shape.ok())
    return Answer(valid_shape);
  bool whole = true;
  for (std::size_t axis = 0; axis < shape.size(); ++axis)
    whole &= !region.dimensions()[axis].offset &&
             region.dimensions()[axis].extent == shape[axis];
  if (whole)
    return Answer(*this);
  if (impl_->kind == Impl::Kind::Mapped || impl_->kind == Impl::Kind::Gather ||
      impl_->kind == Impl::Kind::Reshape ||
      impl_->kind == Impl::Kind::Neighborhood ||
      (impl_->kind == Impl::Kind::Restricted &&
       !impl_->restricted_outputs.valid())) {
    if (!std::equal(impl_->output_shape.begin(), impl_->output_shape.end(),
                    shape.begin(), shape.end()))
      return Answer(invalid_relation());
    bool contained = impl_->mapped_outputs.rank() == region.rank();
    for (std::size_t axis = 0; contained && axis < shape.size(); ++axis) {
      const auto old = impl_->mapped_outputs.dimensions()[axis];
      const auto next = region.dimensions()[axis];
      contained = old.offset >= next.offset &&
                  old.offset - next.offset <= next.extent &&
                  old.extent <= next.extent - (old.offset - next.offset);
    }
    if (contained)
      return Answer(*this);
  }
  if (impl_->depth >= 32)
    return Answer(
        Status{ErrorCode::ResourceExhausted, "Result relation depth limit"});
  auto made = Impl::make(impl_->budget, coverage());
  if (!made.ok())
    return Answer(made.status());
  auto node = made.take_value();
  node->output_shape = ResourceVector<std::uint64_t>(
      shape.begin(), shape.end(),
      ResourceAllocator<std::uint64_t>(impl_->budget));
  auto region_lease = impl_->budget.reserve(
      ResourceCapacity::host(region.rank() * sizeof(RegionDimension),
                             region.rank() * sizeof(RegionDimension)));
  if (!region_lease.ok())
    return Answer(region_lease.status());
  node->mapped_region_lease = region_lease.take_value();
  node->mapped_outputs = region;
  node->kind = Impl::Kind::Restricted;
  node->guarantee = guarantee();
  node->depth = impl_->depth + 1;
  node->children[0] = impl_;
  node->children_count = 1;
  ResultRelation result;
  result.impl_ = std::move(node);
  return Answer(std::move(result));
} catch (const std::bad_alloc&) {
  return Result<ResultRelation>(Status{ErrorCode::ResourceExhausted, {}});
}
Result<ResultRelation> ResultRelation::restrict_to(
    const Footprint& outputs, const FootprintLimits& limits) const try {
  using Answer = Result<ResultRelation>;
  if (!impl_ || !outputs.valid())
    return Answer(invalid_relation());
  if (limits.cancellation.cancelled())
    return Answer(Status{ErrorCode::Cancelled, {}});
  if (outputs.boxes().size() > limits.maximum_boxes)
    return Answer(
        Status{ErrorCode::ResourceExhausted, "relation mask box limit"});
  if (outputs.boxes().size() == 1)
    return restrict_to(outputs.shape(), outputs.boxes()[0]);
  auto checked = restrict_to(outputs.shape(), Region::whole(outputs.shape()));
  if (!checked.ok())
    return checked;
  ResourceAllocationScope scope(impl_->budget);
  if (impl_->gather && impl_->gather->outputs == outputs)
    return Answer(*this);
  if (impl_->restricted_outputs.valid() && impl_->restricted_outputs == outputs)
    return Answer(*this);
  if (impl_->depth >= 32)
    return Answer(
        Status{ErrorCode::ResourceExhausted, "Result relation depth limit"});
  auto retained =
      Footprint::from_regions(outputs.shape(), outputs.boxes(), limits);
  if (!retained.ok())
    return Answer(retained.status());
  auto made = Impl::make(impl_->budget, coverage());
  if (!made.ok())
    return Answer(made.status());
  auto node = made.take_value();
  node->output_shape = ResourceVector<std::uint64_t>(
      outputs.shape().begin(), outputs.shape().end(),
      ResourceAllocator<std::uint64_t>(impl_->budget));
  node->restricted_outputs = retained.take_value();
  node->kind = Impl::Kind::Restricted;
  node->guarantee = guarantee();
  node->depth = impl_->depth + 1;
  node->children[0] = impl_;
  node->children_count = 1;
  ResultRelation result;
  result.impl_ = std::move(node);
  return Answer(std::move(result));
} catch (const std::bad_alloc&) {
  return Result<ResultRelation>(Status{ErrorCode::ResourceExhausted, {}});
}
Result<ResultRelation> ResultRelation::cartesian(
    ResourceBudget budget, std::uint64_t outputs, ResultSupport support,
    DependencyGuarantee guarantee) {
  if (!valid_support(support) || !valid_guarantee(guarantee))
    return Result<ResultRelation>(invalid_relation());
  auto made = Impl::make(std::move(budget), outputs);
  if (!made.ok())
    return Result<ResultRelation>(made.status());
  auto impl = made.take_value();
  impl->kind = Impl::Kind::Cartesian;
  impl->guarantee = guarantee;
  impl->support = support;
  ResultRelation relation;
  relation.impl_ = std::move(impl);
  return Result<ResultRelation>(std::move(relation));
}
Result<ResultRelation> ResultRelation::mapped(
    ResourceBudget budget, const std::vector<std::uint64_t>& output_shape,
    const Region& outputs, const std::vector<std::uint64_t>& input_shape,
    const std::vector<ResultMappedAxis>& axes, ResultSupport support) try {
  using Answer = Result<ResultRelation>;
  if (output_shape.empty() || output_shape.size() > 8 || input_shape.empty() ||
      input_shape.size() > 8 || axes.size() != input_shape.size() ||
      outputs.empty() || !outputs.validate(output_shape).ok() ||
      !valid_support(support))
    return Answer(invalid_relation());
  ResourceAllocationScope scope(budget);
  std::uint64_t output_count = 1;
  for (auto n : output_shape) {
    if (!n)
      return Answer(invalid_relation());
    output_count = core_internal::saturating_multiply(output_count, n);
  }
  for (auto n : input_shape)
    if (!n)
      return Answer(invalid_relation());
  for (std::size_t i = 0; i < axes.size(); ++i) {
    const auto m = axes[i];
    if (m.output_axis < -1 ||
        m.output_axis >= static_cast<std::int32_t>(output_shape.size()) ||
        !m.extent)
      return Answer(invalid_relation());
    const auto d = m.output_axis < 0 ? RegionDimension{0, 1}
                                     : outputs.dimensions()[m.output_axis];
    const auto first = m.source_coordinate(d.offset);
    const auto last = m.source_coordinate(d.offset + d.extent - 1);
    if (!first.ok() || !last.ok() || first.value() >= input_shape[i] ||
        last.value() >= input_shape[i])
      return Answer(invalid_relation());
  }
  auto made = Impl::make(budget, output_count);
  if (!made.ok())
    return Answer(made.status());
  auto impl = made.take_value();
  impl->output_shape =
      ResourceVector<std::uint64_t>(output_shape.begin(), output_shape.end(),
                                    ResourceAllocator<std::uint64_t>(budget));
  impl->input_shape =
      ResourceVector<std::uint64_t>(input_shape.begin(), input_shape.end(),
                                    ResourceAllocator<std::uint64_t>(budget));
  impl->mapping = ResourceVector<ResultMappedAxis>(
      axes.begin(), axes.end(), ResourceAllocator<ResultMappedAxis>(budget));
  auto admitted = budget.reserve(
      ResourceCapacity::host(outputs.rank() * sizeof(RegionDimension),
                             outputs.rank() * sizeof(RegionDimension)));
  if (!admitted.ok())
    return Answer(admitted.status());
  // Region coordinates are accounted with the fixed witness metadata lease.
  impl->mapped_region_lease = admitted.take_value();
  impl->mapped_outputs = outputs;
  impl->support = support;
  impl->kind = Impl::Kind::Mapped;
  impl->guarantee = DependencyGuarantee::Exact;
  ResultRelation result;
  result.impl_ = std::move(impl);
  return Answer(std::move(result));
} catch (const std::bad_alloc&) {
  return Result<ResultRelation>(Status{ErrorCode::ResourceExhausted, {}});
}
Result<ResultRelation> ResultRelation::gather(
    ResourceBudget budget, const std::vector<std::uint64_t>& output_shape,
    const Region& outputs, std::uint32_t output_tuple_axes,
    const std::vector<std::uint64_t>& input_shape,
    const std::vector<ResultMappedAxis>& axes, std::uint32_t indexed_axes,
    std::uint32_t samples_per_tuple,
    const std::function<Result<ResultGatherSample>(std::uint64_t,
                                                   std::uint32_t)>& reader,
    ResultSupport support, std::uint32_t validation_trailing_axes,
    const FootprintLimits& limits) {
  ResourceAllocationScope scope(budget);
  auto requested = Footprint::from_regions(output_shape, {outputs}, limits);
  if (!requested.ok())
    return Result<ResultRelation>(requested.status());
  return gather(std::move(budget), output_shape, requested.value(),
                output_tuple_axes, input_shape, axes, indexed_axes,
                samples_per_tuple, reader, support, validation_trailing_axes,
                limits);
}
Result<ResultRelation> ResultRelation::gather(
    ResourceBudget budget, const std::vector<std::uint64_t>& output_shape,
    const Footprint& outputs, std::uint32_t output_tuple_axes,
    const std::vector<std::uint64_t>& input_shape,
    const std::vector<ResultMappedAxis>& axes, std::uint32_t indexed_axes,
    std::uint32_t samples_per_tuple,
    const std::function<Result<ResultGatherSample>(std::uint64_t,
                                                   std::uint32_t)>& reader,
    ResultSupport support, std::uint32_t validation_trailing_axes,
    const FootprintLimits& bounds) try {
  using Answer = Result<ResultRelation>;
  if (!reader || !outputs.valid() || outputs.empty() ||
      outputs.shape() != output_shape || !output_tuple_axes ||
      output_tuple_axes > output_shape.size() || !samples_per_tuple ||
      samples_per_tuple > 64 || input_shape.size() > 8 ||
      indexed_axes >> input_shape.size() ||
      validation_trailing_axes > input_shape.size() ||
      support.target != ResultSupportTarget::Tensor || (support.roles & ~7U) ||
      support.first || support.count)
    return Answer(invalid_relation());
  ResourceAllocationScope scope(budget);
  auto admitted = budget.reserve(ResourceCapacity::host(
      sizeof(gather_internal::Table), sizeof(gather_internal::Table)));
  if (!admitted.ok())
    return Answer(admitted.status());
  auto table = std::make_shared<gather_internal::Table>(budget);
  table->lease = admitted.take_value();
  gather_internal::Table::QueryWork work(*table, bounds);
  const auto& limits = work.limits;
  auto charged =
      table->charge(outputs.boxes().size() * (output_shape.size() + 1), limits);
  if (!charged.ok())
    return Answer(charged);
  if (outputs.boxes().size() > limits.maximum_boxes)
    return Answer(Status{ErrorCode::ResourceExhausted, "gather box limit"});
  const auto prefix = output_shape.size() - output_tuple_axes;
  auto bounding = outputs.boxes()[0].dimensions();
  table->first =
      ResourceVector<std::uint64_t>(ResourceAllocator<std::uint64_t>(budget));
  table->first.reserve(outputs.boxes().size());
  std::uint64_t rows = 0;
  for (const auto& box : outputs.boxes()) {
    table->first.push_back(rows);
    std::uint64_t count = 1;
    for (std::size_t axis = 0; axis < output_shape.size(); ++axis) {
      const auto d = box.dimensions()[axis];
      if (axis >= prefix) {
        if (d.offset || d.extent != output_shape[axis])
          return Answer(invalid_relation());
      } else {
        if (!core_internal::can_multiply(count, d.extent))
          return Answer(
              Status{ErrorCode::ResourceExhausted, "gather table size"});
        count *= d.extent;
      }
      const auto last = std::max(bounding[axis].offset + bounding[axis].extent,
                                 d.offset + d.extent);
      bounding[axis].offset = std::min(bounding[axis].offset, d.offset);
      bounding[axis].extent = last - bounding[axis].offset;
    }
    if (!core_internal::can_add(count, rows))
      return Answer(Status{ErrorCode::ResourceExhausted, "gather table size"});
    rows += count;
  }
  const Region bounding_region(std::move(bounding));
  auto made =
      mapped(budget, output_shape, bounding_region, input_shape, axes, support);
  if (!made.ok())
    return made;
  std::uint32_t columns = 0;
  for (std::size_t axis = 0; axis < axes.size(); ++axis) {
    const auto m = axes[axis];
    if (indexed_axes & (1U << axis)) {
      if (m.output_axis >= 0 && m.step)
        return Answer(invalid_relation());
      ++columns;
    } else if (m.output_axis >= static_cast<std::int32_t>(prefix) && m.step) {
      return Answer(invalid_relation());
    }
    if (!(indexed_axes & (1U << axis)) &&
        (m.output_axis < 0 || !m.step ||
         bounding_region.dimensions()[m.output_axis].extent == 1)) {
      table->fixed_axes |= 1U << axis;
      table->fixed_coordinates[axis] =
          m.source_coordinate(
               m.output_axis < 0
                   ? 0
                   : bounding_region.dimensions()[m.output_axis].offset)
              .value();
    }
    if (!(indexed_axes & (1U << axis))) {
      const auto d = m.output_axis < 0
                         ? RegionDimension{0, 1}
                         : bounding_region.dimensions()[m.output_axis];
      const auto first = m.source_coordinate(d.offset).value();
      const auto last = m.source_coordinate(d.offset + d.extent - 1).value();
      table->lower[axis] = std::min(first, last);
      table->upper[axis] = std::max(first, last);
    }
  }
  if (!core_internal::can_multiply(rows, samples_per_tuple))
    return Answer(Status{ErrorCode::ResourceExhausted, "gather table size"});
  // Validate, bound and pack each indexed field. Tape clearing is charged by
  // allocate() before allocation and runs in cancellation-bounded chunks.
  const auto unit =
      1 + samples_per_tuple * (input_shape.size() + 5 * columns + 4);
  if (!core_internal::can_multiply(rows, unit, work.remaining))
    return Answer(
        Status{ErrorCode::ResourceExhausted, "gather table work limit"});
  table->output_shape =
      ResourceVector<std::uint64_t>(output_shape.begin(), output_shape.end(),
                                    ResourceAllocator<std::uint64_t>(budget));
  table->input_shape =
      ResourceVector<std::uint64_t>(input_shape.begin(), input_shape.end(),
                                    ResourceAllocator<std::uint64_t>(budget));
  table->axes = ResourceVector<ResultMappedAxis>(
      axes.begin(), axes.end(), ResourceAllocator<ResultMappedAxis>(budget));
  auto retained =
      Footprint::from_regions(output_shape, outputs.boxes(), limits);
  if (!retained.ok())
    return Answer(retained.status());
  table->outputs = retained.take_value();
  table->tuple_axes = output_tuple_axes;
  table->indexed_axes = indexed_axes;
  table->columns = columns;
  table->taps = samples_per_tuple;
  table->validation_axes = validation_trailing_axes;
  table->prepare_keys();
  charged = table->allocate(rows, limits);
  if (!charged.ok())
    return Answer(charged);
  for (std::uint64_t row = 0; row < rows; ++row) {
    if (!(row & 255U)) {
      charged = table->charge(std::min<std::uint64_t>(256, rows - row) * unit,
                              limits);
      if (!charged.ok())
        return Answer(charged);
    }
    for (std::uint32_t tap = 0; tap < samples_per_tuple; ++tap) {
      auto sample = reader(row, tap);
      if (!sample.ok())
        return Answer(sample.status());
      if (!sample.value().present)
        continue;
      for (std::size_t axis = 0; axis < input_shape.size(); ++axis) {
        if (!(indexed_axes & (1U << axis)))
          continue;
        const auto coordinate = sample.value().coordinates[axis];
        if (coordinate >= input_shape[axis])
          return Answer(invalid_relation());
      }
      table->store(row, tap, sample.value().coordinates);
    }
  }
  auto projected = table->project(table->outputs, limits);
  if (!projected.ok())
    return Answer(projected.status());
  table->full_support = projected.take_value();
  auto validation = table->close(table->full_support, limits);
  if (!validation.ok())
    return Answer(validation.status());
  table->full_validation = validation.take_value();
  auto result = made.take_value();
  // mapped() just allocated this node, which has not escaped this constructor.
  auto impl = std::const_pointer_cast<Impl>(result.impl_);
  impl->kind = Impl::Kind::Gather;
  impl->gather = std::move(table);
  return Answer(std::move(result));
} catch (const std::bad_alloc&) {
  return Result<ResultRelation>(Status{ErrorCode::ResourceExhausted, {}});
}
Result<ResultRelation> ResultRelation::neighborhood(
    ResourceBudget budget, const std::vector<std::uint64_t>& shape,
    const std::vector<std::uint64_t>& radii, bool periodic,
    ResultSupport support) try {
  using Answer = Result<ResultRelation>;
  if (shape.empty() || shape.size() > 8 || radii.size() != shape.size() ||
      !valid_support(support) || support.first || support.count ||
      support.target != ResultSupportTarget::Tensor || (support.roles & 8U))
    return Answer(invalid_relation());
  std::uint64_t count = 1;
  for (auto extent : shape) {
    if (!extent)
      return Answer(invalid_relation());
    count = core_internal::saturating_multiply(count, extent);
  }
  ResourceAllocationScope scope(budget);
  auto made = Impl::make(budget, count);
  if (!made.ok())
    return Answer(made.status());
  auto impl = made.take_value();
  impl->output_shape = ResourceVector<std::uint64_t>(
      shape.begin(), shape.end(), ResourceAllocator<std::uint64_t>(budget));
  impl->input_shape = impl->output_shape;
  impl->radii = ResourceVector<std::uint64_t>(
      radii.begin(), radii.end(), ResourceAllocator<std::uint64_t>(budget));
  auto lease = budget.reserve(
      ResourceCapacity::host(shape.size() * sizeof(RegionDimension),
                             shape.size() * sizeof(RegionDimension)));
  if (!lease.ok())
    return Answer(lease.status());
  impl->mapped_region_lease = lease.take_value();
  impl->mapped_outputs = Region::whole(shape);
  impl->kind = Impl::Kind::Neighborhood;
  impl->periodic = periodic;
  impl->support = support;
  impl->guarantee = DependencyGuarantee::Exact;
  ResultRelation result;
  result.impl_ = std::move(impl);
  return Answer(std::move(result));
} catch (const std::bad_alloc&) {
  return Result<ResultRelation>(Status{ErrorCode::ResourceExhausted, {}});
}
Result<ResultRelation> ResultRelation::reshape(
    ResourceBudget budget, const std::vector<uint64_t>& output_shape,
    const Region& outputs, const std::vector<uint64_t>& input_shape,
    const Region& source_window, ResultSupport support) try {
  using Answer = Result<ResultRelation>;
  if (output_shape.empty() || output_shape.size() > 8 || input_shape.empty() ||
      input_shape.size() > 8 || !outputs.validate(output_shape).ok() ||
      source_window.empty() || !source_window.validate(input_shape).ok() ||
      !valid_support(support) ||
      support.target != ResultSupportTarget::Tensor || support.first ||
      support.count)
    return Answer(invalid_relation());
  ResourceAllocationScope scope(budget);
  std::vector<uint64_t> extents;
  auto temporary = budget.reserve(
      ResourceCapacity::host(8 * sizeof(uint64_t), 8 * sizeof(uint64_t)));
  if (!temporary.ok())
    return Answer(temporary.status());
  extents.reserve(8);
  for (auto d : source_window.dimensions())
    extents.push_back(d.extent);
  if (!reshape_internal::equal_products(output_shape, extents))
    return Answer(invalid_relation());
  uint64_t count = 1;
  for (auto n : output_shape)
    count = core_internal::saturating_multiply(count, n);
  auto made = Impl::make(budget, count);
  if (!made.ok())
    return Answer(made.status());
  auto impl = made.take_value();
  impl->output_shape =
      ResourceVector<uint64_t>(output_shape.begin(), output_shape.end(),
                               ResourceAllocator<uint64_t>(budget));
  impl->input_shape =
      ResourceVector<uint64_t>(input_shape.begin(), input_shape.end(),
                               ResourceAllocator<uint64_t>(budget));
  const auto bytes =
      (outputs.rank() + source_window.rank()) * sizeof(RegionDimension);
  auto admitted = budget.reserve(ResourceCapacity::host(bytes, bytes));
  if (!admitted.ok())
    return Answer(admitted.status());
  impl->mapped_region_lease = admitted.take_value();
  impl->mapped_outputs = outputs;
  impl->reshape_source = source_window;
  impl->support = support;
  impl->kind = Impl::Kind::Reshape;
  impl->guarantee = DependencyGuarantee::Exact;
  ResultRelation relation;
  relation.impl_ = std::move(impl);
  return Answer(std::move(relation));
} catch (const std::bad_alloc&) {
  return Result<ResultRelation>(Status{ErrorCode::ResourceExhausted, {}});
}
namespace {
Result<Footprint> clip_projection(const ResourceBudget& budget,
                                  const Footprint& outputs, const Region& mask,
                                  const FootprintLimits& limits) {
  const auto cost = outputs.boxes().size() * outputs.shape().size() + 1;
  if (limits.cancellation.cancelled())
    return Result<Footprint>(Status{ErrorCode::Cancelled, {}});
  if (cost > limits.maximum_work)
    return Result<Footprint>(Status{ErrorCode::ResourceExhausted, {}});
  auto charged =
      limits.consume_work ? limits.consume_work(cost) : budget.consume({cost});
  if (!charged.ok())
    return Result<Footprint>(charged);
  if (limits.cancellation.cancelled())
    return Result<Footprint>(Status{ErrorCode::Cancelled, {}});
  auto valid = mask.validate(outputs.shape());
  if (!valid.ok())
    return Result<Footprint>(valid);
  bool contained = true;
  for (const auto& box : outputs.boxes()) {
    for (std::size_t axis = 0; axis < outputs.shape().size(); ++axis) {
      const auto a = box.dimensions()[axis], b = mask.dimensions()[axis];
      if (a.offset < b.offset || a.extent > b.extent ||
          a.offset - b.offset > b.extent - a.extent) {
        contained = false;
        break;
      }
    }
    if (!contained)
      break;
  }
  if (contained)
    return Result<Footprint>(outputs);
  if (!core_internal::can_add(cost, cost, limits.maximum_work))
    return Result<Footprint>(Status{ErrorCode::ResourceExhausted, {}});
  // Intersect canonical rectangles directly with one rectangular mask instead
  // of sweeping both sets on every axis. Normalize the clipped pieces once.
  charged =
      limits.consume_work ? limits.consume_work(cost) : budget.consume({cost});
  if (!charged.ok())
    return Result<Footprint>(charged);
  const auto bytes =
      outputs.boxes().size() *
      (sizeof(Region) + outputs.shape().size() * sizeof(RegionDimension));
  auto admitted = budget.reserve(ResourceCapacity::host(bytes, bytes));
  if (!admitted.ok())
    return Result<Footprint>(admitted.status());
  auto lease = admitted.take_value();
  std::vector<Region> clipped;
  clipped.reserve(outputs.boxes().size());
  for (const auto& box : outputs.boxes()) {
    auto dimensions = box.dimensions();
    bool empty = false;
    for (std::size_t axis = 0; axis < dimensions.size(); ++axis) {
      const auto a = dimensions[axis], b = mask.dimensions()[axis];
      const auto first = std::max(a.offset, b.offset);
      const auto end = std::min(a.offset + a.extent, b.offset + b.extent);
      if (first >= end) {
        empty = true;
        break;
      }
      dimensions[axis] = {first, end - first};
    }
    if (!empty)
      clipped.emplace_back(std::move(dimensions));
  }
  return Footprint::from_regions(outputs.shape(), clipped, limits);
}
}  // namespace
Result<Footprint> ResultRelation::Impl::clip(
    const Footprint& requested, const FootprintLimits& limits) const {
  if (gather || restricted_outputs.valid()) {
    auto available = mask(limits);
    if (!available.ok())
      return available;
    auto remaining = limits;
    if (requested.shape() == available.value().shape() &&
        requested.boxes().size() == available.value().boxes().size()) {
      // Full queries recur during publication, closure and bundle capture.
      // Compare immutable canonical boxes before rebuilding their intersection;
      // equal cardinality alone must never authorize a different sparse set.
      const auto rank = requested.shape().size();
      if (!core_internal::can_multiply_add(requested.boxes().size(), rank, 1))
        return Result<Footprint>(Status{ErrorCode::ResourceExhausted, {}});
      const auto cost = requested.boxes().size() * rank + 1;
      if (cost > remaining.maximum_work)
        return Result<Footprint>(Status{ErrorCode::ResourceExhausted, {}});
      auto charged = remaining.consume_work ? remaining.consume_work(cost)
                                            : budget.consume({cost});
      if (!charged.ok())
        return Result<Footprint>(charged);
      remaining.maximum_work -= cost;
      bool equal = true;
      for (std::size_t i = 0; equal && i < requested.boxes().size(); ++i) {
        if (!(i & 1023U) && remaining.cancellation.cancelled())
          return Result<Footprint>(Status{ErrorCode::Cancelled, {}});
        for (std::size_t axis = 0; axis < rank; ++axis) {
          const auto a = requested.boxes()[i].dimensions()[axis];
          const auto b = available.value().boxes()[i].dimensions()[axis];
          if (a.offset != b.offset || a.extent != b.extent) {
            equal = false;
            break;
          }
        }
      }
      if (remaining.cancellation.cancelled())
        return Result<Footprint>(Status{ErrorCode::Cancelled, {}});
      if (equal)
        return Result<Footprint>(requested);
    }
    return requested.intersect(available.value(), remaining);
  }
  return clip_projection(budget, requested, mapped_outputs, limits);
}
Status ResultRelation::project(
    const Footprint& outputs,
    const std::function<Status(ResultSupport, const Footprint*)>& visitor,
    const FootprintLimits& bounds) const try {
  if (!impl_)
    return invalid_relation();
  auto limits = bounds;
  auto remaining = bounds.maximum_work;
  limits.consume_work = [&](std::uint64_t n) {
    if (bounds.cancellation.cancelled())
      return Status{ErrorCode::Cancelled, {}};
    if (n > remaining)
      return Status{ErrorCode::ResourceExhausted,
                    "relation projection work limit"};
    remaining -= n;
    return bounds.consume_work ? bounds.consume_work(n)
                               : impl_->budget.consume({n});
  };
  return project_impl(outputs, visitor, limits, true);
} catch (const std::bad_alloc&) {
  return Status{ErrorCode::ResourceExhausted, {}};
}
Status ResultRelation::project_impl(
    const Footprint& outputs,
    const std::function<Status(ResultSupport, const Footprint*)>& visitor,
    const FootprintLimits& limits, bool preflight) const try {
  if (!impl_ || !outputs.valid() || !visitor)
    return invalid_relation();
  ResourceAllocationScope scope(impl_->budget);
  ResourceVector<std::shared_ptr<const Impl>> leaves(
      ResourceAllocator<std::shared_ptr<const Impl>>(impl_->budget));
  std::function<Status(std::shared_ptr<const Impl>)> collect = [&](auto node) {
    if (node->kind == Impl::Kind::Restricted) {
      if (!std::equal(node->output_shape.begin(), node->output_shape.end(),
                      outputs.shape().begin(), outputs.shape().end()))
        return invalid_relation();
      if (preflight) {
        const auto before = leaves.size();
        auto status = collect(node->children[0]);
        leaves.resize(before);
        if (!status.ok())
          return status;
      }
      leaves.push_back(node);
    } else if (node->kind == Impl::Kind::Union) {
      for (std::uint32_t i = 0; i < node->children_count; ++i) {
        auto status = collect(node->children[i]);
        if (!status.ok())
          return status;
      }
    } else if (node->kind == Impl::Kind::Cartesian ||
               (node->kind == Impl::Kind::Identity &&
                outputs.shape().size() == 1) ||
               node->kind == Impl::Kind::Mapped ||
               node->kind == Impl::Kind::Gather ||
               node->kind == Impl::Kind::Reshape ||
               node->kind == Impl::Kind::Neighborhood ||
               node->kind == Impl::Kind::Prefix) {
      if (node->kind == Impl::Kind::Mapped ||
          node->kind == Impl::Kind::Gather ||
          node->kind == Impl::Kind::Reshape ||
          node->kind == Impl::Kind::Neighborhood ||
          node->kind == Impl::Kind::Prefix) {
        if (!std::equal(node->output_shape.begin(), node->output_shape.end(),
                        outputs.shape().begin(), outputs.shape().end()))
          return invalid_relation();
        std::array<bool, 8> used{};
        for (std::size_t i = 0; i < node->mapping.size(); ++i) {
          if (node->gather)
            continue;
          const auto axis = node->mapping[i];
          if (axis.output_axis >= 0 && axis.step) {
            if (step_magnitude(axis.step) > axis.extent ||
                used[axis.output_axis])
              return Status{ErrorCode::NotFound,
                            "scalar relation projection required"};
            used[axis.output_axis] = true;
          }
        }
      }
      leaves.push_back(node);
    } else {
      return Status{ErrorCode::NotFound, "scalar relation projection required"};
    }
    return Status::success();
  };
  auto checked = collect(impl_);
  if (!checked.ok())
    return checked;
  for (const auto& node : leaves) {
    auto work = limits.consume_work ? limits.consume_work(1)
                                    : node->budget.consume({1});
    if (!work.ok())
      return work;
    if (limits.cancellation.cancelled())
      return Status{ErrorCode::Cancelled, {}};
    if (node->kind == Impl::Kind::Restricted) {
      auto clipped = node->clip(outputs, limits);
      if (!clipped.ok())
        return clipped.status();
      if (clipped.value().empty())
        continue;
      ResultRelation child;
      child.impl_ = node->children[0];
      auto status = child.project_impl(clipped.value(), visitor, limits, false);
      if (!status.ok())
        return status;
      continue;
    }
    if (node->kind == Impl::Kind::Cartesian) {
      if (node->support.count && !outputs.empty()) {
        auto status = visitor(node->support, nullptr);
        if (!status.ok())
          return status;
      }
      continue;
    }
    if (node->kind == Impl::Kind::Identity) {
      if (outputs.boxes().size() > limits.maximum_work)
        return {ErrorCode::ResourceExhausted, "identity projection work limit"};
      for (const auto& box : outputs.boxes()) {
        auto charged = node->budget.consume({1});
        if (!charged.ok())
          return charged;
        if (limits.cancellation.cancelled())
          return {ErrorCode::Cancelled, {}};
        const auto d = box.dimensions()[0];
        if (d.offset > node->outputs || d.extent > node->outputs - d.offset)
          return invalid_relation();
        if (d.extent) {
          auto support = node->support;
          support.first = d.offset;
          support.count = d.extent;
          auto status = visitor(support, nullptr);
          if (!status.ok())
            return status;
        }
      }
      continue;
    }
    if (node->kind == Impl::Kind::Prefix) {
      if (outputs.boxes().size() > limits.maximum_work)
        return {ErrorCode::ResourceExhausted, "prefix projection work limit"};
      std::uint64_t end = 0;
      for (const auto& box : outputs.boxes()) {
        auto charged = node->budget.consume({1});
        if (!charged.ok())
          return charged;
        if (limits.cancellation.cancelled())
          return {ErrorCode::Cancelled, {}};
        const auto d = box.dimensions()[0];
        end = std::max(end, d.offset + d.extent);
      }
      if (end) {
        auto support = node->support;
        support.count = end;
        auto status = visitor(support, nullptr);
        if (!status.ok())
          return status;
      }
      continue;
    }
    auto requested = node->clip(outputs, limits);
    if (!requested.ok())
      return requested.status();
    if (requested.value().empty())
      continue;
    if (node->kind == Impl::Kind::Gather) {
      auto status = node->gather->project(requested.value(), node->support,
                                          visitor, limits);
      if (!status.ok())
        return status;
      continue;
    }
    if (node->kind == Impl::Kind::Neighborhood) {
      auto samples = neighborhood_internal::expand(
          node->budget, requested.value(), node->radii, node->periodic, limits);
      if (!samples.ok())
        return samples.status();
      if (!samples.value().empty()) {
        auto status = visitor(node->support, &samples.value());
        if (!status.ok())
          return status;
      }
      continue;
    }
    if (node->kind == Impl::Kind::Reshape) {
      auto shape_lease = node->budget.reserve(
          ResourceCapacity::host(16 * sizeof(uint64_t), 16 * sizeof(uint64_t)));
      if (!shape_lease.ok())
        return shape_lease.status();
      std::vector<uint64_t> extents;
      extents.reserve(8);
      for (auto d : node->reshape_source.dimensions())
        extents.push_back(d.extent);
      auto projected = reshape_internal::project(
          node->budget, outputs.shape(), requested.value(), extents, limits);
      if (!projected.ok())
        return projected.status();
      std::vector<Region> translated;
      auto lease = node->budget.reserve(ResourceCapacity::host(
          projected.value().boxes().size() *
              (sizeof(Region) + extents.size() * sizeof(RegionDimension)),
          projected.value().boxes().size() *
              (sizeof(Region) + extents.size() * sizeof(RegionDimension))));
      if (!lease.ok())
        return lease.status();
      translated.reserve(projected.value().boxes().size());
      for (const auto& box : projected.value().boxes()) {
        auto dimensions = box.dimensions();
        for (size_t i = 0; i < dimensions.size(); ++i)
          dimensions[i].offset += node->reshape_source.dimensions()[i].offset;
        translated.emplace_back(std::move(dimensions));
      }
      auto samples = Footprint::from_regions(
          std::vector<uint64_t>(node->input_shape.begin(),
                                node->input_shape.end()),
          translated, limits);
      if (!samples.ok())
        return samples.status();
      if (!samples.value().empty()) {
        auto status = visitor(node->support, &samples.value());
        if (!status.ok())
          return status;
      }
      continue;
    }
    std::vector<Region> boxes;
    for (const auto& box : requested.value().boxes()) {
      std::vector<RegionDimension> dimensions;
      for (std::size_t axis = 0; axis < node->mapping.size(); ++axis) {
        const auto m = node->mapping[axis];
        const auto d = m.output_axis < 0 ? RegionDimension{0, 1}
                                         : box.dimensions()[m.output_axis];
        const auto a = m.source_coordinate(d.offset).value();
        const auto b = m.source_coordinate(d.offset + d.extent - 1).value();
        const auto first = std::min(a, b), last = std::max(a, b);
        const auto end =
            last + std::min(m.extent, node->input_shape[axis] - last);
        dimensions.push_back({first, end - first});
      }
      boxes.emplace_back(std::move(dimensions));
    }
    auto samples = Footprint::from_regions(
        std::vector<std::uint64_t>(node->input_shape.begin(),
                                   node->input_shape.end()),
        boxes, limits);
    if (!samples.ok())
      return samples.status();
    if (!samples.value().empty()) {
      auto status = visitor(node->support, &samples.value());
      if (!status.ok())
        return status;
    }
  }
  return Status::success();
} catch (const std::bad_alloc&) {
  return Status{ErrorCode::ResourceExhausted, {}};
}
Status ResultRelation::validate_tuple_closure(
    const Footprint& outputs, std::uint32_t input, std::uint32_t slot,
    const std::vector<std::uint64_t>& shape, std::size_t channel,
    std::uint32_t grouped_axes, const FootprintLimits& limits) const try {
  if (!impl_ || channel >= shape.size() || !outputs.valid())
    return invalid_relation();
  ResourceAllocationScope scope(impl_->budget);
  const auto failure = [] {
    return Status{ErrorCode::InvalidArgument,
                  "Result support omits same-observation tuple Validation",
                  FailureReason::None,
                  {FailureOrigin::Protocol, FailureScope::Group}};
  };
  auto tick = [&](std::uint64_t count) {
    return limits.consume_work ? limits.consume_work(count)
                               : impl_->budget.consume({count});
  };
  struct Node {
    const Impl* value;
    Footprint mask;
  };
  using Nodes = ResourceVector<Node>;
  Nodes pending{ResourceAllocator<Node>(impl_->budget)};
  Nodes leaves{ResourceAllocator<Node>(impl_->budget)};
  pending.push_back({impl_.get(), outputs});
  bool compact = true;
  while (!pending.empty()) {
    auto status = tick(1);
    if (!status.ok())
      return status;
    auto current = std::move(pending.back());
    pending.pop_back();
    auto* node = current.value;
    if (node->kind == Impl::Kind::Restricted ||
        node->kind == Impl::Kind::Mapped || node->kind == Impl::Kind::Gather) {
      auto clipped = node->clip(current.mask, limits);
      if (!clipped.ok())
        return clipped.status();
      current.mask = clipped.take_value();
      if (current.mask.empty())
        continue;
    }
    if (node->kind == Impl::Kind::Gather &&
        (node->support.input != input ||
         node->support.target != ResultSupportTarget::Tensor ||
         node->support.slot != slot || !(node->support.roles & 3U) ||
         (std::equal(node->input_shape.begin(), node->input_shape.end(),
                     shape.begin(), shape.end()) &&
          ((node->gather->validation_axes &&
            node->gather->closed_axis(channel)) ||
           ((node->support.roles & 4U) &&
            !(node->gather->indexed_axes & (1U << channel)) &&
            (node->mapping[channel].output_axis < 0 ||
             !node->mapping[channel].step) &&
            !node->mapping[channel].source_origin &&
            node->mapping[channel].extent >= shape[channel])))))
      continue;
    if (node->kind == Impl::Kind::Union ||
        node->kind == Impl::Kind::Restricted) {
      for (unsigned i = 0; i < node->children_count; ++i)
        pending.push_back({node->children[i].get(), current.mask});
    } else if (node->kind == Impl::Kind::Cartesian ||
               node->kind == Impl::Kind::Mapped) {
      if (node->support.input == input &&
          node->support.target == ResultSupportTarget::Tensor &&
          node->support.slot == slot && (node->support.roles & 7U))
        leaves.push_back(std::move(current));
    } else {
      compact = false;
    }
  }
  const auto channels = shape[channel];
  auto complete = [&](const Node& entry) {
    const auto* node = entry.value;
    if (!(node->support.roles & 4U))
      return false;
    if (node->kind == Impl::Kind::Cartesian)
      return channel + 1 == shape.size() &&
             node->support.first % channels == 0 &&
             node->support.count % channels == 0;
    if (!std::equal(node->input_shape.begin(), node->input_shape.end(),
                    shape.begin(), shape.end()))
      return false;
    const auto& axis = node->mapping[channel];
    if (axis.output_axis < 0 || !axis.step)
      return !axis.source_origin && axis.extent >= channels;
    if (!(grouped_axes & (1U << axis.output_axis)) ||
        step_magnitude(axis.step) > axis.extent)
      return false;
    for (std::size_t other = 0; other < node->mapping.size(); ++other) {
      const auto& mapping = node->mapping[other];
      if (other != channel && mapping.output_axis >= 0 && mapping.step &&
          (grouped_axes & (1U << mapping.output_axis)))
        return false;
    }
    for (const auto& box : entry.mask.boxes()) {
      const auto d = box.dimensions()[axis.output_axis];
      const auto first = axis.source_coordinate(d.offset);
      const auto last = axis.source_coordinate(d.offset + d.extent - 1);
      if (!first.ok() || !last.ok())
        return false;
      const auto low = std::min(first.value(), last.value());
      const auto high = std::max(first.value(), last.value());
      if (low || std::min(axis.extent, channels - high) != channels - high)
        return false;
    }
    return true;
  };
  if (compact) {
    ResourceVector<const Node*> global{
        ResourceAllocator<const Node*>(impl_->budget)};
    ResourceVector<const Node*> mapped{
        ResourceAllocator<const Node*>(impl_->budget)};
    // A proof only compares mappings of this input/slot and source shape.
    // Channel intervals and publication masks are verified after lookup.
    auto less = [&](const Node* a, const Node* b) {
      auto work = tick(shape.size() * 6 + 1);
      if (!work.ok())
        throw work;
      if (a->value->output_shape != b->value->output_shape)
        return a->value->output_shape < b->value->output_shape;
      for (std::size_t axis = 0; axis < shape.size(); ++axis) {
        if (axis == channel)
          continue;
        const auto& x = a->value->mapping[axis];
        const auto& y = b->value->mapping[axis];
        const auto left = std::tie(x.output_axis, x.source_origin, x.step,
                                   x.extent, x.output_origin);
        const auto right = std::tie(y.output_axis, y.source_origin, y.step,
                                    y.extent, y.output_origin);
        if (left != right)
          return left < right;
      }
      return false;
    };
    bool needs_index = false;
    for (const auto& entry : leaves) {
      auto charged = tick(shape.size() + 4 * entry.mask.boxes().size() + 1);
      if (!charged.ok())
        return charged;
      if ((entry.value->support.roles & 3U) && !complete(entry)) {
        needs_index = true;
        break;
      }
    }
    if (needs_index) {
      for (const auto& entry : leaves) {
        auto charged = tick(shape.size() + 1);
        if (!charged.ok())
          return charged;
        const auto* validation = entry.value;
        if (validation->kind != Impl::Kind::Mapped ||
            !(validation->support.roles & 4U) ||
            !std::equal(validation->input_shape.begin(),
                        validation->input_shape.end(), shape.begin(),
                        shape.end()))
          continue;
        bool all = true;
        for (std::size_t i = 0; i < validation->mapping.size(); ++i) {
          const auto& axis = validation->mapping[i];
          all = all && (axis.output_axis < 0 || !axis.step) &&
                !axis.source_origin && axis.extent >= shape[i];
        }
        if (all)
          global.push_back(&entry);
        mapped.push_back(&entry);
      }
      std::sort(mapped.begin(), mapped.end(), less);
    }
    bool proved = true;
    for (const auto& data_entry : leaves) {
      const auto* data = data_entry.value;
      if (!(data->support.roles & 3U) ||
          (!data->support.count && data->kind == Impl::Kind::Cartesian))
        continue;
      auto charged =
          tick(shape.size() + 4 * data_entry.mask.boxes().size() + 1);
      if (!charged.ok())
        return charged;
      if (complete(data_entry))
        continue;
      bool global_validation = false;
      for (const auto* global_entry : global) {
        const auto& entry = *global_entry;
        auto charged = tick(1);
        if (!charged.ok())
          return charged;
        auto missing = data_entry.mask.subtract(entry.mask, limits);
        if (!missing.ok())
          return missing.status();
        if (missing.value().empty()) {
          global_validation = true;
          break;
        }
      }
      if (global_validation)
        continue;
      if (data->kind != Impl::Kind::Mapped ||
          !std::equal(data->input_shape.begin(), data->input_shape.end(),
                      shape.begin(), shape.end())) {
        proved = false;
        break;
      }
      const auto& needed = data_entry.mask;
      auto covered = Footprint::none({channels}, limits);
      if (!covered.ok())
        return covered.status();
      auto first =
          std::lower_bound(mapped.begin(), mapped.end(), &data_entry, less);
      for (auto it = first; it != mapped.end() && !less(&data_entry, *it);
           ++it) {
        const auto& validation_entry = **it;
        const auto* validation = validation_entry.value;
        const auto& axis = validation->mapping[channel];
        if (axis.output_axis >= 0 && axis.step)
          continue;
        auto missing = needed.subtract(validation_entry.mask, limits);
        if (!missing.ok())
          return missing.status();
        if (!missing.value().empty())
          continue;
        auto interval = Footprint::from_regions(
            {channels},
            {Region({{axis.source_origin,
                      std::min(axis.extent, channels - axis.source_origin)}})},
            limits);
        if (!interval.ok())
          return interval.status();
        covered = covered.value().unite(interval.value(), limits);
        if (!covered.ok())
          return covered.status();
      }
      auto all = Footprint::all({channels}, limits);
      if (!all.ok())
        return all.status();
      if (covered.value() != all.value()) {
        proved = false;
        break;
      }
    }
    if (proved)
      return Status::success();
  }
  // Project complete output observations; grouped axes stay together.
  // Compact input rectangles do not require a flattened domain cardinality.
  const auto bridge_bytes = (outputs.boxes().size() + 16) *
                            (sizeof(Region) + 8 * sizeof(RegionDimension));
  auto scratch =
      impl_->budget.reserve(ResourceCapacity::host(bridge_bytes, bridge_bytes));
  if (!scratch.ok())
    return scratch.status();
  auto lease = scratch.take_value();
  auto domain = outputs.shape();
  std::vector<Region> observations;
  observations.reserve(outputs.boxes().size());
  for (std::size_t axis = 0; axis < domain.size(); ++axis)
    if (grouped_axes & (1U << axis))
      domain[axis] = 1;
  for (const auto& box : outputs.boxes()) {
    auto dimensions = box.dimensions();
    for (std::size_t axis = 0; axis < domain.size(); ++axis)
      if (grouped_axes & (1U << axis))
        dimensions[axis] = {0, 1};
    observations.emplace_back(std::move(dimensions));
  }
  auto wanted = Footprint::from_regions(domain, observations, limits);
  if (!wanted.ok())
    return wanted.status();
  const auto spans = [&](ResultSupport support) -> Result<Footprint> {
    std::vector<Region> boxes;
    boxes.reserve(16);
    auto first = support.first, remaining = support.count;
    while (remaining) {
      auto charged = tick(shape.size() + 1);
      if (!charged.ok())
        return Result<Footprint>(charged);
      if (boxes.size() == 16)
        return Result<Footprint>(
            Status{ErrorCode::ResourceExhausted, "tuple proof span box limit"});
      std::vector<RegionDimension> dimensions(shape.size());
      auto index = first;
      for (std::size_t axis = shape.size(); axis; --axis) {
        dimensions[axis - 1] = {index % shape[axis - 1], 1};
        index /= shape[axis - 1];
      }
      if (index)
        return Result<Footprint>(failure());
      std::uint64_t stride = 1, span = 1;
      for (std::size_t axis = shape.size(); axis; --axis) {
        const auto current = axis - 1;
        if (!(first % stride) && remaining >= stride) {
          const auto extent = std::min(
              remaining / stride, shape[current] - dimensions[current].offset);
          dimensions[current].extent = extent;
          span = extent * stride;
          for (std::size_t inner = axis; inner < shape.size(); ++inner)
            dimensions[inner] = {0, shape[inner]};
        }
        if (!core_internal::can_multiply(stride, shape[current]))
          break;
        stride *= shape[current];
      }
      boxes.emplace_back(std::move(dimensions));
      remaining -= span;
      first += span;
    }
    return Footprint::from_regions(shape, boxes, limits);
  };
  return wanted.value().visit(
      [&](const auto& at) {
        auto charged = tick(outputs.shape().size() + 1);
        if (!charged.ok())
          return charged;
        std::vector<RegionDimension> dimensions;
        dimensions.reserve(at.size());
        for (std::size_t axis = 0; axis < at.size(); ++axis)
          dimensions.push_back(grouped_axes & (1U << axis)
                                   ? RegionDimension{0, outputs.shape()[axis]}
                                   : RegionDimension{at[axis], 1});
        auto samples = Footprint::from_regions(outputs.shape(),
                                               {Region(dimensions)}, limits);
        if (!samples.ok())
          return samples.status();
        samples = samples.value().intersect(outputs, limits);
        if (!samples.ok())
          return samples.status();
        auto data = Footprint::none(shape, limits);
        auto validation = Footprint::none(shape, limits);
        if (!data.ok() || !validation.ok())
          return !data.ok() ? data.status() : validation.status();
        const auto collect = [&](ResultSupport support,
                                 const Footprint* mapped) {
          auto status = tick(shape.size() + 1);
          if (!status.ok())
            return status;
          if (support.input != input || support.slot != slot ||
              support.target != ResultSupportTarget::Tensor ||
              !(support.roles & 7U))
            return Status::success();
          auto part = mapped ? Result<Footprint>(*mapped) : spans(support);
          if (!part.ok())
            return part.status();
          if (part.value().shape() != shape)
            return failure();
          if (support.roles & 3U) {
            const auto bytes = part.value().boxes().size() *
                               (sizeof(Region) + 8 * sizeof(RegionDimension));
            auto bridge =
                impl_->budget.reserve(ResourceCapacity::host(bytes, bytes));
            if (!bridge.ok())
              return bridge.status();
            auto storage = bridge.take_value();
            std::vector<Region> closed;
            closed.reserve(part.value().boxes().size());
            for (const auto& box : part.value().boxes()) {
              auto dimensions = box.dimensions();
              dimensions[channel] = {0, channels};
              closed.emplace_back(std::move(dimensions));
            }
            auto tuples = Footprint::from_regions(shape, closed, limits);
            if (!tuples.ok())
              return tuples.status();
            data = data.value().unite(tuples.value(), limits);
            if (!data.ok())
              return data.status();
          }
          if (support.roles & 4U) {
            validation = validation.value().unite(part.value(), limits);
            if (!validation.ok())
              return validation.status();
          }
          return Status::success();
        };
        auto status = project(samples.value(), collect, limits);
        if (status.code == ErrorCode::NotFound) {
          data = Footprint::none(shape, limits);
          validation = Footprint::none(shape, limits);
          if (!data.ok() || !validation.ok())
            return !data.ok() ? data.status() : validation.status();
          status = samples.value().visit(
              [&](const auto& coordinate) {
                auto work = tick(coordinate.size() + 1);
                if (!work.ok())
                  return work;
                std::uint64_t output = 0;
                for (std::size_t axis = 0; axis < coordinate.size(); ++axis) {
                  if (!core_internal::can_multiply_add(
                          output, outputs.shape()[axis], coordinate[axis]))
                    return Status{ErrorCode::ResourceExhausted,
                                  "tuple proof output overflow"};
                  output = output * outputs.shape()[axis] + coordinate[axis];
                }
                return visit_declared(output, limits.maximum_work,
                                      [&](ResultSupport support) {
                                        return collect(support, nullptr);
                                      });
              },
              limits.maximum_work, limits.cancellation);
        }
        if (!status.ok())
          return status;
        auto missing = data.value().subtract(validation.value(), limits);
        return !missing.ok()             ? missing.status()
               : missing.value().empty() ? Status::success()
                                         : failure();
      },
      limits.maximum_work, limits.cancellation);
} catch (const Status& status) {
  return status;
} catch (const std::bad_alloc&) {
  return {ErrorCode::ResourceExhausted, {}};
}
Status ResultRelation::certify(const Footprint& outputs,
                               const FootprintLimits& limits) const try {
  if (!impl_ || !outputs.valid())
    return invalid_relation();
  ResourceAllocationScope scope(impl_->budget);
  auto valid_shape = impl_->validate_output_shape(outputs.shape());
  if (!valid_shape.ok())
    return valid_shape;
  auto count = Footprint::all(outputs.shape(), limits);
  if (!count.ok())
    return count.status();
  auto domain = count.value().element_count();
  if ((domain.ok() && domain.value() != impl_->outputs) ||
      (!domain.ok() && impl_->outputs != UINT64_MAX))
    return invalid_relation();
  auto missing = outputs;
  bool scalar = false;
  std::function<Status(std::shared_ptr<const Impl>)> prove =
      [&](auto node) -> Status {
    auto work = node->budget.consume({1});
    if (!work.ok())
      return work;
    if (limits.cancellation.cancelled())
      return Status{ErrorCode::Cancelled, {}};
    if (node->kind == Impl::Kind::Restricted) {
      auto region = node->mask(limits);
      if (!region.ok())
        return region.status();
      auto covered = missing.intersect(region.value(), limits);
      if (!covered.ok())
        return covered.status();
      ResultRelation child;
      child.impl_ = node->children[0];
      auto certified = child.certify(covered.value(), limits);
      if (!certified.ok())
        return certified;
      auto rest = missing.subtract(covered.value(), limits);
      if (!rest.ok())
        return rest.status();
      missing = rest.take_value();
      return Status::success();
    }
    if (node->kind == Impl::Kind::Union) {
      for (std::uint32_t i = 0; i < node->children_count; ++i) {
        auto status = prove(node->children[i]);
        if (!status.ok())
          return status;
      }
      return Status::success();
    }
    if (node->kind == Impl::Kind::Cartesian ||
        node->kind == Impl::Kind::Identity ||
        node->kind == Impl::Kind::Prefix) {
      if (node->kind == Impl::Kind::Prefix &&
          (outputs.shape().size() != 1 || outputs.shape()[0] != node->outputs))
        return invalid_relation();
      if (node->kind == Impl::Kind::Identity && !domain.ok() &&
          !missing.empty())
        return {ErrorCode::ResourceExhausted,
                "identity tensor support cannot be flattened"};
      auto empty = Footprint::none(outputs.shape(), limits);
      if (!empty.ok())
        return empty.status();
      missing = empty.take_value();
      return Status::success();
    }
    if (node->kind == Impl::Kind::Mapped || node->kind == Impl::Kind::Gather ||
        node->kind == Impl::Kind::Reshape ||
        node->kind == Impl::Kind::Neighborhood) {
      if (!std::equal(node->output_shape.begin(), node->output_shape.end(),
                      outputs.shape().begin(), outputs.shape().end()))
        return invalid_relation();
      auto available = node->mask(limits);
      if (!available.ok())
        return available.status();
      auto rest = missing.subtract(available.value(), limits);
      if (!rest.ok())
        return rest.status();
      missing = rest.take_value();
      return Status::success();
    }
    scalar = true;
    return Status::success();
  };
  auto status = prove(impl_);
  if (!status.ok())
    return status;
  if (missing.empty())
    return Status::success();
  if (!scalar)
    return Status{ErrorCode::NotFound, "missing mapped Result witness"};
  return missing.visit(
      [&](const auto& at) {
        std::uint64_t row = 0;
        for (std::size_t axis = 0; axis < at.size(); ++axis) {
          if (!core_internal::can_multiply_add(row, outputs.shape()[axis],
                                               at[axis]))
            return Status{ErrorCode::ResourceExhausted,
                          "tensor witness coordinate cannot be flattened"};
          row = row * outputs.shape()[axis] + at[axis];
        }
        return visit(row, limits.maximum_work,
                     [](ResultSupport) { return Status::success(); });
      },
      limits.maximum_work, limits.cancellation);
} catch (const std::bad_alloc&) {
  return Status{ErrorCode::ResourceExhausted, {}};
}
Result<Footprint> ResultRelation::preimage(const Footprint& outputs,
                                           ResultSupport input,
                                           const Footprint& changed,
                                           const FootprintLimits& limits) const
    try {  // NOLINT(whitespace/indent_namespace)
  using Answer = Result<Footprint>;
  if (!impl_ || !outputs.valid() || !changed.valid())
    return Answer(invalid_relation());
  ResourceAllocationScope scope(impl_->budget);
  auto valid_shape = impl_->validate_output_shape(outputs.shape());
  if (!valid_shape.ok())
    return Answer(valid_shape);
  ResourceVector<Region> boxes{ResourceAllocator<Region>(impl_->budget)};
  ResourceVector<ResourceLease> reshape_box_leases{
      ResourceAllocator<ResourceLease>(impl_->budget)};
  std::function<Status(std::shared_ptr<const Impl>)> visit =
      [&](auto node) -> Status {
    auto work = node->budget.consume({1});
    if (!work.ok())
      return work;
    if (limits.cancellation.cancelled())
      return Status{ErrorCode::Cancelled, {}};
    if (node->kind == Impl::Kind::Restricted) {
      auto region = node->mask(limits);
      if (!region.ok())
        return region.status();
      auto clipped = outputs.intersect(region.value(), limits);
      if (!clipped.ok())
        return clipped.status();
      ResultRelation child;
      child.impl_ = node->children[0];
      auto projected = child.preimage(clipped.value(), input, changed, limits);
      if (!projected.ok())
        return projected.status();
      boxes.insert(boxes.end(), projected.value().boxes().begin(),
                   projected.value().boxes().end());
      if (boxes.size() > limits.maximum_boxes)
        return {ErrorCode::ResourceExhausted, "relation inverse box limit"};
      return Status::success();
    }
    if (node->kind == Impl::Kind::Union) {
      for (std::uint32_t i = 0; i < node->children_count; ++i) {
        auto status = visit(node->children[i]);
        if (!status.ok())
          return status;
      }
      return Status::success();
    }
    if (node->kind != Impl::Kind::Mapped && node->kind != Impl::Kind::Gather &&
        node->kind != Impl::Kind::Cartesian &&
        node->kind != Impl::Kind::Reshape && node->kind != Impl::Kind::Prefix &&
        node->kind != Impl::Kind::Neighborhood)
      return Status{ErrorCode::NotFound, "scalar inverse relation required"};
    const auto support = node->support;
    const auto roles =
        support.roles |
        (node->gather && node->gather->validation_axes ? 4U : 0U);
    if (support.input != input.input || !(roles & input.roles) ||
        support.target != input.target || support.slot != input.slot)
      return Status::success();
    if (node->kind == Impl::Kind::Cartesian) {
      if (!support.count || changed.empty())
        return Status::success();
      std::uint64_t domain = 1;
      for (auto n : changed.shape()) {
        if (!core_internal::can_multiply(domain, n))
          return invalid_relation();
        domain *= n;
      }
      bool hit = false;
      if (!support.first && support.count == domain) {
        hit = true;
      } else if (support.count == 1) {
        std::vector<std::uint64_t> at(changed.shape().size());
        auto first = support.first;
        for (std::size_t i = at.size(); i-- > 0;) {
          at[i] = first % changed.shape()[i];
          first /= changed.shape()[i];
        }
        hit = !first && changed.contains(at);
      } else {
        return Status{ErrorCode::NotFound, "scalar Cartesian inverse required"};
      }
      if (hit)
        boxes.insert(boxes.end(), outputs.boxes().begin(),
                     outputs.boxes().end());
      return Status::success();
    }
    if (!std::equal(node->input_shape.begin(), node->input_shape.end(),
                    changed.shape().begin(), changed.shape().end()) ||
        !std::equal(node->output_shape.begin(), node->output_shape.end(),
                    outputs.shape().begin(), outputs.shape().end()))
      return invalid_relation();
    if (node->kind == Impl::Kind::Gather) {
      auto requested = node->clip(outputs, limits);
      if (!requested.ok())
        return requested.status();
      const bool validation =
          (input.roles & 4U) && node->gather->validation_axes;
      auto selected = node->gather->preimage(requested.value(), changed,
                                             validation, limits);
      if (!selected.ok())
        return selected.status();
      if (!core_internal::can_add(
              selected.value().boxes().size(),
              std::min<std::uint64_t>(limits.maximum_boxes, boxes.size()),
              limits.maximum_boxes))
        return {ErrorCode::ResourceExhausted, "gather inverse box limit"};
      for (const auto& box : selected.value().boxes()) {
        const auto bytes = box.rank() * sizeof(RegionDimension);
        auto admitted =
            node->budget.reserve(ResourceCapacity::host(bytes, bytes));
        if (!admitted.ok())
          return admitted.status();
        reshape_box_leases.push_back(admitted.take_value());
        boxes.push_back(box);
      }
      return Status::success();
    }
    if (node->kind == Impl::Kind::Prefix) {
      if (changed.boxes().size() > limits.maximum_work)
        return {ErrorCode::ResourceExhausted, "prefix inverse work limit"};
      std::uint64_t first = node->outputs;
      for (const auto& box : changed.boxes()) {
        auto charged = node->budget.consume({1});
        if (!charged.ok())
          return charged;
        if (limits.cancellation.cancelled())
          return {ErrorCode::Cancelled, {}};
        first = std::min(first, box.dimensions()[0].offset);
      }
      if (first < node->outputs && !outputs.empty()) {
        if (boxes.size() >= limits.maximum_boxes)
          return {ErrorCode::ResourceExhausted,
                  "prefix inverse rectangle limit"};
        auto lease = node->budget.reserve(ResourceCapacity::host(
            sizeof(RegionDimension), sizeof(RegionDimension)));
        if (!lease.ok())
          return lease.status();
        reshape_box_leases.push_back(lease.take_value());
        boxes.emplace_back(
            std::vector<RegionDimension>{{first, node->outputs - first}});
      }
      return Status::success();
    }
    if (node->kind == Impl::Kind::Neighborhood) {
      auto expanded = neighborhood_internal::expand(
          node->budget, changed, node->radii, node->periodic, limits);
      if (!expanded.ok())
        return expanded.status();
      auto requested = expanded.value().intersect(outputs, limits);
      if (!requested.ok())
        return requested.status();
      if (!core_internal::can_add(
              requested.value().boxes().size(),
              std::min<std::uint64_t>(limits.maximum_boxes, boxes.size()),
              limits.maximum_boxes))
        return Status{ErrorCode::ResourceExhausted, {}};
      for (const auto& box : requested.value().boxes()) {
        const auto bytes = box.rank() * sizeof(RegionDimension);
        auto lease = node->budget.reserve(ResourceCapacity::host(bytes, bytes));
        if (!lease.ok())
          return lease.status();
        reshape_box_leases.push_back(lease.take_value());
        boxes.push_back(box);
      }
      return Status::success();
    }
    if (node->kind == Impl::Kind::Reshape) {
      if (outputs.empty() || changed.empty() || node->mapped_outputs.empty())
        return Status::success();
      auto witness = node->mask(limits);
      if (!witness.ok())
        return witness.status();
      auto requested = outputs.intersect(witness.value(), limits);
      if (!requested.ok())
        return requested.status();
      if (requested.value().empty())
        return Status::success();
      auto available = Footprint::from_regions(changed.shape(),
                                               {node->reshape_source}, limits);
      if (!available.ok())
        return available.status();
      auto clipped = changed.intersect(available.value(), limits);
      if (!clipped.ok())
        return clipped.status();
      auto shape_lease = node->budget.reserve(
          ResourceCapacity::host(8 * sizeof(uint64_t), 8 * sizeof(uint64_t)));
      if (!shape_lease.ok())
        return shape_lease.status();
      std::vector<uint64_t> extents;
      extents.reserve(8);
      for (auto d : node->reshape_source.dimensions())
        extents.push_back(d.extent);
      const auto bytes =
          clipped.value().boxes().size() *
          (sizeof(Region) + extents.size() * sizeof(RegionDimension));
      auto admitted =
          node->budget.reserve(ResourceCapacity::host(bytes, bytes));
      if (!admitted.ok())
        return admitted.status();
      std::vector<Region> local;
      local.reserve(clipped.value().boxes().size());
      for (const auto& box : clipped.value().boxes()) {
        auto dimensions = box.dimensions();
        for (size_t i = 0; i < dimensions.size(); ++i)
          dimensions[i].offset -= node->reshape_source.dimensions()[i].offset;
        local.emplace_back(std::move(dimensions));
      }
      auto points = Footprint::from_regions(extents, local, limits);
      if (!points.ok())
        return points.status();
      const auto cap = limits.maximum_boxes == UINT64_MAX
                           ? UINT64_MAX
                           : limits.maximum_boxes + 1;
      const auto output_cost = reshape_internal::projection_cost(
          outputs.shape(), requested.value(), extents, cap);
      const auto input_cost = reshape_internal::projection_cost(
          extents, points.value(), outputs.shape(), cap);
      // Bound work by independent prefix spans, not by scalar cardinality.
      if (output_cost < input_cost) {
        auto demanded = reshape_internal::project(
            node->budget, outputs.shape(), requested.value(), extents, limits);
        if (!demanded.ok())
          return demanded.status();
        auto relevant = points.value().intersect(demanded.value(), limits);
        if (!relevant.ok())
          return relevant.status();
        points = std::move(relevant);
      }
      auto projected = reshape_internal::project(
          node->budget, extents, points.value(), outputs.shape(), limits);
      if (!projected.ok())
        return projected.status();
      auto inside = projected.value().intersect(requested.value(), limits);
      if (!inside.ok())
        return inside.status();
      if (!core_internal::can_add(
              inside.value().boxes().size(),
              std::min<uint64_t>(limits.maximum_boxes, boxes.size()),
              limits.maximum_boxes))
        return Status{ErrorCode::ResourceExhausted,
                      "reshape inverse rectangle limit"};
      for (const auto& box : inside.value().boxes()) {
        const auto bytes = box.rank() * sizeof(RegionDimension);
        auto lease = node->budget.reserve(ResourceCapacity::host(bytes, bytes));
        if (!lease.ok())
          return lease.status();
        reshape_box_leases.push_back(lease.take_value());
        boxes.push_back(box);
      }
      return Status::success();
    }
    for (const auto& box : changed.boxes()) {
      auto dimensions = node->mapped_outputs.dimensions();
      bool valid = true;
      for (std::size_t i = 0; i < node->mapping.size(); ++i) {
        const auto m = node->mapping[i];
        const auto d = box.dimensions()[i];
        const auto end = d.offset + d.extent;
        const __int128 lower_input =
            static_cast<__int128>(d.offset) - m.extent + 1;
        const __int128 upper_input = static_cast<__int128>(end) - 1;
        if (m.output_axis < 0 || !m.step) {
          if (static_cast<__int128>(m.source_origin) < lower_input ||
              static_cast<__int128>(m.source_origin) > upper_input) {
            valid = false;
            break;
          }
          continue;
        }
        const __int128 a = m.source_origin, b = m.output_origin;
        const __int128 r = step_magnitude(m.step);
        const auto lower = m.step > 0 ? b + ceil_div(lower_input - a, r)
                                      : b + ceil_div(a - upper_input, r);
        const auto upper = m.step > 0 ? b + floor_div(upper_input - a, r)
                                      : b + floor_div(a - lower_input, r);
        auto& output = dimensions[m.output_axis];
        const auto lo = std::max(static_cast<__int128>(output.offset), lower);
        const auto hi = std::min(
            static_cast<__int128>(output.offset) + output.extent - 1, upper);
        if (hi < lo) {
          valid = false;
          break;
        }
        const auto first = static_cast<std::uint64_t>(lo);
        const auto last = static_cast<std::uint64_t>(hi);
        output = {first, last - first + 1};
      }
      if (valid)
        boxes.emplace_back(std::move(dimensions));
      if (boxes.size() > limits.maximum_boxes)
        return Status{ErrorCode::ResourceExhausted, {}};
    }
    return Status::success();
  };
  auto status = visit(impl_);
  if (!status.ok())
    return Answer(status);
  const auto bytes =
      boxes.size() *
      (sizeof(Region) + outputs.shape().size() * sizeof(RegionDimension));
  auto admitted = impl_->budget.reserve(ResourceCapacity::host(bytes, bytes));
  if (!admitted.ok())
    return Answer(admitted.status());
  std::vector<Region> normalized;
  normalized.reserve(boxes.size());
  normalized.insert(normalized.end(), boxes.begin(), boxes.end());
  auto mapped = Footprint::from_regions(outputs.shape(), normalized, limits);
  return mapped.ok() ? mapped.value().intersect(outputs, limits) : mapped;
} catch (const std::bad_alloc&) {
  return Result<Footprint>(Status{ErrorCode::ResourceExhausted, {}});
}
Result<ResultRelation> ResultRelation::identity(
    ResourceBudget budget, std::uint64_t count, std::uint32_t input,
    std::uint32_t roles, ResultSupportTarget target, std::uint32_t slot) {
  auto made = cartesian(std::move(budget), count,
                        {input, roles, 0, count, target, slot});
  if (!made.ok())
    return made;
  auto result = made.take_value();
  // The object has not escaped this constructor; publish only after freezing.
  std::const_pointer_cast<Impl>(result.impl_)->kind = Impl::Kind::Identity;
  return Result<ResultRelation>(std::move(result));
}
Result<ResultRelation> ResultRelation::unknown(ResourceBudget budget,
                                               std::uint64_t outputs) {
  auto made = cartesian(std::move(budget), outputs, {0, 1, 0, 0},
                        DependencyGuarantee::Unknown);
  if (!made.ok())
    return made;
  auto result = made.take_value();
  std::const_pointer_cast<Impl>(result.impl_)->kind = Impl::Kind::Unknown;
  return Result<ResultRelation>(std::move(result));
}
Result<ResultRelation> ResultRelation::prefix(
    ResourceBudget budget, std::uint64_t count, std::uint32_t input,
    std::uint32_t roles, ResultSupportTarget target, std::uint32_t slot) try {
  if (!count || !roles || (roles & ~7U) ||
      target == ResultSupportTarget::Descriptor)
    return Result<ResultRelation>(invalid_relation());
  auto made = cartesian(budget, count, {input, roles, 0, count, target, slot});
  if (!made.ok())
    return made;
  auto result = made.take_value();
  auto impl = std::const_pointer_cast<Impl>(result.impl_);
  impl->output_shape = ResourceVector<std::uint64_t>(
      {count}, ResourceAllocator<std::uint64_t>(budget));
  impl->input_shape = ResourceVector<std::uint64_t>(
      {count}, ResourceAllocator<std::uint64_t>(budget));
  impl->kind = Impl::Kind::Prefix;
  return Result<ResultRelation>(std::move(result));
} catch (const std::bad_alloc&) {
  return Result<ResultRelation>(Status{ErrorCode::ResourceExhausted, {}});
}
Result<ResultRelation> ResultRelation::rows(
    ResourceBudget budget, std::uint64_t outputs, std::uint64_t count,
    const std::function<Result<ResultRelationRow>(std::uint64_t)>& reader,
    DependencyGuarantee guarantee) {
  if (!reader || !valid_guarantee(guarantee) ||
      !core_internal::can_multiply(count, sizeof(ResultRelationRow)))
    return Result<ResultRelation>(invalid_relation());
  auto made = Impl::make(budget, outputs);
  if (!made.ok())
    return Result<ResultRelation>(made.status());
  auto impl = made.take_value();
  auto file = TemporaryStorage::create(budget);
  if (!file.ok())
    return Result<ResultRelation>(file.status());
  impl->rows = file.take_value();
  auto allocated = impl->rows.append_zeroed(count * sizeof(ResultRelationRow));
  if (!allocated.ok())
    return Result<ResultRelation>(allocated.status());
  for (std::uint64_t i = 0; i < count; ++i) {
    auto status = budget.consume({1});
    if (!status.ok())
      return Result<ResultRelation>(status);
    auto row = reader(i);
    if (!row.ok())
      return Result<ResultRelation>(row.status());
    if (row.value().output >= outputs || !valid_support(row.value().support))
      return Result<ResultRelation>(invalid_relation());
    status = impl->rows.write(
        i * sizeof(ResultRelationRow),
        ByteView(reinterpret_cast<const std::uint8_t*>(&row.value()),
                 sizeof(ResultRelationRow)));
    if (!status.ok())
      return Result<ResultRelation>(status);
  }
  auto sealed = impl->rows.seal();
  if (!sealed.ok())
    return Result<ResultRelation>(sealed);
  impl->kind = Impl::Kind::Rows;
  impl->guarantee = guarantee;
  impl->row_count = count;
  ResultRelation relation;
  relation.impl_ = std::move(impl);
  return Result<ResultRelation>(std::move(relation));
}
Result<ResultRelation> ResultRelation::sample_rows(
    ResourceBudget budget, std::uint64_t outputs, std::uint64_t count,
    const std::function<Result<ResultRelationRow>(std::uint64_t)>& reader,
    DependencyGuarantee guarantee) {
  if (!reader || count > 65536 || !valid_guarantee(guarantee))
    return Result<ResultRelation>(invalid_relation());
  try {
    auto made = Impl::make(budget, outputs);
    if (!made.ok())
      return Result<ResultRelation>(made.status());
    auto impl = made.take_value();
    impl->samples = ResourceVector<ResultRelationRow>(
        ResourceAllocator<ResultRelationRow>(budget));
    impl->samples.reserve(count);
    for (std::uint64_t i = 0; i < count; ++i) {
      auto work = budget.consume({1});
      if (!work.ok())
        return Result<ResultRelation>(work);
      auto row = reader(i);
      if (!row.ok())
        return Result<ResultRelation>(row.status());
      if (row.value().output >= outputs || !valid_support(row.value().support))
        return Result<ResultRelation>(invalid_relation());
      impl->samples.push_back(row.take_value());
    }
    std::sort(impl->samples.begin(), impl->samples.end(),
              [](const auto& a, const auto& b) { return a.output < b.output; });
    impl->kind = Impl::Kind::Samples;
    impl->guarantee = guarantee;
    ResultRelation relation;
    relation.impl_ = std::move(impl);
    return Result<ResultRelation>(std::move(relation));
  } catch (const std::bad_alloc&) {
    return Result<ResultRelation>(Status{ErrorCode::ResourceExhausted, {}});
  }
}
Result<ResultRelation> ResultRelation::unite(
    ResourceBudget budget, const std::vector<ResultRelation>& inputs) {
  if (inputs.empty() || inputs.size() > 16 || !inputs[0].owned_by(budget))
    return Result<ResultRelation>(invalid_relation());
  try {
    const auto adjacent_region = [](const Impl& a,
                                    const Impl& b) -> std::optional<Region> {
      if (a.kind != Impl::Kind::Restricted ||
          b.kind != Impl::Kind::Restricted || a.restricted_outputs.valid() ||
          b.restricted_outputs.valid() || a.children[0] != b.children[0] ||
          a.output_shape != b.output_shape)
        return std::nullopt;
      const auto& left = a.mapped_outputs;
      const auto& right = b.mapped_outputs;
      auto dimensions = left.dimensions();
      if (left.rank() != right.rank())
        return std::nullopt;
      unsigned differences = 0;
      for (std::size_t axis = 0; axis < dimensions.size(); ++axis) {
        const auto x = dimensions[axis], y = right.dimensions()[axis];
        if (x.offset == y.offset && x.extent == y.extent)
          continue;
        if (++differences > 1 || x.offset + x.extent < y.offset ||
            y.offset + y.extent < x.offset)
          return std::nullopt;
        const auto first = std::min(x.offset, y.offset);
        const auto end = std::max(x.offset + x.extent, y.offset + y.extent);
        dimensions[axis] = {first, end - first};
      }
      return Region(std::move(dimensions));
    };
    // Adjacent publications of one witness retain one rectangular mask. This
    // avoids growing a union tree for ordered frames or rows.
    if (inputs.size() == 2 && inputs[1].owned_by(budget) &&
        inputs[0].impl_->kind == Impl::Kind::Restricted &&
        inputs[1].impl_->kind == Impl::Kind::Restricted &&
        inputs[0].impl_->children[0] == inputs[1].impl_->children[0] &&
        inputs[0].impl_->output_shape == inputs[1].impl_->output_shape) {
      auto work = budget.consume({inputs[0].impl_->output_shape.size() + 1});
      if (!work.ok())
        return Result<ResultRelation>(work);
      auto region = adjacent_region(*inputs[0].impl_, *inputs[1].impl_);
      if (region) {
        ResultRelation original;
        original.impl_ = inputs[0].impl_->children[0];
        return original.restrict_to({inputs[0].impl_->output_shape.begin(),
                                     inputs[0].impl_->output_shape.end()},
                                    *region);
      }
    }
    ResourceVector<std::shared_ptr<const Impl>> leaves{
        ResourceAllocator<std::shared_ptr<const Impl>>(budget)};
    ResourceVector<std::shared_ptr<const Impl>> pending{
        ResourceAllocator<std::shared_ptr<const Impl>>(budget)};
    const auto outputs = inputs[0].coverage();
    for (const auto& input : inputs) {
      if (!input.owned_by(budget) || input.coverage() != outputs)
        return Result<ResultRelation>(invalid_relation());
      pending.push_back(input.impl_);
    }
    std::set<const Impl*, std::less<const Impl*>,
             ResourceAllocator<const Impl*>>
        seen{std::less<const Impl*>{}, ResourceAllocator<const Impl*>(budget)};
    using CartesianKey = std::tuple<DependencyGuarantee, std::uint32_t,
                                    std::uint32_t, std::uint64_t, std::uint64_t,
                                    ResultSupportTarget, std::uint32_t>;
    std::set<CartesianKey, std::less<CartesianKey>,
             ResourceAllocator<CartesianKey>>
        cartesian{std::less<CartesianKey>{},
                  ResourceAllocator<CartesianKey>(budget)};
    const Impl* shaped = nullptr;
    while (!pending.empty()) {
      std::uint64_t comparisons = 2;
      for (auto size = seen.size(); size; size >>= 1)
        comparisons += 2;
      auto work = budget.consume({comparisons});
      if (!work.ok())
        return Result<ResultRelation>(work);
      auto node = std::move(pending.back());
      pending.pop_back();
      if (!seen.insert(node.get()).second)
        continue;
      if (node->kind == Impl::Kind::Union) {
        for (std::uint32_t i = 0; i < node->children_count; ++i)
          pending.push_back(node->children[i]);
      } else {
        if (node->kind == Impl::Kind::Cartesian) {
          std::uint64_t comparisons = 1;
          for (auto size = cartesian.size(); size; size >>= 1)
            comparisons += 2;
          auto charged = budget.consume({comparisons});
          if (!charged.ok())
            return Result<ResultRelation>(charged);
          const auto& support = node->support;
          const CartesianKey key{node->guarantee, support.input, support.roles,
                                 support.first,   support.count, support.target,
                                 support.slot};
          if (!cartesian.insert(key).second)
            continue;
        }
        if (leaves.size() >= 65536)
          return Result<ResultRelation>(Status{ErrorCode::ResourceExhausted,
                                               "Result relation leaf limit"});
        auto output_geometry = node->output_geometry();
        if (!output_geometry.ok())
          return Result<ResultRelation>(output_geometry.status());
        if (auto geometry = output_geometry.value()) {
          if (shaped && !std::equal(shaped->output_shape.begin(),
                                    shaped->output_shape.end(),
                                    geometry->output_shape.begin(),
                                    geometry->output_shape.end()))
            return Result<ResultRelation>(invalid_relation());
          auto valid = node->validate_output_shape(
              {geometry->output_shape.begin(), geometry->output_shape.end()});
          if (!valid.ok())
            return Result<ResultRelation>(valid);
          shaped = geometry;
        }
        leaves.push_back(std::move(node));
      }
    }
    // Completing another row can close a rectangular frontier held in a
    // union. Reconsider its masks so ordered multi-axis publications retain
    // compact geometry without widening a partially published row.
    ResourceVector<std::shared_ptr<const Impl>> compact{
        ResourceAllocator<std::shared_ptr<const Impl>>(budget)};
    using Node = std::shared_ptr<const Impl>;
    using Candidates = ResourceVector<std::size_t>;
    // Pointer identity selects candidates only; it is never semantic identity.
    const auto same_witness_order = [](const Node& a, const Node& b) {
      const auto* x = a->children[0].get();
      const auto* y = b->children[0].get();
      return x == y ? a->output_shape < b->output_shape
                    : std::less<const Impl*>{}(x, y);
    };
    using Entry = std::pair<const Node, Candidates>;
    std::map<Node, Candidates, decltype(same_witness_order),
             ResourceAllocator<Entry>>
        candidates{same_witness_order, ResourceAllocator<Entry>(budget)};
    for (auto node : leaves) {
      Candidates* group = nullptr;
      if (node->kind == Impl::Kind::Restricted &&
          !node->restricted_outputs.valid()) {
        std::uint64_t comparisons = 1;
        for (auto size = candidates.size(); size; size >>= 1)
          comparisons += 2;
        auto indexed =
            budget.consume({comparisons * (node->output_shape.size() + 1)});
        if (!indexed.ok())
          return Result<ResultRelation>(indexed);
        group = &candidates
                     .try_emplace(node, ResourceAllocator<std::size_t>(budget))
                     .first->second;
        for (std::size_t at = 0; at < group->size();) {
          const auto i = (*group)[at];
          if (!compact[i]) {
            ++at;
            continue;
          }
          auto work = budget.consume({node->output_shape.size() + 1});
          if (!work.ok())
            return Result<ResultRelation>(work);
          auto region = adjacent_region(*node, *compact[i]);
          if (!region) {
            ++at;
            continue;
          }
          ResultRelation original;
          original.impl_ = node->children[0];
          auto merged = original.restrict_to(
              {node->output_shape.begin(), node->output_shape.end()}, *region);
          if (!merged.ok())
            return merged;
          node = merged.take_value().impl_;
          // Stable slots preserve traversal order and avoid shifting unrelated
          // witnesses or invalidating their candidate indices.
          compact[i].reset();
          if (node->kind != Impl::Kind::Restricted)
            break;
          at = 0;
        }
      }
      if (group && node->kind == Impl::Kind::Restricted)
        group->push_back(compact.size());
      compact.push_back(std::move(node));
    }
    compact.erase(std::remove(compact.begin(), compact.end(), nullptr),
                  compact.end());
    leaves = std::move(compact);
    while (leaves.size() > 1) {
      ResourceVector<std::shared_ptr<const Impl>> next{
          ResourceAllocator<std::shared_ptr<const Impl>>(budget)};
      for (std::size_t first = 0; first < leaves.size(); first += 16) {
        auto made = Impl::make(budget, outputs);
        if (!made.ok())
          return Result<ResultRelation>(made.status());
        auto node = made.take_value();
        node->kind = Impl::Kind::Union;
        node->guarantee = DependencyGuarantee::Exact;
        const auto end = std::min(first + 16, leaves.size());
        for (auto i = first; i < end; ++i) {
          if (leaves[i]->depth >= 32)
            return Result<ResultRelation>(Status{
                ErrorCode::ResourceExhausted, "Result relation depth limit"});
          node->children[node->children_count++] = leaves[i];
          node->depth = std::max(node->depth, leaves[i]->depth + 1);
          node->guarantee =
              combine_guarantee(node->guarantee, leaves[i]->guarantee);
        }
        next.push_back(std::move(node));
      }
      leaves = std::move(next);
    }
    ResultRelation relation;
    relation.impl_ = leaves[0];
    return Result<ResultRelation>(std::move(relation));
  } catch (const std::bad_alloc&) {
    return Result<ResultRelation>(Status{ErrorCode::ResourceExhausted, {}});
  }
}
Result<ResultRelation> ResultRelation::compose(ResourceBudget budget,
                                               ResultRelation first,
                                               ResultRelation second,
                                               std::uint64_t maximum_work) {
  if (!first.owned_by(budget) || !second.owned_by(budget) ||
      first.impl_->depth >= 32 || second.impl_->depth >= 32)
    return Result<ResultRelation>(invalid_relation());
  if (first.guarantee() != DependencyGuarantee::Unknown) {
    for (std::uint64_t row = 0; row < first.coverage(); ++row) {
      auto checked =
          first.impl_->walk(row, &maximum_work, [&](ResultSupport middle) {
            return middle.input == 0 &&
                           middle.target == ResultSupportTarget::Value &&
                           middle.slot == 0 &&
                           middle.first <= second.coverage() &&
                           middle.count <= second.coverage() - middle.first
                       ? Status::success()
                       : invalid_relation();
          });
      if (!checked.ok())
        return Result<ResultRelation>(checked);
    }
  }
  auto made = Impl::make(std::move(budget), first.coverage());
  if (!made.ok())
    return Result<ResultRelation>(made.status());
  auto impl = made.take_value();
  impl->kind = Impl::Kind::Compose;
  impl->guarantee = combine_guarantee(first.guarantee(), second.guarantee());
  impl->depth = std::max(first.impl_->depth, second.impl_->depth) + 1;
  impl->children[0] = std::move(first.impl_);
  impl->children[1] = std::move(second.impl_);
  impl->children_count = 2;
  ResultRelation relation;
  relation.impl_ = std::move(impl);
  return Result<ResultRelation>(std::move(relation));
}
Status ResultRelation::visit(
    std::uint64_t output, std::uint64_t maximum_work,
    const std::function<Status(ResultSupport)>& visitor) const {
  if (!impl_ || !visitor || output >= coverage())
    return invalid_relation();
  if (guarantee() == DependencyGuarantee::Unknown)
    return Status{ErrorCode::NotFound, "Unresolved result support"};
  return impl_->walk(output, &maximum_work, visitor);
}
Status ResultRelation::visit_declared(
    uint64_t output, uint64_t maximum_work,
    const std::function<Status(ResultSupport)>& visitor) const {
  if (!impl_ || !visitor || output >= coverage())
    return invalid_relation();
  return impl_->walk(output, &maximum_work, visitor, true);
}
Result<std::optional<bool>> ResultRelation::intersects(
    std::uint64_t output, const std::vector<ResultSupport>& changed,
    std::uint64_t maximum_work) const {
  if (!impl_ || output >= coverage() || changed.size() > 1024 ||
      std::any_of(changed.begin(), changed.end(),
                  [](auto s) { return !valid_support(s); }))
    return Result<std::optional<bool>>(invalid_relation());
  if (guarantee() == DependencyGuarantee::Unknown)
    return Result<std::optional<bool>>(std::optional<bool>{});
  bool dirty = false;
  auto status = impl_->walk(output, &maximum_work, [&](ResultSupport support) {
    for (const auto& edit : changed) {
      if (!maximum_work)
        return Status{ErrorCode::ResourceExhausted,
                      "result relation work exhausted"};
      auto charged = impl_->budget.consume({1});
      if (!charged.ok())
        return charged;
      --maximum_work;
      dirty |= edit.count && support.input == edit.input &&
               support.target == edit.target && support.slot == edit.slot &&
               (support.roles & edit.roles) &&
               support.first < edit.first + edit.count &&
               edit.first < support.first + support.count;
    }
    return Status::success();
  });
  return status.ok() ? Result<std::optional<bool>>(std::optional<bool>{dirty})
                     : Result<std::optional<bool>>(status);
}
}  // namespace ps
