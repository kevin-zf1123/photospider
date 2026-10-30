---
spec_schema_version: 1
id: GEN-01A
parent_id: GEN-01
function: constant_tensor
kind: primitive
category: 03-generation
status: Accepted
implementation_status: implemented
clarification_status: resolved
operation_keys:
  - generation.constant_tensor_v1_strict
  - generation.constant_tensor_v1_accelerated_apple_silicon
  - generation.constant_tensor_v1_accelerated_x86_64
---

# GEN-01A: constant tensor

Inherits [GEN-01](GEN-01_constant_contract.md) and [GEN common](GEN_common_contract.md).

## Interface

Operation keys: `generation.constant_tensor_v1_strict`, `generation.constant_tensor_v1_accelerated_apple_silicon`, `generation.constant_tensor_v1_accelerated_x86_64`.

value[C] with C>=1; values[H,W,C] of the same dtype, including C=1.

### Parameters and domain

canvas; UInt8/UInt16/Int8/Int16/Int64/Float32/Float64; no scaling.

## Mathematics

Copy value[c] bit-for-bit at every requested pixel, including raw NaN payloads, infinities and signed zero. No arithmetic or transfer conversion.

## Behavior

Support class: Regional.

## Acceptance

Signed-zero and NaN payload copies, nonzero origin, 1x1 and static rejection of mismatched component counts.
