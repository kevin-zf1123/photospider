#include "photospider/data/result.hpp"

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
#include "data/result_state.hpp"
#include "data/result_support.hpp"
#include "data/result_window_access.hpp"
#include "data/tensor_validation.hpp"
#include "photospider/core/checked_math.hpp"

namespace ps {
bool ResultRef::owned_by(const ResourceBudget& budget) const noexcept {
  return impl_ && impl_->budget.same_owner(budget);
}
std::uint64_t ResultRef::object_id() const noexcept {
  return impl_ ? impl_->object : 0;
}
const SchemaTemplate& ResultRef::schema() const {
  if (!impl_)
    throw std::logic_error("invalid ResultRef");
  return impl_->schema;
}
bool ResultRef::matches_scope(std::string_view scope) const noexcept {
  return impl_ && std::string_view(impl_->scope) == scope;
}
Status ResultRef::production_status() const {
  if (!impl_)
    return Status{ErrorCode::Stale, {}};
  std::lock_guard<std::timed_mutex> lock(impl_->mutex);
  if (captured_ ? captured_->descriptor.sealed() : impl_->complete)
    return Status::success();
  return impl_->failure.ok() && !impl_->cancelled_publication.load()
             ? data_internal::unavailable()
             : impl_->status();
}
}  // namespace ps
