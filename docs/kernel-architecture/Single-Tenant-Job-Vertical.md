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
  output, staging, retention, and a sorted unique configured-device vector of
  at most 128 rows;
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
deletion releases the quota authority's exact retained charge only after the
artifact-directory, artifacts-directory, and root barriers confirm manifest
visibility removal. A visibility-unconfirmed failure keeps the charge; a
confirmed visibility removal releases it even when private payload/directory
cleanup later fails. Startup reconstructs retained charges from validated
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

Job-record replacement has three explicit outcomes:

- `NotPublished`: atomic rename did not occur, the prior durable/cache truth
  remains authoritative, and ordinary caller rollback is safe;
- `RecordPublishedDurabilityUnconfirmed`: rename made the replacement visible,
  but a required jobs/control/root directory barrier failed; and
- `ConfirmedCommitted`: every required directory barrier completed.

Serialization, filenames, and replacement-cache storage are prepared before
rename. The cache is swapped into its replacement under the durable-state
mutex before filesystem mutation and is restored on `NotPublished`; after
rename, cache truth is already aligned and no allocation or throwing cache
publication remains. A failure at either published state is never translated
into rollback. `SingleTenantJobService` keeps the published snapshot and its
active quota, fences workers, rejects later durable mutations, and requires
restart. Restart then revalidates the record and converts a surviving
nonterminal attempt to `RecoveryInterrupted` unless its stable artifact proves
success.

Artifact commit validates the server-owned request and CPU image, copies active
rows into a tight payload, verifies output/staging/retention bounds, and
reconciles any pre-existing durable occurrence or safe residue. A fresh
publication then:

1. prepares complete private `ArtifactId` and `OutputCommitId` index copies and
   one ArtifactId-keyed durability-confirmation copy, then installs all three
   with rollback authority;
2. creates the fixed opaque artifact directory;
3. writes, synchronizes, reopens, and hashes `payload.bin`;
4. writes a private canonical manifest;
5. atomically publishes the fixed `manifest` name without replacement and
   immediately makes both installed aliases authoritative and recognizable,
   with their shared confirmation state still pending;
6. removes the private manifest; and
7. synchronizes the artifact directory, artifacts directory, and root, then
   records confirmation under the same mutex before acknowledging completion.

Manifest presence is the visibility point. Pre-manifest unambiguous residue is
removed and both alias plus confirmation copies are restored. After manifest
publication both aliases are authoritative and internally recognizable before
any later throwing cleanup, validation, observer, or barrier, so neither alias
can be partially indexed or misreported absent. They are not yet eligible for
an external artifact/receipt return while confirmation is pending. The first
`ArtifactId` lookup, `OutputCommitId` lookup, same-commit retry, or service
reconciliation must reload and verify the exact descriptor, payload
length/digest, tenant/Job/spec/slot/artifact/commit joins and replay the complete
artifact-directory, artifacts-directory, and root barrier chain. Only the
single locked no-throw confirmation transition then permits a crash-durable
return. Confirmation occurs before the final completion observer, so a lost
acknowledgement after the root barrier preserves confirmed truth. Recovery and
lazy repair install both exact aliases plus confirmation as one transaction.
A retry with the same stable commit returns the original receipt only when all
stable identity, descriptor, digest, and payload facts match. The reporting
attempt may differ because the original acknowledgement can be lost. Any other
collision fails closed.

Deletion pre-stages both indexes without the target, then reports one of four
irreversible states: `NotRemoved`,
`ManifestRemovedDurabilityUnconfirmed`,
`VisibilityRemovalConfirmedCleanupPending`, or `FullyCleaned`. Before manifest
removal, failure keeps both aliases and mutation remains available. Once the
manifest is absent, both aliases are revoked together, so lookup and same-
process checkpoint admission cannot expose stale bytes. The service retains
quota while visibility durability is unconfirmed; after the full visibility
barrier chain it releases the quota authority's exact charge even if payload or
directory cleanup remains restart-recoverable. Any failure after visibility
became irreversible fail-stops workers and later durable mutation until
restart. A successful Job keeps its historical receipt, but deleted bytes no
longer resolve for lookup or checkpoint admission.

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
quota envelope, inserts the in-memory Job and ownership record, starts its sole
assignment thread while the service mutex still blocks worker progress, and
then publishes accepted truth. A native-thread start failure therefore occurs
before durable publication and exposes neither a Job nor a handle. A
`NotPublished` journal failure removes the candidate and releases its quota. A
published failure keeps the Job, worker authority, and quota aligned with the
visible record and enters the monotonic journal fail-stop.
`query()` copies current truth; `wait_for()` bounds only observer waiting, and
both remain available while fail-stopped.

`retry(JobId)` accepts only a settled `Failed` Job with no current worker or
reservation. It preserves stable Job/spec/checkpoint/output truth, increments
the lease generation, creates fresh attempt/worker/quota authority, installs
the replacement and blocked worker, and then publishes the replacement.
`NotPublished` restores the prior failed truth and releases the new
reservation; either published outcome retains the new attempt and reservation,
fences worker progress, and fail-stops later durable mutation. Reports must
match the complete current tenant/Job/spec/attempt/worker/lease tuple, so a
stale attempt is fenced without settling, failing, cancelling, or committing
the replacement.

Once a worker report passes the identity and semantic-shape fence, a later
control-plane durability failure does not erase its outcome or settlement
evidence. A failure before manifest publication becomes settled `Failed` with
`ArtifactCommit`, releases the active reservation, and remains explicitly
retryable. A failure after manifest publication first revalidates and replays
the pending barrier chain before reconciling that stable occurrence to
`Succeeded` and charging retention exactly once. Once exact matching artifact
truth is observed—or lookup/revalidation remains manifest-visible ambiguous—any
later barrier replay, quota conversion, or `Succeeded` Job-record publication
failure enters monotonic reconciliation fail-stop. It never writes a
compensating `Failed/ArtifactCommit` record and never releases a still-valid
reservation. A failed quota conversion keeps the active reservation; a
successful conversion followed by pre-publication Job-journal failure keeps
the retained charge. Workers and later reports/mutations are fenced until
restart reconstructs and reconciles the strongest durable truth.

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
win first cannot be rewritten by later cancellation. If cancellation intent is
not published, the prior intent remains authoritative; if its record becomes
visible but a later durability barrier or completion observer fails, the
service keeps `Cancelling`, fences the worker, and enters the same journal
fail-stop. The current public Host has no forced compute cancellation, so a
provider that ignores cooperative observation can still delay shutdown
indefinitely.

One private reaper joins completed assignment threads outside the Job mutex.
Destruction marks active Jobs cancelling and waits for worker/reaper drainage.
This is orderly in-process ownership only: there is no WorkerManager process,
heartbeat, crash/hang/OOM classification, address-space or syscall isolation,
forced termination, or bounded shutdown. Those properties remain Issue #100.

## Product Boundaries and Maintained Evidence

- The non-installed, non-exported
  `photospider_single_tenant_job_internal` target and both maintained Job test
  targets exist by default only on Darwin and Linux. The independent
  `PHOTOSPIDER_BUILD_SINGLE_TENANT_JOB` gate defaults off elsewhere, rejects an
  explicit enable on unsupported systems, and CMake asserts the target
  inventory in both enabled and disabled profiles.
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

Maintained tests cover canonical digest/validation, the shared 128-device
admission/recovery maximum and 129-device rejection, every quota dimension and
multi-device accounting, exact settlement, all Job-record publication/barrier
fault stages with in-memory and restart truth, manifest-before/after failure,
pre-manifest dual-index preparation rollback, manifest-visible pending
lookup/retry barrier replay and replay failure, post-root-barrier acknowledgement
loss, quota-conversion reconciliation fail-stop, repeated `Succeeded`
pre-publication journal failure, worker/report fencing and restart quota truth,
root locking/no-follow/identity drift, safe cleanup, corruption and exact
Job/artifact recovery joins, idempotent reconciliation, all artifact-deletion
fault stages with dual-alias revocation, exact quota, fail-stop and restart
cleanup, same-process deleted-checkpoint rejection, checkpoint
authorization/re-authorization, explicit retry and fresh fencing, submit/retry
thread-start rollback, submit/retry/cancel journal fail-stop,
interrupted/successful restart, cancellation ordering, stale/malformed reports,
ongoing thread reaping, target-inventory platform gating, and real Embedded Host
output/checkpoint/restart behavior.

The target multi-process model remains governed by
[ADR 0011](../adr/0011-server-control-plane-workers-and-plugin-runtimes-are-separate-security-domains.md)
and the [server roadmap](../roadmap/Kernel-Evolution.md#server-and-plugin-isolation).
