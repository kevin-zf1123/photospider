---
spec_schema_version: 1
id: FRQ-08
kind: family_contract
category: 05-filter
status: Proposed
document_maturity: D1
implementation_status: implemented
parent_id: 05-filter
members:
- FRQ-08A
- FRQ-08B
research_sources:
- S12
---

# FRQ-08: Blocked Convolution

OLA and OLS are different execution configurations for the same fixed linear convolution, not block-boundary filters.

Inherits [FILTER common contract](FILTER_common_contract.md), [NUM](../../01-numeric/op_specs/NUM_common_contract.md) and [FMT](../../02-format-color/op_specs/FMT_common_contract.md).

## Members

| ID | member specification | kind | numeric reference |
| --- | --- | --- | --- |
| FRQ-08A | [Overlap-add convolution](FRQ-08A_overlap_add.md) | primitive | F |
| FRQ-08B | [Overlap-save convolution](FRQ-08B_overlap_save.md) | primitive | F |

Suffix letters identify distinct mathematical or interface objects, not quality tiers.

## Interface

Each member has one unsuffixed key under the F profile. The kernel is dynamic: every invocation consumes and normalizes its supplied coefficients. Static shape, anchor, normalization, padding and block constraints are mandatory.

## Mathematics

The block grid is fixed and global. Overlap-add accumulates every contributing block in the working dtype; overlap-save discards contaminated edges and selects outputs relative to the global convolution origin. Block sizes, padding and work order may affect rounding but not the mathematical result.

## Numeric Requirements

Numeric reference: **F**, ordinary native floating-point computation consistent with FFT/IFFT. Float32 input and output normally use binary32 working arithmetic. Float64 input, output and working arithmetic remain binary64 and are never silently narrowed to Float32. Kernel coefficients and none|sum|l1 normalization use binary64 before conversion to the working dtype. The Float32-input path may use binary64 working arithmetic when needed to retain the Float64 kernel's range, then convert the final output to Float32. Explicit FMA is permitted.

NaN, infinities, signed zero, underflow and overflow follow the selected native execution path and established floating-point environment. No finite-only payload scan or numerical-domain operation failure is introduced. Correct rounding, a fixed ULP bound and bitwise reproducibility across algorithms, block sizes, padding or platforms are not promised. There is no exact-result certification or precision refinement.

This local F profile takes precedence over inherited E/S-strict accuracy provisions of NUM/FILTER. The mathematical convolution, axes, normalization, anchor, output origin, shapes, boundary indexing, ROI/global-block rules, exact discrete choices and metadata remain unchanged.

Alpha semantics, spatial extension and structured outputs follow [color composition](FILTER_color_composition.md), the [boundary contract](FILTER_boundary_contract.md) and the [collection contract](FILTER_collections_contract.md).

## Behavior and Acceptance

The member support set is normative. Having a halo does not mean that an arbitrarily cropped ROI can run independently. Numerical comparisons use the family F profile with stated absolute/relative error criteria, rather than strict bit patterns or fixed ULP limits, under the [acceptance protocol](FILTER_oracle_protocol.md).

## References

[S12 · SciPy oaconvolve](../research-sources.md#s12)
