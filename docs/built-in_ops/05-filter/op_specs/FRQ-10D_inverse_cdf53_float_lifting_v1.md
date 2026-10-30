---
spec_schema_version: 1
id: FRQ-10D
kind: primitive
category: 05-filter
status: Proposed
document_maturity: D2
implementation_status: implemented
parent_id: FRQ-10
function: inverse_cdf53_float_lifting_v1
proposed_operation_keys:
- frequency.inverse_cdf53_float_lifting_v1
numeric_reference: F
oracle_scope: mathematical_reference
research_sources:
- S16
- S17
---

# FRQ-10D: inverse_cdf53_float_lifting_v1

cdf53_float_lifting_v1 reconstruction.

Inherits [FRQ-10](FRQ-10_contract.md), [FILTER](FILTER_common_contract.md), [NUM](../../01-numeric/op_specs/NUM_common_contract.md) and [FMT](../../02-format-color/op_specs/FMT_common_contract.md). The family-local F profile defines arithmetic accuracy. Shape/support, boundaries and bit-preserving COPY remain normative.

## Interface

Operation key: `frequency.inverse_cdf53_float_lifting_v1`.

### Ports

`bands` profile-specific Result → numerical `output` for one 0..5-level direct node. SDK inverse consumes the complete FilterBands/v1 directory, including independently edited references, and reconstructs the explicitly saved original shape.

### Parameters

The SDK uses explicit WaveletBands.options and validates the complete directory, including saved original shape, dtype, global levels, identities, roles, orientations, phase and profile. It does not infer odd original extents from coefficient shapes or producer identity. Direct nodes require y_axis, x_axis, levels, first_level, original_height and original_width. Levels range from 0..5. first_level is the zero-based global offset for stationary groups and zero for decimated profiles. Optional expected_shape and expected_dtype provide additional static consistency checks.

The input descriptor determines Float32 or Float64 working arithmetic. Special values, underflow and overflow follow the family F profile; no finite-only payload scan or numerical-overflow failure is introduced. Source-grid identity, unit, spacing, origin and runtime association remain caller-owned interpretation.

## Mathematics

Numeric reference: **F**, defined locally in [FRQ-10](FRQ-10_contract.md). The directory records rounding_profile=native_float_v1.

Inverse uses the reverse-order synthesis and crop defined by **cdf53_float_lifting_v1** in the family contract, using ordinary native arithmetic under the family F profile. At levels=0, preserve exact input bits.

The named lifting formulas, analysis_x_y_synthesis_y_x order, scales, boundaries and shape rules remain fixed. Correct rounding, per-component ULP bounds and cross-platform bit identity are not promised.

## Behavior

The band directory records shape, role, level, orientation, phase and reconstruction configuration without computing coefficients. Each required band retains its whole-plane mathematical dependency. Shapes and static configuration are checked statically; payload and association premises are unchecked. Edited coefficient Results may come from any conforming producer.

ROI and tile edges are not image boundaries; boundary coordinates use the complete logical image.

## Acceptance

Acceptance fixtures cover constant inputs, analytical 2x2 bases, odd 3x5 shapes, zero-level bit-preserving COPY and comparison with an independent lifting reference. Inverse fixtures include independently edited bands, particularly odd-boundary Haar detail and stationary coefficients that do not come from a forward transform. State dtype, input range, error norm and tolerance for nonzero-level comparisons. Exactly representable fixtures may additionally check exact results; general round trips are not a bit-identity contract.

## References

[S16 · PyWavelets signal extension modes](../research-sources.md#s16) ; [S17 · PyWavelets 2D DWT/IDWT](../research-sources.md#s17)
