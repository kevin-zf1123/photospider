---
spec_schema_version: 1
id: FIL-14C
kind: authoring_helper
category: 05-filter
status: Proposed
document_maturity: D2_draft
implementation_status: implemented
parent_id: FIL-14
function: gabor_bank
proposed_operation_keys: []
numeric_reference: S
oracle_scope: composed_math_references
research_sources:
- S01
- S26
---

# FIL-14C: gabor_bank

Named Gabor filter bank.

Inherits [FIL-14](FIL-14_contract.md), the [FILTER common contract](FILTER_common_contract.md), [NUM](../../01-numeric/op_specs/NUM_common_contract.md) and [FMT](../../02-format-color/op_specs/FMT_common_contract.md).

## Interface

`input` plus a static list of configurations → FilterBands/v1 `responses`; each bank_id has real and imaginary roles with the input shape.

The public SDK helper is `ps::filter::gabor_bank` (declared in `photospider/ops/filter.hpp`). The metadata-bearing overload accepts an explicit `FilterGrid` and ordered `GaborConfiguration` entries and returns a `GaborBank`, a logical FilterBands/v1 directory with independent real and imaginary references. It records grid identity, original shape, axes, dtype, crop, sampling step, origin/spacing/unit, configuration, rounding and padding history. A reference-only overload returns ordered `GaborBand` references for composition. The helper rejects duplicate IDs and malformed structure, and binds the declared source shape/dtype to response construction. Repeated parameter sets under distinct IDs are allowed. There is no opaque bank arithmetic key.

### Parameters

Require at least one unique bank_id with explicit ordering. Each entry has FIL-14A parameters and FIL-14B boundaries; frequency is never selected automatically from data.

## Mathematics

Numerical class: **S**; see [FILTER_numeric_reference](FILTER_numeric_reference.md) for the E/B/S notation.

Compose FIL-14A→FIL-14B for each entry with S/DAG semantics. Bank outputs are not summed; normalization is recorded with each kernel.

## Behavior

The bank directory and output schemas are static. Only the selected banks and their required inputs contribute. Runtime payload associations are caller premises.

## Acceptance

A single bank matches FIL-14B. Duplicate parameters are allowed under different IDs; duplicate IDs are rejected. Changing order changes only collection order.

The RN_t stage boundaries of the composed members are preserved.

## References

[S01 · OpenCV Image Filtering](../research-sources.md#s01)；[S26 · scikit-image filters](../research-sources.md#s26)
