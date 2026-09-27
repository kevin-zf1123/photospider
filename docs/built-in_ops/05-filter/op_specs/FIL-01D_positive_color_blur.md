---
spec_schema_version: 1
id: FIL-01D
kind: primitive
category: 05-filter
status: Proposed
document_maturity: D1_draft
implementation_status: not_implemented
parent_id: FIL-01
function: positive_color_blur
proposed_operation_keys:
- filter.positive_color_blur_strict
numeric_reference: E
oracle_scope: mathematical_reference
research_sources:
- S01
- S02
---

# FIL-01D: positive_color_blur

Fused positive-kernel blur for a semantic linear RGB or Gray color group. This primitive keeps straight color at its public boundary while evaluating coverage-weighted filtering as one specified expression. It is a separate mathematical profile from raw-field convolution, signed kernels, guided filtering, and restoration.

Inherit [FIL-01](FIL-01_contract.md), the shared [FILTER contract](FILTER_common_contract.md), [NUM](../../01-numeric/op_specs/NUM_common_contract.md), [FMT](../../02-format-color/op_specs/FMT_common_contract.md), and [FILTER color composition](FILTER_color_composition.md). This member explicitly overrides FMT finite-color-sample validation for participating color values only: NaN/Inf color samples are accepted and propagated under NUM. FMT metadata/group validation and the requirement that selected coverage alpha be finite and in [0,1] remain in force. The shared color composition contract defines the exact expression, accumulation order, NaN precedence, and rounding stages; this member follows them without deviation.

## Ports and semantic domain

`input` is a semantic linear RGB or linear-Y Gray image group with straight color and optional associated coverage alpha. Validate the selected group metadata and alpha domain; participating alpha is finite in [0,1]. `kernel` is a Float64 two-dimensional array with nonnegative entries and at least one positive entry. `output` preserves the selected color group and its spatial extent. When alpha is present, the output exposes straight color and the filtered coverage according to the shared color-composition contract. An alpha-only request does not read color payload.

A nonzero kernel tap always reads and consumes its corresponding color even when alpha is zero. Thus `0 * NaN` or `0 * Inf` participates according to NUM arithmetic and can propagate NaN through the color result. Taps whose kernel weight is zero are excluded as specified by the shared contract. If the total contributing coverage is zero and every participating color is finite, the output color is positive zero. If participating colors produce NaN, NaN remains observable even when the entire window is transparent. NaN precedence and quieting follow the corresponding NUM reduction contract.

Without an alpha plane the color input is treated as opaque; the operation does not create an alpha output. No implicit color conversion, alpha association, clipping, or transfer-function conversion is performed. Unselected AOV/emission channels are bit copies. Publish only requested outputs; alpha-only requests do not read color.

## Explicit parameters and legal domain

The caller must provide `y_axis`, `x_axis`, `anchor_y`, `anchor_x`, `boundary`, `cval`, and `kernel`. Anchor is mandatory for every kernel shape, including odd dimensions. The boundary and constant value follow the shared boundary contract. The kernel values must be finite and nonnegative, with finite positive exact sum; interpret them as stored Float64 values. There are no constructor defaults.

## Mathematical semantics

For each component, follow the shared exact formulas `A_exact = sum(K_i*a_i)/S`, `P_exact = sum(K_i*a_i*c_i)/S`, `A_out = RN_t(A_exact)`, and `C_out = NaN(P_exact) ? propagate(P_exact) : A_exact>0 ? RN_t(P_exact/A_exact) : +0`. Products and quotients have no hidden intermediate-dtype rounding. Read color at every positive kernel tap even when alpha is zero, so `0*NaN/Inf` follows NUM propagation. Reduce P in row-major tap order with written K,a,c operand order. Zero-weight taps are excluded. Fully transparent finite windows produce +0; participating NaN still propagates. If positive exact coverage rounds to zero, retain the corresponding hidden straight color. This fused expression is not promised bit-identical to separately associating, filtering, and unassociating through FMT because the rounding stages differ.

## Demand, resources, and validation

The data demand is the exact nonzero kernel support after mapping taps through the selected global-image boundary. Read and validate selected-group metadata and every participating coverage-alpha sample, which must be finite and within [0,1]. Do not apply FMT finite-color validation to participating FIL-01D color values. Kernel and static parameters are validation/control inputs. A region boundary never becomes a new image boundary. Descriptor inference depends only on static parameters and input descriptors. Work and temporary storage scale with output samples, selected planes, and nonzero taps; all admission, cancellation, ownership, and failure behavior inherit the FILTER shared contract. Ordinary floating-point overflow and NaN/Inf results follow NUM and do not become generic operation failures.

## Acceptance

The mathematical oracle must compare the fused expression independently from the multi-node FMT associate/filter/unassociate composition and demonstrate that their allowed rounding differences are intentional. Fixtures must cover opaque input, nonzero alpha, hidden finite color at zero alpha, hidden NaN color at zero alpha, zero kernel taps, fully transparent finite windows, fully transparent windows containing NaN, and alpha-only demand. The reference helper has no runtime registration; this member proposes its own node contract, independent from helper defaults. Runtime registration and execution remain unimplemented; no runtime or third-party conformance is implied by this specification.
