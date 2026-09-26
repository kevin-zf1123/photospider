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

Complete linear RGB image[H,W,C], strength[1]>=0,kernel[Kh,Kw]; same described image.

## Parameters and domain

mode=monochrome/independent_rgb; explicit boundary and normalization=none/sum/l2;
seed/stream/frame. strength is a multiplier, not standard deviation.

## Mathematical specialization

Domain10 Box-Muller white source passes through NOI-03A, preserving both source/filter
RN stages. Shared mode uses one g for RGB, independent mode addresses components0/1/2.
Output RN_T(c+strength*g); strength0 copies RGB bits. Copy alpha unchanged and also
modify hidden RGB at alpha0. No clipping.

## Dependencies, resources and failures

Execution rule: **Halo**. Read only the boundary-mapped source halo required by Q;
validate the full kernel and shared controls. Full shared-control/topology/resource
changes conservatively invalidate dependent output; sample changes follow the actual
dependency map. Empty requests perform static preflight without payload reads or
advancing a random sequence. Compute only requested output mathematics, retaining
required shared validation.

Halo-driven source/correlation plus regional mix; charge actual intermediates and
preserve node rounding.

All output, scratch, indices, validation work and retained owners are accounted. Poll
cancellation in bounded loops. Preserve schema, domain, NoSolution, NotConverged,
InvalidQuality, ArithmeticOverflow and host resource failures as distinct outcomes.
Never replace budget/cancellation failure with a lower-quality successful prefix. Views,
lifetime and actual returned coverage inherit GEN-common.

## Independent fixtures and acceptance

Zero-strength and alpha signed-zero copies; hidden color changes; HDR/negative RGB;
normalization and duplicate boundary-weight cases.

Check requested-region/full-output equivalence, actual read/work bounds, legal
strides/offsets, separate/joint outputs, cache identity, low budgets, cancellation,
associations and retained owners as applicable. The [oracle
coverage](../oracle-coverage.md) describes the available finite reference subset; it is
not runtime completion. Formal schemas, unresolved algorithms and backend admission must
be implemented and validated before registration. No unverified GPU or third-party bit
identity is implied by these requirements.
