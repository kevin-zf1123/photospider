---
spec_schema_version: 1
id: FIL-16A
kind: primitive
category: 05-filter
status: Proposed
document_maturity: D1_draft
implementation_status: implemented
parent_id: FIL-16
function: unsharp_mask
proposed_operation_keys:
- filter.unsharp_mask_strict
- filter.unsharp_mask_accelerated_apple_silicon
- filter.unsharp_mask_accelerated_x86_64
numeric_reference: S
oracle_scope: mathematical_reference
research_sources:
- S01
---

# FIL-16A: unsharp_mask

Unsharp mask.

Inherits [FIL-16](FIL-16_contract.md), the [FILTER common contract](FILTER_common_contract.md), [NUM](../../01-numeric/op_specs/NUM_common_contract.md) and [FMT](../../02-format-color/op_specs/FMT_common_contract.md).

## Interface

Operation keys: `filter.unsharp_mask_strict`, `filter.unsharp_mask_accelerated_apple_silicon`, `filter.unsharp_mask_accelerated_x86_64`.

`input` → `output` same shape.

### Parameters

amount is finite; threshold≥0; Gaussian sigma/radius and axes are explicit; boundary follows FIL-04B.

## Mathematics

Numerical class: **S**; see [FILTER_numeric_reference](FILTER_numeric_reference.md) for the E/B/S notation.

At amount=0, preserve input bits; the Gaussian stage contributes no arithmetic. Otherwise B=FIL-04B(input), D=`RN_t(input-B)`; when abs(D)>threshold, produce `RN_t(input+amount*D)`, otherwise bit-copy input. Equality to the threshold does not sharpen.

## Behavior

At amount=0, each output depends only on input. Otherwise it depends on the input center and Gaussian footprint; threshold selection depends on D.

The unsharp-mask label does not map to arbitrary commercial presets.

## Acceptance

Constants remain unchanged. amount=0 preserves NaN and -0 without triggering blur. A residual exactly equal to threshold takes the copy branch. Negative amount is valid and reverses detail.

## References

[S01 · OpenCV Image Filtering](../research-sources.md#s01)
