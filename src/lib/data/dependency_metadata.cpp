#include "data/dependency_metadata.hpp"

#include <string>
#include <utility>

namespace ps {
DependencyCertificate::DependencyCertificate(const DependencyCertificate& other)
    : metadata_owner_(dependency_internal::metadata_owner(
          dependency_internal::certificate_bytes(
              other.identity_, other.coverage_, other.input_shapes_,
              other.rows_, other.pieces_, true),
          other.metadata_owner_)),
      identity_(other.identity_),
      coverage_(other.coverage_),
      input_shapes_(other.input_shapes_),
      rows_(other.rows_),
      mapped_(other.mapped_),
      pieces_(other.pieces_),
      metadata_entries_(other.metadata_entries_),
      storage_entries_(other.storage_entries_) {
}  // NOLINT(whitespace/indent_namespace)
DependencyCertificate::DependencyCertificate(const DependencyCertificate& other,
                                             std::string identity)
    : metadata_owner_(dependency_internal::metadata_owner(
          dependency_internal::certificate_bytes(
              identity, other.coverage_, other.input_shapes_, other.rows_,
              other.pieces_, true),
          other.metadata_owner_)),
      identity_(std::move(identity)),
      coverage_(other.coverage_),
      input_shapes_(other.input_shapes_),
      rows_(other.rows_),
      mapped_(other.mapped_),
      pieces_(other.pieces_),
      metadata_entries_(other.metadata_entries_),
      storage_entries_(other.storage_entries_) {
}  // NOLINT(whitespace/indent_namespace)
void DependencyCertificate::swap(DependencyCertificate& other) noexcept {
  using std::swap;
  swap(metadata_owner_, other.metadata_owner_);
  swap(identity_, other.identity_);
  swap(coverage_, other.coverage_);
  swap(input_shapes_, other.input_shapes_);
  swap(rows_, other.rows_);
  swap(mapped_, other.mapped_);
  swap(pieces_, other.pieces_);
  swap(metadata_entries_, other.metadata_entries_);
  swap(storage_entries_, other.storage_entries_);
}
DependencyCertificate& DependencyCertificate::operator=(
    const DependencyCertificate& other) {
  if (this != &other) {
    DependencyCertificate replacement(other);
    swap(replacement);
  }
  return *this;
}
DependencyCertificate& DependencyCertificate::operator=(
    DependencyCertificate&& other) noexcept {
  if (this != &other) {
    DependencyCertificate replacement(std::move(other));
    swap(replacement);
  }
  return *this;
}
}  // namespace ps
