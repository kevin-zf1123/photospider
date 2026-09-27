---
spec_schema_version: 1
id: RES-04A
kind: primitive
category: 05-filter
status: Proposed
document_maturity: D2_draft
implementation_status: not_implemented
parent_id: RES-04
function: bm3d_native
proposed_operation_keys: []
numeric_reference: native mathematical profile; third-party comparison pending
oracle_scope: mathematical_reference_pending
---

# RES-04A: bm3d_native

Grayscale BM3D native algorithm contract.

Inherits the [RES-04 family contract](RES-04_contract.md), [FILTER common contract](FILTER_common_contract.md), [NUM numeric/accuracy contract](../../01-numeric/op_specs/NUM_common_contract.md), and [FMT metadata/straight semantics](../../02-format-color/op_specs/FMT_common_contract.md). The family and this member define requirements for the native profile, whose detailed mathematical configuration remains unresolved; RN, underflow, and accelerated error baselines cannot be overridden here.

## Ports, Shape, and Semantics

`input` is a single-plane Float32/Float64 field; `sigma` is Float64 `[1]`; output has the same shape. Require sigma≥0. At sigma=0, explicitly copy without executing denoising stages.

Except at a semantic entry point explicitly defined by this member, inputs are raw numerical fields: channel count does not imply RGB or alpha, and attached color descriptions do not broaden numeric-domain validation. Axes, positive extents, explicit broadcasting, canonical planar publication, and preservation/reconstruction of applicable metadata follow the common contract. Components and multiple outputs declare dependencies per request; associated collections follow the FilterBands/FrequencyGrid drafts.

## Static Parameters and Valid Domains

The family lists the native profile details that must be finalized before registration; it does not yet supply their values. This member accepts a grayscale single-plane input. Do not divide by 255 automatically; require one numerical plane; raw field metadata does not imply a color or alpha interpretation.

Inherit NaN, Inf, overflow, signed-zero, payload, rounding, and precision semantics from the corresponding NUM operations. Validate any finite-parameter, finite-control-model, or narrower mathematical domain explicitly required by this member. Copy/bypass behavior and unconsumed data follow the dependency contract. Supply every parameter explicitly; constructors provide no defaults, except for constants fixed by a mathematical profile. Interpret real parameters as their stored Float64 values, not as infinitely precise decimal text.

## Mathematical Reference and Rounding Boundaries

Numeric classification: **native mathematical profile; third-party comparison pending**. See [FILTER_numeric_reference](FILTER_numeric_reference.md) for E/B/S notation.

For sigma>0, use the native two-stage BM3D algorithm with an explicitly fixed mathematical profile; no callable key is registered while that profile is unresolved. Do not substitute NLM/TV or add video-frame or volumetric dimensions. Native arithmetic follows NUM for signed/HDR values. Parameter/control domains must be specified by the native mathematical profile, not inherited from a third-party library input restriction.

If a formula calls another member, preserve any RN stage at the call site as specified here or by the family contract. Internal temporary operations must not introduce undeclared rounding. Signed zero, dynamic-domain failures, gradual underflow, and output range follow the common contract and NUM.

## Data / Control / Validation / Descriptor

Whole demand for the plane or RGB group; resource identity is Control. There is no fixed local halo. At sigma=0, copy input only and perform no algorithm-domain finite validation.

Boundary coordinates are defined against the complete logical image; ROI/tile edges do not become new boundaries. An empty request reads no payload. Selection conditions retain Control dirty witnesses. Exact support must not be replaced by a convenient rectangular or Whole read; any expansion requires an explicit Conservative plan. Output descriptors derive from static parameters and input descriptors, not input sample values.

## Algorithm, Resources, and Execution

Account for native block matching, group transforms, both filtering stages, aggregation buffers and arbitrary-precision work. Complexity depends on the finalized block/search/group parameters; no generic O(P) bound is asserted. Publish only after required stages succeed. Native loops obey the shared cancellation polling bound, including long searches and transforms.

In complexity expressions, P=HW, C is the number of independent planes, A is the number of taps, and T is the number of steps. Arbitrary-precision limb bit complexity is additional and is not a measured benchmark. The shared protocol defines work, memory, and stage admission; cancellation; failure status; and owner lifetime. Failure publishes no partial Value or CompleteBundle. Do not spill automatically, silently reduce precision, or call a retired implementation as a fallback.

## Oracle, Fixtures, and Acceptance

The third-party comparison golden set covers sigma=0, impulses, seeded Gaussian noise, flat/ramp/texture inputs, color/input scales, and tile crops. Mark cases pending until the actual comparison has been run; package unit tests are not a substitute.

This member requires a native implementation and a comparison against a pinned third-party implementation. The specific comparison version, native profile, and resource details remain unresolved. A manifest-shape check alone establishes only field completeness; resource hashes, licensing, NUM compliance, and actual golden results remain unresolved, and the proposed member remains unregistered.

`comparison_protocol.bm3d(manifest)` → [Source file](../../../../oracle/ops/filter/oracles/comparison_protocol.py).


These are standalone oracle helper signatures, not operator constructors; their Python convenience defaults do not define operation parameter defaults.

The [oracle README](../../../../oracle/ops/filter/README.md) documents reference scope. Runtime acceptance under the [runtime acceptance protocol](FILTER_oracle_protocol.md) must cover parameter boundaries, full rank/batch coverage, metadata, ROI/dirty behavior, budgets, cancellation, and ownership.

## Backend and Registration Gates

This file specifies a native algorithm. Registration requires finalized profiles plus resolved resource, licensing, actual-golden, and numeric-configuration gates.

Float64 accelerated profiles retain Float64 inputs, outputs, and exponent range; they do not first convert to Float32. Values outside the Float32 scale range use the NUM strict fallback.

## Conceptual DAG (Not an Existing API)

`static + descriptors → preflight / demand → Data and Control declared by this member → exact expression / declared RN stages → requested owned output / complete result`

This diagram introduces no implicit color conversion, hidden alpha premultiplication, automatic spectral correction, or zero-filling of missing data. Whether execution is Whole or requires multistage buffers is specified by this member and its family contract.

## References and Conformance Notes

[S20 · BM3D author project](../research-sources.md#s20)

External sources provide algorithmic or definitional background. The finite window, rounding, tie-breaking, units, and execution profile are explicit contract choices; bitwise identity with any library or commercial product is not claimed.


The algorithm is to be implemented natively. No third-party engine, version, build, or resource set is pinned by this contract yet. A later conformance record must identify the comparison implementation and version, fixed inputs and parameters, observable stages, metrics, tolerances, and unobservable stages. Native correctness under NUM and third-party comparison are separate acceptance activities.
