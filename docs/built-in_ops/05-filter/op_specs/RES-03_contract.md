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

CPU implementations and C++ authoring helpers are available under the contracts below. The specification remains Proposed. It inherits the [FILTER common contract](FILTER_common_contract.md), [RES native floating-point contract](RES_native_contract.md), and [FMT](../../02-format-color/op_specs/FMT_common_contract.md).

## Members

| ID | Function Spec | Kind | Numeric Reference |
| --- | --- | --- | --- |
| RES-03A | [Wavelet detail thresholding](RES-03A_threshold_wavelet_details.md) | primitive | F |
| RES-03B | [Wavelet denoising with a fixed shift set](RES-03B_cycle_spin_wavelet_denoise.md) | authoring_helper | F |

Suffix letters denote distinct mathematical/interface objects, not quality tiers that a backend may choose arbitrarily. Each primitive uses one native fp32/fp64 key; a helper orchestrates explicit algorithmic stages.

## Shared Mathematics, Units, and Profiles

See each member file listed above for its formula. Members do not share undeclared implicit preprocessing. Parameters with the same name are reusable only when their units, boundaries, normalization, and RN stages match.

## Shared Execution and Numeric Obligations

All members use numeric class F under [RES native floating-point contract](RES_native_contract.md). The input dtype selects native fp32 or fp64 arithmetic and stored state. Caller domain premises remain unchecked; shape, support, copy branches and algorithmic stages remain normative. There is no strict/accelerated quality split or NUM four-ULP fallback.

Positive-weight normalization, signed kernels, local regression, and global inverse problems have different semantics and must not be treated as one alpha-blurring operation. See [color composition](FILTER_color_composition.md). Spatial extension, anchors, and origins follow the [boundary contract](FILTER_boundary_contract.md); structured outputs follow the [collection contract](FILTER_collections_contract.md). Shape depends only on descriptors; each member defines mathematical support and fixed compiled reads.

## CPU Implementation and Public API

Include `photospider/ops/restoration.hpp` and register the bundled operations
with `make_default_operation_registry()`.

- `ps::restoration::threshold_wavelet_details(document, bands, thresholds, mode)`
  returns an edited `frequency::WaveletBands` directory. It validates every
  static band descriptor and preserves the directory order, profile, axes,
  grid, units and final LL reference. Both decimated FRQ10 profiles and
  `stationary_haar_mean_v1` are supported.
- `ps::restoration::cycle_spin_wavelet_denoise(document, input, thresholds,
  options, shifts, mode)` returns a `WorkflowInput`. It appends explicit
  permutation, analysis, threshold, synthesis and native accumulation nodes.
  At zero levels it returns the input reference and appends no nodes.

Both helpers update the document transactionally. Structural errors return
`InvalidArgument` or `TypeMismatch`; allocation failure may throw. Edits to one
document require caller serialization. References describe graph ownership and
do not cache input payloads. Existing node IDs and forward producer references
are reserved during composition.

The primitive key is `restoration.threshold_wavelet_details`. Its first input
is the FRQ10 packed Result with schema
`photospider.filter_bands.<profile>`, version 1; its second input is a single
Float64 tensor of sample shape `[total_levels,3]`. Output port `bands` preserves
the complete input schema. Packed directories contain one to five local
levels, ordered `LH_y_1, HL_x_1, HH_1, ... , LL`. The SDK composes deeper
directories and uses zero-based `first_level` offsets into the shared threshold
table. Intermediate residual producers close the static directory and are
absent from the executable demand when only detail outputs are requested.

| Primitive parameter | Type | Meaning |
| --- | --- | --- |
| `mode` | String | Explicit `hard` or `soft` |
| `first_level` | Int64 | Nonnegative global threshold-row offset |
| `total_levels` | Int64 | Positive row count of the shared threshold table |
| `y_axis`, `x_axis` | Int64 | Distinct valid logical sample axes |
| `original_height`, `original_width` | Int64 | Positive dimensions entering this local decomposition |

The constructor checks all detail and residual shapes against the profile and
local decomposition, including unselected members. Threshold values remain
unchecked caller premises and are converted to the coefficient dtype once per
invocation. A threshold output sample depends only on that input coefficient
and its one threshold-table entry. LL reads only LL and uses COPY when
materialized. The standard compiled tile layout contract applies; importing a
different external layout requires explicit preparation of compatible storage.

The helper primitives `restoration.wrap_shift` and
`restoration.accumulate_native` implement the explicit cycle-spin stages. The
former takes `y_axis`, `x_axis`, signed Int64 `shift_y`, `shift_x`, and explicit
String `expected_shape`, `expected_dtype`; it uses
`S_s(input)[y,x] = input[(y+sy) mod H,(x+sx) mod W]` and preserves every bit.
The latter takes two matching native tensors and a Float64 `divisor`, converted
to their dtype, and computes a native sum followed by native division.
Intermediate accumulation uses divisor one and the final accumulation uses
the number of specified shifts. A single branch needs no accumulation node.

Thresholding and the helper point operations admit their fixed numerical work
before execution, charge one stage per invocation and poll cancellation at
blocks of at most 256 samples. Narrow two-dimensional regions share SIMD
dispatch across a bounded row block; general rank, tails and fixed strided
views use bounded spans. CPU paths use NEON or runtime-dispatched AVX2 with
native scalar tails. No private worker pool or cross-call numerical cache is
introduced. FRQ10 stages retain their own support, work and cancellation rules.

## Dependencies and Acceptance

Each member’s support set is normative. Having a halo does not imply independent execution on an arbitrary cropped ROI; paging does not eliminate a whole-image dependency. Staged rounding, global connectivity, transforms, and iterative state must follow each member’s definition. Outputs are independently requestable. Only selected computation and required algorithmic control contribute payload reads; no payload-validation phase is introduced.

All members must pass the [common acceptance protocol](FILTER_oracle_protocol.md). Oracle reference scope is documented in the [oracle guide](../../../../oracle/ops/filter/README.md); it does not replace runtime acceptance. D1/D2 indicate draft maturity, not acceptance status.

## References and Conformance Notes

[S17 · PyWavelets 2D DWT/IDWT](../research-sources.md#s17);[S22 · scikit-image restoration](../research-sources.md#s22)

Boundary rules, tie-breaking, numeric domains, public schemas, and proposed keys follow the applicable contracts. Algorithm sources do not establish parameter defaults or grant licenses to third-party software.
