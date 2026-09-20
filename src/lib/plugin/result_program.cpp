#include "photospider/plugin/result_program.hpp"

#include <utility>
#include <vector>

#include "data/input_validation.hpp"

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
  resources_ = {};
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
    resources_ = std::move(other.resources_);
    destroy_ = std::exchange(other.destroy_, nullptr);
    poll_ = std::exchange(other.poll_, nullptr);
  }
  return *this;
}
Result<ResultProgramPoll> ResultContinuation::poll(
    const ResultProgramPhase& phase) {
  if (!poll_)
    return Result<ResultProgramPoll>(Status{ErrorCode::Stale, {}});
  if (!resources_.size() && !phase.query.resources.size() &&
      !input_internal::color_array(phase.query.output.facets))
    return poll_(storage_.data(), phase);
  auto query = phase.query;
  query.resources = resources_;
  if (query.value_outputs) {
    FootprintLimits limits;
    limits.consume_work = phase.consume_work;
    limits.cancellation = query.cancellation;
    auto closed = input_internal::color_output_samples(
        query.output, *query.value_outputs, limits);
    if (!closed.ok())
      return Result<ResultProgramPoll>(closed.status());
    query.value_outputs = closed.take_value();
  }
  ResultProgramPhase normalized{query,
                                phase.values,
                                phase.results,
                                phase.io,
                                phase.allocator,
                                phase.resources,
                                phase.consume_work,
                                phase.failure,
                                phase.failure_observer};
  return poll_(storage_.data(), normalized);
}
}  // namespace ps
