#include "photospider/data/input_snapshot.hpp"

#include <algorithm>
#include <cstring>
#include <memory>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include "data/memory_budget.hpp"
#include "data/tensor_access.hpp"
#include "data/tensor_validation.hpp"
#include "photospider/core/checked_math.hpp"
#include "photospider/core/content_digest.hpp"
#include "photospider/core/status_helpers.hpp"

namespace ps {
namespace {
Status stopped(const SnapshotAccessOptions& options) {
  return core_internal::cancellation_status(options.cancellation,
                                            "snapshot access cancelled");
}
Status coverage(const TensorDescriptor& descriptor, const Region& region,
                const SnapshotAccessOptions& options) {
  auto status = stopped(options);
  if (!status.ok())
    return status;
  if (region.empty() || !region.validate(descriptor.shape).ok())
    return Status::failure(ErrorCode::InvalidArgument,
                           "invalid snapshot coverage");
  auto count = region.element_count();
  if (!count.ok())
    return count.status();
  if (count.value() > options.maximum_samples)
    return Status::failure(ErrorCode::ResourceExhausted,
                           "snapshot sample limit");
  return Status::success();
}
template <class Function>
Status visit(const Region& region, const SnapshotAccessOptions& options,
             Function function) {
  std::vector<std::uint64_t> coordinate;
  for (auto d : region.dimensions())
    coordinate.push_back(d.offset);
  for (;;) {
    auto status = stopped(options);
    if (!status.ok())
      return status;
    status = function(coordinate);
    if (!status.ok())
      return status;
    std::size_t axis = coordinate.size();
    while (axis > 0) {
      --axis;
      const auto d = region.dimensions()[axis];
      ++coordinate[axis];
      if (coordinate[axis] < d.offset + d.extent)
        break;
      coordinate[axis] = d.offset;
      if (axis == 0)
        return stopped(options);
    }
  }
}
Result<std::uint64_t> byte_count(const TensorDescriptor& descriptor,
                                 const Region& region) {
  auto packed = descriptor;
  for (std::size_t axis = 0; axis < region.rank(); ++axis)
    packed.shape[axis] = region.dimensions()[axis].extent;
  auto metadata = input_internal::dense_metadata(packed);
  if (!metadata.ok())
    return Result<std::uint64_t>(metadata.status());
  return Result<std::uint64_t>(metadata.value().bytes);
}
bool intersects(const Region& a, const Region& b) {
  for (std::size_t i = 0; i < a.rank(); ++i) {
    auto x = a.dimensions()[i], y = b.dimensions()[i];
    if (x.offset >= y.offset + y.extent || y.offset >= x.offset + x.extent)
      return false;
  }
  return true;
}
Status validate_tensor(const Tensor& value,
                       const SnapshotAccessOptions& options) {
  auto status = stopped(options);
  if (!status.ok())
    return status;
  if (!value.valid() || value.coverage().empty())
    return Status{ErrorCode::InvalidArgument, "invalid snapshot tensor"};
  auto count = value.coverage().element_count();
  if (!count.ok())
    return count.status();
  if (count.value() > options.maximum_samples)
    return Status{ErrorCode::ResourceExhausted, "snapshot sample limit"};
  return Status::success();
}
bool intersects(const Region& region, const Footprint& coverage) {
  return std::any_of(coverage.boxes().begin(), coverage.boxes().end(),
                     [&](const auto& box) { return intersects(region, box); });
}
}  // namespace
struct InputSnapshotStore::Impl {
  InputSnapshotStoreConfig config;
  std::shared_ptr<data_internal::MemoryBudget> budget;
};
struct InputSnapshot::Impl {
  Tensor tensor;
  std::vector<std::uint64_t> block_extents, grid;
  std::shared_ptr<const void> store;
  const std::uint8_t* sample(
      const std::vector<std::uint64_t>& coordinate) const {
    std::size_t index = 0;
    for (std::size_t axis = 0; axis < coordinate.size(); ++axis)
      index = index * grid[axis] + coordinate[axis] / block_extents[axis];
    const auto& value = tensor.fragments().at(index);
    return value.bytes().data() + value.byte_address(coordinate).value();
  }
};
InputSnapshot::InputSnapshot(std::shared_ptr<const Impl> impl)
    : impl_(std::move(impl)) {}
const Tensor& InputSnapshot::tensor() const {
  if (!impl_)
    throw std::logic_error("invalid snapshot");
  return impl_->tensor;
}
const TensorDescriptor& InputSnapshot::descriptor() const {
  if (!impl_)
    throw std::logic_error("invalid snapshot");
  return impl_->tensor.descriptor();
}
Status InputSnapshot::read(const Region& region, std::uint8_t* destination,
                           std::uint64_t size,
                           const SnapshotAccessOptions& options) const {
  if (!impl_ || !destination)
    return Status::failure(ErrorCode::InvalidArgument, "invalid snapshot read");
  auto status = coverage(impl_->tensor.descriptor(), region, options);
  if (!status.ok())
    return status;
  auto count = byte_count(impl_->tensor.descriptor(), region);
  if (!count.ok())
    return count.status();
  if (count.value() != size)
    return Status::failure(ErrorCode::TypeMismatch,
                           "snapshot read size mismatch");
  const auto width =
      TensorSpan::element_size(impl_->tensor.descriptor().element_type);
  return visit(region, options, [&](const auto& coordinate) {
    std::memcpy(destination, impl_->sample(coordinate), width);
    destination += width;
    return Status::success();
  });
}
Result<std::string> InputSnapshot::content_identity(
    const Region& region, const SnapshotAccessOptions& options) const {
  if (!impl_)
    return Result<std::string>(
        Status::failure(ErrorCode::InvalidArgument, "invalid snapshot"));
  auto status = coverage(impl_->tensor.descriptor(), region, options);
  if (!status.ok())
    return Result<std::string>(status);
  content_internal::Sha256 hash;
  hash.text("photospider.input-region.v3");
  hash.integer(
      static_cast<std::uint32_t>(impl_->tensor.descriptor().element_type));
  hash.integer(impl_->tensor.descriptor().shape.size());
  for (auto n : impl_->tensor.descriptor().shape)
    hash.integer(n);
  for (auto d : region.dimensions()) {
    hash.integer(d.offset);
    hash.integer(d.extent);
  }
  const auto width =
      TensorSpan::element_size(impl_->tensor.descriptor().element_type);
  status = visit(region, options, [&](const auto& coordinate) {
    const auto* sample = impl_->sample(coordinate);
    // Decode at the actual native width, then encode a canonical uint64 LE.
    // This preserves exact integer/IEEE bits on either host byte order.
    std::uint64_t bits = 0;
    if (width == 1) {
      bits = *sample;
    } else if (width == 2) {
      std::uint16_t word;
      std::memcpy(&word, sample, 2);
      bits = word;
    } else if (width == 4) {
      std::uint32_t word;
      std::memcpy(&word, sample, 4);
      bits = word;
    } else {
      std::memcpy(&bits, sample, 8);
    }
    hash.integer(bits);
    return Status::success();
  });
  if (!status.ok())
    return Result<std::string>(status);
  return Result<std::string>(hash.finish());
}
InputSnapshotStore::InputSnapshotStore(InputSnapshotStoreConfig config) {
  if (config.block_size == 0 || config.block_size > 4096 ||
      !config.maximum_blocks)
    throw std::invalid_argument("invalid snapshot block configuration");
  impl_ = std::make_shared<Impl>();
  impl_->config = config;
  impl_->budget =
      std::make_shared<data_internal::MemoryBudget>(config.maximum_bytes);
}
InputSnapshotStore::~InputSnapshotStore() = default;
std::uint64_t InputSnapshotStore::live_bytes() const {
  return impl_->budget->live();
}
Result<InputSnapshot> InputSnapshotStore::import_tensor(
    const Tensor& value, const SnapshotAccessOptions& options) const {
  auto status = validate_tensor(value, options);
  if (!status.ok())
    return Result<InputSnapshot>(status);
  auto full = Footprint::all(value.descriptor().shape);
  if (!full.ok())
    return Result<InputSnapshot>(full.status());
  if (value.coverage() != full.value())
    return Result<InputSnapshot>(Status::failure(
        ErrorCode::InvalidArgument, "snapshot import requires complete input"));
  auto count =
      byte_count(value.descriptor(), Region::whole(value.descriptor().shape));
  if (!count.ok())
    return Result<InputSnapshot>(count.status());
  auto out = std::make_shared<InputSnapshot::Impl>();
  data_internal::TensorAccess::describe(out->tensor, value.descriptor(),
                                        full.take_value());
  out->store = impl_;
  std::uint64_t blocks = 1;
  for (std::size_t axis = 0; axis < out->tensor.descriptor().shape.size();
       ++axis) {
    const auto n = out->tensor.descriptor().shape[axis];
    const auto extent = std::min<std::uint64_t>(n, impl_->config.block_size);
    const auto grid = n / extent + (n % extent != 0);
    if (!core_internal::can_multiply(grid, blocks,
                                     impl_->config.maximum_blocks))
      return Result<InputSnapshot>(Status::failure(ErrorCode::ResourceExhausted,
                                                   "snapshot block limit"));
    blocks *= grid;
    out->block_extents.push_back(extent);
    out->grid.push_back(grid);
  }
  auto reserved = impl_->budget->reserve(count.value());
  if (!reserved.ok())
    return Result<InputSnapshot>(reserved.status());
  auto reservation = reserved.take_value();
  auto allocator = reservation->allocator();
  const auto width = TensorSpan::element_size(value.descriptor().element_type);
  for (std::uint64_t index = 0; index < blocks; ++index) {
    status = stopped(options);
    if (!status.ok())
      return Result<InputSnapshot>(status);
    auto dims = Region::whole(value.descriptor().shape).dimensions();
    auto rest = index;
    for (std::size_t axis = dims.size(); axis-- > 0;) {
      const auto start = (rest % out->grid[axis]) * out->block_extents[axis];
      rest /= out->grid[axis];
      dims[axis] = {start,
                    std::min(out->block_extents[axis],
                             out->tensor.descriptor().shape[axis] - start)};
    }
    Region region(std::move(dims));
    auto made = TensorWriter::allocate(value.descriptor(), region, allocator);
    if (!made.ok())
      return Result<InputSnapshot>(made.status());
    auto block = made.take_value();
    auto* bytes = block.data();
    status = visit(region, options, [&](const auto& coordinate) {
      auto copied = value.read(coordinate, bytes, width);
      bytes += width;
      return copied;
    });
    if (!status.ok())
      return Result<InputSnapshot>(status);
    auto frozen = std::move(block).publish();
    if (!frozen.ok())
      return Result<InputSnapshot>(frozen.status());
    data_internal::TensorAccess::edit(out->tensor)
        .push_back(frozen.take_value());
  }
  status = stopped(options);
  if (!status.ok())
    return Result<InputSnapshot>(status);
  reservation->seal();
  return Result<InputSnapshot>(InputSnapshot(std::move(out)));
}
Result<InputSnapshot> InputSnapshotStore::patch(
    const InputSnapshot& base, const Tensor& replacement,
    const SnapshotAccessOptions& options) const {
  if (!base.valid() || base.impl_->store.get() != impl_.get())
    return Result<InputSnapshot>(
        Status::failure(ErrorCode::InvalidArgument, "foreign snapshot"));
  auto status = validate_tensor(replacement, options);
  if (!status.ok())
    return Result<InputSnapshot>(status);
  if (replacement.descriptor().shape != base.descriptor().shape ||
      replacement.descriptor().element_type != base.descriptor().element_type)
    return Result<InputSnapshot>(
        Status::failure(ErrorCode::TypeMismatch, "patch metadata mismatch"));
  auto out = std::make_shared<InputSnapshot::Impl>(*base.impl_);
  std::uint64_t bytes = 0;
  for (const auto& value : out->tensor.fragments()) {
    status = stopped(options);
    if (!status.ok())
      return Result<InputSnapshot>(status);
    if (intersects(value.region(), replacement.coverage())) {
      const auto n = value.bytes().size();
      if (!core_internal::can_add(bytes, n))
        return Result<InputSnapshot>(Status::failure(
            ErrorCode::ResourceExhausted, "patch size overflow"));
      bytes += n;
    }
  }
  const auto width = TensorSpan::element_size(base.descriptor().element_type);
  if (bytes / width > options.maximum_samples)
    return Result<InputSnapshot>(Status::failure(ErrorCode::ResourceExhausted,
                                                 "snapshot patch copy limit"));
  auto reserved = impl_->budget->reserve(bytes);
  if (!reserved.ok())
    return Result<InputSnapshot>(reserved.status());
  auto reservation = reserved.take_value();
  auto allocator = reservation->allocator();
  for (auto& value : data_internal::TensorAccess::edit(out->tensor))
    if (intersects(value.region(), replacement.coverage())) {
      status = stopped(options);
      if (!status.ok())
        return Result<InputSnapshot>(status);
      auto made =
          TensorWriter::allocate(value.descriptor(), value.region(), allocator);
      if (!made.ok())
        return Result<InputSnapshot>(made.status());
      auto block = made.take_value();
      // Copy bounded chunks so a large retained block cannot mask cancellation.
      for (std::size_t offset = 0; offset < value.bytes().size();) {
        status = stopped(options);
        if (!status.ok())
          return Result<InputSnapshot>(status);
        const auto n =
            std::min<std::size_t>(65536, value.bytes().size() - offset);
        std::memcpy(block.data() + offset, value.bytes().data() + offset, n);
        offset += n;
      }
      for (const auto& patch : replacement.coverage().boxes()) {
        if (!intersects(value.region(), patch))
          continue;
        auto dims = value.region().dimensions();
        for (std::size_t axis = 0; axis < dims.size(); ++axis) {
          const auto p = patch.dimensions()[axis];
          const auto start = std::max(dims[axis].offset, p.offset);
          const auto end = std::min(dims[axis].offset + dims[axis].extent,
                                    p.offset + p.extent);
          dims[axis] = {start, end - start};
        }
        status = visit(
            Region(std::move(dims)), options, [&](const auto& coordinate) {
              return replacement.read(
                  coordinate,
                  block.data() + value.byte_address(coordinate).value(), width);
            });
        if (!status.ok())
          return Result<InputSnapshot>(status);
      }
      auto frozen = std::move(block).publish();
      if (!frozen.ok())
        return Result<InputSnapshot>(frozen.status());
      value = frozen.take_value();
    }
  status = stopped(options);
  if (!status.ok())
    return Result<InputSnapshot>(status);
  reservation->seal();
  return Result<InputSnapshot>(InputSnapshot(std::move(out)));
}
}  // namespace ps
