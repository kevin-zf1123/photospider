---
spec_schema_version: 1
id: FMT-15
kind: shared_operator_contract
category: 02-format-color
status: Proposed
document_maturity: D1_draft
implementation_status: not_implemented
clarification_status: complete
decision_authority: maintainer_delegated_2026_09_24
repository_branch: ops-specs
inspection_commit: 1b403fb9
---

# FMT-15: luminance mapping and explicit view composition

The maintainer delegated these design decisions on 2026-09-24. All members are
Proposed and unimplemented. Inherit [FMT-common](FMT_common_contract.md),
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

A/B each receive one `input` tensor Value and produce `values` with unchanged
Float32/Float64 dtype, shape, channel axis and slots. Select one complete linear
RGB group or Gray `linear_y`. Axis-free Gray remains axis-free. RGB uses the
exact basis Y row; A requires all three Y weights strictly positive, excluding
some otherwise legal virtual bases. B is componentwise and imposes no extra
primary restriction. Gray lightness/luma is converted explicitly before use.
Integers and numeric code-domain values require explicit decoding first.

Static fields: group, interpretation=respect|override|raw (respect default),
source assertions/override, layout=auto|materialize (auto default), and the
member fields below. Forced view is rejected even for a numerically identity
parameter choice because the operation publishes a specified rendering/unit
interpretation. Raw supplies ordered slots, RGB/Gray choice and all required
weights/basis and units explicitly. All modes require consumed inputs and
requested outputs finite; raw does not acquire a new semantic target label.
It retains only applicable unverified original descriptions. Finite-domain
mapping is an explicit exception to unrestricted NUM special-value arithmetic.

Semantic A/B outputs preserve primaries, white, channel roles, coordinates and
same-tensor straight alpha; they publish the reference/units described below.
Other groups/AOVs and alpha pass through bit-for-bit. Reject incompatible
shared/overlapping group declarations; do not silently invalidate another
complete group's meaning. Static metadata checks run even for empty and
bypass-only requests. Color arithmetic never checks or depends on alpha.

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
If Y<0 and negative_luminance=reject, fail; otherwise put z=gain*abs(Y).
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
For all RGB requests, read/validate all three components, even at Y=0 or a
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
explicit black/white range. Record the range convention as provenance; preserve
it on matching inverse use, and reject conflicting assertions in respect mode.

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

Each stage supplies its own required parameters and backend/profile. Respect
checks inferred intermediate descriptors. A source override applies to the first
stage only; later stages must consume their real predecessor interpretation.
This semantic view helper has no whole-pipeline raw switch; raw numerical
experiments can explicitly author the constituent nodes outside C. A selected
raw stage cannot establish C's complete display-color output claim.

Infer and check each transition, including model, basis/white, units, transfer,
reference, numeric encoding and engine resource bindings. C does not insert
FMT-08 relabels, hidden transfer operations or missing bridges. Native stage
outputs must match the explicit final display interpretation and black/white
conditions; claimed values must agree with B/transfer parameters where present.
Conditions are descriptive unless a listed stage actually uses them.

An OCIO endpoint can instead be the final complete config-defined display
interpretation, with explicit display/view/config identity. Analytic units or
nits are required only if an analytic binding claims them; never invent peak
luminance from an OCIO space name. C validates the declared conditions but does
not prove monitor calibration or measurement. An OCIO view already contains its
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
sets are the composition of actual support; intermediate overflow remains an
observable error. The helper cannot upgrade a constituent's execution guarantees.

## Demand, storage, resource and error contract

A: any selected RGB output <- full source RGB at the same position; Gray <- Gray.
B: each selected component <- corresponding source component. A/B bypass <-
corresponding bypass sample only, no color validation. Union demands and transpose
for dirty mapping; no halo, image statistics, alpha-zero skip or frame history.
C follows its expanded graph, including unavoidable upstream Whole operations.

A/B materialize requested values into the kernel's canonical planar owner and
full virtual reservation. Prepare/retain pages explicitly; do not publish padding
as pixels. Account exact coefficients/rational refinement, private scratch,
output backing, read windows and live ancestry. Native point work is O(P) plus
strict precision work, bounded by resource budgets with cancellable refinement.
C additionally charges all actual intermediate owners and any engine resources.
No automatic eviction, alternate numerical profile or cached exposure estimate.

Use NUM error delivery: TypeMismatch for type/shape/model, InvalidArgument with
InvalidDomain for static parameters or conflicts, OperationFailed/InvalidDomain
for consumed nonfinite samples or rejected negative Y, ArithmeticOverflow for
nonfinite requested results from finite inputs. Capacity/work/stage failures and
cancellation retain their native statuses/reasons. Publish only complete valid
requested regions; unaffected previously published observations follow the
kernel's normal immutable failure rules.

## Independent acceptance and implementation gates

- A simple, gain=1: neutral RGB .5 -> 1/3 rounded once; neutral 1 -> .5;
  neutral 3 -> .75. Gain=2, zero-Y RGB under the dyadic FMT-14 fixture basis
  with weights (.25,.5,.25),
  (2,-1,0) -> (4,-2,+0), without division by zero.
- A white_extended, white=2: neutral 2 -> 1; neutral 4 -> 8/5. Signed
  extension maps neutral -1 -> -.5 for simple; default reject fails instead.
- B black=1, white=101: relative (.0,.5,1) -> nits (1,51,101), reverse
  restores this exact fixture. Test native PQ domain checks after B separately.
- Demonstrate that A can leave RGB outside the cube and that a second A changes
  values. No image-statistics dependence: adding unrelated pixels changes no
  existing result. A overflow uses exact-formula final overflow, not temporary
  machine overflow; invalid unrequested bypass values do not interfere.
- Compare sparse/cross-tile/component requests, thread partitions, alpha bits,
  precision at extreme finite values, low budgets and cancellation. Retained
  outputs/windows/resources survive execution-context destruction.
- C's expanded graph must match manually authored stages in bits, metadata,
  support, failures and resources. A failing intermediate cannot be hidden by a
  later clamp. Invalid unit/reference sequence or duplicate IDs must not mutate
  the original graph. Test both native and configured-view recipes.

Implementation must supply runnable public workflows and commands with an
independent exact-rational/staged oracle. None was executed by this spec change.
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
