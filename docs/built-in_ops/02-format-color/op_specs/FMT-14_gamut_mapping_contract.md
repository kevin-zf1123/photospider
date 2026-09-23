---
spec_schema_version: 1
id: FMT-14
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

# FMT-14: explicit native RGB gamut mapping

The maintainer delegated the remaining design decisions on 2026-09-24.
This Proposed specification completes the family design; it supplies no runtime
implementation. Inherit [FMT-common](FMT_common_contract.md), the exact RGB basis
of [FMT-10](FMT-10_basis_math.md), and the fixed OKLab mathematics of
[FMT-11](FMT-11_model_math.md). No transfer, white adaptation, tone rendering,
profile lookup or dtype conversion is inserted implicitly.

## Members and intended use

| Member | Function | Interpretation |
| --- | --- | --- |
| [A](FMT-14A_clip_rgb_gamut.md) | Component clipping | Minimal box projection; hue/luminance can change. |
| [B](FMT-14B_reduce_oklab_chroma.md) | Versioned OKLab chroma reduction | Constant stored OKLab L and chroma-ray direction during a bounded search, subject to explicit endpoint/neutral safeguards and rounding. |
| [C](FMT-14C_compress_rgb_chroma.md) | Smooth linear-RGB neutral-ray compression | Compresses near-boundary colors, including some already in gamut; preserves linear Y in the interior in exact arithmetic. |
| [D](FMT-14D_check_rgb_gamut.md) | Exact RGB-box diagnostic | Binary out-of-box mask, independent of ICC gamut alarms. |

All four are native CPU primitives. First keys expose the strict reference only;
no GPU or accelerated key is claimed before its separate implementation and
NUM quality verification. A caller chooses a member; there is no automatic
choice of mapping algorithm. B is not CSS Local MINDE, and C is not a
perceptually uniform compression or ACES reference gamut compression.

## Interface, parameters and metadata

One `input` tensor Value, Float32/Float64, contains one selected RGB group and
optional same-tensor straight alpha/AOVs. A/B/C produce `values` with identical
dtype, shape, axis and ordered R/G/B slots. D produces `mask` with the same dtype
and non-channel shape S; `keepdims=false` by default, true keeps the original
channel axis at length one. If removal would produce rank zero, true is required.
D metadata contains spatial coordinates, binary diagnostic role and the tested
space/algorithm provenance, not a complete color group or alpha relation.

Semantic modes require native relative linear RGB in the already selected target
basis. Target limits are exactly the closed cube [0,1]^3. Source RGB is already
expressed in that basis; convert primaries explicitly through FMT-10 first.
HDR output headroom or absolute nits must be explicitly normalized before these
members. Finite signed/HDR inputs are valid candidates for mapping, not automatic
input errors. Scene/display reference is retained; this family does not render
scene-referred data to display-referred data.

Static fields: `group`, `metadata_mode=respect|override|raw` (respect default),
applicable source override/assertions, `layout=auto|materialize` (auto default),
and member-specific fields. Forced `view` is rejected for every member, including
sample-dependent identity branches. No data-dependent layout or profile choice.
A/D raw require three ordered distinct slots, but no geometric basis. B/C raw
also require the full explicit target basis needed by their formulas. All modes
require finite consumed samples; this is a finite-domain mapping/classification
contract. Raw skips color consistency, not the finite domain or structural rules;
it retains applicable unverified descriptions instead of certifying target color.
D still publishes the diagnostic meaning of its actual comparison.

B requires D65=(RN64(.3127),RN64(.3290)) and relative Y=1. Other whites require
explicit adaptation before entering B. C admits any FMT-10 basis whose exact
Y-row weights are all strictly positive (they sum exactly to one). This excludes
some virtual-primary bases admitted by general basis conversion. A/D impose no
additional primary restrictions. Geometry and metadata are checked statically,
even when only bypass data or an empty region is requested.

A/B/C preserve the selected complete RGB interpretation and straight alpha
relation in semantic modes, with processing provenance and no inherited global
sample-validity guarantee. Other groups and opaque annotations follow common
propagation rules. Reject overlapping semantic groups whose unchanged claims
would become inconsistent; source metadata is never mutated.

## A: component projection

For each requested color component x, return +0 if x<0, 1 if x>1, otherwise
copy x bit-for-bit. Thus -0 survives the copy branch; no epsilon is used. Only
that component is read and checked for finiteness. This is image-gamut clipping,
not integer overflow clipping, legal-code validation or quantization.

## B: oklab_chroma_bisect_v1

B reads and validates the full RGB triple for any requested mapped component.
Its numerical steps and control decisions are fixed; no perceptual tolerance,
iteration setting or implementation-dependent early termination is exposed.

1. If the input triple is within [0,1]^3, return its bits unchanged.
2. Evaluate FMT-10A strict to relative XYZ, rounding each component to the input
   dtype. Evaluate FMT-11E strict on that stored triple, again rounding to the
   same dtype, yielding (L,a,b). Both complete intermediate triples must be
   finite; their overflow is an error, not a signal to substitute black/white.
3. If L<=0, output (+0,+0,+0); if L>=1, output (1,1,1). These are explicit
   out-of-lightness-range fallbacks, not preservation of HDR lightness.
4. Define candidate R(t), for exact dyadic t in [0,1], by storing
   (L, RN_dtype(t*a), RN_dtype(t*b)), then FMT-11F strict to stored XYZ and
   FMT-10B strict to stored target RGB. Retain both stage-rounding boundaries.
   Exact-zero products become +0. Check every candidate intermediate/output
   finite; arithmetic overflow fails. Test gamut on the resulting stored RGB.
5. Compute R(0). If it is outside the cube, return its component projection A.
   This explicitly handles neutral-axis residuals in the pinned OKLab matrices;
   it does not modify those matrices or silently repair white elsewhere.
6. Compute R(1). If it is in gamut, return R(1). Otherwise set lo=0, hi=1,
   accepted=R(0). Repeat exactly 32 iterations for Float32 or 64 for Float64:
   t=(lo+hi)/2 as an exact dyadic; if R(t) is in gamut, set lo=t and
   accepted=R(t), otherwise set hi=t. Return accepted after the fixed count.

The returned stored triple is in the cube by construction. The algorithm retains
an accepted candidate and does not assume monotone membership for an arbitrary
custom basis. It promises neither the globally largest admissible chroma nor
minimum Delta-E. The scalar bracket width is 2^-32 or 2^-64; this is not a color
error bound. Repeated candidate values caused by rounding do not change the
iteration count. No atan2, hue-unit conversion or near-gray epsilon is needed.
L/ray constancy applies before inverse-transform rounding; endpoint/neutral
fallbacks intentionally relax it. This is a lossy mapping with no inverse.

## C: rgb_neutral_knee_v1

Static `knee` is finite Float64, interpreted as an exact binary rational;
0<=knee<1, default RN64(0.8). Obtain the exact positive luminance weights w from
the target RGB basis. For a finite input triple x, form exact Y=sum(w_i*x_i).

- If Y<=0, return (+0,+0,+0). If Y>=1, return (1,1,1).
- Otherwise set n=Y, d_i=x_i-n and
  q=max_i( d_i/(1-n) if d_i>=0 else -d_i/n ).
- If q<=knee, copy the input triple, including signed zeros.
- Otherwise set u=q-knee, v=1-knee,
  q_new=knee+v*u/(u+v), and output RN_dtype(n+d_i*q_new/q) per component.

All arithmetic up to the final rounding is exact rational arithmetic. No
intermediate Float64 overflow or underflow is permitted to alter the reference.
The q<=knee branch includes q=0 and avoids division by zero. For every finite
q>knee, q_new<1 and the exact output is in the cube, hence so is RN_dtype output.
This continuous knee may modify already in-gamut colors with q>knee. It preserves
Y for 0<Y<1 before rounding and keeps a straight line toward the neutral of that
Y in linear RGB. It does not claim constant perceptual hue or OKLab lightness.
The explicit Y endpoint collapse is the only luminance-range fallback. Applying
C twice can change a pixel twice; it is not idempotent.

## D: geometric diagnostic

Read and validate the full triple. Return exactly 1 if any stored component is
<0 or >1, otherwise +0. Bounds are inclusive; both signed zeros are inside.
No epsilon, profile, alpha cutoff or perceptual threshold is involved. A legal
HDR color can be outside this particular SDR target cube. Dirty source colors
map to the same mask position; unrelated alpha/AOV changes do not invalidate it.

## Demand, storage and resources

No halo or spatial statistics. A requests only each selected input component.
B/C/D request all three source colors at the same spatial position for each
requested mapped color/mask. A/B/C bypass output requests read only the
corresponding original sample, copy every bit and do not validate color or alpha.
Union support for multiple outputs; forward dirty mapping is the transpose of
these component relations. Pixel-dependent branches do not reduce declared
source support. Zero alpha never skips requested hidden-color work. Required
upstream Whole behavior remains unchanged.

auto/materialize produce requested valid samples in a new canonical planar owner
with the kernel's full virtual reservation, DAG tile geometry, explicit page
preparation and retained backing. Bounded private planar scratch is allowed;
row padding and page gaps are not pixels. Missing output remains missing.
Charge static matrices, exact rational/dyadic state, precision refinement, scratch,
output pages and simultaneously live ancestry. A/C/D are O(P) point work; B is
O(P*N) with N fixed above plus strict transform refinement. No whole-image scan,
cache-dependent approximation or adaptive algorithm switch. Cancellation checks
occur in pixel batches, every B iteration and expensive exact-math refinement.
Retained results/windows and metadata owners survive context destruction.

## Errors, precision and acceptance

Inherit NUM error delivery/publication. Static type/shape errors are TypeMismatch;
invalid fields, geometry, interpretation or forced view are InvalidArgument with
InvalidDomain. Consumed nonfinite samples fail OperationFailed/InvalidDomain;
nonfinite requested arithmetic or required B intermediates from finite inputs
fail OperationFailed/ArithmeticOverflow. Budget failures and cancellation retain
their native reasons and observation scope. Do not publish a failed requested
region or silently switch algorithms. Strict copies and mask decisions are exact;
B intermediate rounding is normative, not an error tolerance.

Independent oracle fixtures must include:

- A (-0,1.25,-0.25) -> (-0,1,+0); R-only succeeds even if unrequested G is NaN.
  D on that finite triple yields 1; D on (-0,1,0.5) yields +0.
- C on neutral 0.25 copies it. For the exactly dyadic custom basis Rxy=(.5,.25), Gxy=(0,1),
  Bxy=(0,.5), white=(.25,.5), the Y weights are (.25,.5,.25): x=(1.5,.25,0), knee=0.5 gives Y=.5, q=2,
  q_new=.875, output=(.9375,.390625,.28125).
- B identity copies all in-gamut bits; test both endpoint fallbacks, its neutral
  residual guard and exact iteration trace with an independently staged
  high-precision oracle. Every successful output must pass D with zero mask.
- B/C source NaN peers fail a single color request; alpha-only preserves arbitrary
  alpha bits. Cross-tile/nonzero/sparse requests and thread partitions agree.
- Exercise low budgets, per-iteration cancellation, overflow in B's first XYZ,
  multi-consumer lifetime and absence of hidden Whole or whole-image allocation.

Future public workflows explicitly compose transfer decode, FMT-10 basis/white,
chosen mapper, transfer encode and FMT-06/codec. Runtime acceptance requires
actual commands and independent expected values; no runnable API is claimed here.

## Sources and scope limits

[W3C CSS Color 4 gamut mapping](https://www.w3.org/TR/2026/CRD-css-color-4-20260825/#gamut-mapping)
is a comparison source for RGB clipping versus constant-lightness/chroma mapping.
Its SDR individual-color intent and algorithms are not adopted as image defaults.
B/C are explicitly versioned Photospider algorithms defined above, with no CSS,
commercial-renderer or ACES bit-compatibility claim. ICC/printer gamuts and their
alarms belong to [FMT-18](FMT-18_softproof_contract.md). More elaborate perceptual
compression, spatial mapping and device-specific gamut surfaces are future named
algorithms, not unspecified options of these members.
