# Policy and Execution Architecture

This document is the authoritative current description of how Photospider
chooses ready work and how it executes that work. Policy and execution are
separate ownership domains:

- a **policy** ranks immutable Host-admissible candidates and owns no resource;
- an **execution route** is a private Host implementation that owns physical
  queues, workers, devices, and completion adapters;
- a **Run** owns request identity, cancellation/supersession state, dependency
  progress, staged output, and its terminal result;
- the **Host** alone validates policy output, reserves resources, commits a
  start, and enters an executor.

The former worker-owning scheduler SDK, `IScheduler` hierarchy, per-Graph
physical owners, and scheduler plugin ABI are absent. There is no compatibility
adapter or forwarding API.

## Ownership Model

`ExecutionService` is the process execution-domain owner used by an embedded
Host. It owns:

- the bounded ready store and its complete ready-byte charges;
- one Interactive and one Throughput policy binding;
- process fairness and three-to-one class arbitration state;
- the fixed CPU worker pool, one service-owned Metal worker lane, and private
  `serial_debug` and `gpu_pipeline` routes;
- Host-authored candidate, Graph, Run, entry-version, enqueue, snapshot, and
  selection identities;
- ready-to-execution resource exchange and in-flight callback ownership.

`GraphRuntime` stores only copied HP and RT route ids with nonzero generations.
It owns Graph state, compute/event/trace observation, and request serialization,
but it does not own a physical worker pool or policy-plugin context.

`ComputeRun` remains stable behind Run leases until every callback, dependent
release, completion publication, and staged commit contender settles. Policy
callbacks never receive a Run pointer or lease.

The resource lock order is:

```text
ExecutionService ready-store/service state
  -> Run state
    -> ResourceLedger reservation state
```

No policy callback executes while any of those locks, a Graph lock, or a policy
registry/binding lock is held.

## Policy Classes and Bindings

There are exactly two service classes:

| Class | Intended work | Built-in type |
| --- | --- | --- |
| Interactive | latency-sensitive work, optionally with a monotonic deadline | `interactive` |
| Throughput | weighted background work | `throughput` |

The process owns one binding per class. Even when both classes use the same DSO
type, they receive separate contexts and separate nonzero binding generations.
A same-name replacement still creates a new generation, clears the old
generation's fault, drains its active invocations, destroys its context once,
and retires its DSO lease only after the last dependent value is gone.

`configure_policy_defaults` prepares both candidate bindings outside the
publication lock and commits both or neither. `replace_policy` applies the same
prepare/publish/drain discipline to one class. A failed create, validation, or
publication leaves the previous binding and generation unchanged.

## Pure C Policy ABI v1

The only installed policy header is
`include/photospider/policy/policy_plugin_api.h`. It is self-contained under
C11 and C++17 and defines a natural-layout 64-bit ABI with exactly two exports:

```c
uint32_t ps_policy_plugin_get_abi_version(void);
ps_policy_status_v1 ps_policy_plugin_get_api_v1(
    ps_policy_plugin_api_v1 *out_api);
```

The API table contains four mandatory callbacks: metadata, create, select, and
destroy. The exact record sizes are:

| Record | Bytes |
| --- | ---: |
| `ps_policy_string_view_v1` | 16 |
| `ps_policy_type_metadata_v1` | 80 |
| `ps_policy_create_args_v1` | 40 |
| `ps_policy_candidate_v1` | 120 |
| `ps_policy_selection_snapshot_v1` | 64 |
| `ps_policy_decision_v1` | 48 |
| `ps_policy_plugin_api_v1` | 80 |

ABI v1 accepts exact sizes, kinds, alignments, offsets, callback pointers, enum
values, bounds, and zero-required reserved words. It has no tail-extension
rule. A new record shape requires a new ABI generation.

A policy receives only scalar candidate descriptors: opaque ids, deadline,
weight, trusted work and byte charges, projected Graph/Run service scores,
dispatch age, enqueue sequence, and flags. It never receives an executor,
worker, device, queue, allocation service, resource grant, Run, Graph,
completion route, logger, or lifecycle callback. Borrowed snapshot memory is
valid only until `select` returns.

The Host opens a DSO eagerly and locally, resolves and calls only the version
export before exact ABI equality, then validates the complete API and every
metadata row. One DSO is published as an all-or-nothing type-registration
transaction. Internal duplicates, conflicts, invalid UTF-8, noncanonical names,
reserved built-in names, invalid class masks, or malformed callback output
publish no row.

Active metadata, bindings, contexts, and invocations retain independent DSO
leases. Registry unload removes visibility but cannot invalidate an active
binding. An honest in-process callback that never returns has no timeout or
forced recovery guarantee; process isolation is a separate future boundary.

## Host-Authored Frontier

The Host chooses the service class before invoking a policy. When both classes
have startable work, it permits at most three consecutive Interactive starts
before one Throughput start. Within the chosen class it exposes at most one lane
head per live Run.

Before a plugin sees candidates, the Host reduces them through these rules:

1. only current, startable, cancellation-safe, route-compatible lane heads are
   considered;
2. after eight same-class starts, only the maximum-age frontier remains;
3. otherwise Interactive work with the earliest finite deadline remains;
4. candidates outside the minimum projected Graph-service quantum are removed;
5. candidates outside the minimum projected Run-service quantum are removed;
6. score saturation escapes through the oldest stable enqueue sequence;
7. stable enqueue order is the final built-in tie-break.

The built-in policies use this same frontier and validation path as DSO
policies. The plugin may select one candidate from the immutable original
snapshot or abstain; it cannot widen the frontier or mint work.

## Decision Classification and Fallback

The Host first validates callback completion and every decision byte against
the original call: status, size, kind, reserved fields, decision kind,
generation echoes, and candidate identity. Only a valid original-snapshot
selection is then compared with current Host state.

There are two distinct outcomes:

- **obsolete by Host state**: the decision was valid when made, but readiness,
  cancellation, supersession, route, fairness, or generation state
  changed. The Host may take at most two fresh plugin snapshots, then uses the
  current same-class built-in choice. This records no policy fault.
- **invalid plugin decision**: the callback failed, threw a catchable foreign
  exception, abstained, returned malformed bytes, echoed the wrong generation,
  or named a candidate outside the original snapshot. The first fault is sticky
  for that binding generation, future calls bypass it, and successful
  replacement clears it.

Fault categories are `Abstained`, `CallbackStatus`, `CallbackException`,
`MalformedDecision`, `GenerationMismatch`, and `CandidateOutsideSnapshot`.
Optional Host snapshot allocation/bound failure is non-faulting and uses the
untruncated built-in path. A trusted built-in invariant violation fails only the
affected Run as `GraphErrc::ComputeError`.

## Reserved Start

A returned candidate id is not execution authority. The Host keeps a private
`SelectionPin` containing the original entry identity/version and rechecks
current state under the documented lock order. `StartTransaction` stages the
CPU, retained-memory, and scratch grants with no-throw RAII.

The final commit is allocation-free and non-throwing. It atomically:

- removes the exact ready entry;
- exchanges its ready grant for execution grants;
- advances class, Graph, and Run service accounting;
- updates the Interactive burst count and in-flight state;
- transfers callback ownership to the selected private route.

No executor callback begins before that commit. Every rejection or exception
before commit preserves ready/fairness/burst/in-flight state and releases staged
grants exactly once. Completion, cancellation, supersession, dependency release,
and Run settlement also release their grants exactly once.

Temporary execution-grant exhaustion after revalidation is not a plugin fault
or obsolete-decision retry. The ready store marks the exact candidate/version
only for that worker's current cycle and recomputes class/frontier selection
without removing the entry, releasing its ready grant, or charging fairness.
This lets an independent lower-priority Run start from the remaining current
candidates. If every lane-compatible candidate is marked, the worker waits on
a predicate-protected notification epoch advanced by enqueue, dependency
release, completion/grant release, cancellation/failure purge, policy
replacement, and shutdown. Spurious wakes do not retry; a 50 ms low-frequency
fallback covers an otherwise unobservable external child-grant release, after
which cycle marks are cleared and current Host state is revalidated.

## Private Execution Routes

The route vocabulary is closed:

| Route | Ownership and behavior |
| --- | --- |
| `cpu` | Host-lifetime fixed CPU worker pool with reusable multi-entry execution; exposes CPU only |
| `serial_debug` | CPU worker zero with one callback in flight; exposes CPU only |
| `gpu_pipeline` | the same fixed CPU pool for CPU fallback plus one service-owned Metal lane; exposes Metal then CPU when the Host reports Metal, otherwise CPU only |

`heterogeneous` is not an alias. Execution routes are not plugins and cannot be
scanned or loaded.

`HostExecutionConfig` controls future-session HP/RT route ids and a worker
request in `[0,8]`. Zero selects bounded automatic resolution. Once the process
CPU pool is fixed, zero or an equal request preserves it; a different positive
request is rejected. Existing Graph sessions keep their route bindings.

`replace_execution` validates a closed-vocabulary route, prepares the new
ownerless binding, serializes against active same-session requests, and
publishes a new nonzero generation. A same-name replacement also advances the
generation. Failure preserves the old route.

Operation selection freezes both the implementation callable and its `Device`
before Run admission. Full HP, dirty HP/RT, and connected-parameter preflight
all consume the same route-aware inventory. Every ready submission carries the
frozen device, and `ExecutionService` rejects a device outside the configured
route/Host inventory before publishing the Run. CPU submissions enter the
fixed CPU pool; Metal submissions enter the single GPU lane. Both lanes share
the common ready store, policy decision, reserved-start transaction, Host
ledger, Run maximum-parallelism grant, cancellation, completion, exception,
reuse, shutdown, and drainage rules; no second device-capacity authority or
per-Graph executor is created.

Full HP, dirty HP/RT, connected preflight, initial ready work, and
dependency-released work all enter the common ready-store, policy,
reserved-start, private-route, and Run-lease completion path.

## Host, CLI, and IPC Surfaces

The public Host has eight policy operations and six execution operations. Its
final non-destructor virtual inventory is 58. Policy discovery and bindings are
process-scoped; execution info/replacement and execution trace are session-
scoped copied values.

`graph_cli` exposes:

```text
policy list|get|set|scan|load|plugins|help
execution list|get|set|help
```

Configuration uses `policy_dirs`, `policy_interactive_type`,
`policy_throughput_type`, `execution_hp_type`, `execution_rt_type`, and
`execution_worker_count`. Removed `scheduler` commands and `scheduler_*` keys
are rejected without translation.

IPC protocol version 2 replaces the old method family with eight `policy.*`
and six `execution.*` methods, including non-destructive `execution.trace`.
The daemon advertises exactly 60 sorted unique methods. Protocol version 1 and
old method names are rejected before Host access. The exact schemas and bounds
are maintained in
[`IPC-Protocol-v2.md`](../codebase-structure/IPC-Protocol-v2.md).

## Observability and Lifecycle Proof

Execution trace pages contain copied sequence, epoch, node, worker, action, and
timestamp values. Pages are non-destructive, bounded to 4,096 entries, and
preserve drop/exhaustion semantics. Trace data carries no queue or callback
capability.

`ExecutionService` also owns source-private
`ExecutionLifecycleTelemetry`: a schema-versioned fixed ring of 65,536 records
with non-destructive 1..4,096-record snapshot pages, atomic cuts, explicit
cursor gaps, and saturating cumulative drop accounting. Its 15 post-transition
counters combine registry state with exact ready entry, entered operation
callback, live root reservation, live child grant, policy invocation, and
current/displaced policy-binding ownership. Records contain copied scalar
identities only and expose no label, path, pointer, callback, lease, or mutable
handle. This store is not added to Host, CLI, or protocol v2.

`RunLifecycleRegistry` now drives Graph-close and process-shutdown
cancellation. Shutdown keeps already admitted ready/execution/completion paths
alive until every Run settles, then joins physical workers and retires policy
bindings before publishing `ServiceStopped` with all 15 counters zero. A
nonreturning callback can therefore keep shutdown honestly blocked; it is not
made recoverable. General-data heterogeneous execution belongs to Issue #77;
process-isolated plugin supervision belongs to Issue #91.

## Implementation and Validation Entry Points

- `include/photospider/policy/policy_plugin_api.h`
- `src/lib/policy/policy_registry.hpp` and `.cpp`
- `src/lib/compute/execution_service.hpp` and `.cpp`
- `src/lib/compute/run_lifecycle_registry.hpp` and `.cpp`
- `src/lib/compute/execution_lifecycle_telemetry.hpp` and `.cpp`
- `src/lib/execution/execution_task_runtime.hpp`
- `src/lib/runtime/graph_runtime.hpp` and `.mm`
- `src/lib/runtime/kernel_execution_facade.cpp`
- `include/photospider/host/host.hpp`
- `src/lib/host/embedded_host.cpp`
- `src/lib/ipc/{codec,client,host,request_router}.cpp`
- `tests/unit/test_policy_registry.cpp`
- `tests/unit/test_compute_run.cpp`
- `tests/integration/test_compute_service_split.cpp`
- `tests/integration/test_ipc_daemon.cpp`
- `tests/integration/static_product_consumer_smoke.py`

See also [Compute Flow](Compute-Flow.md),
[Compute Boundaries](Compute-Boundaries.md), [Plugin ABI](Plugin-ABI.md), and
[Graph Lifecycle](Graph-Lifecycle.md).
