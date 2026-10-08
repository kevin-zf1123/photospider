#pragma once

#include <chrono>
#include <cstdint>
#include <optional>
#include <utility>
#include <variant>

#include "execution/structured_execution.hpp"

namespace ps::execution_internal {
struct StructuredStartPhase {
  std::chrono::steady_clock::time_point started =
      std::chrono::steady_clock::now();
  Status dispatched;
  Result<ResultContinuation> result{Status{ErrorCode::Internal, {}}};
  ErrorCode sticky = ErrorCode::Ok;
};
struct StructuredPollPhase {
  std::chrono::steady_clock::time_point started =
      std::chrono::steady_clock::now();
  NumericDiagnostics numeric;
  std::uint64_t native_dispatches = 0;
  bool invoked = false;
  Result<ResultProgramPoll> result{Status{ErrorCode::Internal, {}}};
  ErrorCode sticky = ErrorCode::Ok;
};
// Owns one actor's continuation, retained Start/Poll/Need phase and submission.
// Queue/task owners keep this object alive until callback retirement. Await is
// borrowed from the coordinator; destruction performs no wait or registry call.
// The enclosing actor clears owned storage after drain and before retiring its
// query, source grants and metadata lease. Peer adoption keeps the same owner.
template <class Need>
struct StructuredActorLifecycle {
  ResultContinuation continuation;
  std::variant<std::monostate, StructuredStartPhase, StructuredPollPhase, Need>
      phase;
  std::optional<StructuredSubmission> pending;
  std::uint32_t polls = 0;
  bool busy = false, driving = false, initialized = false, queued = false;
  std::uint64_t visit = 0;
  bool has_submitted_stage() const noexcept { return pending && !queued; }
  template <class Await>
  Status await_stage(const Await& await) const {
    return await(*pending);
  }
  // Called only after coordinator retirement has merged submission diagnostics.
  StructuredStartPhase take_start(Status dispatched) {
    auto started = std::move(std::get<StructuredStartPhase>(phase));
    phase.template emplace<std::monostate>();
    started.dispatched = std::move(dispatched);
    return started;
  }
  StructuredPollPhase take_poll() {
    auto completed = std::move(std::get<StructuredPollPhase>(phase));
    phase.template emplace<std::monostate>();
    return completed;
  }
  // Root work is charged by the caller before this local stage transition.
  Status begin_poll(std::uint32_t maximum, ErrorCode sticky,
                    const char* limit_message) {
    if (polls >= maximum)
      return {ErrorCode::ResourceExhausted, limit_message};
    ++polls;
    phase.template emplace<StructuredPollPhase>();
    std::get<StructuredPollPhase>(phase).sticky = sticky;
    return Status::success();
  }
  // Supply and proof retention borrow this owner only for the current driver
  // advance. Failed supply retains the exact cursor; successful completion
  // retires the Need only after optional proof retention.
  template <class Supply, class Retain>
  Status advance_need(const Supply& supply, const Retain& retain) {
    auto& need = std::get<Need>(phase);
    auto status = supply(need.request, need.cursor);
    if (!status.ok())
      return status;
    if (need.cursor.complete()) {
      retain(need.request);
      phase.template emplace<std::monostate>();
    }
    return Status::success();
  }
  void release_stage() noexcept { pending.reset(); }
  void retire_owned_storage() noexcept {
    phase.template emplace<std::monostate>();
    continuation = {};
    pending.reset();
  }
};
}  // namespace ps::execution_internal
