# ADR 0007: Compute Runs and Process Execution Have Separate Owners

## Status

Accepted as a target architecture. None of `ComputeRun`, the injected
process-owned `ExecutionService`, `GraphRevision`, the target
`ResourceLedger`, or the policy-only scheduler generation is current software
behavior at the time of this decision.

This decision refines and supersedes ADR 0003 as the detailed ownership and
lifecycle contract. ADR 0003 remains the historical high-level decision to move
physical execution resources out of each Graph. ADR 0001 remains fully in
force.

## Context

The current implementation has one HP and one RT `IScheduler` per
`GraphRuntime`. Each scheduler owns workers, ready queues, epochs, completion
counters, exception publication, and ordering policy. The function-static
`SchedulerWorkerBudget::process()` limits conservative scheduler worker charges
to 32 across embedded Hosts, but it owns no worker, ready-store capacity, Run,
memory, scratch, device, I/O, or plugin-process budget.

Current `TaskHandle` values borrow a `TaskExecutor*`. `TaskSubmissionPlan`,
temporary results, dependency state, and executor objects stay on a request
lifetime bounded by the matching scheduler completion wait. Moving those
handles into a process queue without first creating a stable request lifetime
would introduce use-after-free and cross-request completion risks.

ADR 0003 chose the direction—request-owned `ComputeRun`, process-owned
`ExecutionService`, host-owned `ResourceLedger`, and policy-only
`SchedulerPolicy`—but did not decide:

- whether one HP/RT request has one Run or multiple domain Runs;
- which inputs are immutable and which request-local objects require stable
  storage;
- the Run state machine, terminal outcomes, and cancellation/commit race;
- the exact ready-submission and completion-routing boundary;
- the target positive and negative ownership of `GraphRuntime`;
- who may mint and release resource reservations and grants;
- graph-close versus process-shutdown drain scope; or
- the dependency contract consumed by issues #66–#76.

Leaving those choices implicit would let independent migration slices build
incompatible lifetimes, queues, token authorities, and commit rules.

## Decision

### Run identity and request relationship

Each independently executable single-domain plan has one `ComputeRun`. A
non-realtime HP request owns one Run. A request coordinating HP and RT siblings
owns a request/run-group identity and one child Run per domain. Child Runs have
distinct identities, plans, dispatchers, staged outputs, resource reservations,
and terminal outcomes; grouping never creates HP-to-RT or RT-to-HP task
dependencies.

`RunId` is opaque, stable, and never reused during the owning
`ExecutionService` lifetime. A task is identified by
`(RunId, RunLocalTaskId)`; a local task id has no meaning outside its Run.
Request, parent, and run-group ids may record provenance but do not replace the
Run's unique identity.

Before asynchronous planning or execution, the Run captures an immutable
descriptor containing at least:

- graph identity and `GraphRevision`;
- request target and input snapshot;
- `ComputeIntent` and quality;
- QoS class, monotonic deadline, weight, and maximum parallelism; and
- optional supersession key and generation.

`ComputeIntent`, QoS, resource policy, and commit policy remain independent.
None is inferred from another.

### Monotonic state and one terminal outcome

The target phase progression is:

```text
Created -> Admitted -> Queued -> Running -> CommitPending -> Terminal
```

Safe paths may skip nonterminal phases, including cache-hit, admission-failure,
and early-cancellation paths. A Run never moves backward and never leaves
`Terminal`.

Exactly one terminal outcome is published:

- `Succeeded`, only after validated visible commit or validated no-op success;
- `Failed`, carrying the exact host-owned failure or exception category,
  including admission failure; or
- `Cancelled`, carrying a stable reason such as explicit request, graph close,
  process shutdown, supersession, or deadline.

Operation completion alone is not success. The dispatcher must finish
dependency aggregation and the graph-state commit transaction must validate the
Run predicate.

Cancellation intent is monotonic. Cancellation, failure, and commit contenders
use one Run-owned terminal arbiter. The first accepted claim wins; later claims
may only complete cleanup and telemetry. Commit validation, visible publication,
and the success claim form one serialized graph-state transaction under the
same terminal gate:

- cancellation or failure accepted first denies commit;
- a valid commit accepted and published first makes later cancellation a no-op;
  and
- late task or device completion never changes a terminal outcome.

Terminal publication and physical quiescence are different conditions. A
cancelled or failed Run may be terminal while non-preemptible work drains. Run
reclamation waits for quiescence and resource release.

### Stable Run storage and leases

The Run control block is the stable owner of:

- its immutable descriptor and single-domain plan;
- dispatcher dependency counters and dependent maps;
- the Run-local task namespace and completion endpoint;
- temporary results and staged output;
- first-failure/exception, cancellation, and terminal-arbiter state;
- commit policy and captured revision;
- reservations, grants, and Run-scoped accounting; and
- terminal and cleanup telemetry.

A `RunLease` is a strong, non-forgeable lifetime lease to that control block. It
is not Graph ownership and is not a resource token. Every accepted ready-store
entry, executing callback, completion record, dispatcher continuation, and
commit continuation owns or transfers one lease.

Enqueue rollback releases its candidate lease. Dequeue transfers the
ready-store lease to execution. Completion routing retains a lease until the
matching dispatcher accepts the result.

Run destruction is non-throwing and occurs only after:

1. one terminal outcome is published;
2. ready, running, completion, dispatcher, and commit work is quiescent;
3. every `RunLease` has released;
4. every reservation and grant has released exactly once; and
5. no continuation can publish Run or graph state.

Destruction does not implicitly cancel, publish an outcome, or wait on work
that should still hold a lease. Dropping a caller future or observer does not
cancel an admitted Run; cancellation is explicit or lifecycle-driven.

### Dispatcher-owned ready release

ADR 0001 continues to govern dispatch. `ComputeTaskDispatcher` alone owns:

- task dependencies and counters;
- ready detection and dependent release;
- source-first dirty release;
- temporary-result indexing;
- completion aggregation; and
- creation of newly ready submissions.

`ExecutionService` accepts only `ReadyTaskSubmission` values whose compute
dependencies are already satisfied. A submission contains immutable execution
metadata, `(RunId, RunLocalTaskId)`, an owned or otherwise stable executable
handle, resource estimates/requirements, and a `RunLease` bound to the matching
completion endpoint.

The service never receives or derives `GraphModel`, `ComputePlan`,
`ComputeTaskGraph`, dirty snapshot/state, dependency maps, cache authority, or
visible commit authority. `TaskCompletion` returns through the Run lease to the
matching dispatcher, which validates the Run and local-task namespace before
changing dependency state. Newly ready dependent work re-enters process
admission, the bounded ready store, and global policy; a permanent worker-local
path may not bypass fairness, cancellation, or Run isolation.

### Target ownership boundaries

| Owner | Owns | Does not own |
| --- | --- | --- |
| Request / `ComputeRun` | Run identity, immutable inputs, request plan and dispatcher state, staged/temporary output, exception/cancellation/terminal state, Run reservations, commit policy, Run telemetry | Graph state, process workers, ready-store policy, resource mint authority |
| `GraphRuntime` | `GraphModel`, graph-scoped state, graph-state lane, monotonic `GraphRevision`, revision capture, serialized commit validation/publication, graph events, platform/session metadata, graph open/closing lifetime state | Runs, CPU/device/I/O/plugin workers, process ready store, admission, `ResourceLedger`, `SchedulerPolicy`, physical schedulers |
| Process `ExecutionService` | physical CPU workers and later resource executors, bounded ready storage, Run/resource admission, policy-result validation, execution exception fences, completion routing | task planning/dependencies, Graph/document persistence, cache authority, dirty propagation, visible commit, Graph state |
| `ResourceLedger` | checked process limits, transactional reservations, validated child grants, exact-once release accounting | ordering policy, task dependencies, Graph state, third-party token delegation |
| `SchedulerPolicy` | ranking immutable ready descriptors and suggesting a bounded quantum | workers, ready store, Runs, Graph state, reservations/grants/tokens, native device handles, executors, completion or lifecycle authority |

The product composition root constructs one `ExecutionService` from explicit
process configuration and injects it. It is not a static singleton. Tests and
worker products can create and destroy isolated domains. Embedded Hosts that
belong to one product composition share the explicitly supplied service.

The root constructs the service before injected Kernels/Hosts and keeps it
alive until they have stopped Run admission and drained their Runs. Planning,
persistence, cache, dirty propagation, and visible commit remain outside the
deep module.

### Host-authoritative resource accounting

`ExecutionService` exclusively owns an internal `ResourceLedger`, initialized
from composition-root limits. Only trusted host code can mint its move-only,
non-forgeable reservations and execution grants. A built-in or third-party
policy, operation plugin, or policy plugin may request or suggest resources but
cannot construct, duplicate, enlarge, or directly release a token.

The ledger ultimately accounts for:

- CPU execution capacity;
- ready-store entries and bytes;
- retained/in-flight memory and scratch;
- per-device queue, in-flight, memory, and scratch capacity;
- compute-I/O operations and bytes; and
- plugin-process, invocation, IPC/shared-memory, and isolation capacity.

Admission validates one checked resource vector transactionally and returns a
complete Run reservation or nothing. A policy may rank work and suggest a
bounded quantum. Trusted service code validates the suggestion against process
limits and the Run reservation before the ledger mints a child grant.

Every reservation and grant releases exactly once after success, failure,
cancellation, rollback, graph close, process shutdown, or worker failure. A Run
reservation cannot release while child grants remain live. Checked overflow or
capacity exhaustion never overcommits, partially reserves, or silently clamps.
Synchronous documented allocation exhaustion remains `std::bad_alloc`;
asynchronous failure is captured by the exact Run failure channel and cannot
commit partial output.

The current `SchedulerWorkerBudget` remains transitional current behavior until
its migration slice. It is not wrapped, renamed, or aliased into the target
ledger and disappears with per-Graph worker ownership.

### Policy-only scheduler generation

`SchedulerPolicy` may inspect immutable ready descriptors and trusted
host-supplied capacity/telemetry snapshots, rank eligible work, and suggest a
bounded quantum. Trusted service code validates every result. Invalid or
excessive suggestions execute no work and mutate no ledger state.

After built-in interactive and throughput policies prove the seam, the current
worker-owning scheduler ABI is replaced as one complete breaking migration. The
new generation does not adapt `IScheduler`, forward old worker-count grants, or
retain a permanent compatibility shim. The exact replacement ABI is decided
and implemented by issue #75 under these ownership constraints.

### Revision, staged commit, cancellation, and supersession

`GraphRuntime` advances a monotonic `GraphRevision` for graph mutations relevant
to compute correctness. A Run captures one immutable revision before planning.
The target graph-state lane is held for revision capture and validated visible
commit, not for long-running planning/execution.

The minimum commit predicate requires:

- the Graph is still open and the Run retains a valid graph-lifetime lease;
- the current authoritative revision equals the captured revision;
- any supersession key/generation remains current;
- cancellation or failure has not claimed terminal state; and
- dispatcher completion and staged output are valid for this Run.

Revision compatibility is never inferred from equal topology or similar
output. Any future compatible-revision optimization requires a new explicit
decision. Failed validation discards staged output and cannot mutate visible
graph state.

Supersession makes a newer generation current and requests cancellation of
older matching Runs. It does not mutate their plans or reuse their identity.
Non-preemptible work and external side effects may finish, but stale,
cancelled, failed, or overdue output cannot commit.

### Close and shutdown scopes

Graph close:

1. publishes graph closing, stops new Run admission and ordinary external
   graph-state admission for that Graph, and preserves lifecycle admission only
   for already-admitted Run settlement;
2. denies visible commit and cancels or drains only Runs for that Graph;
3. lets their completion, cancellation, and commit continuations enter the
   graph-state finalization path only to observe closing, discard staged output,
   publish terminal state, and release ownership;
4. waits for their ready, running, completion, dispatcher, commit, and resource
   leases to become quiescent;
5. stops and drains the graph-state lane, then destroys graph state; and
6. leaves `ExecutionService` and unrelated Graph Runs running.

Process execution-domain shutdown:

1. stops global admission of new Runs and ordinary external graph-state work;
2. chooses cancellation or drain for every admitted Run while keeping bounded
   ready submission, execution, completion routing, and graph-state
   finalization available only for those Runs;
3. settles every Run and releases its reservations and grants exactly once;
4. stops remaining ready/execution admission after Run quiescence;
5. joins all physical executors; and
6. destroys `ExecutionService`.

Worker and operation exceptions are fenced at the execution boundary and routed
through the matching Run lease. They cannot escape a worker thread, fail a
different Run, or skip resource release. A completion arriving after terminal
publication or graph close performs cleanup only.

## Delivery Boundaries

This decision fixes the dependency contract without implementing the slices:

| Issue | Consumes this decision | Explicit non-goal of the slice |
| --- | --- | --- |
| #66 | HP `ComputeRun` descriptor, state, storage, and one terminal outcome | Process worker migration |
| #67 | Stable Run leases and `(RunId, RunLocalTaskId)` completion isolation | Shared CPU service |
| #68 | Injected CPU-only `ExecutionService` for one Run, ready-only input | Multi-Graph migration and final ledger |
| #69 | Shared multi-Graph and HP/RT CPU domain; removal of per-Graph workers | Full admission/policy model |
| #70 | Production admission, bounded ready store, and `ResourceLedger` | Fairness algorithms |
| #71 | Interactive and throughput built-in policies | Plugin ABI migration |
| #72 | `GraphRevision` capture and staged commit predicate | Cooperative cancellation |
| #73 | Queued/running/commit cancellation, joining #70 and #72 | Latest-wins policy |
| #74 | Latest-wins supersession after #71 and #73 | Scheduler ABI replacement |
| #75 | Complete policy-generation ABI replacement after #71 | Permanent old/new adapter |
| #76 | Graph close, process shutdown, telemetry, and final invariants after #69/#73/#74/#75 | New execution-domain capabilities |

The dependency graph is acyclic. #72 may proceed after #67 in parallel with
#68–#71; #75 may proceed after #71 in parallel with #72–#74. Current-state
documentation changes only when each implementation and its long-lived
behavioral tests land.

## Consequences

- Request-local state can safely outlive a caller stack without transferring
  task-graph correctness into the execution service.
- Different Runs may reuse local task ids without completion or exception
  cross-talk.
- Graph count no longer determines target CPU worker count, while Graph close
  remains graph-scoped.
- One trusted ledger becomes the final authority for every physical-resource
  dimension; policy and plugin code cannot mint capacity.
- Cancellation may publish terminal state before non-preemptible work is
  reclaimed, so tests and telemetry must distinguish terminal from quiescent.
- Exact revision equality is conservative and may discard reusable work. Any
  relaxation requires an explicit later decision and proof.
- The scheduler ABI migration is deliberately breaking.
- Device, I/O, general-data, plugin-isolation, and server-control-plane details
  can evolve without changing the ready-task or resource-authority boundary.

## Testability and Failure Invariants

As behavior lands, long-lived tests must cover:

- Run-id and local-task isolation;
- monotonic phases and exact-once terminal races;
- caller-observer destruction and lease-backed lifetime;
- ready-only submission and dispatcher-owned dependent release;
- bounded process admission and fail-closed arithmetic;
- exact release on every success, failure, cancellation, rollback, close,
  shutdown, and worker-failure path;
- revision-safe staged commit and non-preemptible cancellation;
- interactive/batch progress under policy;
- graph-local close and process-global shutdown; and
- exception routing that cannot contaminate another Run.

CTest and CI remain reserved for lasting software behavior. Issue-specific
replay, provenance, migration-residue, phase-completion, and result
orchestration are not registered or retained.

## Rejected Alternatives

### Make one mixed-domain Run for HP and RT

Rejected because HP and RT have independent plans, staged outputs, commit roles,
and terminal cleanup. A group can coordinate them without creating cross-domain
task dependencies.

### Move dependency counters into the process ready store

Rejected because it violates ADR 0001 and couples execution policy to Graph and
task-graph semantics.

### Extend borrowed TaskExecutor lifetime with a longer wait

Rejected because process queues, device completion, compute I/O, and
cross-Run fairness may outlive that caller stack and wait.

### Reuse SchedulerWorkerBudget as ResourceLedger

Rejected because the transitional counter has the wrong resource model, hidden
process-static ownership, and no Run, queue, memory, scratch, device, I/O, or
plugin-process authority.

### Let SchedulerPolicy mint grants

Rejected because an untrusted or defective strategy could overcommit resources
and evade exact-release accounting.

### Preserve the worker-owning scheduler ABI behind an adapter

Rejected because physical workers and tokens would remain outside the sole
host-authoritative execution domain.

### Let GraphRuntime own Runs or the process ready store

Rejected because Graph count would again multiply physical ownership and graph
close could stop unrelated work.

## Relationship to Other Decisions and Documentation

- [ADR 0001](0001-graph-state-access-is-not-scheduler-dispatch.md) remains
  authoritative for graph-state/ready-dispatch separation.
- [ADR 0003](0003-process-owned-execution-resources.md) remains the historical
  high-level decision and is superseded by this ADR only for detailed target
  ownership and lifecycle.
- [ADR 0006](0006-kernel-documentation-separates-facts-decisions-targets-and-status.md)
  requires current facts, this accepted target decision, roadmap direction, and
  Issue/Project status to remain distinct.
- [Kernel Evolution](../roadmap/Kernel-Evolution.md#run-and-process-execution-domain-contract)
  records the durable target and delivery dependency order.
- Current behavior remains authoritative in
  [Compute Boundaries](../kernel-architecture/Compute-Boundaries.md),
  [Compute Flow](../kernel-architecture/Compute-Flow.md), and
  [Scheduler Architecture](../kernel-architecture/Scheduler-Architecture.md)
  until implementation and durable verification promote each target.
