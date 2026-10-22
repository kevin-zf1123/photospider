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

Inherit [NOI-03](NOI-03_correlated_contract.md), [GEN common](GEN_common_contract.md),
and the applicable [random](NOI_random_contract.md) and
[geometry](PTH_geometry_contract.md) contracts. A key includes algorithm version and profile; within one version/profile,
output bits and discrete decisions are fixed.

## Runtime implementation

The public `ps::generation::correlated_gaussian` helper in
`photospider/ops/generation.hpp` implements this composition.

## Interface and representation

mean[C],sigma[C],kernel[Kh,Kw]; values[H,W,C].

## Parameters and domain

Explicit NOI-02 RNG and NOI-03A boundary/normalization parameters per node; constructor
suggestion mean=0,sigma=1,l2.

## Mathematical specialization

Compose published NOI-02 output into NOI-03A, retaining the white-source rounding stage.
L2 gives ideal unit variance only for distinct independent taps; it does not
automatically preserve nonzero mean. Apply a separate affine node for a final mean.

## Dependencies, resources and failures

Execution support: **Halo**. Derive mathematical halo support from each selected
complete output tile, apply the declared boundary mapping, and close reads on
the source grid. Declare kernel/shared controls as fixed dependencies. Payload
validity is a caller premise. Empty output selection performs static preparation
without payload tasks; shared work uses explicit intermediate Results and edges.

Halo-driven intermediate white samples and accounted backing; no rounding-changing
fusion.

All output, scratch, indices, algorithmic work and retained owners are accounted. Poll
cancellation in bounded loops. Preserve static structural failures and actual
resource, cancellation, backend and upstream statuses. Member-specific no-solution,
refinement or quality failures apply to valid inputs and the explicitly declared
algorithm; numerical-premise violations have no prescribed runtime diagnostic.
Never replace budget/cancellation failure with a lower-quality successful prefix. Views,
lifetime and actual returned coverage inherit GEN-common.

## Independent fixtures and acceptance

Impulse kernel bit-equivalence, small repeated-boundary cases, mean/variance
distinction.

Check selected-tile/full-output equivalence, source-grid read closure, legal
strides/offsets, separate/joint outputs, version/profile identity, low budgets, cancellation,
associations and retained owners as applicable. The [oracle
coverage](../oracle-coverage.md) describes the available finite reference subset; it is
not runtime completion. No unverified GPU or third-party bit
identity is implied by these requirements.
