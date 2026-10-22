---
spec_schema_version: 1
id: NOI-09B
parent_id: NOI-09
function: artistic_linear_grain
kind: composite_workflow
category: 03-generation
status: Accepted
implementation_status: not_implemented
clarification_status: member_technical_details_pending
---

# NOI-09B: artistic linear grain

Inherit [NOI-09](NOI-09_grain_contract.md), [GEN common](GEN_common_contract.md), and
the applicable [random](NOI_random_contract.md) and [geometry](PTH_geometry_contract.md)
contracts. Proposed keys are not runtime registrations. A key includes algorithm version
and profile; within one version/profile, output bits and discrete decisions are fixed.

## Interface and representation

A Result explicitly interpreted as a complete linear RGB image[H,W,C], strength[1]>=0 and kernel[Kh,Kw]; output has the same sample shape. The caller supplies the color description to each consuming operation; no description is attached to the output Result.

## Parameters and domain

mode=monochrome/independent_rgb; explicit boundary and normalization=none/sum/l2;
seed/stream/frame. strength is a multiplier, not standard deviation.

## Mathematical specialization

Domain10 Box-Muller white source uses open52 from word 0 and halfopen53 from word 1 of one block per logical channel with draw fixed to 0, as specified by the shared random contract. It passes through NOI-03A, preserving both source/filter RN stages. Shared mode uses one g for RGB, independent mode addresses components0/1/2.
Output RN_T(c+strength*g); strength0 copies RGB bits. Copy alpha unchanged and also
modify hidden RGB at alpha0. No clipping.

## Dependencies, resources and failures

Execution support: **Halo**. Derive mathematical halo support from each selected
complete output tile, apply the declared boundary mapping, and close reads on
the source grid. Declare kernel/shared controls as fixed dependencies. Payload
validity is a caller premise. Empty output selection performs static preparation
without payload tasks; shared work uses explicit intermediate Results and edges.

Halo-driven source/correlation plus regional mix; charge actual intermediates and
preserve node rounding.

All output, scratch, indices, algorithmic work and retained owners are accounted. Poll
cancellation in bounded loops. Preserve static structural failures and actual
resource, cancellation, backend and upstream statuses. Member-specific no-solution,
refinement or quality failures apply to valid inputs and the explicitly declared
algorithm; numerical-premise violations have no prescribed runtime diagnostic.
Never replace budget/cancellation failure with a lower-quality successful prefix. Views,
lifetime and actual returned coverage inherit GEN-common.

## Independent fixtures and acceptance

Zero-strength and alpha signed-zero copies; hidden color changes; HDR/negative RGB;
normalization and duplicate boundary-weight cases.

Check selected-tile/full-output equivalence, source-grid read closure, legal
strides/offsets, separate/joint outputs, version/profile identity, low budgets, cancellation,
associations and retained owners as applicable. The [oracle
coverage](../oracle-coverage.md) describes the available finite reference subset; it is
not runtime completion. The specified schemas, member algorithms and backend implementations must be delivered and validated before registration. No unverified GPU or third-party bit
identity is implied by these requirements.
