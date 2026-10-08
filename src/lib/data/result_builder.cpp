#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstring>
#include <functional>
#include <memory>
#include <mutex>
#include <new>
#include <numeric>
#include <set>
#include <stdexcept>
#include <string>
#include <string_view>
#include <tuple>
#include <utility>
#include <vector>

#include "data/affine_view.hpp"
#include "data/result_host_access.hpp"
#include "data/result_state.hpp"
#include "data/result_support.hpp"
#include "data/result_window_access.hpp"
#include "data/value_validation.hpp"
#include "photospider/data/representation.hpp"
#include "photospider/data/result.hpp"
#include "photospider/data/tensor_description.hpp"

namespace ps {
Status ResultBuilder::bind_descriptor_relation(ResultRelation relation) {
  if (!impl_)
    return Status{ErrorCode::Stale, {}};
  std::lock_guard<std::timed_mutex> lock(impl_->mutex);
  if (auto busy = impl_->reject_active_mutation(); !busy.ok())
    return busy;
  if (impl_->revision != 1 || impl_->descriptor_relation.valid() ||
      !relation.owned_by(impl_->budget) || relation.coverage() != 1)
    return data_internal::invalid_schema();
  impl_->descriptor_relation = std::move(relation);
  return Status::success();
}
ResultBuilder::ResultBuilder(ResultBuilder&& other) noexcept
    : impl_(std::move(other.impl_)) {}
ResultBuilder& ResultBuilder::operator=(ResultBuilder&& other) noexcept {
  if (this != &other) {
    fail(Status{ErrorCode::Cancelled, {}});
    impl_ = std::move(other.impl_);
  }
  return *this;
}
ResultBuilder::~ResultBuilder() noexcept {
  fail(Status{ErrorCode::Cancelled, {}});
}
Result<ResultBuilder> ResultBuilder::start(
    ResourceBudget budget, const SchemaTemplate& schema,
    std::string_view semantic_key, ResultGrowthLimits limits,
    std::vector<std::uint64_t> association, std::uint64_t tile_height,
    std::uint64_t tile_width, ResourceBindings resources) {
  std::optional<ResourceAllocationScope> metadata_scope;
  const auto* active_root = resource_internal::metadata_budget();
  if (!active_root || !active_root->same_owner(budget))
    metadata_scope.emplace(budget);
  auto valid = schema.validate(true);
  if (!valid.ok())
    return Result<ResultBuilder>(valid);
  if (std::any_of(schema.tensors.begin(), schema.tensors.end(),
                  [](const auto& spec) { return spec.layout.spatial; }) &&
      (!tile_height || !tile_width || (tile_height & (tile_height - 1)) ||
       (tile_width & (tile_width - 1))))
    return Result<ResultBuilder>(data_internal::invalid_schema());
  auto association_work = budget.consume({association.size()});
  if (!association_work.ok())
    return Result<ResultBuilder>(association_work);
  if (semantic_key.empty() || semantic_key.size() > 4096 ||
      std::any_of(association.begin(), association.end(),
                  [](auto id) { return id == 0; }))
    return Result<ResultBuilder>(data_internal::invalid_schema());
  auto admitted_resources = resources.reference(budget);
  if (!admitted_resources.ok())
    return Result<ResultBuilder>(admitted_resources.status());
  resources = admitted_resources.take_value();
  for (const auto& field : schema.fields)
    if (field.rows.kind == ResultExtentKind::Fixed &&
        data_internal::scaled(field.rows, field.rows.value).value() >
            limits.maximum_rows)
      return Result<ResultBuilder>(
          Status{ErrorCode::ResourceExhausted,
                 "fixed result count exceeds growth limit"});
  auto copied = schema.managed_copy(budget);
  if (!copied.ok())
    return Result<ResultBuilder>(copied.status());
  auto encoded = schema.managed_canonical(budget);
  if (!encoded.ok())
    return Result<ResultBuilder>(encoded.status());
  auto extra_work = budget.consume({semantic_key.size() + 8});
  if (!extra_work.ok())
    return Result<ResultBuilder>(extra_work);
  ResourceString schema_key = encoded.take_value();
  try {
    const auto key_size = static_cast<std::uint64_t>(semantic_key.size());
    schema_key.reserve(schema_key.size() + 8 + semantic_key.size());
    for (unsigned i = 0; i < 8; ++i)
      schema_key.push_back(static_cast<char>((key_size >> (8 * i)) & 255));
    if (!semantic_key.empty())
      schema_key.append(semantic_key.data(), semantic_key.size());
  } catch (const std::bad_alloc&) {
    return Result<ResultBuilder>(Status{ErrorCode::ResourceExhausted, {}});
  }
  const auto metadata = sizeof(ResultRef::Impl);
  auto capacity = ResourceCapacity::host(metadata, metadata);
  capacity[ResourceKind::Entries] = 1;
  auto lease = budget.reserve(capacity);
  if (!lease.ok())
    return Result<ResultBuilder>(lease.status());
  try {
    auto impl = std::shared_ptr<ResultRef::Impl>(new ResultRef::Impl(budget));
    impl->lease = lease.take_value();
    impl->schema = copied.take_value();
    auto selected_resources = impl->schema.select_resources(resources);
    if (!selected_resources.ok())
      return Result<ResultBuilder>(selected_resources.status());
    auto owned_resources = selected_resources.value().reference(impl->budget);
    if (!owned_resources.ok())
      return Result<ResultBuilder>(owned_resources.status());
    impl->resources = owned_resources.take_value();
    impl->key = std::move(schema_key);
    impl->association = ResourceVector<std::uint64_t>(
        association.begin(), association.end(),
        ResourceAllocator<std::uint64_t>(impl->budget));
    impl->limits = limits;
    for (std::uint32_t i = 0; i < impl->schema.fields.size(); ++i)
      impl->fields[i].row_bytes = impl->schema.row_bytes(i).value();
    auto page_budget = std::make_shared<PlanarPageBudget>(
        limits.maximum_bytes,
        [root = impl->budget](std::uint64_t bytes,
                              bool metadata) -> Result<std::shared_ptr<void>> {
          auto capacity = ResourceCapacity::host(bytes, metadata ? bytes : 0);
          capacity[ResourceKind::Payload] = metadata ? 0 : bytes;
          auto charged = root.reserve(capacity);
          if (!charged.ok())
            return Result<std::shared_ptr<void>>(charged.status());
          return Result<std::shared_ptr<void>>(
              std::make_shared<ResourceLease>(charged.take_value()));
        });
    page_budget->payload_committed_ = [](const std::shared_ptr<void>& owner,
                                         std::uint64_t bytes) {
      auto lease = std::static_pointer_cast<ResourceLease>(owner);
      if (lease)
        resource_internal::commit_payload(*lease, bytes);
    };
    impl->image_budget = page_budget;
    for (std::size_t i = 0; i < impl->schema.tensors.size(); ++i) {
      const auto& spec = impl->schema.tensors[i];
      auto& image = impl->tensors[i];
      auto initialized = image.initialize(impl->budget, spec, page_budget,
                                          limits, tile_height, tile_width);
      if (!initialized.ok())
        return Result<ResultBuilder>(initialized);
    }
    static std::atomic<std::uint64_t> next{1};
    auto id = next.load();
    do {
      if (id == UINT64_MAX)
        return Result<ResultBuilder>(Status{ErrorCode::ResourceExhausted,
                                            "result object ids exhausted"});
    } while (!next.compare_exchange_weak(id, id + 1));
    impl->object = id;
    ResultBuilder builder;
    builder.impl_ = std::move(impl);
    return Result<ResultBuilder>(std::move(builder));
  } catch (const std::bad_alloc&) {
    return Result<ResultBuilder>(Status{ErrorCode::ResourceExhausted, {}});
  }
}
ResultRef ResultBuilder::reference() const noexcept {
  ResultRef result;
  result.impl_ = impl_;
  return result;
}
void ResultBuilder::fail(Status status) noexcept {
  if (!impl_)
    return;
  std::lock_guard<std::timed_mutex> lock(impl_->mutex);
  if (!impl_->complete && impl_->status().ok())
    impl_->failure.record(status);
}
Result<ResultRef> ResultBuilder::seal() {
  if (!impl_)
    return Result<ResultRef>(Status{ErrorCode::Stale, {}});
  std::lock_guard<std::timed_mutex> lock(impl_->mutex);
  if (auto busy = impl_->reject_active_mutation(); !busy.ok())
    return Result<ResultRef>(busy);
  if (impl_->complete || !impl_->status().ok())
    return Result<ResultRef>(impl_->status().ok() ? Status{ErrorCode::Stale, {}}
                                                  : impl_->status());
  auto reject = [&](Status status) {
    impl_->failure.record(status);
    return Result<ResultRef>(status);
  };
  if (!impl_->descriptor_relation.valid())
    return reject(data_internal::invalid_schema());
  for (std::size_t i = 0; i < impl_->schema.tensors.size(); ++i)
    if (!impl_->tensors[i].relation.valid())
      return reject(data_internal::invalid_schema());
  for (std::uint32_t i = 0; i < impl_->schema.fields.size(); ++i) {
    const auto& field = impl_->schema.fields[i];
    const auto& state = impl_->fields[i];
    if (!state.relation.valid() || state.certified != state.written)
      return reject(data_internal::invalid_schema());
    if (field.rows.kind != ResultExtentKind::RuntimeCount) {
      const auto count = field.rows.kind == ResultExtentKind::FieldRows
                             ? impl_->fields[field.rows.field].written
                             : field.rows.value;
      auto expected = data_internal::scaled(field.rows, count);
      if (!expected.ok() || expected.value() != state.written)
        return reject(data_internal::invalid_schema());
    }
  }
  if (impl_->revision == UINT64_MAX)
    return reject(data_internal::invalid_schema());
  for (auto& field : impl_->fields)
    if (field.storage.valid()) {
      auto sealed = field.storage.seal();
      if (!sealed.ok())
        return reject(sealed);
    }
  impl_->complete = true;
  ++impl_->revision;
  return Result<ResultRef>(reference());
}
}  // namespace ps
