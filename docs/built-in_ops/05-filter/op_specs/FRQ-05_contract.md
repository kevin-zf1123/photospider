---
spec_schema_version: 1
id: FRQ-05
kind: family_contract
category: 05-filter
status: Proposed
document_maturity: D1
implementation_status: implemented
parent_id: 05-filter
members:
- FRQ-05A
- FRQ-05B
- FRQ-05C
research_sources:
- S09
- S26
---

# FRQ-05: Frequency response

Frequencies are measured in cycles per unit. Low/high-pass, band-pass/stop, and paired-notch responses have separate definitions.

Inherits [FILTER shared contract](FILTER_common_contract.md), [NUM](../../01-numeric/op_specs/NUM_common_contract.md) and [FMT](../../02-format-color/op_specs/FMT_common_contract.md).

## Members

| ID | Specification | kind | Numeric reference |
| --- | --- | --- | --- |
| FRQ-05A | [Radial low-pass or high-pass response](FRQ-05A_radial_frequency_response.md) | primitive | B |
| FRQ-05B | [Radial band-pass or band-stop response](FRQ-05B_band_frequency_response.md) | primitive | B |
| FRQ-05C | [Paired periodic-frequency notch response](FRQ-05C_paired_notch_response.md) | primitive | B |

Suffix letters identify distinct mathematical or interface objects, not quality tiers.

## Interface

All three members have strict and accelerated CPU keys. Direct output grids use the [FRQ-01 explicit grid encoding](FRQ-01_contract.md#interface), with rank 2, y_axis=0, x_axis=1, full unshifted packing. Both output components are independently selectable; an imaginary-only request involves no response arithmetic.

For paired notches, the static center list is encoded by `centers_y_bits` and `centers_x_bits`: equally sized nonempty canonical comma-separated unsigned decimal uint64 IEEE-754 Float64 bit patterns, preserving stored Float64 values exactly. Descriptor canonicalization sorts the pairs, without a numerical distinctness or conjugacy validation. `sigma_y/x` and `depth` retain the member's Float64 numerical premises. Int64 `order` is present only for Butterworth; `alpha` is not a response parameter.

## Mathematics

### Grid and Parameters

This family produces only real-valued, even, full and unshifted responses, with Float64 real values, +0 imaginary values, and a FrequencyGrid/v1 response descriptor. Positive H/W and explicit spacing_y/x are required. The signed index is n(k)=k for k<=floor(N/2), otherwise k-N; positive and negative Nyquist indices are equivalent. Then fy=n(ky)/(H*dy), fx=n(kx)/(W*dx), and the exact frequency radius is r=sqrt(fx^2+fy^2). Pixel units are never silently reinterpreted when applying cutoffs. cutoff>0 is required; values above the maximum frequency are allowed and are not clipped.

## Numeric Requirements

E denotes an exact expression rounded once to RN_t; B denotes explicitly generated and fixed coefficients; S denotes explicit staged rounding. See [numeric reference](FILTER_numeric_reference.md). NUM defines NaN/Inf and floating-point overflow behavior; this family adds no category-wide finite-only rule. Strict bits, final FP32-scaled four-ULP bounds, gradual underflow, copy behavior, and exact discrete branches inherit NUM.

Alpha semantics, spatial extension and structured outputs follow [color composition](FILTER_color_composition.md), the [boundary contract](FILTER_boundary_contract.md) and the [collection contract](FILTER_collections_contract.md).

## Behavior and Acceptance

Each member's support set is normative. Outputs are independently requestable. Members are accepted against the [acceptance protocol](FILTER_oracle_protocol.md).

## References

[S09 · FFTW: DFT definition](../research-sources.md#s09); [S26 · scikit-image filters](../research-sources.md#s26)
