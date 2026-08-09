# Single-Tenant Job Vertical

This document defines the current source-private Issue #99 Job behavior under
`src/lib/server/`. ADR 0011 remains the target security-domain decision and the
server roadmap allocates later delivery slices. The current module is a real
single-process vertical for one configured tenant; it is not a network server,
multi-tenant authorization boundary, or OS-isolated worker manager.

## Current Profile and Identity

The supported profile is canonical `jobspec-v2`, `embedded-cpu-v1` execution,
and `crash-durable` image artifacts. A `SingleTenantJobService` owns one
configured `TenantId`, one trusted durable state root, one finite quota
configuration, any number of retained Jobs, and one fresh in-process worker
object and Embedded Host for each explicitly accepted attempt.

The identity domains remain distinct strong C++ types:

| Identity | Current meaning | Explicit non-meaning |
| --- | --- | --- |
| `TenantId` | Sole tenant configured on one service/root | Authenticated principal or multi-tenant authorization |
| `JobId` | Stable identity of one accepted durable Job | IPC compute id or Graph session |
| `JobSpecDigest` | SHA-256 of exact canonical `jobspec-v2` bytes | Authorization by itself |
| `JobAttemptId` | One non-reused attempt in a Job's retry history | Job identity or local Run identity |
| `WorkerInstanceId` + `WorkerLeaseGeneration` | Exact fresh in-process worker assignment | PID, OS process, heartbeat, or supervisor lease |
| `GraphArtifactId` | Graph material key resolved by trusted configuration | Host path or local `GraphSessionId` |
| `ArtifactId` | Stable immutable durable image identity | Content digest, path, or IPC `OutputArtifactId` |
| `OutputCommitId` | Stable idempotent transaction identity for one Job output | Attempt identity or delivery lease |
| `ArtifactContentDigest` | SHA-256 of exact tight payload bytes | Artifact or commit identity |

Initial Job, artifact, and commit ids contain a collision-resistant service
namespace plus a checked local sequence. Retry preserves `JobId`, JobSpec
digest, checkpoint, `ArtifactId`, and `OutputCommitId`; it mints a fresh
attempt, worker, lease generation, and quota reservation. Restart restores
persisted identities and uses a new namespace only for newly submitted Jobs.

## Immutable JobSpec and Checkpoints

`JobSpec` has getters and no mutation surface. Its constructor accepts:

- one bounded opaque `GraphArtifactId`;
- one nonnegative target node;
- one bounded required `OutputSlotId`;
- a complete positive `JobResourceRequest` containing CPU slots, host-memory,
  output, staging, retention, and a sorted unique configured-device vector;
- an optional durable checkpoint `ArtifactId`;
- the closed `embedded-cpu-v1` execution profile; and
- the closed `crash-durable` durability request.

Canonical bytes begin with `jobspec-v2`. Decimal-length framing covers every
field, each resource scalar, each ordered device label/byte pair, and explicit
checkpoint presence/value. Integers use canonical decimal text before
framing. The constructor stores SHA-256 of the exact bytes. The control plane
revalidates before acceptance, retains `shared_ptr<const JobSpec>`, and binds
the digest into every assignment and durable record.

JobSpec contains no Host path, descriptor, pointer, runtime handle, credential,
IPC id, plugin DSO, mutable store location, quota token, or artifact mutation
capability. Before quota admission, the control plane resolves an optional
checkpoint only through the same tenant's validated durable artifact index.
The worker receives a read-only `ArtifactRecord`, never a root/path or commit
authority. The current Embedded CPU adapter validates this provenance but does
not claim algorithm-specific runtime-state restore.

## Tenant Quota Authority

`TenantQuotaAuthority` is the sole server-side capacity authority for the
configured tenant. Under one quota mutex, admission checks concurrency, CPU,
host memory, every configured device, output, staging, and retention as one
complete envelope. It either publishes one opaque reservation and all charges,
or changes nothing. A worker, plugin, JobSpec, or `ResourceLedger` token cannot
mint, enlarge, or release this server reservation.

Failed and cancelled attempts release the complete envelope exactly once.
Successful artifact commit converts reserved retention to the exact tight
payload charge and releases every active-attempt dimension. Durable artifact
deletion releases the retained charge only after manifest removal and
durability barriers. Startup reconstructs retained charges from validated
artifacts and fails closed if configured retention is below recovered data;
active attempt reservations are never reconstructed.

CPU slots also cap Embedded Host `maximum_parallelism`. Host-memory and device
values are conservative in-process admission declarations. They are not OS
memory/device enforcement and do not replace the worker-local
`ResourceLedger`; Issue #100 owns that process and OS-resource boundary.

## Durable Root, Jobs, and Artifacts

`DurableServerState` canonicalizes one trusted root, opens it without following
links, takes an exclusive nonblocking process lock, retains root/control/jobs/
artifacts directory descriptors, and revalidates the root device/inode binding.
No JobSpec, report, checkpoint, or plugin chooses a path. One live authority
owns one root.

Job records contain canonical JobSpec, current assignment and lease, stable
artifact/commit ids, cancellation and terminal facts, and any successful
receipt. Each update writes and synchronizes a private file, atomically renames
it over the authoritative record, then synchronizes jobs, control, and root
directories. Recovery strictly parses every record; ambiguous entries or
identity/content drift fail closed.

Artifact commit validates the server-owned request and CPU image, copies active
rows into a tight payload, and verifies output/staging/retention bounds. It then:

1. creates the fixed opaque artifact directory;
2. writes, synchronizes, reopens, and hashes `payload.bin`;
3. writes a private canonical manifest;
4. atomically publishes the fixed `manifest` name without replacement;
5. removes the private manifest; and
6. synchronizes the artifact directory, artifacts directory, and root.

Manifest presence is the visibility point. Pre-manifest unambiguous residue is
removed; a post-publication exception preserves the occurrence. Recovery and
lazy lookup verify descriptor, payload length/digest, tenant/Job/spec/slot/
artifact/commit joins, clean only safe residue, and reapply the barrier chain.
A retry with the same stable commit returns the original receipt only when all
stable identity, descriptor, digest, and payload facts match. The reporting
attempt may differ because the original acknowledgement can be lost. Any other
collision fails closed.

Deletion removes and synchronizes the authoritative manifest before payload
cleanup and retention release. A successful Job keeps its historical receipt,
but deleted bytes no longer resolve for lookup or checkpoint admission.

## Job State, Recovery, and Explicit Retry

The current state machine is:

```text
submit -> Queued -> Running ---------------------> Succeeded
                    |                                 ^
                    +-> Cancelling -> Cancelled       |
                    +---------------> Failed --retry--+
restart(active, no matching artifact) -> Failed(RecoveryInterrupted)
restart(any non-cancelled state, matching stable artifact) -> Succeeded
```

`submit()` validates/finalizes JobSpec and checkpoint, reserves the complete
quota envelope, persists accepted truth, inserts the Job, and starts one owned
assignment thread. The service inserts an empty ownership record under its
mutex before starting the native thread, then installs the sole handle by
no-throw move; a start failure exposes neither a Job nor a handle. Failure
cleanup independently attempts durable-record rollback and quota release, so
one cleanup error cannot suppress the other. `query()` copies current truth;
`wait_for()` bounds only observer waiting.

`retry(JobId)` accepts only a settled `Failed` Job with no current worker or
reservation. It preserves stable Job/spec/checkpoint/output truth, increments
the lease generation, creates fresh attempt/worker/quota authority, persists
the replacement, and starts a fresh worker. In-memory replacement uses a
no-throw swap; failure swaps back the prior failed truth, independently attempts
to restore its durable record, and releases the new reservation. Reports must
match the complete current tenant/Job/spec/attempt/worker/lease tuple, so a
stale attempt is fenced without settling, failing, cancelling, or committing
the replacement.

Once a worker report passes the identity and semantic-shape fence, a later
control-plane durability failure does not erase its outcome or settlement
evidence. A failure before manifest publication becomes settled `Failed` with
`ArtifactCommit`, releases the active reservation, and remains explicitly
retryable. A failure after manifest publication first revalidates and
reconciles that stable occurrence to `Succeeded` and charges retention exactly
once.

Restart never resumes process-local Graph/Run/Host/thread or ledger objects. A
nonterminal durable record with no matching committed artifact becomes settled
`Failed` with `RecoveryInterrupted` and is explicitly retryable. A matching
stable artifact is revalidated, charged once, and reconciled to `Succeeded`,
including a commit whose manifest became visible before its acknowledgement or
Job-state update. Terminal receipts are embedded in Job records, so historical
success survives later artifact deletion. If a stable artifact exists under a
Job's reserved output identity but any tenant/Job/spec/slot/commit join differs,
recovery reports durable corruption instead of adopting or overwriting it.

## Worker, Cancellation, and Completion Ordering

The Embedded worker validates the assignment, JobSpec digest, and optional
checkpoint, resolves graph material outside JobSpec, creates and seeds a fresh
Embedded Host, loads an attempt-local Graph, computes within reserved CPU
parallelism, validates one nonempty CPU image, closes the Graph, destroys Host
ownership, and returns only typed attempt facts plus a candidate image.

Worker report shapes and full-tuple fencing remain closed. Worker-owned failure
facts take precedence over cancellation relabelling. A stale prior-attempt
invocation is ignored without mutating the current retry; a malformed report
from the current attempt becomes `ReportRejected`. `cancel()` persists
monotonic intent. Cancellation that wins before commit discards a successful
candidate after settlement; durable commit and successful Job publication that
win first cannot be rewritten by later cancellation. The current public Host
has no forced compute cancellation, so a provider that ignores cooperative
observation can still delay shutdown indefinitely.

One private reaper joins completed assignment threads outside the Job mutex.
Destruction marks active Jobs cancelling and waits for worker/reaper drainage.
This is orderly in-process ownership only: there is no WorkerManager process,
heartbeat, crash/hang/OOM classification, address-space or syscall isolation,
forced termination, or bounded shutdown. Those properties remain Issue #100.

## Product Boundaries and Maintained Evidence

- The module is built only as `photospider_single_tenant_job_internal`; it is
  not installed or exported.
- `photospiderd` and protocol v2 are unchanged and do not serialize these Job,
  quota, checkpoint, or durable-artifact contracts.
- The configured `TenantId` is trusted configuration, not authentication.
- Trusted repository CPU operations run in the caller process. Tenant plugin
  ABI/network security and isolated plugin runtime remain absent.
- The current artifact format is one required tight CPU `ImageBuffer`, not a
  general runtime Value/checkpoint format or bulk data plane.

Long-lived entry points are:

- contracts: `src/lib/server/job_contract.{hpp,cpp}`;
- quota: `src/lib/server/tenant_quota.{hpp,cpp}`;
- durable state: `src/lib/server/durable_server_state.{hpp,cpp}`;
- control plane: `src/lib/server/single_tenant_job_service.{hpp,cpp}`;
- Embedded adapter: `src/lib/server/embedded_job_worker.{hpp,cpp}`;
- focused authority/lifecycle tests:
  `tests/unit/test_single_tenant_job_service.cpp`; and
- real Embedded Host durable product path:
  `tests/integration/test_single_tenant_job_product_path.cpp`.

Maintained tests cover canonical digest/validation, every quota dimension and
multi-device accounting, exact settlement, manifest-before/after failure, root
locking/no-follow/identity drift, safe cleanup, corruption and exact Job/artifact
recovery joins, idempotent reconciliation, retention deletion, checkpoint
authorization/re-authorization, explicit retry and fresh fencing, submit/retry
thread-start rollback, interrupted/successful restart, cancellation ordering,
stale/malformed reports, ongoing thread reaping, and real Embedded Host
output/checkpoint/restart behavior.

The target multi-process model remains governed by
[ADR 0011](../adr/0011-server-control-plane-workers-and-plugin-runtimes-are-separate-security-domains.md)
and the [server roadmap](../roadmap/Kernel-Evolution.md#server-and-plugin-isolation).
