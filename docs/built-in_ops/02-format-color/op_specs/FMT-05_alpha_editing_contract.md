---
spec_schema_version: 1
id: FMT-05
kind: shared_operator_contract
category: 02-format-color
status: Proposed
document_maturity: D1_draft
implementation_status: not_implemented
clarification_status: complete
repository_branch: ops-specs
inspection_commit: 1b403fb9
---

# FMT-05: internal alpha editing, extraction and removal

Inherit [FMT-common](FMT_common_contract.md), its NUM baseline and the
[kernel storage contract](../../../kernel-specs/Tensor-Storage-and-Region-Access.md).
The canonical straight-image decision of 2026-09-23 supersedes the former
premultiplied editing policies and persistent external alpha bindings.
This document specifies target behavior; it registers no runtime operation.
Members are [A set](FMT-05A_set_alpha.md), [B extract](FMT-05B_extract_alpha.md)
and [C remove](FMT-05C_remove_alpha.md).

## Purpose and representation

A sets/adds alpha, B extracts alpha and C removes alpha for one explicitly
selected color group. Complete images use straight color coordinates, including
RGB, Gray and non-RGB models. Alpha is an independent plane in the same tensor,
identified by metadata rather than a fixed last-channel convention.
No operation composites a background, transforms a color model or changes
transfer, numeric encoding or spatial sampling implicitly.

External alpha planes and explicit scalars remain valid inputs to A, but become
internal result channels. There is no external placement option or persistent
cross-tensor alpha binding. B produces a standalone alpha component tensor;
that result does not remain semantically attached to the source image.
Ordinary input/view lifetime ownership still follows the kernel contract.

A and C preserve straight color samples bit-for-bit. The former A choices
keep_samples/preserve_straight_color and C choices keep_samples/restore_straight
are removed: they no longer distinguish behavior for canonical inputs.
Neither setting alpha=0 nor removing alpha erases finite hidden straight color.
Premultiplied boundary payloads must first be explicitly converted to canonical
straight images; this family does not silently accept or normalize them.

## Confirmed decisions

| Topic | Selected contract |
| --- | --- |
| Members | A set/add; B extract; C remove; one group per invocation. |
| Identity | A native primitive; B/C compile-time compositions of conforming channel operations, constants and metadata edits. |
| Representation | Canonical straight image, with any alpha in the same tensor. No external persistent binding. |
| A source | Same-tensor selected channel, matching explicit external component plane, or explicit shape-[1] scalar. No implicit broadcast of a plane. |
| A placement | Preserve an existing alpha position by default; otherwise explicit channel position. New alpha has no guessed last-channel placement. |
| A color | Copy stored straight color; no multiplication/division and no zero-alpha clearing. |
| A validation | Requested new alpha or selected color requires finite new alpha in [0,1]; requested selected color must be finite. No old-alpha read solely for validation. |
| B absence | missing_alpha=error by default; explicit opaque generates the legal exact code for decoded coverage one. |
| B samples | Copy actual alpha bits without color reads or an added finite/range scan. |
| B shape | Complete nonchannel shape S; keepdims=false by default, optional singleton original channel axis; no rank-zero result. |
| C absence | missing_alpha=error by default; explicit identity returns unchanged data/description. |
| C color | Copy stored straight color; no restoration division, hidden-color loss or background blending. |
| Shared alpha | Reject A's in-place overwrite of an alpha slot referenced by other groups; use a new channel. C detaches only the selected group. |
| Old alpha cleanup | Remove an old internal alpha channel only when no other declared reference remains; retain shared channels. |
| Target index | A's channel_index is a final output position, never a request to overwrite color/AOV data. |
| Layout | auto by default; explicit view/materialize. A view must still perform required semantic validation. |
| Dtypes | A uses Float32/Float64 with same-dtype alpha. B/C preserve existing dtype. B opaque uses explicit alpha_encoding or FMT-06 dtype-default encoding, without a dependency on retired FMT-07. |

## Common interface and support

A has `input`, optional explicit `alpha`, and one `values` output. B/C are
compile-time helpers taking a graph and `input` edge, returning one `values`
edge. All outputs preserve input dtype. A accepts Float32/Float64, with external
alpha/scalar matching exactly; it interprets alpha directly in [0,1]. Integer
codes require explicit decoding/casting before A. B/C copy existing samples of
UInt8, UInt16, Int8, Int16, Int64, Float32 or Float64. Missing native widths and
integer metadata codecs are explicit implementation prerequisites, not automatic
new runtime support. No implicit precision promotion or range conversion occurs.

B opaque generates a same-dtype code that decodes exactly to mathematical
coverage one. Optional static alpha_encoding specifies the fallback output's
numeric encoding and legal code domain. When omitted, use FMT-06's dtype-default
encoding of coverage [0,1]: Float32/64 one, UInt8 255, UInt16 65535, Int8 127,
Int16 32767 or Int64 2^63-1. An explicit 10-bit encoding in UInt16 yields 1023;
an explicit reversed encoding can yield zero. Do not infer the new alpha encoding
from another color channel. The chosen code must be in its legal domain and
exactly representable in output dtype; otherwise fail before pixel generation.
Do not quantize an almost-opaque substitute or silently clamp it.

The alpha_encoding parameter describes the missing-alpha fallback only. It is
allowed with missing_alpha=opaque, structurally validated, and never re-encodes
an alpha channel that actually exists. Generated output carries its selected
encoding/coverage description. Explicit code-domain restrictions must be supplied
by the encoding description; naming a container dtype does not infer a reduced
bit depth. Existing integer alpha extraction and removal remain bit copies,
retaining applicable encoding without decode/re-encode. A float alpha carrying
a conflicting numeric encoding still requires explicit decoding/reinterpretation
before A's normalized semantic use. Generic encoding metadata and missing native
dtypes are implementation prerequisites; FMT-07 is no longer a prerequisite.

| Common static parameter | Contract |
| --- | --- |
| group | Required explicit group identity; exact unique resolution in effective metadata. Do not infer a group from channel count. |
| metadata_mode | respect by default, or explicit override. This semantic family has no raw mode; raw channel edits use FMT-01/02/03. |
| metadata_override | Effective input description only with override; source identity remains immutable and actual shape/storage cannot be overridden. |
| axis | Optional Int64 assertion of the effective input channel axis, nonnegative and within rank; absent for a component Gray input. |
| layout | auto by default; view/materialize explicit, with the rules below. |
| profile | B/C helper selection: strict by default, accelerated_apple_silicon or accelerated_x86_64. A selects the corresponding proposed primitive key. |

All structural parameters and group/channel maps are static. Samples may differ
between executions. Rank stays in 1..8 with checked positive extents and the NUM
2^40 logical-element bound; inserting an axis requires input rank <=7. The same
spatial axes, origin and grid survive channel surgery. Static coordinate conflicts
require explicit override, never automatic transposition/resampling. Strict and
accelerated outputs are bit-identical; predicates, metadata and failures remain
exact, with no floating tolerance. CPU strict/named profiles are the target;
GPU and cross-backend conformance are not claimed.

## Structure and channel surgery

Resolve the selected group and input channel axis from effective metadata using
shared source-authority and override rules. Let S be the shape after erasing
the channel axis, or the whole shape of a component Gray input. Group selectors,
axis choices, placements and channel maps are static; sample values may vary
between executions. Spatial planes must match S and the declared coordinate grid;
there is no implicit squeeze, resize or spatial broadcast. Scalar is an explicit
source kind, even when a plane happens to have shape [1].

For A, an existing private alpha slot can be replaced at its current position.
When explicitly moving/adding alpha, first remove the old slot if it becomes
unreferenced, then insert a new alpha slot at the stated final index, shifting
other channels in order. Retain an old slot that is still referenced. Reusing
the shared slot as an in-place edit is rejected rather than silently modifying
other groups. Component Gray needs an explicit new output channel axis when
adding alpha. The result contains actual new-alpha samples, including expanded
scalar samples, in its logical alpha plane.

For B, return S by default. If the primary image has a channel axis,
keepdims=true replaces that axis extent by one. Erasing the sole axis of a
rank-one channel vector would produce rank zero, so require keepdims=true.
A component input with no channel axis retains its original nonchannel shape;
keepdims cannot invent an axis. Missing-alpha opaque generation uses this same
output shape, not the shape of a scalar constant source.

For C, remove the selected group's alpha reference. Delete the former alpha
channel only when no other declared reference remains. Reindex all surviving
channel/axis metadata coherently. Do not squeeze an existing channel axis when
only one channel remains. Keep color values and straight interpretation.
A retained shared alpha continues serving other groups, never the detached group.

Every channel map reads original immutable inputs. Static shape/description
checks apply to all required ports even when samples are not requested. Target
assignment cannot change the mandatory straight image representation or create
an external alpha binding. Raw structural editing remains available through
FMT-01/02/03 and ordinary NUM operations; it does not promise FMT-05's semantic
validation or establish another canonical image state.

## Exact request and invalidation consequences

| Requested observation | Required sample support and validation |
| --- | --- |
| A new alpha | Read new alpha at the mapped position (scalar index zero if selected); validate finite [0,1]. No color/old-alpha samples. |
| A selected group color | Read exactly that source component and new alpha; validate finite color and finite [0,1] alpha, then copy the color bits. Alpha=0 permits nonzero hidden straight color. |
| A unrelated channel/group | Read only its remapped source component; no selected-group color/alpha validation. |
| B actual alpha | Read only the selected internal alpha positions; exact bits, no added coverage-domain validation. |
| B opaque fallback | Generate the preflight-resolved exact opaque code for requested positions; no source pixel reads. |
| C surviving channel | Read only its remapped source component; no discarded-alpha samples or association arithmetic. |

A's new-alpha changes affect both its alpha output and the validation dependency
of requested selected-group colors, even though copied color bytes do not depend
arithmetically on alpha. Scalar changes have that effect across the requested
extent. Replaced/removed old-alpha samples have no dependency merely because
the old channel existed; if explicitly selected as the new source or retained
for another group, the corresponding explicit mapping still applies.
B maps dirty alpha coordinates to output coordinates; missing opaque has only
structural dependencies. C drops removed-channel sample invalidations and
remaps retained channels. Descriptor/shape/group changes invalidate static maps.
Preserve disjoint requests; do not close them over unrequested colors or the
whole image. Required upstream Whole work retains its original scope.

## Layout selection

All three members accept static layout=auto (default), view or materialize,
with the same representability rules as FMT-01/02. Auto returns a legal read-only
view only when all output mappings can be proven to belong to one admissible
owner; otherwise it materializes requested coverage. View requires that proof
and fails rather than silently copying. Materialize reserves a new owned result
and copies/generates only requested samples. Layout changes neither metadata,
numerical validity obligations nor required source failures.

A must perform its exact requested alpha/color validation before publishing a
successful view observation. Pre-existing source validity/coverage or bitwise
identity cannot skip that obligation. It must not expose unvalidated observations
as newly successful output merely because the underlying owner has those bytes.
Auto likewise cannot fall back from a semantic validation failure to copying.

Independent external alpha planes and scalar expansion ordinarily require
materialization. An equivalent same-owner source view can be used only if the
complete result mapping satisfies the existing image storage and view rules.
A valid R-only request does not authorize inventing a different layout that
cannot represent this operator's declared full logical output. Shared alpha
retention, channel removal or insertion also requires an explicit legal map.
There is no multi-owner concatenation or implicit virtual-page aliasing feature.

B's existing alpha extraction and C's remaining-channel mapping may be views.
B opaque generation cannot present fabricated image pixels as a view; use auto
or materialize. C's missing_alpha=identity still respects layout: view may return
the unchanged input mapping, while materialize creates a new owned copy of the
requested observations. Identity is not permission to ignore a forced layout.

## Ownership, partial results and failures

Canonical output images obey planar storage and one full-image virtual range.
External input planes are copied or legally mapped into the result's logical
internal alpha channel under existing owner/layout rules; no new multi-owner
image storage or metadata pointer shortcut is introduced. Scalar expansion must
also obey image storage, not expose an illegal image zero-stride backing.

Only requested, successfully produced observations become valid. R-only output
can require reading new alpha for validation without publishing the result's
alpha plane. A later consumer must request that result alpha channel explicitly;
missing alpha coverage does not resolve via retained external semantic metadata.
Normal DAG edges and read/view owners keep required resources alive. Deleting a
logical alpha channel does not promise immediate release of a shared backing.

Static group, shape, reference, placement and canonical-representation conflicts
fail preflight. A requested invalid alpha or nonfinite selected color fails
semantic evaluation as OperationFailed/InvalidDomain; B's bit-copy alone does
not reject such alpha bytes. Missing required alpha fails unless the explicit
B opaque or C identity policy applies. Resource exhaustion, cancellation,
publication and arithmetic used for checked shape/offset calculation inherit
NUM/kernel rules. Channel copying performs no floating arithmetic.

## Required semantic acceptance cases

Required independent cases include:

- Straight C=0.5 with old alpha=0.5 and new alpha=0.25 gives C=0.5, alpha=0.25.
  New alpha=0 still gives C=0.5. Removing alpha preserves C=0.5.
- A accepts finite hidden straight color at new alpha=0. A NaN selected color
  fails only when requested; an unrelated AOV request does not trigger it.
- B preserves negative zero, out-of-range samples and NaN payload bits; opaque
  fallback generates the exact encoded coverage-one code over S. No source color read occurs.
- A shared-slot replacement fails; explicit insertion retains the old shared
  slot. C detaches one group while keeping the channel for others.
- Moving a private alpha removes its former slot, inserts at the final specified
  index and preserves every other channel's order and exact values.
- R-only A output does not certify or secretly supply unproduced alpha. A later
  alpha request obtains the declared output channel through normal execution.
- External/scalar inputs never leave persistent alpha-source references in
  result metadata. Public image outputs are straight in respect/override modes.

## Reference execution, resources and composition

Resolve group, effective metadata, channel surgery and layout before evaluating
payloads. Compile a direct source map and reverse dirty map; never repeatedly
search all channels per output sample. For Q, acquire only declared Data and
Validation windows. A validates exactly the requested selected components and
mapped new alpha even when values alias a legal view. Allocate/prepare destination
pages only for materialization, copy/generate exact bytes, check cancellation and
publish exact successful coverage with coherent metadata. Original input values
always supply sources; no mutation or sequential overwrite occurs.

For rank r, C channel descriptors, Nq requested samples and F windows, reference
work is O(C*r+Nq*r+F*r), plus host page-set work and required upstream execution.
Static maps need O(C+r); admitted windows/footprints, descriptors and page sets
are separately charged. No full-image sample buffer or eager per-reserved-page
directory is required. Deduplicate alpha validation support across requested
colors. Vector loads must stay within authorized spans and prepared pages;
coalescing cannot read padding or unrequested channels. Use host scheduling for
disjoint observations, not a private worker pool.

Charge actual provided backing, simultaneously retained source/result owners,
metadata/maps, staging and windows, not just requested logical bytes. View output
can retain a larger original allocation; deduplicate shared owners in accounting.
No eviction, replay or hidden alpha snapshot storage is introduced. Poll work,
cancellation and currentness at most every 1024 entries/samples during mapping,
validation and copy, and immediately before publication. Failure releases
unpublished resources and leaves prior successful observations intact.

B resolves its opaque constant once with exact descriptor arithmetic, without
source samples; include descriptor/encoding work in compile/admission budgets.
Construct Int64 endpoints directly, never through Float64.

B/C stage their helper expansion and handles transactionally, using the existing
collision-aware authoring ID rules. Errors leave the caller graph unchanged.
Lower B to exact FMT-01A extraction or a conforming constant source containing
the exact opaque code with explicit result shape/storage/metadata. Lower C to FMT-02C/FMT-03 channel mapping
and explicit target grouping; when only metadata changes, use a conforming
identity/remap that still honors layout. These helpers must not add sample checks
from A or retain a reference removed by C. No new runtime registration is implied
by a helper name. Host graph/arity/resource limits remain applicable.

Initial optional sample-only completed-result caching is disabled, following the
inherited primitives. Any later cache needs source/descriptor/coverage/layout
identity and A's validation evidence, including new-alpha dependencies when
color bytes are unchanged. A prior successful byte copy is insufficient proof
that new alpha has the same semantic validity. No external alpha binding is
needed for cache identity; ordinary declared inputs remain dependencies.

## Error phases and diagnostics

| Condition | Phase | Status / reason |
| --- | --- | --- |
| Missing/ambiguous group or selector, invalid axis/placement/mode, overlapping color and target alpha roles, shared-slot overwrite, malformed override | Compile/direct preflight | InvalidArgument / InvalidDomain. |
| Missing alpha without the selected fallback | Static preflight | InvalidArgument / InvalidDomain; identify selected group. |
| Unsupported dtype, mixed A dtypes, nonchannel shape mismatch or noncanonical premultiplied input | Static preflight | TypeMismatch / None. |
| Invalid opaque encoding, or coverage one has no legal exactly representable output code | Static preflight | InvalidArgument / InvalidDomain. |
| Forced view unavailable, including opaque generation | Observation evaluation | InvalidArgument / InvalidDomain; ViewUnavailable diagnostic. |
| Requested invalid new alpha or nonfinite A selected color | Semantic evaluation | OperationFailed / InvalidDomain, affected observation. |
| Size/offset overflow, budget/work, missing source coverage, backend, cancellation or stale failure | Inherited phase | Preserve NUM/kernel code, origin and scope. |

Identify member, group, channel/source and global coordinate for sample errors;
static errors have no invented pixel coordinate. B/C do not introduce NaN/Inf
or coverage-range errors for copies. Missing source coverage remains missing,
not an implicit zero. Required upstream failures keep their original scope.

## Implementation acceptance and performance

Members supply analytic fixtures and independent coordinate/byte oracles.
Implementation must provide runnable public compile/execute workflows and inspect
both output samples and descriptors, not only a visual preview. Cover arbitrary
channel axes, component Gray, shared/private alpha, disjoint/cross-tile requests,
all three layouts, exact A validation dependencies, absent-alpha fallbacks,
integer extraction without scaling, view lifetime after context retirement,
transactional helper failure, low budgets and cancellation. Report unsupported
native dtypes and backends instead of silently substituting representations.

Measure Float32 [4096,4096,4] set/extract/remove with same-owner and independent
alpha sources, scalar A, opaque B and shared-alpha C. Compare full output,
selected-color-only, alpha-only and y/x=[127,130) at tile size 128. Record selected
layout/profile, build/ISA, workers, input/page state, runtime statistics, copied
and validation bytes, virtual span, provided backing, retained-owner and scratch
peaks. Require correctness before timing; no throughput is promised.

Generic group/encoding metadata, exact partial-region shape changes, constant
image generation and view validation/publication are implementation dependencies.
Legacy typed-image/Layer paths and existing generic copy tests do not implement
this target. The specification is clarified and Proposed/not implemented; no
runtime or benchmark result is claimed.
