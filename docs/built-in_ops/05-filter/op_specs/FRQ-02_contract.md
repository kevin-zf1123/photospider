---
spec_schema_version: 1
id: FRQ-02
kind: family_contract
category: 05-filter
status: Proposed
document_maturity: D1
implementation_status: not_implemented
parent_id: 05-filter
members:
- FRQ-02A
- FRQ-02B
- FRQ-02C
research_sources:
- S10
---

# FRQ-02: Real DFT and Hermitian Grid

One axis is compressed. The original width must be explicit, and conjugate pairs in boundary columns must be preserved.

Inherits [FILTER shared contract](FILTER_common_contract.md), [NUM](../../01-numeric/op_specs/NUM_common_contract.md) and [FMT](../../02-format-color/op_specs/FMT_common_contract.md).

## Members

| ID | Specification | kind | Numeric reference |
| --- | --- | --- | --- |
| FRQ-02A | [Real-input half spectrum](FRQ-02A_rfft2.md) | primitive | E |
| FRQ-02B | [Hermitian half-spectrum to real field](FRQ-02B_irfft2.md) | primitive | E |
| FRQ-02C | [Explicit Hermitian-symmetry projection](FRQ-02C_project_hermitian.md) | primitive | E |

Suffix letters identify distinct mathematical or interface objects, not quality tiers that a backend may select freely. A primitive may propose separately named strict and accelerated variants. A helper only orchestrates explicit stages. An external engine requires fixed resources and an approved executable configuration.

## Shared Mathematics, Units, and Profiles

Each member formula is defined in its linked specification. Members do not share undeclared implicit preprocessing. Parameters with the same name are reusable only when their units, boundary behavior, normalization, and RN stages match.


## FrequencyGrid/v1 logical schema

A FrequencyGrid/v1 value associates the real and imaginary coefficient tensors with the original spatial height and width, transformed axes, sampling spacing, coordinate origin and unit, normalization, index order (full/half and shifted/unshifted), and packing convention. Its descriptor is static and complete from the input descriptor and transform parameters; it is available without computing coefficient payloads. The real and imaginary tensors share shape, dtype, and owner lifetime. A requested coefficient plane is a full plane unless a member explicitly guarantees local Region computation. Editing coefficients creates a new immutable grid value with valid reconstruction metadata; consumers do not require the original producer identity.

## Shared Execution and Numeric Requirements

E denotes an exact expression rounded once to RN_t; B denotes explicitly generated and fixed coefficients; S denotes explicit staged rounding. See [numeric reference](FILTER_numeric_reference.md). NUM defines NaN/Inf and floating-point overflow behavior; this family adds no category-wide finite-only rule. Strict bits, final FP32-scaled four-ULP bounds, gradual underflow, copy behavior, and exact discrete branches inherit NUM.

Positive-weight normalized filters, signed kernels, local regression, and global inverse problems have different alpha semantics and must not share one generic blur rule. See [color composition](FILTER_color_composition.md). Spatial extension, anchors, and origins follow the [boundary contract](FILTER_boundary_contract.md). Structured outputs follow the [collection contract](FILTER_collections_contract.md). Shapes depend only on descriptors; actual payload demand and dirty propagation are declared by each member.

## Dependencies and Acceptance

Each member’s support set is normative. Having a halo does not permit independent execution on an arbitrary cropped ROI; paging does not eliminate a Whole dependency. Staged rounding, global connectivity, transforms, and iterative state follow the member definition. Outputs are independently requestable; validating an unrequested component may not introduce a hidden whole-image dependency.

All members must meet the [acceptance protocol](FILTER_oracle_protocol.md). Mathematical references and their self-tests are documented in the [oracle README](../../../../oracle/ops/filter/README.md). D1/D2 describe specification maturity, not acceptance status.

## Sources and Review

[S10 · FFTW: real-data array format](../research-sources.md#s10)

Project boundary behavior, tie-breaking, numeric domains, parameters, public schemas, and proposed operation keys are defined by these specifications. Algorithm sources do not determine parameter defaults or grant rights to third-party software.
