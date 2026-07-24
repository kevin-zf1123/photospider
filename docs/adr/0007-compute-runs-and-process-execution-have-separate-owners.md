# ADR 0007: Compute Runs and Process Execution Have Separate Owners

## Status

Accepted and implemented through Issue #76. Issues #70 through #76 are current software
behavior for the CPU execution/resource, policy, and private-route slice: the
embedded composition root injects one fixed `ExecutionService`; built-in CPU
HP, RT, connected-parameter preflight, and dirty source/downstream work crosses
a move-only `ReadyTaskSubmission` boundary. Independent Runs from multiple
Graphs may overlap, with Run-local completion, failure, trace, and Host state.
Realtime requests create one request-owned `RunGroup` with distinct HP and RT
child Runs. `GraphRuntime` stores only copied HP/RT route ids and nonzero
generations; no Graph owns a physical worker, queue, policy context, or plugin
DSO lifetime. The service exclusively owns one Host-authoritative
`ResourceLedger`, atomically admits complete Run vectors, and routes
initial/dependent work through an entry/byte-bounded ready store. One
Interactive and one Throughput policy binding rank immutable Host-authored
frontiers with work/byte billing, Graph/Run fairness, deadline preference,
aging, headroom, and bounded Throughput progress without owning workers or
resource authority. Product compute captures request-owned Graph/proxy
snapshots, executes outside the graph-state lane, and publishes only after exact
strong Graph identity and authoritative revision validation. A private
compute-request lane serializes same-Graph requests without owning an executor
or policy lifetime, and RT publication is independent of a later stale HP
result. Issue #73 adds private cooperative Run cancellation: explicit requests
and monotonic deadlines contend with failure and a Run-owned commit contender;
matching queued work is purged, entered work drains, dependent re-entry is
rejected, and accepted cancellation cannot publish staged state. RT
cancellation while the sibling gate is `Pending` denies HP commit and requests
HP cancellation. Installed Host, CLI, and IPC protocol v2 surfaces remain
non-cancellable and IPC jobs continue to report `cancellable: false`. Issue #74
adds checked per-Graph supersession generations, exact-key latest pending
coalescing, one persistent continuation ticket per admitted key, stable
supersession cancellation, current-generation commit authority, and
deterministic `RunGroup` aggregate settlement. The existing bounded
compute-request lane worker is the only logical active runner; no per-Graph
background runner or per-generation thread is added. Issue #75 removes the
worker-owning scheduler SDK/ABI and adds pure-C policy ABI v1, atomic
generation-scoped binding replacement, Host-authored frontier and fallback,
sticky generation-local faults, reserved start, and closed private execution
routes. Issue #76 adds the process-owned `RunLifecycleRegistry`, Graph lifetime
anchors/leases, monotonic Graph close, explicit process execution shutdown,
exact lifecycle/resource settlement, and source-private bounded telemetry.

This decision refines and supersedes ADR 0003 as the detailed ownership and
lifecycle contract. ADR 0003 remains the historical high-level decision to move
physical execution resources out of each Graph. ADR 0001 remains fully in
force.

## Context

The current implementation retains one copied execution-route binding per
intent in each `GraphRuntime`. The closed `cpu`, `serial_debug`, and
`gpu_pipeline` route ids name private Host implementations; `GraphRuntime`
owns none of their workers, ready queues, generations, completion adapters,
exception publication, or ordering policy. Policy binding is process/service
state, not Graph state.

The explicitly injected service owns a direct fixed CPU worker pool and one
private Metal worker lane. It freezes one resolved `[1,8]` CPU count before
first use, accepts multiple active Runs,
uses a policy-aware entry/byte-bounded ready store, and keeps completion, first
exception, in-flight drainage, trace Host, and settlement isolated per Run.
Its private ledger owns immutable composition limits and authority for complete
Run vectors. Fixed service threads and private route machinery remain
infrastructure rather than a Run-lifetime charge.

`ComputeRun` shared control owns the plan or dirty staging, temporary results,
and stable leases. Every built-in CPU ready task retains a non-forgeable Run
lease and `(RunId, RunLocalTaskId)`; a matching identity gates failure
publication. Full HP uses Run-owned `TaskSubmissionPlan` callbacks. Dirty HP,
RT, and connected-parameter preflight materialize heap-owned contexts and
move-only `ReadyTaskSubmission` values so no stack `TaskExecutor*` crosses the
service boundary. All three private routes use the same ready-store, policy,
reserved-start, Run-lease, and completion path.

Route-aware planning freezes the selected implementation and `Device` before
admission. `cpu` and `serial_debug` expose CPU only; `gpu_pipeline` exposes
Metal then CPU when the Host reports Metal, otherwise CPU only. CPU and Metal
submissions enter distinct fixed lanes but share the Run grant and maximum-
parallelism ceiling. No Graph, policy binding, or operation provider owns a
lane or a second device-capacity authority.

ADR 0003 chose the direction—request-owned `ComputeRun`, process-owned
`ExecutionService`, host-owned `ResourceLedger`, and policy-only comparison
seams—but did not decide:

- whether one HP/RT request has one Run or multiple domain Runs;
- how paired RT/HP outcomes, sibling cancellation, commit ordering, and caller
  completion aggregate;
- which inputs are immutable and which request-local objects require stable
  storage;
- the Run state machine, terminal outcomes, and cancellation/commit race;
- the exact ready-submission and completion-routing boundary;
- the target positive and negative ownership of `GraphRuntime`;
- which owner provides one atomic admitted-Run/graph-close/process-shutdown
  lifecycle fence;
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

The request-owned `RunGroup` is a coordination record, not a mixed-domain Run.
Its stable control block owns the group identity, one observation `RunLease` per
child, the sibling gate, cancellation fan-out, exact-once aggregate arbiter, and
caller promise. It owns no child plan, dispatcher, staged output, or resource
reservation. Its successful caller-visible payload is the RT child output, but
group success requires both children to succeed. The group chooses one
deterministic aggregate outcome after both child terminal outcomes are known:

1. any child failure makes the group `Failed`;
2. a host-classified resource-exhaustion failure outranks another failure, and
   the RT failure breaks a tie within the selected failure class;
3. when neither child failed, any child cancellation makes the group
   `Cancelled`; a group-origin lifecycle/explicit cancellation reason outranks a
   child-only reason only when its fan-out actually claimed at least one open
   child terminal arbiter. The group's monotonic request arbiter retains its
   first accepted group-origin reason for traceability, but acceptance after
   both children are terminal cannot replace their aggregate. Among
   child-winning group requests the first accepted reason remains stable, and
   otherwise the RT child reason outranks the HP child reason; and
4. only two successful children make the group `Succeeded`, carrying the RT
   output.

Explicit request cancellation, graph close, and process shutdown broadcast
cancellation to both children. RT failure or cancellation before RT commit
permanently denies the sibling commit gate and requests HP cancellation. HP
failure or cancellation does not roll back an already-published RT proxy or
overwrite the RT child's terminal state and does not request RT cancellation;
the RT child still drains to its own terminal outcome while the group reports
the deterministic HP-derived failure or cancellation when no higher-priority
outcome exists.

The group may freeze its aggregate outcome once both child terminal outcomes are
known, but the caller future is not completed until both children are physically
quiescent, graph finalization has committed or discarded every staged output,
their reservations and grants have released exactly once, both admission
attempts have resolved, and every installed admitted-Run registry entry has
unregistered. The caller-facing future stores only a copied aggregate
outcome/output value, never a `RunLease`; the group releases its child
observation leases as the final settlement action before making that future
ready. Dropping the caller future does not cancel either child. A non-realtime
single-Run future follows the same rule for its one Run.

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

The phase progression, current for private Runs through Issue #76, is:

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
- `Cancelled`, carrying a stable reason. Current private sources cover an
  explicit request, an expired monotonic deadline, issue #74 supersession, and
  issue #76 Graph-close/process-shutdown lifecycle cancellation.

Operation completion alone is not success. The dispatcher must finish
dependency aggregation and the graph-state commit transaction must validate the
Run predicate.

Issue #73 makes cancellation intent and terminal contention current.
Cancellation, failure, and commit contenders use one Run-owned terminal arbiter.
The first accepted claim wins; later claims may only complete cleanup and
telemetry. Commit validation, visible publication, and the success claim form
one serialized graph-state transaction under the same terminal gate:

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

The current Issues #70 and #71 slice implements immutable metadata, composite
identity,
owned executable, matching `ComputeRunLease`, multi-Run routing, and isolated
completion/failure settlement. It rejects borrowed handles, anonymous raw
callbacks, mixed initial Run ids, and submissions outside the matching active
Run. `TaskSubmissionPlan` and heap-owned dirty contexts still discover initial
readiness, release dependencies, own result indexes, and retire logical
completion counts. Each submission carries a uniform trusted-host demand;
whole-Run admission and checked aggregation happen before publication, and
initial/dependent submissions require matching child grants from the same
bounded store. Both paths enter the same policy route, and a Run's fairness row
survives temporary emptiness until final Run retirement.

The service never receives or derives `GraphModel`, `ComputePlan`,
`ComputeTaskGraph`, dirty snapshot/state, dependency maps, cache authority, or
visible commit authority. `TaskCompletion` returns through the Run lease to the
matching dispatcher, which validates the Run and local-task namespace before
changing dependency state. Newly ready dependent work re-enters process
admission, the bounded ready store, and global policy; a permanent worker-local
path may not bypass fairness, cancellation, or Run isolation.

### Current ownership boundaries

| Owner | Owns | Does not own |
| --- | --- | --- |
| Request / `RunGroup` | group identity, child observation leases, sibling gate, cancellation fan-out, aggregate outcome/error selection, caller promise | child plans/dispatchers/terminal arbiters, cross-domain dependencies, Graph state, process workers, resource reservations |
| Request / `ComputeRun` | Run identity, immutable inputs, request plan and dispatcher state, staged/temporary output, exception/cancellation/terminal state, Run reservations, commit policy, Run telemetry | Graph state, process workers, ready-store policy, resource mint authority |
| `GraphRuntime` | `GraphModel`, graph-scoped state, graph-state lane, monotonic `GraphRevision`, revision capture, serialized commit validation/publication, graph events, stable graph-instance identity, copied HP/RT route ids and generations, graph-lifetime anchor, platform/session metadata | Runs, admitted-Run indexes, CPU/device/I/O/plugin workers, process ready store, admission, `ResourceLedger`, `PolicyRegistry`, policy bindings, physical execution routes |
| `ExecutionService::RunLifecycleRegistry` | one process admission fence, service accepting/stopping state, graph-indexed open/closing admission rows, pending admission candidates, graph-indexed admitted `RunLease` entries, and process-wide Run enumeration | Run plans, dispatchers, terminal arbitration, staged output, Graph state, resource minting, execution policy |
| Process `ExecutionService` | the lifecycle registry, a fixed CPU pool, one private Metal lane and later resource executors, private serial-debug/GPU routes, policy-aware bounded ready storage, policy-binding state, Run/resource admission, policy-result validation, reserved start, execution exception fences, completion routing | task planning/dependencies, Graph/document persistence, cache authority, dirty propagation, visible commit, Graph state |
| `ResourceLedger` | checked composition limits, transactional reservations, validated child grants, exact-once release accounting | ordering policy, task dependencies, Graph state, third-party token delegation |
| Process `PolicyRegistry` | immutable built-in/DSO policy type records, validated pure-C callback tables, registry visibility, DSO leases | service bindings/contexts, ready work, workers, resources, Graph/Run state, completion or lifecycle authority |
| Policy binding | ranking immutable Host-authored candidate descriptors within service-owned binding state | workers, physical ready store, Runs, Graph state, budget, reservations/grants/tokens, native device handles, executors, completion or lifecycle authority |

The product composition root now constructs and injects the current fixed-lane
`ExecutionService`; it is not a static singleton. Tests create and destroy
isolated domains. Composition-root execution configuration resolves and freezes
its worker count; policy plugins cannot request, grant, or resize that pool.
Graph load, route replacement, Run submission, and dirty phases never resize
the pool. The graph-indexed `RunLifecycleRegistry` is now the sole admitted-Run
index and admission/close/shutdown fence; the former physical-execution-only
weak active-Run map has been removed.

The root constructs the service before injected Kernels/Hosts and keeps it
alive until they have stopped Run admission and drained their Runs. Planning,
persistence, cache, dirty propagation, and visible commit remain outside the
deep module.

### Admitted-Run registry and lifecycle fence

`ExecutionService` owns one private `RunLifecycleRegistry` as part of its
admission subsystem. This is the authoritative choice instead of a Host-adapter
registry or a composition-global singleton: every participating Host/Kernel
uses the injected service, while tests can still construct an isolated service.
`GraphRuntime` registers a stable, non-reused `GraphInstanceId` and a
graph-lifetime anchor with the registry when the Graph opens. User-visible
session names are not registry keys and cannot create close/reopen ABA.

Run admission and graph close use the following two-stage protocol:

1. `ComputeService` creates a child Run in `Created` without making it visible as
   admitted.
2. Under the registry's single lifecycle fence, `begin_graph_admission` checks
   that the service is `Accepting` and the graph row is `Open`, records a pending
   admission candidate, and acquires a graph-lifetime lease from the registered
   anchor. A closing Graph must wait for that candidate to commit or roll back.
3. While that lease keeps the target alive, the graph-state lane captures the
   authoritative `GraphRevision`; planning may then build the immutable
   descriptor and resource request outside the lane.
4. Trusted service code obtains a complete ledger reservation or none. Under the
   same lifecycle fence, `commit_admission` rechecks service and graph state,
   atomically consumes the candidate, installs one registry-owned `RunLease` in
   both the graph and process indexes, transfers the graph-lifetime lease and
   reservation into the Run, and advances `Created -> Admitted`.
5. Successful registry installation is the Run-admission linearization point.
   Any recheck or resource failure rolls back the reservation, pending candidate,
   and graph lease exactly once; the non-admitted Run publishes the exact
   failure/cancellation to its caller but never appears in an admitted index.

Connected-parameter preflight divides its work across that point. Before
installation it freezes the topology closure, operation/device/callable, DSO
lease, and complete service roots without entering provider code. One umbrella
root charges shared Run control, the prepared result, and anticipated missing
staging entries exactly once across the connected closure, together with its
own private carrier and ledger reservation-state envelope; each node root
charges only its unique callback, ready-store, and service-envelope ownership.
The umbrella is prepared after all node roots and final cancellation-slot
capacity are known. After installation, each prepared phase performs reserved
start before provider entry, and provider output drives the output-dependent
dirty plans inside the installed Run. Preparation rollback, installation
failure, and provider failure release every root exactly once; these later
failures use terminal finalization rather than candidate rollback.

Graph close changes the graph row from `Open` to `Closing` under the same fence.
If registration linearizes first, close finds the Run in its graph index. If
close linearizes first, `commit_admission` rejects the candidate and close waits
for its rollback. Therefore no Run is admitted late and no admitted or in-flight
candidate is missed. Process shutdown uses the same fence to change the service
to `Stopping` and every graph row to `Closing`, so global and graph-local
admission cannot disagree.

Visible commit uses the graph-state lane first and then takes the lifecycle
fence only for its final open-row/registered-Run validation and publication.
Graph close never waits for the graph-state lane while holding the lifecycle
fence: it marks `Closing`, releases the fence, and then drains. Thus commit
publication and the closing transition have one order—commit that validates
first may finish before close, while closing that marks first denies commit—and
the lock order cannot form a registry/lane cycle.

Each registry entry owns only a `RunLease` plus immutable identity/indexing
metadata. It does not own or inspect the plan, dispatcher, staged output,
terminal arbiter, or reservation. The Run retains its graph-lifetime lease and
resource owners. The trusted finalization path may unregister the entry only
after the Run is terminal, every ready/running/completion/dispatcher/commit path
is quiescent, graph commit or discard finalization is complete, and every
reservation/grant and the Run-owned graph-lifetime lease have released exactly
once. Unregistration releases only the registry's `RunLease`; only then may
graph close observe its index and candidate count empty or a caller future
publish settled completion.

The worker retires execution-grant/route accounting and then destroys its
worker-local queue entry, submission, callable, DSO lease, and Run lease
outside locks while `in_flight` still prevents settlement. It decrements
`in_flight` and notifies quiescence only afterward. Finalization itself retains
one persistent synchronized `Active -> Finalizing -> Finalized` authority:
pre-removal failure restores `Active` for trusted retry, concurrent success is
idempotent, and production failure or destruction of an active obligation is
fail-stop.

### Host-authoritative resource accounting

`ExecutionService` exclusively owns an internal `ResourceLedger`, initialized
from composition-root limits. Only trusted host code can mint its move-only,
non-forgeable reservations and execution grants. A built-in or third-party
policy, operation plugin, or policy plugin may request or suggest resources but
cannot construct, duplicate, enlarge, or directly release a token.

The current service and ledger delivered by Issues #70 and #71 account for:

- CPU execution capacity;
- ready-store entries and bytes;
- retained/in-flight Host memory and scratch.

The following dimensions remain later target behavior and are not guessed,
reserved, or represented by fake nonzero values in the current ledger:

- per-device queue, in-flight, memory, and scratch capacity;
- compute-I/O operations and bytes; and
- plugin-process, invocation, IPC/shared-memory, and isolation capacity.

Admission validates one checked resource vector transactionally and returns a
complete Run reservation or nothing. Trusted service code suballocates
ready-entry/byte and CPU/memory/scratch child grants within that reservation.
The service charges only active Throughput root reservations against
the general ceiling after configured interactive headroom is subtracted.
Interactive Runs do not debit this class quota, but the ledger still authorizes
every reservation and grant and remains the sole physical-capacity authority.
Throughput quota check, ledger reservation, and class charge are one serialized transaction. The
non-authoritative class charge is removed only at exact physical root release,
including when live child grants defer that release.

Current success, callback failure, construction rollback, worker failure,
Graph close, and Host destruction release each reservation/grant exactly once.
A Run reservation cannot release while child grants remain live.
Checked overflow or capacity exhaustion never overcommits, partially reserves,
or silently clamps. Synchronous documented allocation exhaustion remains
`std::bad_alloc`; asynchronous failure is captured by the exact Run failure
channel and cannot commit partial output. Issue #73 cancellation and Issue #76
lifecycle finalization preserve the same invariant.

The former worker-only counter is completely removed rather than wrapped,
renamed, aliased, or retained as a second authority. Execution worker-count
resolution is composition-root configuration only; all Run admission uses the
`ExecutionService` ledger.

### Policy generation and private execution routes

`ExecutionService` owns exactly one Interactive and one Throughput policy
binding plus every physical ready entry and fairness row. It charges each start
`work_units + ceil(complete_ready_grant_bytes / 4096)`, charges each Graph's
selected-class accumulator the raw cost, and charges each immutable-class Run
row `ceil(cost / weight)`. It chooses the class, exposes at most one lane head
per live Run, applies age/deadline/projected-service frontiers, and permits at
most three consecutive Interactive starts while Throughput remains startable.
Initial and newly released dependent work enter this same route, and Run rows
persist across temporary emptiness until final retirement. QoS class, deadline,
and weight are explicit descriptor inputs and are not inferred from intent,
quality, or maximum parallelism.

Built-in and DSO policies use the same Host-authored frontier and decision
validation path. A DSO policy receives only an immutable scalar candidate
snapshot through the self-contained C11 policy ABI v1 and may select one
original candidate or abstain. It owns no worker, ready entry, token, grant,
reservation, budget, executor, Run, Graph, completion route, or lifecycle
authority. The Host retries an obsolete valid decision at most twice and then
uses the same-class built-in choice. A first invalid DSO decision is sticky for
the exact binding generation; successful replacement creates a fresh nonzero
generation and clears that fault.

A selected candidate is not execution authority. Reserved start rechecks the
exact ready entry and route, stages grants with no-throw ownership, and commits
entry removal, ready-to-execution grant exchange, fairness/burst accounting,
in-flight state, and callback transfer before any executor callback begins.
The closed `cpu`, `serial_debug`, and `gpu_pipeline` routes keep their
workers, queues, devices, and completion adapters private. The worker-owning
scheduler ABI, SDK target, `IScheduler` hierarchy, and per-Graph physical
owners were removed as one complete breaking migration without an adapter,
forwarding API, or old worker-count grant.

Every planning path freezes the selected operation callable and device before
Run publication. The service rejects a device outside the configured route and
Host inventory before installing active Run state. CPU fallback uses the fixed
pool; Metal uses the single GPU lane. Completion, exception, cancellation,
reuse, shutdown, and drainage retire their shared ledger and Run state exactly
once.

### Revision, staged commit, cancellation, and supersession

Issue #72 makes the minimum revision subset current. `GraphRuntime` advances a
checked nonzero `GraphRevision` for graph mutations relevant to compute
correctness, while every live Graph has a strong non-reused instance identity.
A Run captures both before planning and product work uses request-owned
Graph/proxy snapshots. The graph-state lane is held for capture and validated
visible publication, not for long-running planning/execution; the private
compute-request lane serializes same-Graph requests without owning executor or
policy state.
Issue #73 makes private cancellation and commit-terminal arbitration part of
this current boundary.

The current commit predicate and serialized terminal transaction require:

- `CommitPending`, the expected domain and graph label, and the exact staged
  Graph/proxy owners;
- staged identity/revision equality with the immutable Run descriptor;
- live Graph identity/revision equality with that descriptor; and
- valid staged output for the requested domain; and
- a fresh explicit/deadline cancellation observation followed by acceptance of
  the Run-owned commit contender while the terminal arbiter is still open.

The contender is accepted inside the graph-state work item before eligible
persistence or visible publication. Cancellation or failure accepted first
prevents that claim and publishes no Graph, proxy, or deferred cache state.
Successful publication preserves the authoritative revision and resolves the
contender as Run success before the serialized work item returns; predicate or
persistence failure resolves it as the exact Run failure. A later cancellation
is a no-op. Failed validation discards staged output and cannot mutate visible
Graph/proxy state or write deferred cache artifacts.

Issue #74 extends that predicate with exact supersession key/generation equality
against the live Graph coordinator. Issue #76 adds the current final checks:
the registry Graph row is `Open`, the Run is registered, and its Graph lifetime
lease remains valid.

Revision compatibility is never inferred from equal topology or similar
output. Any future compatible-revision optimization requires a new explicit
decision.

The current #74 supersession path makes a newer generation current and requests
cancellation of older matching Runs. It does not mutate their plans or reuse
their identity. Non-preemptible work and external side effects may finish, but
stale, cancelled, failed, or overdue output cannot commit.

For a paired realtime request, the request-owned `RunGroup` gate is a monotonic
three-state latch: `Pending`, `RtCommitted`, or `Denied`. Validated RT
publication transitions `Pending -> RtCommitted`, while RT failure,
cancellation, or supersession before publication transitions `Pending ->
Denied`; cancellation also requests HP cancellation. `Denied` never reopens
even if late RT work completes. The HP child applies independent revision,
generation, cancellation, terminal, and staged-output predicates, so a later
stale or cancelled HP result does not roll back RT. Graph-close and
process-shutdown denial sources now fan through both children.

### Close and shutdown scopes

Current Graph close (#76):

Before the lifecycle transition, close-owner selection is
generation-qualified. If the selected owner fails before `Open -> Closing`,
the coordinator publishes the exact exception to all callers that already
joined that generation and does not admit a fresh owner until the last old
joiner consumes it. The next caller then owns a fresh, non-reused generation,
so no joiner hangs and no retry can observe an ABA-completed generation.
Destruction retries this ordinary pre-linearization failure.

1. under the lifecycle-registry fence changes the graph row to `Closing`, stops
   new/pending Run admission and ordinary external graph-state admission for
   that Graph, and preserves lifecycle admission only for already-admitted Run
   settlement;
2. waits for pre-fence admission candidates to register-before-close or roll
   back, then enumerates the complete graph-indexed admitted-Run set;
3. denies visible commit and requests cancellation or drain only for those Runs;
4. lets their completion, cancellation, and commit continuations enter the
   graph-state finalization path only to observe closing, discard staged output,
   publish terminal state, and release ownership;
5. waits for terminal outcome, physical quiescence, graph finalization, exact
   resource release, admitted-Run unregistration, and graph-lifetime lease
   release;
6. removes the empty registry row, stops and drains the private per-Graph
   compute-request lane while graph-state finalization remains available, then
   stops and drains the graph-state lane and destroys graph state without
   stopping any process-owned execution route; and
7. leaves `ExecutionService` and unrelated Graph Runs running.

After `Open -> Closing` linearizes, cancellation cannot escape as a recoverable
error: cleanup callback failures are contained after complete fan-out, while a
synchronization/structural failure that could lose the preallocated
cancellation record or settlement obligation fails stop. The same rule applies
after process `Accepting -> Stopping`.

The implementation composes the registry fence with the local two-lane order
without restoring the old single-lane model. Graph close unregisters every Run,
removes the empty row, stops/drains the compute-request lane, then stops/drains
the graph-state lane and destroys Graph state. Graph destruction does not stop
process-owned routes, and there is no post-marker reopen.

Current process execution-domain shutdown (#76):

1. under the same registry fence changes the service to `Stopping`, changes
   every graph row to `Closing`, and stops global admission of new Runs and
   ordinary external graph-state work;
2. waits for all pending candidates to register-before-shutdown or roll back,
   then enumerates the complete process admitted-Run set;
3. chooses cancellation or drain for every admitted Run while keeping bounded
   ready submission, execution, completion routing, and graph-state
   finalization available only for those Runs;
4. settles every Run, unregisters it, and releases its graph/resource leases
   exactly once;
5. stops remaining ready/execution admission after registry emptiness and Run
   quiescence;
6. transfers every physical executor thread into an armed local join guard,
   which joins and accounts for all remaining workers even if later shutdown
   bookkeeping unwinds; and
7. destroys `ExecutionService`.

Worker and operation exceptions are fenced at the execution boundary and routed
through the matching Run lease. They cannot escape a worker thread, fail a
different Run, or skip resource release. A completion arriving after terminal
publication or graph close performs cleanup only.

## Delivery Boundaries

This decision fixes the dependency contract. Issues #66 through #76 are now
implemented as current slices; #76 completes this order. The
non-goal column records the historical boundary when each slice was delivered;
it does not describe current repository state:

| Issue | Consumes this decision | Historical non-goal at delivery |
| --- | --- | --- |
| #66 (current) | HP `ComputeRun` descriptor, state, storage, and one terminal outcome | Process worker migration |
| #67 (completed foundation) | Stable Run leases and `(RunId, RunLocalTaskId)` full-HP completion isolation | Shared CPU service |
| #68 (completed foundation) | Injected CPU-only `ExecutionService` for one Run, ready-only input | Multi-Graph migration and final ledger |
| #69 (completed) | Shared multi-Graph and HP/RT CPU domain; removal of per-Graph built-in CPU workers; owned dirty/preflight submissions | Full admission/policy model and `RunGroup` |
| #70 (current) | Production resource admission, bounded ready store, and `ResourceLedger` | Fairness algorithms, lifecycle registry, and new device/I/O/plugin dimensions |
| #71 (current) | Interactive and throughput built-in policies | Plugin ABI migration, revision preference, cancellation, and supersession |
| #72 (current) | `GraphRevision` capture and staged commit predicate | Cooperative cancellation |
| #73 (current) | Queued/running/commit cancellation, joining #70 and #72 | Latest-wins policy |
| #74 (current) | Latest-wins supersession and realtime `RunGroup` after #71 and #73 | Scheduler ABI replacement |
| #75 (current) | Pure-C policy generation, Host frontier/fallback, reserved start, and private execution routes after #71 | Permanent old/new adapter |
| #76 | Graph close, process shutdown, telemetry, and final invariants after #69/#73/#74/#75 | New execution-domain capabilities |

The dependency graph is acyclic. #72 was permitted after #67 in parallel with
#68–#71; #75 was delivered after #71 alongside the #73–#74 line. Current-state
documentation changes only when each additional implementation and its
long-lived behavioral tests land.

## Consequences

- Request-local state can safely outlive a caller stack without transferring
  task-graph correctness into the execution service.
- Different Runs may reuse local task ids without completion or exception
  cross-talk.
- Graph count no longer determines the built-in CPU worker count, while Graph
  close remains graph-scoped.
- One trusted ledger becomes the final authority for every physical-resource
  dimension; policy and plugin code cannot mint capacity.
- Cancellation may publish terminal state before non-preemptible work is
  reclaimed, so tests and telemetry must distinguish terminal from quiescent.
- Exact revision equality is conservative and may discard reusable work. Any
  relaxation requires an explicit later decision and proof.
- The completed scheduler-to-policy ABI migration is deliberately breaking.
- Device, I/O, general-data, plugin-isolation, and server-control-plane details
  can evolve without changing the ready-task or resource-authority boundary.

## Testability and Failure Invariants

As behavior lands, long-lived tests must cover:

- Run-id and local-task isolation;
- monotonic phases and exact-once terminal races;
- caller-observer destruction and lease-backed lifetime;
- paired RT/HP aggregate outcome priority, asymmetric sibling propagation,
  RT-first commit-gate races, late group cancellation after two child
  successes, and caller-future settlement;
- ready-only submission and dispatcher-owned dependent release;
- graph-close/admission and process-shutdown/admission linearization, including
  pending-candidate rollback and admitted-Run registry unregistration;
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

### Reuse the former worker-only counter as ResourceLedger

Rejected because that removed transitional counter had the wrong resource
model, hidden process-static ownership, and no Run, queue, memory, scratch,
device, I/O, or plugin-process authority.

### Let policy callbacks mint grants

Rejected because an untrusted or defective strategy could overcommit resources
and evade exact-release accounting.

### Preserve the worker-owning scheduler ABI behind an adapter

Rejected because physical workers and tokens would remain outside the sole
host-authoritative execution domain.

### Let GraphRuntime or each Host adapter own the admitted-Run registry

Rejected because Graph ownership would multiply process lifecycle state and
could stop unrelated work, while per-adapter Host registries could not provide
one process admission/shutdown fence across all injected owners. The registry is
therefore one private `ExecutionService` admission subcomponent, not a public or
static composition object.

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
- Current behavior, including issue #69's fixed multi-Graph HP/RT CPU pool,
  issue #70's admission/ledger boundary, issue #71's policy-aware ready store,
  issue #72's strong Graph revision and staged publication boundary, issue
  #73's private cooperative cancellation and Run-owned commit arbitration,
  issue #74's latest-wins generations, bounded ticket-backed coalescing,
  current-generation commit predicate and realtime `RunGroup`, and issue #75's
  pure-C policy ABI, Host-authored frontier/fallback, reserved start, and
  private execution routes, and issue #76's lifecycle registry, exact Graph
  close/process shutdown, and source-private telemetry, remains
  authoritative in
  [Compute Boundaries](../kernel-architecture/Compute-Boundaries.md),
  [Compute Flow](../kernel-architecture/Compute-Flow.md), and
  [Policy and Execution Architecture](../kernel-architecture/Policy-and-Execution-Architecture.md);
  Issue #76 lifecycle/telemetry is current implementation behavior; independent
  delivery review and remote integration remain release gates rather than
  runtime ownership semantics.
