---
spec_schema_version: 1
id: RES-01A
kind: primitive
category: 05-filter
status: Proposed
document_maturity: D2_draft
implementation_status: not_implemented
parent_id: RES-01
function: noise_mad_highpass
proposed_operation_keys:
- restoration.noise_mad_highpass_strict
- restoration.noise_mad_highpass_accelerated_apple_silicon
- restoration.noise_mad_highpass_accelerated_x86_64
numeric_reference: S+E
oracle_scope: mathematical_reference
---

# RES-01A: noise_mad_highpass

High-pass MAD noise scale.

Inherits the [RES-01 family contract](RES-01_contract.md), [FILTER common contract](FILTER_common_contract.md), [NUM numeric/accuracy contract](../../01-numeric/op_specs/NUM_common_contract.md), and [FMT metadata/straight semantics](../../02-format-color/op_specs/FMT_common_contract.md). The family formula and this member together define the complete draft; RN, underflow, and accelerated error baselines cannot be overridden here.

## Ports, Shape, and Semantics

`input,mask` → `sigma`, one Float64 `[1]` scalar per independent plane; `mask` is UInt8 with the same H/W and values 0/1. It is explicit, not a hidden optional input; connect an all-ones mask when no mask is used.

Except at a semantic entry point explicitly defined by this member, inputs are raw numerical fields: channel count does not imply RGB or alpha, and attached color descriptions do not broaden numeric-domain validation. Axes, positive extents, explicit broadcasting, canonical planar publication, and preservation/reconstruction of applicable metadata follow the common contract. Components and multiple outputs declare dependencies per request; associated collections follow the FilterBands/FrequencyGrid drafts.

## Static Parameters and Valid Domains

Explicit axis; fixed high-pass kernel `[1,-1;-1,1]/2`, evaluated only at valid 2×2 positions; `min_samples` Int64≥1 and explicitly supplied; even-sample median uses the fixed mean rule.

Inherit NaN, Inf, overflow, signed-zero, payload, rounding, and precision semantics from the corresponding NUM operations. Validate any finite-parameter, finite-control-model, or narrower mathematical domain explicitly required by this member. Copy/bypass behavior and unconsumed data follow the dependency contract. Supply every parameter explicitly; constructors provide no defaults, except for constants fixed by a mathematical profile. Interpret real parameters as their stored Float64 values, not as infinitely precise decimal text.

## Mathematical Reference and Rounding Boundaries

Numeric classification: **S+E**. See [FILTER_numeric_reference](FILTER_numeric_reference.md) for the E/B/S notation.

Define r(y,x)=RN64((I00-I01-I10+I11)/2); a residual is selected only when all four source mask values are 1. First compute m as the exact-average median of r (RN64 may be materialized as the S-stage center); this profile sets m=RN64(median(r)). Compute d_i=abs(r_i-m) exactly, MAD as the exact median of d, and sigma=RN64(MAD/c), where c=Φ^-1(.75) is the mathematical normal quantile and is not fixed to 0.6745. The residual kernel has L2 norm 1, but texture or correlated noise can bias the estimate; it is not claimed unbiased.

If a formula calls another member, preserve any RN stage at the call site as specified here or by the family contract. Internal temporary operations must not introduce undeclared rounding. Signed zero, dynamic-domain failures, gradual underflow, and output range follow the common contract and NUM.

## Data / Control / Validation / Descriptor

Whole demand for that plane’s mask. Data reads only valid patches whose four mask values are all 1. Do not infer shape from the selected count; that count determines success or failure at runtime only.

Boundary coordinates are defined against the complete logical image; ROI/tile edges do not become new boundaries. An empty request reads no payload. Selection conditions retain Control dirty witnesses. Exact support must not be replaced by a convenient rectangular or Whole read; any expansion requires an explicit Conservative plan. Output descriptors derive from static parameters and input descriptors, not input sample values.

## Algorithm, Resources, and Execution

O(P log P) sorting reference with exact rank ties; constant input yields MAD=0; c uses a certified constant enclosure.

In complexity expressions, P=HW, C is the number of independent planes, A is the number of taps, and T is the number of steps. Arbitrary-precision limb bit complexity is additional and is not a measured benchmark. The shared protocol defines work, memory, and stage admission; cancellation; failure status; and owner lifetime. Failure publishes no partial Value or CompleteBundle. Do not spill automatically, silently reduce precision, or call a retired implementation as a fallback.

## Oracle, Fixtures, and Acceptance

Constant/ramp input gives zero residual and sigma; checker texture is a negative example where texture may be mistaken for noise. Zero selected samples or fewer than min_samples yields InvalidDomain. A zero mask entry must not read poisoned source data.

The reference function evaluates the numerical formula on small inputs; it is neither a production kernel nor a complete port-schema/preflight simulator. ExactRational uses rational expressions with direct IEEE rounding. For transcendental functions, only a DirectedMPFR result whose interval is closed to a unique rounded value is a strict golden. If the interval cannot determine rounding, report Inconclusive; do not substitute an approximation.

`restoration.noise_mad(image, mask, *, min_samples=1)` → [Source file](../../../../oracle/ops/filter/oracles/restoration.py).


These are standalone oracle helper signatures, not operator constructors; their Python convenience defaults do not define operation parameter defaults.

The oracle README (../../../../oracle/ops/filter/README.md) documents reference scope. Runtime acceptance under the [runtime acceptance protocol](FILTER_oracle_protocol.md) must cover parameter boundaries, full rank/batch coverage, metadata, ROI/dirty behavior, budgets, cancellation, and ownership.

## Backend and Registration Gates

The keys above are naming proposals only; strict and both CPU accelerated profiles are unregistered and unimplemented. Accelerated profiles must meet the NUM final-output FP32-scaled four-ULP bound and pass fallback checks; the error budget cannot accumulate per tap, axis, stage, or iteration. Approximation must not change thresholds, ordering, boundaries, copies, or output support where exact selection is required.

Float64 accelerated profiles retain Float64 inputs, outputs, and exponent range; they do not first convert to Float32. Values outside the Float32 scale range use the NUM strict fallback.

## Conceptual DAG (Not an Existing API)

`static + descriptors → preflight / demand → Data and Control declared by this member → exact expression / declared RN stages → requested owned output / complete result`

This diagram introduces no implicit color conversion, hidden alpha premultiplication, automatic spectral correction, or zero-filling of missing data. Whether execution is Whole or requires multistage buffers is specified by this member and its family contract.

## References and Conformance Notes

[S22 · scikit-image restoration](../research-sources.md#s22);[S18 · Buades, Coll, Morel: Non-Local Means Denoising](../research-sources.md#s18)

External sources provide algorithmic or definitional background. The finite window, rounding, tie-breaking, units, and execution profile are explicit contract choices; bitwise identity with any library or commercial product is not claimed.
