# Single-Tenant Job Vertical

This document defines the current source-private Issue #98 Job behavior. It is
the authority for what exists under `src/lib/server/`; ADR 0011 explains the
long-term security-domain decision, and the server roadmap describes later
delivery slices. The current module is an executable single-process vertical,
not a network or multi-tenant server.

## Terms and Current Profile

The current profile is `jobspec-v1` plus `embedded-cpu-v1` execution and
`process-lifetime` artifact durability. It supports one configured `TenantId`,
any number of process-lifetime Jobs, one attempt per Job, one fresh in-process
worker object and Embedded Host per attempt, one target node, and one required
CPU image output slot.

These identity domains are distinct strong C++ value types:

| Identity | Current meaning | Explicit non-meaning |
| --- | --- | --- |
| `TenantId` | The sole tenant configured on one `SingleTenantJobService` | Authenticated principal or multi-tenant authorization |
| `JobId` | One accepted immutable Job during the process lifetime | Local IPC compute request or Graph session |
| `JobSpecDigest` | SHA-256 of exact canonical `jobspec-v1` bytes | Job authority by itself |
| `JobAttemptId` | The sole current attempt in this slice | Retry generation; retry is absent |
| `WorkerInstanceId` + `WorkerLeaseGeneration` | Exact fresh worker object and its one assignment | OS process identity, PID, or supervisor lease |
| `GraphArtifactId` | Immutable graph material key interpreted by a trusted resolver | Host path or local Graph session |
| `OutputSlotId` | Required image output declaration | Runtime node/output pointer |
| `ArtifactId` | One immutable artifact record in the process-lifetime store | Content digest, path, `OutputArtifactId`, or buffer handle |
| `OutputCommitId` | One exact artifact commit event | Idempotency key or durable transaction id |
| `ArtifactContentDigest` | SHA-256 of exact tight payload bytes | Artifact or commit identity |

All generated Job, attempt, worker, artifact, and commit ids are non-reused
within the current process. They are not persisted, globally allocated, or
recoverable after restart.

## Immutable JobSpec

`JobSpec` is a validated class with getters and no mutation surface. Its
constructor accepts only:

- one bounded opaque `GraphArtifactId`;
- one nonnegative target node integer;
- one bounded opaque required `OutputSlotId`;
- one positive maximum-parallelism bound;
- the closed `embedded-cpu-v1` profile; and
- requested `process-lifetime` durability.

Canonical bytes begin with `jobspec-v1` and encode all six fields using decimal
length frames. The target-node and maximum-parallelism integers first use their
canonical decimal text and are then framed exactly like the other four fields.
The constructor records SHA-256 of those exact bytes. The control plane
revalidates the value before acceptance, retains it through
`shared_ptr<const JobSpec>`, and records the digest in both Job and assignment
state. The worker repeats field/canonical/digest validation before graph
resolution.

There is no JobSpec field for a Host path, `GraphLoadRequest`, local
`GraphSessionId`, file descriptor, pointer, native/runtime handle, mutable store
location, bearer credential, local IPC id, plugin DSO, checkpoint, retry, quota,
or retention policy. A trusted `GraphArtifactResolver` outside JobSpec maps the
graph identity to local `GraphLoadRequest` path fields for the current adapter.
Those paths never enter canonical Job bytes, attempt reports, or artifact
receipts.

## Observable Job Behavior

`SingleTenantJobService` is the only owner of accepted Job truth. `submit()`
validates and freezes JobSpec, mints `JobId`, `JobAttemptId`,
`WorkerInstanceId`, and lease generation one, and constructs the complete
`JobSubmission` before acceptance. It then records the complete assignment and
starts one joinable worker thread. If thread start fails, state insertion is
rolled back. Once the thread starts, returning the already-built submission is
either copy elision or a compile-time-asserted non-throwing move; receipt-string
allocation cannot make the caller observe failure after the Job was accepted.
`query()` returns a copied snapshot; `wait_for()` bounds only observer waiting
and does not impose an execution deadline.

The current state machine is:

```text
Queued -> Running ---------------------> Succeeded
   |         |                              ^
   +---------+-> Cancelling -> Cancelled    |
             |                              |
             +---------------------------> Failed
```

`Succeeded`, `Failed`, and `Cancelled` are terminal. There is no retry or
attempt replacement. A worker returns only an immutable `JobAttemptReport`:
the complete assignment tuple, worker-local outcome, settlement fact, typed
failure, diagnostic, and optional candidate `ImageBuffer`. It cannot mutate a
Job snapshot or commit an artifact.

The worker report vocabulary is closed:

- `Succeeded` requires `settled=true`, `failure=None`, and exactly one
  candidate image;
- `Cancelled` requires `settled=true`,
  `failure=CancellationObserved`, and no image; and
- `Failed` requires no image and one of `InvalidAssignment`,
  `GraphResolution`, `HostSetup`, `GraphLoad`, `Compute`, `Settlement`, or
  `Unexpected`. Its `settled` value remains the worker's exact cleanup fact.

`ReportRejected` and `ArtifactCommit` are control-plane failures and are never
accepted from a worker. `None` belongs only to success, and
`CancellationObserved` belongs only to cancellation. Invalid underlying enum
representations are not extensions to this vocabulary.

The control plane finds the Job through the assignment retained by the exact
worker thread, then validates the report's full tenant/Job/spec-digest/attempt/
worker/lease tuple. It then validates the complete enum and outcome/settlement/
failure/image shape before copying any report outcome, settlement, failure, or
diagnostic and before cancellation adjudication. A mismatched, empty, stale,
malformed, context-invalid cancellation, or invalid-enum report uniformly
publishes `Failed` + `ReportRejected`, `attempt_settled=false`, and no receipt.
Cancellation intent cannot convert such a report into `Cancelled`. Equality in
one identity domain or equal content cannot repair another mismatch. Because
the rejected report is not trusted, none of its fields can establish retained
current-attempt truth.

Job success requires all of the following under the current control mutex:

1. the report matches the exact current assignment;
2. the attempt reports `Succeeded` and `settled=true`;
3. the report carries one valid nonempty CPU image and no failure fact;
4. the separate artifact authority commits that image for the declared slot;
5. the returned receipt matches the complete assignment, slot, and requested
   process-lifetime durability.

Host/Run success alone therefore does not imply Job success. Artifact commit,
Job terminal publication, cancellation intent, and caller observation remain
separate facts.

## Cancellation and Completion Ordering

`cancel()` records one monotonic control-plane intent. An absent Job, terminal
Job, or repeated request returns false. An accepted active request changes the
observable state to `Cancelling`; it does not detach the worker or claim an
execution deadline.

The public Host currently exposes no active compute-cancellation operation.
The Embedded Host worker observes cancellation before graph resolution, before
Host construction/load/compute, and after compute. Once Host compute has
entered a provider, cancellation may wait indefinitely for that call to return.
The worker then closes the loaded graph and destroys its Host before reporting.

Cancellation and artifact commit linearize under the Job mutex:

- if cancellation wins first, a later image is discarded, no artifact is
  committed, and `Cancelled` appears only after `settled=true`;
- if successful commit and terminal publication win first, later cancel returns
  false and cannot rewrite the receipt or `Succeeded` state.

Service destruction marks active Jobs cancelling and joins every worker. It is
orderly ownership cleanup, not bounded forced termination. WorkerManager,
heartbeat, OS-process kill/reap, crash/hang/OOM containment, and retry belong to
Issue #100 and later work.

## Embedded Host Worker Path

`EmbeddedHostJobWorker` performs these stages for one assignment:

1. validate the full assignment, immutable JobSpec, and exact digest;
2. observe cancellation;
3. ask the trusted resolver for graph material outside JobSpec;
4. create a fresh Embedded Host and seed repository built-in operations;
5. load an attempt-local Graph session whose name never becomes server
   authority;
6. compute the declared node with `fp32`, force-recache, disk-cache disabled,
   no cache save, quiet output, and the JobSpec maximum-parallelism bound;
7. validate a nonempty CPU `ImageBuffer` candidate;
8. observe cancellation again, close the exact Graph, destroy Host ownership,
   and only then report settlement.

Resolution, Host setup, load, compute, output validation, and settlement have
separate `JobAttemptFailure` values. Graph resolution failure constructs no
Host. A graph close failure reports `settled=false` and cannot succeed. The
worker never receives artifact-store mutation authority. A null factory result
or a standard/non-standard exception that escapes worker creation or execution
gives the control plane no settlement proof; it publishes a failed current
attempt with `settled=false` and no receipt. Even accepted cancellation cannot
turn that unsettled failure into `Cancelled`.

## Process-Lifetime Artifact Authority

`ProcessLifetimeArtifactStore` is a separate object from Job state, the local
IPC `OutputStore`, and benchmark `B1OutputStore`. Commit validates the complete
assignment and output slot, calls the public `ImageBuffer` validator, requires a
nonempty CPU payload, copies active bytes row by row into tight immutable
storage, and hashes the copied payload. Source padding is omitted, and later
source mutation or release cannot change the record.

Every commit mints a new `ArtifactId` and `OutputCommitId`, including when
content is byte-identical. `OutputCommitReceipt` binds:

- TenantId, JobId, JobSpecDigest, JobAttemptId;
- WorkerInstanceId and WorkerLeaseGeneration;
- OutputSlotId, ArtifactId, and OutputCommitId;
- width, height, channels, scalar type, tight row bytes, and payload bytes;
- ArtifactContentDigest; and
- achieved `process-lifetime` durability.

Lookup by ArtifactId returns `shared_ptr<const ArtifactRecord>` containing the
same receipt and tight payload. There is no mutable record API or exposed path,
runtime handle, IPC delivery id, or store root.

This store does not implement filesystem publication, idempotency, quotas,
retention, TTL delivery, restart persistence, recovery, atomic-visible or
crash-durable commit. Those properties and stable tenant-scoped identity
allocation belong to Issue #99.

## Boundaries and Limitations

- The module is source-private and built as
  `photospider_single_tenant_job_internal`; it is not installed or exported.
- `photospiderd` and protocol v2 are unchanged. Their session, compute request,
  `OutputArtifactId`, and delivery ids remain local process/transport values.
- The configured single TenantId is not authentication or authorization.
- Worker threads and Embedded Hosts share the caller process; there is no
  security, address-space, syscall, plugin, or crash isolation.
- Only trusted repository CPU operations are in scope. Tenant plugin ABI and
  isolation remain absent.
- One Job has one attempt and one required image slot. There is no retry,
  checkpoint, resource quota, durable retention, or network API.
- A positive JobSpec maximum parallelism bounds the Host Run but does not resize
  process execution workers or create server quota.

The target multi-process security model remains governed by
[ADR 0011](../adr/0011-server-control-plane-workers-and-plugin-runtimes-are-separate-security-domains.md)
and the [server roadmap](../roadmap/Kernel-Evolution.md#server-and-plugin-isolation).

## Source and Long-Lived Test Entry Points

- Contracts and canonical digest: `src/lib/server/job_contract.hpp` and
  `src/lib/server/job_contract.cpp`.
- Job truth and artifact authority:
  `src/lib/server/single_tenant_job_service.hpp` and
  `src/lib/server/single_tenant_job_service.cpp`.
- Real worker adapter: `src/lib/server/embedded_job_worker.hpp` and
  `src/lib/server/embedded_job_worker.cpp`.
- Focused authority/lifecycle tests:
  `tests/unit/test_single_tenant_job_service.cpp`.
- Real Embedded Host vertical:
  `tests/integration/test_single_tenant_job_product_path.cpp`.

The focused tests cover exact six-field canonical bytes and SHA-256, rejection
of path-shaped identity, tight-row deep copy, equal-content identity
separation, receipt-gated success, missing output, mismatched lease fencing,
closed malformed report shapes and invalid enums, all worker-owned typed
failure/settlement combinations, null/exceptional factory and worker
settlement, cancellation after malformed reporting, and cancel-before-commit
ordering. The compiled contract independently static-asserts the no-throw
submission move. Gate cleanup guards release blocked workers before service
destruction even when a fatal test assertion exits early. The product-path test
resolves an immutable graph identity to a tiny YAML graph, executes it through
a fresh Embedded Host, closes it, commits the result, and queries the
identity-complete artifact.
