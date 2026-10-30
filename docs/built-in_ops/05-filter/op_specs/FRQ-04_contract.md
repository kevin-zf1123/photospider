---
spec_schema_version: 1
id: FRQ-04
kind: family_contract
category: 05-filter
status: Proposed
document_maturity: D1
implementation_status: implemented
parent_id: 05-filter
members:
- FRQ-04A
- FRQ-04B
- FRQ-04C
research_sources:
- S14
- S15
---

# FRQ-04: Windows and scaling statistics

Window generation, window application, and window statistics are separate operations. Window normalization is not FFT normalization.

Inherits [FILTER shared contract](FILTER_common_contract.md), [NUM](../../01-numeric/op_specs/NUM_common_contract.md) and [FMT](../../02-format-color/op_specs/FMT_common_contract.md).

## Members

| ID | Specification | kind | Numeric reference |
| --- | --- | --- | --- |
| FRQ-04A | [Two-dimensional Hann, Tukey, or rectangular window](FRQ-04A_window2d.md) | primitive | B+S |
| FRQ-04B | [Apply a window explicitly](FRQ-04B_apply_window.md) | primitive | E |
| FRQ-04C | [Window statistics and equivalent noise bandwidth](FRQ-04C_window_statistics.md) | primitive | E |

Suffix letters identify distinct mathematical or interface objects, not quality tiers.

## Interface

All three members have strict and accelerated CPU keys. `window2d` takes the required member parameters directly. `apply_window` takes Int64 `y_axis`/`x_axis` and a required canonical sorted `broadcast_axes` String; an empty String denotes no broadcasting. `window_statistics` takes no required static parameter and accepts independent selection of its three scalar ports. Optional Int64 `max_work` limits each invocation.

## Mathematics

For a one-dimensional window of length N=1, return [1]. Symmetric sampling uses denominator D=N-1; periodic sampling uses D=N and samples n=0..N-1. Periodic sampling is not defined by generating N+1 points and discarding a rounded point. For a 2D window, first generate baked64 wx and wy values, then compute `window2d` as RN64(wx*wy). This is the specified rounding point for the public 2D window.

## Numeric Requirements

E denotes an exact expression rounded once to RN_t; B denotes explicitly generated and fixed coefficients; S denotes explicit staged rounding. See [numeric reference](FILTER_numeric_reference.md). NUM defines NaN/Inf and floating-point overflow behavior; this family adds no category-wide finite-only rule. Strict bits, final FP32-scaled four-ULP bounds, gradual underflow, copy behavior, and exact discrete branches inherit NUM.

Alpha semantics, spatial extension and structured outputs follow [color composition](FILTER_color_composition.md), the [boundary contract](FILTER_boundary_contract.md) and the [collection contract](FILTER_collections_contract.md).

## Behavior and Acceptance

Each member's support set is normative. Outputs are independently requestable. Members are accepted against the [acceptance protocol](FILTER_oracle_protocol.md).

## References

[S14 · SciPy Hann window](../research-sources.md#s14); [S15 · SciPy Tukey window](../research-sources.md#s15)
