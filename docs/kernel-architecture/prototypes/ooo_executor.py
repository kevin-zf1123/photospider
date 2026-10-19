#!/usr/bin/env python3
"""Executable semantic prototype, not the production executor.

Logical-time events simulate CPU/GPU completion. Actual bytearrays model unique
allocations; assertions inspect the model, not operator numerical correctness.
Explicit task edges constrain readiness without granting payload access. Call
retirement and final RET transfer separate holds to separate result owners.
Ready scans deliberately favor clarity over production scheduling complexity.
"""
from __future__ import annotations

import heapq
import random
import sys
import unittest
from dataclasses import dataclass


@dataclass(frozen=True)
class Task:
    name: str
    call: int
    reads: tuple[str, ...] = ()
    writes: tuple[str, ...] = ()
    duration: int = 1
    backend: str = "cpu"
    scratch: int = 0
    fail: bool = False
    effect: bool = False
    dependencies: tuple[int, ...] = ()  # Static task indices, not payload reads.


@dataclass(frozen=True)
class TaskFailure:
    activation: int
    task_id: int
    call: int
    task_name: str


@dataclass
class Buffer:
    payload: bytearray
    holds: int
    available: bool = False
    released: bool = False

    def retain(self):
        assert not self.released
        self.holds += 1

    def drop(self):
        assert not self.released and self.holds > 0
        self.holds -= 1
        if not self.holds:
            self.released = True
            self.payload.clear()


def capture_edge(old: tuple[Buffer, ...], source_indices: tuple[int, ...]):
    """Acquire all selected argument owners before releasing old parameter owners."""
    incoming = tuple(old[i] for i in source_indices)
    for value in incoming:
        value.retain()
    for value in old:
        value.drop()
    return incoming


class Model:
    """One block activation, including separate completion/retirement/release."""

    def __init__(self, tasks, calls=None, retirement_exports=(), return_values=(),
                 slots=None, budget=1 << 20, tile_bytes=4, activation=1):
        if tile_bytes < 1:
            raise ValueError("prototype requires positive tile_bytes")
        self.tasks = tuple(tasks)
        tasks = self.tasks
        self.calls = calls if calls is not None else max(
            (t.call for t in tasks), default=-1) + 1
        self.retirement_exports = frozenset(retirement_exports)
        self.return_values = frozenset(return_values)
        # Internal obligations transfer into independent external owners.
        self.retirement_holds = set()
        self.return_holds = set()
        self.retired_values = {}
        self.results = {}
        self.slots = dict(slots or {"cpu": 2, "gpu": 1})
        self.activation = activation
        self.tile_bytes = tile_bytes
        self.trace = []
        self.records = []
        self.events = []
        self.now = 0
        self.head = 0
        self.cut = self.calls
        self.abort = False
        self.outcome = None
        self.task_failures = {}       # Observed failures, including younger work.
        self.retired_failures = {}    # Failure detail handed off with a call.
        self.failure = None           # Selected terminal task failure, if any.
        self.state = ["waiting"] * len(tasks)
        self.inflight = {b: 0 for b in self.slots}
        self.peak = dict(self.inflight)
        self.producer = {}
        self.members = [[] for _ in range(self.calls)]
        for i, task in enumerate(tasks):
            if not 0 <= task.call < self.calls or task.duration < 1:
                raise ValueError("invalid task call/duration")
            if task.backend not in self.slots or self.slots[task.backend] < 1:
                raise ValueError("backend needs a lane")
            if task.scratch < 0:
                raise ValueError("negative scratch")
            self.members[task.call].append(i)
            for v in task.writes:
                if v in self.producer:
                    raise ValueError("multiple tile producers")
                self.producer[v] = i
        if not (self.retirement_exports | self.return_values) <= self.producer.keys():
            raise ValueError("missing export")
        self.pred = []
        self.succ = [[] for _ in tasks]
        for i, task in enumerate(tasks):
            if any(v not in self.producer for v in task.reads):
                raise ValueError("missing input")
            if any(type(p) is not int or not 0 <= p < len(tasks)
                   for p in task.dependencies):
                raise ValueError("invalid explicit dependency")
            deps = {self.producer[v] for v in task.reads} | set(task.dependencies)
            if any(tasks[p].call > task.call for p in deps):
                raise ValueError("dependency on later call")
            self.pred.append(deps)
            for p in deps:
                self.succ[p].append(i)
        todo, visited = set(range(len(tasks))), set()
        while todo:
            ready = {i for i in todo if self.pred[i] <= visited}
            if not ready:
                raise ValueError("cyclic tasks")
            visited.update(ready)
            todo.difference_update(ready)
        for members in self.members:
            effects = [i for i in members if tasks[i].effect]
            if len(effects) > 1:
                raise ValueError("multiple effects in call")
            if effects:
                e = effects[0]
                ancestors, work = set(), list(self.pred[e])
                while work:
                    p = work.pop()
                    if p not in ancestors:
                        ancestors.add(p)
                        work.extend(self.pred[p])
                if not set(members) - {e} <= ancestors:
                    raise ValueError("effect must follow all preparation")
        self.pending = list(map(len, self.pred))
        # Scalar resource domain in this model; production uses vectors and M.
        scratch = sum(self.slots[b] * max(
            (t.scratch for t in tasks if t.backend == b), default=0)
                      for b in self.slots)
        self.reserved = len(self.producer) * tile_bytes + scratch
        self.buffers = {}
        if self.reserved > budget:
            self.outcome = "ResourceExhausted"
            return
        for v in self.producer:
            readers = sum(v in set(t.reads) for t in tasks)
            self.buffers[v] = Buffer(bytearray(tile_bytes),
                                     1 + readers + (v in self.retirement_exports)
                                     + (v in self.return_values))
        self.retirement_holds.update(self.retirement_exports)
        self.return_holds.update(self.return_values)
        # Preallocated identity slot per task; one terminal event is accepted.
        self.completion_keys = [(activation, i) for i in range(len(tasks))]

    def drop(self, v):
        buf = self.buffers[v]
        buf.drop()
        if buf.released:
            self.trace.append((self.now, "release", v))

    def skip(self, i):
        assert self.state[i] == "waiting"
        self.state[i] = "skipped"
        self.trace.append((self.now, "skip", self.tasks[i].name))
        for v in set(self.tasks[i].reads):
            self.drop(v)
        for v in self.tasks[i].writes:
            self.drop(v)

    def suppress(self):
        for i, task in enumerate(self.tasks):
            if self.state[i] == "waiting" and (self.abort or task.call >= self.cut):
                self.skip(i)

    def complete(self, key):
        activation, i = key
        if activation != self.activation or not 0 <= i < len(self.tasks):
            raise ValueError("stale completion")
        if self.state[i] != "submitted":
            raise ValueError("duplicate or unaccepted completion")
        task = self.tasks[i]
        self.inflight[task.backend] -= 1
        self.state[i] = "failed" if task.fail else "succeeded"
        self.trace.append((self.now, "complete", task.name))
        if task.fail:
            self.task_failures[i] = TaskFailure(self.activation, i, task.call, task.name)
            self.cut = min(self.cut, task.call)
        else:
            value = (1 + sum(self.buffers[v].payload[0]
                             for v in task.reads)) % 256
            for v in task.writes:
                assert not self.buffers[v].released
                self.buffers[v].payload[:] = bytes([value]) * self.tile_bytes
                self.buffers[v].available = True
            for nxt in self.succ[i]:
                self.pending[nxt] -= 1
                assert self.pending[nxt] >= 0
        for v in set(task.reads):
            self.drop(v)
        for v in task.writes:
            self.drop(v)

    def retire(self):
        while not self.abort and self.head < self.calls:
            members = self.members[self.head]
            if any(self.state[i] in ("waiting", "submitted") for i in members):
                break
            if self.head > self.cut:
                break
            status = ("failed" if self.head == self.cut else
                      "ok" if members else "elided")
            if status == "ok":
                assert all(self.state[i] == "succeeded" for i in members)
                for i in members:
                    for v in self.tasks[i].writes:
                        if v in self.retirement_holds:
                            assert self.buffers[v].available
                            self.retirement_holds.remove(v)
                            self.retired_values[v] = self.buffers[v]
                            self.trace.append((self.now, "retire_value", v))
            self.records.append((self.head, status))
            self.trace.append((self.now, "retire", self.head))
            self.head += 1
            if status == "failed":
                failed = min(i for i in members if i in self.task_failures)
                self.retired_failures[self.head - 1] = self.task_failures[failed]
                break

    def issue(self):
        for i in sorted(range(len(self.tasks)), key=lambda j: (self.tasks[j].call, j)):
            task = self.tasks[i]
            if (self.abort or task.call >= self.cut or self.state[i] != "waiting"
                    or self.pending[i] or (task.effect and task.call != self.head)
                    or self.inflight[task.backend] == self.slots[task.backend]):
                continue
            for v in set(task.reads):
                assert self.buffers[v].available and not self.buffers[v].released
            self.state[i] = "submitted"
            self.inflight[task.backend] += 1
            self.peak[task.backend] = max(self.peak[task.backend], self.inflight[task.backend])
            self.trace.append((self.now, "issue", task.name))
            if task.effect:
                self.trace.append((self.now, "effect", task.name))
            heapq.heappush(self.events, (self.now + task.duration, i))

    def run(self, cancel_at=None):
        if self.outcome:
            return self
        while True:
            if cancel_at is not None and self.now >= cancel_at:
                self.abort = True
            self.suppress()
            self.retire()
            self.issue()
            if not self.events:
                break
            next_time = self.events[0][0]
            if not self.abort and cancel_at is not None and self.now < cancel_at < next_time:
                self.now = cancel_at
                continue
            self.now = next_time
            # Stop has priority over completions at the same logical time.
            if cancel_at is not None and self.now >= cancel_at:
                self.abort = True
            while self.events and self.events[0][0] == self.now:
                _, i = heapq.heappop(self.events)
                self.complete(self.completion_keys[i])
        assert all(s in ("succeeded", "failed", "skipped") for s in self.state)
        assert not any(self.inflight.values())
        self.outcome = ("Cancelled" if self.abort else
                        "OperationFailed" if self.cut < self.calls else "Ok")
        if self.outcome == "OperationFailed":
            self.failure = self.retired_failures[self.cut]
        if self.outcome == "Ok":
            assert self.head == self.calls
            for v in sorted(self.return_holds):
                assert self.buffers[v].available and not self.buffers[v].released
                self.results[v] = self.buffers[v]
                self.trace.append((self.now, "return", v))
            self.return_holds.clear()  # Ownership transferred, not dropped.
        # Failure/abort discards all RET obligations, even for retired calls.
        # Successfully handed-off call exports remain independently owned.
        for holds in (self.retirement_holds, self.return_holds):
            for v in sorted(holds):
                self.drop(v)
            holds.clear()
        return self

    def release_retired_values(self):
        """Release handed-off call values; pending RET holds remain untouched."""
        for v in self.retired_values:
            self.drop(v)
        self.retired_values.clear()

    def release_results(self):
        """Release successful RET values independently of call delivery owners."""
        for v in self.results:
            self.drop(v)
        self.results.clear()


def example():
    return [Task("A", 0, writes=("a",), duration=10),
            Task("C", 1, ("a",), ("c",)),
            Task("B", 2, writes=("b",), duration=2),
            Task("D", 3, ("b",), ("d",)),
            Task("E", 4, ("c", "d"), ("e",))]


class Checks(unittest.TestCase):
    def clean(self, model):
        model.release_retired_values()
        model.release_results()
        self.assertFalse(model.retirement_holds or model.return_holds)
        self.assertTrue(all(b.released and b.holds == 0 for b in model.buffers.values()))

    def test_forwarding_and_retirement(self):
        m = Model(example(), return_values=("e",)).run()
        self.assertEqual([n for _, k, n in m.trace if k == "complete"], ["B", "D", "A", "C", "E"])
        self.assertEqual(m.records, [(i, "ok") for i in range(5)])
        self.assertEqual(m.now, 12)
        self.assertEqual(m.peak["cpu"], 2)
        self.assertEqual(m.buffers["e"].payload[0], 5)
        self.clean(m)

    def test_large_call_not_limited_by_retirement_slots(self):
        m = Model([Task(str(i), 0, writes=(str(i),)) for i in range(100)]).run()
        self.assertEqual(m.now, 50)
        self.assertEqual(m.records, [(0, "ok")])
        self.clean(m)

    def test_multioutput_and_duplicate_reads(self):
        m = Model([Task("A", 0, writes=("a", "b")),
                   Task("B", 1, ("a", "a", "b"), ("c",))], return_values=("c",))
        self.assertEqual(m.pending, [0, 1])
        self.assertEqual(m.buffers["a"].holds, 2)
        m.run()
        self.assertEqual(m.buffers["c"].payload[0], 4)
        self.clean(m)

    def test_older_failure_wins_after_younger_failure(self):
        m = Model([Task("A", 0, writes=("a",), duration=5),
                   Task("B", 1, ("a",), ("b",), fail=True),
                   Task("C", 2, writes=("c",), fail=True),
                   Task("effect", 3, effect=True)]).run()
        self.assertEqual(m.records, [(0, "ok"), (1, "failed")])
        self.assertTrue(any(k == "issue" and n == "B" for _, k, n in m.trace))
        self.assertFalse(any(k == "effect" for _, k, _ in m.trace))
        self.clean(m)

    def test_failure_skips_siblings_but_drains_submitted(self):
        m = Model([Task("bad", 0, writes=("x",), fail=True),
                   Task("slow", 0, writes=("y",), duration=10),
                   Task("skip", 0, ("x",), ("z",)),
                   Task("young", 1, ("y",), ("q",))]).run()
        self.assertEqual(m.now, 10)
        self.assertEqual(m.records, [(0, "failed")])
        self.clean(m)

    def test_effect_is_head_and_follows_preparation(self):
        m = Model([Task("slow", 0, duration=5), Task("prep", 1, writes=("p",)),
                   Task("write", 1, ("p",), effect=True)]).run()
        effect = next(t for t, k, _ in m.trace if k == "effect")
        self.assertGreaterEqual(effect, 5)
        self.clean(m)

    def test_explicit_dependency_orders_effect_without_payload_hold(self):
        m = Model([Task("prepare", 0, writes=("temporary",), duration=5),
                   Task("effect", 0, effect=True, dependencies=(0,))])
        self.assertEqual(m.pending, [0, 1])
        self.assertEqual(m.buffers["temporary"].holds, 1)
        m.run()
        self.assertEqual(m.records, [(0, "ok")])
        self.assertLess(m.trace.index((5, "release", "temporary")),
                        m.trace.index((5, "issue", "effect")))
        self.clean(m)

    def test_explicit_and_payload_dependencies_deduplicate_together(self):
        m = Model([Task("A", 0, writes=("a", "b")),
                   Task("B", 1, ("a", "a", "b"), dependencies=(0, 0))])
        self.assertEqual(m.pred, [set(), {0}])
        self.assertEqual(m.succ, [[1], []])
        self.assertEqual(m.buffers["a"].holds, 2)
        m.run()
        self.assertEqual(m.pending, [0, 0])
        self.clean(m)

    def test_explicit_dependency_can_name_a_later_table_index(self):
        m = Model([Task("consumer", 1, dependencies=(1,)),
                   Task("producer", 0, duration=3)]).run()
        self.assertEqual([(t, n) for t, k, n in m.trace if k == "issue"],
                         [(0, "producer"), (3, "consumer")])
        self.clean(m)

    def test_explicit_failure_never_issues_dependent(self):
        m = Model([Task("fail", 0, fail=True),
                   Task("dependent", 1, writes=("x",), dependencies=(0,))],
                  retirement_exports=("x",), return_values=("x",)).run()
        self.assertEqual(m.state, ["failed", "skipped"])
        self.assertEqual(m.retired_values, {})
        self.assertEqual(m.results, {})
        self.clean(m)

    def test_image_thumbnail_terminal_effect(self):
        m = Model([Task("image", 0, writes=("a",)),
                   Task("thumbnail", 0, writes=("b",), duration=4),
                   Task("save_image", 0, reads=("a",), effect=True,
                        dependencies=(1,))],
                  retirement_exports=("b",), return_values=("b",))
        self.assertEqual(m.pred[2], {0, 1})
        self.assertEqual(m.buffers["b"].holds, 3)  # Producer + two export routes.
        m.run()
        self.assertIn((4, "effect", "save_image"), m.trace)
        self.assertEqual(m.results["b"].payload[0], 1)
        self.assertEqual(m.buffers["b"].holds, 2)
        self.clean(m)

    def test_failure_detail_uses_lowest_observed_task_after_drain(self):
        m = Model([Task("slow_failure", 0, duration=5, fail=True),
                   Task("fast_failure", 0, fail=True)], activation=9).run()
        self.assertEqual(list(m.task_failures), [1, 0])
        self.assertEqual(m.failure, TaskFailure(9, 0, 0, "slow_failure"))
        self.assertEqual(m.retired_failures, {0: m.failure})
        self.assertEqual(m.now, 5)
        self.clean(m)

    def test_failure_detail_prioritizes_call_before_task_index(self):
        m = Model([Task("young", 1, fail=True),
                   Task("old", 0, duration=4, fail=True)]).run()
        self.assertEqual(m.cut, 0)
        self.assertEqual(m.failure, TaskFailure(1, 1, 0, "old"))
        self.assertEqual(set(m.task_failures), {0, 1})
        self.clean(m)

    def test_skipped_task_has_no_invented_failure(self):
        m = Model([Task("skipped", 0, fail=True, dependencies=(2,)),
                   Task("observed", 0, fail=True),
                   Task("prepare", 0, duration=3)]).run()
        self.assertEqual(m.state[0], "skipped")
        self.assertEqual(set(m.task_failures), {1})
        self.assertEqual(m.failure, TaskFailure(1, 1, 0, "observed"))
        self.clean(m)

    def test_cancel_overrides_terminal_failure_but_keeps_diagnostics(self):
        m = Model([Task("failure", 0, fail=True),
                   Task("slow", 1, duration=5)]).run(cancel_at=2)
        self.assertEqual(m.outcome, "Cancelled")
        self.assertIsNone(m.failure)
        self.assertEqual(m.retired_failures, {0: TaskFailure(1, 0, 0, "failure")})
        self.assertEqual(set(m.task_failures), {0})
        self.clean(m)

    def test_invalid_explicit_dependencies_rejected(self):
        cases = [[Task("a", 0, dependencies=(p,))]
                 for p in (-1, 1, True, "0", 0)]
        cases += [[Task("a", 0, dependencies=(1,)),
                   Task("b", 0, dependencies=(0,))],
                  [Task("a", 0, dependencies=(1,)), Task("b", 1)]]
        for tasks in cases:
            with self.subTest(tasks=tasks), self.assertRaises(ValueError):
                Model(tasks)

    def test_ret_only_value_is_discarded_after_later_failure(self):
        m = Model([Task("first", 0, writes=("a",)),
                   Task("fail", 1, duration=3, fail=True)],
                  return_values=("a",)).run()
        self.assertEqual(m.records, [(0, "ok"), (1, "failed")])
        self.assertEqual(m.results, {})
        self.assertEqual(m.retired_values, {})
        self.assertTrue(m.buffers["a"].released)
        self.clean(m)

    def test_retirement_and_ret_have_independent_success_owners(self):
        for first_release in ("release_results", "release_retired_values"):
            m = Model([Task("A", 0, writes=("a",))],
                      retirement_exports=("a",), return_values=("a",)).run()
            self.assertIs(m.results["a"], m.retired_values["a"])
            self.assertEqual(m.buffers["a"].holds, 2)
            getattr(m, first_release)()
            self.assertEqual(m.buffers["a"].holds, 1)
            self.assertFalse(m.buffers["a"].released)
            self.clean(m)
            self.clean(m)  # Empty external-owner release is idempotent.

    def test_release_retirement_owner_before_ret_preserves_return_hold(self):
        m = Model([Task("first", 0, writes=("a",)),
                   Task("slow", 1, duration=5)],
                  retirement_exports=("a",), return_values=("a",))
        m.issue()
        m.now, i = heapq.heappop(m.events)
        m.complete(m.completion_keys[i])
        m.retire()
        self.assertEqual(m.records, [(0, "ok")])
        self.assertEqual(m.results, {})
        self.assertEqual(m.return_holds, {"a"})
        m.release_retired_values()
        self.assertEqual(m.buffers["a"].holds, 1)
        m.run()
        self.assertEqual(m.results["a"].payload[0], 1)
        self.clean(m)

    def test_failed_or_cancelled_run_preserves_only_handed_off_values(self):
        for cancelled in (False, True):
            m = Model([Task("first", 0, writes=("a",)),
                       Task("later", 1, writes=("b",), duration=5,
                            fail=not cancelled)],
                      retirement_exports=("a", "b"), return_values=("a", "b"))
            m.run(cancel_at=2 if cancelled else None)
            self.assertEqual(m.outcome, "Cancelled" if cancelled else "OperationFailed")
            self.assertEqual(set(m.retired_values), {"a"})
            self.assertEqual(m.results, {})
            self.assertEqual(m.buffers["a"].holds, 1)
            self.assertTrue(m.buffers["b"].released)
            self.clean(m)

    def test_cancel_discards_completed_but_unretired_exports(self):
        m = Model([Task("old", 0, duration=5),
                   Task("young", 1, writes=("a",))],
                  retirement_exports=("a",), return_values=("a",)).run(cancel_at=2)
        self.assertEqual(m.records, [])
        self.assertEqual(m.results, {})
        self.assertEqual(m.retired_values, {})
        self.assertTrue(m.buffers["a"].released)
        self.clean(m)

    def test_failed_call_does_not_export_successful_sibling(self):
        m = Model([Task("success", 0, writes=("a",)),
                   Task("failure", 0, duration=3, fail=True)],
                  retirement_exports=("a",), return_values=("a",)).run()
        self.assertEqual(m.records, [(0, "failed")])
        self.assertFalse(m.retired_values or m.results)
        self.clean(m)

    def test_cancellation_drains_and_suppresses_retirement(self):
        m = Model([Task("gpu", 0, writes=("g",), backend="gpu", duration=8),
                   Task("cpu", 1, writes=("c",), duration=4),
                   Task("join", 2, ("g", "c"), ("r",))], return_values=("r",)).run(cancel_at=2)
        self.assertEqual((m.outcome, m.now, m.records), ("Cancelled", 8, []))
        self.clean(m)

    def test_cancel_at_completion_precedes_retirement(self):
        m = Model([Task("a", 0, writes=("a",))], return_values=("a",)).run(cancel_at=1)
        self.assertEqual(m.records, [])
        self.assertEqual(m.outcome, "Cancelled")
        self.clean(m)

    def test_empty_call_and_no_tasks(self):
        self.assertEqual(Model([], calls=2).run().records, [(0, "elided"), (1, "elided")])

    def test_resource_rejection_has_no_issue(self):
        m = Model([Task("A", 0, writes=("a",), scratch=2)],
                  retirement_exports=("a",), return_values=("a",), budget=7).run()
        self.assertEqual(m.outcome, "ResourceExhausted")
        self.assertEqual(m.trace, [])
        m.release_retired_values()
        m.release_results()
        self.assertFalse(m.retirement_holds or m.return_holds)
        self.assertEqual(m.results, {})

    def test_unknown_export_or_return_rejected(self):
        for options in ({"retirement_exports": ("missing",)},
                        {"return_values": ("missing",)}):
            with self.assertRaises(ValueError):
                Model([Task("A", 0)], **options)

    def test_empty_byte_tile_rejected(self):
        with self.assertRaises(ValueError):
            Model([], tile_bytes=0)

    def test_independent_buffers_and_retained_export(self):
        m = Model([Task("A", 0, writes=("a", "b"))], return_values=("a", "b"))
        self.assertIsNot(m.buffers["a"].payload, m.buffers["b"].payload)
        m.run()
        self.assertFalse(m.buffers["a"].released)
        self.clean(m)

    def test_stale_and_duplicate_completion_rejected(self):
        m = Model([Task("A", 0)], activation=9)
        with self.assertRaises(ValueError):
            m.complete((8, 0))
        m.run()
        with self.assertRaises(ValueError):
            m.complete((9, 0))

    def test_parallel_edge_swap_and_duplicate_argument(self):
        a, b = Buffer(bytearray([1]), 1, True), Buffer(bytearray([2]), 1, True)
        incoming = capture_edge((a, b), (1, 0, 1))
        self.assertEqual([v.payload[0] for v in incoming], [2, 1, 2])
        for v in incoming:
            v.drop()
        self.assertTrue(a.released and b.released)

    def test_reject_later_dependency_cycle_and_bad_effect(self):
        for tasks in ([Task("a", 0, ("b",)), Task("b", 1, writes=("b",))],
                      [Task("a", 0, ("b",), ("a",)), Task("b", 0, ("a",), ("b",))],
                      [Task("prep", 0), Task("effect", 0, effect=True)]):
            with self.assertRaises(ValueError):
                Model(tasks)

    def test_random_dags_against_serial_values(self):
        rng = random.Random(20261019)
        for _ in range(200):
            tasks, expected = [], {}
            for i in range(12):
                reads = tuple(str(j) for j in range(i) if rng.random() < .2)
                expected[str(i)] = (1 + sum(expected[v] for v in reads)) % 256
                explicit = tuple(j for j in range(i) if rng.random() < .15)
                tasks.append(Task(str(i), i, reads, (str(i),), rng.randrange(1, 10),
                                  rng.choice(("cpu", "gpu")), dependencies=explicit))
            m = Model(tasks, return_values=tuple(expected),
                      retirement_exports=tuple(expected)[::2]).run()
            self.assertEqual(m.records, [(i, "ok") for i in range(12)])
            self.assertEqual({v: b.payload[0] for v, b in m.buffers.items()}, expected)
            completed = {n: t for t, k, n in m.trace if k == "complete"}
            issued = {n: t for t, k, n in m.trace if k == "issue"}
            for i, task in enumerate(tasks):
                for p in task.dependencies:
                    self.assertLessEqual(completed[tasks[p].name], issued[task.name])
            self.clean(m)


if __name__ == "__main__":
    if "--test" in sys.argv:
        unittest.main(argv=[sys.argv[0]], verbosity=2)
    else:
        model = Model(example(), return_values=("e",)).run()
        for row in model.trace:
            print(*row)
        print("outcome:", model.outcome, "logical ticks:", model.now)
        model.release_retired_values()
        model.release_results()
