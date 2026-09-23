---
spec_schema_version: 1
id: FMT-03
kind: shared_operator_contract
category: 02-format-color
status: Proposed
document_maturity: D1_draft
implementation_status: not_implemented
clarification_status: complete
repository_branch: ops-specs
inspection_commit: 1b403fb9
---

# FMT-03: channel reordering and replacement

Implementation update: package 0.20.0 [removes the legacy format/color code](FMT_legacy_retirement.md).
Descriptions of old registrations below record the inspected baseline only;
those keys and pixel callbacks are no longer available. This target remains
Proposed and unimplemented.

This operator-local draft inherits [FMT-common](FMT_common_contract.md), its
NUM numerical baseline and the accepted
[kernel storage contract](../../../kernel-specs/Tensor-Storage-and-Region-Access.md).
The purpose is to edit a base tensor's channel arrangement or selected channels.
The clarified members are [A swizzle](FMT-03A_swizzle_channels.md) and
[B replacement](FMT-03B_replace_channels.md), both compile-time authoring
compositions. The target is Proposed; this document registers no runtime operation.

## Relationship to earlier families

[FMT-01](FMT-01_channel_extraction_contract.md) extracts single components.
[FMT-02](FMT-02_channel_assembly_contract.md) assembles ordered single components,
concatenates channel blocks or creates a result from explicit multi-source maps.
Its A member fixes input i to output i and independently assigns component
semantics; changing that metadata does not reorder samples. Its C member can
perform actual source/destination remapping.

FMT-03's proposed surface specializes single-base channel editing. Overlapping
copy maps need not acquire a second numerical definition: reuse the same exact
coordinate and metadata principles where applicable. Association arithmetic and
alpha-specific policies remain FMT-04/05 responsibilities; merely editing a
channel must not silently run those operations.

Physical output storage must follow the accepted planar image contract. A
channel-count change does not authorize a new per-operator tile geometry,
cross-owner image storage or implicit packed output. Interpretation and actual
byte mapping remain separate.

## Inspected implementation baseline

The legacy
`channel.swizzle`
registers a rank-three Float32/Float64 tensor, a static String `indices`, and an
output whose final extent is the index-list length. The
callback copies source
element bytes in that order. Repeated selections are supported; implicit
constants and arbitrary-axis replacement are not established by that code.
Package 0.19 rejects its legacy image invocation path. Generic non-image behavior
is separate evidence and is not a conforming implementation of the new family.

[NUM-03A constant](../../01-numeric/op_specs/NUM-03A_constant.md) accepts a typed
scalar Value of shape [1] and repeats its exact bits. It is distinct from a
static untyped floating literal. Any FMT constant design inheriting it must
state how the scalar is supplied. Inherit its numerical copy semantics only;
its Whole/dense layout and legacy metadata validation do not supersede the
current FMT consumer/storage contract.

## Confirmed decisions

| Topic | Selected behavior | State |
| --- | --- | --- |
| Members | A lists every output slot for single-base swizzle/subset/repeat/constants; B changes selected slots and preserves all others and the base channel count. | Confirmed |
| A metadata | Component descriptions follow source selection; retain only groups with an unambiguous valid remapping. Explicit target descriptions permit reinterpretation, as in FMT-02. | Confirmed |
| Constant values | General typed scalar constants under NUM-03A numerical rules, rather than only 0/1. | Confirmed |
| Constant source form | Connected same-dtype shape-[1] scalars, plus authoring sugar for typed literals; scalar values may change between Runs while structure stays static. | Confirmed |
| B metadata | Preserve the destination slot's interpretation by default, including when replacement is a Gray plane; explicit target fields may redefine it. | Confirmed |
| B sources | Original base components, external component/channel tensors and explicit scalar constants; all replacements read original inputs simultaneously. | Confirmed |
| Execution and representation | Inherit exact mapped requests and auto/view/materialize from the previously selected FMT channel contracts; specify constants and overwrite exceptions below. | Inherited |
| Implementation identity | Compile-time swizzle/replace interfaces lowered to FMT-02C and conforming constant sources, without duplicate native copy operators. | Confirmed |

## Established behavior and inherited execution rules

A takes a base tensor and a static ordered output-slot list. Each slot selects
a base component or an explicit constant. It may reorder, omit or repeat source
channels. B preserves the base channel count and replaces selected destination
slots, leaving unlisted slots unchanged. Both publish immutable results; neither
mutates the input. A nonempty result channel axis is required.

Selections use the family's generic tensor/metadata interpretation, not a fixed
HWC assumption. Source component bytes are preserved, including nonfinite IEEE
patterns. Assigning an output role does not request numerical conversion or
automatic alpha association. As established in FMT-02, explicit target component
or group descriptions authorize reinterpretation without additional source raw
or override; physical layout and nonchannel grid constraints still apply.

For A's default metadata, selected descriptions follow their source components.
RGB indexed [2,1,0] therefore produces B/G/R slot descriptions, with a correctly
remapped RGB group if its dependencies remain unambiguous. A complete color
group cannot be fabricated when required components are missing or ambiguously
duplicated. Applicable component descriptions survive without a sample-validity
certificate. Explicit output descriptions can supply a new coherent grouping.

Exact Data support is the union of the source samples selected by the requested
output footprint. Constants require only their own value source, not a read of
base pixels. Unchanged B slots read the corresponding base samples. Replaced
slots do not read overwritten base values unless another requested mapping also
selects them. All static shape/dtype and consumed metadata checks remain in
Descriptor support. There is no new full-color or alpha sample validation scan.
Necessary upstream Whole work and intrinsic producer failure retain their scope.

An output alias requires a legal retained mapping under the kernel contract.
Sharing an owner or having equal sample values alone is insufficient. Constant
zero-stride aliases may be valid generic tensors, but do not automatically
satisfy canonical image row/tile storage. Materialized images reserve their
whole virtual span and provide only the pages required by requested samples.
All output paths retain the DAG's common tile geometry and exact valid coverage.

## Logical interface and shape

Both authoring interfaces return one tensor handle `values`. The primary input
is `base`, a positive rank-1..8 tensor with an explicitly resolved channel axis a.
Let its channel extent be C and let S be the ordered shape with axis a removed.
Axis resolution follows FMT-01/02: attached effective metadata or a static axis,
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

All inputs, including scalars, have the base dtype. Selected target types are
UInt8, UInt16, Int8, Int16, Int64, Float32 and Float64; missing native widths are
implementation dependencies. Counts/products and resource limits follow the
shared NUM/FMT contracts, including the 2^40-element logical bound. No implicit
cast, rounding, range conversion or NaN canonicalization occurs.

## Parameters and dynamic values

These are logical authoring parameters, not new registered ABI parameter kinds.
The shared metadata/map encoding must exist before implementation.

| Field | Meaning / default |
| --- | --- |
| `axis` | Optional static Int64 in [0,base_rank); required when effective metadata has no unique channel axis or mode is raw. |
| `metadata_mode` | respect by default; raw/override follow FMT-common. |
| `input_overrides` | Conditional static per-input effective descriptions, only in override mode. Do not change physical storage. |
| `output_description` | Optional explicit component/group definitions. Assigning target semantics does not move samples or alter their bytes. |
| `layout` | auto by default; view/materialize have the FMT-02 meanings and failures. |
| `profile` | strict by default, or the named Apple Silicon/x86-64 CPU profile. Lower to the corresponding primitive keys; unavailable profiles fail explicitly. |
| A: `slots` | Static ordered list of base selectors or constant references; defines every output slot and M. |
| B: `replacements` | Static list of unique target selectors and source expressions. External sources have explicit component/channels/scalar structure and axis information. |
| Scalar source | Connected same-dtype Value of shape [1], read at index zero. Its value may change between executions; its dtype/shape and map structure do not. |
| Typed literal | Authoring shortcut for an immutable scalar source with an explicit dtype and exact represented bits, using shared numeric construction rules. |

A scalar that supplies a channel is constant over that channel's spatial domain,
not necessarily constant across Runs. An ordinary shape-[1] spatial tensor does
not implicitly become a constant; the source expression must say so. Literal
construction/validation follows NUM conventions and never silently casts a
different scalar dtype to base dtype. Preserve Int64 values and all floating
special bits without routing them through an untyped Float64 parameter.

## Metadata preservation and target assignment

A retains an existing group only if all of its required components and referenced
companions have a unique, complete output remapping. Repeated members, omitted
members or missing association companions remove the ambiguous/incomplete group
description while retaining applicable component meaning. Do not select the
first of duplicate R channels to invent one RGB group. Explicit output grouping
may establish an intended new group. Constants receive no color/alpha role by
position alone; provide a target component or group definition when needed.

B starts from base destination descriptions. Choosing a target slot explicitly
assigns that slot's semantics to the copied replacement, including Gray or
Black/White source values. No extra raw/override is needed for this assignment.
Unlisted slots retain their sample bits and component descriptions. Explicit
target fields may redefine replaced slots or define coherent affected groups;
they must not silently relabel unlisted components. Use a separate explicit
assignment or FMT-02C when unlisted component interpretations must also change.
Drop any group/association description made structurally inapplicable by an
explicit redefinition; retaining a known-invalid group is forbidden.

Target semantics take precedence over conflicting source component meaning only
for explicitly assigned fields; discard source fields that become inapplicable.
Contradictory target definitions fail preflight. Coordinate roles, units, origin
and sampling-grid consistency follow FMT-02; choosing an R or alpha destination
does not authorize an implicit spatial shift or resample. Source overrides/raw
remain available for deliberate source-grid reinterpretation.

Dropping alpha by selection does not flatten against a background or transform
color samples. Canonical images use straight color; replacing alpha preserves
those colors, including hidden finite color at zero alpha. Complete image
metadata uses internal alpha indices only. For explicitly described noncanonical
numeric boundary payloads, retained premultiplied component meaning is provenance,
not a canonical image declaration or persistent external alpha relation.
FMT-04 owns boundary association arithmetic; FMT-05 owns semantic internal-alpha
editing. This family only copies/fills and adds no sample-validity guarantee.

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

Every equality is a byte copy. Data demand is the exact union of these source
coordinates for requested j, deduplicating repeated sources. A scalar reads
index zero only if at least one requested sample selects it. Disjoint regions
remain disjoint; tile/page rounding affects backing admission, not logical Data.
Descriptor checks cover all connected input shapes/dtypes and consumed metadata;
no extra sample Validation or Control payload is introduced.

Forward dirty mapping inverts each spatial selection into every selecting output
slot and intersects the observed footprint. A scalar value change dirties every
observed sample in each slot that selects that scalar, and no other slots.
Metadata, selector or map changes invalidate descriptor/mapping evidence. For
B, an overwritten base sample has no effect unless another effective source
expression reads it. This source-based rule also handles simultaneous swaps.

Only requested constant values participate in runtime numeric reads. A malformed
static literal or inconsistent descriptor fails preflight even for an unrequested
slot. Required upstream failures, resource exhaustion and cancellation retain
their own scope; consumer selection cannot undo already required producer work.

## Compile-time lowering and execution identity

A builds one FMT-02C row per output slot. B begins with C base-to-same-slot rows
and substitutes the explicit replacements, retaining untouched rows. Supply C
with an explicit output description computed from A/B's distinct metadata rules;
do not accidentally use C's default source-description propagation for B.
Both preserve base axis a as C's output_axis.

For a scalar source, generate a conforming scalar-fill source of nonchannel shape
S, with exact bits and generic view storage. For nonempty S, this may use
NUM-03A's scalar-copy behavior. If S is empty, pass the original shape-[1] scalar
as a channel-vector source with axis 0, avoiding a forbidden rank-zero temporary.
Typed literals lower to equivalent scalar sources. Scalar-fill intermediates
are generic data; zero-stride storage must not be advertised as a canonical
planar image. The final C output establishes the required image storage.

Preserve Descriptor dependencies and static validation for the original base and
all connected sources, even if every requested output is constant. Expand only
the static mapping, not one node per pixel. Retain named authoring provenance in
diagnostics. A/B add no independent native operation keys; their profile selects
the matching FMT-02C and constant-provider keys. Existing legacy `channel.swizzle`
does not become an alias. Constants/helpers must obey the selected FMT metadata
rules rather than silently restoring legacy typed-scalar validation.

Lowering is conforming only when view availability, exact Data/dirty support,
output metadata, ownership, errors and publication match these definitions.
Inlining/fusion may remove intermediates if it preserves those observations.
Do not fall back to an old Whole packed-image operation to make the graph run.

Use collision-aware graph ID allocation and transactional expansion as in
[FMT-01C](FMT-01C_split_channels.md): verify supplied descriptors against actual
inference, stage new declarations/nodes/handles, and leave the caller's graph
unchanged if expansion fails. Do not execute an input producer to discover a
static channel count. Check the current host's graph/port/root limits before
appending; count actual literal/provider and fill nodes as well as the C node.
Reuse a fill source for repeated references to the same scalar when identity
and semantics agree. The composition introduces no independent dtype or ABI.

## Storage, resources and failures

Inherit FMT-02's checked virtual span, page-set admission, row padding, page
alignment, retained views and immutable publication. Auto materializes if no
legal view exists; forced view fails rather than returning an incompatible
cross-owner or zero-stride image. Unrequested sample slots do not become valid
merely because a neighboring requested slot supplied the same page.

For n sources, rank r, M effective rows, Nq requested samples and F windows,
post-selector mapping/fill work is O(n*r+M+Nq*r+F*r), plus exact footprint union,
page preparation and actual upstream work. Admit O(M) static mapping/reverse
indices and bounded selector/descriptor structures. Resolve exact name/role
matches once, charging lookup/string capacity as in FMT-02C. Constant source
backing can be scalar-sized, while a canonical materialized constant image still
charges its actual pages. Account retained ancestry, simultaneous inputs/output,
window metadata and literal/scalar owners independently of logical copied bytes.

Poll cancellation/currentness at most every 1024 metadata/map/copy entries and
before publication, or the tighter inherited primitive bound. Use host scheduling
for disjoint destinations; no private thread pool or mutable constant cache.
No result is published partially on failed observation. Generated pages and
leases retire with their final owners. Inherit FMT-02's optional sample-only
cache restriction and capacity/work/source failure attribution.

| Failure | Phase | Outcome |
| --- | --- | --- |
| Empty A slots, duplicate B targets, invalid axis/index/name/role, malformed literal/source kind or contradictory target description | Authoring/compile/direct preflight | InvalidArgument / InvalidDomain, schema origin. |
| Rank/dtype/nonchannel-shape mismatch, scalar shape other than [1] | Preflight | TypeMismatch / None. |
| Checked count/graph/map/metadata/work capacity, unavailable forced view or backend | Inherited phase | Preserve FMT-02/NUM/kernel status and diagnostic. |
| Missing source coverage, required producer failure, cancellation or stale plan | Requested observation | Preserve original failure category/scope; no fabricated success. |

Diagnostics identify the helper/member, output slot, source port/selector and
offending static field. Neither nonfinite sample bytes nor alpha outside [0,1]
alone fail a copy/fill operation. No commercial pixel-equivalence claim is made.

## Acceptance and implementation dependencies

Member files specify analytic fixtures and conceptual public workflows. Verify
with an independent coordinate interpreter, byte oracle and finite-set Data/
dirty evaluator, not with the production lowerer. Cover identity, partial/disjoint
ROI, source reuse, channel-axis positions, constant-only requests, simultaneous
swaps, overwritten-value nonreads, target semantics, view/materialization and
special-value bits. Validate helper lowering against the mathematical expressions
and retain original source descriptions after execution.

Implementation delivery must include an actual public compile/execute example
with checked output, commands and an offset channel ROI. FMT-02C, canonical
metadata/map encodings, missing integer widths, conforming generic constant
sources and scalar-plus-planar dependency/publication support are prerequisites.
The current CPU planar callback subset does not implement this composition.

Benchmark Float32 [4096,4096,4] base data for A BGR reorder, subset plus repeated
source, and opaque-alpha insertion; for B one external-plane replacement and
scalar-alpha fill. Compare whole, one-channel and y/x=[127,130) requests for a
DAG tile size of 128. Record helper expansion size, constant-source count,
compiler/hardware/ISA, axes/dtype, worker count, geometry, page/source state,
latency statistics, source/copied bytes, virtual span, backing, metadata and
retained peaks. Correctness gates timing; no measured speed claim follows.

No user-facing behavior question remains open. The specifications remain
Proposed/not implemented; old runtime tests or kernel page-window tests alone
cannot establish conformance. No runtime or benchmark result is claimed here.

During clarification, an independent byte-coordinate scatter/gather check passed
164 rank/axis/map combinations, including scalar-only source support, overwritten
base nonreads, simultaneous swaps and dirty fan-out. This checks the equations
and finite dependency sets only; it does not execute the proposed helpers,
validate their graph lowering, or establish physical storage conformance.
