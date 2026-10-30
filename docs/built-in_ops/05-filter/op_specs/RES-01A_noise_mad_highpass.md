---
spec_schema_version: 1
id: RES-01A
kind: primitive
category: 05-filter
status: Proposed
document_maturity: D2_draft
implementation_status: implemented
parent_id: RES-01
function: noise_mad_highpass
proposed_operation_keys:
- restoration.noise_mad_highpass
numeric_reference: F
oracle_scope: mathematical_reference
---

# RES-01A: noise_mad_highpass

High-pass MAD noise scale.

Inherits the [RES-01 family contract](RES-01_contract.md), [FILTER common contract](FILTER_common_contract.md), [RES native floating-point contract](RES_native_contract.md), and [FMT metadata/straight semantics](../../02-format-color/op_specs/FMT_common_contract.md). The family formula and this member together define the complete draft; native fp32/fp64 arithmetic follows RES_native_contract; NUM strict bits and FP32-scaled four-ULP guarantees do not apply.

## Ports, Shape, and Semantics

`input,mask` → `sigma`. Input is Float32 or Float64, rank 2–8; explicit distinct `y_axis,x_axis` select the spatial plane. Other dimensions identify independent planes in their original order. Output shape is those nonspatial dimensions followed by a terminal extent 1, with Float64 dtype; a rank-2 image therefore produces `[1]`. `mask` is UInt8 with the same complete input shape and values 0/1. It is explicit, not a hidden optional input; connect an all-ones mask when no mask is used.

Ports carry Results with fixed Tensor member schemas. Unless the member defines an explicit color interpretation, samples are raw numerical values; channel count does not imply RGB or alpha. Color interpretation is explicit configuration. Positive extents, axes and explicit broadcasting are checked during static construction. Materialized output tiles have independent allocations. FilterBands and FrequencyGrid are proposed SDK/workflow representations under the collection contract; lowering records each selected output's fixed reads and writes.

## Static Parameters and Valid Domains

Required Int64 `y_axis,x_axis`; fixed high-pass kernel `[1,-1;-1,1]/2`, evaluated only at valid 2×2 positions; `min_samples` Int64≥1 and explicitly supplied; even-sample median uses the fixed mean rule.

Use native IEEE fp32/fp64 arithmetic under [RES native floating-point contract](RES_native_contract.md). No cross-backend NaN payload or exact-expression rounding is promised. Callers guarantee the member's finite-parameter, control-model and numerical-domain premises; production execution does not validate those premises. Copy/bypass behavior and unconsumed data follow the dependency contract. Supply every parameter explicitly; constructors provide no defaults, except for constants fixed by a mathematical profile. Interpret real parameters as their stored Float64 values, not as infinitely precise decimal text.

## Mathematical Reference and Rounding Boundaries

Numeric classification: **F**, native fp32/fp64. See [RES native floating-point contract](RES_native_contract.md).

In input dtype t, compute r(y,x)=((I00-I01)-(I10-I11))*0.5. A residual is selected only when all four source mask values are 1. Compute the exact middle rank(s) of these native residual values. For an even count, average the two middle values in dtype t, avoiding avoidable overflow; this native midpoint is m. Form native d_i=abs(r_i-m) and take the same median of d. For c=Φ^-1(.75), form scale64=RN64(1/c) once and publish sigma=Float64(MAD)*scale64 with native Float64 multiplication. The reciprocal is correctly rounded from the mathematical quantile; do not use the truncated 0.6745. Participating input/residual/deviation values must be finite; this is an unchecked numerical premise. The residual kernel has L2 norm 1, but texture or correlated noise can bias the estimate; it is not claimed unbiased.

Preserve the declared algorithmic stages, masks, support, copy branches and iteration order. Within a stage, native arithmetic, FMA, vectorization and reduction regrouping are permitted under [RES native floating-point contract](RES_native_contract.md). An explicitly called member keeps its own interface contract.

## Fixed reads, algorithmic control and schemas

The estimator consumes the complete plane mask as algorithmic control. Only patches whose four mask values are all 1 participate in image arithmetic. Lowering declares a finite static input superset for potentially selected patches. The count does not change output shape; fewer than min_samples is the explicitly defined insufficient-samples outcome on otherwise valid input, not mask-encoding validation.

Boundary coordinates use the complete logical image; ROI and tile edges are not new image boundaries. Lowering closes mathematical sample support on fixed input member-tile grids. Data-dependent selection uses a finite static superset of every address the task may consume; runtime selection does not expand the Plan. Data and control are algorithmic inputs, with no separate Validation read set. Empty retained selections introduce no payload tasks. Output schemas derive only from static parameters and input schemas. Numerical, encoding and association premises remain unchecked under the common contract.

## Algorithm, Resources, and Execution

O(P log P) sorting is a reference algorithm. Exact rank selection, SIMD residual/deviation loops, and sample-guided partitioning with rank verification are permitted. Sampling must not approximate the median. Scratch is O(P) per active plane group. Constant input yields MAD=0; the Float64 scale constant derives from the mathematical normal quantile.

In complexity expressions, P=HW, C is the number of independent planes, A is the number of taps, and T is the number of steps. The shared protocol defines work, memory, and stage admission; cancellation; failure status; and owner lifetime. A failed call publishes no successful Result export; previously retired call exports remain valid, and final RET results require successful Run completion. Do not spill automatically, silently reduce precision, or call a retired implementation as a fallback.

## Oracle, Fixtures, and Acceptance

Constant/ramp input gives zero residual and sigma; checker texture is a negative example where texture may be mistaken for noise. Zero selected samples or fewer than min_samples yields OperationFailed with reason InvalidDomain. This includes a valid descriptor with H=1 or W=1 (such as 1×65536), which has no valid 2×2 patch. A zero mask entry must not read poisoned source data.

The reference evaluates the mathematical formula on small inputs. Existing ExactRational/DirectedMPFR fixtures remain ideal mathematical references, not bitwise goldens for native execution. Acceptance compares native results using dtype-aware absolute/relative error and algorithm invariants under [RES native floating-point contract](RES_native_contract.md); it separately verifies runtime schemas, support and ownership.

`res01.restoration.noise_mad(image, mask, *, min_samples=1, dtype="float64")` → [Source file](../../../../oracle/ops/filter/res01/restoration.py).


These are standalone oracle helper signatures, not operator constructors; their Python convenience defaults do not define operation parameter defaults.

The oracle README (../../../../oracle/ops/filter/README.md) documents reference scope. Runtime acceptance under the [runtime acceptance protocol](FILTER_oracle_protocol.md) must cover parameter boundaries, full rank/batch coverage, metadata, compiled ROI/tile behavior, budgets, cancellation, and ownership.

## Backend and Registration Gates

The native key `restoration.noise_mad_highpass` is registered in the bundled operation collection. The spec remains Proposed; implementation evidence is recorded separately. Native numerical validation follows [RES native floating-point contract](RES_native_contract.md); no NUM four-ULP fallback is required.

Float64 paths retain native Float64 inputs, arithmetic, outputs and exponent range; no Float32 conversion or NUM strict fallback is introduced.

## Conceptual DAG (Not an Existing API)

`static parameters + schemas → compile fixed reads/tasks → prepare → native arithmetic with declared algorithmic stages → complete tiles → ordered call retirement`

This diagram introduces no implicit color conversion, hidden alpha premultiplication, automatic spectral correction, or zero-filling of missing data. Whether execution is complete-input or requires multistage buffers is specified by this member and its family contract.

## References and Conformance Notes

[S22 · scikit-image restoration](../research-sources.md#s22);[S18 · Buades, Coll, Morel: Non-Local Means Denoising](../research-sources.md#s18)

External sources provide algorithmic or definitional background. The finite window, rounding, tie-breaking, units, and execution profile are explicit contract choices; bitwise identity with any library or commercial product is not claimed.
