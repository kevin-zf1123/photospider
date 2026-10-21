#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "execution/task_schedule.hpp"
#include "support/physical_program_fixture.hpp"
#include "support/status_check.hpp"

using namespace ps::physical;                // NOLINT(build/namespaces)
using namespace ps::execution_internal;      // NOLINT(build/namespaces)
using namespace ps::test::physical_fixture;  // NOLINT(build/namespaces)

int main() {
  auto p = diamond();
  PS_REQUIRE_OK(finalize_program(&p));
  TaskSchedule schedule(p, 0, 7, 9);
  PS_CHECK(schedule.has_ready(Backend::Cpu));
  PS_CHECK(!schedule.has_ready(Backend::Gpu));
  PS_CHECK(schedule.inflight() == 0 && schedule.next_submission() == 0);
  auto a = schedule.issue_next(Backend::Cpu);
  auto b = schedule.issue_next(Backend::Cpu);
  PS_CHECK(a && b && a->task == 0 && b->task == 2);
  PS_CHECK(!schedule.issue_next(Backend::Cpu));
  PS_CHECK(schedule.inflight() == 2);
  PS_REQUIRE_OK(schedule.complete(b->key, {}));
  auto d = schedule.issue_next(Backend::Cpu);
  PS_CHECK(d && d->task == 3);
  PS_REQUIRE_OK(schedule.complete(d->key, {}));
  // Successful younger tiles forward before any call can retire.
  PS_CHECK(schedule.retirement_head() == 0 && !schedule.retire_next());
  PS_REQUIRE_OK(schedule.complete(a->key, {}));
  auto c = schedule.issue_next(Backend::Cpu);
  PS_CHECK(c && c->task == 1);
  auto retired = schedule.retire_next();
  PS_CHECK(retired && retired->call == 0 && retired->sequence == 0);
  PS_REQUIRE_OK(schedule.complete(c->key, {}));
  auto e = schedule.issue_next(Backend::Cpu);
  PS_CHECK(e && e->task == 4);
  for (Id i = 1; i < 4; ++i) {
    retired = schedule.retire_next();
    PS_CHECK(retired && retired->call == i && retired->sequence == i);
    PS_CHECK(retired->state == CallState::Succeeded);
  }
  PS_REQUIRE_OK(schedule.complete(e->key, {}));
  retired = schedule.retire_next();
  PS_CHECK(retired && retired->call == 4);
  PS_CHECK(schedule.drained() && !schedule.retire_next());
  PS_REQUIRE_OK(schedule.settled_status());

  // A younger failure does not cancel the still-running older prefix.
  auto failures = program(4);
  auto source = output(&failures, task(&failures, 0));
  task(&failures, 1, {source});
  task(&failures, 2);
  auto effect = task(&failures, 3);
  failures.tasks[effect].kind = TaskKind::Effect;
  failures.calls[3].effect_task = effect;
  PS_REQUIRE_OK(finalize_program(&failures));
  TaskSchedule failed(failures, 0, 1, 1);
  auto old = failed.issue_next(Backend::Cpu);
  auto young = failed.issue_next(Backend::Cpu);
  PS_CHECK(old && young && old->task == 0 && young->task == 2);
  PS_REQUIRE_OK(
      failed.complete(young->key, {ps::ErrorCode::OperationFailed, "young"}));
  PS_CHECK(failed.failure_cut() == 2 &&
           failed.state(effect) == TaskState::Skipped);
  PS_REQUIRE_OK(failed.complete(old->key, {}));
  auto earlier = failed.issue_next(Backend::Cpu);
  PS_CHECK(earlier && earlier->task == 1);
  PS_REQUIRE_OK(
      failed.complete(earlier->key, {ps::ErrorCode::OperationFailed, "old"}));
  PS_CHECK(failed.failure_cut() == 1 && failed.drained());
  retired = failed.retire_next();
  PS_CHECK(retired && retired->state == CallState::Succeeded &&
           retired->call == 0);
  retired = failed.retire_next();
  PS_CHECK(retired && retired->state == CallState::Failed &&
           retired->failed_task == 1);
  PS_CHECK(!failed.retire_next() && failed.settled_status().message == "old");
  PS_CHECK(failed.skipped() == std::vector<Id>({effect}));

  // All accepted siblings drain; the smallest actually failed TaskId wins.
  auto siblings = program(2);
  task(&siblings, 0);
  task(&siblings, 0);
  auto blocked = task(&siblings, 0);
  siblings.tasks[blocked].explicit_dependencies = {0};
  task(&siblings, 1);
  PS_REQUIRE_OK(finalize_program(&siblings));
  TaskSchedule group(siblings, 0, 3, 2, 50, 10);
  auto slow = group.issue_next(Backend::Cpu);
  auto fast = group.issue_next(Backend::Cpu);
  auto accepted_young = group.issue_next(Backend::Cpu);
  PS_CHECK(slow && fast && accepted_young && accepted_young->task == 3);
  PS_REQUIRE_OK(
      group.complete(fast->key, {ps::ErrorCode::OperationFailed, "fast"}));
  PS_CHECK(group.state(blocked) == TaskState::Skipped && !group.retire_next());
  PS_REQUIRE_OK(
      group.complete(slow->key, {ps::ErrorCode::OperationFailed, "slow"}));
  retired = group.retire_next();
  PS_CHECK(retired && retired->failed_task == 0 && retired->sequence == 10);
  PS_CHECK(!group.drained() && group.inflight() == 1);
  PS_REQUIRE_OK(group.complete(accepted_young->key, {}));
  PS_CHECK(group.settled_status().message == "slow");
  PS_CHECK(group.failure(blocked).ok());

  // Effects wait for the retirement head without preventing younger pure work.
  auto effects = program(3);
  task(&effects, 0);
  auto prep = task(&effects, 1);
  auto terminal = task(&effects, 1);
  effects.tasks[terminal].kind = TaskKind::Effect;
  effects.tasks[terminal].explicit_dependencies = {prep};
  effects.calls[1].effect_task = terminal;
  task(&effects, 2);
  PS_REQUIRE_OK(finalize_program(&effects));
  TaskSchedule ordered(effects, 0, 3, 3);
  old = ordered.issue_next(Backend::Cpu);
  auto preparing = ordered.issue_next(Backend::Cpu);
  PS_CHECK(old && preparing && preparing->task == prep);
  PS_REQUIRE_OK(ordered.complete(preparing->key, {}));
  young = ordered.issue_next(Backend::Cpu);
  PS_CHECK(young && young->task == 3 && !ordered.issue_next(Backend::Cpu));
  PS_REQUIRE_OK(ordered.complete(old->key, {}));
  PS_CHECK(!ordered.issue_next(Backend::Cpu));
  PS_CHECK(ordered.retire_next());
  auto external = ordered.issue_next(Backend::Cpu);
  PS_CHECK(external && external->task == terminal);
  PS_REQUIRE_OK(ordered.complete(external->key, {}));
  PS_CHECK(ordered.retire_next());
  PS_REQUIRE_OK(ordered.complete(young->key, {}));
  PS_CHECK(ordered.retire_next());
  PS_REQUIRE_OK(ordered.settled_status());

  // Stop priority is independent of observed task failure and completion order.
  TaskSchedule cancelled(p, 0, 4, 1);
  a = cancelled.issue_next(Backend::Cpu);
  b = cancelled.issue_next(Backend::Cpu);
  PS_CHECK(a && b);
  cancelled.abort({ps::ErrorCode::OperationFailed, "observer"});
  cancelled.abort({ps::ErrorCode::BackendUnavailable, "device"});
  PS_CHECK(!cancelled.issue_next(Backend::Cpu) && !cancelled.retire_next());
  PS_CHECK(cancelled.skipped() == std::vector<Id>({1, 3, 4}));
  PS_REQUIRE_OK(
      cancelled.complete(a->key, {ps::ErrorCode::OperationFailed, "kernel"}));
  PS_REQUIRE_OK(cancelled.complete(b->key, {}));
  PS_CHECK(cancelled.settled_status().message == "observer");
  cancelled.abort({ps::ErrorCode::Stale, "graph changed"});
  PS_CHECK(cancelled.settled_status().code == ps::ErrorCode::Stale);
  cancelled.abort({ps::ErrorCode::Cancelled, "user"});
  PS_CHECK(cancelled.settled_status().code == ps::ErrorCode::Cancelled);
  PS_CHECK(cancelled.failure(0).message == "kernel");

  // Inline completion uses an already armed identity. Duplicate/stale events
  // neither decrement counters twice nor settle another submitted task.
  TaskSchedule duplicate(p, 0, 5, 2);
  a = duplicate.issue_next(Backend::Cpu);
  b = duplicate.issue_next(Backend::Cpu);
  PS_CHECK(a && b);
  PS_REQUIRE_OK(duplicate.complete(a->key, {}));
  PS_CHECK(!duplicate.complete(a->key, {}).ok());
  PS_CHECK(duplicate.inflight() == 1);
  auto stale = b->key;
  ++stale.activation;
  PS_CHECK(!duplicate.complete(stale, {}).ok());
  PS_CHECK(duplicate.inflight() == 1);
  PS_REQUIRE_OK(duplicate.complete(b->key, {}));
  PS_CHECK(duplicate.drained());
  PS_CHECK(duplicate.settled_status().code == ps::ErrorCode::Internal);

  auto empty = program(2);
  PS_REQUIRE_OK(finalize_program(&empty));
  TaskSchedule elided(empty, 0, 6, 1);
  for (Id i = 0; i < 2; ++i) {
    retired = elided.retire_next();
    PS_CHECK(retired && retired->state == CallState::Elided &&
             retired->call == i);
  }
  PS_REQUIRE_OK(elided.settled_status());
  TaskSchedule exhausted(p, 0, 6, 1, UINT64_MAX - 1);
  PS_CHECK(exhausted.aborted() && exhausted.drained());
  PS_CHECK(!exhausted.issue_next(Backend::Cpu));
  PS_CHECK(exhausted.settled_status().code == ps::ErrorCode::ResourceExhausted);

  // Boundary imports have no synthetic call and wait for the real retirement
  // prefix. Failure drains accepted siblings, suppresses unsubmitted copies,
  // and selects the smallest failed TaskId without emitting a call record.
  auto boundary = program(1);
  task(&boundary, 0);
  auto copy0 = boundary_import(&boundary);
  auto copy1 = boundary_import(&boundary);
  auto copy2 = boundary_import(&boundary);
  PS_REQUIRE_OK(finalize_program(&boundary));
  TaskSchedule imports(boundary, 0, 9, 1);
  a = imports.issue_next(Backend::Cpu);
  PS_CHECK(a && a->task == 0 && !imports.issue_next(Backend::Cpu));
  PS_REQUIRE_OK(imports.complete(a->key, {}));
  PS_CHECK(!imports.issue_next(Backend::Cpu));
  PS_CHECK(imports.retire_next());
  a = imports.issue_next(Backend::Cpu);
  b = imports.issue_next(Backend::Cpu);
  PS_CHECK(a && b && a->task == copy0 && b->task == copy1);
  PS_REQUIRE_OK(
      imports.complete(b->key, {ps::ErrorCode::OperationFailed, "second"}));
  PS_CHECK(imports.state(copy2) == TaskState::Skipped && !imports.drained());
  PS_CHECK(!imports.issue_next(Backend::Cpu) && !imports.retire_next());
  PS_REQUIRE_OK(
      imports.complete(a->key, {ps::ErrorCode::OperationFailed, "first"}));
  PS_CHECK(imports.drained() && imports.settled_status().message == "first");
  PS_CHECK(imports.next_sequence() == 1);
  TaskSchedule before_boundary(boundary, 0, 9, 2);
  a = before_boundary.issue_next(Backend::Cpu);
  PS_CHECK(a);
  PS_REQUIRE_OK(before_boundary.complete(
      a->key, {ps::ErrorCode::OperationFailed, "call"}));
  PS_CHECK(before_boundary.retire_next());
  PS_CHECK(before_boundary.drained() &&
           !before_boundary.issue_next(Backend::Cpu));
  PS_CHECK(before_boundary.settled_status().message == "call");
  auto no_calls = program(0);
  boundary_import(&no_calls);
  boundary_import(&no_calls);
  PS_REQUIRE_OK(finalize_program(&no_calls));
  TaskSchedule import_only(no_calls, 0, 10, 1);
  a = import_only.issue_next(Backend::Cpu);
  PS_CHECK(a && a->task == 0);
  import_only.abort({ps::ErrorCode::Cancelled, "cancel import"});
  PS_CHECK(!import_only.drained() && !import_only.retire_next());
  PS_REQUIRE_OK(import_only.complete(a->key, {}));
  PS_CHECK(import_only.drained() && import_only.next_sequence() == 0);
  PS_CHECK(import_only.settled_status().code == ps::ErrorCode::Cancelled);

  // A call larger than worker capacity runs without waiting for retirement.
  auto large = program();
  for (unsigned i = 0; i < 100; ++i)
    task(&large, 0);
  PS_REQUIRE_OK(finalize_program(&large));
  TaskSchedule streamed(large, 0, 8, 1);
  for (unsigned i = 0; i < 100; i += 2) {
    auto x = streamed.issue_next(Backend::Cpu);
    auto y = streamed.issue_next(Backend::Cpu);
    PS_CHECK(x && y && x->task == i && y->task == i + 1);
    PS_CHECK(!streamed.retire_next());
    PS_REQUIRE_OK(streamed.complete(y->key, {}));
    PS_REQUIRE_OK(streamed.complete(x->key, {}));
  }
  PS_CHECK(streamed.retire_next());
  PS_REQUIRE_OK(streamed.settled_status());
  return 0;
}
