---
spec_schema_version: 1
id: FIL-17A
kind: primitive
category: 05-filter
status: Proposed
document_maturity: D2_draft
implementation_status: implemented
parent_id: FIL-17
function: local_contrast_tanh
proposed_operation_keys:
- filter.local_contrast_tanh_strict
- filter.local_contrast_tanh_accelerated_apple_silicon
- filter.local_contrast_tanh_accelerated_x86_64
numeric_reference: E
oracle_scope: mathematical_reference
research_sources:
- S07
---

# FIL-17A: local_contrast_tanh

Local contrast with bounded detail.

Inherits [FIL-17](FIL-17_contract.md), the [FILTER common contract](FILTER_common_contract.md), [NUM](../../01-numeric/op_specs/NUM_common_contract.md) and [FMT](../../02-format-color/op_specs/FMT_common_contract.md).

## Interface

Operation keys: `filter.local_contrast_tanh_strict`, `filter.local_contrast_tanh_accelerated_apple_silicon`, `filter.local_contrast_tanh_accelerated_x86_64`.

`input,base` have the same dtype and complete sample shape → `output`; the caller explicitly creates base using FIL-04B or FIL-07.

### Parameters

Both parameters are required Float64 values: `amount` is finite; `detail_scale` is finite and strictly positive. There are no implicit defaults or intensity-range assumptions.

## Mathematics

Numerical class: **E**; see [FILTER_numeric_reference](FILTER_numeric_reference.md) for the E/B/S notation.

At amount=0, bit-copy input; otherwise D=input-base exactly and `output=RN_t(input+amount*s*tanh(D/s))`. s has the units of input. This is not a vendor Clarity reproduction.

## Behavior

Pointwise in input and base. At `amount=0` the output depends only on input. The base node itself may have a halo or global dependency, but this member does not redefine its algorithm.

As s approaches zero, the operation does not switch to hard clipping.

## Acceptance

When input=base, finite values remain unchanged; amount=0 bypasses base; test odd response to detail; HDR values are not clipped.

## References

[S07 · Paris, Hasinoff, Kautz: Local Laplacian Filters](../research-sources.md#s07)
