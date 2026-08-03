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
| `I1-edit-storm-v1` | Uses seed zero. Zero-based edit `i` sets node one's `k` from `[0.82, 1.18, 0.86, 1.14, 0.90, 1.10, 0.94, 1.06, 0.98, 1.02, 0.96, 1.04]` and marks Region `(256*(i mod 4), 256*floor(i/4), 256, 256)`. The 12 edits are submitted 16,666,667 ns apart under the exact `(Graph, target node four, GlobalHighPrecision)` supersession key, Interactive QoS, weight 1, Run cap 8, and a 150 ms relative monotonic deadline per edit. Only edit 12 must publish; it receives a 500 ms drain. Episodes start at least 750,000,000 ns apart. Before every episode, node one is reset to `0.80` and the baseline target is materialized and settled outside the latency sample. |
| `I2-progressive-v1` | Reuses the exact I1 source, graph, seed, edits, source-space Regions, and generation lineage. The 512x512 preview source is a per-channel 4x4 box average of the 2048 source, rounded once to binary32 before the same four transforms; an I1 Region maps to `(64*(i mod 4), 64*floor(i/4), 64, 64)` in preview coordinates. The final evaluates the 2048 source. Only edit 12's preview and final are required latency results, in that order; stale output cannot publish. |
| `B1-immutable-v1` | Contains immutable jobs `0..29`; job `n` uses source seed `n`, the baseline graph, Throughput QoS, weight 1, no deadline or supersession, exact reservation evidence, canonical trace, committed artifact, and a job-indexed golden digest. Even jobs belong to Graph A and odd jobs to Graph B. At the measurement boundary the harness offers both ordered 15-job queues and never pauses a nonempty queue; bounded Host admission, rather than the harness, decides how many Runs are resident. Run caps 1 and 8 are separate required rows. |
| `M1-shared-v1` | At measured time zero, starts I1 and then repeats it every 750,000,000 ns, giving exactly 40 episode starts, while cycling the exact B1 corpus with its even/odd Graph assignment, Run cap 8, and continuous offered backlog for 30 measured seconds. Both streams use one `ExecutionService`, worker set, ready store, policy binding set, and `ResourceLedger`; no hidden pool, duplicate ledger, or separate process may absorb either stream. |

For fairness, one Graph is *eligible* while its producer has unconsumed offered
demand and has not paused submission. This workload-level interval includes
time awaiting bounded admission; it does not claim that all 30 B1 Runs are
admitted simultaneously. Within each Graph the producer offers jobs in
ascending index order and synchronously offers the next job when the prior one
becomes terminal. M1 starts a new `0..29` cycle without a producer-side gap.

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
and no-I/O gates still apply. Edit 12's final logical digest must equal the I1
edit-12 digest, and its preview digest must equal its own fixture golden.

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

The durable-output owner writes and settles the payload before atomically
publishing the manifest last. The commit receipt, payload, and manifest hashes
are evidence; the payload digest must match the immutable job-indexed golden.

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
- all resource limits and Interactive headroom.

The v1 resource configuration is 32 CPU slots, 1 GiB Host retained memory,
512 MiB Host scratch, 65,536 ready entries, 256 MiB ready bytes, and
Interactive headroom of one CPU slot, 64 MiB retained memory, 32 MiB scratch,
1,024 ready entries, and 16 MiB ready bytes. Compute I/O admission is limited
to 64 tasks and 256 MiB of summed planned bytes. When Metal is configured, its
device-memory and scratch limits are 512 MiB and 256 MiB. Absent Metal is predefined
`not-applicable`, not a zero observation.

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

Latency starts immediately before Host admission and ends when the matching
current generation becomes visible.

- I1 final-generation p50/p95/p99 must be at most 50/100/150 ms, with every
  measured episode publishing its final generation.
- I2 edit-12 first-preview p50/p95/p99 must be at most 50/75/100 ms; edit-12
  final p95/p99 must be at most 500/1000 ms. Both endpoints must match their
  required logical digest.
- M1 must satisfy the I1 absolute limits, and its p99 must be no more than 2.0
  times the paired isolated I1 p99.

Cancelled intermediate generations do not enter successful percentiles.
Accepted-cancel-to-physical-quiescence duration remains a separate observation.

#### Throughput

Throughput is successful logical site-operations per second, reported as
MPix-op/s. One B1 job contributes exactly 16,777,216 site-operations only after
Run success, required artifact commit, and golden verification. Its isolated
interval starts immediately before both measured queues are offered and ends
after the final manifest commit and golden verification. Candidate and
reference replicates are paired by ordinal: the median of the three
candidate/reference ratios must be at least 0.95 and every ratio at least 0.90.

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

- logical output SHA-256;
- canonical artifact-manifest SHA-256;
- job-indexed golden SHA-256; and
- a semantic trace fingerprint.

The semantic fingerprint excludes timestamps, physical worker ids, globally
minted ids, and raw sequence numbers, but retains run-relative task, action,
dependency, terminal-outcome, and required-resource facts. Raw physical trace
remains evidence. Cross-environment tolerant comparison is compatibility
evidence and cannot satisfy this exact same-environment verdict.

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
row-owned post-quiescent reservation/grant deltas. No authoritative dimension
may exceed its frozen limit. An isolated row must settle exactly to its
pre-row baseline; M1 shutdown must settle to zero.

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
results, independent verdicts, and the selected reference bundle digest.
Eligibility means the offered-demand intervals defined above. Units, formulas,
denominator definitions, and invalidation reasons are schema fields rather
than prose-only labels.

Reference and candidate must have the same schema, workload id, environment
class, resource limits, and fixture hashes. The reference is immutable and
selected by digest. An unrecorded rerun of a “known good” build and a Markdown
summary are not normative references. Raw evidence must reproduce every
aggregate and verdict.

### Downstream issues own fixed evidence rows

| Issue | Required v1 delivery |
| --- | --- |
| #93 | Implement I1 request/current-generation and cancellation/quiescence observation; publish isolated latency, waste, and memory rows plus required output-correctness evidence. |
| #94 | Implement I2 on the exact I1 lineage; publish preview/final latency, Host/conditional-Metal residency and copy-waste, and memory rows plus required output-correctness evidence. |
| #95 | Implement B1 immutable manifests, reservations, canonical trace, artifact commit, and goldens; publish isolated throughput, determinism, zero-fault waste, and memory rows at Run caps 1 and 8. |
| #96 | Compose the exact I1 and B1 fixtures into M1; publish mixed latency, throughput progress, fairness, waste, and memory rows. |

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
