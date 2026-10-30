---
spec_schema_version: 1
id: FIL-01D
kind: primitive
category: 05-filter
status: Proposed
document_maturity: D1_draft
implementation_status: implemented
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

Inherits [FIL-01](FIL-01_contract.md), the [FILTER common contract](FILTER_common_contract.md), [NUM](../../01-numeric/op_specs/NUM_common_contract.md), [FMT](../../02-format-color/op_specs/FMT_common_contract.md) and [FILTER color composition](FILTER_color_composition.md). Participating color values admit NaN/Inf and propagate them under NUM. FMT interpretation is explicit configuration. Participating coverage alpha is finite and in [0,1] as an unchecked caller premise. The color composition contract defines the exact expression, accumulation order, NaN precedence, and rounding stages; this member follows them without deviation.

## Interface

Operation key: `filter.positive_color_blur_strict`.

`input` is a Result whose Tensor members are explicitly interpreted as a linear RGB or linear-Y Gray group with straight color and optional coverage alpha. `kernel` is a Float64 two-dimensional Tensor with nonnegative entries and at least one positive entry as numerical premises. `output` preserves the declared group shape and spatial extent. When the selected group has alpha, the formula defines straight color and filtered coverage according to the color-composition contract; the requested output ports select which components are materialized. Alpha-only selected computation does not require color arithmetic.

Without an alpha plane the color input is treated as opaque; the operation does not create an alpha output. No implicit color conversion, alpha association, clipping, or transfer-function conversion is performed. Unselected AOV/emission channels are bit copies.

### Parameters

The caller provides `input_description` (canonical TensorDescription v4/v5 String), an explicit `group`, `y_axis`, `x_axis`, `anchor_y`, `anchor_x`, `boundary`, `cval`, and `kernel`. Anchor is mandatory for every kernel shape, including odd dimensions. The boundary and constant value follow the shared boundary contract. The kernel values are finite and nonnegative, with finite positive exact sum; they are interpreted as stored Float64 values. There are no constructor defaults.

## Mathematics

For each component, follow the shared exact formulas `A_exact = sum(K_i*a_i)/S`, `P_exact = sum(K_i*a_i*c_i)/S`, `A_out = RN_t(A_exact)`, and `C_out = NaN(P_exact) ? propagate(P_exact) : A_exact>0 ? RN_t(P_exact/A_exact) : +0`. Products and quotients have no hidden intermediate-dtype rounding. A nonzero kernel tap always reads and consumes its corresponding color even when alpha is zero, so `0*NaN/Inf` follows NUM propagation and can propagate NaN through the color result. Reduce P in row-major tap order with written K,a,c operand order. Zero-weight taps are excluded. If the total contributing coverage is zero and every participating color is finite, the output color is positive zero; participating NaN still propagates even when the entire window is transparent. NaN precedence and quieting follow the corresponding NUM reduction contract. If positive exact coverage rounds to zero, retain the corresponding hidden straight color. This fused expression is not promised bit-identical to separately associating, filtering, and unassociating through FMT because the rounding stages differ.

## Behavior

Mathematical support contains nonzero kernel taps mapped through the complete-image boundary. A region boundary never becomes a new image boundary. Output inference depends only on static parameters and input schemas.

## Acceptance

The fused expression is compared independently from the multi-node FMT associate/filter/unassociate composition, and their rounding differences are intentional. Fixtures cover opaque input, nonzero alpha, hidden finite color at zero alpha, hidden NaN color at zero alpha, zero kernel taps, fully transparent finite windows, fully transparent windows containing NaN, and alpha-only demand.
