---
spec_schema_version: 1
id: RES-07C
kind: primitive
category: 05-filter
status: Proposed
document_maturity: D2_draft
implementation_status: not_implemented
parent_id: RES-07
function: anscombe_inverse_mean
proposed_operation_keys:
- restoration.anscombe_inverse_mean
numeric_reference: F
oracle_scope: diagnostic_inverse_not_strict_golden
---

# RES-07C: anscombe_inverse_mean

Poisson mean-domain inverse.

Inherits the [RES-07 family contract](RES-07_contract.md), [FILTER common contract](FILTER_common_contract.md), [RES native floating-point contract](RES_native_contract.md), and [FMT metadata/straight semantics](../../02-format-color/op_specs/FMT_common_contract.md). The family formula and this member together define the complete draft; native fp32/fp64 arithmetic follows RES_native_contract; NUM strict bits and FP32-scaled four-ULP guarantees do not apply.

## Ports, Shape, and Semantics

`denoised_stabilized_mean` → `mean_count`.

Ports carry Results with fixed Tensor member schemas. Unless the member defines an explicit color interpretation, samples are raw numerical values; channel count does not imply RGB or alpha. Color interpretation is explicit configuration. Positive extents, axes and explicit broadcasting are checked during static construction. Materialized output tiles have independent allocations. FilterBands and FrequencyGrid are proposed SDK/workflow representations under the collection contract; lowering records each selected output's fixed reads and writes.

## Static Parameters and Valid Domains

Require z≥0; the mean-inverse definition is fixed. Solver precision and work limits are execution resources, not quality parameters that change the mathematical result.

Use native IEEE fp32/fp64 arithmetic under [RES native floating-point contract](RES_native_contract.md). No cross-backend NaN payload or exact-expression rounding is promised. Callers guarantee the member's finite-parameter, control-model and numerical-domain premises; production execution does not validate those premises. Copy/bypass behavior and unconsumed data follow the dependency contract. Supply every parameter explicitly; constructors provide no defaults, except for constants fixed by a mathematical profile. Interpret real parameters as their stored Float64 values, not as infinitely precise decimal text.

## Mathematical Reference and Rounding Boundaries

Numeric classification: **F**, native fp32/fp64. See [RES native floating-point contract](RES_native_contract.md).

The mathematical target is the inverse of family m: return +0 when z≤m(0), otherwise the unique root λ. Native evaluation must document function/tail and root tolerances for each dtype and validate against an independent higher-precision reference; exact final rounding is not required. Do not substitute the algebraic inverse or an unvalidated fit.

Preserve the declared algorithmic stages, masks, support, copy branches and iteration order. Within a stage, native arithmetic, FMA, vectorization and reduction regrouping are permitted under [RES native floating-point contract](RES_native_contract.md). An explicitly called member keeps its own interface contract.

## Fixed reads, algorithmic control and schemas

Pointwise, but the infinite sum and refinement work per point can be substantial; report budget exhaustion through the existing task/call failure fields, without requiring a per-sample diagnostic.

Boundary coordinates use the complete logical image; ROI and tile edges are not new image boundaries. Lowering closes mathematical sample support on fixed input member-tile grids. Data-dependent selection uses a finite static superset of every address the task may consume; runtime selection does not expand the Plan. Data and control are algorithmic inputs, with no separate Validation read set. Empty retained selections introduce no payload tasks. Output schemas derive only from static parameters and input schemas. Numerical, encoding and association premises remain unchecked under the common contract.

## Algorithm, Resources, and Execution

Use a Poisson-tail enclosure and monotone root isolation. The accompanying oracle is HighPrecisionDiagnostic and does not certify correct rounding.

In complexity expressions, P=HW, C is the number of independent planes, A is the number of taps, and T is the number of steps. The shared protocol defines work, memory, and stage admission; cancellation; failure status; and owner lifetime. A failed call publishes no successful Result export; previously retired call exports remain valid, and final RET results require successful Run completion. Do not spill automatically, silently reduce precision, or call a retired implementation as a fallback.

## Oracle, Fixtures, and Acceptance

Values at or below m(0) map to zero. A diagnostic may generate high-precision m from a known λ and invert it; low counts differ materially from the algebraic inverse. For very large parameters, report resource limits rather than silently approximating.

The current reference returns HighPrecisionDiagnostic: it numerically evaluates the Poisson mean function and solves for its root, but has no joint proof for the infinite Poisson tail and final rounding interval. Its values cannot serve as strict goldens. Before native registration, document truncation/root tolerances and independently validate error across the supported range; exact final rounding is not required and algebraic-inverse fallback is forbidden.

`res07.restoration.anscombe_mean_inverse(z, *, dps=70, max_lambda=100)` → [Source file](../../../../oracle/ops/filter/res07/restoration.py).


These are standalone oracle helper signatures, not operator constructors; their Python convenience defaults do not define operation parameter defaults.

The oracle README (../../../../oracle/ops/filter/README.md) documents reference scope. Runtime acceptance under the [runtime acceptance protocol](FILTER_oracle_protocol.md) must cover parameter boundaries, full rank/batch coverage, metadata, compiled ROI/tile behavior, budgets, cancellation, and ownership.

## Backend and Registration Gates

The proposed native keys remain unregistered until implementation and runtime acceptance are complete. Native numerical validation follows [RES native floating-point contract](RES_native_contract.md); no NUM four-ULP fallback is required.

Float64 paths retain native Float64 inputs, arithmetic, outputs and exponent range; no Float32 conversion or NUM strict fallback is introduced.

## Conceptual DAG (Not an Existing API)

`static parameters + schemas → compile fixed reads/tasks → prepare → native arithmetic with declared algorithmic stages → complete tiles → ordered call retirement`

This diagram introduces no implicit color conversion, hidden alpha premultiplication, automatic spectral correction, or zero-filling of missing data. Whether execution is complete-input or requires multistage buffers is specified by this member and its family contract.

## References and Conformance Notes

[S21 · Mäkitalo, Foi: inverse Anscombe research](../research-sources.md#s21)

External sources provide algorithmic or definitional background. The finite window, rounding, tie-breaking, units, and execution profile are explicit contract choices; bitwise identity with any library or commercial product is not claimed.
