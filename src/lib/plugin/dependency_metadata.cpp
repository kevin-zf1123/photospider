#include "data/dependency_metadata.hpp"

#include <utility>
#include <vector>

#include "photospider/plugin/dependency_program.hpp"

namespace ps {
namespace {
std::uint64_t batch_bytes(const std::vector<AtomCertificate>& rows,
                          const std::vector<DependencyNeed>& needs, bool copy) {
  dependency_internal::MetadataBytes bytes;
  bytes.rows(rows, copy);
  bytes.needs(needs, copy);
  return bytes.bytes;
}
}  // namespace
DependencyNeedBatch::DependencyNeedBatch(std::vector<AtomCertificate> rows,
                                         std::vector<DependencyNeed> needs,
                                         bool mapping)
    : metadata_owner_(
          dependency_internal::metadata_owner(batch_bytes(rows, needs, false))),
      associations(std::move(rows)),
      request_needs(std::move(needs)),
      static_mapping(mapping) {}  // NOLINT(whitespace/indent_namespace)
DependencyNeedBatch::DependencyNeedBatch(const DependencyNeedBatch& other)
    : metadata_owner_(dependency_internal::metadata_owner(
          batch_bytes(other.associations, other.request_needs, true),
          other.metadata_owner_)),
      associations(other.associations),
      request_needs(other.request_needs),
      static_mapping(other.static_mapping) {
}  // NOLINT(whitespace/indent_namespace)
void DependencyNeedBatch::reseal_metadata() {
  const auto bytes = batch_bytes(associations, request_needs, false);
  const auto* active = resource_internal::metadata_budget();
  if (!metadata_owner_ ||
      (active && !metadata_owner_->root.same_owner(*active))) {
    metadata_owner_ =
        dependency_internal::metadata_owner(bytes, metadata_owner_);
    return;
  }
  auto lease = metadata_owner_->lease;
  const auto before = metadata_owner_->heap_capacity;
  auto status =
      bytes > before
          ? lease.grow(ResourceCapacity::host(bytes - before, bytes - before))
          : lease.shrink(
                ResourceCapacity::host(before - bytes, before - bytes));
  if (!status.ok()) {
    resource_internal::metadata_failure(metadata_owner_->root,
                                        ErrorCode::ResourceExhausted);
    throw std::bad_alloc();
  }
  metadata_owner_->heap_capacity = bytes;
}
void DependencyNeedBatch::swap(DependencyNeedBatch& other) noexcept {
  using std::swap;
  swap(metadata_owner_, other.metadata_owner_);
  swap(associations, other.associations);
  swap(request_needs, other.request_needs);
  swap(static_mapping, other.static_mapping);
}
DependencyNeedBatch& DependencyNeedBatch::operator=(
    const DependencyNeedBatch& other) {
  if (this != &other) {
    DependencyNeedBatch replacement(other);
    swap(replacement);
  }
  return *this;
}
DependencyNeedBatch& DependencyNeedBatch::operator=(
    DependencyNeedBatch&& other) noexcept {
  if (this != &other) {
    DependencyNeedBatch replacement(std::move(other));
    swap(replacement);
  }
  return *this;
}
}  // namespace ps
