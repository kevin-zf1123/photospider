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

#include "core/checked_math.hpp"
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
namespace data_internal {
Status ResultFieldBacking::append(const ResourceBudget& budget,
                                  std::uint64_t rows, ByteView bytes,
                                  const CancellationToken& cancellation) {
  if (!storage.valid()) {
    auto file = TemporaryStorage::create(budget);
    if (!file.ok())
      return file.status();
    storage = file.take_value();
  }
  auto extended = storage.append_zeroed(bytes.size(), cancellation);
  if (!extended.ok())
    return extended.status();
  auto status = storage.write(extended.value(), bytes, cancellation);
  if (!status.ok())
    return status;
  written += rows;
  return Status::success();
}
}  // namespace data_internal
struct ResultReadPlan::Impl {
  ResourceLease lease;
  std::shared_ptr<ResultRef::Impl> result;
  TemporaryStorage storage;
  std::uint64_t offset = 0, bytes = 0;
};
struct ResultWritePlan::Impl {
  ResourceLease lease;
  std::shared_ptr<ResultRef::Impl> result;
  std::shared_ptr<const CpuStorage> payload;
  std::uint32_t field = 0;
  std::uint64_t rows = 0;
  std::atomic<bool> applied{false};
};
bool ResultWritePlan::owned_by(const ResourceBudget& budget) const noexcept {
  return impl_ && impl_->result->budget.same_owner(budget);
}
bool ResultReadPlan::owned_by(const ResourceBudget& budget) const noexcept {
  return impl_ && impl_->result->budget.same_owner(budget);
}
std::uint64_t ResultWritePlan::byte_size() const noexcept {
  return impl_ && impl_->payload ? impl_->payload->capacity() : 0;
}
Status ResultWritePlan::apply(const CancellationToken& cancellation) const {
  if (!impl_ || impl_->applied.exchange(true))
    return Status{ErrorCode::Stale, "result write command already applied"};
  return ResultBuilder::append_to(impl_->result, impl_->field, impl_->rows,
                                  impl_->payload->bytes(), cancellation);
}
Result<ResultWritePlan> ResultBuilder::prepare_append(
    std::uint32_t field, std::uint64_t rows,
    std::shared_ptr<const CpuStorage> payload) const {
  if (!impl_ || !payload)
    return Result<ResultWritePlan>(Status{ErrorCode::Stale, {}});
  auto retained = impl_->budget.reference(std::move(payload));
  if (!retained.ok())
    return Result<ResultWritePlan>(retained.status());
  auto capacity = ResourceCapacity::host(sizeof(ResultWritePlan::Impl),
                                         sizeof(ResultWritePlan::Impl));
  capacity[ResourceKind::Queue] = 1;
  auto lease = impl_->budget.reserve(capacity);
  if (!lease.ok())
    return Result<ResultWritePlan>(lease.status());
  try {
    auto plan = std::make_shared<ResultWritePlan::Impl>();
    plan->lease = lease.take_value();
    plan->result = impl_;
    plan->payload = retained.take_value();
    plan->field = field;
    plan->rows = rows;
    ResultWritePlan result;
    result.impl_ = std::move(plan);
    return Result<ResultWritePlan>(std::move(result));
  } catch (const std::bad_alloc&) {
    return Result<ResultWritePlan>(Status{ErrorCode::ResourceExhausted, {}});
  }
}
Result<ResultReadPlan> ResultRef::prepare_read(const ResultDescriptor& facts,
                                               std::uint32_t field,
                                               std::uint64_t first,
                                               std::uint64_t rows) const {
  if (!impl_ ||
      (captured_ && facts.revision() > captured_->descriptor.revision()))
    return Result<ResultReadPlan>(Status{ErrorCode::Stale, {}});
  std::lock_guard<std::timed_mutex> lock(impl_->mutex);
  if (facts.object_ != impl_->object || !facts.revision_ ||
      facts.revision_ > impl_->revision ||
      facts.field_count_ != impl_->schema.fields.size() ||
      field >= facts.field_count_ || first > facts.rows_[field] ||
      rows > facts.rows_[field] - first ||
      facts.rows_[field] > impl_->fields[field].certified)
    return Result<ResultReadPlan>(
        Status{ErrorCode::Stale, "invalid result descriptor/range"});
  auto capacity = ResourceCapacity::host(sizeof(ResultReadPlan::Impl),
                                         sizeof(ResultReadPlan::Impl));
  capacity[ResourceKind::Entries] = 1;
  auto lease = impl_->budget.reserve(capacity);
  if (!lease.ok())
    return Result<ResultReadPlan>(lease.status());
  try {
    auto plan = std::make_shared<ResultReadPlan::Impl>();
    plan->lease = lease.take_value();
    plan->result = impl_;
    plan->storage = impl_->fields[field].storage;
    plan->offset = first * impl_->fields[field].row_bytes;
    plan->bytes = rows * impl_->fields[field].row_bytes;
    ResultReadPlan result;
    result.impl_ = std::move(plan);
    return Result<ResultReadPlan>(std::move(result));
  } catch (const std::bad_alloc&) {
    return Result<ResultReadPlan>(Status{ErrorCode::ResourceExhausted, {}});
  }
}
Result<ResultRelation> ResultRef::relation(std::uint32_t field) const {
  if (captured_)
    return field < captured_->descriptor.field_count()
               ? Result<ResultRelation>(captured_->fields[field])
               : Result<ResultRelation>(data_internal::unavailable());
  if (!impl_)
    return Result<ResultRelation>(Status{ErrorCode::Stale, {}});
  std::lock_guard<std::timed_mutex> lock(impl_->mutex);
  if (field >= impl_->schema.fields.size() ||
      !impl_->fields[field].relation.valid() ||
      (!impl_->complete &&
       impl_->schema.publication == PublishPolicy::CompleteBundle))
    return Result<ResultRelation>(data_internal::unavailable());
  return Result<ResultRelation>(impl_->fields[field].relation);
}
std::uint64_t ResultReadPlan::byte_size() const noexcept {
  return impl_ ? impl_->bytes : 0;
}
Result<std::shared_ptr<const CpuStorage>> ResultReadPlan::load(
    std::uint64_t maximum_window, const CancellationToken& cancellation) const {
  if (!impl_ || !impl_->bytes)
    return Result<std::shared_ptr<const CpuStorage>>(
        Status{ErrorCode::InvalidArgument, "empty/invalid result read"});
  auto loaded = impl_->storage.read(impl_->offset, impl_->bytes, maximum_window,
                                    cancellation);
  if (!loaded.ok())
    return loaded;
  struct Owner {
    ResourceLease lease;
    std::shared_ptr<const Impl> plan;
    std::shared_ptr<const CpuStorage> storage;
  };
  auto lease = impl_->result->budget.reserve(
      ResourceCapacity::host(sizeof(Owner), sizeof(Owner)));
  if (!lease.ok())
    return Result<std::shared_ptr<const CpuStorage>>(lease.status());
  try {
    auto owner = std::shared_ptr<Owner>(
        new Owner{lease.take_value(), impl_, loaded.take_value()});
    const auto* storage = owner->storage.get();
    return Result<std::shared_ptr<const CpuStorage>>(
        std::shared_ptr<const CpuStorage>(std::move(owner), storage));
  } catch (const std::bad_alloc&) {
    return Result<std::shared_ptr<const CpuStorage>>(
        Status{ErrorCode::ResourceExhausted, {}});
  }
}
Status ResultBuilder::append(std::uint32_t field, std::uint64_t rows,
                             ByteView bytes,
                             const CancellationToken& cancellation) {
  return append_to(impl_, field, rows, bytes, cancellation);
}
Status ResultBuilder::append_to(const std::shared_ptr<ResultRef::Impl>& impl,
                                std::uint32_t field, std::uint64_t rows,
                                ByteView bytes,
                                const CancellationToken& cancellation) {
  if (!impl)
    return Status{ErrorCode::Stale, {}};
  std::lock_guard<std::timed_mutex> lock(impl->mutex);
  if (auto busy = impl->reject_active_mutation(); !busy.ok())
    return busy;
  if (impl->complete || !impl->status().ok())
    return impl->status().ok() ? Status{ErrorCode::Stale, {}} : impl->status();
  auto reject = [&](Status status) {
    impl->failure.record(status);
    return status;
  };
  if (cancellation.cancelled())
    return reject(Status{ErrorCode::Cancelled, {}});
  if (field >= impl->schema.fields.size())
    return reject(data_internal::invalid_schema());
  auto& target = impl->fields[field];
  if (!core_internal::can_add(rows, target.written,
                              impl->limits.maximum_rows) ||
      !core_internal::can_multiply(rows, target.row_bytes) ||
      rows * target.row_bytes != bytes.size() ||
      !core_internal::can_add(bytes.size(), impl->bytes,
                              impl->limits.maximum_bytes))
    return reject(
        Status{ErrorCode::ResourceExhausted, "result append count/byte limit"});
  if (!rows)
    return Status::success();
  auto written = target.append(impl->budget, rows, bytes, cancellation);
  if (!written.ok())
    return reject(written);
  impl->bytes += bytes.size();
  return Status::success();
}
Status ResultBuilder::publish(std::uint32_t field, std::uint64_t end,
                              ResultRelation relation,
                              ResultFinality finality) {
  if (!impl_)
    return Status{ErrorCode::Stale, {}};
  std::lock_guard<std::timed_mutex> lock(impl_->mutex);
  if (auto busy = impl_->reject_active_mutation(); !busy.ok())
    return busy;
  if (impl_->complete || !impl_->status().ok())
    return impl_->status().ok() ? Status{ErrorCode::Stale, {}}
                                : impl_->status();
  auto reject = [&](Status status) {
    impl_->failure.record(status);
    return status;
  };
  if (field >= impl_->schema.fields.size() || !finality.satisfied() ||
      !relation.owned_by(impl_->budget) || !impl_->descriptor_relation.valid())
    return reject(data_internal::invalid_schema());
  auto& target = impl_->fields[field];
  if (end < target.certified || end > target.written ||
      relation.coverage() < end || impl_->revision == UINT64_MAX)
    return reject(data_internal::invalid_schema());
  if (target.relation.valid() && !target.relation.same_owner(relation) &&
      target.certified) {
    if (target.relation.guarantee() != relation.guarantee())
      return reject(data_internal::invalid_schema());
    if (relation.guarantee() != DependencyGuarantee::Unknown) {
      const auto ordered = [](const ResultSupport& a, const ResultSupport& b) {
        return std::tie(a.input, a.roles, a.target, a.slot, a.first, a.count) <
               std::tie(b.input, b.roles, b.target, b.slot, b.first, b.count);
      };
      for (uint64_t row = 0; row < target.certified; ++row) {
        auto work = impl_->budget.consume({1});
        if (!work.ok())
          return reject(work);
        ResourceVector<ResultSupport> prior{
            ResourceAllocator<ResultSupport>(impl_->budget)},
            next{ResourceAllocator<ResultSupport>(impl_->budget)};
        auto status = target.relation.visit(row, impl_->limits.maximum_rows,
                                            [&](auto support) {
                                              prior.push_back(support);
                                              return Status::success();
                                            });
        if (!status.ok())
          return reject(status);
        status =
            relation.visit(row, impl_->limits.maximum_rows, [&](auto support) {
              next.push_back(support);
              return Status::success();
            });
        if (!status.ok())
          return reject(status);
        std::sort(prior.begin(), prior.end(), ordered);
        std::sort(next.begin(), next.end(), ordered);
        if (prior.size() != next.size() ||
            !std::equal(prior.begin(), prior.end(), next.begin(),
                        [&](const auto& a, const auto& b) {
                          return !ordered(a, b) && !ordered(b, a);
                        }))
          return reject(data_internal::invalid_schema());
      }
    }
  }
  if (target.storage.valid()) {
    auto frozen = target.storage.freeze_prefix(end * target.row_bytes);
    if (!frozen.ok())
      return reject(frozen);
  }
  target.relation = std::move(relation);
  target.certified = end;
  ++impl_->revision;
  return Status::success();
}
}  // namespace ps
