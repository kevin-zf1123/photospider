---
spec_schema_version: 1
id: GEN-01B
parent_id: GEN-01
function: constant_image
kind: primitive
category: 03-generation
status: Accepted
implementation_status: implemented
clarification_status: resolved
operation_keys:
  - generation.constant_image_v1_strict
  - generation.constant_image_v1_accelerated_apple_silicon
  - generation.constant_image_v1_accelerated_x86_64
---

# GEN-01B: constant image

Inherits [GEN-01](GEN-01_constant_contract.md) and [GEN common](GEN_common_contract.md).

## Interface

Operation keys: `generation.constant_image_v1_strict`, `generation.constant_image_v1_accelerated_apple_silicon`, `generation.constant_image_v1_accelerated_x86_64`.

A single-tensor Result supplies `color[C]` in Float32/64. The output is a single-tensor Result with logical image shape `[H,W,C]` and the same dtype. Color and alpha interpretation are explicit static configuration; the output carries structure and samples only.

### Parameters and domain

Canvas and a complete FMT `TensorDescription` are explicit static parameters. The description uses the actual input vector's channel axis 0 and explicitly identifies the selected color group and any independent alpha. A constructor may explicitly supply linear sRGB/D65/relative coordinates with applicable encoding ranges. A downstream FMT consumer of `[H,W,C]` receives its own description with channel axis 2 and the corresponding group indices.

## Mathematics

The caller supplies finite color components and alpha in [0,1]; copy each component without payload validation. Preserve finite hidden color at alpha=0 and publish planar image samples. Straight association, model and units remain explicit caller-owned configuration; they are neither attached to the Result nor inferred from C. This member uses the shared unchecked numerical contract.

## Behavior

Support class: Regional.

## Acceptance

No alpha, non-last alpha slot, hidden color, Lab l=0.5 and valid alpha endpoints.
