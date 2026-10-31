---
spec_schema_version: 1
id: CRV-05
kind: shared_operator_contract
category: 01-numeric
status: Proposed
document_maturity: D1_draft
implementation_status: partially_implemented
clarification_status: complete
---

# CRV-05: apply_lut1d family

Numeric profile: strict retains the exact reference defined below. Floating arithmetic in accelerated profiles follows the shared [final FP32 four-ULP contract](NUM_accelerated_contract.md). Discrete results, copies, selected endpoints and special values remain exact.

## Split

Three independent operations:

- Single-table application ([CRV-05A](CRV-05A_apply_lut1d.md)): table[L] maps every numeric element of input.
- Per-channel multi-table application ([CRV-05B](CRV-05B_apply_lut1d_channels.md)): table[L,C] column c maps input[...,c].
- Scalar-to-vector application ([CRV-05C](CRV-05C_apply_lut1d_vector.md)): table[L,C] maps each input scalar through every column and appends C to the output shape. Its proposed fast key is not implemented.

All three receive a precomputed uniform-axis table and explicit axis[3]. A/B preserve input shape and have eight implemented CPU-profile keys in total. C defines one additional native fast key. CRV-06 separately consumes color controls at explicit, potentially nonuniform stops and defines model-specific interpolation. Numerical profiles and output-shape exceptions are specified by each member.

## Interface

Each input Result has one tensor member under any schema id/version/key. Complete `sample_shape()` includes batch axes. Static preparation checks all three input schemas and parameters, including inputs whose payload is inactive. Output `values` uses `photospider.tensor` v1/member `samples`, uses the complete input shape as ordinary axes and selects Float32 or Float64. A/B preserve that shape; C appends the table component count.

## Behavior

Each output element depends on its query input element, all L table rows in its selected column and axis positions 0 and 1. A/B use matching input coordinates; C removes the appended output component coordinate to identify the shared input query. Step is a consistency precondition and has no separate payload dependency. Region and tile results equal the corresponding slice of the whole result.

For L=1 the output is the sole table row (the selected column for channels). Input and axis payloads are not read. The singleton axis `[a,a,+0]`, finite input and reject's equality to a remain caller preconditions; this constant-output path does not diagnose violations.

Callers guarantee finite consumed queries/table entries, a valid complete axis, the selected reject domain and finite representable outputs. For L>1 this includes endpoint/step consistency and strict ordering of the entire endpoint-weighted RN64 grid. These numerical properties are unchecked. Generic table entries outside mathematical stencils have no additional finite requirement. Numerical precondition violations have no promised diagnostic or numerical result. Static schema/parameter errors use their static error categories. Descending axes are supported: lookup swaps both x and y before the positive-denominator evaluation. The endpoint-weighted coordinate formula and single final conversion are specified in [CRV-05A](CRV-05A_apply_lut1d.md).

## Native fast numerical profile

The additional A/B keys `curve.apply_lut1d_fast` and `curve.apply_lut1d_channels_fast` are selected by `CpuNumericProfile::Fast`. The proposed C key `curve.apply_lut1d_vector_fast` shares this native arithmetic while mapping one query through all table columns; its implementation remains pending. The A/B shape, dtype, axis direction and table-column parameters are unchanged; C defines its appended output axis in its member specification. These keys use ordinary binary64 interpolation and final output-dtype conversion, without NUM exact-rational final rounding or accelerated certification.

For L>1, compute `u=(q-start)*(L-1)/(end-start)`, select `i=floor(u)` bounded to `[0,L-2]`, and evaluate `table[i] + (u-i)*(table[i+1]-table[i])` with FMA. Clamp and exact normalized endpoint positions select the corresponding table value; linear extrapolation uses the endpoint segment. The negative denominator naturally handles descending axes. Native normalized-axis arithmetic replaces the strict profile's exact reconstruction of every RN64 grid coordinate; ordinary rounding can differ at interior grid positions.

L=1 selects the single table entry and does not read query or axis payload.

## Acceptance

Expected values are exact rational evaluations for valid numerical inputs. Include both axis directions, each domain policy, singleton tables, mixed input/table/output dtypes, independent channel queries, signed zero and subnormal values. Connect all six CRV-04 templates to the consumer and check each producer's own numerical preconditions. Source samplers may legally produce repeated coordinates; callers must choose a grid satisfying this consumer's stricter axis precondition.

## References

- [Single-table specification](CRV-05A_apply_lut1d.md).
- [Per-channel table specification](CRV-05B_apply_lut1d_channels.md).
- [Scalar-to-vector table specification](CRV-05C_apply_lut1d_vector.md).
- [Baking templates](CRV-04_bake_lut1d.md).
- [Curve category](../curves.md).
- [Operator template](../../00-foundation/spec-template.md).
