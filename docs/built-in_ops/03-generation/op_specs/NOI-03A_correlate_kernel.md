---
spec_schema_version: 1
id: NOI-03A
parent_id: NOI-03
function: correlate_kernel
kind: primitive
category: 03-generation
status: Accepted
implementation_status: implemented
clarification_status: resolved
operation_keys:
  - noise.correlate_kernel_v1_strict
  - noise.correlate_kernel_v1_accelerated_apple_silicon
  - noise.correlate_kernel_v1_accelerated_x86_64
---

# NOI-03A: correlate kernel

Inherits [NOI-03](NOI-03_correlated_contract.md) and [GEN common](GEN_common_contract.md).

## Interface

Operation keys: `noise.correlate_kernel_v1_strict`, `noise.correlate_kernel_v1_accelerated_apple_silicon`, `noise.correlate_kernel_v1_accelerated_x86_64`.

source[H,W,C], kernel[Kh,Kw], finite Float32/64; same-shape values.

### Parameters and domain

Positive odd kernel extents, default each <=255; boundary=zero/clamp/wrap/reflect101 required; normalization=none/sum/l2, default none; output dtype explicit.

## Mathematics

Correlation, not flipped convolution: RN_T(sum w[dy,dx]*sample(y+dy-rh,x+dx-rw)/den). den=1,sum(w),sqrt(sum(w^2)) respectively; nontrivial denominator must be nonzero. reflect101 period=2(N-1), N=1 maps to zero index. Repeated boundary indices are the same variable.

## Behavior

Support class: Halo. Output sample `(y,x)` depends on the source within the kernel radius after boundary mapping. Valid kernel coefficients and nonzero normalization denominators are caller premises.

## Acceptance

Asymmetric kernel direction, 1x1 identity, singleton reflect, sum-normalized constant field and merged boundary weights.
