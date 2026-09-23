---
spec_schema_version: 1
id: FMT-01
kind: shared_operator_contract
category: 02-format-color
status: Proposed
document_maturity: D1_draft
implementation_status: not_implemented
clarification_status: complete
repository_branch: ops-specs
repository_commit: d49d1840
inspection_commit: d49d1840
---

# FMT-01: channel extraction family

Implementation update: package 0.20.0 [removes the legacy format/color code](FMT_legacy_retirement.md).
Descriptions of old registrations below record the inspected baseline only;
those keys and pixel callbacks are no longer available. This target remains
Proposed and unimplemented.

Inherit [FMT-common](FMT_common_contract.md), including the selected generic
tensor/metadata target, consumer-scoped validation, raw computation and call-local
override. Inherit NUM numerical and resource conventions through that contract.
This family selects existing samples and performs no floating arithmetic:
every supported profile preserves element bits, including signed zeros,
infinities and signaling/quiet NaN payloads. It does not infer gray, unassociate
premultiplied components, decode transfer or cast dtype.

## Confirmed family scope

The family supports channel inspection and independent processing: a described
tensor comes from a workflow input or another producer; extracted components
feed numeric/filter operations, explicit later assembly, or exported results.
It serves arbitrary channel-axis tensors as well as spatial image planes.
A/B are proposed independently named default-registry CPU primitives. C is the
explicit authoring composition `split_channels`, with no native operation key.

| ID | Member | Responsibility |
| --- | --- | --- |
| [FMT-01A](FMT-01A_extract_channel_index.md) | Extract by index | Select one channel at an explicitly resolved channel axis. |
| [FMT-01B](FMT-01B_extract_channel_named.md) | Extract by name/role | Resolve one channel through explicit channel-description metadata. |
| [FMT-01C](FMT-01C_split_channels.md) | Split all channels | Compile-time composition exposing each channel through an A node. |

Multiple-channel subsets, reordering and repeated selection belong to FMT-03.
FMT-02 owns assembly/merge. Selecting alpha here copies the stored alpha samples;
alpha creation, replacement, association and removal policy belong to FMT-04/05.
An ID may name a family, and its lettered members have independent specifications.

## Confirmed channel-axis interpretation

Accept any legal tensor rank and resolve the channel axis from an explicit
metadata designation or an explicit static axis. An untagged tensor needs an
explicit axis. Never guess HWC/CHW from extents or assume the last axis. Under
ordinary semantics an explicit assertion must agree with attached metadata;
call-local override can explicitly replace the designation. Raw positional
selection needs an explicit axis and does not interpret color-domain metadata.

Axis interpretation is structural selection metadata. Reading a channel name
does not consume a coverage interval, complete RGB color or alpha-association
sample invariant. The target therefore does not automatically validate unrelated
channels just to extract one. Output channel metadata is descriptive and must
not fabricate a complete RGB/CMYK model from one selected component.

## Current implementation versus target

channel.extract
previously registered a typed Float32/Float64 HWC to HW Whole operation with static
Int64 index in 0..63 and below C. Its implementation copies bytes through
color_common.hpp.
This is the historical subset, not acceptance of the family described here.

[NUM gather](../../01-numeric/op_specs/NUM-10B_gather.md) provides a dynamic index
vector and repeated/reordered selections, preserves rank, clears output facets
and currently executes Whole. [NUM slice](../../01-numeric/op_specs/NUM-09C_slice.md)
is also Whole and preserves rank. Their present runtime behavior cannot be used
as proof of FMT's new metadata policy or exact regional execution. The maintainer
selected exact requested-region extraction for this family; Whole NUM nodes are
not conforming substitutes for that execution contract.

The existing [named-output mechanism](../../../kernel-architecture/Multi-Output-Operations.md)
supports independently requested outputs. Current operation contracts bound
the output list to 64 entries. FMT-01C expands into single-output A nodes and
therefore uses ordinary graph limits rather than widening that native interface.

The front-matter branch/commit records code inspection at ops-specs@d49d1840.
The subsequently committed kernel storage target is documented at ecfb1631; that
documentation commit adds no runtime support. D1_draft describes the mathematical
and interface draft, while Proposed and not_implemented retain their separate
meanings from the [category maturity definitions](../../README.md#规格完整程度).

Dependencies are FMT-common's tensor metadata consumption/codec migration,
native support for the selected missing integer widths, the kernel virtual
image/page-window contract, and the public exact dependency/publication path.
C additionally depends on A's authoring and execution entry. The new codec's
public encoding and the page-window API are implementation prerequisites; this
family does not claim they already exist. B's name table and output component
description must use that shared schema rather than a private substitute.

## Support matrix

| Dimension | Strict target | CPU accelerated targets | Boundary / not claimed |
| --- | --- | --- | --- |
| Dtype | UInt8, UInt16, Int8, Int16, Int64, Float32, Float64; preserve input dtype | Same values and bits | Missing native integer widths require the shared implementation dependency. No Float16 or implicit cast. |
| Shape/channels | Positive rank 1..8, explicit channel axis, count <=2^40; single selected component | Same | keepdims=false requires rank>=2; C is additionally bounded by graph expansion limits. |
| Sample domain | Exact stored bytes, including signed/HDR values, Inf and all NaN payloads | Exact, no ULP relaxation | No color conversion, clamp, premultiplication change or implicit complete-color validation. |
| Source storage | Generic legal strided tensors; planar image storage and regional page windows | Same logical support | Interleaved generic tensors do not qualify as image bindings without explicit import conversion. |
| Execution | Exact requested component/ROI support, no halo | Same mapping and publication obligations | No Whole substitute; genuinely required upstream Whole work retains its own behavior. |
| Backend | Portable CPU exact byte selection | Named Apple Silicon or x86-64 CPU capability | No GPU promise; an incompatible named target fails BackendUnavailable. |
| Output storage | auto/view/materialize; image materialization follows the kernel virtual layout | Same | Packed read windows are separate access requests, not a changed primary image layout. |
| State | Immutable inputs/results and static selectors | Same | No implicit seed, clock, asynchronous mutation or dynamic selector port. |

## Clarification decisions

| Topic | Selected behavior / proposal | State |
| --- | --- | --- |
| Family scope | A index, B name/role, C all-channel split; subsets/reordering in FMT-03. | Confirmed |
| Axis | Arbitrary axis from metadata or explicit axis, with explicit override for conflicts. | Confirmed |
| Output rank | keepdims=false by default; rank-one source requires keepdims=true. | Confirmed |
| Selector timing | A/B/C axis, index, name and output set are static; no dynamic-index member. | Confirmed |
| Output description | Keep component name/role, units, applicable source color interpretation and association meaning without complete-color guarantees. | Confirmed |
| Demand | Exact requested-region/channel support; independent split outputs. Supersedes the briefly selected Whole option. | Confirmed |
| Split interface | Compile-time composition of A with c0,c1,... output handles. | Confirmed |
| Output storage | auto/view/materialize under the kernel virtual image contract; replaces the former dense vocabulary. | Revised and confirmed |
| Name/role resolution | Explicit name/role namespace and exact unique match; missing/ambiguous selection fails. | Confirmed |

Nonnegative indices follow the inherited NUM/FMT rules. Layout and exact
name/role matching have also been confirmed explicitly. These decisions define
the Proposed target; they do not imply acceptance of an unimplemented runtime.

## Common ports, parameters and descriptor inference

A/B have one Value input `input` and one Value output `values`. They preserve
dtype. Target dtypes are UInt8, UInt16, Int8, Int16, Int64, Float32 and Float64;
the missing native widths remain FMT-common implementation dependencies.
Input/output rank is 1..8, extents are positive and each logical count is at most
2^40. Selection operates on one tensor, not a tensor collection; a caller first
selects the relevant tensor from a collection. Every parameter below is static.

| Parameter | Type and legal values | Authoring default / condition |
| --- | --- | --- |
| `metadata_mode` | String `respect`, `raw`, `override` | `respect`; serialized explicitly by helpers. |
| `axis` | Int64 in [0,rank) | Optional when effective metadata identifies exactly one channel axis; mandatory for untagged/raw input. An ordinary supplied value asserts agreement. |
| `metadata_override` | String containing the canonical shared metadata replacement payload | Required exactly for `override`; forbidden otherwise. Only the invocation's effective description changes. |
| `keepdims` | Bool | false; direct nodes supply it explicitly. |
| `layout` | String `auto`, `view`, `materialize` | auto; direct nodes supply it explicitly. The former dense spelling is not a target alias. |

The override payload uses the shared tensor metadata codec required by FMT-common
migration. This document defines its consumed logical fields (channel axis,
ordered channel descriptions and applicable component interpretation), not a
second private codec. Its public key/version/encoding and owned-resource binding
must exist before these entries are implemented. The existing ColorArray v1
String is not implicitly that new payload. A/B add the selector fields in their
individual specifications; C forwards the common parameters to A.

For `respect`, resolve the channel axis from effective metadata if present;
otherwise require `axis`. A supplied disagreement fails. For `override`, apply
the explicit description replacement first, then the same resolution/check.
For `raw`, require `axis` and ignore attached semantic axis selection; known
applicable metadata may still be structurally projected as described below.
B requires an effective name/role table, so raw name-based selection is not
defined; use A or an explicit override supplying that interpretation.

Let source shape be S, rank r, resolved channel axis a, and selected channel k.
Require 0<=k<S[a]. With keepdims=true, output shape O equals S except O[a]=1.
With keepdims=false, remove S[a] and renumber subsequent axes. Reject r=1 with
keepdims=false; [C] with true produces [1]. Do not silently invent rank-zero or
an exceptional squeezed shape. Selection and shape inference read no samples.
Changing any selector, shape or effective metadata requires compile-time inference
and plan/identity validation, not a dynamic numeric selector input.

## Exact mathematical mapping

For an output coordinate o, keepdims=true maps to source s with s[a]=k and
s[j]=o[j] for j!=a. With keepdims=false:

```
s[j] = o[j]       when j < a
s[a] = k
s[j] = o[j-1]     when j > a
values[o] = bit_copy(input[s])
```

Strict and the inherited Apple Silicon/x86-64 CPU accelerated profiles use the
same exact mapping and sample bits. Unsupported accelerated targets fail through
the inherited capability rule. No floating ULP allowance applies to a copy.
Preserve floating environment and all sample bit patterns without loading NaNs
through a numeric conversion. There is no GPU implementation claim.

## Exact demand, errors and dirty support

For output request Q, input Data support is exactly the image of Q under the
coordinate map above. Each source box has channel offset k and extent 1;
nonchannel axes have exactly their requested global offsets/extents. Disjoint
boxes retain their gaps. Do not gather the complete tensor, all channels, an
enclosing spatial rectangle or a complete output before projection. Empty runtime
demand reads no payload; compile/preflight still checks declared structure and
parameters. There is no dynamic Control input or spatial halo.

Descriptor demand covers shape/dtype and the metadata used for selector resolution
and output description. Extraction relies on structural selection, not a finite
RGB tuple or bounded alpha, so it adds no color sample Validation closure.
NaN/Inf or out-of-coverage values do not themselves fail byte extraction, even
when selected. Unselected values are not observed by this node. Required upstream
work retains its own indivisible support and failures: a Whole producer may still
execute Whole. Physical source pages/transports may also exceed logical demand;
account actual I/O and identify that upstream granularity without relabeling it
as exact byte I/O. FMT-01 itself requests only the mapped source support.

For an input dirty set D, intersect D with channel k and project through the
inverse shape mapping to get output invalidation. Changes confined to other
channels invalidate no output samples of this extraction. Nonchannel coordinates
are preserved exactly. Metadata used by selector resolution/propagation enters
descriptor dependency and may require reinference. Broader upstream invalidation
remains broader; do not claim an exact end-to-end chain from a local proof alone.

A failed observation publishes no partial success. Independently completed
observations retain their outcomes. Source/resource/cancellation failures preserve
their original categories and scope; a local view/copy failure is attributed to
the affected observation. C's separate nodes use the same mappings independently
and do not form an atomic bundle or acquire unrequested sibling payloads.

## Reference algorithm and permitted optimizations

The portable reference performs these stages:

1. Validate static parameters and the consumed descriptor fields; resolve axis a
   and index k (B scans the selected name/role namespace once), then infer output
   shape and projected description. This stage reads no sample payload.
2. Map the exact requested footprint Q through the coordinate formula. Declare
   the resulting Data and Descriptor support and obtain authorized source windows
   through the host. Do not expand channel coverage or bridge footprint gaps.
3. Attempt the view representation for auto/view. If forced view is unavailable,
   return its defined failure. Otherwise, for materialize or auto fallback, reserve
   the result representation and prepare only required destination pages/windows.
4. Enumerate requested output coordinates in logical order, locate the mapped
   source address and copy exactly d bytes with no floating conversion. For a view,
   publish the equivalent address mapping rather than copy payload.
5. Publish owned output coverage and its projected metadata only after the
   observation's dependencies and writes succeed. Retain the owners/windows and
   preserve unrelated completed outcomes under the host's publication contract.

Optimizations may coalesce adjacent copies only when both address maps and the
complete copied byte range are authorized and contiguous. Page/tile boundaries
and arbitrary strides can split runs. SIMD or bulk copy must not read unrequested
channels, alignment padding, gaps or unprepared pages; masked/tail handling must
preserve exact sample bytes. No approximate numerical backend or arithmetic
fallback is required. B reuses its compiled resolved index instead of searching
metadata for every pixel; C relies on its ordinary A nodes.

Parallel work uses host scheduling over disjoint output observations/windows.
Input windows are immutable, page provisioning/publication follows the shared
kernel synchronization contract, and no private worker pool or page-fault-driven
producer execution is introduced. The complexity, capacity and cancellation
bounds below apply to optimized as well as reference execution.

## Component metadata propagation

Retain the selected component's name, role and unit, together with applicable
source primaries/white/transfer/reference and stored association meaning. Project
the channel table to the selected component and transform all known axis-indexed
descriptions with the shape mapping. With a removed channel axis, keep the
component description without claiming that the output still has that axis.
Spatial axes keep their own origin/step; extraction does not resample them.

For an explicitly described premultiplied numeric boundary payload, extracted R
remains a premultiplied component, not a canonical complete image. For
Lab, l=L*/100 remains normalized perceptual lightness and is not relabeled as
linear gray. For alpha,
coverage remains a descriptive role; extraction does not validate its interval.
Record the component's source interpretation without attaching an active complete
RGB/CMYK tuple guarantee. This also applies to singleton selections. Component alpha provenance is descriptive and creates no persistent external
alpha binding. Later numeric consumers declare any needed alpha input explicitly;
complete images use internal alpha under FMT-common. Extraction itself does not
introduce a hidden alpha read.

In raw mode, if the explicit axis agrees with the described channel axis, project
known channel descriptions structurally without color-domain validation. If it
selects another axis, only carry descriptions whose axis transformations remain
well-defined; do not label a spatial slice as one selected red channel. Remove
metadata whose schema provides no valid propagation through this shape change.
Unknown annotations are not treated as proven color semantics. Applicable
profile/resource descriptions retain actual owners and their accounting.

Metadata lookup validates the fields it consumes (axis bounds, table length,
selector namespace/match). It does not validate color samples or unconsumed
white/transfer numerical semantics. Publishing a projected description makes
no new semantic validity claim. Consumer-side validation follows FMT-common.

## Storage, ownership, resource and cancellation contract

Inherit the selected [kernel storage contract](../../../kernel-specs/Tensor-Storage-and-Region-Access.md).
Every image is planar, with contiguous row samples and allowed row padding.
One DAG chooses tile geometry for all its tiled images; operators and planes
cannot override it. Interior tiles are tight full blocks. Edge tiles retain only
valid rows, with each row padded to the tile width; every following tile is
page-aligned, including across planes. Alignment gaps are separate from pixel
padding. A full image has one reserved
continuous virtual range with explicit on-demand page backing. Produced pages
remain until the final image lifetime owner retires; budget exhaustion fails.

Generic tensor capabilities remain broader than standard image storage. An
interleaved generic input can be selected as numeric data, but an interleaved
image binding requires explicit import conversion. This distinction does not
allow raw/override to bypass a structural image-layout requirement. A metadata
projection declaring an output to be an image must establish that requirement.

`layout=auto` returns a source view when the authorized output mapping can be
represented without copying under the applicable storage contract. `view` forces
that path and fails if it cannot be represented. `materialize` creates separate
result storage and copies only requested samples. For image results, reserve the
full logical result's virtual address range and provide just the pages needed
by the request; copied samples occupy their canonical planar/tiled image offsets.
Auto uses this path only when view representation is unavailable, not to recover
from resource, source, cancellation or semantic-selection failure.

A view may consist of several tile-local windows in the same image's range; a
cross-tile request does not need one global affine stride vector. It retains the
source address-space owner and accesses only prepared pages and valid samples.
Fixing channel k selects that plane's tile offsets without reading other planes.
For continuous affine source regions, preserve remaining byte strides and use
zero stride on the retained singleton channel axis. Compute offsets using checked
arithmetic relative to logical origins. ROI coordinates remain global.

Image materialization does not promise a tightly packed copy of an arbitrary
cross-tile ROI. Callers requiring that arrangement explicitly request a packed
read window, whose temporary backing is not the authoritative image storage.
For generic non-image tensor results, materialization may produce owned packed
requested fragments under the ordinary tensor contract. No legacy dense alias
is introduced for these new proposed entries.

The structural image directory and virtual-range owner survive as long as an
output alias/window requires them. They are not interchangeable with ordinary
fully readable CpuStorage bytes. A small view can retain the source's complete
virtual reservation and all already produced page backing. Pages are not evicted
when a local read window closes while the image remains alive. General numeric
views may have negative/zero strides; declaring an image still requires the
kernel's canonical planar storage/view rules.

For requested Nq elements of width d, Nq*d is logical copied sample volume,
not allocated memory. Let Vout be the full output virtual layout span and P the
host page size. For materialized image output, reserve Vout; provide and charge
P times the number of previously unprovided pages intersecting the requested
sample byte ranges (or the platform's explicitly larger provision granularity).
Compute a union of page intervals, not ceil(Nq*d/P): pitched/tiled samples can
occupy many nonadjacent pages. A page is charged once and does not make its other
unrequested samples valid. Continuous-plane layouts may share a page across planes;
page-aligned tiled layouts keep different tiles in distinct pages. Retained page capacity, virtual
reservation, metadata and staging are reported separately.

For rank r and involved fragment/window count F, scalar mapping/copy work is
O(Nq*r+F*r), plus checked page-set construction, metadata lookup and actual upstream
work. Fixed coordinate scratch is O(r); window/directory/page bookkeeping is
charged at actual capacity. Virtual layout lookup must be bounded without eager
per-pixel or per-reserved-page metadata for a huge mostly untouched image.
Account simultaneous old/new backing, page/row alignment and referenced ancestry.
No full-image backing allocation or sample read is introduced by FMT-01 itself.

Poll cancellation/currentness and charge work during metadata matching, fragment
mapping, page preparation and copy at most every 1024 entries/elements, and before
publication. No access is handed to the operator until required pages and source
data are ready. Published observations remain immutable; failed unpublished
provision can be cleaned up without discarding existing valid data. Inherit host
failure attribution, resource admission and lifetime rules.

Initial A/B registrations disable optional sample-only completed-result caching:
that cache cannot establish owner/window/physical-layout availability. This does
not disable input sharing, page lifetime or dependency tracking. Kernel-managed
OS faults are not a substitute for explicit workflow demand or page admission.

The inspected runtime still has configurable 128x128 planning defaults and an
independently configured snapshot store with separate block allocations and full
recognized tuples. Those mechanisms are migration dependencies, not the selected
virtual planar image implementation. Acceptance must compare different legal
DAG tile settings and verify that each individual DAG uses its one geometry.

## Error mapping and acceptance

| Failure | Phase | Status |
| --- | --- | --- |
| Missing/unknown/wrongly typed parameter, illegal mode, negative/out-of-range axis or index, conflicting assertion, invalid keepdims rank, missing/ambiguous named selector | Compile/direct preflight | InvalidArgument / InvalidDomain, schema origin |
| Unsupported dtype/rank or incompatible structural channel-table length | Compile/direct preflight | TypeMismatch / None |
| Unavailable forced view | Evaluation of the requested observation | InvalidArgument / InvalidDomain; diagnostic ViewUnavailable |
| Resource, cancellation, stale, unsupported backend or required upstream failure | Respective inherited phase | Preserve inherited code/reason/origin |

Diagnostics identify operation, selector/axis and offending field, with no
invented runtime sample coordinate for a compile-time selection failure. A/B
fixtures below define independent integer-coordinate and byte-copy oracles.
Acceptance must cover both keepdims shapes, arbitrary axis/rank, every supported
dtype, strided generic tensors and planar image windows, raw non-finite payloads, projected component
metadata, exact read/dirty scope, owner lifetime, budget failure and cancellation.
Each actual implementation must provide a public compile/execute workflow with
commands and checked output. Conceptual DAGs in these Proposed specs are not
current runnable APIs, and legacy test passes do not establish this target.

An explicit identity fixture is source shape [2,1], values [7,9], axis=1,
index=0 and keepdims=true: the output keeps shape [2,1] and identical bytes.
With keepdims=false it has shape [2] with the same sample order. This supplements
the nontrivial A/B/C fixtures and the inherited special-value bit cases.

## Performance acceptance plan

Performance remains unmeasured until the new entries and kernel storage exist.
Use the following declared workloads in the public implementation harness:

| Case | Workload and observation |
| --- | --- |
| Small analytic | The member's documented fixture; inspect output bytes, exact support and metadata before timing. |
| Plane throughput | A Float32 planar [4096,4096,4] tensor, axis=2, index=1, with deterministic values; request the complete extracted plane. |
| Cross-tile ROI | The same input with T=128 and output ROI y=[127,130), x=[127,130); verify nine logical source samples across four tiles, separately from backed-page/transport bytes. |
| Layout modes | Compare auto, available forced view, and materialize on the same semantic request; exercise an unavailable view separately as a correctness case. |
| Named lookup | B resolves the named four-channel fixture; additionally use a legal larger channel table and report descriptor-resolution cost separately from pixel execution. |
| Split consumption | C with four channels, requesting one handle and then all four; report active A nodes and source/window sharing actually observed. |

Record build/compiler revision, concrete CPU backend/ISA, hardware, worker count,
dtype/shape/ROI, common DAG tile geometry, page size, row padding, storage mode,
source provision state and optional-cache setting. Report first-use preparation
separately from repeated runs with retained source state; declare repetition
count and median/tail latency. Include copied valid bytes, actual source/transport
bytes, provisioned page capacity, reserved virtual span, metadata/staging peaks,
retained owners and any view-to-materialize fallback. Accounting exclusions follow
the kernel resource contract. No speedup, bandwidth or RSS guarantee is inferred
from these proposed workloads, and pixel correctness/support gates every timing.

## Sources, compatibility and remaining implementation work

Repository source and public-contract links in this family and its members are
the evidence for current behavior. The coordinate-copy formula is specified
directly and checked with an independent integer/byte oracle; no third-party
commercial implementation or pixel-compatibility level is claimed (U: unverified
for such comparisons under the repository template).

No user-facing selection/layout question remains open. Shared metadata encoding,
native dtype expansion, public page-window/region integration and the concrete
registry/authoring implementation remain required work. Actual public workflow
commands, runtime results and benchmark measurements must be supplied by that
implementation; they are not fabricated in this Proposed specification.
