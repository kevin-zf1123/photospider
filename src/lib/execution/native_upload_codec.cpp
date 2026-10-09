#include "execution/native_upload_codec.hpp"

#include <cstring>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "core/checked_math.hpp"
#include "data/content_digest.hpp"
#include "execution/execution_timing.hpp"
#include "plugin/port_validation.hpp"
namespace ps::execution_internal {
/** @brief Computes packed bytes for demanded coverage without allocating
 * payload. */
Result<std::uint64_t> region_bytes(const ValueDescriptor& descriptor,
                                   const Region& region) {
  if (!region.validate(descriptor.shape).ok() || region.empty())
    return Result<std::uint64_t>(
        Status::failure(ErrorCode::InvalidArgument, "invalid packed region"));
  auto count = region.element_count();
  const auto width = Value::element_size(descriptor.element_type);
  if (!count.ok() || !core_internal::can_multiply(count.value(), width))
    return Result<std::uint64_t>(Status::failure(
        ErrorCode::ResourceExhausted, "regional byte count overflows"));
  return Result<std::uint64_t>(count.value() * width);
}
/** @brief Copies only logical coverage into a packed destination with global
 * origin. */
Status copy_region(ValueView source, MutableValue* destination,
                   const Region& available,
                   const std::function<ErrorCode()>& stop) {
  const auto& region = source.region();
  if (region.rank() != available.rank())
    return Status::failure(ErrorCode::TypeMismatch, "copy region rank differs");
  std::vector<std::uint64_t> coordinate;
  for (std::size_t i = 0; i < region.rank(); ++i) {
    const auto part = region.dimensions()[i],
               bounds = available.dimensions()[i];
    if (part.offset < bounds.offset ||
        part.offset + part.extent > bounds.offset + bounds.extent)
      return Status::failure(ErrorCode::TypeMismatch,
                             "copy exceeds destination coverage");
    coordinate.push_back(part.offset);
  }
  auto count = region.element_count();
  if (!count.ok())
    return count.status();
  const auto width = Value::element_size(source.descriptor().element_type);
  for (std::uint64_t element = 0; element < count.value(); ++element) {
    if ((element & 1023) == 0 && stop) {
      const auto code = stop();
      if (code != ErrorCode::Ok)
        return Status{code, {}};
    }
    auto from = source.byte_address(coordinate);
    if (!from.ok())
      return from.status();
    std::uint64_t to = 0;
    for (std::size_t axis = 0; axis < coordinate.size(); ++axis)
      to +=
          (coordinate[axis] - available.dimensions()[axis].offset) *
          static_cast<std::uint64_t>(destination->layout().byte_strides[axis]);
    if (to > destination->size() || width > destination->size() - to)
      return Status::failure(ErrorCode::TypeMismatch,
                             "copy exceeds destination bytes");
    std::memcpy(destination->data() + to, source.bytes().data() + from.value(),
                width);
    for (std::size_t reverse = coordinate.size(); reverse > 0; --reverse) {
      const auto axis = reverse - 1;
      ++coordinate[axis];
      const auto dim = region.dimensions()[axis];
      if (coordinate[axis] < dim.offset + dim.extent)
        break;
      coordinate[axis] = dim.offset;
    }
  }
  return Status::success();
}

/**
 * @brief Materializes one immutable Value into another local backend residency.
 * @param source Valid producer Value.
 * @return Packed physical backing when compact, otherwise a copied Value
 * preserving its source metadata.
 * @throws std::bad_alloc If transfer allocation fails.
 * @note Backend residency is tracked by the owning ExecutionRun; Value itself
 * remains backend-neutral and exposes no native device handle.
 */
Result<Value> transfer_value(const Value& source,
                             const BufferAllocator& allocator, bool compact,
                             const std::function<ErrorCode()>& stop) {
  execution_testing::ExecutionTiming timing(
      execution_testing::TimingKind::ValueMaterialization);
  if (compact) {
    auto allocated =
        MutableValue::allocate(source.descriptor(), source.region(), allocator);
    if (!allocated.ok())
      return Result<Value>(allocated.status());
    auto output = allocated.take_value();
    auto status =
        copy_region(ValueView(source), &output, source.region(), stop);
    if (!status.ok())
      return Result<Value>(status);
    auto published = std::move(output).publish();
    if (published.ok())
      timing.success(published.value().bytes().size());
    return published;
  }
  auto allocated = allocator.allocate(source.bytes().size());
  if (!allocated.ok())
    return Result<Value>(allocated.status());
  auto buffer = allocated.take_value();
  std::memcpy(buffer.data(), source.bytes().data(), source.bytes().size());
  auto published = Value::from_storage(
      source.descriptor(), source.region(), source.layout(),
      std::move(buffer).freeze(), source.facets(), source.resources());
  if (published.ok())
    timing.success(published.value().bytes().size());
  return published;
}

/** @brief Keys immutable physical coverage; a cache entry separately proves
 * that the source allocation is still alive. Facets do not change bytes. */
std::string upload_view_key(const Value& value) {
  content_internal::Sha256 hash;
  hash.text("photospider.native-view.v2");
  hash.integer(reinterpret_cast<std::uintptr_t>(value.storage().get()));
  hash.integer(value.layout().byte_offset);
  hash.integer(static_cast<std::uint32_t>(value.descriptor().element_type));
  hash.integer(value.descriptor().shape.size());
  for (auto n : value.descriptor().shape)
    hash.integer(n);
  hash.integer(value.layout().origin.size());
  for (auto n : value.layout().origin)
    hash.integer(n);
  hash.integer(value.layout().byte_strides.size());
  for (auto n : value.layout().byte_strides)
    hash.integer(static_cast<std::uint64_t>(n));
  hash.integer(value.region().rank());
  for (auto d : value.region().dimensions()) {
    hash.integer(d.offset);
    hash.integer(d.extent);
  }
  return hash.finish();
}

/** @brief Native physical upload identity, excluding Result semantic facts. */
Result<std::string> upload_content_key(const Value& value,
                                       const std::string& device,
                                       const std::function<ErrorCode()>& stop) {
  content_internal::Sha256 hash;
  hash.text("photospider.native-upload.v2");
  hash.text(device);
  hash.integer(static_cast<std::uint32_t>(value.descriptor().element_type));
  hash.integer(value.descriptor().shape.size());
  for (auto n : value.descriptor().shape)
    hash.integer(n);
  hash.integer(value.region().rank());
  for (auto d : value.region().dimensions()) {
    hash.integer(d.offset);
    hash.integer(d.extent);
  }
  auto count = value.region().element_count();
  if (!count.ok())
    return Result<std::string>(count.status());
  const auto width = Value::element_size(value.descriptor().element_type);
  std::vector<std::uint64_t> coordinate(value.region().rank());
  for (std::uint64_t i = 0; i < count.value(); ++i) {
    if ((i & 1023) == 0) {
      const auto code = stop();
      if (code != ErrorCode::Ok)
        return Result<std::string>(Status{code, {}});
    }
    auto index = i;
    for (std::size_t axis = coordinate.size(); axis > 0; --axis) {
      const auto d = value.region().dimensions()[axis - 1];
      coordinate[axis - 1] = d.offset + index % d.extent;
      index /= d.extent;
    }
    auto address = value.byte_address(coordinate);
    if (!address.ok())
      return Result<std::string>(address.status());
    hash.bytes(value.bytes().data() + address.value(), width);
  }
  return Result<std::string>(hash.finish());
}

/** @brief Binds this observation's metadata to an immutable packed backing. */
Result<Value> uploaded_view(const Value& source,
                            std::shared_ptr<const CpuStorage> backing) {
  auto packed = source.descriptor();
  for (std::size_t i = 0; i < source.region().rank(); ++i)
    packed.shape[i] = source.region().dimensions()[i].extent;
  auto dense = input_internal::dense_metadata(packed);
  if (!dense.ok())
    return Result<Value>(dense.status());
  auto layout = dense.take_value().layout;
  for (const auto dim : source.region().dimensions())
    layout.origin.push_back(dim.offset);
  return Value::from_storage(source.descriptor(), source.region(),
                             std::move(layout), std::move(backing),
                             source.facets(), source.resources());
}

}  // namespace ps::execution_internal
