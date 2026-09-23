---
spec_schema_version: 1
id: FMT-10
kind: shared_operator_contract
category: 02-format-color
status: Proposed
document_maturity: D1_draft
implementation_status: not_implemented
clarification_status: complete
repository_branch: ops-specs
inspection_commit: 1b403fb9
---

# FMT-10: linear RGB bases, XYZ and white adaptation

Inherit [FMT-common](FMT_common_contract.md), the NUM numerical/execution baseline,
[FMT-09 transfer separation](FMT-09_transfer_contract.md),
[model coverage](FMT_model_conversion_coverage.md) and canonical planar storage.
The family contains native [A RGB->XYZ](FMT-10A_rgb_to_xyz.md),
[B XYZ->RGB](FMT-10B_xyz_to_rgb.md), [C adapt XYZ white](FMT-10C_adapt_xyz_white.md)
and compile-time [D RGB->RGB](FMT-10D_convert_linear_rgb.md).
[Exact matrix definitions](FMT-10_basis_math.md) are normative. Clarification
is complete for this scope; no runtime implementation or registration is delivered.

## Purpose and stage boundaries

A/B transform coordinates between a described linear RGB basis and XYZ. C
performs an explicitly selected full linear chromatic adaptation. D expands
A -> optional C -> B, preserving constituent rounding, support and failure.
No transfer, exposure, gamut mapping, tone/view rendering, dtype/range scaling,
alpha association or codec operation is implicit. Gray/other model conversions
belong to FMT-11; arbitrary caller matrices belong to NUM-14.

Changing RGB basis and adapting white are distinct operations. D can explicitly
preserve XYZ across unequal whites or map source white to target white using C.
Only the latter attempts the selected chromatic-adaptation correspondence.
Neither implies complete CIECAM appearance rendering or a scene/display change.
All selected XYZ/RGB descriptions must share the same observer convention;
this family does not convert observers or spectral data.

Existing ColorArray primary presets and exact basis validation are shared
NUM/CRV infrastructure. They do not implement FMT-10. The old unsuffixed
RGB/XYZ operations were [removed](FMT_legacy_retirement.md); their old typed,
Whole or rounded-matrix behavior is not retained as a compatibility interface.

## Tensor interface and group mapping

Each primitive has one required tensor Value input `input` and one tensor Value
output `values` under the target generic-metadata/planar contract. D takes an
input edge and returns one output edge. No Result, resource/matrix input,
runtime-dependent output shape or dynamic parameter stream is introduced.

Input/output have the same Float32 or Float64 dtype, rank/extents, channel axis
and channel count. Semantic calls select one complete ordered three-component
color group: RGB for A/D, XYZ for B/C. A writes X/Y/Z to the selected R/G/B
slots; B writes R/G/B to X/Y/Z slots. C/D keep XYZ/RGB roles. The semantic role
order defines matrix rows/columns even if physical slots are permuted. Reorder
slots separately through FMT-03. Gray is not silently expanded or reduced.

The selected group/axis resolves statically and uniquely. All unrelated groups,
internal alpha and AOVs copy bit-for-bit. No alpha read, multiplication, hidden
color removal or whole-pixel opacity validation is introduced. Complete images
remain straight, same-size across planes and canonical planar. Semantic calls
reject a premultiplied boundary representation; explicitly unassociate through
FMT-04B first, or use raw/override for an intentional numeric reinterpretation.

Raw selects exactly three distinct ordered component indices on an explicit or
resolved structural axis. A rank-one [3] tensor can use axis=0. Do not infer RGB
from three channels or accept a partial raw triple. Raw preserves shape/slots
and applies the selected geometry directly to stored floating values. Integer
dtype is unsupported in every mode. Non-native floating numeric encodings must
be explicitly decoded before semantic use, but raw may compute on their stored
values without implicit normalization or a forced FMT-06 step.

## Geometry, units and static parameters

Seven primary/white presets are admitted: sRGB/Rec.709 primaries, Display P3,
Rec.2020, Adobe RGB (1998), ProPhoto RGB, ACES AP0 and AP1. Complete custom
primary xy and white xy are allowed. Presets only expand to the fixed RN64
coordinates in the math contract; they never choose transfer. Contradictory
preset/custom descriptions fail. Resolved numerical equality is exact,
including inherited metadata zero canonicalization, not an epsilon comparison.

Primary coordinates may be virtual, negative or have y=0. White xy must be
finite with x>0,y>0,x+y<1. Both homogeneous primary basis and white-normalized
RGB matrix must be exactly nonsingular. Negative normalization scales are not
inherently invalid. Near-singular exact invertibility is not rejected by a
condition-number threshold; actual resource or numerical overflow remains a
normal possible failure. Geometry validity is static in all modes.

Relative RGB [1,1,1] corresponds to XYZ white with Y=1. Absolute display-linear
RGB uses the FMT-09 cd/m² component scale, and XYZ Y is luminance in cd/m².
A/B preserve the native numerical scale and scene/display reference. Absolute
XYZ Y=100 is not a 0..100 relative encoding merely because of its value.
A relative Y=100 encoding requires a separate explicit range conversion.

C normalizes both chromaticity whites to Y=1 to build its matrix and preserves
the declared unit/reference-luminance scale. It does not apply a ratio of display
peak/white luminances or change exposure. This does not mean every non-neutral
pixel's Y is unchanged by adaptation. Target reference/units cannot disagree
with the source; any such normalization or rendering change is separate.

| Static logical field | Rules |
| --- | --- |
| metadata_mode | respect default; explicit override or raw. |
| group | Required unique RGB/XYZ semantic group according to member. |
| components / axis | Raw ordered triple and structural axis; not combined with semantic group. |
| source_basis | A/D resolve from effective RGB metadata in semantic mode; explicit assertions must agree. Required explicit primary/white description in raw. |
| target_basis | B/D require explicit target primary/white description or one named preset. Target is linear RGB on the preserved unit/reference scale. |
| source_white | C resolves from effective XYZ metadata in semantic mode; raw C requires it explicitly. Source assertions must agree. B's semantic source white belongs to its XYZ description and is not used to build the target inverse matrix. |
| target_white | C requires explicit xy/white shorthand. For B/D it is already part of target_basis; contradictory duplicate specification fails. |
| method | Required for C and D adapt: xyz_scaling, bradford, cat02 or cat16; no default. Forbidden for A/B or D without adapt. |
| white_handling | B: require_match default or preserve_xyz. D: require_match default, preserve_xyz or adapt. Only adapt inserts C. Raw B has no semantic white-match obligation and accepts no separate white_handling parameter. Raw D still needs its geometric composition policy. |
| metadata_override | Complete effective source interpretation only in override mode; does not change actual dtype/shape/backing. |
| layout | auto default, view or materialize; D applies this selection to its constituent nodes. |

Modes, presets, method and layout are closed authoring choices; custom
coordinates are fixed arrays of Float64; selectors use static bounded integer
indices/metadata identifiers. Fields not applicable to a member fail rather
than being ignored. All matrix data are compile-time fixed; changing them
requires reinference/recompilation. Typed canonical serialization of the new
group/unit/geometry descriptions is an implementation prerequisite; the current
ColorArray v1 codec is not silently reinterpreted.

## White handling and adaptation methods

B applies only the target inverse RGB matrix. Its semantic default requires the
source XYZ white and target basis white to match. Explicit preserve_xyz allows
a mismatch while retaining XYZ coordinates before that inverse. B never performs
adaptation internally. To adapt, connect C explicitly first.

D's omitted policy means require_match: unequal source/target whites fail,
while equal whites expand A->B. preserve_xyz expands A->B with explicit B
preserve_xyz. adapt requires a method and expands A->C->B, with matching C output
white and B target white. Explicit adapt still validates its selected method and
white responses even when the whites are equal. Do not erase those parameter
checks through a same-white shortcut. No FMT-08 relabel replaces the C transform.
Raw D freezes the same matrices/policy but does not rely on color metadata in
its intermediate tensors; raw B receives no semantic match assertion.

C supports XYZ Scaling, linear Bradford, CAT02 and CAT16 at complete adaptation.
There is no partial degree, scene/environment adaptation parameter, nonlinear
Bradford variant or full CIECAM02/CAM16 model. All three source/target white
responses under the selected matrix must be strictly positive. This exact
static check applies in raw too; no epsilon or method fallback is allowed.
The constraint applies to white parameters, not to color sample responses.
Finite negative/HDR color samples remain legal. Tiny positive white responses
are not rejected through a new conditioning threshold.

## Numerical evaluation and composition

A/B/C strict rows use the exact coefficients derived by the math contract,
three exact products plus implicit +0 bias, and one final rounding to the
unchanged dtype. Do not round the derived matrix to Float64 or the sample dtype
before defining strict results. The mathematical coefficients are finite
rationals but may exceed finite machine coefficient storage; only actual
resource limits or the final result determine those failures. Accelerated
precision, diagnostics and fallback inherit NUM.

Semantic nonidentity evaluation validates the full consumed source triple as
finite and only the requested output rows as finite after rounding. Do not
reject a negative XYZ coordinate, RGB outside [0,1], or negative cone response
of a color sample merely by sign. No output clipping is introduced. Unrequested
output overflow is not evaluated. Raw nonfinite and signed-zero behavior follows
NUM-14, with the specified implicit +0 bias, even for zero matrix coefficients.

D preserves every A/C/B output rounding and required validation. Intermediate
failure remains observable even when a hypothetical fused product would produce
a finite answer. Equal source/target bases do not eliminate A->B. Optimizer
fusion/removal must prove equal sample bits, metadata, support and failures;
mathematical equality of matrix products alone is insufficient. D is not a
once-rounded RGB matrix primitive or an exact round-trip promise.

## Metadata behavior

Semantic A requires linear RGB on the declared native scale and publishes XYZ
with the source white/reference/units. B requires coherent XYZ and publishes
linear RGB with the target basis and preserved reference/units. C changes XYZ's
white interpretation to the stated target after actual matrix adaptation;
D publishes the target linear RGB description according to its chosen policy.
Preserve channel positions, free-form names, coordinates, internal alpha and
unaffected descriptions. Rebuild selected component roles and applicable group
fields; RGB-only primaries/transfer records do not remain as XYZ model fields.
Remove/update complete named-space labels incompatible with the output.

Source metadata is authoritative in respect; extra source geometry is a checked
assertion. Untagged semantic data needs explicit assignment or a complete call
override. Raw requires the needed explicit geometry, uses the ordinary stored
triple and preserves applicable source metadata without inherited validity or
claiming a completed color-space change. It does not relax physical image
storage, static geometry validity or parameter requirements. A semantic result
only carries its actually checked sample guarantees, not a full-image scan.

## Exact requests, dirty mapping and identity

For a nonidentity native node, every requested participating output component at
spatial coordinate p requires all three selected source components at p as Data
and, in semantic mode, Validation. This includes zero coefficients. Requested
bypass alpha/AOV/other-group samples read only their matching input sample and
are not color-validated. There is no data-dependent Control read or halo.
Static Descriptor checks cover all required group/geometry/method information
for every request, including Empty. Empty requests acquire no pixel windows.

Dirty changes to any selected source component at p affect all observed selected
output components at p for nonidentity nodes, even for exact zero coefficients.
Bypass dirty maps are identity. D composes actual per-node support; a requested
RGB component usually needs all intermediate XYZ components, so required A/C
rows cannot be pruned because the final product has zero entries. Required
upstream Whole work and its failures retain their original scope.

If one native node's exact matrix is I, the selected contract is a bit-copy
identity before dot/nonfinite classification. It reads only the requested source
components; semantic mode validates those finite values, while raw copies every
bit including signaling NaNs and signed zeros. Still perform all static checks
and publish the correct metadata. Same-white C is the common case; approximate
I is not sufficient. A product of separate matrices equaling I does not make
each constituent identity or erase D's intermediate effects.

Identity dirty support is pointwise. Auto may share a legal read-only backing for
that native identity; materialize forces requested copies. Forced view fails for
any nonidentity native transform regardless of an alpha-only query. D forwards
layout to its expansion; view is valid only when every required constituent
meets its own identity/backing rule. A nonidentity diagonal XYZ Scaling still
reads all three source components and cannot use this identity exception.

## Storage, resources, errors and lifetime

Output has unchanged global coordinates and exact requested produced coverage.
Missing/unproduced samples stay missing; padding or unsupplied channels are not
implicit zeros. Shared views publish a distinct output description and scoped
validation, not blanket validity of their owner's bytes. Materialized images
reserve the full planar virtual span with the one DAG tile geometry, prepare
required pages explicitly and retain produced backing under kernel lifetime
rules. Raw image metadata does not legalize interleaved storage.

Resolve/validate static descriptors and exact matrices first; map and acquire
retained exact source windows; admit output/pages/scratch; copy or evaluate
requested rows; check cancellation/currentness before publishing success.
Charge exact coefficient construction, retained rational data, address metadata,
all simultaneous owners/pages/windows and numerical scratch. Virtual reservation,
logical bytes and resident backing are distinct. D includes live intermediate
owners and their true production support, not only final result bytes.

For N requested participating rows, B bypass samples, rank r and F windows,
addressing/row work is O((3N+B)*r+F*r), plus fixed-size exact matrix preparation
and bounded rational arithmetic. Identity reduces source work to requested
samples. No eager full-image scan or per-reserved-page directory is added.
NUM work/capacity exhaustion fails explicitly; it does not authorize machine
coefficient truncation, hidden approximation or a new condition threshold.
Poll cancellation/currentness at most every 1024 visited entries and during
bounded exact arithmetic, and before publication. SIMD cannot access padding,
unprepared pages or undeclared peers. Release unpublished resources on failure.
Normal retained owners/read windows outlive producer/context as required; no
replay, eviction or hidden source reconstruction is introduced.

Initial optional result caching remains disabled pending correct exact coverage
and validation identity. Future keys include native member, source/target
resolved geometry, method/policy, numerical profile, units/reference, selectors,
metadata mode/override, storage/layout and input identity. Do not cache D as a
fused mathematical matrix while omitting its stage boundaries or failures.

| Condition | Phase | Outcome |
| --- | --- | --- |
| Missing/malformed/irrelevant field, ambiguous selector, invalid xy, singular basis, nonpositive white response, source assertion conflict | Compile/direct preflight | InvalidArgument / InvalidDomain. |
| Wrong model, dtype, transfer state, unit/reference, axis/shape/layout, default white mismatch | Static preflight | TypeMismatch / None. |
| Required semantic input NaN/Inf | Requested evaluation | OperationFailed / InvalidDomain. |
| Requested semantic row rounds to nonfinite | Requested evaluation | OperationFailed / ArithmeticOverflow. |
| Raw nonfinite/sample overflow | Raw evaluation | NUM-defined result. |
| Unavailable forced view | Normal layout check | InvalidArgument / InvalidDomain; ViewUnavailable. |
| Budget/work, missing coverage, cancellation/currentness or backend failure | Inherited phase | Preserve status and normal observation attribution. |

Failures publish no success for the affected observation, without revoking
unrelated completed output. D propagates the actual constituent's error and
node/port/sample attribution. Resource or upstream failure is never hidden by
raw mode, same-white handling or a supposed inverse round trip.

## Support and acceptance

Three proposed default-registry CPU entries per native member use strict,
accelerated Apple Silicon and accelerated x86-64 profiles. There is no GPU,
unsuffixed alias, automatic dispatcher or new backend promise. D chooses one
explicit profile for its generated native nodes, defaulting to strict at
construction. Backend availability, precision and fallback reporting inherit NUM.
Non-image raw tensors use valid generic layouts; images obey planar. Rank/extent
and dtype boundaries inherit the common target, without reinstating typed Image.

Use independent exact-rational Gaussian elimination/determinants and dyadic sample
rounding, plus fixed special-value bit oracles, to verify coefficients and outputs.
Check all presets including virtual AP0, valid y=0 primaries, exact singularity,
near-singular cancellation, white response rejection and four methods. Compare
white mapping before rounding with each dtype's actual rounded input cases.
Do not claim exact white or inverse sample results by treating a nonrepresentable
rational XYZ white as a floating input.

Native/composite tests cover full, single-row, disjoint/offset and cross-tile
requests, alpha-only, hidden colors at alpha=0, invalid selected peers, irrelevant
AOV values, Empty and dirty support. Exercise identity/raw payload copies,
nonidentity zero-coefficient dependencies, D intermediate failure, source
immutability, retained owner lifetime, low budgets, cancellation, cache-off and
floating-environment restoration. Members supply concrete analytic fixtures.

Future implementations must provide public compile/execute examples and
correctness-gated timings for Float32/64 [4096,4096,4], full/one-color/alpha-only
outputs and y/x=[127,130) with tile size 128, recording source/page state, axes,
profile/ISA/build, workers, time, backing/reservation and live intermediates.
No executable behavior or throughput is claimed here. Implementation needs
canonical RGB/XYZ/unit metadata, bounded exact coefficient support, exact tuple
requests/publication and new member registrations; current NUM Whole matrix
execution cannot be substituted while claiming this regional contract.
