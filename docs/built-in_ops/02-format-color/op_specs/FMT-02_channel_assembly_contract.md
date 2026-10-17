---
spec_schema_version: 1
id: FMT-02
kind: shared_operator_contract
category: 02-format-color
status: Proposed
document_maturity: D1_draft
implementation_status: implemented_cpu
clarification_status: complete
---

# FMT-02: channel assembly and concatenation family

The default C++ `OperationRegistry` registers twelve CPU Result-operation keys for `assemble`, `concatenate`, `assemble_mapped` and `scalar_literal`, each with `strict`, `accelerated_apple_silicon` and `accelerated_x86_64` profiles. These built-in operations use the current Result contracts; external C modules use the separately versioned Result C ABI 6 importer. Public authoring helpers are installed in `photospider/ops/format/channel_assembly.hpp` and `channel_editing.hpp`. The FMT specification remains Proposed independently of the registered implementation.

This is the operator-local clarification for the Proposed FMT-02 family. Inherit
[FMT-common](FMT_common_contract.md), the NUM numerical/resource conventions
referenced there, and the accepted
[kernel storage contract](../../../kernel-specs/Tensor-Storage-and-Region-Access.md).
The specification remains Proposed while the current runtime implementation is
described separately. Its independently specified members are
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

The family uses tensors with explicit per-invocation interpretation (FMT-common
C13). Image storage is planar and uses the one tile geometry of its DAG. An
optional per-input description supplies channel axes and named selectors; it
cannot change physical addresses. Ordinary numerical policy is inherited rather
than reopened in this interview.

## Confirmed family behavior

FMT-02A stacks single-component inputs by introducing a channel axis. FMT-02B
concatenates existing channel axes. Input-list order defines the order of the
assembled components or concatenated channel blocks; B preserves the order
within each block. Neither member silently reorders its inputs.

FMT-02C supplies an explicit source-to-destination channel mapping. A source
RGBA channel 0 may supply destination channel 2, which a later consumer's
description may call Lab b*. Sample bytes are copied, not transformed from RGB to
Lab or rescaled. A/B retain their confirmed ordered behavior.

Every destination slot must have exactly one source; all slots 0..M-1 must be
covered, where M is the nonzero mapping-entry count. Source channels may be
omitted or reused. Duplicate destinations, holes and out-of-range destinations
fail static validation. There is no implicit fill, blend or accumulation;
constants are explicit preceding producers connected as sources.

Corresponding nonchannel extents must match exactly. An extent of one does not
request broadcasting. Spatial resampling or explicit broadcast, when needed,
precedes assembly. Axis correspondence follows the coordinate rules below.

The output carries no description. A later consumer that treats the assembled
channels as a color group receives that group in its own explicit description;
three input channels do not imply RGB. A Lab color group and an independent alpha
plane may coexist in one output tensor when the consumer's description says so.
Assembly does not consume pixel-domain color or alpha constraints and grants no
sample-validity certificate. A later semantic consumer validates the samples it
actually uses.

## Current Result interface and bounds

The twelve registered keys use single-tensor Result inputs and outputs. Every connected input contributes Descriptor support, including those unused by the selected mapping. Data support includes only mapped sources in the requested output footprint. Each input descriptor contains one tensor.

The implementation supports seven native dtypes by exact bit copy, preserves the first input's Result schema id, tensor key and publication policy, sets `atomic_trailing_axes` to zero and attaches no description. Batch axes form a cell-independent prefix and must match across nonscalar sources; a scalar literal is an explicit unbatched shape-[1] source. Full input and output sample counts are bounded by 2^40, and the combined batch/cell rank is at most eight. `axis`, `input_axes` and `output_axis` address cell axes, excluding the Result batch prefix. Connected ports and static maps follow the implementation's admission limits.

For nonspatial generic tensors, a zero-copy view requires **one physical `CpuStorage` owner for all supplied parts** and **one affine address expression for the entire requested region**. The `generic_view` path checks equal owner identities, compatible nonchannel strides, a consistent channel stride and the expected byte address of every mapped part. Independently owned generic sources, inconsistent strides or addresses make the physical view unavailable.

For spatial output, the operation instead builds explicit `ResultTensorViewPiece` records for each mapped source part and authorized window. The builder checks the complete, disjoint target coverage and physical representability; pieces may retain different storage owners and Roots. A forced `view` policy reports `ViewUnavailable` when the physical mapping cannot be represented. `auto` copies only for that nonsticky result, while `materialize` directly copies the requested regions. Scalar row replication uses batches of at most 256 samples and checks work and cancellation during copying.

The public Result workflow and implementation details are in the
[architecture reference](../../../kernel-architecture/Channel-and-Color-Operations.md#structural-channel-operations).

### Spatial view construction

For each requested destination box, the operation intersects the prepared source spans with that box, resolves the affine axes and acquires an owning read window for every resulting part. It constructs all piece records before binding their borrowed `ResultTensorReadWindow*` pointers, after the windows vector is complete. This keeps the pointers valid during publication.

The operation calls `ResultBuilder::publish_tensor_view(0, box, pieces)` once, so the builder either certifies the entire destination box or leaves its previous coverage unchanged. A physical `ViewUnavailable` lets `auto` materialize that box; `view` reports failure and `materialize` uses the writer directly. Source read, schema, authorization, resource and cancellation failures retain their own errors. The C++ vector overload has no fixed 16-piece limit: Root work and metadata admission bound its size, while the separate C ABI bridge accepts 1..16 pieces.

## Clarification decisions

| Topic | Selected behavior | State |
| --- | --- | --- |
| Family members | A inserts a channel axis; B concatenates existing channel axes; C assembles explicitly selected sources into destination slots. A/B retain input-list order. | Confirmed |
| Nonchannel dimensions | Corresponding extents must match; broadcast and resampling are explicit preceding operations. | Confirmed |
| Output interpretation | The output carries no description; the next consumer supplies its own. No sample-domain color validation. | Confirmed; C13 replaces description remapping |
| Axes and shape | A: explicit inserted axis, no automatic squeeze. B: separately resolved input axes and explicit output axis; corresponding remaining axes match in order. | Confirmed |
| Arity and static structure | Allow one input, reject zero inputs; input list, axes and channel counts are static. | Confirmed |
| Interpretation conflicts | Inputs carry no descriptions, so there is nothing to conflict. Components from Gray/Black-White or differently encoded sources are combined by index; the consumer's description defines the result. | Confirmed; C13 |
| Regions and invalidation | Exact output-channel/region support and corresponding forward dirty mapping; unused inputs have no payload request. | Confirmed |
| Coordinate interpretation | Nonchannel axes correspond by index. No coordinate-label comparison or spatial alignment is performed. | Confirmed; C13 |
| Output storage | auto/view/materialize with default auto; views require a legal retained mapping, otherwise auto materializes. | Confirmed |
| Explicit destination mapping | Support explicit source component to output slot mapping. | Confirmed |
| Mapping member placement | C provides mapped assembly; A/B remain ordered forms. | Confirmed |
| Mapping completeness and reuse | Every output slot has one source, with no holes or duplicate writes; source omissions/reuse are allowed. | Confirmed |
| C input structure | Permit explicit component and channel-tensor sources with equal nonchannel shapes. | Confirmed |
| C source selectors | Static index/name/role, exact unique resolution; name/role require that input's `input_descriptions` entry, index needs none. | Confirmed |

## Confirmed axes and coordinate rules

A receives equal-shape single-component tensors; an input whose
`input_descriptions` entry declares a channel axis is rejected. Its required static axis a inserts a new dimension at any position
0..r in a rank-r input. It does not silently remove a singleton channel axis.
An input such as [H,W,1] with a declared channel axis must first undergo explicit
axis removal, or use B. The cell-rank bounds imply that A accepts cell ranks 1..7 and produces cell
ranks 2..8; batch axes also count toward the full sample-rank limit of 8.
Scalar/rank-zero cell input is not added.

For n inputs of shape S, A's output shape is insert(S,a,n). If j is an output
coordinate and erase(j,a) removes its channel coordinate, the exact copy rule is:

```
Y[j] = X[j[a]][erase(j,a)]
```

B resolves each input channel axis ai separately from its `input_axes` entry or
from the `channel_axis` of its `input_descriptions` entry; when both are present
they must agree, and when neither is present the node fails. It never guesses an
axis from shape. Removing ai from every input shape must yield the same ordered
shape S; there is no implicit permutation of the remaining dimensions. Required
static output_axis a inserts the combined channel extent into S. Inputs and
output have the same cell rank, within the cell-rank bound 1..8; batch plus cell
rank is at most 8.

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

## Interpretation handling

`input_descriptions` maps input ordinals to descriptions for this invocation.
An entry is validated against that input's descriptor; it supplies the input's
channel axis (A rejects one) and, for C, the channel or component table used by
name/role selectors. Duplicate channel names or roles in a description are
allowed; a selector that matches more than one entry fails.

Assembly attaches no description to its output. A Gray or Black/White plane may
supply what a later consumer treats as an RGB, alpha, Lab or other component.
Three Gray planes assembled by A become an RGB image only for a consumer whose
explicit description declares an RGB group on those indices; this is
interpretation, not a numerical color transform. Combining R/G/B samples from
sources that the caller considers differently encoded is the caller's
responsibility; the kernel and this family do not compare or reject source
interpretations.

Every successful result copies original element bits. In particular, UInt8 255
does not become Float32 1.0 and binary values are not automatically expanded to
another range. Cast, interval decoding, transfer or actual gray/XYZ/Lab
conversion are explicit preceding or subsequent operators as appropriate.
Packed one-bit file data belongs to the codec boundary, not a new tensor dtype.

Nonchannel axes correspond by index only. There is no comparison of coordinate
roles, units, origins or sampling grids and no automatic spatial alignment.
Spatial inputs must agree on their physical height/width axes; a description
cannot relabel physical image axes.

## Interface and support matrix

A/B/C target the default registry as separate primitives, with 1..1024 ordered
input Results in `inputs[0..n)`, each carrying one tensor, and one tensor output
`values`. They do not consume a tensor collection as an opaque input; the
authoring layer expands selected collection entries into this static ordered
input list. Port and graph capacity still apply. No
FMT-specific legacy two-to-four-input bound is inherited. All logical counts and
channel prefix sums use checked arithmetic and the inherited 2^40-element bound.
C specializes the static mapping/selection interface in its member specification.

All parameters below are static. The types specify logical authoring values;
ordered optional axes and indexed descriptions use the canonical `v1` String
records written by the installed helpers (`v1;<ordinal>:<description hex>;...`
for `input_descriptions`, `v1;<axis or _>;...` for `input_axes`).

| Parameter | Type / range | Default and conditions |
| --- | --- | --- |
| `input_descriptions` | Optional sorted, unique input-ordinal-to-`TensorDescription` records | Absent means no input is described. Each entry is validated against its input and used only for axis resolution and C name/role selectors in this invocation. |
| `layout` | String: auto, view, materialize | auto; direct nodes provide it explicitly. |
| A: `axis` | Int64 cell-axis index in [0,input_cell_rank] | Required; insertion position, no negative shorthand; excludes batch prefix. |
| B: `input_axes` | Optional ordered list of n optional Int64 cell-axis indices, each in [0,input_cell_rank) | An omitted list or `_` entry requires that input's description to supply `channel_axis`. A supplied entry must agree with a described axis. |
| B: `output_axis` | Int64 cell-axis index in [0,input_cell_rank) | Required; no implicit channel-last default. |

For A, a described channel axis is an error, including extent one. A
description does not convert a physical image backing into another layout. Image inputs and outputs must satisfy the kernel storage contract at
their actual address maps; new interpretations may require explicit conversion
or materialization. Output image axes/groups must be structurally established,
not inferred from rank or channel count alone. Generic tensor output remains
possible without a complete image/color description.

| Dimension | Strict / CPU accelerated target | Not promised |
| --- | --- | --- |
| Dtype | UInt8, UInt16, Int8, Int16, Int64, Float32, Float64, same across all inputs/output | Missing native widths require implementation; no implicit cast or Float16 extension. |
| Cell rank | A: 1..7 to 2..8; B: 1..8 to the same cell rank; C: output cell rank r+1 in 1..8 | Full sample rank includes batch and cell axes and is at most 8. Cell rank zero/empty dimensions and dynamic arity are excluded; C with r=0 has channel-vector inputs only. |
| Values | Exact element bytes, including HDR/signed and IEEE special patterns | No ULP relaxation for copying; no color/alpha-domain scan. |
| Input layout | Legal generic strided tensors and canonical planar image windows | Raw does not admit an interleaved image without explicit import. |
| Output | auto/view/materialize with exact requested coverage | No cross-owner image representation or implicit packed ROI export. |
| Backend | Portable CPU and named Apple Silicon/x86-64 CPU profiles with identical copy semantics | GPU is unpromised; unavailable named backends fail explicitly. |
| Execution | Exact mapped components/regions with no halo | Existing complete-input implementation is not a conforming replacement. |

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
descriptor checks cover every connected input's shape/dtype, axis and supplied
description even if its payload is unrequested. Explicit output grouping does not
add RGB/alpha read closure. Necessary upstream complete-input computation or intrinsic
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
physical addresses or grid origins. Changing an `input_descriptions` entry
changes the invocation identity.

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

For spatial output, a split/reassemble sequence can publish separate authorized pieces as one view when each mapped physical subregion is representable and the builder can certify its coverage, owners and absence of cycles. An unrelated source owner is permitted there, but arbitrary page remapping or equal sample bytes alone do not establish a valid view. Nonspatial generic output keeps the single-`CpuStorage`-owner, single-affine-expression requirement described in [Current Result interface and bounds](#current-result-interface-and-bounds).

Materialized output follows canonical planar offsets, page admission and exact sample publication. Packed ROI export is a separate read operation.

`auto` attempts only a legal view and otherwise materializes. It must not hide a
source, metadata, cancellation or resource failure by retrying a different
layout. `view` fails with ViewUnavailable if no legal view exists. `materialize`
always creates separate result storage. A view retains its backing owner and
the mapping needed to keep every authorized access valid after source
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
Prepared state is invocation-local. Reusing an immutable plan does not share
mutable per-run payload state. Empty observations are stateless. ResultBuilder
publication is transactional; a failed candidate leaves existing valid regions
untouched.

## Reference algorithm and permitted optimizations

1. Validate static parameters, all input descriptors and supplied descriptions;
   infer axes, output shape, B prefix counts or C's resolved mapping table.
   This phase reads no payload and performs no color-domain validation.
2. Partition Q by input channel ownership using the exact formulas above.
   Declare per-port Data and Descriptor support and acquire only needed windows.
3. For auto/view, prove output view representability. If auto requires copying,
   or materialize is selected, admit the full result reservation and prepare
   destination pages before giving a bounded write window to the operation.
4. Enumerate each requested input-owned region and copy d bytes per sample into
   its mapped output position, or publish the proven equivalent retained view.
5. Check cancellation/currentness and publish the exact requested coverage only
   after all required work succeeds.

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
| Zero inputs, malformed or unsorted `input_descriptions`, missing or out-of-range axis, axis that conflicts with a description, illegal rank insertion, absent or ambiguous C selector | Compile/direct preflight | InvalidArgument / InvalidDomain, schema origin. |
| Dtype mismatch, incompatible nonchannel extents, unsupported tensor rank/type or channel-table length | Compile/direct preflight | TypeMismatch / None. |
| Checked shape/prefix overflow, host port/graph capacity or resource/work admission failure | Inherited validation/admission phase | Preserve the shared NUM/kernel status rather than truncate or wrap. |
| Forced view cannot represent the requested result | Requested observation evaluation | InvalidArgument / ViewUnavailable. |
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
- Duplicate names in an `input_descriptions` entry make only the matching C
  selector ambiguous. Gray/Black-White and mixed-source components assemble by
  index with exact bits and no numerical conversion; the output carries no
  description, and a consumer given an RGB description reads the result as RGB.
  An `input_descriptions` entry affects only its invocation and its identity.
- C covers mixed source structures, selector name/role/index equivalence and
  ambiguity, source omissions/reuse, duplicate/hole rejection, mapping order,
  destination fan-out dirty support and an R channel copied to slot 2.
- Full versus offset/disjoint ROI and y/x=[127,130) across four tiles for T=128;
  select one output channel and instrument that other input payloads are not
  requested. Compare Data and dirty support against the independent mapping.
- Generic positive/negative/zero strides; continuous and tiled images; differing
  legal tile configurations across DAGs and rejection of mismatched geometry
  within one DAG; a description cannot bypass the image import boundary.
- View success when genuinely representable, forced-view failure for unrelated
  owners, auto materialization, repeated source aliases, resource/metadata/work
  limits, cancellation, cache-off, and surviving result/windows after context
  teardown. Verify backing charges retire with the last owner.

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
No operator-local behavior question remains open. The current implementation supplies canonical description and mapping encodings,
native dtypes, exact dependency/publication support, legal view checks and the
member registrations.

## Canonical image alpha constraint

Under FMT-common, complete image descriptions supplied to consumers use straight
colors and internal alpha indices only. Assembly cannot establish a persistent
external alpha relation or declare a canonical premultiplied image. Copying a
premultiplied boundary component does not numerically recover straight color.
Missing alpha companions are not resolved through another tensor's owner.
