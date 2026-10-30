---
spec_schema_version: 1
id: GEN-06C
parent_id: GEN-06
function: gradient_color
kind: composite_workflow
category: 03-generation
status: Accepted
implementation_status: implemented
clarification_status: resolved
---

# GEN-06C: gradient color

Inherits [GEN-06](GEN-06_gradient_lookup_contract.md) and [GEN common](GEN_common_contract.md).

## Interface

The ordinary floating-hue members consume `t` of shape S, `stops[K]` and `colors[K,C]`, and produce numerical samples of shape S+[C]. Ports, dtypes, K bounds and model-specific arity follow [CRV-06](../../01-numeric/op_specs/CRV-06_color_ramp.md). Rational-pi members use the corresponding `[K,2]` floating colors and separate Int64 numerator/denominator ports. S=HW gives a logical image-shaped output; the Result carries no color description.

### Parameters and domain

The corresponding RGB/XYZ/CMYK/Lab/LCh/OKLab/OKLCh/HSL/YCbCr CRV-06 member runs after an explicit upstream spread. `color_description` uses the `TensorDescription` v4/v5 codec and describes one canonical logical tuple with `channel_axis=0`, independent of leading query/sample dimensions. RGB alone admits optional straight alpha. A downstream FMT call supplies its own description for the actual output tensor, such as `channel_axis=2` for `[H,W,C]`. CMYK profile resources are selected by the CRV operation.

Public SDK helper, declared in `photospider/ops/generation.hpp`:

```cpp
Result<WorkflowNodeOutput> ps::generation::gradient_color(
    WorkflowDocument* document, WorkflowNode color_ramp,
    std::string method = "pad",
    Profile spread_profile = Profile::Strict);
```

`color_ramp` is built with the matching `numeric::color_ramp_*_node` helper from `photospider/ops/numeric/color_ramps.hpp`; its first input is the unspread query. The helper appends `generation.spread_coordinate_v1_<profile>` (GEN-06A, `method` = `pad`, `repeat` or `reflect`), rewires the ramp's first input to that node's `values`, then appends the CRV node. All other CRV inputs, parameters, color description, resources and numeric profile are retained, and the CRV `values` output is returned. There is no `generation.gradient_color` operation key.

Supported CRV names are `rgb`, `xyz`, `cmyk`, `cielab`, `oklab`, `ycbcr`, plus `cielch`, `oklch` and `hsl` in radian, `_pi` and `_rational_pi` variants: 15 model/unit variants. The rational-pi variants keep separate Int64 numerator/denominator ports. Hue-unit parameter strings are `radian` and `pi_multiple`.

## Mathematics

Use the current CRV-06 mathematics and preserve each public node's rounding boundary. For RGB interpolate in linear light: Q=(1-w)*a0*D(c0)+w*a1*D(c1), A=(1-w)*a0+w*a1; the straight result is E(Q/A) for A>0, and transparent black for A=0. Direct/identical-row paths, zero signs and transparent canonicalization follow the selected CRV member. Lab uses l=L*/100; polar members retain original hue and winding.

Finite ordered stops, consumed queries, selected model-valid colors and representable outputs are the current CRV-06 caller preconditions. If exact output alpha is positive and rounds to zero, every rounded RGB component must also be zero; a valid all-zero tuple becomes canonical +0. Numerical precondition violations have no promised runtime domain, overflow or association-underflow diagnostic. This composition adds no numerical validator.

## Behavior

Support class: Regional. Each sample depends on its query and on the CRV-06 stops and colors selected by that query. The composition inherits the static checks, numerical caller preconditions and failure statuses of its constituent operations.

## Acceptance

Transparent red to opaque blue midpoint is blue with alpha 0.5. Cover native Lab/OKLab units, multi-turn hue, direct/identical-row zero signs and valid all-zero alpha-underflow canonicalization.
