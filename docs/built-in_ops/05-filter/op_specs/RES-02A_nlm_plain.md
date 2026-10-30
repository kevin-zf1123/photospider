---
spec_schema_version: 1
id: RES-02A
kind: primitive
category: 05-filter
status: Proposed
document_maturity: D2_draft
implementation_status: not_implemented
parent_id: RES-02
function: nlm_plain
proposed_operation_keys:
- restoration.nlm_plain
numeric_reference: F
oracle_scope: mathematical_reference
---

# RES-02A: nlm_plain

Plain NLM.

Inherits the [RES-02 family contract](RES-02_contract.md), [FILTER common contract](FILTER_common_contract.md), [RES native floating-point contract](RES_native_contract.md), and [FMT metadata/straight semantics](../../02-format-color/op_specs/FMT_common_contract.md). The family formula and this member together define the complete draft; native fp32/fp64 arithmetic follows RES_native_contract; NUM strict bits and FP32-scaled four-ULP guarantees do not apply.

## Ports, Shape, and Semantics

`input,guide` → `output` with the input shape. `noise_sigma` is a spatially invariant Float64 scalar; the model assumes equal noise sigma across all scaled guide components.

Ports carry Results with fixed Tensor member schemas. Unless the member defines an explicit color interpretation, samples are raw numerical values; channel count does not imply RGB or alpha. Color interpretation is explicit configuration. Positive extents, axes and explicit broadcasting are checked during static construction. Materialized output tiles have independent allocations. FilterBands and FrequencyGrid are proposed SDK/workflow representations under the collection contract; lowering records each selected output's fixed reads and writes.

## Static Parameters and Valid Domains

Require patch_radius≥0, search_radius≥0, and h>0. Specify G, metric_scale, and axes explicitly. `noise_sigma`≥0 is used only by member B.

Use native IEEE fp32/fp64 arithmetic under [RES native floating-point contract](RES_native_contract.md). No cross-backend NaN payload or exact-expression rounding is promised. Callers guarantee the member's finite-parameter, control-model and numerical-domain premises; production execution does not validate those premises. Copy/bypass behavior and unconsumed data follow the dependency contract. Supply every parameter explicitly; constructors provide no defaults, except for constants fixed by a mathematical profile. Interpret real parameters as their stored Float64 values, not as infinitely precise decimal text.

## Mathematical Reference and Rounding Boundaries

Numeric classification: **F**, native fp32/fp64. See [RES native floating-point contract](RES_native_contract.md).

Use the family patch rules. For p≠q, w=exp(-D/h²) in the input dtype. Distances, weights, numerator and denominator use native input precision; the self-weight of 1 guarantees a positive denominator.

Preserve the declared algorithmic stages, masks, support, copy branches and iteration order. Within a stage, native arithmetic, FMA, vectorization and reduction regrouping are permitted under [RES native floating-point contract](RES_native_contract.md). An explicitly called member keeps its own interface contract.

## Fixed reads, algorithmic control and schemas

Guide support is search+patch, at most H(s+r). Input reads only candidate centers in the search window. A zero-underflow weight retains guide Control demand; input[p] may then remain unconsumed. With search_radius=0, only the input center is read.

Boundary coordinates use the complete logical image; ROI and tile edges are not new image boundaries. Lowering closes mathematical sample support on fixed input member-tile grids. Data-dependent selection uses a finite static superset of every address the task may consume; runtime selection does not expand the Plan. Data and control are algorithmic inputs, with no separate Validation read set. Empty retained selections introduce no payload tasks. Output schemas derive only from static parameters and input schemas. Numerical, encoding and association premises remain unchecked under the common contract.

## Algorithm, Resources, and Execution

Direct cost is O(PC*search_area*patch_area*G). Sliding-window or integral patch SSD may reuse data while retaining the native algorithm and measured error; similar-block regrouping is a separate member.

In complexity expressions, P=HW, C is the number of independent planes, A is the number of taps, and T is the number of steps. The shared protocol defines work, memory, and stage admission; cancellation; failure status; and owner lifetime. A failed call publishes no successful Result export; previously retired call exports remain valid, and final RET results require successful Run completion. Do not spill automatically, silently reduce precision, or call a retired implementation as a fallback.

## Oracle, Fixtures, and Acceptance

A constant guide yields the candidate mean. search_radius=0 is identity. Check patch_radius=0 and single-pixel search against analytic weights. For member B, sigma=0 reduces to member A. Rescaling guide units requires corresponding rescaling of h and sigma.

The reference evaluates the mathematical formula on small inputs. Existing ExactRational/DirectedMPFR fixtures remain ideal mathematical references, not bitwise goldens for native execution. Acceptance compares native results using dtype-aware absolute/relative error and algorithm invariants under [RES native floating-point contract](RES_native_contract.md); it separately verifies runtime schemas, support and ownership.

`res02.restoration.nlm(image, guide, *, patch_radius=1, search_radius=1, h_parameter=1, metric_scale=None, noise_sigma=None, dtype='float64')` → [Source file](../../../../oracle/ops/filter/res02/restoration.py).


These are standalone oracle helper signatures, not operator constructors; their Python convenience defaults do not define operation parameter defaults.

The oracle README (../../../../oracle/ops/filter/README.md) documents reference scope. Runtime acceptance under the [runtime acceptance protocol](FILTER_oracle_protocol.md) must cover parameter boundaries, full rank/batch coverage, metadata, compiled ROI/tile behavior, budgets, cancellation, and ownership.

## Backend and Registration Gates

The proposed native keys remain unregistered until implementation and runtime acceptance are complete. Native numerical validation follows [RES native floating-point contract](RES_native_contract.md); no NUM four-ULP fallback is required.

Float64 paths retain native Float64 inputs, arithmetic, outputs and exponent range; no Float32 conversion or NUM strict fallback is introduced.

## Conceptual DAG (Not an Existing API)

`static parameters + schemas → compile fixed reads/tasks → prepare → native arithmetic with declared algorithmic stages → complete tiles → ordered call retirement`

This diagram introduces no implicit color conversion, hidden alpha premultiplication, automatic spectral correction, or zero-filling of missing data. Whether execution is complete-input or requires multistage buffers is specified by this member and its family contract.

## References and Conformance Notes

[S18 · Buades, Coll, Morel: Non-Local Means Denoising](../research-sources.md#s18)

External sources provide algorithmic or definitional background. The finite window, rounding, tie-breaking, units, and execution profile are explicit contract choices; bitwise identity with any library or commercial product is not claimed.
