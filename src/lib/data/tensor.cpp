#include "photospider/data/tensor.hpp"

#include <algorithm>
#include <cstring>
#include <functional>
#include <memory>
#include <set>
#include <utility>
#include <vector>

#include "data/tensor_validation.hpp"
#include "photospider/core/checked_math.hpp"
#include "photospider/core/status_helpers.hpp"

namespace ps {
namespace {
bool same_mapping(const TensorSpan& a, const TensorSpan& b,
                  const Region& overlap) {
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
void append_coalesced(ResourceVector<TensorSpan>* fragments, TensorSpan value) {
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
        TensorSpan::from_storage(prior.descriptor(), Region(dimensions),
                                 prior.layout(), prior.storage());
    if (!joined.ok() || !same_mapping(joined.value(), value, value.region()))
      break;
    value = joined.take_value();
    fragments->pop_back();
  }
  fragments->push_back(std::move(value));
}
// A checked TensorSpan can be copied as one logical run only when the requested
// rectangle is packed in forward row-major order. Singleton strides do not
// affect addressing. TensorWriter has already checked the total byte product.
const std::uint8_t* packed_region_data(const TensorSpan& value,
                                       const Region& region, std::size_t width,
                                       std::size_t bytes) {
  std::size_t stride = width;
  std::vector<std::uint64_t> first(region.rank());
  for (std::size_t axis = region.rank(); axis-- > 0;) {
    const auto wanted = region.dimensions()[axis];
    const auto available = value.region().dimensions()[axis];
    if (wanted.offset < available.offset ||
        wanted.offset + wanted.extent > available.offset + available.extent ||
        (wanted.extent > 1 && value.layout().byte_strides[axis] !=
                                  static_cast<std::int64_t>(stride)))
      return nullptr;
    first[axis] = wanted.offset;
    stride *= wanted.extent;
  }
  const auto address = value.byte_address(first);
  if (!address.ok() || address.value() > value.bytes().size() ||
      bytes > value.bytes().size() - address.value())
    return nullptr;
  return value.bytes().data() + address.value();
}

}  // namespace
void Tensor::swap(Tensor& other) noexcept {
  using std::swap;
  swap(metadata_lifetime_, other.metadata_lifetime_);
  swap(descriptor_, other.descriptor_);
  swap(authorized_, other.authorized_);
  swap(storage_, other.storage_);
}
Tensor& Tensor::operator=(const Tensor& other) {
  if (this != &other) {
    Tensor replacement(other);
    swap(replacement);
  }
  return *this;
}
Tensor& Tensor::operator=(Tensor&& other) noexcept {
  if (this != &other) {
    Tensor replacement(std::move(other));
    swap(replacement);
  }
  return *this;
}
Result<Tensor> Tensor::create(TensorDescriptor descriptor, Footprint authorized,
                              const std::vector<TensorSpan>& fragments,
                              const FootprintLimits& limits) {
  return create_view(std::move(descriptor), std::move(authorized),
                     fragments.data(), fragments.size(), limits, {});
}
Result<Tensor> Tensor::create_view(
    TensorDescriptor descriptor, Footprint authorized,
    const TensorSpan* fragments, std::size_t count,
    const FootprintLimits& limits,
    std::shared_ptr<const void> metadata_lifetime) {
  if (count && !fragments)
    return Result<Tensor>(
        core_internal::invalid_argument("null fragment array"));
  if (!authorized.valid() || authorized.shape() != descriptor.shape)
    return Result<Tensor>(
        core_internal::invalid_argument("fragment domain mismatch"));
  try {
    static_cast<void>(TensorSpan::element_size(descriptor.element_type));
  } catch (const std::invalid_argument&) {
    return Result<Tensor>(
        core_internal::invalid_argument("invalid fragment dtype"));
  }
  auto status = input_internal::validate_value_structure(descriptor);
  if (!status.ok())
    return Result<Tensor>(status);
  auto empty = Footprint::none(descriptor.shape, limits);
  if (!empty.ok())
    return Result<Tensor>(empty.status());
  auto available = empty.take_value();
  Tensor result;
  result.storage_ = std::allocate_shared<Storage>(ResourceAllocator<Storage>{});
  result.metadata_lifetime_ = std::move(metadata_lifetime);
  std::uint64_t work = limits.maximum_work;
  for (std::size_t index = 0; index < count; ++index) {
    const auto& value = fragments[index];
    if (limits.cancellation.cancelled())
      return Result<Tensor>(
          Status::failure(ErrorCode::Cancelled, "fragment creation cancelled"));
    if (!work-- || result.storage_->fragments.size() >= limits.maximum_boxes)
      return Result<Tensor>(Status::failure(ErrorCode::ResourceExhausted,
                                            "fragment construction limit"));
    if (!value.valid())
      return Result<Tensor>(
          core_internal::invalid_argument("invalid fragment"));
    if (value.descriptor().shape != descriptor.shape ||
        value.descriptor().element_type != descriptor.element_type)
      return Result<Tensor>(Status::failure(ErrorCode::TypeMismatch,
                                            "fragment metadata mismatch"));
    auto source =
        Footprint::from_regions(descriptor.shape, {value.region()}, limits);
    if (!source.ok())
      return Result<Tensor>(source.status());
    auto clipped = source.value().intersect(authorized, limits);
    if (!clipped.ok())
      return Result<Tensor>(clipped.status());
    // Disjoint coverage needs no pairwise mapping proof. Dense sequences of
    // tile-local owners usually coalesce to a single coverage rectangle.
    auto shared = clipped.value().intersect(available, limits);
    if (!shared.ok())
      return Result<Tensor>(shared.status());
    if (!shared.value().empty()) {
      for (const auto& prior : result.storage_->fragments) {
        if (!work--)
          return Result<Tensor>(Status::failure(ErrorCode::ResourceExhausted,
                                                "fragment overlap work limit"));
        auto prior_set =
            Footprint::from_regions(descriptor.shape, {prior.region()}, limits);
        if (!prior_set.ok())
          return Result<Tensor>(prior_set.status());
        auto overlap = clipped.value().intersect(prior_set.value(), limits);
        if (!overlap.ok())
          return Result<Tensor>(overlap.status());
        for (const auto& intersection : overlap.value().boxes())
          if (!same_mapping(prior, value, intersection))
            return Result<Tensor>(core_internal::invalid_argument(
                "ambiguous overlapping fragments"));
      }
    }
    auto missing = clipped.value().subtract(available, limits);
    if (!missing.ok())
      return Result<Tensor>(missing.status());
    for (const auto& region : missing.value().boxes()) {
      if (!work-- || result.storage_->fragments.size() >= limits.maximum_boxes)
        return Result<Tensor>(Status::failure(ErrorCode::ResourceExhausted,
                                              "fragment count limit"));
      auto view = value.view(region);
      if (!view.ok())
        return Result<Tensor>(view.status());
      auto part = view.take_value();
      auto canonical = TensorSpan::from_storage(
          part.descriptor(), part.region(), part.layout(), part.storage());
      if (!canonical.ok())
        return Result<Tensor>(canonical.status());
      append_coalesced(&result.storage_->fragments, canonical.take_value());
    }
    auto next = available.unite(clipped.value(), limits);
    if (!next.ok())
      return Result<Tensor>(next.status());
    available = next.take_value();
  }
  if (available != authorized)
    return Result<Tensor>(
        Status::failure(ErrorCode::NotFound, "fragment coverage has a hole"));
  result.descriptor_ = std::move(descriptor);
  result.authorized_ = std::move(authorized);
  return Result<Tensor>(std::move(result));
}
Status Tensor::read(const std::vector<std::uint64_t>& coordinate,
                    void* destination, std::size_t size) const {
  if (!valid() || !destination || !authorized_.contains(coordinate))
    return core_internal::invalid_argument("read outside authorized fragments");
  if (size != TensorSpan::element_size(descriptor_.element_type))
    return Status::failure(ErrorCode::TypeMismatch,
                           "fragment sample width mismatch");
  for (const auto& fragment : storage_->fragments) {
    auto offset = fragment.byte_address(coordinate);
    if (offset.ok()) {
      std::memcpy(destination, fragment.bytes().data() + offset.value(), size);
      return Status::success();
    }
  }
  return Status::failure(ErrorCode::NotFound, "authorized fragment missing");
}
Status Tensor::read(const std::vector<std::uint64_t>& coordinate,
                    void* destination, std::size_t size,
                    const FootprintLimits& limits) const {
  if (!valid() || !destination || coordinate.size() != descriptor_.shape.size())
    return core_internal::invalid_argument("read outside authorized fragments");
  auto remaining = limits.maximum_work;
  const auto admit = [&](std::uint64_t amount) {
    if (limits.cancellation.cancelled())
      return Status{ErrorCode::Cancelled, "fragment read cancelled"};
    if (amount > remaining)
      return Status{ErrorCode::ResourceExhausted, "fragment lookup work limit",
                    FailureReason::WorkLimit};
    if (limits.consume_work) {
      auto status = limits.consume_work(amount);
      if (!status.ok())
        return status;
    }
    remaining -= amount;
    return Status::success();
  };
  auto status = admit(1);
  if (!status.ok())
    return status;
  const auto rank = coordinate.size();
  const auto contains = [&](const Region& region) {
    for (std::size_t axis = 0; axis < rank; ++axis) {
      const auto& dimension = region.dimensions()[axis];
      if (coordinate[axis] < dimension.offset ||
          coordinate[axis] - dimension.offset >= dimension.extent)
        return false;
    }
    return true;
  };
  bool authorized = false;
  for (const auto& box : authorized_.boxes()) {
    status = admit(rank + 1);
    if (!status.ok())
      return status;
    if (contains(box)) {
      authorized = true;
      break;
    }
  }
  if (!authorized)
    return core_internal::invalid_argument("read outside authorized fragments");
  if (size != TensorSpan::element_size(descriptor_.element_type))
    return Status::failure(ErrorCode::TypeMismatch,
                           "fragment sample width mismatch");
  for (const auto& fragment : storage_->fragments) {
    status = admit(rank + 1);
    if (!status.ok())
      return status;
    if (!contains(fragment.region()))
      continue;
    status = admit(4 * rank + 1);
    if (!status.ok())
      return status;
    auto offset = fragment.byte_address(coordinate);
    if (!offset.ok())
      return offset.status();
    std::memcpy(destination, fragment.bytes().data() + offset.value(), size);
    return Status::success();
  }
  return Status::failure(ErrorCode::NotFound, "authorized fragment missing");
}
Result<Tensor> Tensor::restrict(const Footprint& subset,
                                const FootprintLimits& limits) const {
  auto outside = subset.subtract(authorized_, limits);
  if (!outside.ok())
    return Result<Tensor>(outside.status());
  if (!outside.value().empty())
    return Result<Tensor>(core_internal::invalid_argument(
        "fragment restriction exceeds coverage"));
  return create_view(descriptor_, subset, storage_->fragments.data(),
                     storage_->fragments.size(), limits, metadata_lifetime_);
}
Result<TensorSpan> Tensor::collect(const Region& region,
                                   const BufferAllocator& allocator,
                                   const FootprintLimits& limits) const {
  if (!valid() || region.empty())
    return Result<TensorSpan>(
        core_internal::invalid_argument("invalid fragment collection"));
  auto query = Footprint::from_regions(descriptor_.shape, {region}, limits);
  if (!query.ok())
    return Result<TensorSpan>(query.status());
  auto outside = query.value().subtract(authorized_, limits);
  if (!outside.ok())
    return Result<TensorSpan>(outside.status());
  if (!outside.value().empty())
    return Result<TensorSpan>(
        Status::failure(ErrorCode::NotFound, "collection has a hole"));
  auto allocation = TensorWriter::allocate(descriptor_, region, allocator);
  if (!allocation.ok())
    return Result<TensorSpan>(allocation.status());
  auto output = allocation.take_value();
  auto* destination = output.data();
  const auto width = TensorSpan::element_size(descriptor_.element_type);
  // Preserve allocation provenance and logical-sample work limits. A single
  // packed fragment needs no per-sample coordinate/authorization lookup after
  // the full rectangle has passed the coverage checks above.
  const auto* packed = storage_->fragments.size() == 1
                           ? packed_region_data(storage_->fragments[0], region,
                                                width, output.size())
                           : nullptr;
  if (packed) {
    if (limits.cancellation.cancelled())
      return Result<TensorSpan>(Status::failure(
          ErrorCode::Cancelled, "fragment collection cancelled"));
    if (output.size() / width > limits.maximum_work)
      return Result<TensorSpan>(Status::failure(ErrorCode::ResourceExhausted,
                                                "footprint visit limit"));
    constexpr std::size_t kCopyBytes = 64 * 1024;
    for (std::size_t offset = 0; offset < output.size();) {
      if (limits.cancellation.cancelled())
        return Result<TensorSpan>(Status::failure(
            ErrorCode::Cancelled, "fragment collection cancelled"));
      const auto bytes = std::min(kCopyBytes, output.size() - offset);
      if (limits.consume_work) {
        auto charged = limits.consume_work(bytes / width);
        if (!charged.ok())
          return Result<TensorSpan>(charged);
      }
      std::memcpy(destination + offset, packed + offset, bytes);
      offset += bytes;
    }
    if (limits.cancellation.cancelled())
      return Result<TensorSpan>(Status::failure(
          ErrorCode::Cancelled, "fragment collection cancelled"));
    return std::move(output).publish();
  }
  auto status = query.value().visit(
      [&](const auto& coordinate) {
        auto copied = read(coordinate, destination, width, limits);
        destination += width;
        return copied;
      },
      limits.maximum_work, limits.cancellation);
  if (!status.ok())
    return Result<TensorSpan>(status);
  if (limits.cancellation.cancelled())
    return Result<TensorSpan>(
        Status::failure(ErrorCode::Cancelled, "collection cancelled"));
  return std::move(output).publish();
}
Result<std::uint64_t> Tensor::retained_bytes(
    const FootprintLimits& limits) const try {
  if (!valid())
    return Result<std::uint64_t>(
        core_internal::invalid_argument("invalid fragments"));
  std::set<const CpuStorage*, std::less<const CpuStorage*>,
           ResourceAllocator<const CpuStorage*>>
      owners(std::less<const CpuStorage*>{},
             storage_->fragments.get_allocator());
  std::uint64_t remaining = limits.maximum_work;
  std::uint64_t count = 0;
  for (const auto& value : storage_->fragments) {
    if (limits.cancellation.cancelled())
      return Result<std::uint64_t>(Status{ErrorCode::Cancelled, {}});
    if (!remaining--)
      return Result<std::uint64_t>(Status{ErrorCode::ResourceExhausted,
                                          "owner accounting work limit",
                                          FailureReason::WorkLimit});
    if (limits.consume_work) {
      auto charged = limits.consume_work(1);
      if (!charged.ok())
        return Result<std::uint64_t>(charged);
    }
    if (owners.insert(value.storage().get()).second) {
      const auto bytes = value.storage()->capacity();
      if (!core_internal::can_add(count, bytes))
        return Result<std::uint64_t>(Status::failure(
            ErrorCode::ResourceExhausted, "fragment owner sum overflow"));
      count += bytes;
    }
  }
  return Result<std::uint64_t>(count);
} catch (const std::bad_alloc&) {
  return Result<std::uint64_t>(Status{ErrorCode::ResourceExhausted, {}});
}
}  // namespace ps
