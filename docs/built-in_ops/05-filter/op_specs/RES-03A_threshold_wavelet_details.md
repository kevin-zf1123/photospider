---
spec_schema_version: 1
id: RES-03A
kind: primitive
category: 05-filter
status: Proposed
document_maturity: D2_draft
implementation_status: not_implemented
parent_id: RES-03
function: threshold_wavelet_details
proposed_operation_keys:
- restoration.threshold_wavelet_details
numeric_reference: F
oracle_scope: mathematical_reference
---

# RES-03A: threshold_wavelet_details

Wavelet detail thresholding.

Inherits the [RES-03 family contract](RES-03_contract.md), [FILTER common contract](FILTER_common_contract.md), [RES native floating-point contract](RES_native_contract.md), and [FMT metadata/straight semantics](../../02-format-color/op_specs/FMT_common_contract.md). The family formula and this member together define the complete draft; native fp32/fp64 arithmetic follows RES_native_contract; NUM strict bits and FP32-scaled four-ULP guarantees do not apply.

## Ports, Shape, and Semantics

`bands` FilterBands/v1 and `thresholds` Float64 `[levels,3]` → new `bands`; LL residual is bit-copied. Per level, order is LH_y, HL_x, HH.

Ports carry Results with fixed Tensor member schemas. Unless the member defines an explicit color interpretation, samples are raw numerical values; channel count does not imply RGB or alpha. Color interpretation is explicit configuration. Positive extents, axes and explicit broadcasting are checked during static construction. Materialized output tiles have independent allocations. FilterBands and FrequencyGrid are proposed SDK/workflow representations under the collection contract; lowering records each selected output's fixed reads and writes.

## Static Parameters and Valid Domains

Require an explicit `mode=hard|soft`; every threshold≥0. Under the positive-extent rule, this member does not accept an empty threshold table when levels=0; use the identity-bypass helper without creating this node.

Use native IEEE fp32/fp64 arithmetic under [RES native floating-point contract](RES_native_contract.md). No cross-backend NaN payload or exact-expression rounding is promised. Callers guarantee the member's finite-parameter, control-model and numerical-domain premises; production execution does not validate those premises. Copy/bypass behavior and unconsumed data follow the dependency contract. Supply every parameter explicitly; constructors provide no defaults, except for constants fixed by a mathematical profile. Interpret real parameters as their stored Float64 values, not as infinitely precise decimal text.

## Mathematical Reference and Rounding Boundaries

Numeric classification: **F**, native fp32/fp64. See [RES native floating-point contract](RES_native_contract.md).

Hard threshold: abs(d)<T yields +0; otherwise copy d, so equality retains the coefficient. Soft threshold: abs(d)≤T yields +0; otherwise `sign(d)*(abs(d)-T)` in the native dtype. At T=0 all detail coefficients are bit-copied, with no extra finite validation. For nonzero T, NaN coefficients propagate through native arithmetic and infinite coefficients follow the stated arithmetic; they are not rejected by a finite-only rule. LL adds no arithmetic validation.

Preserve the declared algorithmic stages, masks, support, copy branches and iteration order. Within a stage, native arithmetic, FMA, vectorization and reduction regrouping are permitted under [RES native floating-point contract](RES_native_contract.md). An explicitly called member keeps its own interface contract.

## Fixed reads, algorithmic control and schemas

Compilation checks the complete static band directory and selected output schemas. Lowering includes only each selected band, its threshold control and required upstream tasks. Runtime associations are caller premises, and unrelated sibling payloads are not numerical-validation inputs. An unchanged LL output preserves its bits using explicit COPY when materialized; an authoring workflow may explicitly reuse its existing Result. Whole-band mathematical dependencies remain unless locality is separately specified.

Boundary coordinates use the complete logical image; ROI and tile edges are not new image boundaries. Lowering closes mathematical sample support on fixed input member-tile grids. Data-dependent selection uses a finite static superset of every address the task may consume; runtime selection does not expand the Plan. Data and control are algorithmic inputs, with no separate Validation read set. Empty retained selections introduce no payload tasks. Output schemas derive only from static parameters and input schemas. Numerical, encoding and association premises remain unchecked under the common contract.

## Algorithm, Resources, and Execution

O(total coefficients), with native subtraction and comparisons on native values. Do not treat sigma as per-band standard deviation without a declared basis norm.

In complexity expressions, P=HW, C is the number of independent planes, A is the number of taps, and T is the number of steps. The shared protocol defines work, memory, and stage admission; cancellation; failure status; and owner lifetime. A failed call publishes no successful Result export; previously retired call exports remain valid, and final RET results require successful Run completion. Do not spill automatically, silently reduce precision, or call a retired implementation as a fallback.

## Oracle, Fixtures, and Acceptance

At d=T, hard mode retains the coefficient and soft mode clears it to zero. Preserve the sign of negative details. At T=0, preserve NaN and -0 bits. The inverse may consume the edited bands.

The reference evaluates the mathematical formula on small inputs. Existing ExactRational/DirectedMPFR fixtures remain ideal mathematical references, not bitwise goldens for native execution. Acceptance compares native results using dtype-aware absolute/relative error and algorithm invariants under [RES native floating-point contract](RES_native_contract.md); it separately verifies runtime schemas, support and ownership.

`frq10.multiscale.threshold_bands(bands, thresholds, *, mode='soft', dtype='float64')` → [Source file](../../../../oracle/ops/filter/frq10/multiscale.py).


These are standalone oracle helper signatures, not operator constructors; their Python convenience defaults do not define operation parameter defaults.

The oracle README (../../../../oracle/ops/filter/README.md) documents reference scope. Runtime acceptance under the [runtime acceptance protocol](FILTER_oracle_protocol.md) must cover parameter boundaries, full rank/batch coverage, metadata, compiled ROI/tile behavior, budgets, cancellation, and ownership.

## Backend and Registration Gates

The proposed native keys remain unregistered until implementation and runtime acceptance are complete. Native numerical validation follows [RES native floating-point contract](RES_native_contract.md); no NUM four-ULP fallback is required.

Float64 paths retain native Float64 inputs, arithmetic, outputs and exponent range; no Float32 conversion or NUM strict fallback is introduced.

## Conceptual DAG (Not an Existing API)

`static parameters + schemas → compile fixed reads/tasks → prepare → native arithmetic with declared algorithmic stages → complete tiles → ordered call retirement`

This diagram introduces no implicit color conversion, hidden alpha premultiplication, automatic spectral correction, or zero-filling of missing data. Whether execution is complete-input or requires multistage buffers is specified by this member and its family contract.

## References and Conformance Notes

[S17 · PyWavelets 2D DWT/IDWT](../research-sources.md#s17);[S22 · scikit-image restoration](../research-sources.md#s22)

External sources provide algorithmic or definitional background. The finite window, rounding, tie-breaking, units, and execution profile are explicit contract choices; bitwise identity with any library or commercial product is not claimed.
