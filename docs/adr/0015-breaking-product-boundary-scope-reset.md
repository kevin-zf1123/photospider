# ADR 0015: Reset the Product Boundary to an Embedded Kernel and Local Daemon

- Status: Accepted
- Date: 2026-09-01
- Decision type: Breaking 0.x product-boundary reset
- Archive tag: `pre-breaking-scope-reset-2026-09-01`
- Archived kernel commit: `446e83687ecf49be8e9b66beac8b40c7b8b224de`
- Companion daemon decision: `photospider-daemon/docs/adr/0001`

## Context

Photospider accumulated several distinct product concepts in one kernel:
embedded graph execution, local daemon routing, durable service state,
process-worker supervision, policy plugins, plugin admission, artifact
authority, and release-evidence profiles. Those concepts made ownership
ambiguous and caused local correctness mechanisms to be described as service
or security products.

This decision is a deliberate 0.x breaking cut. The pre-reset source remains
available only through Git history and the annotated archive tag above. The
active tree does not retain disabled targets, compatibility adapters, throwing
stubs, forwarding headers, or archived source copies.

## Decision

### Ownership matrix

| Capability | Photospider kernel | Photospider daemon |
| --- | --- | --- |
| Workflow source model and validation | Owns `WorkflowDocument` and graph validation | Accepts a workflow only through the installed public kernel API |
| Compiler pipeline | Owns semantic IR, optimized IR, operation traits, optimization, and physical planning | Does not copy or serialize internal IR |
| Local execution | Owns CPU-required/GPU-optional execution, transfers, residency, local resource accounting, cancellation, fallback, and result publication | Submits work through the public compile/execute facade |
| Runtime data | Owns `Value`, `Region`, layout, memory, and non-durable results | Retains an ephemeral result only while its daemon Job is live |
| Graph ownership | Owns independent `GraphContext`/`ExecutionContext` objects | Owns opaque `SessionId` logical namespaces |
| Work orchestration | Does not define Job identity, queue, status, registry, or retry | Owns ephemeral `JobId`, queue, status, cancellation, result, and release |
| IPC | None | Owns local IPC v3 and the `photospiderd` lifecycle |
| Operations and providers | Owns semantic traits, operation/provider ABI and registry, and trusted in-process DSO loading | Never accepts plugin paths over IPC |
| Benchmarking | Owns raw timings, backend/transfer/resource diagnostics, plan digest, and correctness oracles | May report ordinary daemon lifecycle timing without evidence authority |

The dependency direction is one way:

```text
photospider-daemon
  -> isolated installed Photospider package
  -> public compile/execute/value contracts
```

The kernel never depends on the daemon. The daemon never includes private
kernel headers, links a source-tree target, copies compiler/planner code, or
puts internal IR on the wire.

### Kernel boundary

The kernel is session-agnostic, single-machine, and embeddable. It supports
multiple independent `GraphContext` and `ExecutionContext` instances in one
process, including concurrent instances. A graph context is a kernel object or
handle, not a daemon Session and not a registry entry.

The retained pipeline is:

```text
WorkflowDocument
  -> SemanticGraphIR
  -> OptimizedGraphIR
  -> ExecutionPlan
  -> local ExecutionContext
  -> ExecutionResult
```

Document identity, semantic identity, optimized identity, physical plan
identity, runtime allocation identity, and daemon identity remain distinct.
`SemanticGraphDigest`, `OptimizedGraphDigest`, `ExecutionPlanDigest`, and
`PlanCacheKey` are non-security compiler/cache identities. Derived caches are
disposable and rebuildable.

### Daemon boundary

The daemon is a same-user, local, non-persistent orchestration layer. Its
Sessions are logical namespaces in one process and one trust domain; they are
not tenants or isolation boundaries. Restart clears all Sessions, Jobs, and
results.

The Job state machine is exactly:

```text
Queued -> Running -> Succeeded | Failed | Cancelled
```

There is no attempt identity, automatic retry, checkpoint, recovery journal,
durable Job specification, durable artifact identity, output commit, receipt,
or per-tenant quota. A caller retry is a new submit. Session close cancels
unfinished Jobs and releases temporary results. Job cancellation maps to the
kernel's cooperative, best-effort cancellation and stale publication is
rejected.

Local IPC v3 exposes only:

1. `session.create`
2. `session.close`
3. `job.submit`
4. `job.status`
5. `job.cancel`
6. `job.result`
7. `job.release`
8. `daemon.info`
9. `daemon.shutdown`

The transport is a Unix-domain socket on supported POSIX systems, with a
local named-pipe abstraction permitted on supported Windows builds. There is
no TCP, HTTP, gRPC, TLS, remote endpoint, v2 adapter, or dual protocol.

### Operation and provider boundary

The operation ABI/SDK/registry and the data-definition/provider ABI remain.
Operation semantic traits are compiler inputs. Operation/provider DSOs run
in-process in the same trust domain as the host. The operation set is
configured at process startup and is read-only after execution begins.

ABI version, structure size, alignment, pointer/count pairs, array bounds,
overflow, type/shape/`Region`/layout/facet validation, exception fencing, and
exact cleanup are correctness contracts. They are not a sandbox or a plugin
security product.

The breaking operation ABI is version two. Every operation publishes a closed
typed parameter schema with required flags; compilation rejects unknown,
missing, wrong-type, duplicate/conflicting, or otherwise invalid parameters
before semantic IR exists. Operation callbacks receive the validated values
and plan-derived input Regions. Whole, Elementwise, and overflow-safe clipped
Halo rules therefore change physical input demand and plan identity rather
than serving as digest-only metadata.

### Retained correctness validation

The reset removes security-product claims, not defensive correctness. The
following remain required:

- ABI version/size/alignment and pointer/count/array validation;
- integer and allocation overflow validation;
- graph, typed IR, type, shape, `Region`, layout, facet, and plan validation;
- malformed local-IPC frame and result-shape validation;
- stale handle, stale completion, and post-cancellation publication rejection;
- exception fencing and exact resource cleanup;
- CPU fallback when an optional GPU path is unavailable or rejects work;
- one ExecutionContext-wide waiting-callback bound shared by deterministic CPU
  and optional GPU FIFOs, with running callbacks excluded from that count;
- ordinary negative, concurrency, ASAN, TSAN, and fuzz coverage where the
  platform and toolchain support them.

### Removed product domains

The active products do not contain or advertise:

- execution-profile SLO identities or release evidence;
- verdict/envelope/attestation/receipt authority;
- network service, authentication, authorization, Principal, Tenant, role,
  capability, multi-tenant quota, or control plane;
- durable Jobs, attempts, checkpoints, recovery, journals, backup/restore,
  deployment, rollback, or operations-readiness authority;
- fresh worker processes, heartbeat/lease supervision, TERM/KILL/reap
  ownership, or a remote/distributed worker model;
- plugin process isolation, sandboxing, cryptographic trust, signatures,
  certificates, package admission, or trust bundles;
- policy DSO, policy public ABI/SDK/loader/registry/fixtures;
- durable artifact authority, durable Value identity, output commits,
  receipts, or manifest-last publication.

Ordinary bounded local concurrency and backpressure remain. `ExecutionRun`,
the per-lane deterministic FIFOs, their single shared waiting admission, CPU
workers, an optional local GPU lane, `ResourceLedger`, transfer/residency
tracking, deterministic scheduling, cancellation, fallback, and stale-
completion rejection remain kernel mechanisms and must not be renamed into
daemon or service authority.

### Benchmarks

Maintained benchmarks may report raw compile/plan/execute/operation timings,
selected backend, transfer count/bytes, peak live bytes, fallback/error
reason, plan digest, result digest, and a correctness oracle. A supplied oracle
has a required bounded canonical `oracle_name` recorded on every sample and
report; a run without an oracle is explicitly `unchecked`. They do not emit
execution-profile identities, applicability/startability verdicts, evidence
envelopes, attestations, release evidence, or durable artifact references.

## Exact non-goals

- Remote or multi-user service operation.
- Authentication, authorization, tenant isolation, or a security control
  plane.
- Durable queueing, retry, recovery, checkpointing, or artifact storage.
- Process isolation or sandboxing for operation/provider DSOs.
- Stable serialization of internal semantic/optimized/physical IR.
- Compatibility with IPC v2, the frozen four-cell gate, removed package
  components, or removed public headers.
- Distributed execution or remote devices.

These are removed or out of scope. They are not deferred, optional,
default-disabled, or scheduled for a later roadmap.

## Superseded decisions and authorities

This ADR is the highest active product-boundary authority. It retires ADRs
0001, 0004, 0009, 0010, 0011, and 0013 from the active ADR set because their
exact graph-state scheduler, OpenCV operation, service/evidence, or dense-image
facet decisions are absent from the reset implementation. It supersedes the
product-scope portions of ADRs 0002, 0003, 0005, 0007, 0008, 0012, and 0014;
their retained local-kernel contracts are narrowed in place. It also supersedes every active roadmap,
OpenSpec, architecture page, issue, or Project description that assigns Job,
worker-process, policy, trust, isolation, durable artifact, evidence, or
network-service authority to the kernel, or assigns IPC v2 compatible
maintenance to the daemon.

Historical archives remain historical evidence only. An archive must not be
linked from an active index as current authority and must not be used to
restore a removed domain without a new explicit breaking product decision.

## Consequences

- Source and package compatibility break immediately in the 0.x line.
- Kernel and daemon releases must be built and tested from isolated installed
  packages.
- Removed implementation remains recoverable only from Git history and the
  annotated archive tag.
- Documentation, Issues, Projects, tests, CI inventories, and package exports
  must describe the same boundary.
- Any future proposal that reintroduces a removed domain first requires a new
  product-boundary ADR that explicitly supersedes this decision.
