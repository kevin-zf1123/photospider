---
spec_schema_version: 1
id: FIL-12B
kind: primitive
category: 05-filter
status: Proposed
document_maturity: D1_draft
implementation_status: implemented
parent_id: FIL-12
function: structure_tensor2d
proposed_operation_keys:
- filter.structure_tensor2d_strict
- filter.structure_tensor2d_accelerated_apple_silicon
- filter.structure_tensor2d_accelerated_x86_64
numeric_reference: E
oracle_scope: mathematical_reference
---

# FIL-12B: structure_tensor2d

Smoothed gradient outer-product tensor.

Inherits [FIL-12](FIL-12_contract.md), the [FILTER common contract](FILTER_common_contract.md), [NUM](../../01-numeric/op_specs/NUM_common_contract.md) and [FMT](../../02-format-color/op_specs/FMT_common_contract.md).

## Interface

Operation keys: `filter.structure_tensor2d_strict`, `filter.structure_tensor2d_accelerated_apple_silicon`, `filter.structure_tensor2d_accelerated_x86_64`.

`gx,gy` → independently requestable `jxx,jxy,jyy` outputs.

### Parameters

Explicit Int64 box height, width, and `anchor_y,anchor_x`, including for odd-sized boxes; fixed `reflect_half` boundary with `cval=0`. This operation does not compute derivatives; weights are the valid-box mean.

## Mathematics

`jxx=RN_t(mean(gx²))`, `jxy=RN_t(mean(gx*gy))`, and analogously for jyy. Do not first materialize rounded square/product tensors. Explicit NUM multiplication followed by box filtering is a different S chain.

## Behavior

`jxx` depends only on the `gx` footprint; `jyy` only on the `gy` footprint; `jxy` on both.

## Acceptance

For constant gx=2, gy=3, outputs are 4, 6, and 9. The exact matrix is PSD; do not silently clip tiny negative eigenvalues that appear after elementwise rounding.
