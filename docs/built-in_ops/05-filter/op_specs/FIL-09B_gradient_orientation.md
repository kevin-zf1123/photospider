---
spec_schema_version: 1
id: FIL-09B
kind: primitive
category: 05-filter
status: Proposed
document_maturity: D1_draft
implementation_status: implemented
parent_id: FIL-09
function: gradient_orientation
proposed_operation_keys:
- filter.gradient_orientation_strict
- filter.gradient_orientation_accelerated_apple_silicon
- filter.gradient_orientation_accelerated_x86_64
numeric_reference: E
oracle_scope: mathematical_reference
---

# FIL-09B: gradient_orientation

Gradient orientation and validity.

Inherits [FIL-09](FIL-09_contract.md), the [FILTER common contract](FILTER_common_contract.md), [NUM](../../01-numeric/op_specs/NUM_common_contract.md) and [FMT](../../02-format-color/op_specs/FMT_common_contract.md).

## Interface

Operation keys: `filter.gradient_orientation_strict`, `filter.gradient_orientation_accelerated_apple_silicon`, `filter.gradient_orientation_accelerated_x86_64`.

`gx,gy` → `angle,valid`; `angle` has the input dtype and shape; `valid` is UInt8 with the same shape.

### Parameters

`unit` is explicitly supplied as `radian` or `turn`; output ranges are (-π,π] and (-1/2,1/2], respectively. No constructor default.

## Mathematics

For a nonzero vector, compute `atan2(gy,gx)`; map the negative x axis to +π. If both numeric inputs are zero, return angle +0 and valid=0; otherwise valid=1. In `turn` mode use `RN_t(angle_exact/(2π))`, not a rounded angle divided by a constant. Exceptional inputs and atan2 results follow NUM; a NaN-controlling validity comparison yields false validity and a propagated NaN angle.

## Behavior

Pointwise in `gx` and `gy`. `valid` depends on both values; no finite-only restriction is added.

## Acceptance

In input order `(gx,gy)=(0,1)` yields π/2; `(-1,±0)` yields +π; the zero vector is invalid. Preserve the distinction on either side of the negative x axis for tiny signed gy.
