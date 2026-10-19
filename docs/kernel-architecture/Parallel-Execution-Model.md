# Parallel Execution and Ordered Retirement

Status: Proposed breaking design; see the [Plan contract](Compiler-and-Execution.md) for implementation status. [Chinese reader version](zh/Parallel-Execution-Model.zh.md).

## Scope and ownership

One coordinator owns each Run's states, dependency counters, ready queues, call records and holds. A context supplies a bounded CPU pool and GPU submission queues shared by Runs. Workers and device callbacks write reserved completion records; they never mutate dependency or retirement state.

```text
                        Run coordinator
                  /            |            \
             CPU ready      GPU ready    calls + retire_head
                 |              |                |
             CPU workers    GPU submit      ordered delivery
                 \              /
                reserved completion records
                           |
                    Run coordinator
```

Multi-issue means admitting all eligible tasks that fit available credits during one coordinator turn. Per-backend ready heaps prioritize `(call ordinal, task ID)`; context admission rotates eligible Runs fairly. Dependency processing is O(T + E); heaps add O(T log T). No hardware clock is simulated.

## State and invariants

```cpp
#include <cstdint>
namespace ps_executor_design {
enum class TaskState : std::uint8_t {
  Waiting, Ready, Submitted, Succeeded, Failed, Skipped
};
struct TaskRuntime { TaskState state; std::uint32_t pending; };
struct CallRuntime {
  std::uint32_t unsettled, first_failed_task;
};
struct CompletionKey {
  std::uint64_t run, activation;
  std::uint64_t submission;
};
}  // namespace ps_executor_design
```

`pending(t)` counts distinct predecessors that have not completed successfully. Reading two outputs from one producer waits for one event. Lifetime holds count distinct `(consumer task, tile)` pairs instead; two ports reading one tile hold it once. Every future consumer hold exists before producers can finish.

Each accepted submission owns exactly one terminal completion record. A SubmissionId maps to one task or one immutable packet member span; it is distinct from TaskId. The coordinator checks the submission key/state once before fanning its event out to member task states. `Submitted` includes CPU work queued to a worker and accepted device work. Success means every output write and input/scratch access has ended, with required visibility. Rejection transfers no ownership and enqueues no callback; the coordinator handles a local failure. Success makes all task outputs available together; failure exposes none.

Execution credit and scratch return on completion. A call is settled when all members are terminal; it retires only at `retire_head`. A selected call succeeds only if all its tasks succeed. Zero-task pure calls retire as `Elided`. A call with 100 tasks can run with two execution slots because task slots do not wait for retirement.

On success, the coordinator marks outputs available, visits each successor once, settles the call member, releases input holds/scratch and returns credit. It then retires the contiguous settled prefix and admits more work. Consumer readiness does not wait for retirement. User delivery runs separately.

```text
Two CPU slots:
call order:       A(10), C(A,1), B(2), D(B,1), E(C,D,1)
issue order:      A, B, D, C, E
completion order: B, D, A, C, E
retirement order: A, C, B, D, E
elapsed ticks:    12
```

## Admission and progress

The baseline reserves the block's full output set, persistent frame state, bounded delivery storage, usable scratch lanes and completion capacity before its first task. [Tile storage](Cache-Model.md) defines the bound. Failure returns `ResourceExhausted` before any task in that block starts; earlier blocks' results/effects remain observable.

A task is eligible when active, unsubmitted, dependency-ready, before the failure cut and able to acquire all credits. An effect additionally requires its call to equal retirement head. A busy scratch lane can be bypassed temporarily; fair selection revisits it. Tasks acquire their complete multi-domain resource vector atomically.

The coordinator drains events, applies stops, skips work, retires calls and fills lanes. It sleeps only if accepted work or a concrete context-credit event can make progress. A rejected block reservation fails instead of waiting indefinitely. Unfinished work with no in-flight or eligible task after successful admission is an invariant failure (`Internal`).

Progress assumes a finite DAG, dependencies within the same or earlier call, finite callbacks, eventual device completion, fair context service and the complete reservation. The earliest unfinished prefix call has either in-flight work or a task whose predecessors can finish. Its remaining ready work fits a guaranteed lane after current work completes. Each completion reduces unfinished work. This is a resource/scheduling guarantee under these assumptions, not a termination proof for arbitrary native code.

## Failure, effects and cancellation

`failure_cut` is the smallest ordinal of an observed task-failing call, initially the call count. Continue all work strictly older than the cut. Skip unsubmitted work at or after it; drain accepted work there. Failure never satisfies a successful dependency. Skipped consumers release their future holds once. A later event from an older failing call moves the cut backward.

Retire the successful older prefix and then one failure record for the cut call after its accepted members quiesce. No younger call retires. Return final failure only after all younger accepted work drains too. For several failures in that call, select the lowest static TaskId among actually failed tasks. Skipped tasks have no invented error. This is stable for the observed failures, not a promise to evaluate every possible sibling failure. Failure-cut suppression uses a separate internal stop mechanism; it must not cancel the older prefix with the user cancellation token.

The prototype records `TaskFailure(activation, task_id, call, task_name)` for each actual failed completion in `task_failures`. At failure retirement, `retired_failures[call]` records the lowest observed TaskId in that call. The terminal `failure` is set only for an `OperationFailed` Run; cancellation keeps observed/retired failure diagnostics without presenting them as the terminal cause. Skipped tasks produce no failure detail. Packet-scoped attribution remains separate and is not modeled by this prototype.

An effectful call has pure preparation and one terminal effect task. It starts only at retirement head after its own predecessors succeed, so all earlier calls have retired successfully. Later pure computations may already have run. Partial external writes before an effect failure cannot be rolled back. Several ordered external effects require separate calls or an operator-owned transactional protocol.

User cancellation is a Run abort. The coordinator closes admission, skips unsubmitted work, drains accepted work and suppresses further retirement. Already handed-off records remain valid and their delivery may finish. Before terminal status is latched, observed cancellation takes priority over Stale, then context/device/observer abort, then task-local failure. Within the context/device/observer category, retain the first coordinator-observed cause. Later requests cannot alter a latched outcome. Fatal device loss that prevents older work from completing uses context/device abort rather than claiming ordinary successful-prefix completion.

Graph/binding invalidation and observer failure also abort. Same-Run callback serialization, delivery failure and terminal latching follow the [return interface](Compiler-and-Execution.md#run-control-flow-and-return-interface). Device loss must establish DMA/callback quiescence before freeing storage. The backend retains leases until then. A false success, escaped CPU task or out-of-range write violates the trusted operator contract.

## CPU and GPU completion boundary

A CPU invocation receives immutable input views, exclusive preallocated outputs, private bounded scratch, parameters and cancellation. Synchronous `invoke` returns status after all accesses and internal work end. Workers publish with release ordering and the coordinator consumes with acquire ordering. Baseline kernels use one worker each; parallel phases lower into tasks rather than block workers on nested pool jobs.

The backend interface is logically `SubmitResult submit(const Invocation&, CompletionSlot&) noexcept`, returning `Rejected(Status)` or `Accepted(Ticket)`. Acceptance transfers owners until exactly one completion; rejection keeps them with the coordinator and enqueues no completion. Arm the slot before submit, so synchronous completion is legal. Callbacks only write bounded status and signal a wakeup; they allocate nothing and invoke no user code.

GPU encoding and queue acceptance do not make outputs readable. Input/output/scratch, parameters, commands, pipeline/module owners and completion slots live through device quiescence. Host-visible completion includes required cache maintenance. GPU consumers require access/layout transitions and memory visibility, including in shared memory. Cross-domain transfers are explicit COPY tasks.

Completion keys and queue timepoints are different. Task 7 completing does not imply tasks 0 through 6 completed. GPU timeline values follow actual submission order on one ordered queue; independent queues use independent timelines. TaskId and CallId must never be used as a universal completed counter.

### Device packets

Individual submissions are the baseline. Optional compiler packets batch a same-device subgraph whose external incoming edges are all satisfied before submission and whose outgoing edges become visible at packet completion. First-version packets contain no effect and belong to one call. Validate the contracted packet graph for cycles: grouping A tasks around an intervening dependent B can create an A/B cycle.

A packet consumes one backend submission credit and one completion slot; its member tasks are all Submitted until that single event is processed. Device barriers enforce internal edges. Reserve all outputs and the full packet scratch/command bound first. Success settles packet members in topological order in one event. If member-level failure information is unavailable, fail the whole call with `PacketFailure(submission, members)` and expose no packet output. Do not invent a first failed member; retain packet-scoped provenance separately from task-local error selection. Restricting packets to one call preserves cross-call failure attribution. Cross-call chains remain separate until a backend offers adequate per-task completion semantics. Batching can delay consumers; measure that cost against submission overhead.

## Diagnostics and checks

Measure ready, submit, start, device completion and retirement separately. Keep backend in-flight peaks, ready/scratch waits, transfers and drain latency. Only subtract timestamps in a comparable clock domain. The first C++ implementation exposes these additional counters:

| Metric | Definition |
| --- | --- |
| `reserved_bytes_by_domain` | Current admitted budget capacity by memory domain, including unused block reservation; count each lease once |
| `live_bytes_by_domain` | Currently allocated controlled bytes by domain, including retained returned owners; allocation-size accounting, not logical image size or RSS |
| `allocation_count` | Cumulative successful materialized tile allocations in the Run; exclude scratch, commands and borrowed input owners |
| `block_admission_failure` | Admission-failure count plus block/activation, resource dimension, requested capacity and available capacity; report before issuing that block |
| `retirement_head_wait` | Monotonic time from a call becoming head until it is settled and eligible to retire; exclude callback delivery delay and report unfinished waits separately on abort |

Report current and peak reserved/live capacity. At block end, unused reservation returns to the context while exported owners remain charged; releasing those owners updates the accounting even after Run completion. Declining live bytes does not imply declining reserved bytes within a block. [Capacity examples](Cache-Model.md#capacity-cost-and-measurement) quantify this baseline cost.

The first native GPU evaluation covers four distinct workload shapes:

| Workload | Required observations |
| --- | --- |
| Many tiles within one call | Throughput, packet size, submissions, allocation count and peak capacity |
| Independent calls | Actual overlap, queue occupancy and time waiting for retirement head |
| Dependent GPU calls, such as blur -> color transform -> blend | Device-complete to next-submit gap, per-call latency and host wakeups |
| CPU/GPU chain with explicit COPY | Transfer bytes/time, host/device synchronization and lifetime through drain |

Same-call packets cannot pre-submit a dependent cross-call chain under completion-based readiness. Measure that host-mediated gap before deciding on finer device completion points or cross-call orchestration. Compare identical shapes, tile extent, placement, worker/queue grants and resource limits, recording the physical device and driver. Block coalescing must be evaluated together with capacity and admission failures; keep full-block reservation until a separately justified policy replaces it. These are implementation acceptance measurements, not results from the Python model.

The prototype checks explicit ordering without payload holds, independent retirement/RET ownership, observed task-error selection, forwarding before retirement, large groups, failure cuts, effects, cancellation and release. It uses logical ticks and scans/sorts tables for clarity. C++ uses precompiled adjacency, dependency counters, ready queues and unsettled call counts. Remaining checks include complete CFG/loops, selection closure rebuilding, callback delivery and finalization races, multi-domain/metadata capacity, Accepted/Rejected ownership races, inline completion, device loss, visibility and real packet completion/failure.

## Primary design references

- [BOOM ROB](https://docs.boom-core.org/en/latest/sections/reorder-buffer.html): in-order commit and head exception handling motivate call records and a retirement cursor, without speculative register state.
- [IREE Stream](https://iree.dev/reference/mlir-dialects/Stream/) and [HAL](https://iree.dev/reference/mlir-dialects/HAL/): explicit asynchronous resources and wait/signal fences motivate declared completion dependencies.
- [CUDA Graphs](https://docs.nvidia.com/cuda/cuda-programming-guide/04-special-topics/cuda-graphs.html): separate graph definition, instantiation and execution motivate prepare-once data and optional packets.
- [Metal completion](https://developer.apple.com/documentation/metal/mtlcommandbuffer/addcompletedhandler(_:)): GPU completion notification motivates the distinction between submit and Done.
  Apple's [archived command guide](https://developer.apple.com/library/archive/documentation/Miscellaneous/Conceptual/MetalProgrammingGuide/Cmd-Submiss/Cmd-Submiss.html) (updated 2016-12-12) also documents command-buffer retention and short completion handlers; only these foundational lifetime rules are used here.
- [Vulkan synchronization](https://docs.vulkan.org/spec/latest/chapters/synchronization.html): execution order, availability and visibility require explicit backend synchronization.

These sources motivate selected mechanisms; they provide no evidence of a completed Photospider implementation.
