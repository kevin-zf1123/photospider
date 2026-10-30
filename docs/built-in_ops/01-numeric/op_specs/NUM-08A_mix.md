---
spec_schema_version: 1
id: NUM-08A
parent_id: NUM-08
function: mix
proposed_operation_keys:
  - numeric.mix_strict
  - numeric.mix_accelerated_apple_silicon
  - numeric.mix_accelerated_x86_64
category: 01-numeric
kind: primitive
status: Proposed
document_maturity: D1_draft
implementation_status: implemented_manual_acceptance
---

# NUM-08A: mix

The strict profile defines the exact reference result below. Accelerated profiles follow the shared [final FP32 four-ULP contract](NUM_accelerated_contract.md). Discrete selections and special-value handling are exact. Inherits the [NUM baseline](NUM_common_contract.md).

## Interface

Three dynamic Result inputs in order: `a`, `b` and `t`. Each Result contains one tensor member, whose key may be any valid member key. The three members have identical `sample_shape()` (including batch axes) and Float32 or Float64 dtype. Rank is 1..8, every extent is positive, and the sample count is at most 2^40.

The output port is `values`, a Result with schema `photospider.tensor` and tensor member key `samples`. Its dtype and complete shape match the input members. Every output axis is an ordinary axis; output batch axes are empty. There is no implicit broadcasting, cast, color-space conversion, alpha association or image compositing. A common scalar factor is connected through an explicit broadcast operation.

## Mathematics

For each logical coordinate,

$$
\operatorname{mix}(a,b,t)=(1-t)a+tb.
$$

The caller supplies finite factors satisfying `0 <= t <= 1`; negative zero is valid. This is an unchecked numerical precondition.

At `t = 0`, `a`'s bits are copied; at `t = 1`, `b`'s bits are copied. These selections preserve signaling-NaN and signed-zero encodings without floating-point evaluation. Both endpoints and `t` are dependencies at every coordinate, so an upstream failure on any declared input can prevent execution.

For `0 < t < 1`, both endpoint samples participate. If either endpoint is NaN, the first NaN in input order is returned, quieted with sign and payload preserved. If an endpoint is infinite, that infinity is returned when it is the only infinity or both infinities have the same sign; opposite-sign infinities produce the fixed positive quiet NaN. These cases produce numeric output successfully.

For finite endpoints and an interior factor, the exact real expression is rounded once directly to the output dtype. Strict results are bitwise reproducible. Accelerated floating results use the shared FP32-scaled error bound. Two negative-zero endpoints produce negative zero; any other exact zero interior result produces positive zero. A nonzero exact result that underflows to zero keeps its mathematical sign. Since `t` is in [0,1], finite endpoints cannot produce an exact result outside their range. Intermediate subtraction or products must not introduce spurious overflow.

## Acceptance

Compare strict results with independent exact arithmetic and accelerated errors with the stated bound. Cases: endpoint raw-bit copies, quieted NaNs, infinities of both signs, signed zeros, underflowing interior results.
