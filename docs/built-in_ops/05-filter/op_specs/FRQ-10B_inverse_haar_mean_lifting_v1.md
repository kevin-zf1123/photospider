---
spec_schema_version: 1
id: FRQ-10B
kind: primitive
category: 05-filter
status: Proposed
document_maturity: D2
implementation_status: implemented
parent_id: FRQ-10
function: inverse_haar_mean_lifting_v1
proposed_operation_keys:
- frequency.inverse_haar_mean_lifting_v1
numeric_reference: F
oracle_scope: mathematical_reference
research_sources:
- S16
- S17
---

# FRQ-10B: inverse_haar_mean_lifting_v1

haar_mean_lifting_v1 reconstruction. Specification status: **Proposed**. Implementation: **implemented**.

Inherits [FRQ-10](FRQ-10_contract.md), [FILTER](FILTER_common_contract.md), [NUM](../../01-numeric/op_specs/NUM_common_contract.md) and [FMT](../../02-format-color/op_specs/FMT_common_contract.md). The family-local F profile defines arithmetic accuracy. Shape/support, boundaries, bit-preserving COPY, resources, cancellation and ownership remain normative.

## Ports, Shape, and Semantics

`bands` profile-specific Result → numerical `output` for one 0..5-level direct node. SDK inverse consumes the complete FilterBands/v1 directory, including independently edited references, and reconstructs the explicitly saved original shape.

Ports carry Results with fixed Tensor member schemas. Samples are ordinary numerical values, with no implicit RGB or alpha interpretation. Positive extents and axes are checked statically. Each materialized tile has independent allocation. FilterBands/v1 is static SDK/workflow metadata; helpers compose deeper transforms through the fixed per-Result member limit. Lowering fixes reads and writes for each selected output.

## Static Parameters and Valid Domain

The SDK uses explicit WaveletBands.options and validates the complete directory, including saved original shape, dtype, global levels, identities, roles, orientations, phase and profile. It does not infer odd original extents from coefficient shapes or producer identity. Direct nodes require y_axis, x_axis, levels, first_level, original_height and original_width. Levels range from 0..5. first_level is the zero-based global offset for stationary groups and zero for decimated profiles. Optional expected_shape and expected_dtype provide additional static consistency checks.

The input descriptor determines Float32 or Float64 working arithmetic. Special values, underflow and overflow follow the family F profile; no finite-only payload scan or numerical-overflow failure is introduced. Static execution parameters are stored in direct nodes. Source-grid identity, unit, spacing, origin and runtime association remain caller-owned interpretation. Copy and unconsumed inputs retain stated demand semantics.

## Mathematical Reference and Rounding

Numeric reference: **F**, defined locally in [FRQ-10](FRQ-10_contract.md). The directory records rounding_profile=native_float_v1.

Inverse uses the reverse-order synthesis and crop defined by **haar_mean_lifting_v1** in the family contract, using ordinary native arithmetic under the family F profile. At levels=0, preserve exact input bits. A materialized output uses explicit COPY into independent tile allocations; an authoring workflow may explicitly reuse an existing Result without materializing another value. Unrequested bands introduce no arithmetic payload reads.

The named lifting formulas, analysis_x_y_synthesis_y_x order, scales, boundaries and shape rules remain fixed. SIMD fusion and explicit FMA do not require separately certified rounding stages. Correct rounding, per-component ULP bounds and cross-platform bit identity are not promised. Internal details required by a low-band update or deeper LL level may be computed without exporting unrelated siblings.

## Fixed reads, algorithmic control and schemas

The static workflow/SDK band directory records shape, role, level, orientation, phase and reconstruction configuration without computing coefficients. Compilation selects the requested bands and required predecessor tasks; unrelated sibling outputs need not be materialized. Each required band retains its specified whole-plane mathematical dependency unless local computation is explicitly guaranteed, while physical outputs remain complete independent tiles. Shapes and static configuration are checked before execution; payload and association premises are unchecked. Edited coefficient Results may come from any conforming producer. Published Results do not acquire additional bands after retirement.

Boundary coordinates use the complete logical image; ROI and tile edges are not new image boundaries. Lowering closes mathematical sample support on fixed input member-tile grids. Data-dependent selection uses a finite static superset of every address the task may consume; runtime selection does not expand the Plan. Data and control are algorithmic inputs, with no separate Validation read set. Empty retained selections introduce no payload tasks. Output schemas derive only from static parameters and input schemas. Numerical, encoding and association premises remain unchecked under the common contract.

## Algorithm, Resources, and Execution

O(PC) as a geometric series. Account for intermediate planes and all published band owners in the budget.

P=HW and C is the number of independent planes. Charge intermediate storage, arithmetic work and every published band owner under shared admission and cancellation rules. A failed call publishes no successful Result export; previously retired exports remain valid and final RET results require successful Run completion. Working dtype is fixed; no silent narrowing, automatic spill or precision-refinement fallback is permitted.

## Oracle, Fixtures, and Acceptance

Acceptance fixtures cover constant inputs, analytical 2x2 bases, odd 3x5 shapes, zero-level bit-preserving COPY and comparison with an independent lifting reference. Inverse fixtures include independently edited bands, particularly odd-boundary Haar detail and stationary coefficients that do not come from a forward transform. State dtype, input range, error norm and tolerance for nonzero-level comparisons. Exactly representable fixtures may additionally check exact results; general round trips are not a bit-identity contract.

The preserved oracle is a mathematical reference for formulas, band orientation, boundaries and small-input fixtures. It is not a production kernel or complete schema/admission simulator. Its exact or staged evaluation does not impose strict golden bits or a fixed ULP bound on this F profile. Numerical differential checks and runtime-contract checks have separate evidence scopes.

`frq10.multiscale.wavelet(image, levels, profile, inverse, bands, dtype)` → [source](../../../../oracle/ops/filter/frq10/multiscale.py).

Fixture mapping and evidence scope: see [oracle coverage](../oracle-coverage.md).

Reproduction steps and proof levels are in [oracle README](../../../../oracle/ops/filter/README.md) These self-tests validate only the reference program. Parameter limits, full rank and batch behavior, metadata, compiled ROI/tile selection, budgets, cancellation, and owner lifetime require validation through [runtime acceptance protocol](FILTER_oracle_protocol.md) Passing identity or constant fixtures does not establish acceptance of the full algorithm or every configuration.

## Backend and Registration Status

`frequency.inverse_haar_mean_lifting_v1` is implemented and registered with native Float32/Float64 arithmetic and native_float_v1 metadata. Scalar, SIMD and threaded execution share this unsuffixed key. Float64 input, output and arithmetic remain Float64. Levels=0 preserves exact input bits through explicit COPY.

Implementation and registration are complete; this does not establish complete numerical, runtime or performance acceptance.

## Execution Outline

`static parameters + schemas → compile fixed reads/tasks → prepare → ordinary native floating-point arithmetic → complete tiles → ordered call retirement`

This diagram adds no implicit color conversion, hidden alpha premultiplication, automatic spectral correction, or zero-fill for missing data. Complete-input demand and multistage buffering are defined by this member and its family contract.

## Sources and Open Items

[S16 · PyWavelets signal extension modes](../research-sources.md#s16) ; [S17 · PyWavelets 2D DWT/IDWT](../research-sources.md#s17)

External sources support the algorithmic definition. The finite support, rounding, tie-breaking, units, parameters, and execution profile specified here are project choices and do not claim bitwise equivalence with any library or commercial product.

The three named profiles and six analysis/synthesis members are defined in this family contract. Compiled band selection follows the shared collection contract.
