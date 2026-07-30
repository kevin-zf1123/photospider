# ADR 0009: Compute I/O Durability and Completion Are Separate Contracts

## Status

Accepted as the target contract for Issue #87. Issue #87 remains open for
independent audit, and its OpenSpec change remains active and unarchived.

This ADR is a decision and documentation change. It does not add
`ComputeIoExecutor`, change protocol v2 or the installed ABI, make current
Graph/cache writers atomic, or turn the current private IPC `OutputStore` into
a crash-durable store. Issue #88 consumes only the bounded execution boundary;
later focused changes must implement durable output commit, Graph-document
transactions, and legacy output-side-effect migration.

## Context

PhotoSpider already has several valid completion and persistence mechanisms,
but each answers a different question:

- `ComputeRun::Succeeded` means a validated Graph/RT publication, or a
  validated no-op, won the Run terminal arbiter. A provider returning is not
  sufficient.
- `GraphCacheService` writes configured image and metadata paths directly.
  One cache entry is not published transactionally, and the current product
  commit policy can fail a Run when deferred cache persistence fails before
  visible Graph publication.
- `ImageArtifactCodec` and `CacheMetadataCodec` own representation conversion,
  not directory creation, path authority, atomic replacement, retry,
  visibility, or durability.
- Graph-document load validates a detached definition before replacing the
  in-memory Graph. Current YAML save emits before open but writes the
  destination stream directly; failure after open can leave a created,
  truncated, or partial document.
- Protocol-v2 daemon jobs are bounded process-local polling records. Graceful
  shutdown drains accepted work, but a daemon crash loses queued, running, and
  terminal records.
- The private IPC `OutputStore` writes a same-owner mode-`0600` stage, calls
  file `fsync`, performs a no-replace atomic rename, validates identity, and
  then publishes an in-memory leased record. It does not synchronize the
  containing directory or persist the record/index, and TTL/lease cleanup
  intentionally removes artifacts.
- The legacy `io/save` operation calls `cv::imwrite` inside provider execution.
  Its user-selected path can become visible before the enclosing Run commit
  and cannot be rolled back if cancellation or another terminal claimant later
  wins.

Calling all of those states “complete” or “saved” would let a worker-pool
choice accidentally define transaction ownership. Issue #87 therefore has to
freeze authority and failure ordering before Issue #88 introduces bounded
compute-I/O execution.

This ADR follows:

- [ADR 0003](0003-process-owned-execution-resources.md), which assigns physical
  execution resources to the process;
- [ADR 0006](0006-kernel-documentation-separates-facts-decisions-targets-and-status.md),
  which forbids presenting target behavior as current fact;
- [ADR 0007](0007-compute-runs-and-process-execution-have-separate-owners.md),
  which assigns Run terminal and visible Graph commit authority; and
- [ADR 0008](0008-generic-values-memory-bindings-and-regions-are-explicit-versioned-contracts.md),
  which separates logical values, runtime bindings, artifact identity, and
  persistence layers.

## Decision

### Completion is a typed partial order

The target contract distinguishes these observable facts:

| Fact | Owner and meaning |
| --- | --- |
| `RequestAccepted` | An owner admitted a request and returned its stable request identity. |
| `OperationReturned` | A provider or codec call returned without an immediate error; native work can still be pending. |
| `ValueReady` | Every producer fence needed to consume a value is terminal and successful. |
| `RunTerminal` | The `ComputeRun` arbiter published exactly one success, failure, or cancellation. |
| `ResultAvailable` | A retained snapshot or delivery lease can be obtained. |
| `OutputCommitted` | Output authority crossed its requested atomic visibility and durability point and can return a stable receipt. |
| `OutputCommitFailed` | The output transaction reported a typed encode, staging, or requested-durability commit failure; it returned no receipt claiming the requested durability. |
| `GraphDocumentSaved` | An independent document transaction published a version and reports its achieved durability. |
| `ResponseObserved` | A client received an acknowledgement; response loss does not reverse an earlier server transition. |

These facts form an explicit partial order, not aliases. `OperationReturned`
does not imply `ValueReady`; readiness does not imply Run success; Run success
does not imply cache save, output commit, Graph-document save, daemon result
availability, or response observation.

The legal Run branches are different partial orders:

- a successful value-producing Run observes successful operation return,
  successful producer-fence completion, `ValueReady`, validated Graph/RT
  publication, and then `RunTerminal(Succeeded)`;
- an operation, readiness, dependency, or pre-terminal `ComputeRun`
  result-commit failure can publish its typed failure and proceed directly to
  `RunTerminal(Failed)`, without fabricating `ValueReady` or
  `OutputCommitted`. Here a result-commit failure is limited to this Run's
  Graph/RT validation, publication, or commit resolution; it does not include
  a later `OutputStore` transaction;
- cancellation that wins the Run arbiter proceeds to
  `RunTerminal(Cancelled)`. Late or stale provider/fence completions perform
  cleanup only and cannot publish a value or durable-output receipt; an output
  receipt committed independently before a later cancellation remains
  authoritative for that output transaction; and
- an admitted empty-plan, zero-work, or otherwise validated no-op can proceed
  from no-work validation directly to `RunTerminal(Succeeded)`. Reusing an
  already complete result does not fabricate a new `OperationReturned` or
  `ValueReady` fact or a durable-output receipt.

`ComputeRun::Succeeded` retains the ADR 0007 meaning: validated Graph/RT
publication or a validated no-op won. A future `compute-and-persist` or export
API is a named composite operation. It exposes both outcomes and succeeds only
when every outcome it promised succeeds; it does not broaden the Run terminal.
Target post-Run cache, codec, and output work has its own typed outcome and
must neither delay nor rewrite an already published Run terminal.
An output failure after Run terminal publishes `OutputCommitFailed`, not
`RunTerminal(Failed)`, and neither creates nor revokes `ValueReady`. A caller
or daemon may report a composite request failure, but it must preserve the Run
terminal and the output, document, cache/codec, and response facts underneath;
it cannot project the aggregate result back into Run state.

### Persistence classes have one authority each

| Persistence class | Authority | Contract |
| --- | --- | --- |
| Graph document | Graph-document transaction owner | User-authored topology/configuration, versioned independently from Runs and cache. |
| Cache artifact | Cache policy/service | Rebuildable acceleration; absence or failure is not user-output truth. |
| Codec result | Calling operation | Representation conversion only; no transaction or durability policy. |
| Daemon job/response | Daemon registry/transport | Process-scoped control and delivery state, not durable storage. |
| User-visible compute output | `OutputStore` transaction | One atomic committed output identified by a stable commit id and receipt. |

The current private IPC `OutputStore` is a protected delivery store, not yet
the target durable authority. A later implementation can evolve it or
deliberately introduce a renamed durable owner, but it must leave only one
output commit authority. Delivery leases and TTL cleanup remain separate from
durable retention.

Cache paths, daemon artifact paths, delivery ids, Run ids, output commit ids,
and Graph-document versions are different identities. Equal payload bytes do
not make them interchangeable.

### Failure propagation follows authority

Required source-asset acquisition, decode, producer readiness, and execution
can fail a Run because a valid staged value cannot be produced. Ordinary cache
absence, incompatibility, corruption, or I/O failure is a structured cache
diagnostic and becomes a miss when authoritative recomputation is allowed.
Security-policy violations and resource exhaustion remain typed failures.

The target moves cache writes after visible Graph publication or into an
independent cache-persistence stage. Cache write failure then records a cache
failure without rolling back or replacing an already successful Run or output
commit.

Output encode, staging, or durable commit failure belongs to the output
transaction and reports `OutputCommitFailed` without producing an
`OutputCommitted` receipt at the requested durability. Graph-document save
failure belongs to the save transaction. Cache write/codec failure belongs to
cache policy or the calling operation, and daemon response loss changes only
what the client observed. None of those failures delays, rolls back, or
rewrites a previously published Run terminal.

The current product differs: deferred cache persistence can fail before
visible Graph publication. This is a documented migration gap, not evidence
that cache is output authority.

The legacy `io/save` provider callback is another migration exception. Target
provider work produces a staged output intent/value; only `OutputStore`
orchestration can publish caller-visible output after the Run result is known.
The direct side-effect path must not be expanded or treated as a durable commit
surface.

### OutputStore uses manifest-last idempotent commit

Every target output transaction receives a stable `OutputCommitId` before a
transport response can become ambiguous. It binds one namespace, output slot,
representation descriptor, content identity, committed version, and retention
policy.

The authority performs this protocol:

1. validate the rooted namespace, requested durability, quota, descriptor,
   content identity, and commit id;
2. reconcile idempotency: return the original receipt for the same committed
   identity, and reject the same key with different content;
3. create private same-root stages for immutable payload/chunk data, metadata,
   and every transaction-owned temporary file;
4. completely write every payload and metadata file, synchronize each file,
   and then revalidate its exact length, digest, filesystem identity,
   `OutputCommitId`, committed version/generation, and content binding. Any
   failure before manifest publication leaves no published manifest or
   receipt;
5. encode the complete canonical manifest into a unique private stage, write
   all manifest bytes, and validate the stored canonical content, exact
   reference set, payload lengths/digests, and
   `OutputCommitId`/version/generation/content bindings. Synchronize the
   manifest file itself before publication;
6. use a platform-supported atomic no-replace operation to publish the final
   manifest/commit record as the sole multi-file visibility point, then
   revalidate the published identity;
7. for crash durability, execute a durability barrier for every directory
   created, renamed, or modified by the transaction, ordered from the leaf
   directory through the configured durability root. Each barrier is directory
   `fsync` or a documented platform-equivalent mechanism;
8. persist and return an `OutputCommitReceipt` only after every barrier required
   by the requested durability class succeeds; and
9. on recovery, recognize committed manifests, reconstruct the commit index,
   and conservatively remove or quarantine incomplete stages and orphans.

The receipt identifies commit, descriptor/content, namespace, version, and
achieved durability. It is not a mutable cache or staging path. The default
policy never overwrites a committed output; replacement uses an explicit new
version/commit identity.

The achieved durability is typed. An explicitly requested atomic-visible
transaction can return only an atomic-visibility receipt after the no-replace
manifest publication and identity validation. A crash-durable receipt is
available only after the manifest file and all referenced files are
synchronized and the complete leaf-to-root directory barrier succeeds. A
platform/filesystem without the required file synchronization, directory
barrier, or atomic no-replace publication reports a typed unsupported or
transaction failure and never labels the weaker result crash durable. A crash
durability failure after atomic visibility produces no crash-durable receipt;
retry with the same commit identity reconciles that state idempotently.

Delivery is at-least-once and commit is idempotent. A response lost after the
commit point is resolved by querying or repeating with the same commit id.
The store returns the same receipt, resumes a recoverable pending transaction,
or reports a typed conflict/failure. It does not claim exactly-once transport
and does not create a second committed output for the same key.

Before the manifest commit point, cancellation or failure revokes publication
permission. Late codec/I/O work can finish only a private stage, followed by
identity-safe cleanup. After the commit point, cancellation is a no-op for
that transaction and cannot relabel or delete the committed output. Recovery
reconstructs receipts from committed manifests and conservatively cleans or
quarantines incomplete stages.

### Graph-document save is a separate versioned transaction

Graph documents contain authored topology/configuration and exclude runtime
output, cache entries, native bindings, delivery leases, and output receipts.
The target save transaction:

1. captures one Graph revision and expected document version;
2. serializes and validates the complete detached document before destination
   mutation;
3. resolves a normalized destination below a caller-authorized root;
4. writes and validates a private same-directory stage;
5. synchronizes it for the requested durability;
6. atomically replaces only the expected document version;
7. synchronizes the directory when crash durability was requested; and
8. returns a typed save receipt with document/version identity and achieved
   durability.

A stale expected version fails rather than overwriting a concurrent save.
Platforms/filesystems that cannot provide the requested atomic replacement or
durability report a typed unsupported capability. A caller can explicitly
request a weaker process-visible level, but the implementation never silently
labels direct stream close as durable.

Graph-document save is never an implicit phase of `ComputeRun`.

### Daemon states remain transport states

The daemon can own accepted, queued, running, terminal, result-available, and
response states. A job becomes terminal only after the work promised by its
result mode finishes, but its terminal name does not imply crash-durable output
unless a separate output receipt exists.

Graceful shutdown continues to stop admission and drain accepted jobs. A crash
can lose the process-local registry. Recovery uses stable domain identities and
`OutputStore`; it does not promote the daemon registry to a write-ahead log or
automatically replay a mutation that has no idempotency key.

Protocol v2 remains unchanged by Issue #87. A later versioned protocol can
expose an independent output transaction and receipt when remote callers need
durable commit.

### ComputeIoExecutor owns bounded mechanism, not policy

Issue #88 can add one process-owned `ComputeIoExecutor` for cache read/write,
asset fetch, and codec subwork. Admission is bounded by both operation count
and estimated bytes. Each task retains its Run/transaction lifetime token,
returns a typed result or readiness fence, and cannot mutate Graph state or
cross a persistence visibility point. CPU-heavy codec work returns to an
admitted CPU lane instead of hiding unbounded compute on I/O workers.

Graph-document transactions, daemon sockets/polling, and `OutputStore`
validation, commit, receipt, retention, and recovery remain with their domain
owners. Those owners can submit bounded byte-transfer or codec subwork, but the
executor never chooses paths, retry, overwrite, idempotency, retention, commit,
or durability policy.

One generic pool for every filesystem and socket operation is rejected because
it would combine unrelated lifecycles and make a worker mechanism an accidental
transaction owner.

### Security and durability capability are explicit

Persistence owners resolve normalized paths below caller-authorized roots,
prevent symlink escape, create private same-directory stages without following
links, verify filesystem identity, and account for in-flight and retained
quota. Untrusted plugins/codecs receive stage access only, never publication
authority.

Receipts distinguish at least:

- process-visible atomic publication; and
- crash-durable commit with every requested and supported data/directory
  barrier complete.

File sync, directory sync, atomic rename, remote filesystem behavior, hardware
write caches, and platform support are not assumed equivalent. An unsupported
requested guarantee fails explicitly rather than returning a stronger label
than the implementation can prove.

## Consequences

- Callers see more explicit states, but can no longer mistake cache save,
  daemon acknowledgement, or file rename for durable user output.
- A successful Run can legitimately have a failed or absent optional cache.
  Structured cache diagnostics and recomputation preserve correctness.
- Durable output requires retained idempotency metadata, manifest recovery,
  directory barriers, and explicit garbage collection; this costs storage and
  sync latency.
- Delivery leases can expire without deleting durable output. Retention becomes
  an explicit output policy instead of an IPC side effect.
- Graph-document save gains optimistic versioning and a typed durability
  result, but remains independent from compute frequency and runtime state.
- The existing `io/save` operation, direct cache writers, direct YAML writer,
  and private IPC store remain current facts and documented migration gaps;
  accepting this ADR does not silently repair them.
- Future tests must validate software behavior: bounded admission, failure
  independence, manifest-last visibility, idempotent ambiguity recovery,
  cancellation ordering, recovery, durability capability, and stale document
  versions. Issue-specific scans or orchestration do not enter CTest/CI.

## Rejected Alternatives

### Make `ComputeRun::Succeeded` mean that everything is persisted

Rejected because cache, Graph state, authored documents, and caller-selected
outputs have different authorities, retention, retries, and failure timing.

### Treat cache or the leased daemon artifact as the durable output

Rejected because both are intentionally evictable/backend-owned and neither
currently has a persistent commit receipt or recovery index.

### Claim exactly-once because rename is atomic

Rejected because response delivery can be ambiguous, multi-file output needs a
separate commit marker, and atomic visibility is not filesystem durability.

### Put Graph-document save inside every Run commit

Rejected because runtime output is not authored Graph state, and Run frequency
must not create stale document overwrite or storage latency in graph-state
commit.

### Route all I/O through one generic executor

Rejected because executor admission is a mechanism, while path, transaction,
retry, overwrite, commit, retention, and durability are domain policies.

### Silently downgrade unsupported durability

Rejected because a successful but mislabeled receipt is worse than a typed
unsupported-capability failure.

## Relationship to Current Facts and Evolution

Current behavior remains authoritative in:

- [Kernel Data Model](../kernel-architecture/Data-Model.md);
- [ImageBuffer Memory Contract](../kernel-architecture/ImageBuffer-Memory-Contract.md);
- [Compute Boundaries](../kernel-architecture/Compute-Boundaries.md);
- [Compute Flow](../kernel-architecture/Compute-Flow.md);
- [Policy and Execution Architecture](../kernel-architecture/Policy-and-Execution-Architecture.md);
- [Kernel Terminology](../kernel-architecture/Terminology.md); and
- [Kernel Cache Model](../kernel-architecture/Cache-Model.md).

The accepted target and dependency sequence live in
[Kernel Evolution Roadmap](../roadmap/Kernel-Evolution.md) and the active
OpenSpec change
`decide-compute-io-durability-and-completion-semantics`. Live Issue and Project
state remain the delivery-status authority.
