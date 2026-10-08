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

Permute logical axes of input `input`, a single-tensor Result with any schema
id/member key. Its complete `sample_shape()` includes batch axes and has positive
extents, rank 1..8 and logical element count <=2^40. Output `values` is a Result
tensor with the same dtype and rank. Supported dtypes are UInt8, Int8, UInt16,
Int16, Int64, Float32 and Float64. Preserve all element bits including
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
rectangle to its axis-permuted input rectangle numerically. The compiler
validates static shape, schema and permutation metadata before runtime. For
nonempty work, the Whole program requests the complete input with a Tensor Need
using Data, Validation and Descriptor roles (role 13), which triggers
typed-payload validation. It publishes a complete
`photospider.tensor`/`samples` Result
with ordinary axes and empty facets/batch metadata. Publication uses global
output coordinates. A query `Q` limits observed dependencies and downstream
reads rather than packing the output to an ROI. Empty output has empty coverage
and support. Source, typed and domain failures remain visible to the Run.

A View permutes strides of one affine input owner; compatible same-owner
fragments may join after address-map proof. Multiple owners make explicit View
unavailable. Auto materializes a complete packed output only for an unavailable
view; Dense always materializes it. Other failures do not trigger Auto
collection. Negative and zero strides remain valid and raw bits are unchanged.
Published views retain source storage and resources after context retirement.

Compile-time shape inference parses and validates the node's permutation.
A registry-wide fixed permutation does not implement this parameterized
operation. Dtype inference uses input; output schema remains independent of
runtime layout choices. All three CPU profiles preserve identical values and
obey explicit layout rules.

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
The nine formal profile keys use CPU Whole and disable cross-run content caching
because content alone does not prove physical owner/stride identity; same-Run
sharing remains available. See [NUM-09 Whole execution](../layouts-whole.md) for
the current Result workflow and validation.
