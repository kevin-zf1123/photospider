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
#include "data/tensor_validation.hpp"
#include "photospider/data/result.hpp"

namespace ps {
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
Result<ResultBuilder> ResultBuilder::start(ResourceBudget budget,
                                           const SchemaTemplate& schema,
                                           std::string_view scope,
                                           ResultGrowthLimits limits,
                                           std::uint64_t tile_extent) {
  std::optional<ResourceAllocationScope> metadata_scope;
  const auto* active_root = resource_internal::metadata_budget();
  if (!active_root || !active_root->same_owner(budget))
    metadata_scope.emplace(budget);
  auto valid = schema.validate();
  if (!valid.ok())
    return Result<ResultBuilder>(valid);
  if (!tile_extent || (tile_extent & (tile_extent - 1)))
    return Result<ResultBuilder>(
        Status{ErrorCode::InvalidArgument,
               "tile extent must be a positive power of two"});
  if (scope.empty() || scope.size() > 4096)
    return Result<ResultBuilder>(data_internal::invalid_schema());
  auto copied = schema.managed_copy(budget);
  if (!copied.ok())
    return Result<ResultBuilder>(copied.status());
  auto extra_work = budget.consume({scope.size()});
  if (!extra_work.ok())
    return Result<ResultBuilder>(extra_work);
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
    impl->scope = ResourceString(scope.data(), scope.size(),
                                 ResourceAllocator<char>(budget));
    impl->limits = limits;
    impl->image_budget = std::make_shared<TensorBudget>(limits.maximum_bytes);
    for (std::size_t i = 0; i < impl->schema.tensors.size(); ++i) {
      const auto& spec = impl->schema.tensors[i];
      auto& image = impl->tensors[i];
      auto initialized = image.initialize(impl->budget, spec, tile_extent);
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
Result<ResultRef> ResultBuilder::seal_empty() {
  if (!impl_)
    return Result<ResultRef>(Status{ErrorCode::Stale, {}});
  std::lock_guard<std::timed_mutex> lock(impl_->mutex);
  if (impl_->revision != 1 || impl_->complete || !impl_->status().ok())
    return Result<ResultRef>(data_internal::invalid_schema());
  for (std::size_t i = 0; i < impl_->schema.tensors.size(); ++i)
    if (!impl_->tensors[i].tensor.coverage().empty())
      return Result<ResultRef>(data_internal::invalid_schema());
  impl_->complete = true;
  ++impl_->revision;
  return Result<ResultRef>(reference());
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

  if (impl_->revision == UINT64_MAX)
    return reject(data_internal::invalid_schema());
  impl_->complete = true;
  ++impl_->revision;
  return Result<ResultRef>(reference());
}
}  // namespace ps
