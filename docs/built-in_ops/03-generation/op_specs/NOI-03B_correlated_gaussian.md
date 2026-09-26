---
spec_schema_version: 1
id: NOI-03B
parent_id: NOI-03
function: correlated_gaussian
kind: composite_workflow
category: 03-generation
status: Accepted
implementation_status: not_implemented
clarification_status: member_technical_details_pending
---

# NOI-03B: correlated gaussian

Inherit [NOI-03](NOI-03_correlated_contract.md), [GEN common](GEN_common_contract.md),
and the applicable [random](NOI_random_contract.md) and
[geometry](PTH_geometry_contract.md) contracts. Proposed keys are not runtime
registrations. A key includes algorithm version and profile; within one version/profile,
output bits and discrete decisions are fixed.

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

Execution rule: **Halo**. Read only the boundary-mapped source halo required by Q;
validate the full kernel and shared controls. Full shared-control/topology/resource
changes conservatively invalidate dependent output; sample changes follow the actual
dependency map. Empty requests perform static preflight without payload reads or
advancing a random sequence. Compute only requested output mathematics, retaining
required shared validation.

Halo-driven intermediate white samples and accounted backing; no rounding-changing
fusion.

All output, scratch, indices, validation work and retained owners are accounted. Poll
cancellation in bounded loops. Preserve schema, domain, NoSolution, NotConverged,
InvalidQuality, ArithmeticOverflow and host resource failures as distinct outcomes.
Never replace budget/cancellation failure with a lower-quality successful prefix. Views,
lifetime and actual returned coverage inherit GEN-common.

## Independent fixtures and acceptance

Impulse kernel bit-equivalence, small repeated-boundary cases, mean/variance
distinction.

Check requested-region/full-output equivalence, actual read/work bounds, legal
strides/offsets, separate/joint outputs, cache identity, low budgets, cancellation,
associations and retained owners as applicable. The [oracle
coverage](../oracle-coverage.md) describes the available finite reference subset; it is
not runtime completion. Formal schemas, unresolved algorithms and backend admission must
be implemented and validated before registration. No unverified GPU or third-party bit
identity is implied by these requirements.
