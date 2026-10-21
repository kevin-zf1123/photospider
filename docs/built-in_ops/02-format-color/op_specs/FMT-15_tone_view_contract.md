---
spec_schema_version: 1
id: FMT-15
kind: shared_operator_contract
category: 02-format-color
status: Proposed
document_maturity: D1_draft
implementation_status: implemented
clarification_status: complete
decision_authority: maintainer_delegated_2026_09_24
---

# FMT-15: luminance mapping and explicit view composition

All member specifications remain Proposed. A/B are registered strict CPU
operations with fixed invocation ABI 8. C implements native and resource-aware
configured stage composition. See [native implementation](../FMT-14-15_implementation.md)
and [configured implementation](../FMT-12-13-18_implementation.md). Inherit [FMT-common](FMT_common_contract.md),
[FMT-10 exact geometry](FMT-10_basis_math.md), and the native NUM numerical,
resource, cancellation and immutable publication contracts.

## Family boundary

| Member | Role | Implementation identity |
| --- | --- | --- |
| [A](FMT-15A_map_luminance.md) | Explicit-exposure global luminance curve | Native CPU strict primitive |
| [B](FMT-15B_convert_display_luminance.md) | Relative display range <-> absolute display nits | Native CPU strict primitive |
| [C](FMT-15C_compose_view.md) | Ordered, explicitly authored view stages | Compile-time graph composition |

A uses a global curve with static parameters; it computes no image histogram,
log-average, exposure estimate, neighborhood adaptation or temporal state.
Such analyses belong to separately specified STAT/GRD workflows. Filmic, ACES,
AgX or camera-manufacturer names do not select an invented curve here. Explicit
versioned configured rendering uses FMT-13. This family supplies no automatic
source-to-screen converter, monitor lookup, scene-white estimate or display
calibration. A lossy view cannot recover its original scene samples.

## Native ports, units and common parameters

A/B each receive one `input` tensor Tensor and produce `values` with unchanged
Float32/Float64 dtype, shape, channel axis and slots. Select one complete linear
RGB group or Gray `linear_y`. Axis-free Gray remains axis-free. RGB uses the
exact basis Y row; A requires all three Y weights strictly positive, excluding
some otherwise legal virtual bases. B is componentwise and imposes no extra
primary restriction. Gray lightness/luma is converted explicitly before use.
Integers and numeric code-domain values require explicit decoding first.

Static fields include explicit input_description/group or raw selectors and
geometry, layout=auto|materialize (auto default), and the
member fields below. Forced view is rejected even for a numerically identity
parameter choice under this family's fixed materialization policy. Raw supplies ordered slots, RGB/Gray choice and all required
weights/basis and units explicitly. All modes require consumed inputs and
requested outputs finite as unchecked caller premises. The same boundary
covers numeric gain/white/knee/display conditions. No output carries an attached
domain description. Finite-domain mapping is distinct from unrestricted NUM
special-value arithmetic and adds no validation scan.

A/B compute in the configured primaries and white with the reference/units
described below. Other groups/AOVs and alpha pass through bit-for-bit. Callers
supply the resulting interpretation explicitly to later consumers. Static
configuration checks run even for empty and bypass-only requests. Color arithmetic never checks or depends on alpha.

## A: reinhard_luminance_v1

Input is relative linear RGB or linear-Y Gray with explicit scene-relative or
display-relative reference. Output is display-relative linear in the same basis,
with reference-white coordinate 1. It is a declared rendering step; this
reference change is not FMT-08 relabeling. Absolute input must be normalized
explicitly first. No physical peak or black luminance is inferred by A.

Static parameters:

| Field | Domain | Default |
| --- | --- | --- |
| curve | simple, white_extended | simple |
| gain | finite Float64 >0; exact stored rational | 1 |
| white | finite Float64 >0 in post-gain relative luminance | required only for white_extended; forbidden for simple |
| negative_luminance | reject, signed_extension | reject |

Let x be Gray or the RGB vector; Y=x for Gray, otherwise Y=w dot x using exact
FMT-10 Y weights. A color with a negative component but nonnegative Y is allowed.
For negative_luminance=reject, Y>=0 is an unchecked caller premise; the retained spelling selects that domain and does not request a validator. signed_extension permits negative Y. Put z=gain*abs(Y).
Define the gain applied to every selected component:

k = gain/(1+z)                              # simple
    k = gain*(1+z/(white*white))/(1+z)           # white_extended
    out_i = RN_dtype(k*x_i)

Strict evaluates each complete finite rational expression once at the output;
Y/z/k are exact internal quantities, not separately rounded tensors. Exact zero
arithmetic returns +0, nonzero underflow keeps its sign. No premature machine
multiply or denominator overflow may alter this definition.

For positive Y the simple curve maps luminance to z/(1+z). The extended curve
maps z=white to 1, and values above white can exceed 1: white is a curve anchor,
not an implicit clamp or physical peak. The signed extension is odd in luminance;
it is explicitly a mathematical extension, not negative emitted light. At Y=0,
k=gain, including nonzero signed RGB whose weighted Y cancels exactly. Thus
there is no epsilon, 0/0 branch or automatic destruction of hidden chroma.
For all RGB requests, read all three components for the formula, even at Y=0 or a
special parameter value. Gray requests consume only their one component.

RGB ratios are preserved before rounding where defined; bounded luminance does
not ensure bounded RGB. Choose FMT-14 explicitly if an output cube is required.
Neither A nor its white anchor guarantees hue, gamut or a complete appearance
model. The mapping is not generally idempotent and no inverse member is offered.

## B: explicit display-range unit conversion

Require display-referred linear RGB or linear-Y Gray. Static `direction` is
required: relative_to_nits or nits_to_relative. Finite Float64 parameters
`black_nits` and `white_nits` are mandatory, 0<=black_nits<white_nits.
They are exact stored rationals; no 100/203/1000-nit default is inferred.

nits_i = RN_dtype(black_nits + (white_nits-black_nits)*relative_i)
    relative_i = RN_dtype((nits_i-black_nits)/(white_nits-black_nits))

Each expression rounds once. In RGB the same neutral offset/scale applies to
all three components, so the basis Y undergoes the corresponding transform in
exact arithmetic. For relative_to_nits, input coordinate 0 represents the chosen
black and 1 the chosen white under this mapping; output is absolute display-linear
cd/m², compatible in units with PQ/BT.1886 when their own domains are satisfied.
For nits_to_relative output is a display-relative coordinate referenced to this
explicit black/white range. Supply the same explicit range convention when composing a matching inverse;
reject contradictory range assertions during authoring.

B accepts finite negative/headroom values without clamping or inferring physical
validity. Absolute display unit conversion does not imply a tone curve, scene
rendering, changed transfer or changed primaries. PQ/BT.1886 impose their own
admissible domains later. In particular, mapping with nonzero black differs from
simple division by reference-white nits. For ordinary white normalization choose
black_nits=0 explicitly. Each requested component consumes only itself.

## C: explicit view recipe

The authoring helper receives a graph, input edge, complete entry interpretation,
a nonempty ordered list of stage specifications, and a complete final display
interpretation/conditions assertion. It returns the last `values` edge and
publishes no native C key. Stage choices are explicit members of FMT-09, FMT-10,
FMT-14A/B/C, FMT-15A/B or FMT-13A..F. No arbitrary script or automatically searched
conversion route. An explicitly retained input edge handles a no-operation path.

Each stage supplies its own required parameters and backend/profile. The helper
checks explicit stage input/output configurations and serializes complete node
parameters. It retains no hidden edge-to-description map and adds no descriptor
propagation. Raw numerical experiments may author constituent nodes directly;
C requires complete configuration for each declared transition.

Infer and check each transition, including model, basis/white, units, transfer,
reference, numeric encoding and engine resource bindings. C does not insert
description-editing graph nodes, hidden transfer operations or missing bridges. Native stage
outputs must match the explicit final display interpretation and black/white
conditions; claimed values must agree with B/transfer parameters where present.
Conditions are descriptive unless a listed stage actually uses them.

An OCIO endpoint can instead be the final complete config-defined display
interpretation, with explicit display/view/config identity. Analytic units or
nits are required only if an analytic binding claims them; never invent peak
luminance from an OCIO space name. C checks the structure and mutual
consistency of declared conditions; numeric/physical validity is a caller
premise. It does not prove monitor calibration or measurement. An OCIO view already contains its
configured rendering. A native A before/after it appears only if explicitly
listed; no duplicate rendering is added by the helper.

Order is observable and never sorted. For example:

FMT-09A -> FMT-10D -> FMT-15A -> FMT-14B -> FMT-09B

is an explicit relative SDR recipe only when all descriptor prerequisites match.
A D65 target is necessary for B. A PQ recipe may explicitly use

FMT-15A -> FMT-14C -> FMT-15B(relative_to_nits) -> FMT-09B(PQ)

with matching target basis, display reference and caller-specified peak <=10000
nits. These are conceptual examples, not canned defaults or registered APIs.
For a supplied versioned ACES configuration a single FMT-13B stage is sufficient
when its entry/exit assertions agree; no native tone/gamut stage is implied.

Reserve collision-free node IDs and stage the expansion atomically; a failed
inference leaves the graph and exports unchanged. Keep every constituent's
rounding, failure, request, cache identity and owner lifetime. No matrix/curve
fusion, no-op elision or engine replacement is presumed bit-equivalent. Dirty
sets are the composition of actual support. Each constituent's numeric and
representability premises also apply to intermediate values without a scan. The helper cannot upgrade a constituent's execution guarantees.

## Demand, storage, resource and error contract

A: any selected RGB output <- full source RGB at the same position; Gray <- Gray.
B: each selected component <- corresponding source component. A/B bypass <-
corresponding bypass sample only, no color validation. Complete retained output
tiles determine the fixed source tile union; there is no halo, image statistics,
alpha-zero skip or frame history. Incremental dirty propagation is outside the
fixed executor protocol.
C follows its expanded graph, including unavoidable upstream complete-input operations.

A/B use fixed invocation ABI 8, one CPU Host task per complete requested output
tile. Source component regions close on the source grid. Auto/materialize write
independent preallocated dense output tiles; padding is not published as pixels.
Preparation checks exact schemas, read slots, output geometry and fixed scratch.
Account exact coefficients/rational refinement, private scratch, output backing,
read windows and live ancestry. Native point work is O(P) plus
strict precision work, bounded by resource budgets with cancellable refinement.
C additionally charges all actual intermediate owners and any engine resources.
No automatic eviction, alternate numerical profile or cached exposure estimate.

Use NUM error delivery: TypeMismatch for type/shape/model, InvalidArgument with
InvalidDomain for malformed structural parameters or conflicts. Numeric
parameter domains, finite samples, Y>=0 under reject, and finite representable
results are unchecked premises. Capacity/work/stage failures and cancellation
retain their native statuses/reasons. Complete writes retire under the kernel's
normal publication rules, without input or completed-output validation.

## Independent acceptance and implementation gates

- A simple, gain=1: neutral RGB .5 -> 1/3 rounded once; neutral 1 -> .5;
  neutral 3 -> .75. Gain=2, zero-Y RGB under the dyadic FMT-14 fixture basis
  with weights (.25,.5,.25),
  (2,-1,0) -> (4,-2,+0), without division by zero.
- A white_extended, white=2: neutral 2 -> 1; neutral 4 -> 8/5. Signed
  extension maps neutral -1 -> -.5 for simple; reject fixtures require Y>=0.
- B black=1, white=101: relative (.0,.5,1) -> nits (1,51,101), reverse
  restores this exact fixture. Use native PQ fixtures satisfying its domain after B.
- Demonstrate that A can leave RGB outside the cube and that a second A changes
  values. No image-statistics dependence: adding unrelated pixels changes no
  existing result. Valid fixtures satisfy exact-formula result representability;
  temporary machine overflow must not alter a valid exact computation.
  Unrequested bypass bit patterns do not interfere.
- Compare sparse/cross-tile/component requests, thread partitions, alpha bits,
  precision at extreme finite values, low budgets and cancellation. Retained
  outputs/windows/resources survive execution-context destruction.
- C's expanded graph must match manually authored stages in bits, metadata,
  support, failures and resources. A failing intermediate cannot be hidden by a
  later clamp. Invalid unit/reference sequence or duplicate IDs must not mutate
  the original graph. Test both native and configured-view recipes.

The implementation page records public workflow commands and the independent
exact-rational/staged oracle. Configured-view numerical/rollback evidence and
synchronous, noncancellable engine construction are recorded in the separate
[external-engine implementation](../FMT-12-13-18_implementation.md).
The first revision exposes strict native CPU only; named alternatives and GPU
require separate contracts and verified quality before registration.

## Source and compatibility boundary

Reinhard et al., [Photographic Tone Reproduction for Digital Images](https://www-old.cs.utah.edu/docs/techreports/2002/pdf/UUCS-02-001.pdf)
(2002), equations for simple/white-extended luminance mapping, provides A's curve
source. Explicit gain replaces automatic log-average exposure; the signed
extension, exact rounding and reference/metadata contract are project choices.
This is not the paper's local dodging/burning algorithm or a commercial preset.
B is the stated affine unit mapping. C delegates configured behavior to the
pinned [FMT-13 contract](FMT-13_ocio_transform_contract.md), never to a product name.
