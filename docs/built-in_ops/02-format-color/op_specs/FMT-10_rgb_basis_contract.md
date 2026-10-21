---
spec_schema_version: 1
id: FMT-10
kind: shared_operator_contract
category: 02-format-color
status: Proposed
document_maturity: D1_draft
implementation_status: implemented_cpu_fixed_abi_8
clarification_status: complete
---

# FMT-10: linear RGB bases, XYZ and white adaptation

Inherit [FMT-common](FMT_common_contract.md), the NUM numerical/execution baseline,
[FMT-09 transfer separation](FMT-09_transfer_contract.md),
[model coverage](FMT_model_conversion_coverage.md) and the Result tensor contract.
The family contains registered [A RGB->XYZ](FMT-10A_rgb_to_xyz.md),
[B XYZ->RGB](FMT-10B_xyz_to_rgb.md) and [C adapt XYZ white](FMT-10C_adapt_xyz_white.md)
operations, plus the compile-time [D RGB->RGB](FMT-10D_convert_linear_rgb.md)
helper. Each native member has strict, accelerated Apple Silicon and accelerated
x86-64 CPU keys under fixed invocation ABI 8. [Exact matrix definitions](FMT-10_basis_math.md)
remain normative. The specifications remain Proposed; runtime implementation
status is separate.

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

The installed `photospider/ops/format/rgb_basis.hpp` header exposes static parameter
codecs and transactional authoring helpers. D resolves real upstream structure and uses caller-supplied color configuration,
then stages A, optional C and B in a temporary workflow before publishing the
expansion. Writes to a shared WorkflowDocument must be serialized; compiled plans
and their immutable preparations can be executed concurrently.

## Tensor interface and group mapping

Each native member accepts one input Result containing one Float32 or Float64
tensor , then publishes one `values` output Result containing one
tensor. The output preserves Result schema identity, tensor key, logical shape
and batch axes, carries no description, and
sets `atomic_trailing_axes` to zero. The dtype and complete sample shape remain
unchanged. Full sample rank, including batch and cell axes, is at most 8; sample
count is at most 2^40. Raw calls also produce no description. D accepts a graph
edge and returns the final B output edge; it creates no native D registry key.

Semantic calls select one complete ordered three-component color group: RGB for
A/D, XYZ for B/C. A writes X/Y/Z to the selected R/G/B slots; B writes R/G/B to
X/Y/Z slots. C/D keep XYZ/RGB roles. The semantic role order defines matrix
rows/columns even if physical slots are permuted. `axis` parameters and
TensorDescription channel axes index cell axes and exclude the Result batch
prefix. Reorder slots separately through FMT-03. Gray is not silently expanded
or reduced.

The selected group/axis resolves statically and uniquely. All unrelated groups,
internal alpha and AOVs copy bit-for-bit. No alpha read, multiplication, hidden
color removal or whole-pixel opacity validation is introduced. Complete images
remain straight, same-size across planes. Both generic and spatial Result tensor
storage are supported when the requested operation can represent them. Semantic
calls reject a premultiplied boundary representation; explicitly unassociate
through FMT-04B first, or supply a deliberate explicit interpretation or use raw numerical parameters.

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
condition-number threshold; actual static resource exhaustion remains a
possible failure. Geometry validity is static in all modes.

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
| input_description | Optional explicit source interpretation. Presence selects the described path; absence requires raw selectors and geometry. |
| group | Required unique RGB/XYZ semantic group according to member. |
| components / axis | Raw ordered triple and structural axis; not combined with semantic group. |
| source_basis | A/D resolve from explicit RGB description in semantic mode; explicit assertions must agree. Required explicit primary/white description in raw. |
| target_basis | B/D require explicit target primary/white description or one named preset. Target is linear RGB on the preserved unit/reference scale. |
| source_white | C resolves from explicit XYZ description in semantic mode; raw C requires it explicitly. Source assertions must agree. B's semantic source white belongs to its XYZ description and is not used to build the target inverse matrix. |
| target_white | C requires explicit xy/white shorthand. For B/D it is already part of target_basis; contradictory duplicate specification fails. |
| method | Required for C and D adapt: xyz_scaling, bradford, cat02 or cat16; no default. Forbidden for A/B or D without adapt. |
| white_handling | B: require_match default or preserve_xyz. D: require_match default, preserve_xyz or adapt. Only adapt inserts C. Raw B has no semantic white-match obligation and accepts no separate white_handling parameter. Raw D still needs its geometric composition policy. |
| layout | auto default, view or materialize; D applies this selection to its constituent nodes. |

Presets, method and layout are closed authoring choices; custom
coordinates are fixed arrays of Float64; selectors use static bounded integer
indices/metadata identifiers. Fields not applicable to a member fail rather
than being ignored. All matrix data are compile-time fixed; changing them
requires reinference/recompilation. Typed group/unit/geometry descriptions use
canonical TensorDescription v4/v5. NUM/CRV share that codec under their logical
tuple contract; the removed ColorArray v1 payload is rejected.

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
before defining strict results. The mathematical coefficients are finite rationals; static coefficient construction is bounded by the admitted exact-integer capacity. Accelerated numerical bounds inherit NUM. Candidate acceptance uses an internal rounding proof; an unresolved candidate is evaluated with fixed exact arithmetic.

For semantic nonidentity evaluation, the caller guarantees that every consumed source triple and every rounded output row in the complete retained output tiles is finite. Negative XYZ, RGB outside [0,1], HDR and negative color responses remain valid. These are sample-domain preconditions of the unchecked fixed invocation, with no promised error for violations. Source tiles may grant additional samples; only samples actually used to compute retained output tiles are subject to these preconditions. Raw retains the NUM-14 nonfinite and signed-zero behavior, including zero matrix coefficients and implicit +0 bias.

D preserves every A/C/B rounding and each node's sample-domain preconditions. Equal source/target bases retain A->B. Real producer, resource, cancellation and backend failures retain their constituent attribution. Mathematical equality of a fused matrix does not establish equal intermediate rounding, raw IEEE results or physical dependencies.

## Explicit interpretation and composition

With `input_description`, A/D select a complete linear RGB group and B/C select
XYZ. The supplied group is required; raw axis/components are not accepted on
this path. Extra source geometry is a consistency assertion. Without a
description, callers provide the ordered triple, axis and required geometry.
Neither path reads labels from a Result or writes labels to an output.

A computes XYZ on the source scale; B computes target-basis linear RGB; C applies
the selected white adaptation. Callers supply the resulting interpretation to
later operations. No observer, transfer, exposure or rendering conversion is
implied. Static checks remain active; semantic sample validity is the caller's obligation.

D computes intermediate descriptions from its explicit source configuration
and the chosen transforms, then serializes them into the generated A/C/B node
parameters. Those configuration values are private authoring state. Structural
inference still follows the real graph edges, and no hidden edge-description
map or runtime Result propagation is used.

## Output tiles, source closure and identity

The compiler retains complete output tiles. Lowering intersects each output tile with at most seven channel spans. Each nonidentity participating component requires all three selected source coordinates at the same point, including zero-coefficient terms. Bypass alpha/AOV/other-group coordinates require only the matching input coordinate. Each mapped region is closed over the input's own TileGrid; the input may have different atomic trailing axes from the output. The fixed read slots are the deduplicated union of those tiles. Rectangle and union sizes are bounded before enumeration. There is no sample-dependent dependency discovery.

Static descriptor, group, geometry and method checks apply to every compilation, including Empty. An Empty output request produces no payload task. D composes the real A/C/B tile closures. A retained B row can require three intermediate XYZ components and therefore retain A/C rows that would disappear under a fused symbolic matrix.

Only a complete native matrix exactly equal to I selects bit-copy behavior before dot/nonfinite classification. Each retained output coordinate then reads its corresponding source coordinate. Raw preserves every bit, including signaling NaNs and signed zeros. For semantic identity, the caller guarantees finite selected samples actually copied into retained tiles; bypass samples have no additional finite requirement. A single identity-looking row in a nonidentity matrix does not select this exception.

`auto` and `materialize` write independent dense output tiles for both identity and arithmetic. `view` rejects a statically nonidentity transform during preparation. For exact-I, a nonempty forced-view task reads only the input schema and returns `InvalidArgument/ViewUnavailable`; it neither acquires source pixels nor publishes a result. D applies these rules to every generated node. Equal-white C still checks its selected method and all white responses before selecting identity.

## Storage, resources, errors and lifetime

The Plan fixes each task's source views, independent output allocation and scratch size. Preparation verifies the exact output schema, member/tile coordinates, dtype, Host domain, read slots and scratch declaration. Runtime uses the prepared signed/zero-stride input views and writes every sample of the dense row-major output tile. The invocation borrows these allocations until completion; a successful call publishes its retained Result values at ordered retirement. Missing payload coverage is not implicit zero.

Arithmetic scratch contains a 64-lane candidate/gather block and six 10496-bit mutable integers used by exact multiplication, accumulation and rounding. Static rows own three immutable integer numerators and one positive common denominator, each bounded to 8192 bits. With binary64 inputs expressed in units of 2^-1074, the absolute sum of three products is below 2^10292; division alignment and doubled remainder fit within 10496 bits. Copy-only tiles and forced-view tasks declare zero scratch. Coefficients are shared immutably across preparations and Runs. Runtime allocates no per-sample storage and has no dynamic Result builder or publication callback.

For N participating output samples, B bypass samples and rank r, gathering and addressing cost O((3N+B)*r), plus bounded integer arithmetic and source tile lookup. Cancellation is polled at most every 64 visited points and within exact multiplication/division. The floating environment is restored on every return. Source views, outputs and scratch remain owned through completion; D also retains its actual intermediate values. Execution currentness and ordered failure retirement follow the physical Plan contract.

| Condition | Phase | Outcome |
| --- | --- | --- |
| Missing/malformed/irrelevant field, ambiguous selector, invalid xy, singular basis, nonpositive white response, source assertion conflict | Static preparation | InvalidArgument / InvalidDomain. |
| Wrong model, dtype, transfer, units/reference, axis/shape, default white mismatch | Static preparation | TypeMismatch. |
| Semantic consumed nonfinite input or nonfinite rounded output | Caller sample-domain precondition | Outside the fixed invocation contract; no sample diagnostic guaranteed. |
| Raw nonfinite or sample overflow | Evaluation | NUM-defined IEEE result. |
| Forced view of a statically nonidentity matrix | Static preparation | InvalidArgument / InvalidDomain, ViewUnavailable message. |
| Nonempty exact-I forced view | Fixed invocation | InvalidArgument / ViewUnavailable. |
| Static work/capacity, allocation admission, missing coverage, cancellation/currentness or backend failure | Corresponding host/kernel phase | Preserve the operational status. |

Failed calls publish no successful Result. Unrelated calls that have already retired remain observable. D preserves actual constituent operational failures. The family provides no runtime numerical diagnostic stream or result cache.

## Support and acceptance

Three registered default-registry CPU entries per native member use strict,
accelerated Apple Silicon and accelerated x86-64 profiles. There is no GPU,
unsuffixed alias or automatic dispatcher. D chooses one explicit profile for its
generated native nodes, defaulting to strict at construction. Backend
availability and numerical bounds inherit NUM. Non-image raw
Results use valid generic layouts; spatial Results use their declared spatial
layout. Rank, extent and dtype bounds apply to the complete sample shape.

Use independent exact-rational Gaussian elimination/determinants and dyadic sample
rounding, plus fixed special-value bit oracles, to verify coefficients and outputs.
Check all presets including virtual AP0, valid y=0 primaries, exact singularity,
near-singular cancellation, white response rejection and four methods. Compare
white mapping before rounding with each dtype's actual rounded input cases.
Do not claim exact white or inverse sample results by treating a nonrepresentable
rational XYZ white as a floating input.

Native/composite acceptance covers complete, partial and disjoint output tile requests, nonzero origins, differing input/output grids, bypass-only and Empty requests. Check raw NaN order, zero-coefficient peers, whole-I bit copies, negative/zero/unaligned input strides, static rejection, schema-only view failure, allocation-free invocation, scratch bounds, cancellation, retained owners, concurrent Runs and floating-environment restoration. Semantic fixtures satisfy the finite caller preconditions; no sample-error behavior is asserted outside them.

Exercise the registered operations through public Result workflows and independent coefficient/sample oracles. Verify D against independently rounded A/C/B stages, including raw intermediate infinity consumed by a zero coefficient. Keep the independent oracle; remove temporary built-in execution checkers after validation.

## Native coordinate spelling compatibility

The implementation accepts `relative`/`1` and `cd/m2`/`cd/m^2` as the native
relative/absolute unit spellings. Canonical `scene`/`display` references and the
legacy `scene_relative`, `display_relative`, `display_absolute` and
`absolute_display_cd_m2` spellings remain subject to the existing reference
rules. A supplied `coordinates.scale` must agree with units. These aliases do
not rescale samples or insert a display-range conversion. The FMT-15C helper
retains each native FMT-10 node's rounding and explicit intermediate description.
