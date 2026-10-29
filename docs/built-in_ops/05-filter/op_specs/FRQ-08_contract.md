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

Letter suffixes identify distinct mathematical or interface objects. Each member is registered under one unsuffixed native key with internal CPU strategy selection under the F profile. Helpers orchestrate explicit stages.

## Shared Mathematics, Units, and Profiles

Each member formula is defined in its linked specification. Members do not share undeclared implicit preprocessing. Parameters with the same name are reusable only when their units, boundary behavior, normalization and declared native numerical profile match.

## Shared Execution and Numeric Requirements

Numeric reference: **F**, ordinary native floating-point computation consistent with FFT/IFFT. Float32 input and output normally use binary32 working arithmetic. Float64 input, output and working arithmetic remain binary64 and are never silently narrowed to Float32. Kernel coefficients and none|sum|l1 normalization use binary64 before conversion to the working dtype. The Float32-input path may use binary64 working arithmetic when needed to retain the Float64 kernel's range, then convert the final output to Float32. Explicit FMA is permitted.

NaN, infinities, signed zero, underflow and overflow follow the selected native execution path and established floating-point environment. No finite-only payload scan or numerical-domain operation failure is introduced. Correct rounding, a fixed ULP bound and bitwise reproducibility across algorithms, block sizes, padding or platforms are not promised. There is no exact-result certification, precision refinement or strict numerical fallback.

This local F profile takes precedence over inherited E/S-strict accuracy provisions of NUM/FILTER. The mathematical convolution, axes, normalization, anchor, output origin, shapes, boundary indexing, ROI/global-block rules, exact discrete choices and metadata remain unchanged. Resource admission, cancellation, copy/bypass, publication and owner-lifetime requirements still apply.

Positive-weight normalized filters, signed kernels, local regression, and global inverse problems have different semantics; do not apply one alpha-blurring rule to all of them. See [color composition](FILTER_color_composition.md). Spatial extension, anchor, and origin follow the [boundary contract](FILTER_boundary_contract.md); structured outputs follow the [collection contract](FILTER_collections_contract.md). Shapes depend only on descriptors; each member defines mathematical support and fixed compiled reads.

### Use of ordinary FFT primitives

The current native implementation uses direct convolution when Kh<=15, Kw<=15 and Kh*Kw<=81; otherwise it uses the configured FFT strategy. Both strategies implement the same declared convolution under the F profile and the same unsuffixed operation key. Static shape, anchor, normalization, padding and block constraints remain mandatory, including for the direct strategy. The kernel is dynamic: every invocation consumes and normalizes its supplied coefficients and performs direct preparation or the kernel FFT. Shape-dependent plans and coefficient tables may be reused; kernel spectra are not reused across invocations.

The FFT path uses the fixed global block grid. Overlap-add accumulates every contributing block in the working dtype; overlap-save discards contaminated edges and selects outputs relative to the global convolution origin. Block sizes, padding and work order may affect rounding. Reserve block buffers, overlap accumulators, transform workspace, immutable plans and unpublished outputs.

Capacity, work and cancellation retain their existing failure semantics. Every internal worker must drain on success, failure or cancellation before invocation completion. Fewer executor lanes do not reduce declared per-invocation scratch.

## Dependencies and Acceptance

The member support set is normative. Having a halo does not mean that an arbitrarily cropped ROI can run independently; paged access does not remove a whole-image dependency. Follow each member's mathematical support, native numerical profile, transforms and execution dependencies. Only selected computation and required algorithmic control contribute payload reads; no payload-validation phase is introduced.

All members follow the structural, resource and lifecycle requirements of the [common acceptance protocol](FILTER_oracle_protocol.md). Numerical comparisons use the family F profile with stated absolute/relative error criteria, rather than strict bit patterns or fixed ULP limits. Preserve the mathematical references and self-tests in the [oracle usage guide](../../../../oracle/ops/filter/README.md). D1/D2 describe draft maturity, not acceptance rates.

Implementation and registration are complete; this does not establish complete numerical, runtime or performance acceptance.

## Sources and Review

[S12 · SciPy oaconvolve](../research-sources.md#s12)

Project boundary behavior, tie-breaking, numeric domains, parameters, public schemas, and registered operation keys are defined by these specifications. Algorithm sources do not determine parameter defaults or grant rights to third-party software.
