---
spec_schema_version: 1
id: PTH-04A
parent_id: PTH-04
function: resample_uniform_t
kind: primitive
category: 03-generation
status: Accepted
implementation_status: implemented
clarification_status: resolved
operation_keys:
  - path.resample_uniform_t_v1_strict
---

# PTH-04A: resample uniform t

Inherits [PTH-04](PTH-04_resampling_contract.md) and [GEN common](GEN_common_contract.md).

## Interface

Operation keys: `path.resample_uniform_t_v1_strict`.

PathSet; PathSamplesParam Result with positions,source_segment,local_t.

The associated Result uses `photospider.ops.path_samples_param` v1 from the [shared geometry schema](PTH_geometry_contract.md). Payload validity is unchecked.

### Parameters and domain

samples_per_segment n>=2, hard max_count; endpoint_mode=per_segment.

Accepts CoreVerbs and Primitives PathSets. `samples_per_segment` defaults to 2; `max_count` is a static capacity. The output schema is constructed by `generation::path_samples_schema` (parameter variant).

## Mathematics

For every evaluable segment use exact t_j=j/(n-1); RN64 positions from the complete formula, local_t rounded independently. Order subpath/segment/j; Z is a segment. Shared endpoints may repeat. M-only emits its point once; FullTurn includes duplicate closure endpoint.

## Behavior

Support class: complete input. No arc length is computed.

## Acceptance

Unequal distances at uniform t, shared endpoint duplication,n2 endpoints and empty Result.
