---
spec_schema_version: 1
id: FRQ-04
kind: family_contract
category: 05-filter
status: Proposed
document_maturity: D1
implementation_status: partially_implemented
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

Suffix letters identify distinct mathematical or interface objects, not quality tiers that a backend may select freely. A primitive may propose separately named strict and accelerated variants. A helper only orchestrates explicit stages. An external engine requires fixed resources and an approved executable configuration.

## Runtime integration and remaining acceptance

All three members are registered by `register_frequency` in
`plugins/ops/05-filter/frequency_operations.cpp`, with arithmetic in
`frequency_local.cpp` and certified candidates in `frequency_fast.hpp`.
CPU strict and target-gated Apple Silicon/x86-64 keys exist; no GPU backend.
Only local Apple CPU execution has been measured.

`window2d` takes the required member parameters directly. `apply_window` takes
Int64 `y_axis`/`x_axis` and a required canonical sorted `broadcast_axes` String;
an empty String denotes no broadcasting. `window_statistics` takes no required
static parameter. It accepts independent selection of its three scalar ports.
Optional Int64 `max_work` limits each invocation independently.

Overall acceptance retains the frozen-ABI H08 root-work/refinement-stage
limitation documented by [FRQ-01](FRQ-01_contract.md#runtime-integration-and-remaining-acceptance).
Capacity/ownership and mathematical targets are unchanged.

## Shared Mathematics, Units, and Profiles

For a one-dimensional window of length N=1, return [1]. Symmetric sampling uses denominator D=N−1; periodic sampling uses D=N and samples n=0..N−1. Do not define periodic sampling by generating N+1 points and discarding a rounded point. For a 2D window, first generate baked64 wx and wy values, then compute `window2d` as RN64(wx*wy). This is the specified rounding point for the public 2D window.

## Shared Execution and Numeric Requirements

E denotes an exact expression rounded once to RN_t; B denotes explicitly generated and fixed coefficients; S denotes explicit staged rounding. See [numeric reference](FILTER_numeric_reference.md). NUM defines NaN/Inf and floating-point overflow behavior; this family adds no category-wide finite-only rule. Strict bits, final FP32-scaled four-ULP bounds, gradual underflow, copy behavior, and exact discrete branches inherit NUM.

Positive-weight normalized filters, signed kernels, local regression, and global inverse problems have different alpha semantics and must not share one generic blur rule. See [color composition](FILTER_color_composition.md). Spatial extension, anchors, and origins follow the [boundary contract](FILTER_boundary_contract.md). Structured outputs follow the [collection contract](FILTER_collections_contract.md). Shapes depend only on descriptors; each member defines mathematical support and fixed compiled reads.

## Dependencies and Acceptance

Each member’s support set is normative. Having a halo does not permit independent execution on an arbitrary cropped ROI; paging does not eliminate a complete-input dependency. Staged rounding, global connectivity, transforms, and iterative state follow the member definition. Outputs are independently requestable. Only selected computation and required algorithmic control contribute payload reads; no payload-validation phase is introduced.

All members must meet the [acceptance protocol](FILTER_oracle_protocol.md). Mathematical references and their self-tests are documented in the [oracle README](../../../../oracle/ops/filter/README.md). D1/D2 describe specification maturity, not acceptance status.

## Sources and Review

[S14 · SciPy Hann window](../research-sources.md#s14); [S15 · SciPy Tukey window](../research-sources.md#s15)

Project boundary behavior, tie-breaking, numeric domains, parameters, public schemas, and proposed operation keys are defined by these specifications. Algorithm sources do not determine parameter defaults or grant rights to third-party software.
