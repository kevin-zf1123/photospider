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
| `I1-edit-storm-v1` | Uses seed zero and the twelve natural edit ordinals `1..12`. For `edit_index = edit_ordinal - 1` in `0..11`, node one's `k` is selected from `[0.82, 1.18, 0.86, 1.14, 0.90, 1.10, 0.94, 1.06, 0.98, 1.02, 0.96, 1.04]`, and the source Region is `(256*(edit_index mod 4), 256*floor(edit_index/4), 256, 256)`. Every Run uses `ComputeIntent::GlobalHighPrecision`, `ComputeRunQuality::Full`, Interactive QoS, weight 1, Run cap 8, the checked absolute monotonic deadline `D_i=A_i+150,000,000 ns`, and the exact `(Graph, target node four, GlobalHighPrecision)` supersession key. The twelfth edit (`edit_index=11`, `k=1.04`, Region `(768,512,256,256)`) is the only required publication and must publish no later than `D_11`; a separate 500 ms quiescence drain follows the cadence. |
| `I2-progressive-v1` | Reuses the exact I1 source, graph, seed, edit ordinals, source-space Regions, and realtime request lineage. The 512x512 preview source is a per-channel 4x4 box average of the 2048 source, rounded once to binary32 before the same four transforms; preview Region `edit_index` is `(64*(edit_index mod 4), 64*floor(edit_index/4), 64, 64)`. The final evaluates the 2048 source. Only the twelfth edit (`edit_index=11`, preview Region `(192,128,64,64)`) has required preview and final latency results, in that order; stale output cannot publish. |
| `B1-immutable-v1` | Contains immutable jobs `0..29`; job `n` uses source seed `n`, the baseline graph, Throughput QoS, weight 1, no deadline or supersession, exact reservation evidence, a canonical semantic trace, a crash-durable committed artifact, and job-indexed logical/raw goldens. Even jobs belong to Graph A and odd jobs to Graph B. At the measurement boundary the harness offers both ordered 15-job queues and never pauses a nonempty queue; bounded Host admission, rather than the harness, decides how many Runs are resident. Run caps 1 and 8 are separate required rows. |
| `M1-shared-v1` | At measured time zero, starts I1 and then repeats it every 750,000,000 ns, giving exactly 40 episode starts, while cycling the exact B1 corpus with its even/odd Graph assignment, Run cap 8, and continuous offered backlog for 30 measured seconds. Both streams use one `ExecutionService`, worker set, ready store, policy binding set, and `ResourceLedger`; no hidden pool, duplicate ledger, or separate process may absorb either stream. |

For fairness, one Graph is *eligible* while its producer has unconsumed offered
demand and has not paused submission. This workload-level interval includes
time awaiting bounded admission; it does not claim that all 30 B1 Runs are
admitted simultaneously. Within each Graph the producer offers jobs in
ascending index order and synchronously offers the next job when the prior one
becomes terminal. M1 starts a new `0..29` cycle without a producer-side gap.

#### B1 job occurrence identity is distinct from retry identity

`job_index` remains the immutable fixture and golden selector in `0..255`; it
is not sufficient to identify one execution occurrence because M1 reuses
`0..29` in every cycle. Every B1-bearing cold, warmup, or measured row therefore
assigns each offered job one canonical `job-instance-v1` fixed record with
components in this exact order:

```text
(row_workload_id:identifier,
 replicate_ordinal:uint64,
 phase:enum(cold|warmup|measured),
 cycle_ordinal:uint64,
 job_index:uint64,
 run_cap:uint64)
```

The canonical payload is the concatenation of one `frame(component-payload)`
per component under the fixed-record grammar below. `replicate_ordinal` is
`1..3`; `job_index` is `0..255`; `run_cap` is the row's frozen cap. In every
phase, `cycle_ordinal` starts at zero. It advances only after the preceding
complete `0..29` M1 corpus in that same phase and is never used for a partial
retry. B1 cold/warmup seed jobs and isolated measured jobs use cycle zero; M1
jobs in every phase use the current repeated-corpus cycle. The coordinate
`(phase,cycle_ordinal,job_index)` cannot repeat within one B1-bearing row.

The logical Compute I/O task is `(job_instance_id,stage)`, where `stage` is
`payload-stage` or `manifest-commit`; its full attempt identity is
`(job_instance_id,stage,attempt)`. `attempt` starts at zero and changes only
when an explicit retry/reconciliation policy reissues the same logical task
after a terminal failure. Capacity rejection, repeated observation, or an
idempotent duplicate `try_submit` keeps the same attempt identity and charge.
`cycle_ordinal` must never be encoded as, inferred from, or increment
`attempt`. Fault-free B1/M1 permits only attempt zero, one accepted admission,
and one start per logical task.

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
admission invocation; it is both the latency start and the deadline anchor.
The harness checks `S_i <= A_i <= S_i + 2,000,000 ns` before invoking Host and
computes, with checked arithmetic, the one absolute Run deadline:

```text
D_i = A_i + 150,000,000 ns
```

Anchoring `D_i` to `S_i`, the episode origin, an earlier preparation timestamp,
or the post-admission return time is invalid. The permitted start lateness does
not consume the 150 ms Run budget. An overflow, early start, start more than
2 ms late, admission failure, dropped edit, or cadence-event gap invalidates
the replicate. A missed edit is not submitted late: before any Host call for
that edit, the harness requests cancellation/supersession for every earlier
generation, records its acceptance, revokes all publication permission for the
episode, records the missed/drop/gap facts, and never catches up, backfills, or
shifts later nominal times. Already entered non-preemptible work may drain and
is charged as waste; work starting after accepted cancellation must remain
zero. No invalid or expired edit may publish output, receipt, or a successful
latency sample.

Expiry at `D_i` uses the same monotonic clock, requests cancellation of that
Run, and records its acceptance. Queued work is removed, dependent re-entry is
denied, and entered non-preemptible work drains without commit authority. A
deadline-expired result
cannot become current even if execution later succeeds. These rules apply to
every isolated and M1 I1 episode, including the twelfth edit; the separate
500 ms drain is only a quiescence observation window and never extends `D_i`.

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
identities `(job_instance_id,payload-stage,attempt)` and
`(job_instance_id,manifest-commit,attempt)`. The payload-stage task declares
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
  `[a-z0-9][a-z0-9._+-]*`; an `enum` is one exact field-specific token.
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
| 1 | `workload_id` | `identifier`; one of the four frozen workload ids | No |
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
| 1 | `workload_id` | `identifier`; one of the four frozen workload ids | No |
| 2 | `subject_role` | `enum`: `candidate` or `reference` | No |
| 3 | `bundle_provenance_digest` | `sha256`; section digest over retained repository/build/binary/provider provenance | No |
| 4 | `comparison_reference_bundle_digest` | `sha256`; immutable external reference for a candidate | `reference-has-no-comparison-baseline` |
| 5 | `row_references` | `row-reference-list-v1`; nonempty canonical list | No |

`bundle_provenance_digest` uses the section formula above with
`section_name=bundle-provenance` and
`section_schema_id=execution-profile-bundle-provenance-v1`.
`row-reference-v1` is a fixed record with components in the exact order
`(workload_id:identifier,run_cap:uint64,replicate_ordinal:uint64,row_digest:sha256)`.
`row-reference-list-v1` uses the generic list grammar with one frame around
each complete row-reference payload. Its functional row key is exactly
`(workload_id,run_cap,replicate_ordinal)`. The list is nonempty and functionally
unique by that key; two items with the same key are invalid even when their
`row_digest` values differ. Complete-payload duplicates are also invalid. The
list is sorted by numeric run cap, numeric replicate ordinal, then the complete
payload bytes, and every item workload id must equal the enclosing bundle
`workload_id`.

For every item, the verifier resolves the `row_digest` to exactly one retained
canonical row, recomputes the row digest, parses all 15 fields, and requires
the parsed row's `workload_id`, `run_cap`, and `replicate_ordinal` to equal the
item and its `subject_role` to equal the enclosing bundle. Zero or multiple
resolved rows, a digest mismatch, or any item/row/bundle mismatch invalidates
the bundle. A candidate encodes a known external
`comparison_reference_bundle_digest`; a reference encodes
`not-applicable/reference-has-no-comparison-baseline` with a zero-byte payload.
The candidate target must be a `reference` bundle with the same workload. For
each candidate row used by a reference-relative verdict, its target row is the
exactly one reference row with the same functional row key. The bundle digest
identifies only the target bundle; it never selects a row. A missing or
duplicate key, or a resolved target row that fails the same item/row/bundle
checks, makes the reference-relative verdict `invalid`.

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
| #93 | Implement I1 request/current-generation and cancellation/quiescence observation; publish isolated latency, waste, and memory rows plus required output-correctness evidence. |
| #94 | Implement I2 on the exact I1 lineage; publish preview/final latency, Host/conditional-Metal residency and copy-waste, and memory rows plus required output-correctness evidence. |
| #95 | Implement B1 immutable manifests, occurrence-scoped job/task identities, reservations, canonical semantic trace, crash-durable artifact commit, fixed storage/performance probe-to-schema adapters, mount normalization, the single encoder/digests, eligibility/B1 checks, and logical/raw goldens; publish closed-schema isolated throughput, determinism, zero-fault waste, and memory rows at Run caps 1 and 8. |
| #96 | Compose the exact I1 and B1 fixtures into M1, assign a distinct `cycle_ordinal` to every repeated B1 corpus without treating it as retry, reuse the exact v1 manifest bytes, enforce the same-ordinal full M1/B1 environment pair while leaving the I1-only pair base-only, and publish closed-schema mixed latency, throughput progress, fairness, waste, and memory rows. |

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
