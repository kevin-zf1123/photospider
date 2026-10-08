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

Inherit the [NUM baseline](NUM_common_contract.md) and [NUM-11 shared contract](NUM-11_reduction_contract.md). The strict key follows the exact reference; accelerated floating results follow the shared [final FP32 four-ULP contract](NUM_accelerated_contract.md), including range and fallback rules.

`numeric.reduce_variance_*` accepts UInt8, Int64, Float32 or Float64 input in a single-member Result. The `values` output is a `photospider.tensor` Result with member `samples`, Float32/Float64 dtype and rank-preserving keepdims shape. Required static `dtype` selects the floating output and defaults to Float64 in authoring helpers. Required static Int64 `ddof` is nonnegative, defaults to zero in helpers, and must be less than N, the statically known reduction-group size. Direct nodes supply both parameters.

For finite inputs, let `S=sum(x)` and `Q=sum(x^2)` be exact mathematical quantities. Compute

```text
V = (N*Q - S^2) / (N*(N-ddof))
```

and round V once to the selected dtype. No rounded mean, square or partial sum defines the result. Integer values participate exactly. The exact numerator is nonnegative; clamping a negative approximate result to zero is not a substitute for the contract. A singleton finite group with `ddof=0` yields +0.

The first NaN in logical row-major order wins under shared sign/payload conversion. With no NaN, any infinity produces the fixed positive quiet NaN, including all-equal infinities. A finite constant group, including mixtures of signed zeros, yields +0. A finite positive result may round to +Inf or +0 successfully.

Invalid `ddof` (`ddof < 0` or `N <= ddof`) fails static specialization with `InvalidArgument` / `InvalidDomain` before sample reads. Nonfinite numeric values produce the defined numerical result, not a Status failure. For `input=[1,2,3]`, `axes="0"`, Float64 `ddof=0` gives `2/3` rounded once; `ddof=1` gives 1. Exact moments and scratch are budgeted. Nonempty Whole demand reads and validates the complete input and publishes all groups before projection; Empty reads no payload. Current evidence is in [NUM-11 Whole execution](../reductions-whole.md).
