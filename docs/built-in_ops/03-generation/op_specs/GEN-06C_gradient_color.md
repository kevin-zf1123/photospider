---
spec_schema_version: 1
id: GEN-06C
parent_id: GEN-06
function: gradient_color
kind: composite_workflow
category: 03-generation
status: Accepted
implementation_status: not_implemented
clarification_status: member_technical_details_pending
---

# GEN-06C: gradient color

Inherit [GEN-06](GEN-06_gradient_lookup_contract.md), [GEN
common](GEN_common_contract.md), and the applicable [random](NOI_random_contract.md) and
[geometry](PTH_geometry_contract.md) contracts. Proposed keys are not runtime
registrations. A key includes algorithm version and profile; within one version/profile,
output bits and discrete decisions are fixed.

## Interface and representation

The ordinary floating-hue members consume `t` of shape S, `stops[K]` and `colors[K,C]`, and produce numerical samples of shape S+[C]. Ports, dtypes, K bounds and model-specific arity follow [CRV-06](../../01-numeric/op_specs/CRV-06_color_ramp.md). Rational-pi members use the corresponding `[K,2]` floating colors and separate Int64 numerator/denominator ports. S=HW gives a logical image-shaped output; the Result carries no color description.

## Parameters and domain

Compose the corresponding RGB/XYZ/CMYK/Lab/LCh/OKLab/OKLCh/HSL/YCbCr CRV-06 member with explicit upstream spread. `color_description` uses the existing `TensorDescription` v4/v5 codec and describes one canonical logical tuple with `channel_axis=0`; it is independent of leading query/sample dimensions. RGB alone admits optional straight alpha. For downstream FMT calls, explicitly construct a description for the actual output tensor, such as `channel_axis=2` for `[H,W,C]`. CMYK profile resources are selected by the consuming CRV operation's preparation.

## Mathematical specialization

Use the current CRV-06 mathematics and preserve each public node's rounding boundary. For RGB interpolate in linear light: Q=(1-w)*a0*D(c0)+w*a1*D(c1), A=(1-w)*a0+w*a1; the straight result is E(Q/A) for A>0, and transparent black for A=0. Direct/identical-row paths, zero signs and transparent canonicalization follow the selected CRV member. Lab uses l=L*/100; polar members retain original hue and winding.

Finite ordered stops, consumed queries, selected model-valid colors and representable outputs are the current CRV-06 caller preconditions. If exact output alpha is positive and rounds to zero, every rounded RGB component must also be zero; a valid all-zero tuple becomes canonical +0. Numerical precondition violations have no promised runtime domain, overflow or association-underflow diagnostic. This composition adds no numerical validator.

## Dependencies, resources and failures

Execution rule: **Regional**. Compute requested output coordinates only. Read
corresponding query/sample inputs and the controls required by the mathematics. Complete control reads do not imply runtime payload validation. Full
shared-control/topology/resource changes conservatively invalidate dependent output;
sample changes follow the actual dependency map. Empty requests perform static preflight
without payload reads or advancing a random sequence. Compute only requested output mathematics, following the declared dependencies and static structure checks.

Regional query computation retains each composed producer's fixed-task dependency rules. Current CRV-06 reads the selected query tiles and conservatively declares complete stops/colors/p/q tile closures because row selection depends on payload values. Full declared reads do not imply numerical inspection of unselected rows. Empty requests retain static schema/parameter/resource checks and emit no CRV payload task.

Preserve static schema/parameter/resource failures and operational capacity, cancellation, stale-plan, backend and upstream statuses from each constituent operation. Incomplete CRV arithmetic certification returns ResourceExhausted. Account output, scratch, tile records, required static validation and retained owners. Successful call outputs follow the frozen executor's retirement rules; a failed required task prevents publication of that call's output. Numerical precondition violations are outside this workflow's promised runtime diagnostics.

## Independent fixtures and acceptance

Transparent red to opaque blue midpoint is blue with alpha 0.5. Cover native Lab/OKLab units, multi-turn hue, direct/identical-row zero signs and valid all-zero alpha-underflow canonicalization. An oracle may reject probes outside the CRV caller premises to delimit its reference domain; that rejection is not an expected runtime failure. Static description/resource errors and operational failures retain their separate acceptance cases.

Check requested-region/full-output equivalence, actual read/work bounds, legal
strides/offsets, separate/joint outputs, cache identity, low budgets, cancellation,
associations and retained owners as applicable. The [oracle
coverage](../oracle-coverage.md) describes the available finite reference subset; it is
not runtime completion. The specified schemas, member algorithms and backend implementations must be delivered and validated before registration. No unverified GPU or third-party bit
identity is implied by these requirements.
