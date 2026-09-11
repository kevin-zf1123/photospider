#include "photospider/data/value_fragments.hpp"

#include <algorithm>
#include <cstring>
#include <set>
#include <utility>
#include <vector>

#include "data/input_validation.hpp"

namespace ps {
namespace {
bool same_mapping(const Value& a, const Value& b, const Region& overlap) {
  if (a.storage() != b.storage())
    return false;
  std::vector<std::uint64_t> first;
  for (std::size_t axis = 0; axis < overlap.rank(); ++axis) {
    const auto d = overlap.dimensions()[axis];
    first.push_back(d.offset);
    if (d.extent > 1 &&
        a.layout().byte_strides[axis] != b.layout().byte_strides[axis])
      return false;
  }
  const auto left = a.byte_address(first), right = b.byte_address(first);
  return left.ok() && right.ok() && left.value() == right.value();
}
// Coalesce only rectangular neighbors with identical checked byte mappings.
// This keeps a shared table published one atom at a time from becoming a
// quadratic overlap list. It never fills holes or merges distinct owners.
void append_coalesced(std::vector<Value>* fragments, Value value) {
  while (!fragments->empty()) {
    const auto& prior = fragments->back();
    if (prior.storage() != value.storage())
      break;
    auto dimensions = prior.region().dimensions();
    const auto& incoming = value.region().dimensions();
    std::size_t differing = 0;
    bool adjacent = true;
    for (std::size_t axis = 0; axis < dimensions.size(); ++axis) {
      auto& a = dimensions[axis];
      const auto b = incoming[axis];
      if (a.offset == b.offset && a.extent == b.extent)
        continue;
      if (++differing > 1 || (a.offset + a.extent != b.offset &&
                              b.offset + b.extent != a.offset)) {
        adjacent = false;
        break;
      }
      a.offset = std::min(a.offset, b.offset);
      a.extent += b.extent;
    }
    if (!adjacent || differing != 1)
      break;
    auto joined =
        Value::from_storage(prior.descriptor(), Region(dimensions),
                            prior.layout(), prior.storage(), prior.facets());
    if (!joined.ok() || !same_mapping(joined.value(), value, value.region()))
      break;
    value = joined.take_value();
    fragments->pop_back();
  }
  fragments->push_back(std::move(value));
}
Status invalid(const char* message) {
  return Status::failure(ErrorCode::InvalidArgument, message);
}
}  // namespace
Result<ValueFragments> ValueFragments::create(
    ValueDescriptor descriptor, std::vector<ValueFacet> facets,
    Footprint authorized, const std::vector<Value>& fragments,
    const FootprintLimits& limits) {
  if (!authorized.valid() || authorized.shape() != descriptor.shape)
    return Result<ValueFragments>(invalid("fragment domain mismatch"));
  try {
    static_cast<void>(Value::element_size(descriptor.element_type));
  } catch (const std::invalid_argument&) {
    return Result<ValueFragments>(invalid("invalid fragment dtype"));
  }
  auto status = input_internal::canonicalize_facets(&facets);
  if (!status.ok())
    return Result<ValueFragments>(status);
  for (const auto& facet : facets)
    if (facet.key == "photospider.image" ||
        facet.key == "photospider.semantic") {
      auto semantic = decode_semantic(facet);
      if (!semantic.ok())
        return Result<ValueFragments>(semantic.status());
      status = validate_semantic_descriptor(semantic.value(), descriptor);
      if (!status.ok())
        return Result<ValueFragments>(status);
    }
  for (const auto& region : authorized.boxes())
    if (!input_internal::complete_image_channels(descriptor, facets, region))
      return Result<ValueFragments>(
          invalid("image fragments require full channels"));
  auto empty = Footprint::none(descriptor.shape, limits);
  if (!empty.ok())
    return Result<ValueFragments>(empty.status());
  auto available = empty.take_value();
  ValueFragments result;
  std::uint64_t work = limits.maximum_work;
  for (const auto& value : fragments) {
    if (limits.cancellation.cancelled())
      return Result<ValueFragments>(
          Status::failure(ErrorCode::Cancelled, "fragment creation cancelled"));
    if (!work-- || result.fragments_.size() >= limits.maximum_boxes)
      return Result<ValueFragments>(Status::failure(
          ErrorCode::ResourceExhausted, "fragment construction limit"));
    if (!value.valid())
      return Result<ValueFragments>(invalid("invalid fragment"));
    if (value.descriptor().shape != descriptor.shape ||
        value.descriptor().element_type != descriptor.element_type ||
        !input_internal::same_facets(value.facets(), facets))
      return Result<ValueFragments>(Status::failure(
          ErrorCode::TypeMismatch, "fragment metadata mismatch"));
    if (!input_internal::complete_image_channels(descriptor, facets,
                                                 value.region()))
      return Result<ValueFragments>(
          invalid("every image fragment requires full channels"));
    auto source =
        Footprint::from_regions(descriptor.shape, {value.region()}, limits);
    if (!source.ok())
      return Result<ValueFragments>(source.status());
    auto clipped = source.value().intersect(authorized, limits);
    if (!clipped.ok())
      return Result<ValueFragments>(clipped.status());
    for (const auto& prior : result.fragments_) {
      if (!work--)
        return Result<ValueFragments>(Status::failure(
            ErrorCode::ResourceExhausted, "fragment overlap work limit"));
      auto prior_set =
          Footprint::from_regions(descriptor.shape, {prior.region()}, limits);
      if (!prior_set.ok())
        return Result<ValueFragments>(prior_set.status());
      auto overlap = clipped.value().intersect(prior_set.value(), limits);
      if (!overlap.ok())
        return Result<ValueFragments>(overlap.status());
      for (const auto& intersection : overlap.value().boxes())
        if (!same_mapping(prior, value, intersection))
          return Result<ValueFragments>(
              invalid("ambiguous overlapping fragments"));
    }
    auto missing = clipped.value().subtract(available, limits);
    if (!missing.ok())
      return Result<ValueFragments>(missing.status());
    for (const auto& region : missing.value().boxes()) {
      if (!work-- || result.fragments_.size() >= limits.maximum_boxes)
        return Result<ValueFragments>(Status::failure(
            ErrorCode::ResourceExhausted, "fragment count limit"));
      auto view = value.view(region);
      if (!view.ok())
        return Result<ValueFragments>(view.status());
      append_coalesced(&result.fragments_, view.take_value());
    }
    auto next = available.unite(clipped.value(), limits);
    if (!next.ok())
      return Result<ValueFragments>(next.status());
    available = next.take_value();
  }
  if (available != authorized)
    return Result<ValueFragments>(
        Status::failure(ErrorCode::NotFound, "fragment coverage has a hole"));
  result.descriptor_ = std::move(descriptor);
  result.facets_ = std::move(facets);
  result.authorized_ = std::move(authorized);
  return Result<ValueFragments>(std::move(result));
}
Status ValueFragments::read(const std::vector<std::uint64_t>& coordinate,
                            void* destination, std::size_t size) const {
  if (!valid() || !destination || !authorized_.contains(coordinate))
    return invalid("read outside authorized fragments");
  if (size != Value::element_size(descriptor_.element_type))
    return Status::failure(ErrorCode::TypeMismatch,
                           "fragment sample width mismatch");
  for (const auto& fragment : fragments_) {
    auto offset = fragment.byte_address(coordinate);
    if (offset.ok()) {
      std::memcpy(destination, fragment.bytes().data() + offset.value(), size);
      return Status::success();
    }
  }
  return Status::failure(ErrorCode::NotFound, "authorized fragment missing");
}
Result<ValueFragments> ValueFragments::restrict(
    const Footprint& subset, const FootprintLimits& limits) const {
  auto outside = subset.subtract(authorized_, limits);
  if (!outside.ok())
    return Result<ValueFragments>(outside.status());
  if (!outside.value().empty())
    return Result<ValueFragments>(
        invalid("fragment restriction exceeds coverage"));
  return create(descriptor_, facets_, subset, fragments_, limits);
}
Result<Value> ValueFragments::collect(const Region& region,
                                      const BufferAllocator& allocator,
                                      const FootprintLimits& limits) const {
  if (!valid() || region.empty())
    return Result<Value>(invalid("invalid fragment collection"));
  auto query = Footprint::from_regions(descriptor_.shape, {region}, limits);
  if (!query.ok())
    return Result<Value>(query.status());
  auto outside = query.value().subtract(authorized_, limits);
  if (!outside.ok())
    return Result<Value>(outside.status());
  if (!outside.value().empty())
    return Result<Value>(
        Status::failure(ErrorCode::NotFound, "collection has a hole"));
  if (!input_internal::complete_image_channels(descriptor_, facets_, region))
    return Result<Value>(invalid("image collection requires full channels"));
  auto allocation = MutableValue::allocate(descriptor_, region, allocator);
  if (!allocation.ok())
    return Result<Value>(allocation.status());
  auto output = allocation.take_value();
  auto* destination = output.data();
  const auto width = Value::element_size(descriptor_.element_type);
  auto status = query.value().visit(
      [&](const auto& coordinate) {
        auto copied = read(coordinate, destination, width);
        destination += width;
        return copied;
      },
      limits.maximum_work, limits.cancellation);
  if (!status.ok())
    return Result<Value>(status);
  if (limits.cancellation.cancelled())
    return Result<Value>(
        Status::failure(ErrorCode::Cancelled, "collection cancelled"));
  return std::move(output).publish(facets_);
}
Result<std::uint64_t> ValueFragments::retained_bytes() const {
  if (!valid())
    return Result<std::uint64_t>(invalid("invalid fragments"));
  std::set<const CpuStorage*> owners;
  std::uint64_t count = 0;
  for (const auto& value : fragments_)
    if (owners.insert(value.storage().get()).second) {
      const auto bytes = value.storage()->capacity();
      if (bytes > UINT64_MAX - count)
        return Result<std::uint64_t>(Status::failure(
            ErrorCode::ResourceExhausted, "fragment owner sum overflow"));
      count += bytes;
    }
  return Result<std::uint64_t>(count);
}
}  // namespace ps
