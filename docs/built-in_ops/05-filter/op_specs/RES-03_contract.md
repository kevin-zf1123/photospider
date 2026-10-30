---
spec_schema_version: 1
id: RES-03
kind: family_contract
category: 05-filter
status: Proposed
document_maturity: D1_draft
implementation_status: implemented
parent_id: 05-filter
members:
- RES-03A
- RES-03B
---

# RES-03: Wavelet Denoising

Coefficient thresholding and finite cycle-spinning workflow. Statistical assumptions for converting sigma to thresholds are not hidden in algorithm defaults.

It inherits the [FILTER common contract](FILTER_common_contract.md), [RES native floating-point contract](RES_native_contract.md), and [FMT](../../02-format-color/op_specs/FMT_common_contract.md).

## Members

| ID | Function Spec | Kind | Numeric Reference |
| --- | --- | --- | --- |
| RES-03A | [Wavelet detail thresholding](RES-03A_threshold_wavelet_details.md) | primitive | F |
| RES-03B | [Wavelet denoising with a fixed shift set](RES-03B_cycle_spin_wavelet_denoise.md) | authoring_helper | F |

Suffix letters denote distinct mathematical/interface objects, not quality tiers. Each primitive uses one native fp32/fp64 key; a helper orchestrates explicit algorithmic stages.

## Interface

The SDK header is `photospider/ops/restoration.hpp`.

- `ps::restoration::threshold_wavelet_details(document, bands, thresholds, mode)` returns an edited `frequency::WaveletBands` directory. It validates every static band descriptor and preserves the directory order, profile, axes, grid, units and final LL reference. Both decimated FRQ-10 profiles and `stationary_haar_mean_v1` are supported.
- `ps::restoration::cycle_spin_wavelet_denoise(document, input, thresholds, options, shifts, mode)` returns a `WorkflowInput`. It appends explicit permutation, analysis, threshold, synthesis and native accumulation nodes. At zero levels it returns the input reference and appends no nodes.

Both helpers update the document transactionally. Structural errors return `InvalidArgument` or `TypeMismatch`; allocation failure may throw. Edits to one document require caller serialization.

The primitive key is `restoration.threshold_wavelet_details`. Its first input is the FRQ-10 packed Result with schema `photospider.filter_bands.<profile>`, version 1; its second input is a single Float64 tensor of sample shape `[total_levels,3]`. Output port `bands` preserves the complete input schema. Packed directories contain one to five local levels, ordered `LH_y_1, HL_x_1, HH_1, ... , LL`. The SDK composes deeper directories and uses zero-based `first_level` offsets into the shared threshold table.

| Primitive parameter | Type | Meaning |
| --- | --- | --- |
| `mode` | String | Explicit `hard` or `soft` |
| `first_level` | Int64 | Nonnegative global threshold-row offset |
| `total_levels` | Int64 | Positive row count of the shared threshold table |
| `y_axis`, `x_axis` | Int64 | Distinct valid logical sample axes |
| `original_height`, `original_width` | Int64 | Positive dimensions entering this local decomposition |

All detail and residual shapes are checked against the profile and local decomposition, including unselected members. Threshold values remain unchecked caller premises and are converted to the coefficient dtype once per invocation.

The helper primitives `restoration.wrap_shift` and `restoration.accumulate_native` implement the explicit cycle-spin stages. The former takes `y_axis`, `x_axis`, signed Int64 `shift_y`, `shift_x`, and explicit String `expected_shape`, `expected_dtype`; it computes `S_s(input)[y,x] = input[(y+sy) mod H,(x+sx) mod W]` and preserves every bit. The latter takes two matching native tensors and a Float64 `divisor`, converted to their dtype, and computes a native sum followed by native division. Intermediate accumulation uses divisor one and the final accumulation uses the number of specified shifts. A single branch needs no accumulation node.

## Behavior

A threshold output sample depends only on that input coefficient and its one threshold-table entry. LL reads only LL and is a copy.

## Numeric Requirements

All members use numeric class F under [RES native floating-point contract](RES_native_contract.md). The input dtype selects native fp32 or fp64 arithmetic and stored state. Caller domain premises remain unchecked; shape, support, copy branches and algorithmic stages remain normative. There is no strict/accelerated quality split or NUM four-ULP fallback.

Alpha semantics, spatial extension and structured outputs follow [color composition](FILTER_color_composition.md), the [boundary contract](FILTER_boundary_contract.md) and the [collection contract](FILTER_collections_contract.md).

## Behavior and Acceptance

Each member's support set is normative. Having a halo does not imply independent execution on an arbitrary cropped ROI. Staged rounding, global connectivity, transforms, and iterative state follow each member's definition. Outputs are independently requestable. Members are accepted against the [acceptance protocol](FILTER_oracle_protocol.md).

## References

[S17 · PyWavelets 2D DWT/IDWT](../research-sources.md#s17); [S22 · scikit-image restoration](../research-sources.md#s22)
