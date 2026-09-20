---
spec_schema_version: 1
id: NUM-09B
parent_id: NUM-09
function: transpose
proposed_operation_keys:
  - array.transpose_strict
  - array.transpose_accelerated_apple_silicon
  - array.transpose_accelerated_x86_64
category: 01-numeric
kind: primitive
status: Proposed
document_maturity: D1_draft
implementation_status: implemented_manual_acceptance
repository_branch: ops-impl
repository_commit: current working tree
---

# NUM-09B: transpose

Numeric profile: strict retains the exact reference defined below. Floating
arithmetic in accelerated profiles follows the shared
[final FP32 four-ULP contract](NUM_accelerated_contract.md), including its
range/fallback rules. Discrete results, copies, selected endpoints and special
values remain exact.

Inherit the [NUM baseline](NUM_common_contract.md) for specification status,
registration, shared execution and acceptance requirements; explicit rules below
and in the named family contract take precedence.

Permute logical axes of input `input`. Output `values` has the same dtype and
rank, with empty facets. Support UInt8/Int64/Float32/Float64, positive rank-1..8
shapes and logical element count <=2^40. Preserve all element bits including
signaling NaNs, infinities and signed zeros. No image/color/axis semantic metadata
is inferred; actual-read typed input validation remains required.

Required static String `permutation` is a canonical comma-separated list of
nonnegative decimal input-axis indices. Its length is input rank and each input
axis occurs exactly once. Output axis j corresponds to input axis permutation[j].
There are no negative indices, repeated/missing axes, implicit defaults or
rank changes. Required static String `layout` is auto/view/dense, constructor
default auto, with the complete-output policy from
[reshape](NUM-09A_reshape.md). Direct nodes specify both parameters.

## Shape, mapping and view

For input shape I and permutation p, output shape O[j]=I[p[j]]. At output
coordinate o, read s with s[p[j]]=o[j]. This exact bijection maps each output
rectangle to its axis-permuted input rectangle numerically. The formal Whole
execution reads and validates the complete input for any nonempty request,
invalidates the complete output on any input edit, and publishes the complete
output before projection. Empty reads nothing. Source/typed/domain failures are
Run-wide. A View permutes strides of one complete affine input owner; compatible
same-owner fragments may join. Multiple owners fail explicit View and can be
collected by Auto/Dense. Dense owns all output elements. Negative/zero strides
remain valid and raw bits are unchanged.

Compile shape inference must parse and validate the node's permutation before
execution. A registry-wide fixed permutation does not implement this parameterized
operation. Dtype inference uses input; output descriptor remains independent of
runtime layout choices. All three platform profiles preserve identical values
and obey explicit layout rules.

## Resources, errors and acceptance

Inherit reshape's owner retention, source-set/typed-validation support, layout
errors, host budget, cache, cancellation and output publication contracts. Mapping
cost is O(N*r) for complete dense N-element copies, or region/owner metadata work for views;
a permutation introduces no arithmetic scratch. Account actual retained source
owners even when no new output payload is allocated. Shape/parameter violations
fail compile/preflight; unavailable explicit views fail with diagnostic
ViewUnavailable. Source, validation, budget and cancellation failures cannot be
hidden by auto layout selection.

Conceptual fixture: input shape [2,3,4] with value 100*i+10*j+k and
permutation [2,0,1] yields shape [4,2,3], with values[k,i,j]=100*i+10*j+k.
Use an independent coordinate oracle and source-read logs. Cover identity and
all rank-2/rank-3 permutations, reverse/zero input strides, fragmented owners,
sNaN payloads, view/auto/dense choices, disjoint requests and inverse dirty sets.
The nine formal profile keys use CPU Whole; legacy unsuffixed keys remain
separate. See [NUM-09 Whole execution](../layouts-whole.md) for current public
workflow, validation and performance. Earlier regional validation predates Whole.
