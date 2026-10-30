---
spec_schema_version: 1
id: FRQ-10A
kind: primitive
category: 05-filter
status: Proposed
document_maturity: D2
implementation_status: implemented
parent_id: FRQ-10
function: decompose_haar_mean_lifting_v1
proposed_operation_keys:
- frequency.decompose_haar_mean_lifting_v1
numeric_reference: F
oracle_scope: mathematical_reference
research_sources:
- S16
- S17
---

# FRQ-10A: decompose_haar_mean_lifting_v1

haar_mean_lifting_v1 decomposition.

Inherits [FRQ-10](FRQ-10_contract.md), [FILTER](FILTER_common_contract.md), [NUM](../../01-numeric/op_specs/NUM_common_contract.md) and [FMT](../../02-format-color/op_specs/FMT_common_contract.md). The family-local F profile defines arithmetic accuracy. Shape/support, boundaries and bit-preserving COPY remain normative.

## Interface

Operation key: `frequency.decompose_haar_mean_lifting_v1`.

### Ports

`input` → profile-specific Result `bands` for one 0..5-level direct node. The SDK FilterBands/v1 directory contains 3*levels+1 references across as many direct nodes as required, with each band's actual shape recorded.

### Parameters

Direct nodes require distinct y_axis/x_axis, levels and first_level. Levels range from 0..5. first_level is the zero-based global offset for stationary groups and zero for decimated profiles. Optional expected_shape and expected_dtype provide additional static consistency checks. The profile and boundaries are fixed by the member. SDK helpers accept every globally shape-valid level count and compose deeper directories from ordinary nodes; levels=0 is the identity residual.

The input descriptor determines Float32 or Float64 working arithmetic. Special values, underflow and overflow follow the family F profile; no finite-only payload scan or numerical-overflow failure is introduced. Source-grid identity, unit, spacing, origin and runtime association remain caller-owned interpretation.

## Mathematics

Numeric reference: **F**, defined locally in [FRQ-10](FRQ-10_contract.md). The directory records rounding_profile=native_float_v1.

Use the analysis and recursive level algorithm defined by **haar_mean_lifting_v1** in the family contract, using ordinary native arithmetic under the family F profile. At levels=0, preserve exact input bits.

The named lifting formulas, analysis_x_y_synthesis_y_x order, scales, boundaries and shape rules remain fixed. Correct rounding, per-component ULP bounds and cross-platform bit identity are not promised.

## Behavior

The band directory records shape, role, level, orientation, phase and reconstruction configuration without computing coefficients. Each required band retains its whole-plane mathematical dependency. Shapes and static configuration are checked statically; payload and association premises are unchecked. Edited coefficient Results may come from any conforming producer.

ROI and tile edges are not image boundaries; boundary coordinates use the complete logical image.

## Acceptance

Acceptance fixtures cover constant inputs, analytical 2x2 bases, odd 3x5 shapes, zero-level bit-preserving COPY and comparison with an independent lifting reference. Inverse fixtures include independently edited bands, particularly odd-boundary Haar detail and stationary coefficients that do not come from a forward transform. State dtype, input range, error norm and tolerance for nonzero-level comparisons. Exactly representable fixtures may additionally check exact results; general round trips are not a bit-identity contract.

## References

[S16 · PyWavelets signal extension modes](../research-sources.md#s16) ; [S17 · PyWavelets 2D DWT/IDWT](../research-sources.md#s17)
