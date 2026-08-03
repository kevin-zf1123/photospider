# ADR 0010: Execution-Profile SLOs Are Six Independent Benchmark Verdicts

## Status

Accepted as the target contract for Issue #92 and roadmap key S-1. This ADR
freezes the `execution-profile-slo-v1` workload, measurement, evidence, and
verdict contract consumed by Issues #93 through #96.

Issue #92 is a decision and documentation change. It does not add a runtime
profile enum, public API, benchmark result field, collector, workload harness,
CTest entry, CI performance gate, or current-behavior claim. The downstream
issues must implement and validate their assigned evidence rows before those
targets can be promoted into current architecture documentation.

## Context

The process execution domain already distinguishes Interactive and Throughput
QoS. Current Host policy applies deadline preference, trusted work/ready-byte
charging, hierarchical Graph/Run fairness, eight-dispatch aging, at most three
Interactive starts before a Throughput start while both remain startable, and
Interactive admission headroom. Those are ordering and admission mechanisms,
not end-to-end execution-profile SLOs.

Current measurement evidence is narrower:

- `BenchmarkService` repeats Host compute and reports mean wall time, a trimmed
  operation-time mean, mean I/O time, and resolved Run parallelism. It has no
  warmup, percentile, stable output digest, completed-work window,
  discarded-work counter, or memory high-water contract.
- `opencv_operation_concurrency_benchmark` is a maintained manual tool with
  warmups, raw wall samples, median MPix/s, speedup, and maximum callback
  overlap for one synthetic OpenCV graph. Its recorded snapshot is not a
  permanent performance baseline.
- maintained tests prove exact callback overlap for Run caps 1/2/4/8, bitwise
  equality for one cap-1/cap-8 fixture, deterministic policy ordering, the
  three-to-one class-start bound, headroom accounting, cancellation isolation,
  and exact resource release.
- `ExecutionLifecycleTelemetry` retains bounded transition records,
  service-relative monotonic timestamps, identities, and lifecycle counters.
  It does not expose queue wait, completed work, Host/device bytes, result
  digests, or a lossless history; a cursor gap invalidates reconstruction.
- `ResourceLedger` exposes current/limit snapshots for authoritative Host and
  configured-device dimensions, but the current public benchmark path does not
  retain per-workload high-water samples.

[ADR 0006](0006-kernel-documentation-separates-facts-decisions-targets-and-status.md)
requires those current facts to stay separate from this target and from live
Issue state. [ADR 0003](0003-process-owned-execution-resources.md) and
[ADR 0007](0007-compute-runs-and-process-execution-have-separate-owners.md)
require isolated and mixed rows to use the same process-owned execution
authority rather than profile-specific hidden pools.
[ADR 0009](0009-compute-io-durability-and-completion-semantics.md) requires B1
throughput to wait for its independently required artifact commit instead of
treating provider return as completion.

## Decision

### One exact graph family underlies four immutable workloads

The v1 source is a generated 2048x2048 RGBA FP32 value. For zero-based
coordinates `x`, `y`, channel `c`, and an unsigned eight-bit seed, each stored
binary32 sample is the round-to-nearest-ties-to-even value of:

```text
((17*x + 31*y + 47*c + seed) mod 256) / 255
```

The graph applies four sequential repository
`image_process:curve_transform` nodes with baseline `k` values `0.80`, `1.00`,
`1.20`, and `1.40`; node four is the target. The exact repository OpenCV HP
tiled provider executes those nodes on Host CPU. One logical site-operation is
one transform of one RGBA pixel site, independent of channel count. The
generated source bytes, normalized graph and parameter values, selected
operation/provider binary and generation, and all later payloads are
content-addressed in the workload manifest.

The canonical workload matrix is:

| Workload | Frozen behavior |
| --- | --- |
| `I1-edit-storm-v1` | Uses seed zero and the twelve natural edit ordinals `1..12`. For `edit_index = edit_ordinal - 1` in `0..11`, node one's `k` is selected from `[0.82, 1.18, 0.86, 1.14, 0.90, 1.10, 0.94, 1.06, 0.98, 1.02, 0.96, 1.04]`, and the source Region is `(256*(edit_index mod 4), 256*floor(edit_index/4), 256, 256)`. Every Run uses `ComputeIntent::GlobalHighPrecision`, `ComputeRunQuality::Full`, Interactive QoS, weight 1, Run cap 8, a 150 ms relative monotonic deadline, and the exact `(Graph, target node four, GlobalHighPrecision)` supersession key. Only the twelfth edit (`edit_index=11`, `k=1.04`, Region `(768,512,256,256)`) must publish; it receives a 500 ms drain. |
| `I2-progressive-v1` | Reuses the exact I1 source, graph, seed, edit ordinals, source-space Regions, and realtime request lineage. The 512x512 preview source is a per-channel 4x4 box average of the 2048 source, rounded once to binary32 before the same four transforms; preview Region `edit_index` is `(64*(edit_index mod 4), 64*floor(edit_index/4), 64, 64)`. The final evaluates the 2048 source. Only the twelfth edit (`edit_index=11`, preview Region `(192,128,64,64)`) has required preview and final latency results, in that order; stale output cannot publish. |
| `B1-immutable-v1` | Contains immutable jobs `0..29`; job `n` uses source seed `n`, the baseline graph, Throughput QoS, weight 1, no deadline or supersession, exact reservation evidence, a canonical semantic trace, a crash-durable committed artifact, and job-indexed logical/raw goldens. Even jobs belong to Graph A and odd jobs to Graph B. At the measurement boundary the harness offers both ordered 15-job queues and never pauses a nonempty queue; bounded Host admission, rather than the harness, decides how many Runs are resident. Run caps 1 and 8 are separate required rows. |
| `M1-shared-v1` | At measured time zero, starts I1 and then repeats it every 750,000,000 ns, giving exactly 40 episode starts, while cycling the exact B1 corpus with its even/odd Graph assignment, Run cap 8, and continuous offered backlog for 30 measured seconds. Both streams use one `ExecutionService`, worker set, ready store, policy binding set, and `ResourceLedger`; no hidden pool, duplicate ledger, or separate process may absorb either stream. |

For fairness, one Graph is *eligible* while its producer has unconsumed offered
demand and has not paused submission. This workload-level interval includes
time awaiting bounded admission; it does not claim that all 30 B1 Runs are
admitted simultaneously. Within each Graph the producer offers jobs in
ascending index order and synchronously offers the next job when the prior one
becomes terminal. M1 starts a new `0..29` cycle without a producer-side gap.

#### Edit ordinals and monotonic cadence are exact

Natural-language edit numbers always mean `edit_ordinal` in `1..12`; formulas,
arrays, Regions, lineage records, and evidence use zero-based `edit_index` in
`0..11`, with `edit_index = edit_ordinal - 1`. A bare phrase such as “edit 12”
is not a v1 identifier. The required final is always written as “the twelfth
edit (`edit_index=11`)”. Its coefficient, source Region, preview Region,
lineage, logical digest, metric sample, and evidence record all carry that same
index.

For one episode, the harness chooses monotonic origin `E` only after the reset
baseline is materialized and settled. The nominal admission-call start for
`edit_index=i` is:

```text
S_i = E + i * 16,666,667 ns,  i in 0..11
```

The harness must not start the Host admission call before `S_i`. Its recorded
actual start `A_i` must satisfy `S_i <= A_i <= S_i + 2,000,000 ns`; this is a
bounded start-lateness rule, not a claim that an operating system wakes at an
exact nanosecond. An early start, a start more than 2 ms late, an admission
failure, a dropped edit, or a cadence-event gap invalidates the replicate. A
missed edit is not submitted late: the harness cancels the rest of that
episode, records the missed/drop/gap facts, and never catches up, backfills, or
shifts later nominal times.

Within each cold, warmup, or measured phase, episode origins are exactly
`E_r = E_0 + r * 750,000,000 ns`. Reset/baseline preparation must finish before
`E_r`; failure to do so invalidates that episode rather than sliding the
schedule or inserting an unrecorded cooling delay. M1 uses
`E_r = M_0 + r * 750,000,000 ns` for `r=0..39`. Paired isolated and mixed
evidence therefore share the same v1 schedule, start-lateness bound, and
miss/drop/gap rules without claiming identical physical wake times.

#### I2 freezes one target progressive state machine

This state machine is target benchmark-harness semantics assigned to #94. It
does not claim that #92 adds a current public API or that current callers
already expose progressive publication. “Same I1 lineage” means the same
Graph/target/revision and ordinal-to-generation mapping; it does not reuse I1's
`GlobalHighPrecision` canonical request key. For each `edit_index`, the target
harness mints the next nonzero generation under the legal realtime request key
`(Graph, target node four, ComputeIntent::RealTimeUpdate)`. It immediately
starts one preview child with `ComputeIntent::RealTimeUpdate`,
`ComputeRunQuality::Interactive`, Interactive QoS, weight 1, Run cap 8, and an
absolute steady-clock deadline 100 ms after that edit's recorded Host admission
start. It also arms one final child with `ComputeIntent::GlobalHighPrecision`,
`ComputeRunQuality::Full`, Interactive QoS, weight 1, Run cap 8, and an absolute
deadline 1,000 ms after the same start. Both children carry the realtime request
`SupersessionIdentity`, so the HP child's compute intent remains distinct from
its canonical request key as required by the current `ComputeRunSubmission`
contract.

The final child is submitted exactly when that edit's preview first becomes
visible and its generation is still current. If a newer edit is accepted first,
the armed final is discarded without submission; an already submitted preview
or final is superseded and may only drain. A new generation revokes publication
permission for both older children. A stale terminal event may update cleanup,
waste, and quiescence evidence, but cannot publish a `Value`, digest, receipt,
or required latency result. The twelfth edit (`edit_index=11`) must publish its
preview and then its final; failure, deadline expiry, reverse order, duplicate
publication, or a newer generation before both endpoints makes the episode
invalid. Earlier generations may publish only while current and are not
required results.

Preview latency starts immediately before the preview Host admission call and
ends at current preview visibility. Final end-to-end latency uses that same
start and ends at current final visibility; the later final Host admission time
is retained separately as a diagnostic trigger timestamp. Thus the final gate
includes preview, trigger, admission, execution, and publication rather than
hiding the preview interval.

I2 has a required Host-local output path and a conditional Metal residency
component. Preview and final each expose their immutable CPU
`ValueRevisionId`, Host binding/allocation identity, and storage bytes twice to
the same local consumer; both acquisitions must reuse the same binding with no
CPU copy. When `DeviceId(DeviceBackend::Metal, 0)` exists, the first access for
each distinct preview/final revision may perform one exact-size Host-to-Metal transfer and
the second must reuse the same device-local residency with zero transfer or
allocation. Metal-to-Host transfer, filesystem/codec I/O, and any transfer
beyond those two conditional first accesses are forbidden. Without Metal only
the device-specific component is predefined `not-applicable`; the Host reuse
and no-I/O gates still apply. The twelfth edit (`edit_index=11`) final logical
digest must equal the I1 `edit_index=11` digest, and its preview logical digest
must equal its own fixture golden.

Every required logical output digest is obtained by calling
`compute_content_digest(Value)`. A sample is valid only when the returned
`ContentDigestResult.state` is `ContentDigestState::Available`, `digest` is
present, and `digest->algorithm` is
`CanonicalDigestAlgorithm::Sha256CanonicalV1`. Evidence stores the algorithm
tag and lowercase hexadecimal `ContentDigest.bytes`. Any other state, absent
digest, provider/readiness failure, or different algorithm makes the affected
row `invalid`. This canonical logical `ContentDigest` is not the SHA-256 of an
artifact's raw bytes.

Each B1 job commits two files below a fresh disposable job directory. The
payload `output.rgba32le` is tightly packed row-major RGBA with little-endian
IEEE-754 binary32 samples. `manifest.txt` is UTF-8 without BOM, uses LF
including after its final line, and contains exactly these fixed-order fields:

```text
schema=execution-profile-artifact-v1
job=<unpadded decimal 0..255>
width=2048
height=2048
channels=RGBA
scalar=ieee754-binary32
byte-order=little
row-stride=32768
payload=output.rgba32le
payload-sha256=<lowercase 64-hex SHA-256>
```

For v1, the payload byte count is checked as
`2048 * 2048 * 4 * 4 = 67,108,864`. For every valid job `0..255`, the exact
manifest length is `242 + decimal_digit_count(job)` bytes: 243 bytes for jobs
`0..9`, 244 bytes for jobs `10..99`, and 245 bytes for jobs `100..255`.
Consequently, measured jobs `0..29` use 243 or 244 bytes, while the cold/warmup
jobs `252..255` use 245 bytes. Each job's target durable-output owner uses the
process-owned `ComputeIoExecutor` for two ordered tasks with stable charge
identities `(job, payload-stage, attempt)` and
`(job, manifest-commit, attempt)`. The payload-stage task declares
`planned_bytes=67,108,864`; the manifest-commit task declares `planned_bytes`
equal to that job's exact manifest length.
Checked arithmetic must produce those values before every `try_submit`, and
all attempts for one identity must use the same charge. The 64-task and
268,435,456-byte summed-planned-byte limits apply at every accepted admission.

`planned_bytes` is a stable admission estimate of task-retained bytes. It is
mandatory and authoritative evidence for Compute I/O admission, snapshot
high-water, and final settlement, but it is not a measurement of physical
allocation or proof of memory ownership. It does not replace process RSS or
the ledger/device ownership evidence. Capacity rejection leaves the already
offered job eligible and the same task pending; every admission attempt and
typed status is retained. In fault-free B1 each task may be accepted and
started only once, and no output retry, duplicate task, or changed charge
identity is permitted.

The payload-stage task completely writes, hashes, synchronizes, and revalidates
the private payload stage before it settles. Only then may manifest-commit
write and synchronize the private canonical manifest, atomically publish it
with no replacement, revalidate the published identity, and execute every
leaf-to-root directory barrier. B1 requests typed `crash-durable` durability
and accepts only typed achieved `crash-durable`; atomic visibility alone is not
success. Unsupported file synchronization, directory barriers, atomic
no-replace publication, a weaker achieved class, or any transaction failure
makes the job invalid and yields no successful crash-durable receipt.

The `OutputCommitReceipt` evidence binds at least the stable `OutputCommitId`,
rooted namespace/output slot, job index, descriptor and logical content
identity, committed version/generation, payload and manifest names, exact byte
counts and raw SHA-256 values, requested and achieved durability, and the
published manifest identity. It is returned only after all requested barriers
succeed. This is ADR 0009's target `OutputStore` authority, not the current
private IPC delivery store and not an expansion of #92 runtime behavior.

Every B1 artifact destination, whether an explicitly disposable path or
release-artifact storage, must be below one selected `OutputStore` root or
rooted namespace whose requested `crash-durable` capability succeeds. A remote,
RAM-backed, copy-on-write, or otherwise nonlocal root is not rejected by name,
but it is not presumed durable or comparable. The bundle retains the selected
root/path spelling, resolved root and mount identity, and proof that every job
directory is below that root. Those path facts are audit evidence; a transient
absolute path or fresh job-directory name is not the compatibility key.

Each B1 or M1 row sets `storage_environment_applicability=required` and records
one normalized, hashable `execution-profile-storage-environment-v1`
fingerprint. It contains at least:

- the `OutputStore` provider/backend identity and its applicable generation or
  version;
- backend class, explicit local/remote locality, and volatile/nonvolatile
  persistence class;
- filesystem type, stable mount identity, and the normalized mount options and
  semantics that affect file sync, directory sync, atomic no-replace, rename,
  barriers, and copy-on-write behavior;
- the durability capability set plus requested and provably achieved
  durability classes;
- a stable backing volume/device/storage identity and storage class, or the
  provider-specific equivalent; and
- hardware write-cache and power-loss-protection policies, each with an
  explicit known, unknown, or schema-defined not-applicable state.

Every required fact is a typed observation with state `known`,
`not-applicable`, `unknown`, `unobserved`, `unsupported`, or `unprovable`.
`not-applicable` is valid only with a schema-defined reason and evidence that
the layer is outside the end-to-end durability path. The normalized object is
retained in raw evidence, and its canonical
`execution-profile-storage-environment-v1` serialization is hashed with
SHA-256 into lowercase `storage_environment_digest`. A fingerprint is
compatibility-eligible only when all required facts are `known` or justified
`not-applicable`, the capability set proves the required operations, and both
requested and achieved durability are `crash-durable`. Equal `unknown`,
`unobserved`, `unsupported`, or `unprovable` states never become compatible by
having equal bytes or equal digests.

The raw payload SHA-256 hashes the exact 67,108,864 little-endian bytes; the
manifest SHA-256 hashes that job's exact
`242 + decimal_digit_count(job)` canonical bytes (243, 244, or 245 bytes over
the valid range). The job-indexed golden fixture separately binds the expected
typed logical `ContentDigest`, expected raw-payload SHA-256, and its own
content-addressed golden identity. These three identities are never substituted
for one another.
One B1 job reaches its unique throughput completion endpoint only after Run
success, a valid crash-durable receipt, and successful comparison with both
goldens. The isolated interval ends at the last job's golden-verification
completion, not at provider return, Run terminal, payload close, manifest
rename, or atomic visibility.

The first downstream fixture implementation may only materialize and hash these
choices. Changing a source formula, operation, coefficient, edit, preview
filter, Graph assignment, cadence, required output, or semantic manifest
creates a new workload id. Existing v1 evidence is never overwritten.

### The reference protocol separates cold, warm, and mixed evidence

Every row uses three fresh process/execution-domain replicates. Before warmup,
each replicate records and freezes:

- repository commit and dirty-state declaration, build type, compiler, flags,
  OS/kernel, CPU/GPU/device inventory, and power/thermal eligibility;
- provider/plugin binary hashes and generations, fixed process worker count,
  Run caps, workload/fixture hashes, seeds, cache and residency preconditions;
  and
- all resource limits and Interactive headroom; and
- for B1 and M1, the selected `OutputStore` root evidence, normalized storage
  fingerprint, `storage_environment_digest`, compatibility eligibility, and
  raw capability observations.

The v1 resource configuration is 32 CPU slots, 1 GiB Host retained memory,
512 MiB Host scratch, 65,536 ready entries, 256 MiB ready bytes, and
Interactive headroom of one CPU slot, 64 MiB retained memory, 32 MiB scratch,
1,024 ready entries, and 16 MiB ready bytes. Compute I/O admission is limited
to 64 tasks and 256 MiB of summed planned bytes. When Metal is configured, its
device-memory and scratch limits are 512 MiB and 256 MiB. Absent Metal is predefined
`not-applicable`, not a zero observation.

B1 evidence samples `ComputeIoExecutor::snapshot()` immediately after every
accepted task admission and every task settlement, with an initial pre-row
sample and a final post-quiescent sample. It retains the task charge identity,
planned bytes, admission status, completion status, active-task count, and
active-planned-byte count. Every active-planned-byte total is the checked sum
of the true per-job charges, and its high-water is the maximum of this complete
event-aligned stream. Any missing sample, arithmetic inconsistency, value over
the frozen limit, or nonzero final count makes the row invalid. The final
snapshot must be exactly zero for both active tasks and active planned bytes.

Disk-cache/codec I/O and cross-episode/job result reuse are disabled for cold,
warmup, and measured work. I1/I2 retain only the explicitly recomputed baseline
and current episode target plus I2's declared output residency; each B1 job
starts without a reusable result for its fixture identity. The cold and warmup
observations are exact rather than harness choices:

| Workload | Cold diagnostic | Warmup | Measured evidence per replicate |
| --- | ---: | ---: | ---: |
| I1 | 1 episode | 20 episodes | 200 episodes |
| I2 | 1 episode | 10 episodes | 100 episodes |
| B1 | seed 252 job per required Run cap | seed 253, 254, and 255 jobs per required Run cap | jobs `0..29` per required Run cap |
| M1 | 1 mixed second | 5 mixed seconds | 30 non-overlapping one-second windows |

Warmup B1 jobs use the same graph and full artifact path but warmup-only
identities and directories. Warmup and cold output is removed after its owner
settles; process/provider/JIT state remains. Measurement counters reset at the
boundary without restarting the process, and M1 restarts its cadence with the
first episode at measured time zero. Cold first use is retained separately and
never pooled into steady-state aggregates. All durations use a monotonic
clock. Percentiles use nearest rank: sort `N` samples and select one-based rank
`ceil(p*N)`. Every replicate must pass independently; pooling cannot hide a bad
process. A summary may report the median of the three replicate aggregates.

### Each SLO dimension has a non-substitutable verdict

Each required dimension emits `pass`, `fail`, `invalid`, or a schema-defined
`not-applicable`. Missing evidence, checked-arithmetic overflow, monotonic-clock
failure, telemetry gaps or drops, fixture/environment drift, an unpinned or
incompatible reference, and an unapproved `not-applicable` make the row
`invalid` and non-conformant. No composite score, average, start count, RSS
sample, or faster dimension may replace another verdict.

#### Latency

I1 latency starts immediately before the final edit's Host admission and ends
when its matching current generation becomes visible. I2 uses the two explicit
start/end boundaries in its state machine above.

- I1 final-generation p50/p95/p99 must be at most 50/100/150 ms, with every
  measured episode publishing its final generation.
- I2 twelfth-edit (`edit_index=11`) first-preview p50/p95/p99 must be at most
  50/75/100 ms; twelfth-edit final p95/p99 must be at most 500/1000 ms. Both
  endpoints must match their required logical `ContentDigest`.
- M1 must satisfy the I1 absolute limits, and its p99 must be no more than 2.0
  times the paired isolated I1 p99.

Cancelled intermediate generations do not enter successful percentiles.
Accepted-cancel-to-physical-quiescence duration remains a separate observation.

#### Throughput

Throughput is successful logical site-operations per second, reported as
MPix-op/s. One B1 job contributes exactly 16,777,216 site-operations only after
Run success, a crash-durable output receipt, and both logical/raw golden
verifications. Its isolated interval starts immediately before both measured
queues are offered and ends at the final job's golden-verification completion.
Candidate and
reference replicates are paired by ordinal: the median of the three
candidate/reference ratios must be at least 0.95 and every ratio at least 0.90.
At each ordinal the candidate and reference B1 rows must have compatible
storage fingerprints as defined below. B1 cap-1/cap-8 determinism comparisons
within a subject also require that same compatible fingerprint; Run cap is the
intended difference, not storage.

For M1, each one-second mixed B1 rate is divided by its paired isolated cap-8
replicate's measured B1 rate. The nearest-rank p05 ratio must be at least 0.20.
A missing or zero denominator is `invalid`.

#### Fairness

For every one-second window in which Graph A and Graph B are both eligible for
the complete window, let their completed charged service be `x_A` and `x_B`:

```text
J = (x_A + x_B)^2 / (2 * (x_A^2 + x_B^2))
```

The nearest-rank p05 Jain index must be at least 0.95. A zero-total-service
window is `invalid`. Charged service uses the Host policy unit
`work_units + ceil(ready_bytes/4096)`.

While both classes remain continuously startable, no more than three
Interactive starts may precede one Throughput start. M1 additionally requires
zero Interactive admission failures caused by Throughput consuming declared
headroom, the Interactive latency gates, and the 0.20 Throughput progress
floor. Start order, completed progress, headroom admission, and latency are
independent evidence.

#### Determinism

For the same B1 job index across all three replicates, fresh-process restarts,
and Run caps 1 and 8, every mismatch count must be zero for:

- typed logical output `ContentDigest`;
- raw little-endian payload SHA-256;
- canonical artifact-manifest SHA-256;
- immutable job-indexed logical/raw golden identity; and
- the `execution-profile-semantic-trace-v1` fingerprint.

The semantic trace has exactly one `ready`, one `start`, and one `terminal`
record for every logical task in the deterministic plan. `task` is the
zero-based contiguous plan ordinal assigned by deterministic plan traversal,
not physical start order. Each record contains `job`, Graph role, `task`,
`action`, the numerically sorted dependency ordinals, terminal outcome, and the
task's declared `work_units`, ready entries/bytes, CPU slots, Host
retained/scratch bytes, and device-memory/scratch bytes. Fault-free B1 requires
terminal outcome `succeeded`; nonterminal records use outcome `-`.

The canonical bytes begin with this exact ASCII header and LF:

```text
execution-profile-semantic-trace-v1
```

Each following record is one exact ASCII line in this field order:

```text
job=<u>;graph=<A|B>;task=<u>;action=<ready|start|terminal>;deps=<u,...|->;outcome=<succeeded|->;work=<u>;ready-entries=<u>;ready-bytes=<u>;cpu=<u>;host-retained=<u>;host-scratch=<u>;device-memory=<u>;device-scratch=<u>\n
```

The displayed `\n` denotes one LF byte (`0x0a`), not the two bytes backslash
and `n`.
All unsigned integers are unpadded decimal (`0` is the only zero spelling),
there is no whitespace or BOM, dependencies are comma-separated ascending
ordinals or `-`, and LF terminates every line including the last. Records sort
by numeric job, Graph `A` before `B`, numeric task, then action rank
`ready < start < terminal`. The fingerprint is lowercase hexadecimal SHA-256
of those exact bytes.

Missing or duplicate required records, noncontiguous task ordinals, an absent
dependency target, an unknown/extra field or action, an invalid outcome, an
encoding violation, or an event-collector gap makes the trace invalid rather
than merely different. Timestamps, durations, physical worker/thread/device
queue identities, globally minted Run/task ids, raw sequence numbers, queue
positions, retries, and physical start/completion order are excluded. The
uncanonicalized physical trace, including those diagnostics, remains separately
retained. The semantic record set therefore compares cap 1, cap 8, and fresh
replicates without encoding their permitted physical completion order.

Cross-environment tolerant comparison is compatibility evidence and cannot
satisfy this exact same-environment verdict.

#### Waste

Started-service waste is:

```text
discarded_started_service / all_started_service
```

Started service uses `work_units + ceil(ready_bytes/4096)`. The numerator
includes every started callback whose result cannot commit because of
cancellation, supersession, failure, duplicate execution, or retry. Work that
starts after cancellation or supersession is accepted is counted separately
and must be exactly zero. Entered non-preemptible work is charged until it
drains.

Each I1 and I2 replicate must have an Interactive discarded-service ratio at
most 0.25. M1 applies that same ratio to Interactive service alone so completed
B1 service cannot dilute it. I2 additionally requires zero filesystem/codec
bytes, zero CPU-copy bytes, and zero transfer/allocation bytes beyond the two
conditional first Host-to-Metal accesses defined above. Fault-free isolated or
mixed B1 must have zero discarded, duplicate, and retry service.

#### Memory

Memory evidence retains byte high-water marks for Host retained memory, Host
scratch, ready bytes, and each configured device's memory and scratch, plus
row-owned post-quiescent reservation/grant deltas. B1 additionally retains the
event-aligned `ComputeIoExecutor` active-task and active-planned-byte high-water
and its exact zero settlement. The B1 planned-byte stream is mandatory,
authoritative evidence for Compute I/O admission, planned-byte high-water, and
final settlement; it does not establish physical memory ownership and does not
replace RSS or ledger/device ownership evidence. No authoritative dimension
may exceed its frozen limit. An isolated row must settle exactly to its pre-row
baseline; M1 shutdown must settle to zero.

For every authoritative dimension, candidate B1 and I2 peaks must be no more
than 105 percent of the pinned same-environment reference while still meeting
absolute limits. Process RSS is diagnostic because it includes allocations
outside current authority; it cannot replace ledger/device evidence or waive
settlement.

### Evidence is content-addressed and fail-closed

Every measured row belongs to an `execution-profile-slo-v1` bundle. The bundle
contains all frozen provenance, raw samples/events, eligibility windows,
drop/gap counters, output/artifact/trace/golden digests and commit receipts,
transfer/copy/residency evidence, high-water samples, aggregate inputs and
results, independent verdicts, and typed comparison/pairing references.
Eligibility means the offered-demand intervals defined above. Units, formulas,
denominator definitions, and invalidation reasons are schema fields rather
than prose-only labels.

Every bundle records `subject_role=candidate|reference`. A candidate's
`comparison_reference_bundle_digest` selects the immutable external baseline
used for candidate/reference regression; it is not an M1 isolated denominator.
Candidate and comparison reference must have the same evidence schema,
workload id, environment class, resource configuration, and fixture hashes.
Their repository/build identity may differ and is recorded because that is the
subject of the comparison.

Environment class is row-applicable rather than one unqualified machine label.
`base_environment_digest` binds OS/kernel, architecture, CPU/GPU/device
inventory, compiler/build mode and flags, worker count, provider/plugin
generations, frozen resources, cache/residency preconditions, and
power/thermal eligibility, but not repository commit. The row's
`environment-class digest` hashes the base digest plus
`storage_environment_applicability` and, when applicability is `required`, the
compatibility-eligible `storage_environment_digest`. I1 and I2 set storage
applicability to `not-applicable` and carry no storage digest because their
required paths perform no `OutputStore` artifact commit. B1 and M1 set it to
`required`. The normalized fingerprint and raw observations remain in the
bundle so a reader can recompute both digests.

Storage compatibility requires the same fingerprint schema, exact equality of
every normalized field, and equal independently recomputed
`storage_environment_digest` values, with both fingerprints compatibility-
eligible. A missing object, digest, raw field, eligibility proof, or any field
or digest mismatch makes every affected B1/M1 candidate/reference throughput,
memory-reference, or other relative verdict `invalid`. Different disposable
absolute paths may still compare when their normalized fields match and each
root-containment proof succeeds; equal path strings never override a
fingerprint mismatch.

Every M1 replicate ordinal `1..3` additionally records two same-subject pairs:
`paired_isolated_i1={row_digest,bundle_digest,replicate_ordinal}` and
`paired_isolated_b1_cap8={row_digest,bundle_digest,replicate_ordinal}`. A
candidate M1 row pairs with candidate isolated rows; a reference M1 row pairs
with reference isolated rows. The I1 pair supplies the relative latency
denominator, and the B1 cap-8 pair supplies every one-second throughput
denominator. Neither pair may be replaced by the generic comparison reference
or by one ambiguous “reference bundle digest”.

The paired row and M1 row must have the same replicate ordinal, evidence schema
version, subject build/provider/plugin identities, worker count, resource
limits/headroom, cache/residency preconditions, and power/thermal eligibility
policy. Both isolated pairs must exactly match `base_environment_digest`. The
paired I1 fixture hash must equal the I1 component embedded by M1, but the I1
row keeps `storage_environment_applicability=not-applicable`; M1's unrelated
storage fields neither participate in nor invalidate the I1 latency pair. The
paired B1 fixture/corpus/golden hashes and Run cap 8 must equal M1's B1
component, and the M1/B1 pair must have equal full `environment-class digest`
and compatible storage fingerprints. Missing, zero, wrong-ordinal,
cross-subject, unknown/unobserved/unsupported/unprovable storage state, or
otherwise incompatible pair evidence makes the affected M1 relative verdict
`invalid`.

All referenced bundles and rows are immutable and selected by content digest.
An unrecorded rerun of a “known good” build and a Markdown summary are not
normative references. Raw evidence must reproduce every aggregate and verdict.

### Downstream issues own fixed evidence rows

| Issue | Required v1 delivery |
| --- | --- |
| #93 | Implement I1 request/current-generation and cancellation/quiescence observation; publish isolated latency, waste, and memory rows plus required output-correctness evidence. |
| #94 | Implement I2 on the exact I1 lineage; publish preview/final latency, Host/conditional-Metal residency and copy-waste, and memory rows plus required output-correctness evidence. |
| #95 | Implement B1 immutable manifests, reservations, canonical semantic trace, crash-durable artifact commit, storage-environment collection/canonicalization, and logical/raw goldens; publish isolated throughput, determinism, zero-fault waste, and memory rows at Run caps 1 and 8. |
| #96 | Compose the exact I1 and B1 fixtures into M1, record its required storage fingerprint, enforce the M1/B1 storage pair while leaving the I1-only pair storage-independent, and publish mixed latency, throughput progress, fairness, waste, and memory rows. |

An issue may add lasting deterministic behavior tests for its mechanisms, but
cannot redefine a workload or promote a target using a missing, invalid, or
different-version row. Machine-dependent latency, throughput, and reference
ratios remain a maintained manual/release benchmark, not an ordinary CTest or
default CI correctness gate.

## Consequences

- Interactive speed cannot conceal starvation, nondeterminism, excess waste,
  or memory overcommit; throughput cannot conceal latency failure.
- Absolute Interactive budgets can fail on a valid slow machine. The evidence
  reports that result honestly; changing v1 requires a superseding decision,
  not a local relaxation.
- Relative gates can bless a slow reference, so absolute latency, exact
  determinism, and resource ceilings remain independent.
- Trusted `work_units` are estimates, not elapsed CPU time. They are used only
  for the declared scheduling-service units; wall throughput and latency remain
  independent.
- A telemetry gap invalidates affected evidence rather than being extrapolated.
- #92 deliberately adds no placeholder fields. Zero-valued unsupported SLO
  fields would appear authoritative, while implementing their collectors here
  would absorb the separate #93 through #96 delivery work.

Changing this contract after downstream consumption requires a new workload
or evidence-schema version and, when the accepted decision changes, a
superseding ADR. Existing evidence remains immutable and interpretable.
