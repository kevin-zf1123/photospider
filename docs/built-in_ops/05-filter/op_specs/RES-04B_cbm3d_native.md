---
spec_schema_version: 1
id: RES-04B
kind: primitive
category: 05-filter
status: Proposed
document_maturity: D2_draft
implementation_status: not_implemented
parent_id: RES-04
function: cbm3d_native
proposed_operation_keys: []
numeric_reference: F
oracle_scope: mathematical_reference_pending
---

# RES-04B: cbm3d_native

Color CBM3D native algorithm contract.

Inherits the [RES-04 family contract](RES-04_contract.md), [FILTER common contract](FILTER_common_contract.md), [RES native floating-point contract](RES_native_contract.md), and [FMT metadata/straight semantics](../../02-format-color/op_specs/FMT_common_contract.md). The family and this member define requirements for the native profile, whose detailed mathematical configuration remains unresolved; native fp32/fp64 arithmetic follows RES_native_contract; NUM strict bits and FP32-scaled four-ULP guarantees do not apply.

## Ports, Shape, and Semantics

`input` is a three-component opaque RGB semantic image; `sigma` is Float64 `[1]`; output has the same shape. Require sigma≥0. At sigma=0, explicitly copy without executing denoising stages. The initial RGB group must not be associated with alpha; callers must handle alpha explicitly.

Ports carry Results with fixed Tensor member schemas. Unless the member defines an explicit color interpretation, samples are raw numerical values; channel count does not imply RGB or alpha. Color interpretation is explicit configuration. Positive extents, axes and explicit broadcasting are checked during static construction. Materialized output tiles have independent allocations. FilterBands and FrequencyGrid are proposed SDK/workflow representations under the collection contract; lowering records each selected output's fixed reads and writes.

## Static Parameters and Valid Domains

The family lists the native profile details that must be finalized before registration; it does not yet supply their values. This member requires explicit source color interpretation and a precisely specified color transform; Results do not carry source color metadata. Do not divide by 255 automatically; reject alpha, non-RGB, and AOV inputs.

Use native IEEE fp32/fp64 arithmetic under [RES native floating-point contract](RES_native_contract.md). No cross-backend NaN payload or exact-expression rounding is promised. Callers guarantee the member's finite-parameter, control-model and numerical-domain premises; production execution does not validate those premises. Copy/bypass behavior and unconsumed data follow the dependency contract. Supply every parameter explicitly; constructors provide no defaults, except for constants fixed by a mathematical profile. Interpret real parameters as their stored Float64 values, not as infinitely precise decimal text.

## Mathematical Reference and Rounding Boundaries

Numeric classification: **F**, native fp32/fp64. See [RES native floating-point contract](RES_native_contract.md).

For sigma>0, use the native two-stage CBM3D algorithm with an explicitly fixed mathematical profile; no callable key is registered while that profile is unresolved. Do not substitute NLM/TV or add video-frame or volumetric dimensions. Native arithmetic follows RES_native_contract and the explicitly selected FMT color-domain constraints. Any narrower mathematical domain must be stated by the native profile, not inherited from a comparison library input restriction.

Preserve the declared algorithmic stages, masks, support, copy branches and iteration order. Within a stage, native arithmetic, FMA, vectorization and reduction regrouping are permitted under [RES native floating-point contract](RES_native_contract.md). An explicitly called member keeps its own interface contract.

## Fixed reads, algorithmic control and schemas

complete-input demand for the plane or RGB group; resource identity is Control. There is no fixed local halo. At sigma=0, copy input only and perform no algorithm-domain finite validation.

Boundary coordinates use the complete logical image; ROI and tile edges are not new image boundaries. Lowering closes mathematical sample support on fixed input member-tile grids. Data-dependent selection uses a finite static superset of every address the task may consume; runtime selection does not expand the Plan. Data and control are algorithmic inputs, with no separate Validation read set. Empty retained selections introduce no payload tasks. Output schemas derive only from static parameters and input schemas. Numerical, encoding and association premises remain unchecked under the common contract.

## Algorithm, Resources, and Execution

Account for native block matching, group transforms, both filtering stages, aggregation buffers and native transform/reduction work. Complexity depends on the finalized block/search/group parameters; no generic O(P) bound is asserted. Publish only after required stages succeed. Native loops obey the shared cancellation polling bound, including long searches and transforms.

In complexity expressions, P=HW, C is the number of independent planes, A is the number of taps, and T is the number of steps. The shared protocol defines work, memory, and stage admission; cancellation; failure status; and owner lifetime. A failed call publishes no successful Result export; previously retired call exports remain valid, and final RET results require successful Run completion. Do not spill automatically, silently reduce precision, or call a retired implementation as a fallback.

## Oracle, Fixtures, and Acceptance

The third-party comparison golden set covers sigma=0, impulses, seeded Gaussian noise, flat/ramp/texture inputs, color/input scales, and tile crops. Mark cases pending until the actual comparison has been run; package unit tests are not a substitute.

This member requires a native implementation and a comparison against a pinned third-party implementation. The specific comparison version, native profile, and resource details remain unresolved. A manifest-shape check alone establishes only field completeness; resource hashes, licensing, native numerical accuracy, and actual golden results remain unresolved, and the proposed member remains unregistered.

`comparison_protocol.cbm3d(manifest)` → [Source file](../../../../oracle/ops/filter/common/comparison_protocol.py).


These are standalone oracle helper signatures, not operator constructors; their Python convenience defaults do not define operation parameter defaults.

The [oracle README](../../../../oracle/ops/filter/README.md) documents reference scope. Runtime acceptance under the [runtime acceptance protocol](FILTER_oracle_protocol.md) must cover parameter boundaries, full rank/batch coverage, metadata, compiled ROI/tile behavior, budgets, cancellation, and ownership.

## Backend and Registration Gates

This file specifies a native algorithm. Registration requires finalized profiles plus resolved resource, licensing, actual-golden, and numeric-configuration gates.

Float64 paths retain native Float64 inputs, arithmetic, outputs and exponent range; no Float32 conversion or NUM strict fallback is introduced.

## Conceptual DAG (Not an Existing API)

`static parameters + schemas → compile fixed reads/tasks → prepare → native arithmetic with declared algorithmic stages → complete tiles → ordered call retirement`

This diagram introduces no implicit color conversion, hidden alpha premultiplication, automatic spectral correction, or zero-filling of missing data. Whether execution is complete-input or requires multistage buffers is specified by this member and its family contract.

## References and Conformance Notes

[S20 · BM3D author project](../research-sources.md#s20)

External sources provide algorithmic or definitional background. The finite window, rounding, tie-breaking, units, and execution profile are explicit contract choices; bitwise identity with any library or commercial product is not claimed.


The algorithm is to be implemented natively. No third-party engine, version, build, or resource set is pinned by this contract yet. A later conformance record must identify the comparison implementation and version, fixed inputs and parameters, observable stages, metrics, tolerances, and unobservable stages. Native numerical correctness and third-party comparison are separate acceptance activities.
