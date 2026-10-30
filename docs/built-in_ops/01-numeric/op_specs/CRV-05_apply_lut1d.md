---
spec_schema_version: 1
id: CRV-05
kind: shared_operator_contract
category: 01-numeric
status: Proposed
document_maturity: D1_draft
implementation_status: implemented
clarification_status: complete
---

# CRV-05: apply_lut1d family

Numeric profile: strict retains the exact reference defined below. Floating arithmetic in accelerated profiles follows the shared [final FP32 four-ULP contract](NUM_accelerated_contract.md). Discrete results, copies, selected endpoints and special values remain exact.

## Split

Two independent operations, both preserving input shape:

- Single-table application ([CRV-05A](CRV-05A_apply_lut1d.md)): table[L] maps every numeric element of input.
- Per-channel multi-table application ([CRV-05B](CRV-05B_apply_lut1d_channels.md)): table[L,C] column c maps input[...,c].

Mapping one scalar to a vector/color ramp is CRV-06 rather than this same-shape LUT family. Both operations receive explicit axis[3], use fixed linear interpolation and inherit their specified finite-value, exact-rounding and mathematical table-selection contracts. They use six explicit CPU-profile keys and public constructors.

## Interface

Each input Result has one tensor member under any schema id/version/key. Complete `sample_shape()` includes batch axes. Static preparation checks all three input schemas and parameters, including inputs whose payload is inactive. Output `values` uses `photospider.tensor` v1/member `samples`, preserves the complete input shape as ordinary axes and selects Float32 or Float64.

## Behavior

Each output element depends on the matching input element, all L table rows (the channel operation restricts table columns to the element's channel) and axis positions 0 and 1. Step is a consistency precondition and has no separate payload dependency. Region and tile results equal the corresponding slice of the whole result.

For L=1 the output is the sole table row (the selected column for channels). Input and axis payloads are not read. The singleton axis `[a,a,+0]`, finite input and reject's equality to a remain caller preconditions; this constant-output path does not diagnose violations.

Callers guarantee finite consumed queries/table entries, a valid complete axis, the selected reject domain and finite representable outputs. For L>1 this includes endpoint/step consistency and strict ordering of the entire endpoint-weighted RN64 grid. These numerical properties are unchecked. Generic table entries outside mathematical stencils have no additional finite requirement. Numerical precondition violations have no promised diagnostic or numerical result. Static schema/parameter errors use their static error categories. Descending axes are supported: lookup swaps both x and y before the positive-denominator evaluation. The endpoint-weighted coordinate formula and single final conversion are specified in [CRV-05A](CRV-05A_apply_lut1d.md).

## Acceptance

Expected values are exact rational evaluations for valid numerical inputs. Include both axis directions, each domain policy, singleton tables, mixed input/table/output dtypes, independent channel queries, signed zero and subnormal values. Connect all six CRV-04 templates to the consumer and check each producer's own numerical preconditions. Source samplers may legally produce repeated coordinates; callers must choose a grid satisfying this consumer's stricter axis precondition.

## References

- [Single-table specification](CRV-05A_apply_lut1d.md).
- [Per-channel table specification](CRV-05B_apply_lut1d_channels.md).
- [Baking templates](CRV-04_bake_lut1d.md).
- [Curve category](../curves.md).
- [Operator template](../../00-foundation/spec-template.md).
