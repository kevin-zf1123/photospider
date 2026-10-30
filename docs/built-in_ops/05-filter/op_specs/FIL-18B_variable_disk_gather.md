---
spec_schema_version: 1
id: FIL-18B
kind: primitive
category: 05-filter
status: Proposed
document_maturity: D1_draft
implementation_status: implemented
parent_id: FIL-18
function: variable_disk_gather
proposed_operation_keys:
- filter.variable_disk_gather_strict
- filter.variable_disk_gather_accelerated_apple_silicon
- filter.variable_disk_gather_accelerated_x86_64
numeric_reference: E
oracle_scope: mathematical_reference
research_sources:
- S01
---

# FIL-18B: variable_disk_gather

Variable-radius disk mean.

Inherits [FIL-18](FIL-18_contract.md), the [FILTER common contract](FILTER_common_contract.md), [NUM](../../01-numeric/op_specs/NUM_common_contract.md) and [FMT](../../02-format-color/op_specs/FMT_common_contract.md).

## Interface

Operation keys: `filter.variable_disk_gather_strict`, `filter.variable_disk_gather_accelerated_apple_silicon`, `filter.variable_disk_gather_accelerated_x86_64`.

`input,radius` → `output,valid`; radius is a Float32/Float64 scalar `[1]` or H/W field, with explicit broadcast=scalar|spatial; batches match and radius is shared across components.

### Parameters

Required Int64 `y_axis` and `x_axis` address distinct spatial axes. Required Int64 `component_axis` is -1 for no component axis, or a distinct nonspatial axis. Required String `broadcast` is `scalar` or `spatial`; scalar radius has sample shape [1], while spatial radius matches the complete input sample shape with component extent 1. Radius dtype is independently Float32 or Float64. Required Float64 `max_radius` is finite and nonnegative; its floor and doubled support must fit static coordinate/capacity arithmetic. Required String `boundary` is `truncate|clamp|reflect_half|reflect_whole|wrap`. Radius payload lies in [0,max_radius] as an unchecked caller premise. There is no cval parameter.

## Mathematics

Numerical class: **E**; see [FILTER_numeric_reference](FILTER_numeric_reference.md) for the E/B/S notation.

At each q, read the exact stored floating-point value r. Candidate integer offsets dx/dy range over ±floor(r); retain those satisfying dx²+dy²≤r² using an exact comparison. Compute an equal-weight mean over valid taps with one exact RN_t. The center is always included; valid is always 1 under the caller premise 0≤r≤max_radius; r is not validated. At r=0, explicitly bit-copy the center.

## Behavior

`valid` has the input sample shape and UInt8 dtype. `valid` is always 1 and does not depend on payload. At static `max_radius=0`, the numerical output depends only on the source and copies its bits.

Radius is algorithmic control: its exact value selects the participating terms among those within static `max_radius` after boundary mapping. It does not change the output shape.

Disk row spans are determined with exact comparisons; floating-point sqrt rounding must not omit taps.

## Acceptance

r=0 bit-copies; at r=1 the disk has 5 taps and the square box 9. Near sqrt(2), the actual Float64 value determines whether the four corners are included. Truncation changes the valid count at edges. A radius above max_radius violates the caller premise.

## References

[S01 · OpenCV Image Filtering](../research-sources.md#s01)
