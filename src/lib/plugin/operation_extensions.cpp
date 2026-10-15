#include "photospider/plugin/operation_extensions.hpp"

#include <algorithm>
#include <cstring>
#include <memory>
#include <utility>
#include <vector>

#include "photospider/core/utf8_validation.hpp"
#include "plugin/operation_exception.hpp"

namespace ps {
// NOLINTBEGIN(whitespace/indent_namespace)
DiagnosticAccumulator::Record::Record(
    std::shared_ptr<const DiagnosticDefinition> definition,
    const ResourceBudget& root)
    : type(std::move(definition)),
      value(ResourceAllocator<std::uint8_t>(root)),
      next(ResourceAllocator<std::uint8_t>(root)) {}
// NOLINTEND
DiagnosticAccumulator::DiagnosticAccumulator(const ResourceBudget& resources)
    : resources_(resources), records_(ResourceAllocator<Record>(resources)) {}
DiagnosticAccumulator::DiagnosticAccumulator(const DiagnosticAccumulator& other)
    : DiagnosticAccumulator(other.resources_) {
  records_.reserve(other.records_.size());
  for (const auto& source : other.records_) {
    auto scratch = resources_.reserve(ResourceCapacity::host(
        source.type->scratch_bytes, source.type->scratch_bytes));
    if (!scratch.ok())
      throw std::bad_alloc();
    records_.emplace_back(source.type, resources_);
    auto& target = records_.back();
    target.scratch = scratch.take_value();
    target.value = source.value;
    target.next.resize(source.next.size());
    target.reported = source.reported;
  }
}
DiagnosticAccumulator& DiagnosticAccumulator::operator=(
    const DiagnosticAccumulator& other) {
  if (this != &other) {
    DiagnosticAccumulator copy(other);
    *this = std::move(copy);
  }
  return *this;
}
Status DiagnosticAccumulator::initialize(
    const std::vector<std::shared_ptr<const DiagnosticDefinition>>& types) try {
  if (types.size() > 64)
    return {ErrorCode::InvalidArgument, "too many diagnostic types"};
  ResourceVector<Record> admitted{ResourceAllocator<Record>(resources_)};
  admitted.reserve(types.size());
  for (const auto& type : types) {
    if (!type || !type->payload_bytes || type->payload_bytes > 65536 ||
        !type->validate || !type->merge || !type->identity.version ||
        !core_internal::valid_utf8_key(type->identity.key) ||
        !core_internal::valid_utf8_key(type->identity.implementation) ||
        std::any_of(admitted.begin(), admitted.end(), [&](const auto& record) {
          return record.type->identity.key == type->identity.key;
        }))
      return {ErrorCode::InvalidArgument, "invalid diagnostic definition"};
    admitted.emplace_back(type, resources_);
    auto& record = admitted.back();
    auto work = resources_.consume({type->payload_bytes});
    if (!work.ok())
      return work;
    record.value.resize(type->payload_bytes);
    record.next.resize(type->payload_bytes);
    auto scratch = resources_.reserve(
        ResourceCapacity::host(type->scratch_bytes, type->scratch_bytes));
    if (!scratch.ok())
      return scratch.status();
    record.scratch = scratch.take_value();
  }
  records_.swap(admitted);
  return Status::success();
} catch (...) {
  return plugin_internal::current_operation_exception();
}
Status DiagnosticAccumulator::report(const ExtensionIdentity& type,
                                     ByteView report) try {
  const auto found =
      std::find_if(records_.begin(), records_.end(),
                   [&](const auto& r) { return r.type->identity == type; });
  if (found == records_.end() || report.size() != found->value.size() ||
      !report.data())
    return {ErrorCode::InvalidArgument, "undeclared diagnostic report"};
  auto status = resources_.consume({report.size()});
  if (!status.ok())
    return status;
  status = found->type->validate(report);
  if (!status.ok())
    return status;
  std::fill(found->next.begin(), found->next.end(), 0);
  status = found->type->merge({found->value.data(), found->value.size()},
                              report, {found->next.data(), found->next.size()});
  if (!status.ok())
    return status;
  status = found->type->validate({found->next.data(), found->next.size()});
  if (!status.ok())
    return status;
  found->value.swap(found->next);
  found->reported = true;
  return Status::success();
} catch (...) {
  return plugin_internal::current_operation_exception();
}
Status DiagnosticAccumulator::report(std::uint32_t index, ByteView bytes) {
  if (index >= records_.size())
    return {ErrorCode::InvalidArgument, "undeclared diagnostic type index"};
  return report(records_[index].type->identity, bytes);
}
Status DiagnosticAccumulator::merge(const DiagnosticAccumulator& other) try {
  for (const auto& record : other.records_) {
    if (!record.reported)
      continue;
    auto found =
        std::find_if(records_.begin(), records_.end(), [&](const auto& r) {
          return r.type->identity == record.type->identity;
        });
    if (found == records_.end() || found->value.size() != record.value.size())
      return {ErrorCode::InvalidArgument,
              "incompatible diagnostic accumulator"};
    const ByteView incoming(record.value.data(), record.value.size());
    auto status = resources_.consume({incoming.size()});
    if (!status.ok())
      return status;
    status = found->type->validate(incoming);
    if (!status.ok())
      return status;
    std::fill(found->next.begin(), found->next.end(), 0);
    status =
        found->type->merge({found->value.data(), found->value.size()}, incoming,
                           {found->next.data(), found->next.size()});
    if (!status.ok())
      return status;
    status = found->type->validate({found->next.data(), found->next.size()});
    if (!status.ok())
      return status;
  }
  for (const auto& record : other.records_) {
    if (!record.reported)
      continue;
    auto found =
        std::find_if(records_.begin(), records_.end(), [&](const auto& r) {
          return r.type->identity == record.type->identity;
        });
    found->value.swap(found->next);
    found->reported = true;
  }
  return Status::success();
} catch (...) {
  return plugin_internal::current_operation_exception();
}
Result<ByteView> DiagnosticAccumulator::find(
    const ExtensionIdentity& type) const {
  for (const auto& record : records_)
    if (record.type->identity == type && record.reported)
      return Result<ByteView>(
          ByteView(record.value.data(), record.value.size()));
  return Result<ByteView>(
      Status{ErrorCode::NotFound, "diagnostic not reported"});
}
void DiagnosticAccumulator::clear() noexcept {
  for (auto& record : records_) {
    std::fill(record.value.begin(), record.value.end(), 0);
    record.reported = false;
  }
}
}  // namespace ps
