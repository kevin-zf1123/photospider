---
spec_schema_version: 1
id: FRQ-09A
kind: primitive
category: 05-filter
status: Proposed
document_maturity: D1
implementation_status: implemented
parent_id: FRQ-09
function: dct2
proposed_operation_keys:
- frequency.dct2
numeric_reference: F
oracle_scope: mathematical_reference
research_sources:
- S13
---

# FRQ-09A: dct2

2D DCT.

Inherits the [FRQ-09 family contract](FRQ-09_contract.md), [FILTER common contract](FILTER_common_contract.md), [NUM numerical and accuracy contract](../../01-numeric/op_specs/NUM_common_contract.md) and [FMT metadata and straight-color semantics](../../02-format-color/op_specs/FMT_common_contract.md). The family's local F profile defines arithmetic accuracy; structural requirements remain applicable.

## Interface

Operation key: `frequency.dct2`.

### Ports

`input` → `output` with the same shape and dtype. The DCT coefficient-grid tag is associated with its type, norm, and axes; it is not ordinary RGB data.

### Parameters

Axes are explicit; type=I|II|III|IV; norm=backward|ortho. Both transformed dimensions for type I must be >=2; other types permit length 1. There is no ambiguous `orthogonalize` option.

NaN, infinities, signed zero, underflow and overflow follow the native arithmetic and floating-point environment; no finite-only scan or numerical-domain failure applies. All listed parameters are mandatory. Real parameters denote their stored Float64 values.

## Mathematics

Numeric reference: **F**, defined by the [FRQ-09 family contract](FRQ-09_contract.md).

Evaluate the family's selected 2D separable matrix with native floating-point arithmetic. Preserve the backward matrix definition or every ortho endpoint weight; ortho is not one universal scalar multiplier.

Arithmetic follows the F profile of the [FRQ-09 family contract](FRQ-09_contract.md#numeric-requirements).

## Behavior

Each output depends on the complete plane. Block DCT requires an explicit external tile collection; this node does not reset block boundaries automatically.

ROI and tile edges are not image boundaries; boundary coordinates use the complete logical image.

## Acceptance

Fixtures: for N=2 and backward normalization, type I maps [a,b] to [a+b,a-b]. Type II has only DC for a constant input. Verify all four matrices, II/III pairing, ortho Parseval and type-I rejection of N=1.

## References

[S13 · SciPy dct](../research-sources.md#s13)
