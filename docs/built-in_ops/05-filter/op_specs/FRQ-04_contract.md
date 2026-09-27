---
spec_schema_version: 1
id: FRQ-04
kind: family_contract
category: 05-filter
status: Proposed
document_maturity: D1
implementation_status: not_implemented
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

## Shared Mathematics, Units, and Profiles

For a one-dimensional window of length N=1, return [1]. Symmetric sampling uses denominator D=N−1; periodic sampling uses D=N and samples n=0..N−1. Do not define periodic sampling by generating N+1 points and discarding a rounded point. For a 2D window, first generate baked64 wx and wy values, then compute `window2d` as RN64(wx*wy). This is the specified rounding point for the public 2D window.

## Shared Execution and Numeric Requirements

E denotes an exact expression rounded once to RN_t; B denotes explicitly generated and fixed coefficients; S denotes explicit staged rounding. See [numeric reference](FILTER_numeric_reference.md). NUM defines NaN/Inf and floating-point overflow behavior; this family adds no category-wide finite-only rule. Strict bits, final FP32-scaled four-ULP bounds, gradual underflow, copy behavior, and exact discrete branches inherit NUM.

Positive-weight normalized filters, signed kernels, local regression, and global inverse problems have different alpha semantics and must not share one generic blur rule. See [color composition](FILTER_color_composition.md). Spatial extension, anchors, and origins follow the [boundary contract](FILTER_boundary_contract.md). Structured outputs follow the [collection contract](FILTER_collections_contract.md). Shapes depend only on descriptors; actual payload demand and dirty propagation are declared by each member.

## Dependencies and Acceptance

Each member’s support set is normative. Having a halo does not permit independent execution on an arbitrary cropped ROI; paging does not eliminate a Whole dependency. Staged rounding, global connectivity, transforms, and iterative state follow the member definition. Outputs are independently requestable; validating an unrequested component may not introduce a hidden whole-image dependency.

All members must meet the [acceptance protocol](FILTER_oracle_protocol.md). Mathematical references and their self-tests are documented in the [oracle README](../../../../oracle/ops/filter/README.md). D1/D2 describe specification maturity, not acceptance status.

## Sources and Review

[S14 · SciPy Hann window](../research-sources.md#s14); [S15 · SciPy Tukey window](../research-sources.md#s15)

Project boundary behavior, tie-breaking, numeric domains, parameters, public schemas, and proposed operation keys are defined by these specifications. Algorithm sources do not determine parameter defaults or grant rights to third-party software.
