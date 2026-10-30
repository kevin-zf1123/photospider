---
spec_schema_version: 1
id: RES-01B
kind: primitive
category: 05-filter
status: Proposed
document_maturity: D1_draft
implementation_status: implemented
parent_id: RES-01
function: calibrated_noise_variance
proposed_operation_keys:
- restoration.calibrated_noise_variance
numeric_reference: F
oracle_scope: mathematical_reference
---

# RES-01B: calibrated_noise_variance

Calibrated Poisson–Gaussian variance field.

Inherits the [RES-01 family contract](RES-01_contract.md), [FILTER common contract](FILTER_common_contract.md), [RES native floating-point contract](RES_native_contract.md), and [FMT metadata/straight semantics](../../02-format-color/op_specs/FMT_common_contract.md). The family formula and this member together define the complete draft; native fp32/fp64 arithmetic follows RES_native_contract; NUM strict bits and FP32-scaled four-ULP guarantees do not apply.

## Ports, Shape, and Semantics

`mean_signal` is a Float32/Float64 tensor of logical rank 1–8. It produces `variance,sigma`, both with the same shape and dtype. Participating mean values are finite model inputs, an unchecked caller premise. Native intermediate/output overflow follows IEEE arithmetic.

Ports carry Results with fixed Tensor member schemas. Unless the member defines an explicit color interpretation, samples are raw numerical values; channel count does not imply RGB or alpha. Color interpretation is explicit configuration. Positive extents, axes and explicit broadcasting are checked during static construction. Materialized output tiles have independent allocations. FilterBands and FrequencyGrid are proposed SDK/workflow representations under the collection contract; lowering records each selected output's fixed reads and writes.

## Static Parameters and Valid Domains

Required real parameters are `gain` a>0, finite `offset` b, and `read_sigma` s≥0, with units defined by y=a*P+b+N(0,s²). Required string `below_offset=clamp|error` chooses the mathematical domain. `clamp` applies the stated zero clamp. `error` means the caller guarantees mean_signal>=b; it does not request a production payload-validation scan, and violating that premise has no guaranteed error diagnostic. Parameters are converted to the input dtype t once; callers guarantee representability and the stated finite domain.

Use native IEEE fp32/fp64 arithmetic under [RES native floating-point contract](RES_native_contract.md). No cross-backend NaN payload or exact-expression rounding is promised. Callers guarantee the member's finite-parameter, control-model and numerical-domain premises; production execution does not validate those premises. Copy/bypass behavior and unconsumed data follow the dependency contract. Supply every parameter explicitly; constructors provide no defaults, except for constants fixed by a mathematical profile. Interpret real parameters as their stored Float64 values, not as infinitely precise decimal text.

## Mathematical Reference and Rounding Boundaries

Numeric classification: **F**, native fp32/fp64. See [RES native floating-point contract](RES_native_contract.md).

The ideal model is λ=(mean_signal-b)/a, variance=a²*max(λ,0)+s². Native evaluation eliminates the division: compute delta=max(mean_signal-b,0), read2=s*s, and variance=fma(a,delta,read2) in dtype t. Compute sigma=sqrt(variance) from that native variance, including when both outputs are requested. A sigma-only computation may reuse the value in a register without allocating or publishing variance. The input estimates expected signal; it does not claim to be the true mean of a single noisy observation.

Preserve the declared algorithmic stages, masks, support, copy branches and iteration order. Within a stage, native arithmetic, FMA, vectorization and reduction regrouping are permitted under [RES native floating-point contract](RES_native_contract.md). An explicitly called member keeps its own interface contract.

## Fixed reads, algorithmic control and schemas

Pointwise. A sigma-only selected computation has the same caller-supplied numerical premises. It computes the native variance intermediate required by the square root, which may remain in a register, without materializing or publishing a variance output image. Joint output reuses that same native variance value; variance-only omits the square root. No output subset introduces a domain-validation scan.

Boundary coordinates use the complete logical image; ROI and tile edges are not new image boundaries. Lowering closes mathematical sample support on fixed input member-tile grids. Data-dependent selection uses a finite static superset of every address the task may consume; runtime selection does not expand the Plan. Data and control are algorithmic inputs, with no separate Validation read set. Empty retained selections introduce no payload tasks. Output schemas derive only from static parameters and input schemas. Numerical, encoding and association premises remain unchecked under the common contract.

## Algorithm, Resources, and Execution

O(PC) native work. One fused pass can compute either output or both, sharing subtraction, clamping, read-noise square and variance. Output selection must be outside the inner SIMD loop where possible; no extra intermediate image is required. The selected region admits a deterministic bound of six scalar work units per sample before numerical work; independent cancellation checks occur before spans of at most 128 samples and before completion. Run and Context cumulative limits still apply.

In complexity expressions, P=HW, C is the number of independent planes, A is the number of taps, and T is the number of steps. The shared protocol defines work, memory, and stage admission; cancellation; failure status; and owner lifetime. A failed call publishes no successful Result export; previously retired call exports remain valid, and final RET results require successful Run completion. Do not spill automatically, silently reduce precision, or call a retired implementation as a fallback.

## Oracle, Fixtures, and Acceptance

For mean_signal=b+4a and s=0, variance=4a² and sigma=2a. Under below-offset clamping, only the read-noise variance remains.

The reference evaluates the mathematical formula on small inputs. Existing ExactRational/DirectedMPFR fixtures remain ideal mathematical references, not bitwise goldens for native execution. Acceptance compares native results using dtype-aware absolute/relative error and algorithm invariants under [RES native floating-point contract](RES_native_contract.md); it separately verifies runtime schemas, support and ownership.

`res01.restoration.calibrated_variance(image, *, gain, offset=0, read_sigma=0, below_offset='clamp', dtype='float64')` → [Source file](../../../../oracle/ops/filter/res01/restoration.py).


These are standalone oracle helper signatures, not operator constructors; their Python convenience defaults do not define operation parameter defaults.

The oracle README (../../../../oracle/ops/filter/README.md) documents reference scope. Runtime acceptance under the [runtime acceptance protocol](FILTER_oracle_protocol.md) must cover parameter boundaries, full rank/batch coverage, metadata, compiled ROI/tile behavior, budgets, cancellation, and ownership.

## Backend and Registration Gates

The native key `restoration.calibrated_noise_variance` is registered in the bundled operation collection. The spec remains Proposed; implementation evidence is recorded separately. Native numerical validation follows [RES native floating-point contract](RES_native_contract.md); no NUM four-ULP fallback is required.

Float64 paths retain native Float64 inputs, arithmetic, outputs and exponent range; no Float32 conversion or NUM strict fallback is introduced.

## Conceptual DAG (Not an Existing API)

`static parameters + schemas → compile fixed reads/tasks → prepare → native arithmetic with declared algorithmic stages → complete tiles → ordered call retirement`

This diagram introduces no implicit color conversion, hidden alpha premultiplication, automatic spectral correction, or zero-filling of missing data. Whether execution is complete-input or requires multistage buffers is specified by this member and its family contract.

## References and Conformance Notes

[S22 · scikit-image restoration](../research-sources.md#s22);[S18 · Buades, Coll, Morel: Non-Local Means Denoising](../research-sources.md#s18)

External sources provide algorithmic or definitional background. The finite window, rounding, tie-breaking, units, and execution profile are explicit contract choices; bitwise identity with any library or commercial product is not claimed.
