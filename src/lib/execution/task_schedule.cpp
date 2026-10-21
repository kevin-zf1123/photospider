#include "execution/task_schedule.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <stdexcept>
#include <string>
#include <utility>

namespace ps::execution_internal {
namespace {
constexpr std::size_t failure_bytes = 256;
void copy_failure(Status* target, const Status& source) {
  target->code = source.code;
  target->reason = source.reason;
  target->detail = source.detail;
  target->message.assign(source.message.data(),
                         std::min(source.message.size(), failure_bytes));
}
unsigned priority(const Status& status) {
  if (status.ok())
    return 0;
  if (status.code == ErrorCode::Cancelled)
    return 3;
  if (status.code == ErrorCode::Stale)
    return 2;
  return 1;
}
}  // namespace

TaskSchedule::TaskSchedule(const physical::Program& program, physical::Id block,
                           std::uint64_t run, std::uint64_t activation,
                           std::uint64_t first_submission,
                           std::uint64_t first_sequence, ResourceLease metadata)
    : program_(program),
      block_(program.blocks.at(block)),
      run_(run),
      activation_(activation),
      first_submission_(first_submission),
      next_submission_(first_submission),
      next_sequence_(first_sequence),
      allocation_(std::move(metadata)),
      failure_cut_(static_cast<physical::Id>(block_.calls.size())),
      suppressed_from_(static_cast<physical::Id>(block_.calls.size())),
      unsettled_(block_.tasks.size()) {
  tasks_.resize(program.tasks.size());
  allocation_.commit(tasks_.capacity() * sizeof(Task));
  calls_.resize(program.calls.size());
  allocation_.commit(calls_.capacity() * sizeof(Call));
  static_assert(sizeof(std::string) < failure_bytes);
  abort_.message.reserve(failure_bytes);
  allocation_.commit(abort_.message.capacity() + 1);
  for (auto& task : tasks_) {
    task.failure.message.reserve(failure_bytes);
    allocation_.commit(task.failure.message.capacity() + 1);
  }
  // Validate counters before any task can issue, including empty blocks.
  if (!run || !activation || activation == UINT64_MAX ||
      block_.tasks.size() > UINT64_MAX - first_submission ||
      block_.calls.size() > UINT64_MAX - first_sequence) {
    abort_.code = ErrorCode::ResourceExhausted;
    abort_.message.assign("Run identity or sequence exhausted");
  }
  for (auto& queue : ready_) {
    queue.reserve(block_.tasks.size());
    allocation_.commit(queue.capacity() * sizeof(physical::Id));
  }
  submissions_.reserve(block_.tasks.size());
  allocation_.commit(submissions_.capacity() * sizeof(physical::Id));
  skipped_.reserve(block_.tasks.size());
  allocation_.commit(skipped_.capacity() * sizeof(physical::Id));
  for (auto ci : block_.calls)
    calls_[ci].unsettled =
        static_cast<physical::Id>(program.calls[ci].tasks.size());
  for (auto ti : block_.tasks) {
    tasks_[ti].state = TaskState::Waiting;
    tasks_[ti].pending =
        static_cast<physical::Id>(program.tasks[ti].predecessors.size());
  }
  if (aborted()) {
    skip_from(0);
    return;
  }
  for (auto ti : block_.tasks)
    if (!tasks_[ti].pending)
      make_ready(ti);
}

std::uint64_t TaskSchedule::metadata_bytes(const physical::Program& program,
                                           physical::Id block) {
  return sizeof(TaskSchedule) + std::uint64_t{512} +
         program.tasks.size() * (sizeof(Task) + std::uint64_t{512}) +
         program.calls.size() * sizeof(Call) +
         program.blocks.at(block).tasks.size() * 4 * sizeof(physical::Id);
}

bool TaskSchedule::lower_priority(physical::Id a, physical::Id b) const {
  auto ordinal = [&](auto ti) {
    auto ci = program_.tasks[ti].call;
    return ci == physical::none ? physical::none : program_.calls[ci].ordinal;
  };
  auto a_call = ordinal(a);
  auto b_call = ordinal(b);
  return a_call == b_call ? a > b : a_call > b_call;
}

ReadyWait TaskSchedule::WaitClock::sample(
    std::chrono::steady_clock::time_point now) noexcept {
  const auto ns = static_cast<std::uint64_t>(
      std::chrono::duration_cast<std::chrono::nanoseconds>(now - last).count());
  if (reason == AdmissionWait::Lane)
    elapsed.lane_ns += ns;
  else if (reason == AdmissionWait::Context)
    elapsed.context_ns += ns;
  last = now;
  return elapsed;
}
void TaskSchedule::waiting(physical::Backend backend,
                           AdmissionWait reason) noexcept {
  auto& clock = waits_[static_cast<unsigned>(backend)];
  clock.sample(std::chrono::steady_clock::now());
  clock.reason = reason;
}

void TaskSchedule::admitted() noexcept {
  const auto now = std::chrono::steady_clock::now();
  for (unsigned backend = 0; backend < 2; ++backend) {
    const auto baseline = waits_[backend].sample(now);
    for (auto ti : ready_[backend]) {
      tasks_[ti].ready_at = now;
      tasks_[ti].wait = baseline;
    }
  }
}

void TaskSchedule::make_ready(physical::Id ti) {
  auto& state = tasks_[ti];
  if (state.state != TaskState::Waiting)
    return;
  state.state = TaskState::Ready;
  const auto& task = program_.tasks[ti];
  if (task.call == physical::none && head_ != block_.calls.size())
    return;
  if (task.kind == physical::TaskKind::Effect &&
      program_.calls[task.call].ordinal != head_)
    return;
  auto backend = program_.placements[task.placement].backend;
  auto& queue = ready_[static_cast<unsigned>(backend)];
  state.ready_at = std::chrono::steady_clock::now();
  state.wait = waits_[static_cast<unsigned>(backend)].sample(state.ready_at);
  queue.push_back(ti);
  std::push_heap(queue.begin(), queue.end(),
                 [this](auto a, auto b) { return lower_priority(a, b); });
}

void TaskSchedule::activate_effect() {
  if (head_ >= block_.calls.size() || head_ >= failure_cut_ || aborted())
    return;
  auto ti = program_.calls[block_.calls[head_]].effect_task;
  if (ti == physical::none || tasks_[ti].state != TaskState::Ready)
    return;
  auto backend = program_.placements[program_.tasks[ti].placement].backend;
  auto& queue = ready_[static_cast<unsigned>(backend)];
  tasks_[ti].ready_at = std::chrono::steady_clock::now();
  tasks_[ti].wait =
      waits_[static_cast<unsigned>(backend)].sample(tasks_[ti].ready_at);
  queue.push_back(ti);
  std::push_heap(queue.begin(), queue.end(),
                 [this](auto a, auto b) { return lower_priority(a, b); });
}

void TaskSchedule::activate_boundary() {
  if (head_ != block_.calls.size() || failure_cut_ != block_.calls.size() ||
      aborted())
    return;
  for (auto ti : block_.tasks) {
    if (program_.tasks[ti].call != physical::none ||
        tasks_[ti].state != TaskState::Ready)
      continue;
    tasks_[ti].state = TaskState::Waiting;
    make_ready(ti);
  }
}

bool TaskSchedule::has_ready(physical::Backend backend) {
  if (aborted())
    return false;
  if (static_cast<unsigned>(backend) > 1) {
    abort({ErrorCode::Internal, "invalid execution backend"});
    return false;
  }
  auto& queue = ready_[static_cast<unsigned>(backend)];
  while (!queue.empty()) {
    if (tasks_[queue.front()].state == TaskState::Ready)
      return true;
    std::pop_heap(queue.begin(), queue.end(),
                  [this](auto a, auto b) { return lower_priority(a, b); });
    queue.pop_back();
  }
  return false;
}

std::optional<Submission> TaskSchedule::issue_next(physical::Backend backend) {
  if (!has_ready(backend))
    return std::nullopt;
  auto& queue = ready_[static_cast<unsigned>(backend)];
  std::pop_heap(queue.begin(), queue.end(),
                [this](auto a, auto b) { return lower_priority(a, b); });
  const auto ti = queue.back();
  queue.pop_back();
  const auto elapsed = waits_[static_cast<unsigned>(backend)].sample(
      std::chrono::steady_clock::now());
  auto& wait = tasks_[ti].wait;
  wait = {elapsed.lane_ns - wait.lane_ns, elapsed.context_ns - wait.context_ns};
  tasks_[ti].state = TaskState::Submitted;
  submissions_.push_back(ti);
  ++inflight_;
  return Submission{{run_, activation_, next_submission_++}, ti};
}

void TaskSchedule::skip_from(physical::Id ordinal) {
  for (auto i = ordinal; i < suppressed_from_; ++i) {
    auto ci = block_.calls[i];
    for (auto ti : program_.calls[ci].tasks) {
      auto& state = tasks_[ti].state;
      if (state != TaskState::Ready && state != TaskState::Waiting)
        continue;
      state = TaskState::Skipped;
      skipped_.push_back(ti);
      --calls_[ci].unsettled;
      --unsettled_;
    }
  }
  if (!boundary_suppressed_) {
    boundary_suppressed_ = true;
    for (auto ti : block_.tasks) {
      if (program_.tasks[ti].call != physical::none)
        continue;
      auto& state = tasks_[ti].state;
      if (state != TaskState::Ready && state != TaskState::Waiting)
        continue;
      state = TaskState::Skipped;
      skipped_.push_back(ti);
      --unsettled_;
    }
  }
  suppressed_from_ = std::min(suppressed_from_, ordinal);
}

void TaskSchedule::abort(const Status& status) {
  const Status invalid{ErrorCode::Internal, {}};
  const auto& cause = status.ok() ? invalid : status;
  const bool first = !aborted();
  if (priority(cause) > priority(abort_)) {
    copy_failure(&abort_, cause);
    if (status.ok())
      abort_.message.assign("abort requires a failure");
  }
  if (first)
    skip_from(0);
}

Status TaskSchedule::complete(CompletionKey key, const Status& status) {
  if (key.run != run_ || key.activation != activation_ ||
      key.submission < first_submission_ ||
      key.submission >= next_submission_) {
    Status failure{ErrorCode::Internal, {}, FailureReason::MalformedEnvelope};
    abort(failure);
    return failure;
  }
  auto ti = submissions_[static_cast<std::size_t>(key.submission -
                                                  first_submission_)];
  auto& task = tasks_[ti];
  if (task.state != TaskState::Submitted) {
    Status failure{ErrorCode::Internal, {}, FailureReason::MalformedEnvelope};
    abort(failure);
    return failure;
  }
  auto ci = program_.tasks[ti].call;
  --inflight_;
  --unsettled_;
  if (ci != physical::none)
    --calls_[ci].unsettled;
  if (status.ok()) {
    task.state = TaskState::Succeeded;
    for (auto successor : program_.tasks[ti].successors) {
      auto& next = tasks_[successor];
      if (next.state != TaskState::Waiting)
        continue;
      if (--next.pending == 0)
        make_ready(successor);
    }
  } else {
    task.state = TaskState::Failed;
    copy_failure(&task.failure, status);
    if (ci == physical::none) {
      boundary_failure_ = std::min(boundary_failure_, ti);
      skip_from(static_cast<physical::Id>(block_.calls.size()));
    } else {
      auto& call = calls_[ci];
      call.first_failed = std::min(call.first_failed, ti);
      auto ordinal = program_.calls[ci].ordinal;
      if (ordinal < failure_cut_) {
        failure_cut_ = ordinal;
        skip_from(ordinal);
      }
    }
  }
  return Status::success();
}

std::optional<CallRetirement> TaskSchedule::retire_next() {
  if (aborted() || head_ >= block_.calls.size() || head_ > failure_cut_)
    return std::nullopt;
  auto ci = block_.calls[head_];
  const auto& call = calls_[ci];
  if (call.unsettled)
    return std::nullopt;
  CallRetirement record;
  record.sequence = next_sequence_++;
  record.activation = activation_;
  record.call = ci;
  record.source_call = program_.calls[ci].source_call;
  record.failed_task = call.first_failed;
  record.state = head_ == failure_cut_              ? CallState::Failed
                 : program_.calls[ci].tasks.empty() ? CallState::Elided
                                                    : CallState::Succeeded;
  ++head_;
  activate_effect();
  activate_boundary();
  return record;
}

Status TaskSchedule::settled_status() const {
  if (!drained())
    return {ErrorCode::Internal, "block has unsettled tasks"};
  if (aborted())
    return abort_;
  if (failure_cut_ < block_.calls.size()) {
    if (head_ != failure_cut_ + 1)
      return {ErrorCode::Internal, "failure has not retired"};
    auto failed = calls_[block_.calls[failure_cut_]].first_failed;
    return tasks_[failed].failure;
  }
  if (head_ != block_.calls.size())
    return {ErrorCode::Internal, "block has unretired calls"};
  if (boundary_failure_ != physical::none)
    return tasks_[boundary_failure_].failure;
  return Status::success();
}
void TaskSchedule::copy_settled_status(Status* target) const {
  if (drained() && aborted()) {
    copy_failure(target, abort_);
    return;
  }
  if (drained() && failure_cut_ < block_.calls.size() &&
      head_ == failure_cut_ + 1) {
    copy_failure(
        target,
        tasks_[calls_[block_.calls[failure_cut_]].first_failed].failure);
    return;
  }
  if (drained() && head_ == block_.calls.size() &&
      boundary_failure_ != physical::none) {
    copy_failure(target, tasks_[boundary_failure_].failure);
    return;
  }
  target->code = ErrorCode::Ok;
  target->reason = FailureReason::None;
  target->detail = {};
  target->message.clear();
  const char* error =
      !drained()                           ? "block has unsettled tasks"
      : failure_cut_ < block_.calls.size() ? "failure has not retired"
      : head_ != block_.calls.size()       ? "block has unretired calls"
                                           : nullptr;
  if (error) {
    target->code = ErrorCode::Internal;
    target->message.assign(error);
  }
}
}  // namespace ps::execution_internal
