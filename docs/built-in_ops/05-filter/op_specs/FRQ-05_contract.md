---
spec_schema_version: 1
id: FRQ-05
kind: family_contract
category: 05-filter
status: Proposed
document_maturity: D1
implementation_status: partially_implemented
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

Suffix letters identify distinct mathematical or interface objects, not quality tiers that a backend may select freely. A primitive may propose separately named strict and accelerated variants. A helper only orchestrates explicit stages. An external engine requires fixed resources and an approved executable configuration.

## Runtime integration and remaining acceptance

All three members are registered by `register_frequency` in
`plugins/ops/05-filter/frequency_operations.cpp`; exact/directed expressions
are in `frequency_local.cpp`, certified candidates in `frequency_fast.hpp`.
CPU strict and target-gated Apple Silicon/x86-64 keys exist; no GPU backend.
Only local Apple CPU execution has been measured. Direct output grids use the
[FRQ-01 explicit grid encoding](FRQ-01_contract.md#runtime-integration-and-remaining-acceptance),
with rank 2, y_axis=0, x_axis=1, full unshifted packing. Both output components
are independently selectable; imaginary-only execution has no response arithmetic.

For paired notches, the static center list is encoded by `centers_y_bits` and
`centers_x_bits`: equally sized nonempty canonical comma-separated unsigned
decimal uint64 IEEE-754 Float64 bit patterns. They preserve stored Float64
values exactly. Descriptor canonicalization sorts the pairs, without a
numerical distinctness/conjugacy-validation pass. `sigma_y/x` and `depth` retain
the member's Float64 numerical premises. Int64 `order` is present only for
Butterworth; `alpha` is not a response parameter.

Overall acceptance retains the frozen-ABI H08 root-work/refinement-stage
limitation documented by FRQ-01. Optional `max_work` is per invocation, not a
node-across-tiles or Run accumulator. Mathematical targets remain unchanged.

### Grid and Parameters
This family produces only real-valued, even, full and unshifted responses, with Float64 real values, +0 imaginary values, and a FrequencyGrid/v1 response descriptor. Require positive H/W and explicit spacing_y/x. Define signed index n(k)=k for k≤floor(N/2), otherwise k−N; positive and negative Nyquist indices are equivalent. Compute fy=n(ky)/(H*dy), fx=n(kx)/(W*dx), and exact frequency radius r=sqrt(fx²+fy²). Do not silently reinterpret pixel units when applying cutoffs. Require cutoff>0; values above the maximum frequency are allowed and are not clipped.

## Shared Execution and Numeric Requirements

E denotes an exact expression rounded once to RN_t; B denotes explicitly generated and fixed coefficients; S denotes explicit staged rounding. See [numeric reference](FILTER_numeric_reference.md). NUM defines NaN/Inf and floating-point overflow behavior; this family adds no category-wide finite-only rule. Strict bits, final FP32-scaled four-ULP bounds, gradual underflow, copy behavior, and exact discrete branches inherit NUM.

Positive-weight normalized filters, signed kernels, local regression, and global inverse problems have different alpha semantics and must not share one generic blur rule. See [color composition](FILTER_color_composition.md). Spatial extension, anchors, and origins follow the [boundary contract](FILTER_boundary_contract.md). Structured outputs follow the [collection contract](FILTER_collections_contract.md). Shapes depend only on descriptors; each member defines mathematical support and fixed compiled reads.

## Dependencies and Acceptance

Each member’s support set is normative. Having a halo does not permit independent execution on an arbitrary cropped ROI; paging does not eliminate a complete-input dependency. Staged rounding, global connectivity, transforms, and iterative state follow the member definition. Outputs are independently requestable. Only selected computation and required algorithmic control contribute payload reads; no payload-validation phase is introduced.

All members must meet the [acceptance protocol](FILTER_oracle_protocol.md). Mathematical references and their self-tests are documented in the [oracle README](../../../../oracle/ops/filter/README.md). D1/D2 describe specification maturity, not acceptance status.

## Sources and Review

[S09 · FFTW: DFT definition](../research-sources.md#s09); [S26 · scikit-image filters](../research-sources.md#s26)

Project boundary behavior, tie-breaking, numeric domains, parameters, public schemas, and proposed operation keys are defined by these specifications. Algorithm sources do not determine parameter defaults or grant rights to third-party software.
