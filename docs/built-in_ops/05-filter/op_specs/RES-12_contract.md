---
spec_schema_version: 1
id: RES-12
kind: family_contract
category: 05-filter
status: Proposed
document_maturity: D1_draft
implementation_status: not_implemented
parent_id: 05-filter
members:
- RES-12A
- RES-12B
---

# RES-12: Moiré and screen-pattern suppression

Explicit notch editing and low-pass screen-pattern suppression are separate workflows; automatic recognition of unknown patterns is not delegated to the implementation.

It inherits the [FILTER common contract](FILTER_common_contract.md), [RES native floating-point contract](RES_native_contract.md), and [FMT](../../02-format-color/op_specs/FMT_common_contract.md).

## Members

| ID | Function spec | Composition type | Numeric reference |
| --- | --- | --- | --- |
| RES-12A | [Explicit paired-notch periodic-interference suppression](RES-12A_paired_notch_denoise.md) | authoring_helper | S |
| RES-12B | [Gaussian low-pass filter (workflow helper)](RES-12B_gaussian_lowpass.md) | authoring_helper | B+E |

Letter suffixes identify distinct mathematical or interface objects, not quality tiers. Primitives use native fp32/fp64 arithmetic.

## Numeric Requirements

All members use native class F under [RES_native_contract](RES_native_contract.md). Shape, support, copies and declared algorithmic stages remain normative. Numerical/model premises are caller obligations, without a production validation scan or NUM four-ULP fallback.

Alpha semantics, spatial extension and structured outputs follow [color composition](FILTER_color_composition.md), the [boundary contract](FILTER_boundary_contract.md) and the [collection contract](FILTER_collections_contract.md).

## Behavior and Acceptance

Each member's support set is normative. A halo does not imply independent execution on an arbitrary cropped ROI. Staged rounding, global connectivity, transforms, and iterative state follow each member's definition. Outputs are independently requestable. Members are accepted against the [acceptance protocol](FILTER_oracle_protocol.md). Effectiveness against all periodic interference and lossless retention of suppressed texture are not implied.

## References

[S09 · FFTW: DFT definition](../research-sources.md#s09); [S26 · scikit-image filters](../research-sources.md#s26)
