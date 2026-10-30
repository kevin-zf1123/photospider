---
spec_schema_version: 1
id: NOI-08B
parent_id: NOI-08
function: shot_electrons
kind: composite_workflow
category: 03-generation
status: Accepted
implementation_status: implemented_public_authoring_helper
clarification_status: resolved
---

# NOI-08B: shot electrons

Inherits [NOI-08](NOI-08_poisson_contract.md) and [GEN common](GEN_common_contract.md).

## Interface

expected_electrons[H,W,C] finite nonnegative; electrons same shape Int64.

### Parameters and domain

NOI-08A parameters; nonnegative expected electrons are an unchecked caller premise; explicit upstream linear-light/electron calibration.

Public SDK helper, declared in `photospider/ops/generation.hpp`:

```cpp
Result<WorkflowNodeOutput> ps::generation::shot_electrons(
    WorkflowDocument* document, WorkflowInput expected_electrons,
    Parameters parameters);
```

The helper appends one NOI-08A strict node and returns its `counts` output. `parameters` are the NOI-08A parameters. Electron units and upstream calibration are caller-owned; no extra conversion, rounding stage or random source is added.

## Mathematics

Compose NOI-08A with electron units. No implicit dark current, read noise, full well, gain, black level, clipping or ADC. Finite random-grid approximation is disclosed separately from ideal Poisson mean/variance.

## Behavior

Support class: Regional.

## Acceptance

Zero exposure, nonnegative expected-electron inputs, unit binding; no EMVA/camera calibration claim.
