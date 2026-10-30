---
spec_schema_version: 1
id: FIL-14A
kind: primitive
category: 05-filter
status: Proposed
document_maturity: D1_draft
implementation_status: implemented
parent_id: FIL-14
function: gabor_kernel
proposed_operation_keys:
- filter.gabor_kernel_strict
- filter.gabor_kernel_accelerated_apple_silicon
- filter.gabor_kernel_accelerated_x86_64
numeric_reference: B
oracle_scope: mathematical_reference
research_sources:
- S01
- S26
---

# FIL-14A: gabor_kernel

Generate complex Gabor kernel.

Inherits [FIL-14](FIL-14_contract.md), the [FILTER common contract](FILTER_common_contract.md), [NUM](../../01-numeric/op_specs/NUM_common_contract.md) and [FMT](../../02-format-color/op_specs/FMT_common_contract.md).

## Interface

Operation keys: `filter.gabor_kernel_strict`, `filter.gabor_kernel_accelerated_apple_silicon`, `filter.gabor_kernel_accelerated_x86_64`.

No dynamic input → `kernel_real,kernel_imag` Float64 arrays of shape `[2ry+1,2rx+1]`.

### Parameters

frequency f∈[0,.5] cycles/pixel; theta and phase are finite radians; sigma>0; aspect γ>0; radius_y/x≥0; `dc=keep|remove`; `normalization=none|l1_envelope`.

## Mathematics

Numerical class: **B**; see [FILTER_numeric_reference](FILTER_numeric_reference.md) for the E/B/S notation.

X=x*cosθ+y*sinθ, Y=-x*sinθ+y*cosθ; E=exp(-(X²+γ²Y²)/(2σ²)); z=E*(cos(2πfX+phase)+i sin(...)). For keep, round each complete component expression once to RN64. For remove, subtract the exact continuous-valued discrete full-kernel mean, then round to RN64 (the stored sum is therefore not necessarily zero). For l1_envelope, divide by exact ΣE, then round to RN64. Apply DC removal before normalization.

## Behavior

There is no source data input. DC removal and normalization depend on all static taps.

## Acceptance

At f=0, phase=0, keep yields a real Gaussian envelope and zero imaginary part. The theta+π/phase relation yields conjugate symmetry. After remove, stored residual DC is not guaranteed to be zero.

## References

[S01 · OpenCV Image Filtering](../research-sources.md#s01)；[S26 · scikit-image filters](../research-sources.md#s26)
