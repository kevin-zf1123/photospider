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
#include "photospider/data/result.hpp"
#include "photospider/data/tensor_description.hpp"

namespace ps {
namespace data_internal {
Status ResultFieldBacking::append(const ResourceBudget& budget,
                                  std::uint64_t rows, ByteView bytes,
                                  const CancellationToken& cancellation) {
  if (cancellation.cancelled())
    return Status{ErrorCode::Cancelled, {}};
  if (!bytes.data() || bytes.empty())
    return Status{ErrorCode::InvalidArgument, "invalid field append bytes"};
  auto charged = budget.consume({bytes.size()});
  if (!charged.ok())
    return charged;
  try {
    auto target = storage;
    if (!target)
      target = std::allocate_shared<Storage>(ResourceAllocator<Storage>(budget),
                                             budget);
    auto allocated = budget.allocator().allocate(bytes.size());
    if (!allocated.ok())
      return allocated.status();
    auto buffer = allocated.take_value();
    std::memcpy(buffer.data(), bytes.data(), bytes.size());
    if (cancellation.cancelled())
      return Status{ErrorCode::Cancelled, {}};
    if (target->chunks.size() == target->chunks.capacity()) {
      auto metadata_work = budget.consume({target->chunks.size()});
      if (!metadata_work.ok())
        return metadata_work;
    }
    // Publish only after payload admission and metadata growth succeed.
    target->chunks.push_back({target->size, std::move(buffer).freeze()});
    target->size += bytes.size();
    storage = std::move(target);
    written += rows;
    return Status::success();
  } catch (const std::bad_alloc&) {
    return Status{ErrorCode::ResourceExhausted, {}};
  }
}
Result<std::shared_ptr<const CpuStorage>> ResultFieldBacking::read(
    const ResourceBudget& budget, std::uint64_t offset, std::uint64_t bytes,
    std::uint64_t maximum_window, const CancellationToken& cancellation) const {
  using Answer = Result<std::shared_ptr<const CpuStorage>>;
  if (cancellation.cancelled())
    return Answer(Status{ErrorCode::Cancelled, {}});
  if (!storage || !bytes || offset > storage->size ||
      bytes > storage->size - offset)
    return Answer(
        Status{ErrorCode::InvalidArgument, "invalid field read range"});
  if (bytes > maximum_window)
    return Answer(Status{ErrorCode::ResourceExhausted,
                         "field read exceeds window limit",
                         FailureReason::CapacityLimit});
  std::uint64_t lookup_work = 1;
  for (auto size = storage->chunks.size(); size > 1; size /= 2)
    ++lookup_work;
  auto charged = budget.consume({lookup_work});
  if (!charged.ok())
    return Answer(charged);
  charged = budget.consume({bytes});
  if (!charged.ok())
    return Answer(charged);
  auto allocated = budget.allocator().allocate(bytes);
  if (!allocated.ok())
    return Answer(allocated.status());
  auto buffer = allocated.take_value();
  const auto& chunks = storage->chunks;
  auto first = std::upper_bound(
      chunks.begin(), chunks.end(), offset,
      [](std::uint64_t at, const Chunk& chunk) { return at < chunk.offset; });
  if (first != chunks.begin())
    --first;
  std::uint64_t copied = 0;
  for (; first != chunks.end() && copied < bytes; ++first) {
    if (cancellation.cancelled())
      return Answer(Status{ErrorCode::Cancelled, {}});
    const auto within = offset + copied - first->offset;
    const auto data = first->bytes->bytes();
    const auto count =
        std::min<std::uint64_t>(bytes - copied, data.size() - within);
    std::memcpy(buffer.data() + copied, data.data() + within, count);
    copied += count;
  }
  if (copied != bytes)
    return Answer(Status{ErrorCode::Internal, "field backing is incomplete"});
  if (cancellation.cancelled())
    return Answer(Status{ErrorCode::Cancelled, {}});
  return Answer(std::move(buffer).freeze());
}
}  // namespace data_internal
struct ResultReadPlan::Impl {
  ResourceLease lease;
  std::shared_ptr<ResultRef::Impl> result;
  std::uint32_t field = 0;
  std::uint64_t offset = 0, bytes = 0;
};
bool ResultReadPlan::owned_by(const ResourceBudget& budget) const noexcept {
  return impl_ && impl_->result->budget.same_owner(budget);
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
    plan->field = field;
    plan->offset = first * impl_->fields[field].row_bytes;
    plan->bytes = rows * impl_->fields[field].row_bytes;
    ResultReadPlan result;
    result.impl_ = std::move(plan);
    return Result<ResultReadPlan>(std::move(result));
  } catch (const std::bad_alloc&) {
    return Result<ResultReadPlan>(Status{ErrorCode::ResourceExhausted, {}});
  }
}
std::uint64_t ResultReadPlan::byte_size() const noexcept {
  return impl_ ? impl_->bytes : 0;
}
Result<std::shared_ptr<const CpuStorage>> ResultReadPlan::load(
    std::uint64_t maximum_window, const CancellationToken& cancellation) const {
  if (!impl_ || !impl_->bytes)
    return Result<std::shared_ptr<const CpuStorage>>(
        Status{ErrorCode::InvalidArgument, "empty/invalid result read"});
  Result<std::shared_ptr<const CpuStorage>> loaded(
      Status{ErrorCode::Internal, {}});
  {
    std::lock_guard<std::timed_mutex> lock(impl_->result->mutex);
    loaded = impl_->result->fields[impl_->field].read(
        impl_->result->budget, impl_->offset, impl_->bytes, maximum_window,
        cancellation);
  }
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
Status ResultBuilder::publish(std::uint32_t field, std::uint64_t end) {
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
  if (field >= impl_->schema.fields.size())
    return reject(data_internal::invalid_schema());
  auto& target = impl_->fields[field];
  if (end < target.certified || end > target.written ||
      impl_->revision == UINT64_MAX)
    return reject(data_internal::invalid_schema());
  target.certified = end;
  ++impl_->revision;
  return Status::success();
}
}  // namespace ps
