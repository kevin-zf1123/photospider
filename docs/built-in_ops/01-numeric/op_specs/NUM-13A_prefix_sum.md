---
spec_schema_version: 1
id: NUM-13A
parent_id: NUM-13
function: prefix_sum
proposed_operation_keys:
  - numeric.prefix_sum_strict
  - numeric.prefix_sum_accelerated_apple_silicon
  - numeric.prefix_sum_accelerated_x86_64
category: 01-numeric
kind: primitive
status: Proposed
document_maturity: D1_draft
implementation_status: implemented
---

# NUM-13A: prefix_sum

Exclusive-boundary prefix sums along one axis. Inherits the [NUM baseline](NUM_common_contract.md), overridden where unchecked numerical preconditions differ.

## Interface

The three `numeric.prefix_sum_<profile>` keys accept one Result tensor member under any member key. Output port `values` uses `photospider.tensor` with member `samples`. The output takes the complete input `sample_shape()`, including batch axes, as ordinary axes and extends the selected axis from N to N+1; other axes keep their extents. There is no inclusive/exclusive mode or image-role inference. Public helpers are in `photospider/ops/numeric/scans.hpp`.

| Parameter | Type and domain | Default |
| --- | --- | --- |
| `axis` | Int64, nonnegative index below rank | Required |
| `dtype` | String `uint8`, `int64`, `float32` or `float64`, in the same integer/floating domain as the input | Helper: Int64 for integer inputs, Float64 for floating inputs |

Input rank is 1..8, extents are positive and both input/output logical counts are at most 2^40. Shape addition, products, axes and dtype compatibility are checked before execution.

## Mathematics

Boundary k sums source positions [0,k); k=0 is zero and k=N includes the complete line. The arithmetic follows [reduce_sum](NUM-11A_reduce_sum.md): each boundary is the exact sum of its source interval followed by one destination conversion. Integer final representability is an unchecked caller precondition. Strict floating conversion is correctly rounded and bitwise reproducible. Accelerated profiles follow the [shared floating bound](NUM_accelerated_contract.md); discrete results and special-value classification are exact.

Empty mathematical prefixes produce integer zero or floating +0. That padding is not an arithmetic contribution: a nonempty prefix of only negative zeros produces -0. Source NaN priority follows logical axis order, with the sum contract's quieting and payload conversion. Mixed infinities follow the sum rules. A NaN generated at an earlier boundary never becomes a source element; `[+Inf,-Inf,NaN_payload]` produces `[+0,+Inf,canonical_NaN,quiet_payload_NaN]`.

Each output boundary is converted independently; the exact sum may exceed the destination range. Floating overflow produces infinity without contaminating later boundaries. For Int64 `[INT64_MAX,1,-1]`, boundary 3 is `INT64_MAX`, while boundary 2 violates integer representability.

## Behavior

Boundary k depends on the source line positions [0,k) at the same non-scan coordinates. Boundary zero has no source dependency. Empty selection reads nothing.

## Acceptance

`[1,2,3]` gives `[0,1,3,6]`. Cases: batch and nonadjacent axes, same-domain dtypes, special values, boundary-zero-only requests, an upstream failure in the tail of a line that does not affect an earlier prefix when only that prefix is requested. The reference is independent exact enumeration.
