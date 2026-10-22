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

Inherit [GEN-01](GEN-01_constant_contract.md), [GEN common](GEN_common_contract.md), and
the applicable [random](NOI_random_contract.md) and [geometry](PTH_geometry_contract.md)
contracts. A key includes algorithm version
and profile; within one version/profile, output bits and discrete decisions are fixed.

## Interface and representation

A single-tensor Result supplies `color[C]` in Float32/64. The output is a single-tensor Result with logical image shape `[H,W,C]` and the same dtype. Color and alpha interpretation are explicit static configuration; the output carries structure and samples only.

## Parameters and domain

Canvas and a complete FMT `TensorDescription` are explicit static parameters. The description uses the actual input vector's channel axis 0 and explicitly identifies the selected color group and any independent alpha. A constructor may explicitly supply linear sRGB/D65/relative coordinates with applicable encoding ranges. A downstream FMT consumer of `[H,W,C]` receives its own description with channel axis 2 and the corresponding group indices.

## Mathematical specialization

The caller supplies finite color components and alpha in [0,1]; copy each component without payload validation. Preserve finite hidden color at alpha=0 and publish planar image samples. Straight association, model and units remain explicit caller-owned configuration; they are neither attached to the Result nor inferred from C. This member uses the shared unchecked numerical contract.

## Dependencies, resources and failures

Execution rule: **Regional**. Compute requested output coordinates only. Read
corresponding query/sample inputs and the controls required by the mathematics. Complete control reads do not imply runtime payload validation. Full
shared-control/topology/resource changes conservatively invalidate dependent output;
sample changes follow the actual dependency map. Empty requests perform static preflight
without payload reads or advancing a random sequence. Compute only requested output mathematics, following the declared dependencies and static structure checks.

O(C*area(Q)); read the required color controls without validating their values.

All output, scratch, indices, algorithmic work and retained owners are accounted. Poll
cancellation in bounded loops. Preserve static structural errors and operational NoSolution, NotConverged,
InvalidQuality, ArithmeticOverflow and host resource failures as distinct outcomes.
Never replace budget/cancellation failure with a lower-quality successful prefix. Views,
lifetime and actual returned coverage inherit GEN-common.

## Independent fixtures and acceptance

No alpha, non-last alpha slot, hidden color, Lab l=0.5 and valid alpha endpoints.

Check requested-region/full-output equivalence, actual read/work bounds, legal
strides/offsets, separate/joint outputs, cache identity, low budgets, cancellation,
associations and retained owners as applicable. The [oracle
coverage](../oracle-coverage.md) describes the available finite reference subset; it is
not runtime completion. No unverified GPU or third-party bit
identity is implied by these requirements.
