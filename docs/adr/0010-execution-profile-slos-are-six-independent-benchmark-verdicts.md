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

For I1's exact HP path, each parsed curve coefficient is rounded once to
binary32 under round-to-nearest-ties-to-even (RNE). Each sample then uses three
explicit binary32 cuts: `p=RNE32(input*k32)`, `d=RNE32(1+p)`, and
`output=RNE32(1/d)`. The provider temporarily installs RNE on its worker,
avoids architecture-dependent bulk reciprocal approximations, and restores the
previous floating-point environment before reuse.

The independent I1 final oracle is versioned
`i1-coordinate-pattern-curve-chain-fp32-v1`. It reconstructs the source and
four stages without Host, Kernel, cache, scheduler, YAML, or the candidate
provider. For the HWC `[2048,2048,4]` FloatingPoint/NativeScalar32 tensor with
ImageFacet `(x=1,y=0,channel=2)`, the frozen `Sha256CanonicalV1` digest is
`17266cf3871544d61decc0805ce300ded59a688e75e826c15ce4b6989db4c493`.
The expected value is fixed before candidate execution; a product-path test
cross-checks it but can never bootstrap it.

The canonical workload matrix is:

| Workload | Frozen behavior |
| --- | --- |
| `I1-edit-storm-v1` | Uses seed zero and the twelve natural edit ordinals `1..12`. For `edit_index = edit_ordinal - 1` in `0..11`, node one's `k` is selected from `[0.82, 1.18, 0.86, 1.14, 0.90, 1.10, 0.94, 1.06, 0.98, 1.02, 0.96, 1.04]`, and the source Region is `(256*(edit_index mod 4), 256*floor(edit_index/4), 256, 256)`. Every Run uses `ComputeIntent::GlobalHighPrecision`, `ComputeRunQuality::Full`, Interactive QoS, weight 1, Run cap 8, the checked absolute monotonic deadline `D_i=A_i+150,000,000 ns`, and the exact `(Graph, target node four, GlobalHighPrecision)` supersession key. The twelfth edit (`edit_index=11`, `k=1.04`, Region `(768,512,256,256)`) is the only required publication and must publish no later than `D_11`. One continuous 221-slot cold/warmup/measured grid fixes every isolated episode origin; each episode's 500 ms settlement-observation window starts at its twelfth nominal start `S_11` and ends before the next origin. |
| `I2-progressive-v1` | Reuses the exact I1 source, graph, seed, edit ordinals, source-space Regions, realtime request lineage, and complete node-one coefficient sequence `[0.82, 1.18, 0.86, 1.14, 0.90, 1.10, 0.94, 1.06, 0.98, 1.02, 0.96, 1.04]` under the same `edit_index=edit_ordinal-1` mapping. One continuous 111-slot steady-clock grid links the cold, warmup, and measured phase origins; episodes are exactly 1,500,000,000 ns apart and contain twelve nominal preview-admission starts 16,666,667 ns apart with at most 2,000,000 ns lateness. At each index it updates node one to that coefficient, then executes nodes one through four in order with `k` values `[coefficient, 1.00, 1.20, 1.40]`. The 512x512 preview source is a per-channel 4x4 box average of the original 2048 source, rounded once to binary32 before that update/transform sequence; preview Region `edit_index` is `(64*(edit_index mod 4), 64*floor(edit_index/4), 64, 64)`. The final starts from the original 2048 source and uses the same I1 full-resolution update/transform path; it is never derived from preview pixels. Only the twelfth edit (`edit_index=11`, preview Region `(192,128,64,64)`) has required preview and final latency results, in that order; stale output cannot publish. |
| `B1-immutable-v1` | Contains immutable jobs `0..29`; job `n` uses source seed `n`, the baseline graph, Throughput QoS, weight 1, no deadline or supersession, exact reservation evidence, a canonical semantic trace, a crash-durable committed artifact, and job-indexed logical/raw goldens. Even jobs belong to Graph A and odd jobs to Graph B. At the measurement boundary the harness offers both ordered 15-job queues and never pauses a nonempty queue; bounded Host admission, rather than the harness, decides how many Runs are resident. Run caps 1 and 8 are separate required rows. |
| `M1-shared-v1` | Derives exact cold and warmup boundaries `C^M1=B^M1-6,000,000,000 ns` and `W^M1=B^M1-5,000,000,000 ns`, runs one cold I1 origin plus the fixed B1 seed-252 job, then seven warmup I1 origins plus the fixed seed-253/254/255 B1 protocol. At the exact warmup-cutoff/measurement-origin boundary, it starts measured I1 and repeats it every 750,000,000 ns for exactly 40 episode starts. The even Graph A and odd Graph B producers then repeat their own 15-job subsequences under independent producer-local cycles, Run cap 8, and continuous offered backlog for 30 measured seconds; neither waits for the other producer to finish the same local ordinal. The boundary neither pauses nor drains the shared domain: already offered warmup work retains its phase identity and resource authority ahead of newly offered measured B1 work. Both streams use one `ExecutionService`, worker set, ready store, policy binding set, and `ResourceLedger`; no hidden pool, duplicate ledger, or separate process may absorb either stream. |

Every workload-bearing field or fixed-record component uses the dedicated,
case-sensitive scalar type `workload-id-v1`. Its complete domain is exactly
`I1-edit-storm-v1`, `I2-progressive-v1`, `B1-immutable-v1`, and
`M1-shared-v1`; it performs no case folding, aliasing, Unicode normalization,
or open-ended identifier acceptance. These raw ASCII payloads frame exactly as
`16:I1-edit-storm-v1`, `17:I2-progressive-v1`,
`15:B1-immutable-v1`, and `12:M1-shared-v1`. The generic `identifier` type
remains lowercase-only and continues to serve every non-workload field that
declares it.

For fairness, one Graph is *eligible* while its producer has unconsumed offered
demand and has not paused submission. This workload-level interval includes
time awaiting bounded admission; it does not claim that all 30 B1 Runs are
admitted simultaneously. Within each Graph the producer offers jobs in
ascending index order and synchronously offers the next job when the prior one
becomes terminal. In measured M1, Graph A repeats `0,2,...,28` and Graph B
repeats `1,3,...,29`. Each producer starts its own next 15-job local cycle
immediately after its own final job becomes terminal; a fast producer may be
in local cycle `c+1` while the other remains in `c`. A shared cross-Graph
cycle barrier or a gap waiting for the other producer is invalid.

#### B1 job occurrence identity is distinct from retry identity

`job_index` remains the immutable fixture and golden selector in `0..255`; it
is not sufficient to identify one execution occurrence because M1 reuses
`0..29` in every cycle. Every B1-bearing cold, warmup, or measured row therefore
assigns each offered job one canonical `job-instance-v1` fixed record with
components in this exact order:

```text
(row_workload_id:workload-id-v1,
 replicate_ordinal:uint64,
 phase:enum(cold|warmup|measured),
 cycle_ordinal:uint64,
 job_index:uint64,
 run_cap:uint64)
```

The canonical payload is the concatenation of one `frame(component-payload)`
per component under the fixed-record grammar below. `replicate_ordinal` is
`1..3`; `job_index` is `0..255`; `run_cap` is the row's frozen cap. In every
phase, `cycle_ordinal` starts at zero. B1 cold/warmup seed jobs and isolated
measured jobs use cycle zero. For measured M1, the unchanged wire component
stores the `producer_cycle_ordinal`: the producer lane is derived without a new
field from even Graph A versus odd Graph B `job_index`. Graph A increments its
counter only after job 28 of its current local cycle becomes terminal, and
Graph B independently increments only after job 29; each immediately offers
its own job zero or one in the new local cycle. A producer never waits for,
increments, or completes the other producer's cycle. The coordinate
`(phase,cycle_ordinal,job_index)` remains unique within one B1-bearing row, and
the existing six-component record, outer schema, and retry semantics do not
change.

The logical Compute I/O task is `(job_instance_id,stage)`, where `stage` is
`payload-stage` or `manifest-commit`; its full attempt identity is
`(job_instance_id,stage,attempt)`. `attempt` starts at zero and changes only
when an explicit retry/reconciliation policy reissues the same logical task
after a terminal failure. Capacity rejection, repeated observation, or an
idempotent duplicate `try_submit` keeps the same attempt identity and charge.
`cycle_ordinal` must never be encoded as, inferred from, or increment
`attempt`. Fault-free B1/M1 permits only attempt zero, one accepted admission,
and one start per logical task.

The B1 output owner makes at most 64 total admission attempts for the current
stage after capacity rejection, always with that same attempt-zero identity
and charge. This is a deterministic count, not an elapsed-time or availability
policy. A non-capacity rejection or the sixty-fourth capacity rejection returns
typed `AdmissionFailed`, removes the incomplete occurrence slot, records one
`Final` boundary, and performs no further offer for that stage.

Every charge declaration, admission/status event, ledger or executor snapshot,
start/terminal record, `OutputCommitId`, rooted no-replace output slot,
`OutputCommitReceipt`, and row-evidence entry for B1 work binds the complete
`job_instance_id`; task-specific records additionally bind `stage` and
`attempt`. Different cycles may have the same fixture/golden and semantic-trace
digests, but they have distinct commit identities, output slots, receipts, and
evidence keys. The normalized semantic trace deliberately continues to encode
`job_index` rather than occurrence or physical scheduling identity so exact
determinism comparisons remain possible; the row's job-instance index binds
each retained physical trace and trace digest to its unique occurrence.

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

The harness must not start the Host admission call before `S_i`. `A_i` is the
single monotonic-clock sample captured immediately before the final Host
admission invocation; it is the latency start, deadline anchor, and, if that
invocation succeeds, the normative admission/acceptance timestamp. After
validating `A_i` and before invoking Host, the harness reserves one unique,
strictly increasing row-local `event_sequence_i` for that would-be
accepted-admission logical event. The harness checks
`S_i <= A_i <= S_i + 2,000,000 ns` before invoking Host and computes, with
checked arithmetic, the one absolute Run deadline:

```text
D_i = A_i + 150,000,000 ns
```

Anchoring `D_i` to `S_i`, the episode origin, an earlier preparation timestamp,
or the post-admission return time is invalid. The permitted start lateness does
not consume the 150 ms Run budget. On successful Host admission, the
accepted-admission logical event has the exact coordinate
`(A_i,event_sequence_i)`. The new edit/generation becomes current at that
coordinate, and every current-generation, latest-wins, supersession, and
same-timestamp ordering decision uses it. The collector carries that
pre-reserved coordinate through the source-private I1 Host and Kernel request
and binds it into the product `SupersessionIdentity` before coordinator
publication. Currentness linearizes when the coordinator publishes the complete
identity under its lineage lock. When both current and candidate carry accepted
coordinates, that coordinate is the sole replacement order: a strictly newer
coordinate replaces current even when its generation is numerically lower, and
an older or equal coordinate cannot replace current even when its generation is
higher. Equal timestamps use the row-local sequence. Generation remains a
nonzero unique preparation identity and Run join key; it does not encode bound
admission order. If either identity is unbound, the established generation rule
remains authoritative so legacy and mixed traffic are unchanged. The native
freshness registry follows the coordinator's exact managed current generation
rather than taking a numeric maximum, preventing a stale higher generation from
reviving after coordinate-authorized replacement. The independent observation
`causal_sequence` still starts at one and orders lifecycle facts only. The
current-generation observation and evaluator must reproduce the exact identity
binding, require nonzero unique generations plus strictly advancing accepted
coordinates, and never infer binding or currentness from callback/edit order.
The later Host return timestamp and status remain raw
measurement evidence only; neither may replace or reanchor that coordinate,
and learning success only on return does not move the logical boundary. On
admission failure, no accepted-admission logical event
exists, the reserved sequence plus failure/return facts remain raw evidence,
the replicate is invalid, and the harness must not synthesize, backfill, or
select an alternate acceptance timestamp. These facts remain in the existing
workload-manifest and measurement-evidence sections and add no outer row or
bundle field.

The embedded Host must make that success-only boundary transactional with
respect to its own resource preparation. Before it enters Kernel for either the
ordinary public request or the source-private I1 request, it constructs the
caller promise/future, successful result envelope, one-delivery backend bridge,
joined status worker, and close-visible tracking. Kernel current publication may
race ahead of its return, so after Kernel may have published a product identity,
Host performs only no-throw future sharing, bridge delivery, and movement of the
prebuilt result. Any recoverable Host preparation failure, including the
deterministic source-private test injection, therefore occurs before Kernel
entry and creates no current observation, accepted binding, or visible output.
A structural duplicate delivery or settlement is fail-stop.

An overflow, early start, start more than 2 ms late, admission failure, dropped
edit, or cadence-event gap invalidates the replicate. A missed edit is not
submitted late: before any Host call for that edit, the harness requests
cancellation/supersession for every earlier generation, records its acceptance,
revokes all publication permission for the episode, records the missed/drop/gap
facts, and never catches up, backfills, or shifts later nominal times. Already
entered non-preemptible work may drain and is charged as waste; work starting
after accepted cancellation must remain zero. No invalid or expired edit may
publish output, receipt, or a successful latency sample.

Irreversible physical service-start commitment and cancellation acceptance use
one Run-owned terminal arbiter. Cancellation accepted first prevents route
commit; a route commit that wins first reserves the lower causal coordinate.
The service publishes the start observation only after releasing its pool,
Run-state, and terminal-arbiter locks. For each materialized Run, generation
precedes every service start, every service start precedes terminal, and
terminal precedes quiescence, root-resource return, and Host settlement. The
evaluator therefore treats synthetic `cancellation < start < terminal` evidence
as structurally ordered but fails the independent waste verdict; the product
contract still requires zero such starts. The lossless fixed collector bound is
derived from one monolithic source plus four 64-tile curve nodes: at most
`1 + 4 * 64 = 257` starts per complete Run and
`12 * 257 = 3,084` per episode. Start 3,085 fails closed.

Expiry at `D_i` uses the same monotonic clock, requests cancellation of that
Run, and records its acceptance. Queued work is removed, dependent re-entry is
denied, and entered non-preemptible work drains without commit authority. A
deadline-expired result cannot become current even if execution later succeeds.
These rules apply to every isolated and M1 I1 episode, including the twelfth
edit. The settlement-observation window uses the nominal twelfth start rather
than the variable admission or deadline as its independent anchor:

```text
Q^I1_start(E) = S_11 = E + 11 * 16,666,667 ns
                = E + 183,333,337 ns
Q^I1_end(E) = Q^I1_start(E) + 500,000,000 ns
              = E + 683,333,337 ns
```

The window includes events at both boundaries. At `Q^I1_start`, the nominal
schedule marker orders before an actual admission with the same timestamp. At
`Q^I1_end`, the runner reserves the first excluded coordinate from the same
request-scoped causal sequence used at every product transition. An event
belongs to the boundary history only when its monotonic timestamp is no later
than `Q^I1_end` and its sequence precedes that cut; equal-time lifecycle events
therefore retain their authoritative order. Every materialized Run requires its
terminal, quiescence, exact root-resource return, and Host settlement in that
history. A missing transition or any active/later settlement invalidates the
replicate. A later eventual resource/lifecycle snapshot cannot backdate an
event across the cut. The window may observe an active final Run; it does not
cancel work, delay the next origin, or extend any `D_i`. At the latest legal
admission, `D_11 <= E + 335,333,337 ns`, leaving exactly 348,000,000 ns from
that deadline to `Q^I1_end` and 66,666,663 ns from `Q^I1_end` to the next
750,000,000 ns origin. Reset/baseline preparation must use that fixed remaining
guard and finish before the next origin rather than slide it. Every grid,
nominal-start, admission, deadline, and drain computation uses checked
arithmetic; overflow is invalid.

Each visible output is traversed for its typed digest at most once during the
measurement window, after which its `Value` handle is released. Evaluation and
serialization use only the frozen result. Normal `Q^I1_end` handling moves one
Value-free input into an owned async evaluator while the main thread prepares
the next baseline; the evaluator must finish before the next admission. JSON
construction, dump, and disk flush wait until `T^I1` and preserve exact slot
order. Exceptional paths revoke later submission and drain every closed row
before returning. This bounds ownership to one evaluator and 221 Value-free
rows, and no Host, Graph, collector, mutable `Value`, or worker exception can
escape the sole future boundary.

Except solely for M1's final warmup occurrence at `k=6`, the twelfth-edit
publication remains current through `Q^I1_end`. That one exception keeps the
same publication current at the `B^M1` carryover snapshot and until the first
measured edit's success-only accepted coordinate
`(A_0,event_sequence_0)`; the exact acceptance and supersession rules are
frozen in the M1 boundary below. The exception does not move `Q^I1_end` or
weaken its occurrence-local quiescence requirement.

One retained isolated-I1 replicate-grid origin `G^I1` fixes every phase rather
than allowing three independent origins:

```text
E^I1_g = G^I1 + g * 750,000,000 ns
E^I1_cold,0 = E^I1_0
E^I1_warmup,r = E^I1_(1+r),       r in 0..19
E^I1_measured,r = E^I1_(21+r),    r in 0..199
T^I1 = G^I1 + 221 * 750,000,000 ns
```

Natural episode ordinal maps to zero-based `r` within its phase. Cold occupies
slot zero, warmup slots `1..20`, and measured slots `21..220`; `T^I1` is a
terminal non-start boundary. Counter reset completes before the already fixed
measured origin. No phase may choose another origin, insert a cooling delay, or
shift a later slot. Each episode must be quiescent at its `Q^I1_end`; the last
measured episode must therefore settle before `T^I1` with the same exact
66,666,663 ns guard.

M1 separately uses `E_r = M_0 + r * 750,000,000 ns` for `r=0..39`, where
`M_0` is the exact mixed-load warmup cutoff and measurement origin described
below. Paired isolated and mixed evidence therefore share the same per-episode
schedule, start-lateness, drain, and miss/drop/gap rules without claiming
identical physical wake times.

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

I2 uses the same steady-clock domain but freezes its own complete episode
cadence. Within each cold, warmup, or measured phase's local enumeration,
`episode_ordinal=1..N` maps to zero-based
`episode_index=r=episode_ordinal-1`; measured I2 uses exactly `N=100` and
`r=0..99`. One retained phase origin `E^I2_0` fixes every origin as:

```text
E^I2_r = E^I2_0 + r * 1,500,000,000 ns
S^I2_{r,i} = E^I2_r + i * 16,666,667 ns,  i in 0..11
```

Here `E^I2_0` is the first origin of the current phase. One retained replicate-
grid origin `G^I2` determines all three phase origins without a pause:

```text
E^I2_{cold,0} = G^I2
E^I2_{warmup,0} = G^I2 + 1 * 1,500,000,000 ns
E^I2_{measured,0} = G^I2 + 11 * 1,500,000,000 ns
T^I2 = G^I2 + 111 * 1,500,000,000 ns
```

Thus cold, warmup, and measured episode starts occupy one continuous 111-slot
grid. The warmup-to-measured counter reset occurs before the already fixed
measured origin; it never inserts a cooling delay or chooses a new clock
origin. `T^I2` is the terminal grid boundary, not an episode start.

The harness prepares immutable edit input before `S^I2_{r,i}`. The single
monotonic sample `A^I2_{r,i}` captured immediately before preview Host admission
is the normative edit-acceptance/admission time: a successful call accepts the
edit, makes its pre-minted generation current, and admits its preview child as
one ordered boundary. Before the call, the harness must prove
`S^I2_{r,i} <= A^I2_{r,i} <= S^I2_{r,i} + 2,000,000 ns`. It must use checked
arithmetic to retain both absolute child deadlines:

```text
D^preview_{r,i} = A^I2_{r,i} + 100,000,000 ns
D^final_{r,i} = A^I2_{r,i} + 1,000,000,000 ns
```

An overflow, early admission, admission more than 2 ms late, failed admission,
missing/duplicate/out-of-order edit, or cadence-event gap invalidates the
replicate. The missing or invalid edit is never submitted late; publication is
revoked, accepted cancellation/supersession is recorded, and no later nominal
start or episode origin moves to catch up, backfill, or cool down. Entered work
may drain only as waste, and work starting after accepted cancellation remains
zero. Baseline preparation and all prior episode work must be quiescent before
the next fixed origin; the final measured episode must be quiescent before
`T^I2`. Failure is invalid rather than permission to shift either boundary.

The 1,500,000,000 ns stride is exact, not runner-selected pacing. Even at the
latest legal twelfth admission,
`D^final_{r,11} <= E^I2_r + 1,185,333,337 ns`, leaving a deterministic minimum
314,666,663 ns quiescence guard before `E^I2_{r+1}`, or before `T^I2` for the
last measured episode. The guard does not extend the final deadline. Thus the
one cold, ten warmup, and 100 measured episodes
use the same continuous replicate grid, while only the 100 measured twelfth-
preview/final pairs enter steady-state aggregates. The
ten warmup slots and 100 measured slots occupy nominal 15 s and 150 s phase
spans, matching I1's 20-by-750-ms warmup and 200-by-750-ms measured pacing
without overlapping I2's 1,000 ms final deadline.
Memory and output verdicts nevertheless consume all 111 rows. Latency and
waste consume complete verdicts, endpoint samples, and service only from
measured slots `11..110`; cold slot zero and warmup slots `1..10` propagate
Invalid only, while their Pass or Fail values and service remain outside the
steady-state aggregate.

For this state machine, “same I1 lineage” also freezes the complete numeric and
execution sequence. Let
`K=[0.82,1.18,0.86,1.14,0.90,1.10,0.94,1.06,0.98,1.02,0.96,1.04]`.
For every `edit_index=i` in `0..11`, I2 uses `K[i]`, the I1 source Region at
index `i`, the I2 preview Region at index `i`, and the same ordinal/generation
record. Each preview first computes the per-channel 4x4 box average from the
original 2048 source and rounds that source once to binary32, then updates node
one to `K[i]` and executes node one, node two, node three, and node four in that
order with `k` values `[K[i],1.00,1.20,1.40]`. Each final instead starts from
the original 2048 source, applies the same node-one update, and executes the
same four-node full-resolution path as I1. It must not upsample, reuse, or
otherwise derive final pixels from the preview. A coefficient substitution,
sequence reorder, index shift, Region/index mismatch, different rounding point,
or different final path is not `I2-progressive-v1`: a row carrying that id is
invalid, and a deliberately changed fixture requires a new workload id.

The final child is submitted exactly when that edit's preview first becomes
visible and its generation is still current. Edits `0..10` do not wait for
their preview: acceptance of `i+1` remains fixed by `S^I2_{r,i+1}` and
`A^I2_{r,i+1}`. A preview for `i` is current only when its visibility timestamp
is strictly less than `A^I2_{r,i+1}`; at equality, acceptance of the newer edit
orders first and the preview is stale. If a newer edit is accepted first,
the armed final is discarded without submission; an already submitted preview
or final is superseded and may only drain. A new generation revokes publication
permission for both older children. A stale terminal event may update cleanup,
waste, and quiescence evidence, but cannot publish a `Value`, digest, receipt,
or required latency result. The twelfth edit (`edit_index=11`) must publish its
preview and then its final; failure, deadline expiry, reverse order, duplicate
publication, or a newer generation before both endpoints makes the episode
invalid. Earlier generations may publish only while current and are not
required results. The twelfth preview must become visible no later than
`D^preview_{r,11}`; its final must become visible no later than
`D^final_{r,11}`. Expiry uses the same clock, revokes publication, and never
reanchors either deadline.

The RT and HP Run arbiters bind one request-local final gate. Cancellation
denies it inside the matching Run terminal critical section before `Cancelled`
is published; cleanup callbacks remain outside that section and cannot decide
the race. Final trigger consumes the same atomic gate. A cancellation winner
therefore suppresses trigger and HP service, while a trigger winner remains
subject to later cancellation and currentness at visible commit.

Preview latency starts at `A^I2_{r,i}` immediately before the preview Host
admission call and
ends at current preview visibility. Final end-to-end latency uses that same
start and ends at current final visibility; the later final trigger and Host
admission timestamps are retained separately but never reset
`D^final_{r,i}`. Thus the final gate includes preview, trigger, admission,
execution, and publication rather than hiding the preview interval.

No outer evidence-envelope field changes for this cadence. The existing
`execution-profile-workload-manifest-v1` section retains the clock domain,
replicate-grid origin, derived phase origins, terminal boundary, episode
ordinal/index, stride, twelve nominal starts, lateness bound, deadline formulas,
and tie-order rule.
The existing
`execution-profile-measurement-evidence-v1` section and raw events retain every
`E^I2`, `S^I2`, `A^I2`, child deadline, preview visibility, final trigger/
admission/visibility, cancellation, gap/drop, and quiescence observation.
Their existing section digests and verdict evidence are sufficient for an
independent cadence oracle; the closed 15-record row and five-record bundle do
not gain fields. Any origin/index, episode stride, edit cadence/order,
start-lateness, deadline anchor, or equal-time ordering drift makes a row
labelled `I2-progressive-v1` invalid. A deliberate change requires a new
workload id and new manifest/digest/golden lineage.

For each edit, the I2 Host settlement sequence is strictly greater than every
materialized child resource settlement, and its steady timestamp is no earlier
than any of them. Host status is the deterministic progressive terminal
aggregate: success exactly when at least one child materialized and every
materialized child Succeeded. Preview-only and preview plus successful final
therefore succeed; preview plus cancelled final and no-child fail. A sequence,
time, or status contradiction makes all four independently reported inner
verdict axes Invalid instead of inventing or backdating child evidence.

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
and no-I/O gates still apply. An already-Ready immutable Value may be acquired
after a newer generation becomes current while its coordinator-managed lineage
remains live. This
verification acquisition does not mutate currentness, but still requires exact
seed, revision, source/destination binding, producer, and fence identity;
ordinary current Run submissions retain exact stale-generation rejection.
After the second access and its diagnostic,
resource, and no-I/O facts have been copied, the Host removes only that row's
resident by exact `revision + complete StorageBinding + producer` identity
before the final row snapshot. A wrong identity is a no-op. This
verification-only release neither clears the cache broadly nor changes normal
lookup, publication, replacement, capacity, or eviction semantics. After
acquisition-local Values unwind, the complete memory-and-scratch device
`reserved` vector equals the pre-row baseline. The twelfth edit
(`edit_index=11`) final logical
digest must equal the I1 `edit_index=11` digest, and its preview logical digest
must equal its own fixture golden. The workload manifest and fixture oracle bind
the complete `K` array, index/Region mapping, node-update/transform order,
preview average-and-rounding order, and full-resolution final path; a drift in
any of those inputs changes the manifest and cannot be accepted under the v1
digest/golden linkage.

Every required logical output digest is obtained by calling
`compute_content_digest(Value)`. A sample is valid only when the returned
`ContentDigestResult.state` is `ContentDigestState::Available`, `digest` is
present, and `digest->algorithm` is
`CanonicalDigestAlgorithm::Sha256CanonicalV1`. Evidence stores the algorithm
tag and lowercase hexadecimal `ContentDigest.bytes`. Any other state, absent
digest, provider/readiness failure, or different algorithm makes the affected
row `invalid`. This canonical logical `ContentDigest` is not the SHA-256 of an
artifact's raw bytes.

I1 requires its expected digest to equal the immutable I1 oracle above. I2
requires its expected preview to equal `i2_frozen_preview_content_digest()` and
its expected final to equal `i1_frozen_final_content_digest()`, including the
exact typed algorithm. Missing, unsupported, or caller-substituted expected
evidence is Invalid even when the candidate observation is changed to match
that substitution. Only after the expected oracle is independently valid does
a complete candidate mismatch become Fail. Neither the evaluator nor JSON
encoder may recalculate a payload digest.

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
identities `(job_instance_id,payload-stage,attempt)` and
`(job_instance_id,manifest-commit,attempt)`. The payload-stage task declares
`planned_bytes=67,108,864`; the manifest-commit task declares `planned_bytes`
equal to that job's exact manifest length.
Checked arithmetic must produce those values before every `try_submit`, and
all attempts for one identity must use the same charge. The 64-task and
268,435,456-byte summed-planned-byte limits apply at every accepted admission.

`planned_bytes` is a stable admission estimate of task-retained bytes. Under
the same executor mutex as the decision, each offer receives an immutable
admission event with a monotonic nonzero sequence, exact charged task/byte
delta, typed status, and resulting process-global snapshot. Under the same
mutex as exact release, each accepted task receives a settlement event linked
to that admission and carrying the exact released delta plus resulting
snapshot. These per-task events are mandatory and authoritative evidence for
Compute I/O admission, snapshot high-water, and that task's settlement, but
they are not a measurement of physical allocation or proof of memory ownership.
They do not replace process RSS or the ledger/device ownership evidence.
Capacity rejection leaves the already offered job eligible and the same task
pending; every admission attempt and typed status is retained. In fault-free B1
each task may be accepted and started only once, and no output retry, duplicate
task, or changed charge identity is permitted.

The retained Compute I/O evidence is one exact state machine: `Initial` first;
payload attempt-zero offer/admission; payload settlement; manifest attempt-zero
offer/admission; manifest settlement; and `Final` last. Capacity rejections may
repeat only in the current offer state up to the 64-attempt bound. Every event
binds the expected job, stage, attempt, charge, typed status, and coherent
event-aligned snapshot. Accepted admission charges exactly one task and the
offered bytes; its linked settlement releases exactly that charge, while a
rejected admission charges zero. Global snapshots may include unrelated
concurrent jobs and may remain nonzero at this job's `Final`; the per-task delta
proves attribution, and the row boundary still settles to its required process
baseline. Missing, duplicate, reordered, gapped, wrong-identity, wrong-status,
undercharged, forged-zero, invalid-event/snapshot, or post-final evidence
invalidates throughput, determinism, waste, and memory together.

The payload-stage task completely writes, hashes, synchronizes, and revalidates
the private payload stage before it settles. Only then may manifest-commit
write and synchronize the private canonical manifest, atomically publish it
with no replacement, revalidate the published identity, and execute every
leaf-to-root directory barrier. B1 requests typed `crash-durable` durability
and accepts only typed achieved `crash-durable`; atomic visibility alone is not
success. Unsupported file synchronization, directory barriers, atomic
no-replace publication, a weaker achieved class, or any transaction failure
makes the job invalid and yields no successful crash-durable receipt.

The selected canonical root is opened without following links and its directory
descriptor, plus the fresh slot descriptor, remains the transaction's namespace
authority. Slot/payload/manifest mutation, publication, barriers, revalidation,
and cleanup are descriptor-relative. A pathname replacement or symlink can only
make the final path-to-descriptor binding fail; it cannot redirect writes. An
allocation-free guard owns the slot immediately after creation, adopts any
accepted completion before later work can throw, and on exceptional exit first
cancels/waits for exact charge retirement before identity-verified cleanup. The
same commit identity remains retryable.

The `OutputCommitReceipt` evidence binds at least the stable `OutputCommitId`,
rooted namespace/output slot, complete `job_instance_id`, job index, descriptor
and logical content
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

#### Storage, base, and row-environment manifests are closed v1 schemas

This ADR is the normative byte-schema authority. A v1 producer cannot add an
extension field, substitute a provider-specific object, omit a fixed field, or
interpret “equivalent” as permission to invent another representation. A
provider adapter maps its observations into the exact fields below or emits an
ineligible state.

All three manifests are ASCII and use the same field-record grammar. For an
ASCII byte string `B`, `frame(B)` is the unpadded decimal byte length, one
colon, and `B`. Thus the empty frame is `0:`. A field record is exactly:

```text
field=<frame(name)><frame(state)><frame(reason)><frame(type)><frame(payload)>\n
```

The literal header, every field record, and the final LF are digest input.
There is no BOM, CR, indentation, trailing space, locale-dependent formatting,
or final extra blank line. Each table below fixes record order, type, and
cardinality. A missing, duplicate, reordered, unknown-name, or extra record is
not a v1 manifest.

The scalar and composite payload grammar is closed:

- `identifier` is nonempty lowercase ASCII matching
  `[a-z0-9][a-z0-9._+-]*`. `workload-id-v1` is one raw ASCII token from the
  exact four-value domain above and is case-sensitive; it is not an
  `identifier`, and lowercase aliases or unknown tokens are invalid. An
  `enum` is one exact field-specific token.
- `uint64` is one complete ASCII decimal matching exactly
  `0|[1-9][0-9]*`, interpreted in the inclusive range
  `0..18446744073709551615`; `00`, `01`, any other leading-zero spelling, and
  overflow are invalid. Field tables may impose a higher minimum. `boolean`
  is exactly `true` or `false`, and `sha256` is exactly 64 lowercase
  hexadecimal digits.
- `text` is nonempty valid UTF-8 normalized to Unicode NFC and then encoded as
  two lowercase hexadecimal digits per UTF-8 byte. The manifest therefore
  remains ASCII while preserving case-sensitive identity bytes.
- A generic list payload is
  `<unpadded-count>:<frame(item-1)>...<frame(item-n)>`. A generic map payload is
  `<unpadded-count>:<frame(key-1)><frame(value-1)>...`. A generic fixed-record
  payload is the concatenation of one frame for each component's canonical
  scalar or composite payload in its declared order; component names and type
  tokens are schema metadata and do not appear inside that payload. Frame
  lengths and collection counts use the `uint64` lexical form and range; a
  list count is its item count and a map count is its key/value-pair count.
  Checked parsing rejects arithmetic overflow, a declared length beyond the
  remaining input, or a count that does not consume exactly that many values.
- `token-set-v1` is exactly the generic list grammar whose items are raw ASCII
  bytes of one exact token from the field's closed enum/identifier universe.
  Items sort by unsigned ASCII comparison of those unframed token bytes and
  are unique. The encoded payload is the count followed by one frame per
  token; the empty set is exactly `0:`. A duplicate or a token outside the
  field's closed universe is invalid, not merely ineligible.
- `ordered-text-list-v1` is exactly the generic list grammar whose item bytes
  are the canonical lowercase-hex payload of one `text` value, not a complete
  field record and not an additional text frame. The list grammar contributes
  the one item frame. Items remain in compiler invocation order and may
  repeat; the empty list is exactly `0:`.
- `cpu-record-list-v1`, `device-record-list-v1`, and
  `contract-record-list-v1` are exactly the generic list grammar. Each item is
  one frame around the complete corresponding fixed-record payload. Sorting
  and duplicate detection compare the unframed complete fixed-record payload
  bytes in unsigned ASCII order, not only `stable_identity` or `contract_id`.
  CPU and provider-contract lists contain at least one item; GPU,
  other-device, and plugin-contract lists may be exactly `0:`.
- `mount-map-v1` and `commit-semantics-v1` are exactly the generic map grammar.
  Their key and value item bytes are the raw ASCII tokens shown below, each
  surrounded by the map grammar's one frame. Keys sort by their unframed
  unsigned ASCII bytes and are unique; their counts are exactly seven and six
  key/value pairs respectively.
- `cpu-record-v1`, `device-record-v1`, `contract-record-v1`,
  `b1-performance-configuration-v1`, `resource-limits-v1`,
  `metal-resource-limits-v1`, `cache-preconditions-v1`,
  `residency-preconditions-v1`, `power-policy-v1`, and
  `thermal-eligibility-v1` are exactly the generic fixed-record grammar. Their
  component orders and scalar types are fixed below; there is no separator,
  component-name text, omitted component, or alternate provider object.
- The `type` frame contains the exact type token from the field table even when
  `state` is not `known`. A `known` record has reason `none` and its canonical
  nonempty payload (`0:` is the explicit payload of an allowed empty
  collection). Every other state has a zero-byte payload, encoded by the final
  `0:` frame.

For example, the complete known I1 workload field record is:

```text
field=11:workload_id5:known4:none14:workload-id-v116:I1-edit-storm-v1\n
```

The other three payload frames are the exact 17-, 15-, and 12-byte frames
listed above. A `job-instance-v1` or `row-reference-v1` fixed-record payload
contains only the component payload frame, so this correction preserves those
token bytes while changing their schema/parser domain. By contrast, evidence
row and bundle field records include the type frame in their canonical bytes:
`14:workload-id-v1` is mandatory, `10:identifier` is invalid, and every row or
bundle address must be recomputed from the corrected bytes. The earlier
`identifier` annotation could not encode any of the four uppercase-leading
workloads under the unchanged lowercase grammar, so it did not define a valid
legacy v1 object to reinterpret.

The eight-token durability capability example is byte-exact. Its
`token-set-v1` payload is 156 ASCII bytes:

```text
8:17:atomic-no-replace14:atomic-visible13:crash-durable20:idempotent-reconcile13:manifest-last13:manifest-sync28:namespace-durability-barrier12:payload-sync
```

The complete 221-byte field record, including its final LF, is:

```text
field=23:durability_capabilities5:known4:none12:token-set-v1156:8:17:atomic-no-replace14:atomic-visible13:crash-durable20:idempotent-reconcile13:manifest-last13:manifest-sync28:namespace-durability-barrier12:payload-sync\n
```

Here and in the remaining inline byte examples, the displayed final `\n`
denotes one LF byte. A known empty collection and a non-applicable scalar are
different bytes. Known-empty `build_flags` frames the two-byte list payload
`0:` and therefore ends in `2:0:`:

```text
field=11:build_flags5:known4:none20:ordered-text-list-v12:0:\n
```

By contrast, the I1/I2 non-applicable storage digest has a zero-byte payload
and therefore ends in `0:`:

```text
field=26:storage_environment_digest14:not-applicable24:row-has-no-output-commit6:sha2560:\n
```

States and reasons are also closed:

| State | Permitted exact reason | Compatibility eligibility |
| --- | --- | --- |
| `known` | `none` | Eligible if the value and all cross-field rules pass. |
| `not-applicable` | One field-specific reason from the table below. | Eligible only with retained proof that the named layer is absent from the complete durability/execution path. |
| `unknown` | `probe-returned-indeterminate` | Never eligible. |
| `unobserved` | `probe-not-run` or `probe-failed-before-observation` | Never eligible. |
| `unsupported` | `probe-contract-unsupported` or `platform-capability-unsupported` | Never eligible. |
| `unprovable` | `evidence-chain-incomplete` or `conflicting-effective-values` | Never eligible. |

The only v1 `not-applicable` pairs are:

| Field | Exact reason |
| --- | --- |
| storage `filesystem_type` | `filesystem-layer-absent` |
| storage `mount_identity`, `mount_effective_options` | `mount-layer-absent` |
| storage `hardware_write_cache_policy` | `hardware-write-cache-layer-absent` |
| storage `power_loss_protection_policy` | `power-loss-protection-layer-absent` |
| base `metal_resource_limits` | `configured-metal-executor-absent` |
| environment-class `storage_environment_digest` | `row-has-no-output-commit` |

No other field in the three environment manifests accepts `not-applicable`.
The evidence-row and bundle envelopes define their own closed optional-reference
reasons below. Layer opacity, lack of a probe, or a remote provider boundary is
not layer absence; those conditions use one of the four ineligible states.

Every B1 or M1 row sets `storage_environment_applicability=required`. Its
storage manifest starts with the exact header
`execution-profile-storage-environment-v1\n`, followed by exactly these 21
records:

| # | Field | Exact type and known value domain | Allowed N/A |
| ---: | --- | --- | --- |
| 1 | `output_store_contract_id` | `identifier`; stable `OutputStore` contract id | No |
| 2 | `output_store_contract_generation` | `uint64`; `1..18446744073709551615` | No |
| 3 | `backend_semantics_id` | `identifier`; stable normalization/semantics contract id | No |
| 4 | `backend_semantics_generation` | `uint64`; `1..18446744073709551615` | No |
| 5 | `backend_instance_id` | `text`; stable account/export/bucket/filesystem instance, excluding a disposable job directory | No |
| 6 | `backend_class` | `enum`: `filesystem`, `network-filesystem`, `object-store`, `memory-store`, or `composite` | No |
| 7 | `locality` | `enum`: `process-local`, `host-local`, or `network-remote` | No |
| 8 | `persistence` | `enum`: `volatile`, `host-restart-persistent`, or `externally-persistent` | No |
| 9 | `filesystem_type` | `identifier`; normalized filesystem type | `filesystem-layer-absent` |
| 10 | `mount_identity` | `text`; stable filesystem/export mount identity, not a path spelling | `mount-layer-absent` |
| 11 | `mount_effective_options` | `mount-map-v1`; exact map below | `mount-layer-absent` |
| 12 | `commit_semantics` | `commit-semantics-v1`; exact map below, including provider transaction equivalents | No |
| 13 | `durability_capabilities` | `token-set-v1`; subset of the closed capability set below | No |
| 14 | `requested_durability` | `enum`: `atomic-visible` or `crash-durable` | No |
| 15 | `achieved_durability` | `enum`: `atomic-visible` or `crash-durable` | No |
| 16 | `durability_endpoint_identity` | `text`; configured namespace/root through which the last required barrier or provider commit extends | No |
| 17 | `durability_anchor_identity` | `text`; stable backing filesystem, volume, device, bucket, or provider durability-domain identity | No |
| 18 | `storage_class` | `enum`: `memory`, `local-block`, `remote-block`, `network-filesystem`, `object`, or `composite` | No |
| 19 | `b1_performance_configuration` | `b1-performance-configuration-v1`; exact fixed record below | No |
| 20 | `hardware_write_cache_policy` | `enum`: `disabled`, `write-through`, `write-back-protected`, `write-back-unprotected`, `provider-managed-protected`, or `provider-managed-unprotected` | `hardware-write-cache-layer-absent` |
| 21 | `power_loss_protection_policy` | `enum`: `present`, `absent`, `provider-guaranteed`, or `provider-not-guaranteed` | `power-loss-protection-layer-absent` |

`durability_anchor_identity` is the one fixed representation for the backing
volume/device/storage identity previously described as a provider-specific
equivalent. Adapters do not add alternate fields. The selected absolute root,
resolved path components, fresh job directories, and root-containment proof
remain raw audit evidence outside the manifest. They cannot replace the stable
instance, endpoint, mount, or anchor identities.

`mount-map-v1` contains exactly seven lowercase key/value pairs in the order
shown; no key or value outside these enums is valid:

| Key | Exact value enum |
| --- | --- |
| `access_mode` | `read-only`, `read-write` |
| `atime_policy` | `strict`, `relaxed`, `none` |
| `cache_coherence` | `host-local`, `close-to-open`, `strong`, `eventual` |
| `copy_on_write_mode` | `disabled`, `enabled`, `provider-managed` |
| `data_write_mode` | `buffered`, `synchronous` |
| `journal_mode` | `none`, `writeback`, `ordered`, `full`, `provider-managed` |
| `metadata_write_mode` | `buffered`, `synchronous` |

The collector resolves effective behavior, not input spelling. An omitted
native option and an explicitly specified default emit the same value. Native
case is folded only where the platform contract declares the option domain
ASCII case-insensitive; canonical keys and enum values are always the lowercase
tokens above. The canonical map never preserves native order or duplicates. If
the platform defines a deterministic duplicate winner, the collector probes
and emits that one effective value; otherwise conflicting duplicates produce
`unprovable/conflicting-effective-values`. An unknown native option is excluded
only when retained evidence proves it cannot affect any of the seven keys,
`commit_semantics`, the fixed B1 performance configuration, hardware-cache/PLP
policy, or any measured storage-path timing; otherwise the relevant record is
`unprovable/evidence-chain-incomplete`. A missing canonical key, extra key,
duplicate key, unsorted key, or raw/canonical inconsistency is invalid.

`commit-semantics-v1` is a six-entry map in the order below. It applies to all
backends, including those whose known value is a provider transaction rather
than a filesystem primitive:

| Key | Exact value enum |
| --- | --- |
| `atomic_no_replace` | `rename-no-replace`, `link-no-replace`, `conditional-create`, `provider-transaction` |
| `barrier` | `file-then-leaf-to-root`, `write-through`, `provider-transaction` |
| `copy_on_write` | `none`, `filesystem`, `backend` |
| `directory_sync` | `directory-fsync`, `full-fsync`, `write-through`, `provider-transaction` |
| `file_sync` | `file-fsync`, `full-fsync`, `write-through`, `provider-transaction` |
| `rename` | `same-namespace-atomic`, `conditional-rebind`, `provider-transaction` |

The closed `durability_capabilities` token universe is
`atomic-no-replace`, `atomic-visible`, `crash-durable`,
`idempotent-reconcile`, `manifest-last`, `manifest-sync`,
`namespace-durability-barrier`, and `payload-sync`. A
compatibility-eligible manifest contains all eight. The set is still retained
for an ineligible manifest so a validator can report exactly what was absent.

`b1-performance-configuration-v1` is one fixed-record payload with exactly the
following 37 components. Each row contributes one frame containing the
component's canonical scalar payload, in this order; the component names do
not appear on the wire:

| # | Component | Exact scalar type and domain |
| ---: | --- | --- |
| 1 | `compression_mode` | `enum`: `disabled`, `enabled`, or `provider-managed` |
| 2 | `compression_algorithm` | `identifier`; exact normalized algorithm id or the sentinel required below |
| 3 | `compression_level` | `uint64`; effective numeric level, or the exact zero case below |
| 4 | `compression_profile` | `identifier`; exact normalized profile id or sentinel |
| 5 | `encryption_path` | `enum`: `none`, `host-client`, `filesystem`, `block-device`, `network-service`, `provider-managed`, or `composite` |
| 6 | `encryption_profile` | `identifier`; exact algorithm/mode/offload profile or `none` |
| 7 | `checksum_mode` | `enum`: `disabled`, `metadata-only`, `data-only`, `data-and-metadata`, or `provider-managed` |
| 8 | `checksum_algorithm` | `identifier`; exact storage checksum id or `none` |
| 9 | `deduplication_mode` | `enum`: `disabled`, `inline`, `post-process`, or `provider-managed` |
| 10 | `logical_block_bytes` | `uint64`; effective logical block size |
| 11 | `physical_block_bytes` | `uint64`; effective physical block size |
| 12 | `record_bytes` | `uint64`; effective backend record/object-write unit |
| 13 | `allocation_unit_bytes` | `uint64`; effective allocation unit |
| 14 | `allocation_mode` | `enum`: `preallocated`, `on-demand`, `sparse`, `copy-on-write`, `memory-resident`, or `provider-managed` |
| 15 | `provisioning_mode` | `enum`: `thick`, `thin`, `elastic`, `memory-resident`, or `provider-managed` |
| 16 | `layout_mode` | `enum`: `single`, `striped`, `mirrored`, `replicated`, `erasure-coded`, or `provider-managed` |
| 17 | `layout_data_units` | `uint64`; effective data-unit count |
| 18 | `layout_parity_units` | `uint64`; effective parity-unit count |
| 19 | `layout_replica_count` | `uint64`; effective complete-copy count |
| 20 | `layout_stripe_unit_bytes` | `uint64`; effective stripe/chunk unit |
| 21 | `layout_profile` | `identifier`; exact normalized geometry/service profile or `none` |
| 22 | `upper_write_cache_mode` | `enum`: `absent`, `disabled`, `write-through`, `write-back`, or `provider-managed` |
| 23 | `upper_write_cache_profile` | `identifier`; exact filesystem/backend/provider cache profile, `none`, or `not-applicable` |
| 24 | `io_scheduler` | `identifier`; exact normalized scheduler/profile id |
| 25 | `io_queue_policy` | `enum`: `serial`, `fixed`, `unbounded`, or `provider-managed` |
| 26 | `io_queue_depth` | `uint64`; effective queue depth under the rule below |
| 27 | `io_concurrency_policy` | `enum`: `serial`, `fixed`, `unbounded`, or `provider-managed` |
| 28 | `io_concurrency_limit` | `uint64`; effective storage-write concurrency under the rule below |
| 29 | `network_path` | `enum`: `not-applicable`, `host-loopback`, `lan`, `wan`, or `provider-internal` |
| 30 | `network_protocol` | `identifier`; exact protocol/version profile or `not-applicable` |
| 31 | `network_link_profile` | `identifier`; exact link/transport profile or `not-applicable` |
| 32 | `network_mtu_bytes` | `uint64`; effective path MTU, or zero only when no network hop exists |
| 33 | `network_qos_profile` | `identifier`; exact QoS/traffic-class profile or `not-applicable` |
| 34 | `network_region` | `identifier`; exact provider/placement region or `not-applicable` |
| 35 | `backend_service` | `identifier`; exact backend/provider service id |
| 36 | `backend_performance_tier` | `identifier`; exact service/performance tier or `not-applicable` |
| 37 | `device_performance_profile` | `identifier`; exact device/volume performance profile or `not-applicable` |

This fixed record uses the following closed sentinel and cross-component
rules:

- `compression_mode=disabled` requires algorithm/profile `none` and level
  zero. `enabled` requires a non-`none` algorithm and a profile that names the
  exact effective default or explicit parameter set; `provider-managed`
  requires algorithm `provider-managed`, level zero, and a non-`none` stable
  profile. A default level is not silently encoded as zero unless the named
  profile defines that exact default under the recorded backend-semantics
  generation.
- `encryption_path=none` requires `encryption_profile=none`; every other path
  requires a non-`none` profile that identifies algorithm, mode, and offload
  path without recording secrets or key material. `checksum_mode=disabled`
  requires `checksum_algorithm=none`; every other checksum mode requires its
  exact algorithm id. Deduplication has no omitted state: `disabled` is the
  explicit known-disabled value.
- A byte-unit component is zero only when retained evidence proves that the
  complete measured path genuinely has no fixed/applicable unit of that kind.
  An opaque, unobserved, variable, or undisclosed unit is not zero and makes
  the outer performance record ineligible. Otherwise the exact effective
  value is at least one.
- `layout_mode=single` requires data/parity/replica/stripe values `1/0/1/0`.
  `striped` requires data at least two, parity zero, replica one, and positive
  stripe unit. `mirrored` or `replicated` requires data one, parity zero,
  replica at least two, and stripe zero. `erasure-coded` requires positive
  data, parity, and stripe values and replica one.
- `layout_mode=provider-managed` still requires one stable non-`none`
  `layout_profile` that identifies the exact effective provider layout or
  service profile under the recorded backend-semantics generation; a generic
  `provider-managed`, opaque, unknown, or undisclosed placeholder is not a
  profile. All four geometry frames remain present in a `known` performance
  payload. For each frame, a positive value means that the concept exists on
  the complete measured provider path and that this exact effective value was
  observed. Zero is permitted only with the matching retained raw-proof kind:

  | Geometry component | Exact raw-proof kind required for zero |
  | --- | --- |
  | `layout_data_units` | `provider-layout-data-units-absent` |
  | `layout_parity_units` | `provider-layout-parity-units-absent` |
  | `layout_replica_count` | `provider-layout-replica-count-absent` |
  | `layout_stripe_unit_bytes` | `provider-layout-stripe-unit-absent` |

  Each proof must establish that the named concept is absent from the complete
  effective provider path, not merely hidden by an API boundary. These four
  proof kinds are closed raw-evidence labels; they are not new manifest
  state/reason pairs, N/A cases, fields, or digest inputs. If a concept exists
  but its one exact effective value is opaque, variable, undisclosed, or
  unobserved, the entire `b1_performance_configuration` field is
  `unprovable/evidence-chain-incomplete` with an empty payload; it cannot carry
  zero or a partial 37-component record. Conflicting values or an absence
  proof that contradicts the observed path produce
  `unprovable/conflicting-effective-values`. The profile, all positive values,
  all zero proofs, and the complete provider path must come from one frozen
  observation and satisfy the profile-specific relationships defined by
  `backend_semantics_id` and `backend_semantics_generation`. An all-zero
  geometry vector is therefore valid only when all four exact absence proofs
  and the non-placeholder layout profile are present and mutually consistent;
  provider opacity is never absence.
- `upper_write_cache_mode=absent` requires profile `not-applicable` with raw
  layer-absence proof; `disabled` requires `none`; every enabled or managed
  mode requires its exact profile. Queue/concurrency `serial` requires value
  one, `fixed` and `provider-managed` require a positive effective bound, and
  `unbounded` requires zero. An undisclosed provider limit is not unbounded.
- `network_path=not-applicable` requires protocol, link, QoS, and region all
  `not-applicable` plus MTU zero, with proof that the complete storage path has
  no network hop. Every other path requires non-sentinel protocol/link/QoS/
  region identifiers and a positive effective MTU. A remote-provider boundary
  does not waive those fields.
- `backend_service` always names the exact effective service. A performance
  tier or device profile may be `not-applicable` only with retained proof that
  the corresponding configurable layer is absent. Normalized identifier
  values are bound to `backend_semantics_id` and
  `backend_semantics_generation`; an implementation cannot invent aliases to
  force two native configurations to compare equal.

The complete performance configuration is observed and frozen before warmup
and must remain effective through the replicate. It contains stable effective
configuration, not instantaneous queue occupancy/latency, cache temperature,
free-space watermark, provider autoscaler/load state, competing-process load,
network RTT/jitter, a disposable path/job-directory name, or the subject
repository commit/build/binary identity. Those time-varying facts remain
eligibility/precondition evidence or raw diagnostics, and v1 does not require
two runs to have identical background noise. Configuration drift during a
replicate is invalid; diagnostic noise is not promoted into compatibility
bytes.

Every effective mount, filesystem, volume, device, backend, provider, or
network option/configuration that can change B1 payload or manifest write,
synchronization, barrier, provider-commit, revalidation, or golden-readback
timing must map to the fixed mount map, commit map, performance record,
hardware-cache policy, or PLP policy. Pure application CPU hashing does not by
itself add a storage field, but an option that changes the reads or writes
feeding revalidation remains in scope. An option may be excluded only when
retained authoritative evidence proves that it affects neither performance nor
durability anywhere on that complete measured path. Otherwise
`b1_performance_configuration` is
`unprovable/evidence-chain-incomplete`, and storage eligibility includes
`performance-configuration-unprovable`. For example, Btrfs
`compress=zstd` and disabled compression necessarily produce different
performance records and are incompatible even if their seven-key mount maps,
commit maps, and artifact manifests are otherwise identical.

The base manifest starts with
`execution-profile-base-environment-v1\n` and then exactly these 24 records.
Except for the one stated N/A case, every record must be `known`:

| # | Field | Exact type and known value domain |
| ---: | --- | --- |
| 1 | `os_family` | `enum`: `darwin`, `linux`, or `windows` |
| 2 | `os_release` | `text` |
| 3 | `kernel_name` | `text` |
| 4 | `kernel_release` | `text` |
| 5 | `architecture` | `enum`: `aarch64` or `x86_64` |
| 6 | `cpu_inventory` | `cpu-record-list-v1`; one or more unique records |
| 7 | `gpu_inventory` | `device-record-list-v1`; zero or more unique records |
| 8 | `other_device_inventory` | `device-record-list-v1`; zero or more unique records |
| 9 | `compiler_id` | `enum`: `apple-clang`, `clang`, `gcc`, or `msvc` |
| 10 | `compiler_version` | `text` |
| 11 | `compiler_target` | `text` |
| 12 | `standard_library_id` | `enum`: `libcxx`, `libstdcxx`, or `msvc` |
| 13 | `standard_library_version` | `text` |
| 14 | `build_mode` | `enum`: `debug`, `release`, `relwithdebinfo`, or `minsizerel` |
| 15 | `build_flags` | `ordered-text-list-v1`; zero or more flags in compiler invocation order |
| 16 | `process_worker_count` | `uint64`; `1..18446744073709551615` |
| 17 | `provider_contracts` | `contract-record-list-v1`; one or more execution/data/operation/policy provider contracts, excluding `OutputStore` |
| 18 | `plugin_contracts` | `contract-record-list-v1`; zero or more loaded plugin contracts |
| 19 | `resource_limits` | `resource-limits-v1`; exact record below |
| 20 | `metal_resource_limits` | `metal-resource-limits-v1`; exact record below, or N/A only with `configured-metal-executor-absent` |
| 21 | `cache_preconditions` | `cache-preconditions-v1`; exact record below |
| 22 | `residency_preconditions` | `residency-preconditions-v1`; exact record below |
| 23 | `power_policy` | `power-policy-v1`; exact record below |
| 24 | `thermal_eligibility` | `thermal-eligibility-v1`; exact record below |

The inventory and contract record component orders are fixed:

- `cpu-record-v1` is `(stable_identity:text, vendor:text, model:text,
  firmware:text, physical_cores:uint64, logical_cpus:uint64)`.
- `device-record-v1` is `(stable_identity:text, class:enum,
  vendor:text, model:text, driver_contract_id:identifier,
  driver_contract_generation:uint64)`; `class` is `gpu`, `accelerator`, or
  `io`.
- `contract-record-v1` is `(contract_id:identifier,
  contract_generation:uint64, semantics_id:identifier,
  semantics_generation:uint64)`, with both generations at least one.

`resource-limits-v1` has exactly the following components and values in this
order: `cpu_slots=32`, `host_retained_limit_bytes=1073741824`,
`host_scratch_limit_bytes=536870912`, `ready_entry_limit=65536`,
`ready_byte_limit=268435456`, `interactive_headroom_cpu_slots=1`,
`interactive_headroom_host_retained_bytes=67108864`,
`interactive_headroom_host_scratch_bytes=33554432`,
`interactive_headroom_ready_entries=1024`,
`interactive_headroom_ready_bytes=16777216`, `compute_io_task_limit=64`, and
`compute_io_planned_byte_limit=268435456`; every component type is `uint64`.
`metal-resource-limits-v1` is exactly `(executor:enum=metal,
device_memory_limit_bytes:uint64=536870912,
device_scratch_limit_bytes:uint64=268435456)`.

The remaining fixed records are:

- `cache-preconditions-v1` is `(disk_cache:enum=disabled,
  codec_io:enum=disabled, cross_episode_result_reuse:enum=disabled,
  cross_job_result_reuse:enum=disabled)`.
- `residency-preconditions-v1` is
  `(i1_host:enum=baseline-and-current,
  i2_host:enum=baseline-preview-final,
  i2_metal:enum=conditional-first-upload-then-reuse,
  b1_result_reuse:enum=disabled,
  m1_execution_authority:enum=single-process-domain)`.
- `power-policy-v1` is `(source:enum, mode:enum, sleep:enum)`, where `source` is
  `external-ac` or `battery`, `mode` is `automatic`, `balanced`,
  `high-performance`, or `low-power`, and `sleep` is `inhibited` or `allowed`.
- `thermal-eligibility-v1` is `(start:enum, maximum_allowed:enum)`; each
  component is `nominal`, `fair`, `serious`, or `critical`.

Repository commit, dirty state, executable/library/provider/plugin binary
hashes, bundle and row identities, and disposable paths remain mandatory raw
subject/audit evidence but are not base- or storage-manifest fields. This
permits candidate and reference subject binaries to differ while requiring the
same compiler/build configuration and contract generations. A same-subject M1
pair still compares those raw subject identities separately.

The lowercase digest definitions are exact:

```text
storage_environment_digest = lowerhex(SHA-256(exact storage manifest bytes))
base_environment_digest = lowerhex(SHA-256(exact base manifest bytes))
environment_class_digest = lowerhex(SHA-256(exact environment-class manifest bytes))
```

The environment-class manifest header is exactly
`execution-profile-environment-class-v1\n`. It has exactly four records in
this order: `base_environment_digest` (`sha256`),
`storage_environment_applicability` (`enum`),
`storage_environment_not_applicable_reason` (`enum`), and
`storage_environment_digest` (`sha256`). B1/M1 encode known values
`required`, `none`, and the recomputed storage digest. The exact applicability
domain is `required` or `not-applicable`; the exact N/A-reason domain is `none`
or `row-has-no-output-commit`. I1/I2 encode known values `not-applicable` and
`row-has-no-output-commit`; their final digest record has state
`not-applicable`, that same reason, and an empty payload. No row omits any of
the four records.

Before self, cap-one/cap-eight, candidate/reference, or mixed compatibility is
accepted, each side independently parses its retained base, optional storage,
and class manifests and recomputes every applicable digest from the actual
bytes. The class base-digest payload must equal the recomputed base and its
claim. B1/M1 must bind `required`/`none` and the known class storage-digest
payload to present storage bytes, their recomputed digest, and their claim, and
must retain the exact raw storage proof. Each side independently recomputes the
complete eligibility result from its retained storage bytes plus that proof and
requires exact equality with the retained eligible flag and ordered reason
list. Missing, incomplete, stale, or drifting proof and stale eligibility fail
the binding. I1/I2 must bind `not-applicable`/`row-has-no-output-commit` and the exact
N/A state/reason/empty payload to the absence of every storage evidence object.
The recomputed class digest must match its claim, but a valid class self-hash
does not repair a mismatched embedded base or storage digest payload.

The retained proof is not a list of producer assertions. Its only accepted
encoding is the canonical
`execution-profile-b1-storage-raw-proof-v1\n` document using the same
`field=<name-frame><state-frame><reason-frame><type-frame><payload-frame>`
grammar as the manifests. It contains exactly six known fields in order:
`backend_observation`, `field_observations`, `mount_observation`,
`performance_observation`, `transaction_observation`, and
`containment_observation`. Together they retain the backend/root cut; all 21
raw field values, arbitrary raw bytes, proof kinds, and proof identities;
provider-ordered native mount options/defaults/case/duplicate/no-effect proofs;
both exact 37-component performance cuts and option/absence/conflict evidence;
the complete contract/backend/durability/receipt binding and seven commit-event
observations; and the selected/resolved root plus every destination authority
and owner identity. Counts, fields, kinds, order, and uniqueness are closed,
and parse followed by encode must reproduce the exact proof bytes.

No eligibility, mapping-complete, consistency, N/A-validity, performance-valid,
or containment boolean is a proof input. The validator parses each side's
canonical proof bytes independently and reconstructs every predicate by
rerunning the backend adapter, mount normalizer, performance mapper,
transaction/receipt binding, and component-wise containment checks. Missing,
unknown, duplicate, malformed, stale, or internally drifting evidence therefore
fails even when the 21-field manifest and all claimed/class digests have been
recomputed to valid values. Durable JSON evidence carries the canonical proof
bytes, their digest, and a complete readable decoding of the same observations;
it does not introduce an alternate JSON proof grammar.

Storage compatibility eligibility is derived evidence, not digest input. Its
reason list is a deterministic result, not a producer-selected subset:

1. The validator first parses and validates the complete canonical storage
   manifest through framing, lexical form, field/type/state/reason rules,
   scalar/composite domains, cardinality, ordering/uniqueness, fixed-record
   shape, and all cross-field rules. If any of that phase fails, storage is
   `ineligible`, the reason list is exactly the single token
   `canonical-schema-invalid`, and eligibility evaluation stops because no
   raw-evidence or semantic predicate can be evaluated safely.
2. For a canonical manifest, the validator evaluates every predicate below
   independently and emits all and only the tokens whose predicates are true,
   once each, in unsigned-ASCII order. The exact possible output order is
   `canonical-schema-invalid`, `commit-semantics-inconsistent`,
   `durability-class-not-crash-durable`, `durability-path-inconsistent`,
   `mount-normalization-unprovable`, `not-applicable-proof-invalid`,
   `performance-configuration-unprovable`,
   `raw-observation-proof-incomplete`, `required-capability-absent`,
   `required-observation-ineligible`, and `root-containment-unproved`.
   An empty list means `eligible`; any nonempty list means `ineligible`.

For the canonical-manifest phase, the predicates are exact:

| Reason token | Predicate that makes it true |
| --- | --- |
| `commit-semantics-inconsistent` | The six known commit-semantic values and retained transaction/receipt observations do not describe one internally consistent payload-stage, manifest-last, no-replace, synchronization, and leaf-to-root/provider-transaction commit under the recorded backend semantics. Missing capabilities are not this predicate. |
| `durability-class-not-crash-durable` | At least one of `requested_durability` or `achieved_durability` is `known` and is not `crash-durable`. An ineligible observation state without a known weaker value is handled by `required-observation-ineligible`. |
| `durability-path-inconsistent` | Known contract/backend/instance/mount, endpoint, anchor, commit, and retained receipt/path facts affirmatively identify conflicting paths or fail to form one end-to-end durability path. A merely missing raw binding proof is handled by `raw-observation-proof-incomplete`. |
| `mount-normalization-unprovable` | A present mount cannot be reduced uniquely to the known identity and seven-key effective map because `mount_identity` or `mount_effective_options` is `unprovable`, default/case/duplicate/unknown-option resolution is unresolved, or retained native observations contradict the canonical mount mapping. A proved absent mount uses the N/A predicate instead. |
| `not-applicable-proof-invalid` | At least one syntactically permitted N/A field/reason pair lacks its exact complete-path layer-absence proof, or that proof conflicts with retained path observations. |
| `performance-configuration-unprovable` | The performance field is not `known`; any effective performance/durability option lacks a complete mapping or no-effect proof; any provider-managed geometry value/zero proof is incomplete; or the frozen configuration drifts during the replicate. Observed drift maps here even when the raw observation stream is otherwise complete. |
| `raw-observation-proof-incomplete` | Raw observation or raw-to-canonical mapping proof required to justify a `known` storage value, a permitted N/A claim, or a canonical normalization is missing, incomplete, stale, or contradicts the canonical value. This is not a catch-all for schema failure, read-only access, a missing capability, a known weaker durability class, complete-evidence commit/path inconsistency, observed configuration drift, or root containment. |
| `required-capability-absent` | The known effective `access_mode` is `read-only`, or one or more of the eight closed durability capability tokens is absent. |
| `required-observation-ineligible` | At least one required storage field has state `unknown`, `unobserved`, `unsupported`, or `unprovable`. A syntactically permitted N/A state is evaluated only by its proof predicate. |
| `root-containment-unproved` | Any measured job or retained release-artifact destination lacks a successful, unambiguous proof below the selected `OutputStore` root, or the retained proof fails or conflicts. Containment proof is owned by this predicate rather than the generic raw-mapping predicate. |

Overlaps are intentional and deterministic. An `unprovable` mount or
performance field also triggers `required-observation-ineligible`; a missing or
conflicting mount/performance raw mapping triggers its specific token and
`raw-observation-proof-incomplete`; an invalid N/A absence proof triggers
`not-applicable-proof-invalid` and `raw-observation-proof-incomplete`. A mount
conflict may therefore emit all three applicable tokens. A commit or
durability-path contradiction emits its specific token and adds the raw-proof
token only when the raw-to-canonical binding itself is missing or conflicting.
The reason list remains excluded from every environment digest, but the
canonical manifest plus retained raw evidence must let an independent
validator reproduce it exactly.

Two storage environments are compatible only when both are eligible, their
retained canonical storage manifests are byte-for-byte equal, each supplied
digest equals its independent recomputation, and the two digests are equal.
The byte comparison necessarily includes the complete framed
`b1_performance_configuration` field. Digest equality never substitutes for
byte equality. Base compatibility uses the same exact-byte and
recomputed-digest rule on the base manifests. A candidate/reference I1 or I2
comparison requires exact base compatibility and the fixed storage-N/A
environment-class manifest. Candidate/reference B1/M1, B1 cap-1/cap-8, and
M1/paired-B1-cap-8 comparisons require exact base and storage compatibility
plus equal recomputed full environment-class manifests and digests.
M1/paired-I1 compares only exact base compatibility; their environment-class
manifests intentionally differ, and M1 storage cannot invalidate that I1
latency pair.

Issue #95 owns fixed raw probe-to-schema mappings for the complete storage
path, including performance configuration; backend adapters into this exact
schema; mount normalization; state/reason proof; the one canonical encoder and
digest production; eligibility/root-containment evidence; and B1 cap-1/cap-8
plus candidate/reference checks. Issue #96 reuses those bytes unchanged,
records the M1 storage observation before warmup, and enforces the exact
same-ordinal M1/B1 pair while preserving base-only M1/I1 pairing. Neither issue
may add a v1 field, reinterpret a sentinel, or define an alternate provider
grammar. Issue #92 adds no probe, serializer, API, runtime result field,
harness, or compatibility code.

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
  fingerprint including the frozen B1 performance configuration,
  `storage_environment_digest`, compatibility eligibility, and raw
  capability/configuration observations.

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
settles; process/provider/JIT state remains.

#### The M1 cold and warmup input grid is exact

M1 derives two additional checked monotonic boundaries from the already
retained measurement boundary:

```text
C^M1 = B^M1 - 6,000,000,000 ns
W^M1 = B^M1 - 5,000,000,000 ns = C^M1 + 1,000,000,000 ns
```

Their row-local event sequences are `c^M1` and `w^M1`, with
`(C^M1,c^M1) < (W^M1,w^M1) < (B^M1,b^M1)`. Checked subtraction/addition
failure is invalid. Cold, warmup, and measured intervals are exactly
`[(C^M1,c^M1),(W^M1,w^M1))`,
`[(W^M1,w^M1),(B^M1,b^M1))`, and
`[(B^M1,b^M1),(U^M1,u^M1))`; the runner cannot choose a different origin or
length.

At `(C^M1,c^M1)`, a zero-duration start transaction establishes the sole cold
I1 nominal origin `E^M1_cold=C^M1` and offers exactly B1 Graph A seed 252 with
`phase=cold`, `cycle_ordinal=0`, and `attempt=0`. The B1 offer orders after
`c^M1`; a cold I1 admission at the same timestamp orders after that offer. The
cold I1 occurrence uses the existing I1 offsets and closes its own settlement
window at `C^M1+683,333,337 ns`, leaving a fixed 316,666,663 ns guard before
`W^M1`. Its generation must be quiescent at that endpoint. Seed 252's unique
terminal endpoint, artifact owner settlement, and output removal must all
order before `(W^M1,w^M1)`. A miss does not move `W^M1` or insert a drain; it
invalidates the replicate.

At `(W^M1,w^M1)`, a second zero-duration transaction verifies that cold work
has already met those endpoints, closes the cold sources, establishes warmup
I1 origin `E^M1_warmup,0=W^M1`, and activates the finite warmup B1 protocol.
It then offers seed 253 to Graph B followed by seed 254 to Graph A, both with
`phase=warmup`, `cycle_ordinal=0`, and `attempt=0`, satisfying
`w^M1 < sequence(B253) < sequence(A254)`. A first warmup I1 admission at the
same timestamp orders after both offers. When B253 becomes terminal, and only
then, the Graph B producer synchronously offers B255 at the same timestamp with
a greater sequence and the same warmup cycle/attempt. B255 must be offered
before `(B^M1,b^M1)`; otherwise the fixed warmup fixture is incomplete and the
replicate is invalid. Graph A has no warmup successor after A254. Thus the
complete offered warmup prefix is fixed by protocol, while the incomplete
subset at `B^M1` is determined only by the retained terminal history, never by
runner choice.

Warmup I1 has exactly seven nominal origins
`E^M1_warmup,k=W^M1+k*750,000,000 ns` for `k=0..6`. Origins `k=0..5` close
their per-occurrence settlement windows before the next origin. Origin `k=6`
is exactly `B^M1-500,000,000 ns`; its fixed
`Q_end=B^M1+183,333,337 ns` makes that occurrence/generation settlement-pending
at the measurement boundary. It remains immutable `phase=warmup` and appears
in the `B^M1` carryover snapshot. At its `Q_end`, the endpoint snapshot applies
only to that warmup occurrence/generation: it must be quiescent and settled,
but concurrently active measured generations and the shared execution service
need not be globally empty. Cold and warmup transitions never restart the
process, cool providers, rebuild queues, release shared resources, or shift a
boundary.

#### The M1 measurement boundary preserves warmup carryover

M1 retains one boundary event with exact monotonic timestamp `B^M1=M_0` and
row-local sequence `b^M1`, plus one terminal-cutoff event at checked timestamp
`U^M1=B^M1+30,000,000,000 ns` and sequence `u^M1`. Every raw boundary and
lifecycle event has a unique, strictly increasing row-local sequence. Event
coordinates are ordered as `(monotonic_timestamp,event_sequence)`, so
concurrent lifecycle events with the same clock value remain unambiguous. The
30 one-second windows occupy the ordered interval
`[(B^M1,b^M1),(U^M1,u^M1))`. Checked addition failure is invalid.

At `B^M1`, one zero-duration boundary transaction performs the following
logical steps in order without stopping the shared execution domain:

1. close every warmup offer source--the warmup I1 cadence and both B1 Graph
   producers--so no warmup occurrence is offered at or after the boundary
   event;
2. snapshot every warmup occurrence offered before the boundary whose unique
   completion endpoint has not yet ordered before it, including complete
   `job_instance_id` or I1 episode/generation identity, offered-waiting,
   accepted, queued, or running state, queue predecessor, reservation/grant,
   and owner-settlement state;
3. reset only measured logical accumulators while preserving raw events,
   occurrence identities, queues, policy state, resource authority, and the
   carryover snapshot;
4. establish the first measured I1 nominal origin at `M_0`;
5. offer measured B1 Graph A job zero followed by Graph B job one, both at
   timestamp `B^M1`, with sequence values satisfying
   `b^M1 < sequence(Graph A job zero) < sequence(Graph B job one)`, each with
   `phase=measured`, producer-local `cycle_ordinal=0`, and `attempt=0`.

Steps one through four form one atomic logical transition at the boundary
coordinate: no other row-local lifecycle event interleaves with their snapshot
or counter reset. A lifecycle event at timestamp `B^M1` with sequence below
`b^M1` orders before the whole transition; one with sequence above `b^M1`
orders after its snapshot/reset and then relative to the two measured B1 offers
by sequence.

The first measured-I1 Host admission invocation targets measured
`edit_index=0` and is not part of the atomic snapshot. Under the shared I1
rule, the harness samples `A_0` and reserves its row-local `event_sequence_0`
before that call. A successful call creates the exact accepted coordinate
`(A_0,event_sequence_0)`, with
`B^M1 <= A_0 <= B^M1+2,000,000 ns`; that coordinate, and only that coordinate,
may make the measured generation current and ordinarily latest-wins supersede
the old warmup generation. If `A_0` equals `B^M1`,
`event_sequence_0` must order after both measured B1 offers. The final warmup
I1 twelfth-edit publication must still be current in the `B^M1` snapshot and
immediately before `(A_0,event_sequence_0)`. A missing, failed, early, or late
admission invalidates the replicate; failure creates no accepted event and
cannot supersede the warmup generation. A Host return timestamp/status remains
raw evidence and never replaces `A_0` or the reserved sequence. No earlier
event--including the phase cutoff, nominal measured origin, carryover snapshot,
or either measured B1 offer--may revoke the old generation's current status,
cancel it, or rewrite the snapshot. The old generation still
settles and quiesces at its unchanged
`Q_end=B^M1+183,333,337 ns`; after acceptance, the remaining settlement time is
therefore within `[181,333,337 ns,183,333,337 ns]`. Any cancellation, terminal,
or settlement causally produced for that old generation retains its later
event sequence and immutable warmup phase, while its post-boundary physical
effects remain measured-window evidence.

If a same-timestamp lifecycle event orders before the boundary, the snapshot
reflects its new state; if it orders after, it is a cross-boundary event. A
terminal warmup event at the same timestamp never creates a new warmup
successor after step one. There is no phase-boundary wait, cooling interval,
drain, cancellation, process restart, worker/policy/queue reconstruction, or
resource release. Only the successful `(A_0,event_sequence_0)` coordinate
described above may supersede the retained final warmup I1 generation under
the frozen latest-wins rules; the harness adds no boundary-only cancellation.

Every outstanding warmup B1 occurrence retains its immutable `phase=warmup`,
cycle, job, and attempt identity and its existing per-Graph FIFO position. The
new measured offers follow the complete already-offered warmup prefix for their
Graph even when that prefix is queued or running. This exact transition is the
sole exception to waiting for the predecessor to become terminal before an
offer. Afterward Graph A offers the next even job on its measured predecessor's
terminal and advances from job 28 directly to job zero of its next
producer-local cycle; Graph B independently does the same from job 29 to job
one. Neither producer waits for the other, and neither completes, increments,
or rewrites an unfinished warmup identity. The cap-eight admission bound,
active backlog, queue order, and resource ownership span the boundary
unchanged.

Occurrence-owned results are attributed by immutable phase, never by completion
timestamp. A warmup occurrence's terminal result, completed service, output
bytes, latency, receipt, golden/digest result, duplicate/retry/discarded service,
and owner settlement remain warmup evidence even when observed after `B^M1`;
they do not enter measured throughput, completed-service fairness `x`, latency,
determinism, or waste numerators or denominators. Measured occurrence endpoints
contribute only when their ordered event coordinate lies inside the measured
interval. In contrast, scheduler and resource observations are time-windowed:
actual class-start ordering, headroom failures, queue contention, active
reservations/grants, Compute I/O counts, and Host/device/ready-memory high-water
after `B^M1` include physical effects from every phase. Thus carryover cannot be
hidden from contention or memory evidence. The class-start bound observes every
actual Throughput start in the measured interval, including a warmup start,
while Jain completed service remains measured-occurrence service only.

Warmup evidence remains required: a carryover failure, missing event evidence,
duplicate event sequence, non-total event coordinate, illegal phase rewrite,
boundary-only cancellation, queue reorder, snapshot mismatch, or unproved
settlement invalidates the replicate even though its occurrence-owned
quantities are excluded from measured aggregates. At `(U^M1,u^M1)`, the
ordered cutoff stops new measured B1 offers without cancelling already offered
work. An endpoint ordered at or after that cutoff is retained but does not
enter a 30-second numerator. Teardown must drain all phases and reach the
existing exact-zero resource/Compute-I/O settlement; quiescence is deliberately
not required at `B^M1`.

The workload manifest retains `C^M1`, `W^M1`, `B^M1`, `U^M1`, exact phase
intervals, I1 origin/count/index and `Q_end` arithmetic, the cold/warmup B1
offer protocol, the event-order/tie rule, boundary step order, queue/carryover
policy, producer-local cycle rule, and phase-attribution rules. Measurement
evidence retains all four boundary events, every fixed cold/warmup offer and
actual terminal-derived prefix transition, the full carryover snapshot, every
tied event coordinate and state transition, the first measured offers,
per-Graph predecessor and next-cycle counters, queue/start/terminal/receipt
joins, counter epochs, resource samples, failures, and final settlement.
Existing section and verdict digests cover these bytes; the closed 15-field row
and five-field bundle do not change. Any origin, offer, cycle, boundary,
ordering, carryover, attribution, or evidence drift while retaining
`M1-shared-v1` is
invalid and requires a new workload id if intentional.

Cold first use is retained separately and never pooled into steady-state
aggregates. All durations use a monotonic clock. Percentiles use nearest rank:
sort `N` samples and select one-based rank `ceil(p*N)`. Every replicate must
pass independently; pooling cannot hide a bad process. A summary may report the
median of the three replicate aggregates.

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
storage fingerprints under the closed schema above. B1 cap-1/cap-8
determinism comparisons within a subject also require that same compatible
fingerprint; Run cap is the intended difference, not storage.

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

Those canonical candidate records are produced only after execution from
actual source-private product observations. Ready materialization observes the
actual local identity, planned dependencies, shape/device, and submission
resource declaration; the execution service observes the irreversible start;
and task execution observes its terminal outcome. The collector maps the
actual shape/declaration into the B1 resource vector. The frozen semantic plan
is only the independent expectation oracle and is never emitted as observed
evidence before execution. Missing, duplicate, or gapped observations,
dependency/resource drift, causal reordering, or terminal-outcome drift makes
determinism invalid even when every artifact digest matches.

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

For I1 and I2, only the earliest causal start of one
`(run_id, local_task_id)` in a visible successful Run is useful. Later starts
for that identity are duplicate/retry work and contribute their full charge to
discarded service; distinct local task identities remain useful. Starts from
non-visible Runs remain discarded. Post-cancellation accounting is independent,
so an intersecting start contributes once to each applicable sum.

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

For I2 with Metal configured, the exact row-scoped resident release occurs
after second-reuse evidence is copied and before the final row snapshot. The
complete device `reserved` vector, including persistent memory and scratch,
must match its pre-row baseline so distinct revisions cannot accumulate device
memory below the fixed resident-entry capacity.

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
The exact 24-field base manifest, 21-field storage manifest, four-field
environment-class manifest, record grammar, and digest inputs are fixed above.
The base manifest binds OS/kernel, architecture, inventories, compiler/build
configuration, worker count, execution provider/plugin contracts, frozen
resources, cache/residency preconditions, and power/thermal eligibility. It
excludes repository commit and binary artifact hashes because those identify
the compared subject rather than its controlled environment.

Every bundle retains all three applicable canonical manifests, claimed and
independently recomputed digests, raw observations, normalization proof,
eligibility result/reasons, and root-containment evidence. I1/I2 use the fixed
storage-N/A environment-class encoding; B1/M1 use the required storage digest.
A missing record, manifest, digest, raw observation, proof, eligibility reason,
exact-byte match, or independent digest match makes every affected relative
verdict `invalid`. Different disposable absolute paths may compare only when
their stable normalized fields match and both containment proofs succeed;
equal path strings never override manifest drift.

#### Evidence row and bundle bytes are closed v1 schemas

Content addressing is reproducible only from canonical bytes. The evidence
envelope therefore reuses the exact `field=<frame(...)>...\n` grammar, scalar
lexical rules, fixed-record grammar, list framing, state/reason rules, and
no-BOM/final-LF rules above. Row and bundle manifests do not permit JSON,
provider-native objects, omitted fields, `null`, reordered fields, or extension
fields. For digest formulas in this subsection only, `frame(O)` accepts any
octet sequence `O` and uses its byte count; field-record names, metadata, and
canonical manifests remain ASCII as specified above.

An evidence row begins with the exact ASCII header
`execution-profile-evidence-row-v1\n` and then has exactly these 15 records:

| # | Field | Exact type and known value domain | Allowed N/A |
| ---: | --- | --- | --- |
| 1 | `workload_id` | `workload-id-v1`; one of the exact four frozen tokens | No |
| 2 | `subject_role` | `enum`: `candidate` or `reference` | No |
| 3 | `replicate_ordinal` | `uint64`; `1..3` | No |
| 4 | `run_cap` | `uint64`; the workload row's frozen cap | No |
| 5 | `base_environment_digest` | `sha256`; independently recomputed | No |
| 6 | `storage_environment_applicability` | `enum`: `required` or `not-applicable` | No |
| 7 | `storage_environment_digest` | `sha256`; independently recomputed when required | `row-has-no-output-commit` |
| 8 | `environment_class_digest` | `sha256`; independently recomputed | No |
| 9 | `workload_manifest_digest` | `sha256`; section digest defined below | No |
| 10 | `job_instance_index_digest` | `sha256`; section digest defined below | No |
| 11 | `measurement_evidence_digest` | `sha256`; section digest defined below | No |
| 12 | `output_evidence_digest` | `sha256`; section digest defined below | No |
| 13 | `verdict_evidence_digest` | `sha256`; section digest defined below | No |
| 14 | `paired_isolated_i1` | `evidence-pair-reference-v1` | `row-has-no-isolated-pair` |
| 15 | `paired_isolated_b1_cap8` | `evidence-pair-reference-v1` | `row-has-no-isolated-pair` |

The environment records repeat the exact applicable manifest digests so a row
can be validated without an implicit machine label. I1/I2 use the exact
storage-N/A state/reason/zero-byte payload already defined above. Both pair
records are known only for M1; every non-M1 row encodes each as
`not-applicable/row-has-no-isolated-pair` with a zero-byte payload. No digest
uses an empty string as a sentinel.

`evidence-pair-reference-v1` is one fixed record whose payload concatenates
frames for `(row_digest:sha256,bundle_digest:sha256,replicate_ordinal:uint64)`
in that order. `job-instance-list-v1` is the generic list grammar with one
frame around each complete `job-instance-v1` payload. Items sort by phase rank
`cold < warmup < measured`, numeric `cycle_ordinal`, numeric `job_index`, then
the remaining complete payload bytes; duplicate complete payloads and repeated
`(phase,cycle_ordinal,job_index)` coordinates are invalid. Its retained section
bytes are exactly the ASCII header
`execution-profile-job-instance-index-v1\n` followed by one field record named
`job_instances`, state `known`, reason `none`, type
`job-instance-list-v1`, and the canonical list payload. I1/I2 encode a known
empty list as payload `0:`; B1/M1 encode every offered B1 occurrence, including
cold and warmup. This index is the authoritative join from occurrence identity
to charge, admission, output, receipt, trace, aggregate, and verdict evidence.

The five section fields use these exact `(section_name,section_schema_id)`
pairs:

| Row field | `section_name` | `section_schema_id` |
| --- | --- | --- |
| `workload_manifest_digest` | `workload-manifest` | `execution-profile-workload-manifest-v1` |
| `job_instance_index_digest` | `job-instance-index` | `execution-profile-job-instance-index-v1` |
| `measurement_evidence_digest` | `measurement-evidence` | `execution-profile-measurement-evidence-v1` |
| `output_evidence_digest` | `output-evidence` | `execution-profile-output-evidence-v1` |
| `verdict_evidence_digest` | `verdict-evidence` | `execution-profile-verdict-evidence-v1` |

For each field, the exact retained section octets, including an explicit
known-empty collection where its versioned section schema permits one, produce:

```text
section_digest = lowerhex(SHA-256(
  "execution-profile-evidence-section-digest-v1\n" ||
  frame(section_name) || frame(section_schema_id) || frame(section_bytes)))
```

The row stores that value. The retained section bytes are mandatory and must
recompute it; the digest never substitutes for the section. #93 through #96
own the versioned inner records for their assigned collectors, but may not
change this envelope, domain separator, section-name binding, or occurrence
join.

Every versioned retained-section schema and the bundle-provenance schema is
closed over its address-bearing dependencies. Such a dependency exists when
canonical bytes are copied from, name, or are otherwise derived from another
object's content address; literal inclusion of the final hexadecimal digest is
not required. The schema must identify every typed address-bearing field and
every address input used to derive its canonical bytes. An opaque or
unclassified address-bearing field, an omitted dependency, or a producer that
cannot prove the complete dependency set makes that section and every
dependent verdict `invalid`.

An evidence bundle begins with the exact ASCII header
`execution-profile-evidence-bundle-v1\n` and then has exactly these five
records:

| # | Field | Exact type and known value domain | Allowed N/A |
| ---: | --- | --- | --- |
| 1 | `workload_id` | `workload-id-v1`; one of the exact four frozen tokens | No |
| 2 | `subject_role` | `enum`: `candidate` or `reference` | No |
| 3 | `bundle_provenance_digest` | `sha256`; section digest over retained repository/build/binary/provider provenance | No |
| 4 | `comparison_reference_bundle_digest` | `sha256`; immutable external reference for a candidate | `reference-has-no-comparison-baseline` |
| 5 | `row_references` | `row-reference-list-v1`; nonempty canonical list | No |

`bundle_provenance_digest` uses the section formula above with
`section_name=bundle-provenance` and
`section_schema_id=execution-profile-bundle-provenance-v1`.
`row-reference-v1` is a fixed record with components in the exact order
`(workload_id:workload-id-v1,run_cap:uint64,replicate_ordinal:uint64,row_digest:sha256)`.
`row-reference-list-v1` uses the generic list grammar with one frame around
each complete row-reference payload. Its functional row key is exactly
`(workload_id,run_cap,replicate_ordinal)`. The list is nonempty and functionally
unique by that key; two items with the same key are invalid even when their
`row_digest` values differ. Complete-payload duplicates are also invalid. The
list is sorted by numeric run cap, numeric replicate ordinal, then the complete
payload bytes, and every item workload id must equal the enclosing bundle
`workload_id`.

Before workload equality, functional-key construction, or target lookup, every
row, bundle, job-instance, and row-reference workload component must parse as
`workload-id-v1`. A generic `identifier` type frame, a case variant, or an
unknown workload fails canonical validation; a verifier must not bypass that
failure merely because two invalid byte strings compare equal.

For every item, the verifier resolves the `row_digest` to exactly one retained
canonical row, recomputes the row digest, parses all 15 fields, and requires
the parsed row's `workload_id`, `run_cap`, and `replicate_ordinal` to equal the
item and its `subject_role` to equal the enclosing bundle. Zero or multiple
resolved rows, a digest mismatch, or any item/row/bundle mismatch invalidates
the bundle. A candidate encodes a known external
`comparison_reference_bundle_digest`; a reference encodes
`not-applicable/reference-has-no-comparison-baseline` with a zero-byte payload.
Before any target-row lookup, the verifier resolves the candidate's comparison
digest to exactly one retained bundle object. It parses that object as the
exact canonical header and five records above, independently recomputes its
`bundle_digest`, and requires the result to equal the candidate's claim. The
resolved object must have `subject_role=reference` and the same `workload_id`
as the candidate, and its complete nonempty row-reference list must pass
canonical ordering, functional-key uniqueness, and every exact-one row,
15-field parse, rehash, and item/row/bundle check. Zero or multiple retained
objects, including multiple objects carrying the same digest claim, a five-
record parse/schema failure, claimed/recomputed digest mismatch, wrong role or
workload, or an invalid row list make every related reference-relative
verdict `invalid`; the verifier cannot choose one object by path, insertion
order, or byte equality.

Only after that bundle resolution succeeds, each candidate row used by a
reference-relative verdict selects exactly one reference row with the same
functional row key. The comparison bundle digest identifies only the target
bundle; it never selects a row. A missing or duplicate key, or a resolved
target row that fails the same item/row/bundle checks, makes the related
reference-relative verdict `invalid`.

Let `row_manifest_bytes` and `bundle_manifest_bytes` be the complete canonical
bytes from header through final LF. Their content addresses are exactly:

```text
row_digest = lowerhex(SHA-256(
  "execution-profile-evidence-row-digest-v1\n" ||
  frame(row_manifest_bytes)))
bundle_digest = lowerhex(SHA-256(
  "execution-profile-evidence-bundle-digest-v1\n" ||
  frame(bundle_manifest_bytes)))
```

In these formulas, quotation marks are notation: the bytes between them,
including the displayed LF, are input, and the quote characters are not.

Content addresses are sealed in this one-way order:

1. recursively resolve and validate every immutable external prerequisite row
   and bundle;
2. freeze each local retained section and the bundle-provenance bytes in
   dependency-topological order, then compute its section digest;
3. construct and freeze each row manifest from sealed section digests and
   allowed already-sealed external pair addresses, then compute its row digest;
4. construct and freeze the bundle manifest from its sealed provenance, sealed
   rows, and allowed already-sealed comparison reference, then compute its
   bundle digest; and
5. publish the claimed row and bundle digests beside, never inside, their
   immutable canonical objects.

No stage may use a fixed-point search, rewrite an already sealed object, or use
an address that is not sealed before the object that depends on it. The
address-dependency graph has one node for every retained section, provenance
object, row, and bundle. An edge `X -> Y` means that `X`'s canonical bytes or
content address depend on `Y`'s content address. The graph must be finite and
acyclic, and every edge target must precede its source in the sealing order. A
section or provenance node may depend only on already sealed prerequisites; it
must not directly or transitively reach its enclosing row, its enclosing
bundle, or any unsealed or later-stage node. A row may depend on its sealed
sections and already sealed external pair targets, but not on its enclosing
bundle. A bundle may depend on its sealed provenance, its sealed rows, and an
already sealed comparison target. Any direct or transitive path from a node
back to itself is invalid.

The claimed `row_digest` and `bundle_digest` are deliberately absent from their
own manifest bytes. A bundle contains row digests but never its own digest. The
external bundle graph contains an edge from an enclosing bundle to every
bundle named by its `comparison_reference_bundle_digest` or by an M1 pair in
one of its rows. Every target must already be materialized and sealed, and this
complete comparison/M1 graph must be globally acyclic; checking only the
direct target is insufficient.

Every known M1 pair must resolve its bundle and exact named row digest, then
pass the same canonical item/row/bundle consistency checks. The target bundle
and row `subject_role` must equal the enclosing M1 bundle role, the pair and
target row ordinals must equal the M1 row ordinal, and the target functional
key must use `I1-edit-storm-v1` at cap 8 or `B1-immutable-v1` at cap 8 for the
corresponding pair. Missing or multiple canonical objects, a missing or
duplicate functional key, any claimed/recomputed mismatch, an undeclared
address dependency, a later-stage/enclosing dependency, or any direct or
transitive cycle makes every dependent verdict `invalid`. These rules let an
independent verifier recompute section, row, bundle, comparison, and pairing
identities without trusting producer-assigned ids.

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
policy. Both isolated pairs must have byte-identical canonical base manifests
and equal independently recomputed `base_environment_digest` values. The
paired I1 fixture hash must equal the I1 component embedded by M1, but the I1
row keeps `storage_environment_applicability=not-applicable`; M1's unrelated
storage fields neither participate in nor invalidate the I1 latency pair. The
paired B1 fixture/corpus/golden hashes and Run cap 8 must equal M1's B1
component, and the M1/B1 pair must have equal full `environment_class_digest`
values and exact storage compatibility. Missing, zero, wrong-ordinal,
cross-subject, unknown/unobserved/unsupported/unprovable storage state, or
otherwise incompatible pair evidence makes the affected M1 relative verdict
`invalid`.

All referenced bundles and rows are immutable and selected by content digest.
An unrecorded rerun of a “known good” build and a Markdown summary are not
normative references. Raw evidence must reproduce every aggregate and verdict.

### Downstream issues own fixed evidence rows

| Issue | Required v1 delivery |
| --- | --- |
| #93 | Implement the reusable I1 accepted-boundary collector that samples `A_i`, reserves row-local `event_sequence_i` before Host invocation, emits `(A_i,event_sequence_i)` only on successful admission, carries the proposed coordinate into product supersession identity before current publication, requires exact row/current binding while keeping accepted-row and observer-causal sequence domains independent, and retains failure as raw evidence without an accepted event, current observation, or product binding; use it for the continuous 221-slot isolated-I1 grid, exact `S_11` drain/tie/guard behavior, request/current-generation and cancellation/quiescence observation; publish isolated latency, waste, and memory rows plus required output-correctness evidence. |
| #94 | Implement I2 on the exact 100-episode/12-edit cadence, acceptance/deadline anchors, preview-before-next-edit ordering, and I1 coefficient/index/update/full-resolution-final lineage frozen here; it cannot redefine those schedules or select different coefficients for edits `0..10` while retaining `I2-progressive-v1`. Publish preview/final latency, child-resource-before-Host settlement closure, exact row-scoped conditional-Metal residency release and copy-waste, and memory rows plus required output-correctness evidence. |
| #95 | Implement B1 immutable manifests, occurrence-scoped job/task identities, reservations, canonical semantic trace, crash-durable artifact commit, fixed storage/performance probe-to-schema adapters, mount normalization, the single encoder/digests, eligibility/B1 checks, and logical/raw goldens; publish closed-schema isolated throughput, determinism, zero-fault waste, and memory rows at Run caps 1 and 8. |
| #96 | Compose the exact I1 and B1 fixtures into M1; reuse #93's I1 accepted-boundary collector without redefining it, binding the first measured edit exactly to `edit_index=0`, `A_0`, and its pre-call reserved sequence; implement the fixed `C^M1`/`W^M1` cold/warmup origins, counts, B1 offer protocol, cross-`B^M1` I1 settlement, and the frozen final-warmup current-hold exception through that successful coordinate in `[B^M1,B^M1+2,000,000 ns]`; implement the exact cutoff/carryover/FIFO/phase-attribution and temporal-resource boundary; interpret the existing `cycle_ordinal` component as an independent producer-local counter for each measured B1 Graph without treating it as retry or adding a field; reuse the exact v1 manifest bytes, enforce the same-ordinal full M1/B1 environment pair while leaving the I1-only pair base-only, and publish closed-schema mixed latency, throughput progress, fairness, waste, and memory rows. |

The current #94 source tree implements its private preview-then-final product
coordination, exact preview/final arithmetic, Host and conditional real-Metal
acquisition evidence, exact row-scoped resident release, child-resource-before-
Host settlement order and aggregate status, continuous-grid profile, fail-
closed inner evaluator, and explicit manual runner. Its emitted
`execution-profile-i2-inner-row-v1` record
is deliberately narrower than the canonical outer row, bundle, and reference
composition frozen by this ADR. The runner is excluded from the default build
and CTest, and no exact 111-slot machine result is asserted here. Thus this
implementation status completes the assigned mechanism and inner-evidence
surface without promoting an absent machine run or claiming #95/#96 delivery.

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
