---
spec_schema_version: 1
id: FMT-02
kind: shared_operator_contract
category: 02-format-color
status: Proposed
document_maturity: D1_draft
implementation_status: not_implemented
clarification_status: complete
repository_branch: ops-specs
inspection_commit: 1b403fb9
---

# FMT-02: channel assembly and concatenation family

Implementation update: package 0.20.0 [removes the legacy format/color code](FMT_legacy_retirement.md).
Descriptions of old registrations below record the inspected baseline only;
those keys and pixel callbacks are no longer available. This target remains
Proposed and unimplemented.

This is the operator-local clarification for the Proposed FMT-02 family. Inherit
[FMT-common](FMT_common_contract.md), the NUM numerical/resource conventions
referenced there, and the accepted
[kernel storage contract](../../../kernel-specs/Tensor-Storage-and-Region-Access.md).
Confirmed decisions describe the target; they do not register or implement an API.
Its independently specified members are
[FMT-02A assembly](FMT-02A_assemble_channels.md),
[FMT-02B concatenation](FMT-02B_concatenate_channels.md) and
[FMT-02C mapped assembly](FMT-02C_assemble_mapped_channels.md).

## Purpose and inherited boundaries

FMT-02 assembles separately processed components or concatenates channel groups.
Its inputs may come from [FMT-01](FMT-01_channel_extraction_contract.md), generic
numeric processing, or explicitly imported image planes. One requirement is a
split/reassemble identity with explicit channel correspondence. FMT-02C selects
source components into explicitly mapped result slots; single-tensor swizzle,
replacement and generated constant channels retain their FMT-03 home. Association and
alpha editing belong to FMT-04/05. Merely joining alpha with color planes does
not request premultiplication or color conversion.

The target uses tensors and composable metadata. Image storage is planar and
uses the one tile geometry of its DAG. Source metadata authority, call-local
override, raw interpretation and consumer-scoped validation follow FMT-common.
These modes cannot change physical addresses by relabeling a tensor.
Ordinary numerical policy is inherited rather than reopened in this interview.

## Confirmed family and metadata behavior

FMT-02A stacks single-component inputs by introducing a channel axis. FMT-02B
concatenates existing channel axes. Input-list order defines the order of the
assembled components or concatenated channel blocks; B preserves the order
within each block. Neither member silently reorders its inputs.

FMT-02C supplies explicit source-to-destination channel mapping and optional
per-destination-component semantic definitions. A source RGBA channel 0 may
supply destination channel 2 described as Lab b*. Sample bytes are copied,
not transformed from RGB to Lab or rescaled. The explicit destination definition
authorizes reinterpretation of its specified fields without source mutation or
global raw mode. A/B retain their confirmed ordered behavior.

Every destination slot must have exactly one source; all slots 0..M-1 must be
covered, where M is the nonzero mapping-entry count. Source channels may be
omitted or reused. Duplicate destinations, holes and out-of-range destinations
fail static validation. There is no implicit fill, blend or accumulation;
constants are explicit preceding producers connected as sources.

Corresponding nonchannel extents must match exactly. An extent of one does not
request broadcasting. Spatial resampling or explicit broadcast, when needed,
precedes assembly. Axis correspondence follows the coordinate rules below.

Output descriptions retain and remap applicable per-component names, roles,
units and source interpretations. Complete output color groups must be supplied
explicitly; three input channels do not imply RGB. A Lab color group and an
independent alpha plane may coexist in one output tensor. Structural description
checks ensure declared groups refer to compatible components, but do not make
assembly a consumer of pixel-domain color or alpha constraints. A later semantic
consumer validates the samples it actually uses. Assembly grants no new
sample-validity certificate merely because a complete output description exists.

## Current implementation and explicit migration boundary

At the inspected commit, the legacy
`channel.merge`
registers two to four rank-two Float32/Float64 inputs, a required `semantic`
parameter, and a typed output with a channel-last shape. This registration is
not the proposed FMT-02 family. Package 0.19's
[registry gates](../../../../src/lib/plugin/operation_registry.cpp) reject legacy
image outputs before their callbacks; generic non-image uses remain separate.
No compatibility with its former image numerical rules is selected here.

The kernel now implements a physical PlanarImage owner and bounded page windows.
Its current CPU operation subset does not establish arbitrary channel assembly:
the FMT metadata codec, missing integer dtypes, shape-changing region mapping,
and actual member registrations still require implementation work. Existing
Whole or Elementwise support is not evidence of an exact FMT-02 mapping.

## Clarification decisions

| Topic | Selected behavior | State |
| --- | --- | --- |
| Family members | A inserts a channel axis; B concatenates existing channel axes; C assembles explicitly selected sources into destination slots. A/B retain input-list order. | Confirmed |
| Nonchannel dimensions | Corresponding extents must match; broadcast and resampling are explicit preceding operations. | Confirmed |
| Output metadata | Remap applicable component descriptions; complete color/group descriptions are explicit. Validate structural compatibility without introducing sample-domain color validation. | Confirmed |
| Axes and shape | A: explicit inserted axis, no automatic squeeze. B: separately resolved input axes and explicit output axis; corresponding remaining axes match in order. | Confirmed |
| Arity and static structure | Allow one input, reject zero inputs; input list, axes and channel counts are static. | Confirmed |
| Duplicate names/roles | Preserve duplicates without automatic renaming; a later named selector must still resolve uniquely. | Confirmed |
| Metadata conflicts | Explicit target semantics are a standard A/B/C capability, including Gray/Black-White input components. Unredefined applicable fields still follow respect. | Confirmed; supersedes the earlier requirement for a separate source override |
| Regions and invalidation | Exact output-channel/region support and corresponding forward dirty mapping; unused inputs have no payload request. | Confirmed |
| Coordinate metadata | Check compatible descriptions for corresponding nonchannel axes in respect mode; raw/override expresses deliberate index-based reinterpretation. | Confirmed |
| Output storage | auto/view/materialize with default auto; views require a legal retained mapping, otherwise auto materializes. | Confirmed |
| Explicit destination mapping | Support explicit source component to output slot mapping and per-output-channel semantic descriptions. | Confirmed |
| Mapping member placement | C provides mapped assembly; A/B remain ordered forms. | Confirmed |
| Explicit destination reinterpretation | Explicit target channel/group fields authorize local reinterpretation in A/B/C without separate source override/raw; C may also define them in mapping records. | Confirmed |
| Mapping completeness and reuse | Every output slot has one source, with no holes or duplicate writes; source omissions/reuse are allowed. | Confirmed |
| C input structure | Permit explicit component and channel-tensor sources with equal nonchannel shapes. | Confirmed |
| C source selectors | Static index/name/role, exact unique resolution; raw permits index only. | Confirmed |
| Standard target semantic assignment | Explicit output channel/group descriptions authorize corresponding reinterpretation in A/B/C, without an additional source override/raw and without implicit numeric conversion. | Confirmed |

## Confirmed axes and coordinate rules

A receives equal-shape single-component tensors with no effective channel-axis
designation. Its required static axis a inserts a new dimension at any position
0..r in a rank-r input. It does not silently remove a singleton channel axis.
An input such as [H,W,1] with a declared channel axis must first undergo explicit
axis removal, or use B. The inherited positive rank-1..8 bounds imply that A
accepts ranks 1..7 and produces ranks 2..8; scalar/rank-zero input is not added.

For n inputs of shape S, A's output shape is insert(S,a,n). If j is an output
coordinate and erase(j,a) removes its channel coordinate, the exact copy rule is:

```
Y[j] = X[j[a]][erase(j,a)]
```

B resolves each input channel axis ai separately from effective metadata or an
explicit assertion, with FMT-common override/raw behavior. It never guesses an
axis from shape. Removing ai from every input shape must yield the same ordered
shape S; there is no implicit permutation of the remaining dimensions. Required
static output_axis a inserts the combined channel extent into S. Inputs and
output have the same rank, within the inherited rank-1..8 bound.

Let ci be the channel count of input i, p0=0 and p(i+1)=pi+ci using checked
arithmetic. The output shape is insert(S,a,pn). For output channel k=j[a], find
the unique input i for which pi<=k<p(i+1). Then:

```
q = insert(erase(j,a), ai, k-pi)
Y[j] = Xi[q]
```

The equations describe element-byte copies, with no rescaling, dtype promotion,
rounding or alpha/color arithmetic. All inputs must share a dtype; dtype changes
are explicit preceding operations. Supported target dtypes follow FMT-common;
unimplemented native integer widths remain dependencies. Float signed zeros,
Inf and NaN payloads are preserved exactly when successfully supplied.

For A, [H,W] planes may produce [C,H,W] or [H,W,C]. For B, [3,H,W] and
[H,W,1] may produce [H,W,4]. Physical planar addressing is separate from this
logical axis choice, and no local tile geometry is introduced.

All three members permit a single input and reject an empty input list. A single
input to A still inserts an axis of extent one. A single input to B preserves
channel order and may change the logical position of that axis; it is a full
shape identity only when the output axis agrees. Input arity, extents, axes and
channel order are fixed at compile time, not runtime scalar controls. Host graph,
port and resource limits still apply to the requested static invocation.

## Confirmed metadata conflict handling

Duplicate channel names or roles are allowed and retained exactly; no suffix or
automatic renaming is added. A subsequent FMT-01B selector still needs a unique
match. Output channel positions remain unambiguous for index-based selection.

Explicit target channel or complete color-group descriptions authorize the
corresponding component reinterpretation in A/B/C. No additional source override,
raw mode or separate assign node is required. A Gray or Black/White plane may
supply an explicitly described RGB, alpha, Lab or other output component. Three
Gray planes can be assembled by A with an explicit RGB group. The group defines
the target coordinate roles and color interpretation on its declared channel
indices; it does not imply a numerical color transform.

For example, explicitly assigning one sRGB output group to copied R/G/B samples
from different source interpretations is deliberate relabeling and is allowed.
It is not a source-space conversion. Without such explicit target definitions,
respect rejects incompatible applicable descriptions rather than inventing a
common color interpretation. Source overrides remain available to change the
effective input interpretation, including selector resolution, independently
of destination assignment. A destination definition never rewrites the source.

Untagged components may receive meaning from an explicit output description.
All still-applicable fields not redefined by the target obey respect. Incompatible
model-dependent source fields are removed when a new model makes them
inapplicable. Effective output descriptions must remain internally coherent;
target fields cannot contradict inferred shape, dtype or structural axes. A
target group and explicit component entry defining the same field inconsistently
fail, rather than applying an undocumented precedence.

Every successful result still copies original element bits. In particular,
UInt8 255 does not become Float32 1.0 and binary values are not automatically
expanded to another range. Cast, interval decoding, transfer or actual gray/XYZ/
Lab conversion are explicit preceding or subsequent operators as appropriate.
Packed one-bit file data belongs to the codec boundary, not a new tensor dtype.

Where reinterpretation changes a field, an incompatible old field is replaced
or removed rather than retained as a second authoritative output interpretation.
Applicable per-component descriptions outside the replacement remain descriptive
and do not certify sample validity. Referenced profiles/resources retain their
owners. Inputs and other consumers retain their original metadata.

Respect checks explicitly supplied coordinate roles, units, origins and sampling
grids on corresponding nonchannel axes. Conflicting descriptions fail before
payload reads even when extents agree. Missing descriptions are not invented;
retain a common coordinate description only when supported for the output, or
supply it explicitly. There is no automatic spatial alignment. Override changes
the selected input interpretation before these checks; raw applies the index
mapping without consuming semantic coordinate labels. Conflicting source labels
are not propagated as a common output grid in raw mode.

## Interface and support matrix

A/B/C target the default registry as separate primitives, with ordered repeated
tensor inputs `inputs[0..n)` and one tensor output `values`. They do not consume
a tensor collection as an opaque input; the authoring layer expands the selected
collection entries into a static ordered input list. n>=1, subject to the shared
kernel's compile-time port/graph limits and explicit resource admission. No
FMT-specific legacy two-to-four-input bound is inherited. All logical counts and
channel prefix sums use checked arithmetic and the inherited 2^40-element bound.
C specializes the static mapping/selection interface in its member specification.

All parameters below are static. The types specify logical authoring values;
ordered optional axes and indexed override descriptions require a canonical
shared encoding before implementation. They are not undocumented encodings in
the old `semantic` String parameter or new working runtime parameter kinds.

| Parameter | Type / range | Default and conditions |
| --- | --- | --- |
| `metadata_mode` | String: respect, raw, override | respect; authoring helpers serialize it explicitly. |
| `input_overrides` | Static index-to-description map using the shared tensor metadata schema | Only for override; at least one valid input index. Replace the supplied inputs' effective descriptions for this call; other inputs retain theirs. |
| `output_description` | Optional shared tensor metadata description, including per-channel definitions and explicit groups | Explicit channel/group fields authorize corresponding target reinterpretation in A/B/C. Match inferred shape/dtype/axes, propagate omitted applicable component fields and discard inapplicable model-dependent fields. No group or numerical conversion is guessed. |
| `layout` | String: auto, view, materialize | auto; direct nodes provide it explicitly. |
| A: `axis` | Int64 in [0,input_rank] | Required; insertion position, no negative shorthand. |
| B: `input_axes` | Optional ordered list of n optional Int64 axis values, each in [0,input_rank) | Omitted entries require a unique effective metadata axis. Supplied entries assert agreement in respect/override; all entries are required in raw. |
| B: `output_axis` | Int64 in [0,input_rank) | Required; no implicit channel-last default. |

For A, an existing effective channel axis is an error, including extent one.
Raw does not convert a physical image backing into another layout by clearing
labels. Image inputs and outputs must satisfy the kernel storage contract at
their actual address maps; new interpretations may require explicit conversion
or materialization. Output image axes/groups must be structurally established,
not inferred from rank or channel count alone. Generic tensor output remains
possible without a complete image/color description.

| Dimension | Strict / CPU accelerated target | Not promised |
| --- | --- | --- |
| Dtype | UInt8, UInt16, Int8, Int16, Int64, Float32, Float64, same across all inputs/output | Missing native widths require implementation; no implicit cast or Float16 extension. |
| Rank | A: 1..7 to 2..8; B: 1..8 to same rank; C: mixed component/channel tensors with common nonchannel rank r, output rank r+1 in 1..8 | Rank zero, empty dimensions and dynamic arity are excluded; C with r=0 has channel-vector inputs only. |
| Values | Exact element bytes, including HDR/signed and IEEE special patterns | No ULP relaxation for copying; no color/alpha-domain scan. |
| Input layout | Legal generic strided tensors and canonical planar image windows | Raw does not admit an interleaved image without explicit import. |
| Output | auto/view/materialize with exact requested coverage | No cross-owner image representation or implicit packed ROI export. |
| Backend | Portable CPU and named Apple Silicon/x86-64 CPU profiles with identical copy semantics | GPU is unpromised; unavailable named backends fail explicitly. |
| Execution | Exact mapped components/regions with no halo | Existing Whole implementation is not a conforming replacement. |

## Data, descriptor and dirty mapping

Let Q be the requested output footprint, preserving every disjoint box and its
global coordinates. For A, input i receives exactly the projection of
Q intersected with j[a]=i, with axis a removed. For B, input i receives exactly
Q intersected with pi<=j[a]<p(i+1), with output axis a removed and source axis ai
inserted at channel coordinate j[a]-pi. Empty intersections request no input
payload. Adjacent regions may coalesce only when their union is exact; do not
bridge gaps or round coverage up to tiles, rows, channel tuples or pages.
C uses its destination-indexed mapping table as specified by its member contract;
source support is the exact union over requested mapping entries, including
deduplication when multiple destinations select the same source component.

There is no Control payload and no additional sample Validation domain. Static
descriptor checks cover every connected input's shape/dtype, axis and consumed
metadata even if its payload is unrequested. Explicit output grouping does not
add RGB/alpha read closure. Necessary upstream Whole computation or intrinsic
producer failures retain their own scope; a raw consumer cannot manufacture a
missing value from a failed producer.

Forward dirty mapping is the inverse insertion for A and inverse axis movement
plus channel-prefix offset for B, intersected with the observed output domain.
C maps each changed source component to every selecting destination, so its dirty
relation can have several outputs for one source sample.
Changing an unselected input channel dirties no observed output samples. Changes
to arity, shape, axis, prefix counts or consumed descriptions invalidate the
compiled descriptor/mapping; no pixel-only cache may conceal them. Coordinate
translation occurs only through the declared index formulas, never by comparing
physical addresses or grid origins.

Every output element has exactly one source. C may reuse that source for several
destinations; A/B may likewise receive aliased input ports. This is local exactness, not
a claim that a broader upstream dependency graph is exact. Failed observations
publish no partial success; separately completed observations retain their
outcomes under the inherited host contract.

## Output layout, ownership and resource behavior

Separate source planes may belong to unrelated address-space owners. A list of
those owners is not by itself one assembled image under the accepted storage
contract. Any proposed zero-copy result must establish a legal output address
map, retained ownership and authorized coverage within the applicable image
storage contract. The kernel currently supplies no general facility for
remapping arbitrary source pages into a new shared image reservation.

A split/reassemble sequence whose components retain a common source owner may
admit a view in some cases; that requires an explicit correspondence proof and
supported view representation. It must not be promised for unrelated planes.
Materialized output follows canonical planar offsets, page admission and exact
sample publication. Packed ROI export is a separate read operation.

`auto` attempts only a legal view and otherwise materializes. It must not hide a
source, metadata, cancellation or resource failure by retrying a different
layout. `view` fails with ViewUnavailable if no legal view exists. `materialize`
always creates separate result storage. A view retains its backing owner and
the mapping/metadata needed to keep every authorized access valid after source
handles or ExecutionContext retire. Complete source provenance alone is not
enough: current byte mappings, component order, storage policy and requested
coverage must also agree. Generic tensor views require a representable legal
mapping; sharing one owner does not by itself guarantee representability.

For materialized images, reserve the complete canonical output virtual span and
prepare only the union of pages intersecting requested sample bytes. One DAG
tile geometry applies to input and output. Use the kernel's edge-row padding
and page-alignment formulas; padding/gaps are not pixels, valid samples or zero
boundary values. Generic non-image materialization may use ordinary owned
requested fragments. It does not create a second authoritative image layout.

For A/B, with n inputs, rank r, Nq requested elements and F participating windows, the
reference mapping/copy work is O(n*r+Nq*r+F*r), plus metadata size, page-set
construction and actual upstream work. B walks disjoint prefix intervals in
channel order; do not scan all n sources per element. Mapping state is O(n+r)
plus admitted footprint/window records. C additionally admits its M-entry
mapping/reverse index and selector lookup structures as specified in its member.
A view copies zero sample payload but
still pays descriptor, request, window and ownership costs.

Charge Nq*d only as logical copied volume. Actual allocations include output
page capacity, temporary/staging capacity, window/metadata capacity and retained
source ancestry. Deduplicate backing shared by multiple input ports or views
using kernel accounting identity. Account simultaneous source/destination peaks,
row/page padding, and sparse coverage metadata. Virtual bytes, supplied backing
and logical samples remain separate; none is an RSS guarantee. No eager metadata
entry is allocated for every possible sample/page in an unused full-image span.

Produced pages survive until the final image owner retires; no eviction/replay
or temporary-file paging is introduced. Failed unpublished allocations roll
back without losing existing valid regions. Poll cancellation/currentness and
charge work at most every 1024 entries/elements during metadata validation,
mapping, page preparation and copy, and immediately before publication.
Initial member registrations disable optional sample-only completed-result
caching; retained-owner sharing and dependency evidence remain enabled. A later
cache requires the same physical-layout, owner and metadata proof as a fresh call.

## Reference algorithm and permitted optimizations

1. Validate static parameters, all input descriptors and effective descriptions;
   infer axes, output shape, B prefix counts or C's resolved mapping table, and
   the explicit/projected output component/group descriptions.
   This phase reads no payload and performs no color-domain validation.
2. Partition Q by input channel ownership using the exact formulas above.
   Declare per-port Data and Descriptor support and acquire only needed windows.
3. For auto/view, prove output view representability. If auto requires copying,
   or materialize is selected, admit the full result reservation and prepare
   destination pages before giving a bounded write window to the operation.
4. Enumerate each requested input-owned region and copy d bytes per sample into
   its mapped output position, or publish the proven equivalent retained view.
5. Check cancellation/currentness and publish the exact requested coverage and
   coherent output metadata only after all required work succeeds.

CPU optimizations can coalesce consecutive source and destination row runs only
when their complete byte intervals are both authorized and contiguous. SIMD
loads must not cross into unrequested channels, row padding, footprint gaps or
unprepared pages. Independent destination observations may run through host
scheduling; no private pool, arithmetic approximation or full-input gather is
needed. Repeated references to one source still map to their separate requested
output channels without mutating the source.

## Error mapping

| Failure | Phase | Status / scope |
| --- | --- | --- |
| Zero inputs, malformed mode/description, missing or out-of-range axis, illegal rank insertion, forbidden override, conflicting coordinate/component assertion | Compile/direct preflight | InvalidArgument / InvalidDomain, schema origin. |
| Dtype mismatch, incompatible nonchannel extents, unsupported tensor rank/type or channel-table length | Compile/direct preflight | TypeMismatch / None. |
| Checked shape/prefix overflow, host port/graph capacity or resource/work admission failure | Inherited validation/admission phase | Preserve the shared NUM/kernel status rather than truncate or wrap. |
| Forced view cannot represent the requested result | Requested observation evaluation | InvalidArgument / InvalidDomain; diagnostic ViewUnavailable. |
| Missing source coverage, required upstream failure, cancellation, stale plan, unsupported backend or allocation failure | Respective inherited phase | Preserve inherited code/reason/origin and observation scope. |

Diagnostics identify the input ordinal, relevant axis/group/field and operation.
Do not invent a pixel coordinate for static descriptor errors. NaN/Inf or an
out-of-range alpha alone is not an assembly failure; this operator does not
consume those sample invariants.

## Acceptance and performance plan

The member files provide explicit input/output fixtures. Use an independent
coordinate scatter oracle and independent byte-address evaluator rather than
the production mapper. Strict and accelerated profiles must produce identical
sample bits; descriptor/discrete results are exact. Include these cases:

- Split/reassemble with original ordering and explicit original grouping;
  independently owned planes; A single-input insertion; B single-input identity
  and channel-axis movement; B rank-one vectors and all legal axis positions.
- Same-dtype limits and raw bit fixtures; reject mixed dtypes, unequal extents,
  implicit broadcast, invalid axes, rank-zero and A rank-eight inputs.
- Preserve duplicates; named extraction subsequently reports ambiguity. Reject
  unredefined respect-mode interpretation/grid conflicts; accept explicitly
  assigned target semantics for Gray/Black-White and mixed-source components in
  A/B/C. Verify only declared destination fields are reinterpreted, with exact
  bits and no implicit numerical conversion. Source override/raw affects only
  the invocation; destination assignment does not change source selectors/grids.
- C covers mixed source structures, selector name/role/index equivalence and
  ambiguity, source omissions/reuse, duplicate/hole rejection, mapping order,
  destination fan-out dirty support and explicit R-to-Lab-b* reinterpretation.
- Full versus offset/disjoint ROI and y/x=[127,130) across four tiles for T=128;
  select one output channel and instrument that other input payloads are not
  requested. Compare Data and dirty support against the independent mapping.
- Generic positive/negative/zero strides; continuous and tiled images; differing
  legal tile configurations across DAGs and rejection of mismatched geometry
  within one DAG; raw cannot bypass the image import boundary.
- View success when genuinely representable, forced-view failure for unrelated
  owners, auto materialization, repeated source aliases, resource/metadata/work
  limits, cancellation, cache-off, and surviving result/windows after context
  teardown. Verify backing charges retire with the last owner.

Implementation delivery must run an actual public WorkflowDocument/Compiler/
ExecutionContext workflow through new member registrations, document commands
and expected bytes, and exercise a partial channel ROI. Conceptual member DAGs
and legacy test passes are not runtime evidence for the new interface.

Benchmark correctness first, then A assembling four independent Float32
[4096,4096] planes and B concatenating RGB plus alpha for the same spatial
extent. Compare full requests, one-channel requests and the cross-tile ROI.
Measure common-owner view, forced materialization and unrelated-owner auto
separately. Record revision/compiler, hardware/ISA, worker count, dtype, axes,
ROI, DAG geometry, page size, row pitch, initial source/page state and cache mode;
report median/tail latency, copied/source bytes, virtual reservation, backing,
metadata/staging peak and retained owners. No throughput guarantee is inferred.

## Sources and remaining implementation work

Repository contracts and source links above establish the inspected baseline.
The byte-copy and index equations are specified directly; no external commercial
pixel-compatibility claim is made (U: unverified under the template's convention).
No operator-local behavior question remains open. Public encoding of shared
metadata, optional axis lists and indexed overrides, missing native dtypes,
shape-changing exact dependency/publication support, legal result views and the
concrete member registrations remain implementation dependencies. Runtime and
performance results must be supplied by that delivery, not inferred from this
Proposed specification. This clarification changes documentation only.

Documentation-level verification compared independent coordinate scatter and
gather equations for 105 A cases, 141 B cases and 204 C cases across legal ranks
and axis positions. C checks included mixed component/channel sources, repeated
selection, selected-source support, dirty fan-out, and invalid destination maps.
These 450 cases check the specified index equations; they neither execute a
registered FMT operator nor establish physical storage or metadata conformance.

## Canonical image alpha constraint

Under the 2026-09-23 FMT-common revision, explicit complete image descriptions
use straight colors and internal alpha indices only. A/B/C target assignment
can reinterpret copied values but cannot establish a persistent external alpha
relation or declare a canonical premultiplied image. Copying a premultiplied
boundary component does not numerically recover straight color. Preserve its
component provenance unless explicit target reinterpretation is requested.
Missing alpha companions are not resolved through another tensor's owner.
