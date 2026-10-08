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
Result<ResultRef> ResultRef::capture() const {
  if (!impl_)
    return Result<ResultRef>(Status{ErrorCode::Stale, {}});
  if (captured_)
    return Result<ResultRef>(*this);
  std::lock_guard<std::timed_mutex> lock(impl_->mutex);
  if (!impl_->complete &&
      impl_->schema.publication == PublishPolicy::CompleteBundle)
    return Result<ResultRef>(data_internal::unavailable());
  auto admitted = impl_->budget.reserve(
      ResourceCapacity::host(sizeof(Capture), sizeof(Capture)));
  if (!admitted.ok())
    return Result<ResultRef>(admitted.status());
  try {
    auto capture = std::shared_ptr<Capture>(new Capture());
    capture->lease = admitted.take_value();
    auto& facts = capture->descriptor;
    facts.object_ = impl_->object;
    facts.revision_ = impl_->revision;
    facts.sealed_ = impl_->complete;
    facts.field_count_ = impl_->schema.fields.size();
    facts.tensor_count_ = impl_->schema.tensors.size();
    for (uint32_t i = 0; i < facts.field_count_; ++i) {
      facts.rows_[i] = impl_->fields[i].certified;
      capture->fields[i] = impl_->fields[i].relation;
    }
    for (uint32_t i = 0; i < facts.tensor_count_; ++i) {
      facts.tensors_[i] = impl_->tensors[i].coverage;
      capture->tensors[i] = impl_->tensors[i].relation;
    }
    capture->basis = impl_->descriptor_relation;
    capture->dependencies = impl_->dependencies;
    ResultRef result = *this;
    result.captured_ = std::move(capture);
    return Result<ResultRef>(std::move(result));
  } catch (const std::bad_alloc&) {
    return Result<ResultRef>(Status{ErrorCode::ResourceExhausted, {}});
  }
}
WeakResultRef ResultRef::weak() const noexcept {
  WeakResultRef weak;
  weak.impl_ = impl_;
  weak.captured_ = captured_;
  weak.captured_view_ = captured_ != nullptr;
  weak.request_record_ = request_record_;
  return weak;
}
ResultRef WeakResultRef::lock() const noexcept {
  ResultRef result;
  result.impl_ = impl_.lock();
  result.request_record_ = request_record_;
  if (captured_view_) {
    result.captured_ = captured_.lock();
    if (!result.captured_)
      result.impl_.reset();
  }
  return result;
}
void ResultRef::bind_dependencies(
    std::shared_ptr<const execution_internal::DependencyBundle> bundle) const {
  std::lock_guard<std::timed_mutex> lock(impl_->mutex);
  impl_->dependencies.bind(std::move(bundle));
}
std::shared_ptr<const execution_internal::DependencyBundle>
ResultRef::dependencies() const {
  if (captured_)
    return captured_->dependencies.get();
  std::lock_guard<std::timed_mutex> lock(impl_->mutex);
  return impl_->dependencies.get();
}
Result<ResultDescriptor> ResultRef::descriptor(bool require_complete) const {
  if (captured_)
    return require_complete && !captured_->descriptor.sealed()
               ? Result<ResultDescriptor>(data_internal::unavailable())
               : Result<ResultDescriptor>(captured_->descriptor);
  if (!impl_)
    return Result<ResultDescriptor>(Status{ErrorCode::Stale, {}});
  std::lock_guard<std::timed_mutex> lock(impl_->mutex);
  if (!impl_->complete &&
      (require_complete ||
       impl_->schema.publication == PublishPolicy::CompleteBundle))
    return Result<ResultDescriptor>(
        impl_->status().ok() ? data_internal::unavailable() : impl_->status());
  ResultDescriptor facts;
  facts.object_ = impl_->object;
  facts.revision_ = impl_->revision;
  facts.sealed_ = impl_->complete;
  facts.field_count_ = static_cast<std::uint32_t>(impl_->schema.fields.size());
  for (std::uint32_t i = 0; i < facts.field_count_; ++i)
    facts.rows_[i] = impl_->fields[i].certified;
  facts.tensor_count_ =
      static_cast<std::uint32_t>(impl_->schema.tensors.size());
  for (std::uint32_t i = 0; i < facts.tensor_count_; ++i)
    facts.tensors_[i] = impl_->tensors[i].coverage;
  return Result<ResultDescriptor>(facts);
}
}  // namespace ps
