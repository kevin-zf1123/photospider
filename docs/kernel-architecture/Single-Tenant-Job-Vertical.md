# Single-Tenant Job Vertical

This document defines the current source-private Issue #99/#100/#105 Job behavior
under `src/lib/server/`. ADR 0011 remains the broader target security-domain
decision and the server roadmap allocates later delivery slices. The current
module is a real local vertical for one configured tenant with freshly execed
attempt processes and an attempt-scoped stream artifact data plane. It
is not a network server, multi-tenant authorization boundary, separate
WorkerManager/artifact service, remote data plane, or untrusted-plugin sandbox.

## Current Profile and Identity

The supported profile is canonical `jobspec-v2`, `embedded-cpu-v1` execution,
and `crash-durable` image artifacts. A `SingleTenantJobService` owns one
configured `TenantId`, one trusted durable state root, one finite quota
configuration, any number of retained Jobs, and one fresh worker process and
Embedded Host for each explicitly accepted attempt. One source-private
`WorkerManager` inside the service authority owns every process and supervision
handle.

The identity domains remain distinct strong C++ types:

| Identity | Current meaning | Explicit non-meaning |
| --- | --- | --- |
| `TenantId` | Sole tenant configured on one service/root | Authenticated principal or multi-tenant authorization |
| `JobId` | Stable identity of one accepted durable Job | IPC compute id or Graph session |
| `JobSpecDigest` | SHA-256 of exact canonical `jobspec-v2` bytes | Authorization by itself |
| `JobAttemptId` | One non-reused attempt in a Job's retry history | Job identity or local Run identity |
| `WorkerInstanceId` + `WorkerLeaseGeneration` | Exact fresh process assignment and manager-fenced lease | Raw PID capability, Job identity, or authorization by itself |
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
The worker receives the receipt, descriptor, digest, size, and a non-authorizing
join reference over control, then reconstructs a read-only `ArtifactRecord`
only from its inherited checkpoint data descriptor. It never receives a root,
path, quota token, stable-output transaction, or commit authority. The current
Embedded CPU adapter validates this provenance but does not claim algorithm-
specific runtime-state restore.

Checkpoint authorization is independent of control-frame capacity. The exact
durable bytes and receipt must join the configured tenant and JobSpec
`ArtifactId`, and the payload must fit the accepted host-memory envelope.
Worker-side materialization receives the exact declared byte count followed by
stream EOF, then revalidates descriptor and SHA-256 before Graph construction.
A mismatch is rejected without quota, Job, retry, or artifact authority being
inferred from the stream occurrence.

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

Active-attempt release validates the complete subtraction before its first
mutation and has a strong exception guarantee. If release raises, the service
keeps the exact reservation owner on the terminal Job control, or transfers a
submit/retry rollback owner with no durable Job into one service-owned stranded
slot. It then monotonically fail-stops submit, retry, cancellation, worker
reports, artifact deletion, and every other durable mutation. Query, bounded
wait, artifact lookup, and quota inspection remain available. The service does
not retry release or publish a compensating terminal record in the same
process. Restart drops process-local active reservations, reconstructs durable
Job/artifact truth, and clears this fail-stop.

CPU slots also cap Embedded Host `maximum_parallelism`. WorkerManager applies
accepted host memory as the worker's POSIX `RLIMIT_AS` soft bound before
`exec`; this constrains total virtual address space rather than measured RSS,
so the executable and its runtime mappings must fit the requested envelope.
As an independent worker-filesystem defense, WorkerManager prepares an
`RLIMIT_FSIZE` soft bound exactly equal to the accepted output/staging/retention
minimum. A lower finite inherited hard limit fails the owning attempt as
`WorkerStartup` before `fork`; it never silently narrows the stream data-plane
maximum already derived from the JobSpec. The limit is not a transport and no
file-backed fallback exists.
Configured device values remain admission declarations, not device isolation.
Neither dimension replaces the worker-local `ResourceLedger`; there is no
current syscall filter, cgroup/container, GPU-memory enforcement, or hostile-
plugin sandbox.

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

`Running` is the durable control-plane fence that a still-current,
non-cancelled assignment has begun manager supervision. It is published before
the external process is spawned and therefore does not prove exec,
`AssignmentAccepted`, or entry into the worker's heartbeat/cancellation loop.
Maintained real-process cancellation evidence waits instead for a
source-private, non-authorizing first-valid-heartbeat observation. The
cooperative and ignoring fixtures run in separate service instances with
branch-local heartbeat/cooperative bounds, so scheduler headroom for a
responsive worker does not slow the short forced-escalation branch. That
observer changes no production deadline and exposes no identity, PID,
descriptor, signal, wait, reap, cancellation, or completion authority.

`submit()` validates/finalizes JobSpec and checkpoint, reserves the complete
quota envelope, inserts the in-memory Job, and asks WorkerManager to construct
and retain its sole manager record and supervision handle while the service
mutex still blocks assignment progress, then publishes accepted truth. Bulk
data-plane creation, checkpoint transfer, and output transfer occur only after
the registered supervision thread owns the attempt, outside this service mutex.
A
manager-record construction, registry insertion, or supervision-thread start
failure therefore occurs before child spawn or durable publication and exposes
neither a Job nor a handle. Supervision-thread construction begins only after
`records_.emplace()` succeeds and shares one catch boundary with a deterministic
source-private start-failure seam; any exception erases that exact record before
submit/retry performs its Job and candidate-quota rollback. Maintained tests
capture proof of prior insertion, observe zero manager ownership after the
exception, and then prove later submit/retry recovery. A
`NotPublished` journal failure removes the candidate and releases its quota. A
published failure keeps the Job, worker authority, and quota aligned with the
visible record and enters the monotonic journal fail-stop.
If manager-record/thread start or `NotPublished` rollback cannot release quota,
the candidate Job remains unpublished, the exact reservation owner moves to
the service's stranded slot, the original submit error is rethrown, and all
later mutation is fail-stopped until restart.
`query()` copies current truth; `wait_for()` bounds only observer waiting, and
both remain available while fail-stopped.

`retry(JobId)` accepts only a settled `Failed` Job with no current worker or
reservation. It preserves stable Job/spec/checkpoint/output truth, increments
the lease generation, creates fresh attempt/worker/quota authority, retains the
blocked manager record, and then publishes the replacement.
`NotPublished` restores the prior failed truth and releases the new
reservation; either published outcome retains the new attempt and reservation,
fences worker progress, and fail-stops later durable mutation. Reports and
manager actions must match the complete current tenant/Job/spec/attempt/worker/
lease tuple, so a
stale attempt is fenced without settling, failing, cancelling, or committing
the replacement.
If manager-record/thread start or `NotPublished` retry rollback cannot release
the fresh reservation, the prior failed Job truth remains authoritative, the
fresh owner moves to the stranded slot, the triggering retry error is rethrown,
and the same fail-stop applies without a same-process retry.

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

The same release-failure rule applies after Failed, Cancelled,
`ReportRejected`, malformed-report, and pre-manifest `ArtifactCommit` terminal
publication. Durable terminal truth is not rewritten merely because quota
settlement raised; the reservation remains owned, all mutation and later
reports are fenced, and restart recovers the recorded terminal state with zero
active reservations.

Restart never resumes process-local Graph/Run/Host/process or ledger objects. A
nonterminal durable record with no matching committed artifact becomes settled
`Failed` with `RecoveryInterrupted` and is explicitly retryable. A matching
stable artifact is revalidated, charged once, and reconciled to `Succeeded`,
including a commit whose manifest became visible before its acknowledgement or
Job-state update. Terminal receipts are embedded in Job records, so historical
success survives later artifact deletion. If a stable artifact exists under a
Job's reserved output identity but any tenant/Job/spec/slot/commit join differs,
recovery reports durable corruption instead of adopting or overwriting it.

## Worker, Cancellation, and Completion Ordering

Product composition resolves trusted graph material outside JobSpec and before
service ownership, then retains it in an immutable
`PreparedExternalGraphCatalog`. Before retaining each entry, catalog
construction applies the private protocol's sole 16-KiB text-field byte bound
to `root_dir`, `yaml_path`, `config_path`, `cache_root_dir`, and `message`.
Exactly 16 KiB is valid; one extra byte synchronously raises a field-specific
`std::length_error` before factory/service construction and therefore before
DurableServerState, quota, Job, supervision-thread, channel, or process
ownership. A failed constructor exposes no partial catalog and cannot become a
late `WorkerStartup` fact. WorkerManager first registers one immutable manager
record and supervision handle. Only that supervision owner creates two private
`AF_UNIX SOCK_STREAM` lanes, reduces the checkpoint lane to
manager-send/worker-receive and the output lane to worker-send/manager-receive,
and marks both manager endpoints nonblocking. This setup, all bulk transfer,
and every failure occur outside the service mutex; a setup failure retires only
the owning attempt and closes every endpoint without a path, staging file, or
artifact residue. The supervisor then forks/execs one non-installed
`photospider-worker` and registers its exact PID before a non-virtual in-memory
catalog lookup copies material into exactly one immutable assignment. It
invokes no resolver and performs no graph/path or data-plane filesystem I/O.
The fork child performs only descriptor setup, `RLIMIT_AS`, `RLIMIT_FSIZE`,
descriptor closure, and `exec`; the freshly execed worker validates Assignment
metadata and JobSpec digest, receives and validates any checkpoint through fd
5, creates and seeds a fresh Embedded Host, opens and loads an attempt-local
Graph, computes within reserved CPU parallelism, sends tight candidate rows
through fd 6, closes the Graph, destroys Host ownership, and returns only typed
attempt and staged-output metadata.

Pre-exec descriptor ownership is exact: fd 0-2 are standard streams, fd 3 is
the private control socket, and close-on-exec fd 4 carries setup `errno` to the
parent. Fd 5 is the worker's receive-only checkpoint lane and fd 6 is its
send-only output lane; the manager retains only the opposite direction of each
stream.
Reverse worker checkpoint send and reverse manager output send fail locally
without SIGPIPE termination: Linux uses `MSG_NOSIGNAL`, Darwin installs
`SO_NOSIGPIPE` on both endpoints, and the portable rejection set is `EPIPE`,
`ECONNRESET`, or `ENOTCONN`. Both permitted directions retain exact byte and
EOF semantics.
On Darwin, the parent queries the kernel `kern.maxfilesperproc` ceiling before
`fork` and the allocation-free child closes every slot in `[7, ceiling)`,
treating only `EBADF` as an unused slot. The current soft
`RLIMIT_NOFILE` is not a safe boundary because an already-open high descriptor
survives a later limit decrease. On Linux, the child uses raw
`close_range(7, UINT_MAX, 0)`; any error, including an unavailable syscall on
an older kernel, is reported through fd 4 and fails startup. There is no
arbitrary finite fallback or `RLIM_INFINITY`-to-`INT_MAX` userspace scan. The
maintained process regressions hold a high non-close-on-exec sentinel in an
isolated authority, exercise both an infinite soft limit and a soft limit
lowered below the already-open sentinel, and prove timely exec plus sentinel
non-inheritance while the parent copy stays open.

Parent-side WorkerManager descriptors follow a different close rule from the
fork-child closure sweep: a `UniqueFd` first replaces or clears ownership, then
issues exactly one `close` and ignores every result, including `EINTR`. Linux
may already have released and reassigned the numeric fd before reporting an
interrupted close, so a retry could close another thread's newly acquired
descriptor. A source-private callback regression forces that release/reuse
ordering and proves no second close consumes the reused descriptor.

The worker exec bootstrap requires `--control-fd`, `--checkpoint-data-fd`,
`--output-data-fd`, `--startup-timeout-ms`, and `--io-timeout-ms`.
WorkerManager prepares those strings before fork. The worker applies the exact
configured startup duration to its initial Assignment receive/checkpoint
materialization and the exact I/O duration to acceptance, heartbeat, and
Report writes; no worker-local default or cap can shorten the manager policy.
Bootstrap descriptors and startup remain outside control payloads because they
are process capabilities/policy needed before the first frame is available.

The parent also applies that configured startup duration to fd 4 as one
absolute exec-bootstrap deadline. Its nonblocking reader retains partial
native-`int` state across `EINTR`/`EAGAIN`; partial-record EOF remains a
truncated setup failure. Poll/read readiness is not acceptance: after reading
one complete child `errno` or clean close-on-exec EOF, WorkerManager takes a
fresh monotonic observation and exposes the result only while
`now < deadline`. Equality or later is the typed `WorkerStartup` deadline,
before child-error or exec-success classification, and the existing exact PID
owner performs TERM-to-KILL-to-`waitpid` cleanup. The separate Assignment
startup window created afterward cannot revive a late exec result. Maintained
real-process tests cross the boundary deterministically for both complete
results and require zero process, thread, quota, receipt, and artifact residue.

The source-private duration domain is closed before durable ownership. Each of
the nine `WorkerManagerOptions` fields is positive and at most the inclusive
`4,294,967,295 ms` shared maximum. `heartbeat_interval` has the narrower
inclusive maximum `4,294,967,294 ms` and must remain strictly less than
`heartbeat_timeout`; this also preserves the protocol's unsigned 32-bit
millisecond cadence. Construction validates every named field before opening
the durable root, while exec argument construction and parsing independently
enforce the startup/I/O maximum. Supported Darwin/Linux monotonic clocks
represent every admitted millisecond exactly. Each deadline captures its base
once, checks `base <= time_point::max() - duration`, and adds to that same base;
clock-range exhaustion raises `std::overflow_error` without evaluating an
overflowing sum. Consequently `milliseconds::max()` cannot enter conversion or
deadline arithmetic, and validation cannot become stale against a later clock
observation.

The private bounded protocol now has fixed magic, sole supported version 2,
closed message kinds, a 128-KiB control-payload maximum, deadline-aware partial
I/O, and strict trailing-byte, enum, identity, digest, descriptor, and Job-
resource validation. Version 1 is rejected without a compatibility decoder or
bulk fallback. One source-private `kMaximumWorkerTextFieldBytes` constant
governs all five prepared graph strings plus Report diagnostics in catalog
admission, Assignment/Report encoding, and decoding. The exact limit is
inclusive; local prepared-value excess is `std::length_error`, while oversized
wire content is `WorkerProtocolError`.

Assignment carries complete attempt/JobSpec and bounded graph metadata,
optional checkpoint receipt/descriptor/digest/size/reference, output-stage
reference/maximum, and cadence. Report carries outcome facts and, only for a
settled success, output reference/slot/descriptor/size/digest. Neither message
can encode checkpoint bytes, candidate rows, an `ImageBuffer`, blob, `Value`,
path, or raw file descriptor. The fully declared maximum metadata envelope fits
the control bound. A candidate above the former 64-MiB aggregate frame limit is
valid when it fits the accepted output/staging/retention and file-size
envelopes. The file-size limit exactly matches that accepted maximum; a lower
finite inherited hard limit is a pre-fork `WorkerStartup`, not a smaller
runtime envelope. A resource-envelope excess becomes one identity-preserving
settled `Failed(Compute)` metadata Report with a fixed bounded diagnostic and
an empty stage; there is no transport-size fallback.

WorkerManager exposes staged output only after one current-identity Report,
exact stream EOF, clean zero exit, exact reap, and control-channel EOF. The
worker sends reference/descriptor/exact-size/digest metadata before output
bytes and retains its source while its real heartbeat thread stays active.
While the exact child remains lifecycle-owned and unreaped, the manager creates
one lazy pathless anonymous owner of exactly that final image size, drains at
most one 64-KiB nonblocking slice directly into it per monitor iteration,
enforces the accepted maximum, and incrementally computes SHA-256. Every later
slice follows cancellation/shutdown/runtime/heartbeat arbitration. Output
progress, including continuous or prebuffered bytes, cannot renew or revive the
heartbeat deadline; only a valid current-identity Heartbeat frame can. There is
no cumulative accumulator growth, whole-payload reconstruction copy, or
post-reap bulk access. Reference, slot, tight descriptor, exact stream byte
count, resource bound, and digest must all join before completion handoff. The
worker closes the output lane only after exact bytes and remains alive and
terminable until the manager completes the join and O(1) moves the already-final
image owner, then replies with one matching identity-only
`CompletionReady`. The acknowledgement grants no Job, quota, artifact, commit,
or publication authority. If a prior cancellation-channel failure makes the
reply impossible, an already completely joined Report may retain its ordinary
classification. The post-reap drain is control metadata only: it never reads
the bulk lane and performs no filesystem I/O, blocking data transfer, bulk
allocation, or content hashing. A mismatch becomes a worker-protocol failure
and cannot mint a receipt or Job/quota/retry truth. Only the existing service and
`DurableServerState` may then publish stable ArtifactId/OutputCommitId truth
through the manifest-last durable transaction.

Maintained real-process evidence makes a greater-than-64-MiB candidate readable,
then holds it pending across one complete heartbeat timeout. A source-private,
non-authorizing manager observation proves that a second-or-later valid
current-identity Heartbeat is accepted during that phase. The paired fixture
that emits only its first valid Heartbeat must fail as `WorkerHeartbeatTimeout`
with no receipt, artifact, quota, process, thread, or descriptor residue. The
observation is absent from product construction and cannot alter liveness,
ownership, or publication.

Manager and worker short-poll loops each retain one decoder for their channel:
deadline expiry preserves partial header/payload bytes and exact offsets, while
clean EOF remains valid only at a fresh frame boundary. A transport-complete
frame remains decoder-owned until the caller interprets its Assignment,
`AssignmentAccepted`, Heartbeat, Report, Cancel, or `CompletionReady` semantics
and the decoder takes one fresh monotonic sample. Only a sample strictly before
the earliest applicable absolute lifecycle deadline moves/resets that frame and
returns its exact acceptance time; a tie or later sample retains the complete
frame for the next bounded semantic retry and grants no lifecycle mutation.
Socket readiness budget is distinct from that semantic lifecycle acceptance.
During pending output, a due budget performs exactly one nonblocking control
probe before one bulk slice; it does not authorize a late frame. Control writes
check the same strict boundary before and after positive send progress. A
timeout after progress may mean that the peer received a prefix or final byte,
so callers treat the write as failed and never retry that frame. A cancellation
owner may retain the channel only for bounded receive-side report/EOF/exit
drainage. `test_worker_protocol` places a deterministic clock crossing after
Cancel identity interpretation and before semantic commit, then half-closes the
peer to prove retry returns the retained frame rather than EOF; the real-process
fragmented-Cancel supervisor test continues to cross multiple poll slices.

WorkerManager alone owns spawn, the private channel, the PID, signal delivery,
`waitpid`, and supervision-thread reaping. No API accepts or exposes a PID.
Every cancellation or signal path revalidates the complete tenant/Job/spec/
attempt/worker/lease record and its retained PID. A candidate report becomes
eligible for control-plane adjudication only after one clean worker exit,
channel closure, and exact reap. Startup/exec, nonzero exit, signal death,
channel loss, protocol violation, heartbeat timeout, runtime timeout, and
forced-cancellation facts use separate durable failure categories and affect
only the owning attempt.
Product construction rejects `SIGCHLD=SIG_IGN` and `SA_NOCLDWAIT` before
opening the durable root, and every spawn revalidates waitable `SIGCHLD`
immediately before `fork`. A later policy mutation, a competing reaper, or any
non-`EINTR` exact-`waitpid` error including `ECHILD` loses exact-reaping
authority and fail-stops before any completion callback, completed-record
mark, or record deletion. A record retaining a live PID can never be marked
complete or erased.

Exact reaping alone also cannot retire the manager record. Once assignment
begin succeeds or raises, WorkerManager constructs every actual first
`Report` (external or explicit in-process test marker), `Failure`, and
`ForcedCancellation` completion inside its own no-throw construction boundary,
and its control-plane callback must return before `mark_completed`. If fault
injection, identity/message/report retention, wait-status formatting, or return
construction raises, including `std::bad_alloc`, that local boundary invokes no
callback and immediately enters the fixed allocation-free fail-stop; the
exception cannot escape to the outer generic classifier and be rewritten as a
second failure completion. If the callback raises, it is not retried because it
may have partially applied durable truth. Both cases fail-stop before completed-
record marking or ordinary record deletion. They never fabricate a replacement
completion or release the service-owned quota reservation. Restart remains the
only reconciliation boundary for the durable Job and quota owner after this
fail-stop. A begin callback that returns false remains the sole legitimate no-
completion retirement path because it fences an unpublished or replaced
assignment before worker execution.

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
fail-stop. After accepted intent, WorkerManager first sends exact cooperative
cancellation. A send failure continues bounded report/EOF/wait-status drainage
and preserves an actual Failed report, nonzero exit, signal death, or channel
close; it does not itself mint forced cancellation. A worker still alive at the
cooperative deadline has its channel closed/revoked and receives owned
`SIGTERM`/`SIGKILL` escalation under configured bounds before exact reaping.
The deadline decision performs another exact nonblocking exit observation. If
that observation reaps a natural exit before channel revocation, reaping is not
treated as channel EOF: WorkerManager retains the parent socket and stateful
decoder through a separate bounded post-reap drain so an already buffered
Report and EOF remain ordinary report/channel/exit truth. That path sends no
signal, performs no second reap, and cannot produce forced cancellation merely
because the cooperative deadline elapsed.
After cancellation delivery has been attempted, a socket-system read error
marks the channel unavailable inside the same bounded monitor. Further decoding
stops, but process ownership, cooperative/escalation deadlines, and exact reap
observation continue, so a signal/nonzero wait status or already decoded Report
outranks `WorkerChannel`. Only a clean zero exit with no Report remains a
channel failure. While the cooperative deadline is active, the ordinary EOF/
post-report deadline is subordinate to it and cannot terminate/reap the worker
through the generic channel path first. Exact exit status or manager-owned
escalation therefore settles before any residual `WorkerChannel` fact.
A complete, valid candidate Report does not weaken this ordering while its
worker remains alive: the ordinary post-report close/exit deadline cannot
terminate or reap first. A worker-owned signal or nonzero exit observed before
the cooperative deadline remains authoritative; only a worker still alive at
that deadline enters cancellation-owned `SIGTERM`/`SIGKILL` escalation and may
be classified as forced cancellation.
Only an exact `WIFSIGNALED` status matching a successfully delivered owned
`SIGTERM` or `SIGKILL` yields a forced-cancellation fact. Successful `kill()`
against an already exited zombie is not causality; a normal zero exit remains
report/channel/exit truth. Destruction records cancellation without waiting
under the Job mutex, then drains all attempts concurrently through the same
escalation path. Reap observation is nonblocking through the final kill/reap
deadline; if exact reaping remains unobservable, the authority process fails
stop rather than block indefinitely or return live ownership.

Because all filesystem opening and graph loading now occurs after exec under an
exact retained PID, blocked trusted I/O follows that same escalation path. A
maintained FIFO-backed process fixture proves bounded forced cancellation and
exact reaping, bounded service destruction, and successful execution by a
fresh worker after the material becomes readable. The non-virtual prepared
catalog boundary separately removes any pre-PID resolver callback that could
hold the manager reaper indefinitely.

One private reaper joins completed supervision handles outside both manager and
Job mutexes. The explicit source-private test marker may execute deterministic
unit-test workers in the supervision thread; it is non-installed and makes no
process-isolation or bounded-termination claim. Ordinary construction rejects
an unmarked in-process factory, an absent/non-executable worker path, or a
product process configured to auto-reap `SIGCHLD` children.

## Product Boundaries and Maintained Evidence

- The non-installed, non-exported
  `photospider_single_tenant_job_internal` and `photospider-worker` targets,
  protocol/manager unit coverage, process fixture/supervision integration
  coverage, and Embedded product-path coverage exist by default only on Darwin
  and Linux. The independent
  `PHOTOSPIDER_BUILD_SINGLE_TENANT_JOB` gate defaults off elsewhere, rejects an
  explicit enable on unsupported systems, and CMake asserts the target
  inventory in both enabled and disabled profiles.
- `photospiderd` and local IPC protocol v2 are unchanged and do not serialize
  these Job, quota, checkpoint, or durable-artifact contracts; private worker
  protocol v2 is a distinct source-private wire.
- The configured `TenantId` is trusted configuration, not authentication.
- Trusted repository CPU operations for this vertical run in the attempt
  process. Tenant plugin ABI/network security, syscall isolation, and isolated
  hostile-plugin runtime remain absent.
- The current artifact format is one required tight CPU `ImageBuffer`. The
  local direction-reduced stream adapter is an attempt-scoped bulk data plane for that
  format, not a general runtime `Value` format, remote transport, object store,
  or standalone artifact service.

Long-lived entry points are:

- contracts: `src/lib/server/job_contract.{hpp,cpp}`;
- quota: `src/lib/server/tenant_quota.{hpp,cpp}`;
- durable state: `src/lib/server/durable_server_state.{hpp,cpp}`;
- control plane: `src/lib/server/single_tenant_job_service.{hpp,cpp}`;
- Embedded adapter: `src/lib/server/embedded_job_worker.{hpp,cpp}`;
- private worker transport and lifecycle:
  `src/lib/server/worker_protocol.{hpp,cpp}`,
  `src/lib/server/worker_artifact_data_plane.{hpp,cpp}`, and
  `src/lib/server/worker_manager.{hpp,cpp}`;
- one-assignment composition root: `apps/photospider_worker/main.cpp`;
- focused authority/lifecycle tests:
  `tests/unit/test_single_tenant_job_service.cpp` and
  `tests/unit/test_worker_protocol.cpp`;
- real-process lifecycle fixture and integration coverage:
  `tests/support/photospider_worker_fixture.cpp` and
  `tests/integration/test_worker_supervisor.cpp`; and
- real Embedded Host durable product path:
  `tests/integration/test_single_tenant_job_product_path.cpp`.

Maintained tests cover canonical digest/validation, the shared 128-device
admission/recovery maximum and 129-device rejection, every quota dimension and
multi-device accounting, exact settlement and strong-guarantee release fault,
all Job-record publication/barrier
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
manager-record/thread-start rollback, submit/retry/cancel journal fail-stop,
interrupted/successful restart, cancellation ordering, stale/malformed reports,
release-failure ownership for submit/retry manager-record/thread start and
`NotPublished` rollback, Failed/Cancelled/rejected/malformed/pre-manifest terminal truth,
read-only availability, report/mutation fencing, and restart convergence,
ongoing handle/process reaping, target-inventory platform gating, bounded
protocol reconstruction, fresh process identity, crash/protocol/heartbeat/
runtime isolation, all-nine duration over-bound rejection before durable
ownership, exact shared-duration and heartbeat-relation boundaries, checked
monotonic deadline range, FIFO-held fresh-retry stale-lease rejection,
cooperative/
forced cancellation after a first-heartbeat rendezvous with branch-local
bounds, cancel-channel-versus-wait-status attribution,
deadline-side natural-reap buffered-report drainage, candidate-Report-deadline-
versus-wait-status attribution, maximum-declared metadata-envelope accounting,
protocol-v1 rejection, metadata-only control independence from bulk size,
digest/descriptor/reference fail-closed checks including true staged-byte joins
and stale attempts with zero process/quota/receipt/artifact/staging residue,
checkpoint and candidate transfer above the former aggregate control bound,
paused checkpoint transfer with responsive service cancellation, bounded
shutdown while a worker is backpressured on paused output drainage, all five prepared graph
fields at the exact shared boundary through
a real process, field-specific one-byte-over catalog rejection before service
ownership with no durable-root/Job/quota/thread/process residue or
`WorkerStartup`, resource-over-bound typed failure, data-plane setup rollback
without a path or stage, isolated finite-hard-`RLIMIT_FSIZE` pre-fork rejection
without process/quota/artifact residue, and
retry/restart/new-Job preservation of checkpoint identity, digest, durability,
and quota truth,
concurrent shutdown drainage, actual first completion/reconstruction allocation
fail-stop, completion-callback exception fail-stop, and real Embedded Host
output/checkpoint/restart behavior.

Issue #106 adds a pure product `decode_worker_assignment` boundary and a manual
opt-in local parser robustness harness for bounded worker control metadata.
Successful decoding must re-encode to the same canonical frame; the socket
receiver delegates to that decoder and retains its absolute acceptance
deadline. The harness transfers no artifact bytes and creates no descriptor,
worker, process, quota, receipt, or publication authority. Registered tests,
not the manual harness, own deterministic canonical, prefix, truncation,
trailing-byte, identity, digest, descriptor, and heartbeat behavior.

This local Issue #99/#100/#105 executable subset plus the Issue #106 validation
and observation layer does not add the network/multi-tenant control plane, a
separately deployed WorkerManager or artifact service, remote/authenticated
data-plane capabilities, an untrusted plugin sandbox, or Issue #125 Metal work.
Those broader boundaries remain governed by
[ADR 0011](../adr/0011-server-control-plane-workers-and-plugin-runtimes-are-separate-security-domains.md)
and the [server roadmap](../roadmap/Kernel-Evolution.md#server-and-plugin-isolation).
