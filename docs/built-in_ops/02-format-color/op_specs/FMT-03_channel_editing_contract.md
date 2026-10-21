---
spec_schema_version: 1
id: FMT-03
kind: shared_operator_contract
category: 02-format-color
status: Proposed
document_maturity: D1_draft
implementation_status: implemented_cpu
clarification_status: complete
---

# FMT-03: channel reordering and replacement

Runtime contract: FMT-03A/B are installed public authoring helpers over the
registered FMT-02C Result operation. Scalar sources use prepared native bits
through the registered `channel.scalar_literal_<profile>` operation. There is
no native swizzle or replace key. The FMT specification decision remains
Proposed.

This operator-local draft inherits [FMT-common](FMT_common_contract.md), its
NUM numerical baseline and the accepted
[kernel storage contract](../../../kernel-specs/Tensor-Storage-and-Region-Access.md).
The purpose is to edit a base tensor's channel arrangement or selected channels.
The clarified members are [A swizzle](FMT-03A_swizzle_channels.md) and
[B replacement](FMT-03B_replace_channels.md), both compile-time authoring
compositions. The specification remains Proposed; current runtime status is described above.

## Relationship to earlier families

[FMT-01](FMT-01_channel_extraction_contract.md) extracts single components.
[FMT-02](FMT-02_channel_assembly_contract.md) assembles ordered single components,
concatenates channel blocks or creates a result from explicit multi-source maps.
Its A member fixes input i to output i without assigning component semantics. Its C member can
perform actual source/destination remapping.

FMT-03's proposed surface specializes single-base channel editing. Overlapping
copy maps need not acquire a second numerical definition: reuse the same exact
coordinate and explicit-selector principles where applicable. Association arithmetic and
alpha-specific policies remain FMT-04/05 responsibilities; merely editing a
channel must not silently run those operations.

Physical output storage follows the fixed Plan: each produced tile is dense and independently owned. Channel-count changes retain the common tile geometry and explicit spatial topology. Interpretation and actual byte mapping remain separate.

FMT-03 inherits exact scalar bit-copy semantics from the current numeric contract.
Connected scalar Results have shape `[1]`; authored literals are encoded as
native-order bytes with an explicit dtype. Result execution and storage follow
the FMT-02 mapping contract below.

## Confirmed decisions

| Topic | Selected behavior | State |
| --- | --- | --- |
| Members | A lists every output slot for single-base swizzle/subset/repeat/constants; B changes selected slots and preserves all others and the base channel count. | Confirmed |
| A interpretation | Output carries no description. Callers configure subsequent consumers explicitly. | Confirmed |
| Constant values | General typed scalar constants under NUM-03A numerical rules, rather than only 0/1. | Confirmed |
| Constant source form | Connected same-dtype shape-[1] scalars, plus authoring sugar for typed literals; scalar values may change between Runs while structure stays static. | Confirmed |
| B interpretation | Replacement copies data into destination indices; it does not assign destination labels. | Confirmed |
| B sources | Original base components, external component/channel tensors and explicit scalar constants; all replacements read original inputs simultaneously. | Confirmed |
| Execution and representation | Inherit exact mapped requests and auto/view/materialize from the previously selected FMT channel contracts; specify constants and overwrite exceptions below. | Inherited |
| Implementation identity | Compile-time swizzle/replace interfaces lowered to FMT-02C and conforming constant sources, without duplicate native copy operators. | Confirmed |

## Established behavior and inherited execution rules

FMT-03 emits `expected_inputs` as `result-inputs-v1:` followed by 64 lowercase
hexadecimal SHA-256 characters (81 characters total). The digest uses domain
separation and covers the complete ordered canonical schema and spatial topology
for every connected input, including repeated and unused inputs. It hashes no
sample payload and is not a validity proof. The zero-input fixed ABI 8 literal path
uses output shape `[1]` and prepared native bits.

A takes a base tensor and a static ordered output-slot list. Each slot selects
a base component or an explicit constant. It may reorder, omit or repeat source
channels. B preserves the base channel count and replaces selected destination
slots, leaving unlisted slots unchanged. Both publish immutable results; neither
mutates the input. A nonempty result channel axis is required.

Selections use explicit axis/index arguments or caller-supplied input descriptions,
without a fixed HWC assumption. Source bytes are preserved, including nonfinite
IEEE patterns. Outputs carry no descriptions or color/alpha validity claims.

The mathematical source of each output coordinate is its effective expression: constants use the scalar, unchanged B slots use the original base, and replaced slots use their replacement. Runtime applies this relation to every coordinate in complete retained output tiles, then closes source regions through their grids. A retained tile containing both base and constant slots needs both sources. Static validation covers every connected input, including unused sources; required upstream tasks retain their declared scope.

Auto/materialize copy or fill complete selected output tiles into independent dense allocations. Every nonempty forced-view task has only schema dependencies and fails ViewUnavailable. Empty validates static configuration and emits no task. This behavior is inherited from fixed FMT-02C; scalar literal providers use zero-input, zero-scratch fixed tasks.

## Logical interface and shape

Both authoring interfaces return one tensor handle `values`. Input 0 is a Result containing one tensor, `base`, with a positive cell rank and
an explicitly resolved cell-axis a. Every other nonscalar input Result also
contains one tensor.
Axis parameters index cell axes and exclude the Result batch prefix. The full
sample rank, including batch and cell axes, is at most 8.
Let its channel extent be C and let S be the ordered shape with axis a removed.
Axis resolution follows FMT-01/02: an explicit `input_descriptions` entry or a static axis,
no HWC/CHW guess, ordinary assertions must match, and raw requires an explicit
axis. Output retains the base channel-axis position; moving that axis is a
separate explicit operation, for example FMT-02B with one input.

For A, the required nonempty static slot list has length M and output shape is
insert(S,a,M). Every slot is either a base-channel selector or a constant-source
reference; omissions and duplicate source selections are allowed. A one-slot
result retains the channel dimension rather than squeezing it.

For B, the static replacement list maps distinct base destination channels to
source expressions. Output shape equals base shape. Empty replacement lists
are valid identity edits; duplicate targets fail even if their source bytes
would agree. Source expressions are original base channels, external component
tensors, selected external channel-tensor components, or explicit constants.
All spatial sources must yield exactly S after removing their declared channel
axis, if any. External channel axes may differ from a. A component source cannot
have an implicit singleton channel axis. With rank-one base, S is empty and
external rank-zero component tensors remain unsupported; scalar constants and
rank-one channel sources still work. No ordinary spatial source is broadcast.

Names and roles are separate selector namespaces with case-sensitive exact
unique matches, as in FMT-01B. Resolve source and destination selectors against
their original effective input descriptions before any replacement or output
reinterpretation. Source reuse and swaps always read original inputs; the list
is not a sequence of in-place assignments. Raw uses index selectors only.

Every nonscalar input has exactly the base batch prefix; scalar inputs are
unbatched shape `[1]` values. All inputs, including scalars, have the base dtype. Selected target types are
UInt8, UInt16, Int8, Int16, Int64, Float32 and Float64; missing native widths are
implementation dependencies. Counts/products and resource limits follow the
shared NUM/FMT contracts, including the 2^40-element logical bound. No implicit
cast, rounding, range conversion or NaN canonicalization occurs.

## Parameters and dynamic values

These are logical authoring parameters, not new registered ABI parameter kinds.
The helpers serialize the current bounded selector/map encoding.

| Field | Meaning / default |
| --- | --- |
| `axis` | Optional static Int64 cell-axis index in [0,base_cell_rank); required when no explicit input description supplies a channel axis. Batch axes are excluded. |
| `input_descriptions` | Optional map of input ordinals to explicit TensorDescription values. Used for axes and name/role resolution; does not change storage. |
| `layout` | auto by default; view/materialize have the FMT-02 meanings and failures. |
| `profile` | strict by default, or the named Apple Silicon/x86-64 CPU profile. Lower to the corresponding primitive keys; unavailable profiles fail explicitly. |
| A: `slots` | Static ordered list of base selectors or constant references; defines every output slot and M. |
| B: `replacements` | Static list of unique target selectors and source expressions. External sources have explicit component/channels/scalar structure and axis information. |
| Scalar source | Connected single-tensor Result of unbatched shape [1], read at index zero. Its value may change between executions; its dtype/shape and map structure do not. |
| Typed literal | Authoring shortcut for an immutable scalar source with an explicit dtype and exact represented bits, using shared numeric construction rules. |

A scalar that supplies a channel is constant over that channel's spatial domain,
not necessarily constant across Runs. An ordinary shape-[1] spatial tensor does
not implicitly become a constant; the source expression must say so. Literal
construction/validation follows NUM conventions and never silently casts a
different scalar dtype to base dtype. Preserve Int64 values and all floating
special bits without routing them through an untyped Float64 parameter.

## Explicit interpretation

A and B copy or fill bytes at selected indices. They neither inherit source
labels nor assign destination labels. The next operation receives its own
explicit description. The helper validates supplied input descriptions against
their structural descriptors and uses them only to resolve axes and selectors.
There is no output-description parameter or hidden edge-description table.

Dropping alpha does not flatten against a background. Replacing an alpha slot
does not multiply or divide color samples or scan a coverage interval. FMT-04
owns association arithmetic and FMT-05 owns alpha-specific editing. A later
consumer validates the domain it explicitly requests.

## Coordinate and dependency reference

For requested output coordinate j, write k=j[a] and u=erase(j,a). Resolve the
effective source expression e(k): A uses slots[k]; B uses its replacement if
present, otherwise original base channel k. Define:

```
base channel s:        Y[j] = base[insert(u,a,s)]
external component X:  Y[j] = X[u]
external channel s:    Y[j] = X[insert(u,source_axis,s)]
constant scalar V:     Y[j] = V[0]
```

Every equality defines a mathematical byte copy at one output coordinate. Runtime evaluates the relation over complete retained output tiles and deduplicates each port's source tile union. A scalar is read if any destination in those tiles selects it, including destinations added by output tile closure. Disjoint mapped regions remain separate before source-grid closure. Static descriptor checks cover every connected input, with no sample-domain validation.

The mathematical source relation includes simultaneous swaps and excludes overwritten base samples that no effective output expression selects. Physical execution first closes requests to complete output tiles and then closes each effective source region through its source grid. A base payload can be omitted only when all destinations in retained output tiles use other sources. This contract does not define incremental dirty propagation.

Constant values selected by complete retained output tiles participate in runtime reads. A malformed
static literal or inconsistent descriptor fails preflight even for an unrequested
slot. Required upstream failures, resource exhaustion and cancellation retain
their own scope; consumer selection cannot undo already required producer work.

## Compile-time lowering and execution identity

A builds one FMT-02C row per output slot. B begins with base-to-same-slot
rows and substitutes explicit replacements. Both preserve base axis a as
`output_axis`; mapping rows contain input, match, selector and destination.

Connected scalar sources remain unbatched shape `[1]`. The generated mapped
assembly receives an explicit scalar source-structure record and repeats the
scalar for selected coordinates without a separate fill node. A distinct typed
literal produces a `channel.scalar_literal_<profile>` node; repeated identical
literal bits share that node. No rank-zero temporary is created.

Preserve Descriptor dependencies and static validation for the original base and
all connected sources, even if every retained output tile is constant. Expand only
the static mapping, not one node per pixel. Retain named authoring provenance in
diagnostics. A/B add no independent native operation keys; their profile selects
the matching FMT-02C and constant-provider keys. Existing legacy `channel.swizzle`
does not become an alias. Constants/helpers must obey the selected FMT metadata
rules rather than silently restoring legacy typed-scalar validation.

Lowering is conforming only when fixed layout policy, source tile closure,
output structure, ownership, errors and publication match these definitions.
Inlining/fusion may remove intermediates if it preserves those observations.
Do not fall back to an old complete-input packed-image operation to make the graph run.

Use collision-aware graph ID allocation and transactional expansion as in
[FMT-01C](FMT-01C_split_channels.md): verify supplied descriptors against actual
inference, stage new declarations/nodes/handles, and leave the caller's graph
unchanged if expansion fails. Do not execute an input producer to discover a
static channel count. Check the current host's graph/port/root limits before
appending; count actual literal/provider and fill nodes as well as the C node.
Reuse a fill source for repeated references to the same scalar when identity
and semantics agree. The composition introduces no independent dtype or ABI.

## Storage, resources and failures

The fixed FMT-02C task writes an independently owned dense output tile with zero scratch. Input view metadata and immutable preparation scale with static spans and source tile closure; selected output allocations scale with complete selected tiles. Literal providers own their scalar output independently. Runs share immutable preparation and use fixed-size coordinate/cache state, with cancellation checks between at most 256 copies.

Only effective mapped coordinates are numerically read. Physical tiles may cover extra samples, and upstream tasks retain their declared scope. Completed tiles can enable internal consumers; each public call retires only after its required work succeeds. Failed calls export no partial result. Output owners survive source/context destruction. No mutable constant cache, private pool or dynamic view publication is introduced.


| Failure | Phase | Outcome |
| --- | --- | --- |
| Empty A slots, duplicate B targets, invalid axis/index/name/role, malformed literal/source kind or invalid explicit input description | Authoring/compile/direct preflight | InvalidArgument / InvalidDomain, schema origin. |
| Full sample rank above 8, batch-prefix mismatch, cell-rank/dtype/nonchannel-shape mismatch, or scalar batch/shape other than unbatched [1] | Preflight | TypeMismatch / None. |
| Checked count/graph/map/metadata/work capacity, unavailable forced view or backend | Inherited phase | Preserve FMT-02/NUM/kernel status and diagnostic. |
| Missing source coverage, required producer failure, cancellation or stale plan | Requested observation | Preserve original failure category/scope; no fabricated success. |

Diagnostics identify the helper/member, output slot, source port/selector and
offending static field. Neither nonfinite sample bytes nor alpha outside [0,1]
alone fail a copy/fill operation. No commercial pixel-equivalence claim is made.

## Acceptance and implementation dependencies

Member files specify analytic fixtures and conceptual public workflows. Verify
with an independent coordinate interpreter, byte oracle and finite-set source tile evaluator. Cover identity, partial/disjoint
ROI, source reuse, channel-axis positions, constant-only requests, simultaneous
swaps, overwritten-value nonreads, explicit selector descriptions, view/materialization and
special-value bits. Validate helper lowering against the mathematical expressions
and retain original source descriptions after execution.

Implementation delivery must include an actual public compile/execute example
with checked output, commands and an offset channel ROI. FMT-02C, canonical
metadata/map encodings, conforming generic constant
sources and fixed scalar/tile dependency and retirement support are prerequisites.
The CPU implementation uses fixed FMT-02C tasks and scalar providers for this composition.

Benchmark Float32 [4096,4096,4] base data for A BGR reorder, subset plus repeated
source, and opaque-alpha insertion; for B one external-plane replacement and
scalar-alpha fill. Compare whole, one-channel and y/x=[127,130) requests for a
DAG tile size of 128. Record helper expansion size, constant-source count,
compiler/hardware/ISA, axes/dtype, worker count, geometry, page/source state,
latency statistics, source/copied bytes, virtual span, backing, metadata and
retained peaks. Correctness gates timing; no measured speed claim follows.

No user-facing behavior question remains open. The specifications remain
Proposed; old runtime tests or kernel page-window tests alone
cannot establish conformance.

Mathematical oracles establish expected values and logical source mappings. Public compile/execute checks separately establish helper lowering and fixed runtime behavior.
