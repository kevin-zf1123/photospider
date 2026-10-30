---
spec_schema_version: 1
id: FIL-12C
kind: primitive
category: 05-filter
status: Proposed
document_maturity: D1_draft
implementation_status: implemented
parent_id: FIL-12
function: symmetric_eigen2d
proposed_operation_keys:
- filter.symmetric_eigen2d_strict
- filter.symmetric_eigen2d_accelerated_apple_silicon
- filter.symmetric_eigen2d_accelerated_x86_64
numeric_reference: E
oracle_scope: mathematical_reference
---

# FIL-12C: symmetric_eigen2d

Eigenvalues and principal orientation of a 2D symmetric matrix.

Inherits [FIL-12](FIL-12_contract.md), the [FILTER common contract](FILTER_common_contract.md), [NUM](../../01-numeric/op_specs/NUM_common_contract.md) and [FMT](../../02-format-color/op_specs/FMT_common_contract.md).

## Interface

Operation keys: `filter.symmetric_eigen2d_strict`, `filter.symmetric_eigen2d_accelerated_apple_silicon`, `filter.symmetric_eigen2d_accelerated_x86_64`.

`a,b,c` with identical shapes → `lambda_max,lambda_min,angle,orientation_valid` with corresponding shapes.

### Parameters

Orientation is fixed in radians. No positive-semidefinite assumption is made.

## Mathematics

For matrix `[[a,b],[b,c]]`, let `d=sqrt((a-c)²+4b²)` and `λ±=RN_t((a+c±d)/2)`. The orientation is `0.5*atan2(2b,a-c)`, normalized to [0,π). When a=c and b=0, return angle 0 and valid=0; otherwise valid=1. Evaluate each expression exactly before its specified rounding; do not derive the eigenvector from rounded eigenvalues.

## Behavior

Pointwise operation. Each eigenvalue output still depends on a, b, and c. A validity-only request may classify the isotropic case by testing a=c and b=0.

## Acceptance

`(4,0,1)` yields eigenvalues 4 and 1 with angle 0. Isotropic `3I` has no valid orientation. The indefinite matrix `[[0,1],[1,0]]` yields 1, -1, and angle π/4.
