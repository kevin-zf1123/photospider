---
spec_schema_version: 1
id: NOI-09B
parent_id: NOI-09
function: artistic_linear_grain
kind: composite_workflow
category: 03-generation
status: Accepted
implementation_status: implemented_public_authoring_helper
clarification_status: resolved
---

# NOI-09B: artistic linear grain

Inherits [NOI-09](NOI-09_grain_contract.md) and [GEN common](GEN_common_contract.md).

## Interface

A Result explicitly interpreted as a complete linear RGB image[H,W,C], strength[1]>=0 and kernel[Kh,Kw]; output has the same sample shape. The caller supplies the color description to each consuming operation; no description is attached to the output Result.

### Parameters and domain

mode=monochrome/independent_rgb; explicit boundary and normalization=none/sum/l2; seed/stream/frame. strength is a multiplier, not standard deviation.

Public SDK helper, declared in `photospider/ops/generation.hpp`:

```cpp
Result<WorkflowNodeOutput> ps::generation::artistic_linear_grain(
    WorkflowDocument* document, WorkflowInput image, WorkflowInput strength,
    WorkflowInput kernel, const TensorDescription& description,
    std::string color_group, Parameters white_parameters,
    Parameters correlation_parameters, std::string mode = "monochrome",
    Profile profile = Profile::Strict);
```

The helper appends `noise.grain_white_v1_<profile>` (domain 10), `noise.correlate_kernel_v1_<profile>` (NOI-03A) and `noise.artistic_grain_mix_v1_<profile>`, and returns the mix `image` output. `white_parameters` and `correlation_parameters` configure the first two nodes; their dtypes define the two published RN stages. `description` must be a complete straight linear RGB description (`channel_axis=2`) with optional same-tensor alpha; `color_group` selects the group. Physically permuted channels are mapped by their logical red/green/blue roles. The mix retains the image dtype, preserves alpha bits, and copies RGB bits when strength is zero. The output carries no color description. No composite operation key exists.

## Mathematics

Domain10 Box-Muller white source uses open52 from word 0 and halfopen53 from word 1 of one block per logical channel with draw fixed to 0, as specified by the shared random contract. It passes through NOI-03A, preserving both source/filter RN stages. Shared mode uses one g for RGB, independent mode addresses components0/1/2. Output RN_T(c+strength*g); strength0 copies RGB bits. Copy alpha unchanged and also modify hidden RGB at alpha0. No clipping.

## Behavior

Support class: Halo for the source and correlation stages; the final mix is pointwise.

## Acceptance

Zero-strength and alpha signed-zero copies; hidden color changes; HDR/negative RGB; normalization and duplicate boundary-weight cases.
