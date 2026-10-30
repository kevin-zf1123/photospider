---
spec_schema_version: 1
id: NUM-14
function: matrix_transform
proposed_operation_keys:
  - numeric.matrix_transform_strict
  - numeric.matrix_transform_accelerated_apple_silicon
  - numeric.matrix_transform_accelerated_x86_64
category: 01-numeric
kind: primitive
status: Proposed
document_maturity: D1_draft
implementation_status: implemented
---

# NUM-14: matrix_transform

Numeric profile: strict is the exact reference defined below. Floating arithmetic in accelerated profiles follows the shared [final FP32 four-ULP contract](NUM_accelerated_contract.md). Discrete results, copies, selected endpoints and special values are exact. Inherits the [NUM baseline](NUM_common_contract.md); explicit rules below take precedence.

## Interface

Applies a shared affine transform y=M*x+b to a collection of vectors. The SDK helper is `matrix_transform_node`; the strict, Apple Silicon accelerated and x86_64 accelerated keys are `numeric.matrix_transform_{strict,accelerated_apple_silicon,accelerated_x86_64}`.

Inputs in port order are `vectors`, `matrix`, `bias`; each is a `Result` with one tensor member under any member key. Their dtypes match and are Float32 or Float64. The vectors tensor has `sample_shape()` [...,Cin], Cin in {2,3,4}; a rank-1 vector is allowed. The matrix has `sample_shape()` [Cout,Cin] and the bias [Cout], Cout in {2,3,4}. Matrix and bias are dynamic inputs shared by all vector instances. A zero bias uses an explicit constant. There are no implicit dtype casts, per-vector matrices, broadcasting or optional bias input. There are no static parameters; Cin/Cout are inferred from the validated input shapes.

The output port is `values`; its `photospider.tensor` Result schema contains tensor member `samples`, with shape [...,Cout], the same dtype, and no batch topology. Input/output vector ranks are 1..8 and logical counts at most 2^40. Dtype mismatch, unsupported Cin/Cout, bad bias/matrix shape and logical vector count above 2^40 are rejected at compile/preflight.

## Mathematics

Components follow y[r]=sum_c matrix[r,c]*vectors[c]+bias[r], defining matrix row/column orientation independently of storage strides. For finite operands, the complete dot product plus bias is evaluated as exact mathematical arithmetic and correctly rounded once to the output dtype; no separately rounded products or FMA chain define the contract. Strict defines the reference; accelerated follows the shared FP32-scaled bound. Singular matrices are valid. There is no inverse or implicit homogeneous-coordinate division.

Special values. All floating inputs may contain NaN/Inf. For an output row, source NaN priority is vectors[c] by increasing c, then the selected matrix row by increasing c, then bias[r]. The first source NaN is quieted with its sign and payload preserved. This priority precedes newly generated exceptional values and does not short-circuit declared input dependencies.

With no source NaN, any zero-times-infinity product yields the fixed positive quiet NaN. Otherwise each product is classified using operand sign XOR and exact finite multiplication, and bias is included. Opposite signed infinities among terms yield the canonical NaN; a sole infinity sign determines that infinity result. Finite products are never rounded individually, including overflow/underflow decisions. For a finite exact-zero total, -0 is returned only if every product term is signed negative zero and bias is -0; otherwise +0. A zero product's sign is the XOR of its input signs. Nonzero exact totals that underflow preserve their sign. Final floating overflow is the correctly signed infinity, a successful result. Fixed NaN patterns follow [NUM-04](NUM-04_unary_contract.md).

## Behavior

Output component r of a vector depends on that vector's Cin components, matrix row r (all Cin columns) and bias[r]. Zero coefficients keep all declared vector dependencies. A partial channel request reads only the selected matrix rows and bias channels. Empty selection reads nothing.

## Acceptance

Fixture: vectors=[2,3], matrix=[[1,2],[-1,0]], bias=[4,5] gives [12,3]. Independent exact rational dot products and source bit classification are the reference. Cases: rectangular 2->4 and 4->3 transforms, multiple batches, identity and singular matrices, cancellation after huge products, subnormals, all NaN priorities, 0*Inf, infinity cancellation, signed-zero terms, rank 1 and 8, all Cin/Cout pairs.
