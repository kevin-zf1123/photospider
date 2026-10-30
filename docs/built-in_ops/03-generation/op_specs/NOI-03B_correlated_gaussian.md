---
spec_schema_version: 1
id: NOI-03B
parent_id: NOI-03
function: correlated_gaussian
kind: composite_workflow
category: 03-generation
status: Accepted
implementation_status: implemented_public_authoring_helper
clarification_status: resolved
---

# NOI-03B: correlated gaussian

Inherits [NOI-03](NOI-03_correlated_contract.md) and [GEN common](GEN_common_contract.md).

## Interface

mean[C],sigma[C],kernel[Kh,Kw]; values[H,W,C].

### Parameters and domain

Explicit NOI-02 RNG and NOI-03A boundary/normalization parameters per node; constructor suggestion mean=0,sigma=1,l2.

Public SDK helper, declared in `photospider/ops/generation.hpp`:

```cpp
Result<WorkflowNodeOutput> ps::generation::correlated_gaussian(
    WorkflowDocument* document, WorkflowInput mean, WorkflowInput sigma,
    WorkflowInput kernel, Parameters gaussian_parameters,
    Parameters correlation_parameters, Profile profile = Profile::Strict);
```

The helper appends a NOI-02A node followed by a NOI-03A node and returns the correlation `values` output. The white-source dtype and the final dtype are selected independently in `gaussian_parameters` and `correlation_parameters`; boundary and normalization are set on the correlation node.

## Mathematics

Compose published NOI-02 output into NOI-03A, retaining the white-source rounding stage. L2 gives ideal unit variance only for distinct independent taps; it does not automatically preserve nonzero mean. Apply a separate affine node for a final mean.

## Behavior

Support class: Halo. The intermediate white samples are the published NOI-02A output; no fusion changes the two rounding stages.

## Acceptance

Impulse kernel bit-equivalence, small repeated-boundary cases, mean/variance distinction.
