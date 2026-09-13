#include "photospider/plugin/result_program.hpp"

#include <utility>
#include <vector>

namespace ps {
Status ResultProgramPhase::read(std::uint32_t input,
                                const std::vector<std::uint64_t>& coordinate,
                                void* destination, std::size_t bytes) const {
  Status status;
  try {
    auto found = values.find(input);
    status = found == values.end()
                 ? Status{ErrorCode::InvalidArgument,
                          "unauthorized structured input read"}
                 : found->second.read(coordinate, destination, bytes);
  } catch (const std::bad_alloc&) {
    status = Status{ErrorCode::ResourceExhausted, {}};
  } catch (...) {
    status = Status{ErrorCode::OperationFailed, {}};
  }
  if (!status.ok() && status.code == ErrorCode::InvalidArgument) {
    status.reason = FailureReason::UnauthorizedRead;
    status.detail = {FailureOrigin::Protocol, FailureScope::Group};
  }
  if (!status.ok() && failure_observer)
    failure_observer(status);
  if (!status.ok() && failure) {
    auto expected = ErrorCode::Ok;
    failure->compare_exchange_strong(expected, status.code);
  }
  return status;
}
void ResultContinuation::reset() noexcept {
  if (destroy_)
    destroy_(storage_.data());
  destroy_ = nullptr;
  poll_ = nullptr;
  storage_ = {};
  definition_.reset();
}
ResultContinuation::~ResultContinuation() noexcept {
  reset();
}
ResultContinuation::ResultContinuation(ResultContinuation&& other) noexcept {
  *this = std::move(other);
}
ResultContinuation& ResultContinuation::operator=(
    ResultContinuation&& other) noexcept {
  if (this != &other) {
    reset();
    storage_ = std::move(other.storage_);
    definition_ = std::move(other.definition_);
    destroy_ = std::exchange(other.destroy_, nullptr);
    poll_ = std::exchange(other.poll_, nullptr);
  }
  return *this;
}
Result<ResultProgramPoll> ResultContinuation::poll(
    const ResultProgramPhase& phase) {
  if (!poll_)
    return Result<ResultProgramPoll>(Status{ErrorCode::Stale, {}});
  return poll_(storage_.data(), phase);
}
}  // namespace ps
