---
spec_schema_version: 1
id: CRV-06H
parent_id: CRV-06
function: color_ramp_ycbcr
operation_family: curve.color_ramp_ycbcr
proposed_operation_keys:
  - curve.color_ramp_ycbcr_strict
  - curve.color_ramp_ycbcr_accelerated_apple_silicon
  - curve.color_ramp_ycbcr_accelerated_x86_64
category: 01-numeric
kind: primitive
status: Proposed
document_maturity: D1_draft
implementation_status: implemented
clarification_status: complete
---

# CRV-06H: color_ramp_ycbcr

Dynamic inputs inherit the [family Result tensor-port contract](CRV-06_color_ramp.md#result-tensor-ports): each is a Result with exactly one tensor member, under any structurally valid schema id/version/member key. Shape checks use complete `sample_shape()` values, including batch axes.

Numeric profile: strict retains the exact reference defined below. Floating
arithmetic in accelerated profiles follows the shared
[final FP32 four-ULP contract](NUM_accelerated_contract.md), including its
range/fallback rules. Discrete results, copies, selected endpoints and special
values remain exact.

Directly interpolate the three Y',Cb,Cr components, retaining the same complete
color description. The description specifies underlying RGB primaries, white,
transfer, YCbCr matrix and numeric range/encoding. The initial variant has no
alpha and no chroma subsampling. It does not convert to RGB to perform blending.

The matrix uses the non-constant-luminance (NCL) form parameterized by static
Kr,Kb, requiring finite Kr>0, Kb>0 and exact Kr+Kb<1. Provide BT.601, BT.709 and
BT.2020 NCL matrix presets plus custom coefficients. A preset only fills matrix
coefficients; primaries/white and transfer remain explicit. BT.2020 constant
luminance uses a different definition and is not supported by this NCL variant.

## Numeric encoding and mathematical identity

Use unbiased floating coordinates: nominal Y' range [0,1], nominal Cb/Cr range
[-0.5,0.5], with neutral chroma at zero. All finite extensions are allowed;
nominal ranges are not clipping or validity limits. Integer full/limited code
values require a separate conversion before this ramp. No code-depth parameter,
chroma offset or full/limited code-range interpretation is inferred.

The descriptor identifies the coordinates through
Y'=Kr*R'+(1-Kr-Kb)*G'+Kb*B', Cb=(B'-Y')/(2*(1-Kb)),
Cr=(R'-Y')/(2*(1-Kr)). These formulas describe the model; the ramp consumes
Y'/Cb/Cr directly and executes no RGB conversion. Named matrix coefficients are
BT.601 Kr=0.299,Kb=0.114; BT.709 Kr=0.2126,Kb=0.0722; BT.2020 NCL
Kr=0.2627,Kb=0.0593. The decimal values identify the standard matrices;
preset helpers correctly round these decimals to Float64; stored values define
the descriptor, and identical custom coefficients match the same preset values. A matrix preset alone does not imply a complete broadcast
standard profile, its RGB primaries or its transfer.

## Interface, arithmetic and execution

Inherit [CIELAB CRV-06C](CRV-06C_color_ramp_cielab.md) for ordered dynamic
input, stops, colors ports and named values output. colors is [K,3] in Y',Cb,Cr
order; values is sample_shape(input)+[3]. Float32/Float64 ports may mix independently;
output dtype defaults to colors dtype. K is 1..65536, input rank 1..7, all
extents positive and logical element counts <=2^40. Stops are globally finite
and strictly increasing. out_of_domain is clamp/reject, default clamp, including
the established K=1 behavior. Input positions remain finite and are read even
when the selected table is constant.

Required static String parameters are color_description, dtype, out_of_domain.
The description explicitly declares YCbCr NCL, underlying RGB primaries/white/
transfer, matrix coefficients/preset, unbiased floating coordinate units and no
alpha. No matrix default silently fills a missing required description. Output coordinates use that interpretation without attaching it to the Result.

For each component c, interpolate RN_dtype((1-w)*c0+w*c1), using the exact
stop-derived w and only one final rounding for strict. Accelerated results obey
the shared final FP32 bound. Hit/clamp/K=1 directly converts the selected row. Direct/identical-row
paths preserve signed zero; genuine interpolation exact zero is +0; nonzero
underflow preserves sign. Mathematically selected source components and final results must be finite.

All profiles use the [fixed execution contract](CRV-06_color_ramp.md#fixed-execution-and-caller-preconditions). Each selected complete-color output tile has one CPU task. Its reads cover the corresponding query region and all stops/colors/p/q tiles, each closed on its own source grid. Runtime selection uses one exact-hit/clamp/singleton row or two enclosing rows. Generic unselected colors and denominators are numerically unused; upstream failures still propagate through declared tile dependencies.

The `values` Result uses `photospider.tensor` v1/member `samples`, descriptor shape `sample_shape(input)+[C]`, and `atomic_trailing_axes=1`. It carries no color description or input batch-axis partition. The kernel writes only selected dense output tiles, borrowing affine source views and owning fixed arithmetic scratch. Empty emits no task or sample read. Successful call publication follows required task completion in retirement order, and output storage survives context teardown.

Finite ordered stops, finite queries, selected complete-color model/association validity, selected rational q>0, reject-domain membership and representable final output are unchecked caller preconditions. Lookup costs O(P log K) for P selected query points, plus exact/certified arithmetic; the kernel searches source stops directly without an O(K) copy. Static schemas, parameters, resources and kernel signatures remain checked. Cancellation and capacity failures stop execution without approximate substitution.

## Failure and acceptance

Malformed static parameters or descriptions fail preparation with InvalidArgument; incompatible port schemas, shapes or dtypes use TypeMismatch. Numerical conditions are caller preconditions, including finite destination results. Operational resource, backend, cancellation, stale-plan and upstream failures preserve their status; a failed required task prevents successful call publication.

Fixture: stops=[0,1], colors=[[0,0,0],[1,0.5,-0.5]], input=[0.5]
returns [[0.5,0.25,-0.25]] with unchanged declared YCbCr description. Include
finite Y' outside [0,1], large signed chroma, descriptor mismatches, custom matrix
validation, all float dtype combinations and signed zeros. Use independent exact
rational interpolation/rounding as the numeric oracle. Apply CRV-06C's full
sparse-request, stride, budget, cancellation and owner-lifetime acceptance.
Use temporary Compiler/ExecutionContext workflows and the independent family oracle; the document remains Proposed.

- [Ramp family](CRV-06_color_ramp.md).
- [Color-array description](../../02-format-color/op_specs/FMT-COLOR_color_array_contract.md).
- [BT.709-6](https://www.itu.int/rec/R-REC-BT.709-6-201506-I).
- [BT.601-7](https://www.itu.int/dms_pubrec/itu-r/rec/bt/R-REC-BT.601-7-201103-I%21%21PDF-E.pdf).
- [BT.2020-2](https://www.itu.int/rec/R-REC-BT.2020-2-201510-I/en).
