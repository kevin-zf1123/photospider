---
spec_schema_version: 1
id: CRV-07
category: 01-numeric
kind: shared_operator_contract
status: Proposed
document_maturity: D1_draft
implementation_status: implemented
clarification_status: complete
---

# CRV-07: three-dimensional LUT application

## Native lightness and explicit description

The [shared scale contract](../../02-format-color/op_specs/FMT_relative_coordinate_scale.md) uses native CIELAB/CIELCh `l=L*/100` for ramp colors, LUT input axes, output table coordinates and validation points. Finite values outside 0..1 remain legal. Opponent/chroma scales, hue winding and arithmetic formulas are unchanged. Color configuration uses canonical TensorDescription v4/v5 tuple parameters; explicit L* units are rejected without sample-magnitude inference.

Numeric profile: strict retains the exact reference defined below. Floating arithmetic in accelerated profiles follows the shared [final FP32 four-ULP contract](NUM_accelerated_contract.md). Discrete results, copies, selected endpoints and special values remain exact.

Inherit the [NUM execution baseline](NUM_common_contract.md) for specification/registration status, CPU target identity and floating environment. Rounding is nearest/ties-to-even with gradual underflow. This is limited inheritance: the ports, output structures, observation units, mathematical rounding boundaries and explicit numerical/error rules in this specification take precedence. It does not turn a composite template or structured Result into a generic NUM Tensor primitive.

## Discrete table and explicit interpolation selection

The table samples a finite grid; it is not a mapping entry for each representable floating input. For example a 33x33x33 grid stores 35,937 three-component colors, and values between grid vertices are interpolated. The floating input dtype does not determine the number of table entries.

Interpolation is selected by operation identity: apply_lut3d_trilinear or apply_lut3d_tetrahedral, each with strict, apple_silicon and x86_64 profile keys. There is no implicit interpolation default or additional method parameter. The two methods can produce different values from the same grid, as shown by the cross-component fixture below. Nr/N0 and the other table extents control sampling density; they do not select the interpolation algorithm.

Supported models are RGB, XYZ, CIELAB, OKLab, CIELCh(ab), OKLCh, HSL and YCbCr. Input and output must use the same model. Input is [...,3], table is [N0,N1,N2,3], and output values has the complete input sample shape. The first three table axes follow the input model's channel order; the last axis contains output model components. This is a joint three-variable mapping, not three independent one-dimensional LUTs or a scalar color ramp.

Channel order is RGB; X,Y,Z; l,a*,b*; L,a,b; l,C*,h; L,C,h; H,S,L; or Y',Cb,Cr, respectively. Hue uses the original floating radian/pi-multiple value without normalization. No implicit periodic lookup or hue endpoint substitution occurs. CMYK's four input dimensions and alpha are outside this scope.

## Grid coordinates

Dynamic axis is Float64[3,3]. Each row is [start,end,step] for one input component. Each extent Ni is 2..256, independently ascending or descending. The caller guarantees finite endpoints and step, distinct endpoints, and step bits equal to RN64((end-start)/(Ni-1)), with a finite nonzero result of the correct direction. Interior knots are RN64(((Ni-1-i)*start+i*end)/(Ni-1)); the first and last knots preserve endpoint bits. Every reconstructed knot must be strictly ordered numerically, so -0 and +0 cannot form separate knots.

These numerical conditions are unchecked caller preconditions. Knots are defined by the six endpoints; the redundant step column does not contribute to interpolation. Unequal extents are supported.

## Color spaces

Static input and output color descriptions declare identical model identity. Input coordinates are interpreted using the input description; table and output coordinates use the output description. Results carry no description. Descriptions may differ within the same model: e.g. RGB primaries/transfer, CIELAB white, or polar hue unit. The LUT values express that transformation; OKLab/OKLCh still require fixed D65. Lookup uses the actual input component values directly. No implicit decoding, white adaptation, model conversion or re-encoding surrounds interpolation.

## Boundary behavior

Static out_of_domain is reject or clamp, default reject. Clamp each input component independently to that axis's actual numeric minimum/maximum, then apply the selected interpolation method. Descending axes use numeric endpoints, not an assumption that the first stored coordinate is smaller. No extrapolation is supported.

## Types and model validity

input and table independently accept Float32/Float64, axis is Float64, and static output dtype is Float32/Float64; the public helper defaults it from the explicit authoring `input_type` hint, independently of the bound Result dtype. Consumed query components, positive-weight table colors and final outputs must be finite and obey the relevant model's legality rules as unchecked caller preconditions. Original query colors must be legal before clamp; complete positive-weight table colors must be legal in the output description. In particular CIELCh/OKLCh require C>=0; HSL S/L and other signed/HDR coordinates retain the finite extensions selected in CRV-06. No automatic hue normalization or nominal-range clipping occurs.

## Precision

Both methods provide strict, accelerated_apple_silicon and accelerated_x86_64 operation keys. Strict correctly rounds the complete interpolation reference formula; accelerated final results use the shared FP32-scaled bound. The strict reference computes exact weights from the reconstructed Float64 grid coordinates and actual input values, then correctly rounds each whole weighted sum once at output dtype. Rounded per-axis blends are not the reference mathematical function.

## Interface

Ordered dynamic inputs are input, table, axis; output is values. Required static String parameters are `input_color_description` and `output_color_description` (encoded with `tensor_description_parameter` on canonical logical color tuples), `dtype` and `out_of_domain`. Both descriptions require three components, no alpha and a floating hue unit; rational hue is restricted to split-port ramps. They explicitly identify the same supported model and applicable white/primaries/transfer and hue/coordinate units; no description is read from or attached to a Result. Table values use the output description. Constructors provide the documented dtype/policy defaults. No extra interpolation mode is accepted because the methods are independent operations.

Each input is a Result containing one tensor member under any schema id/version and member key. Shapes use complete `sample_shape()`: input [...,3] has rank 2..8 and logical element count <=2^40, table is [N0,N1,N2,3], and axis is [3,3]. A single query color uses [1,3].

The output `values` uses a fresh `photospider.tensor` v1 schema with member `samples`, the complete input sample shape as its descriptor shape, selected Float32/Float64 dtype, and `atomic_trailing_axes=1`. It carries no color description and does not preserve the input's batch-axis partition.

Malformed descriptions/parameters or extents outside supported ranges fail with InvalidArgument; incompatible shape/dtype/schema uses TypeMismatch. Unsupported CPU profiles fail BackendUnavailable.

## Mathematics: interpolation formulas

For each input axis, choose adjacent table indices j,j+1 containing the query after any clamp. At an interior exact grid coordinate choose the cell starting at that index; at the terminal coordinate choose the last cell. This rule uses index order, including descending coordinate arrays. Local coordinates are exact t_i=(query_i-axis_i[j_i])/(axis_i[j_i+1]-axis_i[j_i]), in [0,1].

Trilinear: for vertex bits b in {0,1}^3, weight is the product of t_i when b_i=1 and 1-t_i otherwise. Each component is the correctly rounded exact sum of weight*table[j+b,component]. Only exact positive weights contribute.

Tetrahedral: split every unit cube along (0,0,0) to (1,1,1) into six tetrahedra. Sort t descending, breaking ties by input axis index 0,1,2, to obtain a,b,c. Vertices are v0=000, v1=e_a, v2=e_a+e_b, v3=111. Weights are 1-t_a, t_a-t_b, t_b-t_c, t_c. Correctly round the exact weighted component sum once. Skip zero weights. All comparisons and differences are exact rational operations, so cell/split classification never depends on floating evaluation order.

Both formulas are convex within the cell. A table's hue components are ordinary unnormalized numeric values in their declared output unit; there is no circular interpolation or implicit periodic domain. Input/output model constraints, including chroma nonnegativity, are caller preconditions on original queries, contributing table colors and final outputs.

## Behavior

Each output color depends on its query color, the axis endpoints (axis region [0:3,0:2]) and the positive-weight table vertices: at most eight for trilinear and four for tetrahedral. Numerical conditions on zero-weight table vertices are outside this operation's preconditions. Region and tile results equal the corresponding slice of the whole result.

The caller guarantees axis consistency/order, finite and model-valid original queries and contributing table colors, the closed axis domain under reject, and finite final values in the chosen output dtype. Clamp acts on otherwise valid original queries. Violating these numerical preconditions has no promised domain or overflow diagnostic. Insufficient arithmetic capacity never substitutes approximate interpolation.

### Signed zero

If exactly one vertex contributes, its weight is one: directly convert its color and preserve zero signs. With multiple contributing vertices, a component's exact zero is -0 only when every contributing component is -0; otherwise it is +0. Nonzero underflow preserves the mathematical result's sign. Zero-weight vertices never participate in sign determination. These rules apply equally to hue components and every supported color model.

## Acceptance

Expected values use exact-rational vertex weights and component sums plus independent correctly rounded dtype conversion. Identity tables, affine transforms, grid vertices, faces/edges, split-plane ties, all six tetrahedra, all axis directions, unequal axis extents, mixed dtypes and each supported model/description pair are required. Use finite signed/HDR values and large opposite values to expose premature rounding/overflow. Strict matches the method reference bitwise; accelerated uses the shared FP32-scaled final-result bound; trilinear and tetrahedral are not required to match each other.

For axis rows [0,1,1] and a 2x2x2 table at binary vertices (r,g,b), store (r*g,g*b,b*r). Query [0.75,0.25,0.5] gives trilinear [0.1875,0.125,0.375] and tetrahedral [0.25,0.25,0.5]. Use an RGB description for this fixture. A polar-model table with hue 0 to 4 retains intermediate hue 2. No modulo reduction can be substituted for that fixture.

Verify that output tiles preserve complete colors and that generic zero-weight vertices remain numerically unused. Use valid original colors under clamp; invalid numerical inputs do not have a runtime-error oracle.

## References

- [Trilinear primitive](CRV-07A_apply_lut3d_trilinear.md).
- [Tetrahedral primitive](CRV-07B_apply_lut3d_tetrahedral.md).
- [Curve category](../curves.md).
- [1D LUT contract](CRV-05_apply_lut1d.md).
- [Color-array dependency](../../02-format-color/op_specs/FMT-COLOR_color_array_contract.md).
- [Operator template](../../00-foundation/spec-template.md).
- [CLF v3 interpolation appendix](https://docs.acescentral.com/clf/specification/#appendix-a-interpolation): a method reference, not a claim of full CLF file, domain or bitwise compatibility.
