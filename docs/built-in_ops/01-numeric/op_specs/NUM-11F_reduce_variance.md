---
spec_schema_version: 1
id: NUM-11F
parent_id: NUM-11
function: reduce_variance
proposed_operation_keys:
  - numeric.reduce_variance_strict
  - numeric.reduce_variance_accelerated_apple_silicon
  - numeric.reduce_variance_accelerated_x86_64
category: 01-numeric
kind: primitive
status: Proposed
document_maturity: D1_draft
implementation_status: implemented_manual_acceptance
---

# NUM-11F: reduce_variance

Inherits the [NUM baseline](NUM_common_contract.md) and the [NUM-11 shared contract](NUM-11_reduction_contract.md). The strict key follows the exact reference; accelerated floating results follow the shared [final FP32 four-ULP contract](NUM_accelerated_contract.md).

## Interface

`numeric.reduce_variance_*` accepts UInt8, Int64, Float32 or Float64 input in a single-member Result. The `values` output is a `photospider.tensor` Result with member `samples`, Float32/Float64 dtype and rank-preserving keepdims shape.

| Parameter | Type and domain | Default |
| --- | --- | --- |
| `axes` | Canonical String of distinct axes | Required |
| `dtype` | String `float32` or `float64` | Helpers default to Float64; direct nodes supply it |
| `ddof` | Int64, `0 <= ddof < N` where N is the statically known group size | Helpers default to 0; direct nodes supply it |

`0 <= ddof < N` is an unchecked caller premise even though ddof is a static parameter.

## Mathematics

For finite inputs, let `S=sum(x)` and `Q=sum(x^2)` be exact mathematical quantities. Then

```text
V = (N*Q - S^2) / (N*(N-ddof))
```

and V is rounded once to the selected dtype. No rounded mean, square or partial sum defines the result. Integer values participate exactly. The exact numerator is nonnegative; clamping a negative approximate result to zero is not a substitute. A singleton finite group with `ddof=0` yields +0.

The first NaN in logical row-major order wins under the shared sign/payload conversion. With no NaN, any infinity produces the fixed positive quiet NaN, including all-equal infinities. A finite constant group, including mixtures of signed zeros, yields +0. A finite positive result may round to +Inf or +0 successfully. Nonfinite values produce the defined numerical result, not a failure.

## Behavior

Each output sample depends on every input sample of its group. Empty selection reads nothing.

## Acceptance

`input=[1,2,3]`, `axes="0"`, Float64: `ddof=0` gives `2/3` rounded once; `ddof=1` gives 1.
