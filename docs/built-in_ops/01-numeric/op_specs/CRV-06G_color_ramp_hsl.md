---
spec_schema_version: 1
id: CRV-06G
parent_id: CRV-06
function: color_ramp_hsl
category: 01-numeric
kind: shared_operator_contract
status: Proposed
document_maturity: D1_draft
implementation_status: implemented
clarification_status: complete
repository_branch: ops-specs
verification_status: focused_result_validation_passed
repository_commit: current working tree
---

# CRV-06G: HSL ramps

Dynamic inputs inherit the [family Result tensor-port contract](CRV-06_color_ramp.md#result-tensor-ports): each is a Result with exactly one tensor member and no fields, under any structurally valid schema id/version/member key. Shape checks use complete `sample_shape()` values, including batch axes.

Numeric profile: strict retains the exact reference defined below. Floating
arithmetic in accelerated profiles follows the shared
[final FP32 four-ULP contract](NUM_accelerated_contract.md), including its
range/fallback rules. Discrete results, copies, selected endpoints and special
values remain exact.

The explicit color description identifies the underlying RGB primaries, white
and transfer. Defaults are sRGB primaries, D65 and standard piecewise sRGB
transfer. Interpolate supplied H/S/L coordinates directly, retaining the same
underlying RGB description in HSL output. This ramp does not decode and perform
the RGB ramp's linear-light interpolation.

The initial model has exactly three channels and no alpha. Both S and L accept
all finite extended values; [0,1] is not a validity or clipping limit. No automatic
RGB gamut mapping or HSL clipping is performed. The precise extended HSL
conversion interpretation belongs to the associated color-management contract.

## Entrypoints, interpolation and inherited execution

Three independent entrypoints use radian, floating pi_multiple or exact
rational_pi hue. The floating forms take ordered input, stops, colors[K,3],
with channel order H,S,L. The rational form takes input, stops,
saturation_lightness[K,2], hue_numerator[K], hue_denominator[K]. Integers are
Int64; selected denominators must be positive, and unreduced fractions are
valid. Unselected generic q<=0 rows add no mathematical-domain check. Floating ports independently
accept Float32/Float64. Output is values, shape sample_shape(input)+[3], ordered H,S,L.

Inherit [CIELCh CRV-06D](CRV-06D_color_ramp_cielch.md) for K=1..65536,
input rank 1..7 and all positive logical counts <=2^40, finite strictly increasing
stops, clamp/reject default clamp, singleton behavior, output hue units and defaults,
static description matching, complete-color observation, Whole role-13 validation and selected-row mathematics,
whole-input dirty witnesses, layout/owner/resource accounting, cancellation and errors.
Replace its lightness_chroma port with saturation_lightness. Output dtype defaults
to colors dtype or saturation_lightness dtype. Required static String parameters
are color_description, dtype, out_of_domain, output_hue_unit. No alpha or hue_path
parameter is accepted. Input/output descriptions identify HSL, underlying RGB
primaries/white/transfer, hue unit and S/L scales. Described input must match.

Hue is the original unnormalized angle. Interpolate H=(1-w)*H0+w*H1 with the
same exact w as S=(1-w)*S0+w*S1 and L=(1-w)*L0+w*L1. Apply any selected hue unit
conversion as part of the exact formula and correctly round only the final
components. Strict is correctly rounded; accelerated finite arithmetic follows the shared
FP32-scaled final-result bound. S=0 or any other achromatic
configuration never discards hue. No modulo reduction or periodic endpoint
substitution occurs. No HSL-to-RGB or transfer evaluation is needed for this ramp.

The complete color at a query is read, validated and returned atomically.
Global stops and local color rows follow CRV-06D, including local rational q
validation even at zero saturation. All demanded floats and final components
must be finite; unlike CIELCh chroma, S may be negative. Destination overflow
fails the complete color with OperationFailed/ArithmeticOverflow. Invalid q or
nonfinite demanded data uses OperationFailed/InvalidDomain; preflight, upstream,
resource and cancellation categories are inherited without reinterpretation.

## Acceptance

With stops=[0,1], pi colors=[[0,0.25,0.125],[4,0.75,0.875]], input=[0.5],
expect [[2,0.5,0.5]]. Repeat with saturation zero, negative S or L outside [0,1];
hue still follows the original angle. Rational numerators=[0,4], denominators=[1,1]
with saturation_lightness=[[0.25,0.125],[0.75,0.875]] give the same result.
Use independent exact-rational and certified-pi oracles, both output units/dtypes,
mixed source precision, large multi-turn angles, q=0 demand failures, complete-input
reads, partial-channel/full-color expansion, strides, budgets and owner lifetime.
Public Compiler/ExecutionContext workflow bindings and values/description checks
are maintained; current commands and runtime evidence are linked below. Source HSL-to-RGB extension semantics will be fixed by
separate conversion specifications, not guessed by this interpolation operation.

- [Ramp family](CRV-06_color_ramp.md).
- [Radian entrypoint](CRV-06G1_color_ramp_hsl.md).
- [Floating pi entrypoint](CRV-06G2_color_ramp_hsl_pi.md).
- [Rational pi entrypoint](CRV-06G3_color_ramp_hsl_rational_pi.md).
- [CIELCh unnormalized-hue contract](CRV-06D_color_ramp_cielch.md).
- [Color-array description](../../02-format-color/op_specs/FMT-COLOR_color_array_contract.md).

## Maintained implementation and validation
The public helpers `color_ramp_hsl_node`, `color_ramp_hsl_pi_node` and `color_ramp_hsl_rational_pi_node` are declared in [`color_ramps.hpp`](../../../../include/photospider/numeric/color_ramps.hpp); `color_ramps.cpp` implements their Whole Result programs. Coordinate values and original hue ratios use exact rational interpolation. Cross-unit conversion in either direction multiplies or divides by certified pi, with a 4096-bit precision ceiling; same-unit RationalPi expressions cancel pi symbolically.
The focused Result CTest, Strict/Apple manual groups, independent Fraction/Machin-pi and RGB rational/root/Decimal oracles, and installed consumer have passed. See the [family contract](CRV-06_color_ramp.md#maintained-implementation-and-validation) and [workflow README](../../../../examples/numeric_workflow/README.md#color-ramps) for coverage and unsupported platforms/shapes.
