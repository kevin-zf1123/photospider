---
spec_schema_version: 1
id: FRQ-02
kind: family_contract
category: 05-filter
status: Proposed
document_maturity: D1
implementation_status: implemented
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
| FRQ-02A | [Real-input half spectrum](FRQ-02A_rfft2.md) | primitive | F |
| FRQ-02B | [Hermitian half-spectrum to real field](FRQ-02B_irfft2.md) | primitive | F |
| FRQ-02C | [Explicit Hermitian-symmetry projection](FRQ-02C_project_hermitian.md) | primitive | E |

Suffix letters identify distinct mathematical or interface objects, not quality tiers that a backend may select freely. A primitive may propose separately named strict and accelerated variants. A helper only orchestrates explicit stages. An external engine requires fixed resources and an approved executable configuration.

## Runtime integration and remaining acceptance

`frequency.rfft2` and `frequency.irfft2` use the ordinary floating-point CPU
transform in `frequency_transform.cpp`. They inherit the F-profile arithmetic,
dimension-derived scratch, prepared plan cache and ABI9 resource accounting in
[FRQ-01](FRQ-01_contract.md#runtime-integration-and-remaining-acceptance).
`project_hermitian` remains an E-profile operation in `frequency_local.cpp`, with
its strict and target-gated accelerated keys and exact conjugate construction.
The SDK is `photospider/ops/frequency.hpp`.

The real transforms require `packing=r2c_x`, `index_order=unshifted`, and explicit
original spatial width. For even W, forward computation packs adjacent real
samples as even+i*odd, performs length-W/2 row FFTs, and recombines their paired
coefficients using length-W roots. It then transforms H-element columns of the
compact H*(W/2+1) plane. For odd W, forward computation uses a full complex plane
with zero imaginary input. Both paths emit columns 0 through floor(W/2).

DC and even-width Nyquist boundary columns use the smaller y index of each
conjugate pair as their common representative. The opposite imaginary value is
its negation, and self-conjugate imaginary intersections are structural +0.
No enclosure or numerical validation is needed for this packing.

For even W, inverse computation transforms the compact columns first, recombines
the half spectrum into doubled packed row coefficients, performs length-W/2
inverse row FFTs, and unpacks real and imaginary components into adjacent samples.
The factor of two preserves normalization by the original H*W. For odd W, it
loads the half spectrum once, completes missing columns in scratch using
`F[y,x]=conj(F[(-y)%H,W-x])`, and emits the real component of the full inverse FFT.
Both paths support independently edited interior coefficients under the caller's
boundary conjugacy premise. Neither path validates or repairs boundary pairs.
`backward` and `ortho` scaling always use the explicit original H and W.
Packing, recombination and data scaling use native Float32 for Float32 inputs
and Float64 for Float64 inputs. Compact pitches such as 513 and 2049 retain
their extra boundary column; column batches handle that remainder explicitly.

## Shared Mathematics, Units, and Profiles

Each member formula is defined in its linked specification. Members do not share undeclared implicit preprocessing. Parameters with the same name are reusable only when their units, boundary behavior, normalization, and RN stages match.


## FrequencyGrid/v1 logical schema

The proposed FrequencyGrid/v1 SDK/workflow representation pairs real and imaginary coefficient Results with explicit static transform configuration: original spatial shape, transformed axes, sampling spacing, coordinate origin/unit, normalization, index order and packing. Shapes and the directory are available before coefficient computation. Real and imaginary tensors have compatible schemas, while each materialized tile owns a distinct allocation. A selected transform coefficient plane has its specified whole-plane mathematical dependency unless locality is explicitly guaranteed. Edited coefficients may come from any conforming producer; numerical and payload association premises are caller obligations. Results carry no implicit frequency-grid metadata or numerical validity certificate.

## Shared Execution and Numeric Requirements

FRQ-02A/B use ordinary floating-point F semantics, including intermediate rounding and overflow as specified by FRQ-01. FRQ-02C retains E semantics: exact-expression output rounding, NUM exceptional-value rules and its named accelerated bounds. Resource, shape, fixed-read and publication obligations apply to all three members.

Positive-weight normalized filters, signed kernels, local regression, and global inverse problems have different alpha semantics and must not share one generic blur rule. See [color composition](FILTER_color_composition.md). Spatial extension, anchors, and origins follow the [boundary contract](FILTER_boundary_contract.md). Structured outputs follow the [collection contract](FILTER_collections_contract.md). Shapes depend only on descriptors; each member defines mathematical support and fixed compiled reads.

## Dependencies and Acceptance

Each member’s support set is normative. Having a halo does not permit independent execution on an arbitrary cropped ROI; paging does not eliminate a complete-input dependency. Staged rounding, global connectivity, transforms, and iterative state follow the member definition. Outputs are independently requestable. Only selected computation and required algorithmic control contribute payload reads; no payload-validation phase is introduced.

All members must meet the [acceptance protocol](FILTER_oracle_protocol.md). Mathematical references and their self-tests are documented in the [oracle README](../../../../oracle/ops/filter/README.md). D1/D2 describe specification maturity, not acceptance status.

## Sources and Review

[S10 · FFTW: real-data array format](../research-sources.md#s10)

Project boundary behavior, tie-breaking, numeric domains, parameters, public schemas, and proposed operation keys are defined by these specifications. Algorithm sources do not determine parameter defaults or grant rights to third-party software.
