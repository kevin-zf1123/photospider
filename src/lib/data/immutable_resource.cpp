#include "photospider/data/immutable_resource.hpp"

#include <algorithm>
#include <cstring>
#include <memory>
#include <stdexcept>
#include <utility>

#include "photospider/core/content_digest.hpp"

namespace ps {
namespace {
Status consume(const ResourceBudget& resources,
               const CancellationToken& cancellation, std::uint64_t* remaining,
               std::uint64_t count) {
  if (cancellation.cancelled())
    return {ErrorCode::Cancelled, {}};
  if (count > *remaining)
    return {ErrorCode::ResourceExhausted, "immutable resource work limit",
            FailureReason::WorkLimit};
  *remaining -= count;
  return resources.consume({count});
}
}  // namespace
Result<ImmutableResource> ImmutableResource::import(
    ByteView bytes, const ResourceBudget& resources,
    const CancellationToken& cancellation, std::uint64_t maximum_work) {
  using Answer = Result<ImmutableResource>;
  auto status = consume(resources, cancellation, &maximum_work, 0);
  if (!status.ok())
    return Answer(status);
  if (bytes.empty() || !bytes.data())
    return Answer(
        Status{ErrorCode::InvalidArgument, "empty or null resource bytes"});
  auto allocated = resources.allocator().allocate(bytes.size());
  if (!allocated.ok())
    return Answer(allocated.status());
  auto buffer = allocated.take_value();
  for (std::size_t offset = 0; offset < bytes.size();) {
    const auto count = std::min<std::size_t>(65536, bytes.size() - offset);
    status = consume(resources, cancellation, &maximum_work, count);
    if (!status.ok())
      return Answer(status);
    std::memcpy(buffer.data() + offset, bytes.data() + offset, count);
    offset += count;
  }
  return from_storage(std::move(buffer).freeze(), resources, cancellation,
                      maximum_work);
}
Result<ImmutableResource> ImmutableResource::from_storage(
    std::shared_ptr<const CpuStorage> storage, const ResourceBudget& resources,
    const CancellationToken& cancellation, std::uint64_t maximum_work) {
  using Answer = Result<ImmutableResource>;
  auto status = consume(resources, cancellation, &maximum_work, 0);
  if (!status.ok())
    return Answer(status);
  if (!storage || storage->bytes().empty())
    return Answer(Status{ErrorCode::InvalidArgument, "empty resource storage"});
  auto admitted = resources.reference(storage);
  if (!admitted.ok())
    return Answer(admitted.status());
  auto scratch = resources.reserve(ResourceCapacity::host(128, 128));
  if (!scratch.ok())
    return Answer(scratch.status());
  content_internal::Sha256 hash;
  const auto bytes = storage->bytes();
  for (std::size_t offset = 0; offset < bytes.size();) {
    const auto count = std::min<std::size_t>(65536, bytes.size() - offset);
    status = consume(resources, cancellation, &maximum_work, count);
    if (!status.ok())
      return Answer(status);
    hash.bytes(bytes.data() + offset, count);
    offset += count;
  }
  ResourceIdentity identity;
  identity.byte_length = bytes.size();
  const auto digest = hash.finish();
  const auto digit = [](char c) { return c <= '9' ? c - '0' : c - 'a' + 10; };
  for (unsigned i = 0; i < 32; ++i)
    identity.sha256[i] = static_cast<std::uint8_t>(16 * digit(digest[2 * i]) +
                                                   digit(digest[2 * i + 1]));
  status = consume(resources, cancellation, &maximum_work, 0);
  return status.ok()
             ? Answer(ImmutableResource(admitted.take_value(), identity))
             : Answer(status);
}
const ResourceIdentity& ImmutableResource::identity() const {
  if (!valid())
    throw std::logic_error("invalid immutable resource");
  return identity_;
}
const std::shared_ptr<const CpuStorage>& ImmutableResource::storage() const {
  if (!valid())
    throw std::logic_error("invalid immutable resource");
  return storage_;
}
ByteView ImmutableResource::bytes() const {
  return storage()->bytes();
}
Result<ImmutableResource> ImmutableResource::reference(
    const ResourceBudget& resources) const {
  if (!valid())
    return Result<ImmutableResource>(
        Status{ErrorCode::InvalidArgument, "invalid immutable resource"});
  auto admitted = resources.reference(storage_);
  if (!admitted.ok())
    return Result<ImmutableResource>(admitted.status());
  return Result<ImmutableResource>(
      ImmutableResource(admitted.take_value(), identity_));
}
}  // namespace ps
