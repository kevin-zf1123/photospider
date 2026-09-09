#include "photospider/data/input_snapshot.hpp"

#include <algorithm>
#include <cstring>
#include <memory>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include "data/content_digest.hpp"
#include "data/input_validation.hpp"
#include "execution/memory_budget.hpp"

namespace ps {
namespace {
/** @brief Visits packed spatial samples without materializing an image. */
template <class Function>
Status visit(const Region& region, Function function) {
  const auto yd = region.dimensions()[0], xd = region.dimensions()[1];
  const std::uint64_t channels =
      region.rank() == 3 ? region.dimensions()[2].extent : 1;
  for (std::uint64_t y = yd.offset; y < yd.offset + yd.extent; ++y)
    for (std::uint64_t x = xd.offset; x < xd.offset + xd.extent; ++x)
      for (std::uint64_t c = 0; c < channels; ++c) {
        auto status = function(y, x, c);
        if (!status.ok())
          return status;
      }
  return Status::success();
}
/** @brief Checked packed region size under the existing dense address bound. */
Result<std::uint64_t> byte_count(const Region& r) {
  std::vector<std::uint64_t> shape;
  for (const auto d : r.dimensions())
    shape.push_back(d.extent);
  auto metadata = input_internal::dense_metadata({ElementType::Float32, shape});
  if (!metadata.ok())
    return Result<std::uint64_t>(metadata.status());
  return Result<std::uint64_t>(metadata.value().bytes);
}
/** @brief Intersects spatial rectangles; callers already checked equal rank. */
bool intersects(const Region& a, const Region& b) {
  for (std::size_t i = 0; i < a.rank(); ++i) {
    auto x = a.dimensions()[i], y = b.dimensions()[i];
    if (x.offset >= y.offset + y.extent || y.offset >= x.offset + x.extent)
      return false;
  }
  return true;
}
}  // namespace
struct InputSnapshotStore::Impl {
  InputSnapshotStoreConfig config;
  std::shared_ptr<execution_internal::MemoryBudget> budget;
};
struct InputSnapshot::Impl {
  ValueDescriptor descriptor;
  std::vector<ValueFacet> facets;
  std::uint64_t block_size = 0, columns = 0;
  std::vector<Value> blocks;
  std::shared_ptr<const void> store;
  /** @brief Resolves one logical sample into the owning immutable block. */
  const std::uint8_t* sample(std::uint64_t y, std::uint64_t x,
                             std::uint64_t c) const {
    const auto& v = blocks.at((y / block_size) * columns + x / block_size);
    std::vector<std::uint64_t> coordinate{y, x};
    if (descriptor.shape.size() == 3)
      coordinate.push_back(c);
    return v.bytes().data() + v.byte_address(coordinate).value();
  }
  Status coverage(const Region& region) const {
    if (region.empty() || !region.validate(descriptor.shape).ok() ||
        (descriptor.shape.size() == 3 &&
         (region.dimensions()[2].offset != 0 ||
          region.dimensions()[2].extent != descriptor.shape[2])))
      return Status::failure(ErrorCode::InvalidArgument,
                             "invalid snapshot coverage");
    return Status::success();
  }
};
InputSnapshot::InputSnapshot(std::shared_ptr<const Impl> impl)
    : impl_(std::move(impl)) {}
const ValueDescriptor& InputSnapshot::descriptor() const {
  if (!impl_)
    throw std::logic_error("invalid snapshot");
  return impl_->descriptor;
}
const std::vector<ValueFacet>& InputSnapshot::facets() const {
  if (!impl_)
    throw std::logic_error("invalid snapshot");
  return impl_->facets;
}
Status InputSnapshot::read(const Region& region, std::uint8_t* destination,
                           std::uint64_t size) const {
  if (!impl_ || !destination)
    return Status::failure(ErrorCode::InvalidArgument, "invalid snapshot read");
  auto status = impl_->coverage(region);
  if (!status.ok())
    return status;
  auto count = byte_count(region);
  if (!count.ok())
    return count.status();
  if (count.value() != size)
    return Status::failure(ErrorCode::TypeMismatch,
                           "snapshot read size mismatch");
  return visit(region, [&](auto y, auto x, auto c) {
    std::memcpy(destination, impl_->sample(y, x, c), 4);
    destination += 4;
    return Status::success();
  });
}
Result<std::string> InputSnapshot::content_identity(
    const Region& region) const {
  if (!impl_)
    return Result<std::string>(
        Status::failure(ErrorCode::InvalidArgument, "invalid snapshot"));
  auto status = impl_->coverage(region);
  if (!status.ok())
    return Result<std::string>(status);
  content_internal::Sha256 hash;
  hash.text("photospider.input-region.v1");
  hash.integer(impl_->descriptor.shape.size());
  for (auto n : impl_->descriptor.shape)
    hash.integer(n);
  for (auto d : region.dimensions()) {
    hash.integer(d.offset);
    hash.integer(d.extent);
  }
  hash.integer(impl_->facets.size());
  for (const auto& f : impl_->facets) {
    hash.text(f.key);
    hash.integer(f.version);
    hash.integer(f.payload.size());
    hash.bytes(f.payload.data(), f.payload.size());
  }
  status = visit(region, [&](auto y, auto x, auto c) {
    std::uint32_t bits;
    std::memcpy(&bits, impl_->sample(y, x, c), 4);
    hash.integer(bits);
    return Status::success();
  });
  if (!status.ok())
    return Result<std::string>(status);
  return Result<std::string>(hash.finish());
}
InputSnapshotStore::InputSnapshotStore(InputSnapshotStoreConfig config) {
  if (config.block_size == 0 || config.block_size > 4096)
    throw std::invalid_argument("invalid snapshot block size");
  impl_ = std::make_shared<Impl>();
  impl_->config = config;
  impl_->budget =
      std::make_shared<execution_internal::MemoryBudget>(config.maximum_bytes);
}
InputSnapshotStore::~InputSnapshotStore() = default;
std::uint64_t InputSnapshotStore::live_bytes() const {
  return impl_->budget->live();
}
Result<InputSnapshot> InputSnapshotStore::import_value(
    const Value& value) const {
  auto status = input_internal::validate_image_storage_value(value);
  if (!status.ok())
    return Result<InputSnapshot>(status);
  if (!input_internal::whole_region(value.region(), value.descriptor().shape))
    return Result<InputSnapshot>(Status::failure(
        ErrorCode::InvalidArgument, "snapshot import requires complete input"));
  auto count = byte_count(value.region());
  if (!count.ok())
    return Result<InputSnapshot>(count.status());
  auto reserved = impl_->budget->reserve(count.value());
  if (!reserved.ok())
    return Result<InputSnapshot>(reserved.status());
  auto reservation = reserved.take_value();
  auto allocator = reservation->allocator();
  auto out = std::make_shared<InputSnapshot::Impl>();
  out->descriptor = value.descriptor();
  out->facets = value.facets();
  out->store = impl_;
  out->block_size = impl_->config.block_size;
  const auto h = out->descriptor.shape[0], w = out->descriptor.shape[1];
  out->columns = w / out->block_size + (w % out->block_size != 0);
  for (std::uint64_t y = 0; y < h;) {
    const auto bh = std::min(out->block_size, h - y);
    for (std::uint64_t x = 0; x < w;) {
      const auto bw = std::min(out->block_size, w - x);
      auto dims = value.region().dimensions();
      dims[0] = {y, bh};
      dims[1] = {x, bw};
      Region r(dims);
      auto made = MutableValue::allocate(value.descriptor(), r, allocator);
      if (!made.ok())
        return Result<InputSnapshot>(made.status());
      auto block = made.take_value();
      auto* bytes = block.data();
      status = visit(r, [&](auto yy, auto xx, auto c) {
        std::vector<std::uint64_t> coord{yy, xx};
        if (r.rank() == 3)
          coord.push_back(c);
        std::memcpy(
            bytes, value.bytes().data() + value.byte_address(coord).value(), 4);
        bytes += 4;
        return Status::success();
      });
      if (!status.ok())
        return Result<InputSnapshot>(status);
      auto frozen = std::move(block).publish(value.facets());
      if (!frozen.ok())
        return Result<InputSnapshot>(frozen.status());
      out->blocks.push_back(frozen.take_value());
      x += bw;
    }
    y += bh;
  }
  reservation->seal();
  return Result<InputSnapshot>(InputSnapshot(std::move(out)));
}
Result<InputSnapshot> InputSnapshotStore::patch(
    const InputSnapshot& base, const Value& replacement) const {
  if (!base.valid() || base.impl_->store.get() != impl_.get())
    return Result<InputSnapshot>(
        Status::failure(ErrorCode::InvalidArgument, "foreign snapshot"));
  auto status = input_internal::validate_image_storage_value(replacement);
  if (!status.ok())
    return Result<InputSnapshot>(status);
  if (replacement.descriptor().shape != base.descriptor().shape ||
      !input_internal::same_facets(replacement.facets(), base.facets()))
    return Result<InputSnapshot>(
        Status::failure(ErrorCode::TypeMismatch, "patch metadata mismatch"));
  auto out = std::make_shared<InputSnapshot::Impl>(*base.impl_);
  std::uint64_t bytes = 0;
  for (const auto& v : out->blocks)
    if (intersects(v.region(), replacement.region())) {
      const auto n = v.bytes().size();
      if (n > UINT64_MAX - bytes)
        return Result<InputSnapshot>(Status::failure(
            ErrorCode::ResourceExhausted, "patch size overflow"));
      bytes += n;
    }
  auto reserved = impl_->budget->reserve(bytes);
  if (!reserved.ok())
    return Result<InputSnapshot>(reserved.status());
  auto reservation = reserved.take_value();
  auto allocator = reservation->allocator();
  for (auto& v : out->blocks)
    if (intersects(v.region(), replacement.region())) {
      auto made = MutableValue::allocate(v.descriptor(), v.region(), allocator);
      if (!made.ok())
        return Result<InputSnapshot>(made.status());
      auto block = made.take_value();
      std::memcpy(block.data(), v.bytes().data(), v.bytes().size());
      auto dims = v.region().dimensions();
      for (std::size_t a = 0; a < dims.size(); ++a) {
        const auto p = replacement.region().dimensions()[a];
        const auto start = std::max(dims[a].offset, p.offset);
        const auto end =
            std::min(dims[a].offset + dims[a].extent, p.offset + p.extent);
        dims[a] = {start, end - start};
      }
      status = visit(Region(dims), [&](auto y, auto x, auto c) {
        std::vector<std::uint64_t> coord{y, x};
        if (dims.size() == 3)
          coord.push_back(c);
        std::memcpy(block.data() + v.byte_address(coord).value(),
                    replacement.bytes().data() +
                        replacement.byte_address(coord).value(),
                    4);
        return Status::success();
      });
      if (!status.ok())
        return Result<InputSnapshot>(status);
      auto frozen = std::move(block).publish(v.facets());
      if (!frozen.ok())
        return Result<InputSnapshot>(frozen.status());
      v = frozen.take_value();
    }
  reservation->seal();
  return Result<InputSnapshot>(InputSnapshot(std::move(out)));
}
}  // namespace ps
