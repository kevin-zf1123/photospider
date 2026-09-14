---
spec_schema_version: 1
id: NUM-07H
parent_id: NUM-07
function: select
proposed_operation_keys:
  - numeric.select_strict
  - numeric.select_accelerated_apple_silicon
  - numeric.select_accelerated_x86_64
category: 01-numeric
kind: primitive
status: Proposed
document_maturity: D1_draft
implementation_status: implemented_manual_acceptance
repository_branch: ops-impl
repository_commit: current working tree
---

# NUM-07H: select

Inherit the [NUM baseline](NUM_common_contract.md) for specification status,
registration, shared execution and acceptance requirements; explicit rules below
and in the named family contract take precedence.

Select an element from one of two arrays using a boolean-valued UInt8 condition.
Inputs in order are `condition`, `when_true`, `when_false`; all have the same
rank-1..8 positive shape. Both value branches share one dtype chosen from UInt8,
Int64, Float32 and Float64. Output `values` uses the branch dtype, unchanged
shape and empty facets. Infer dtype from when_true, not the UInt8 condition;
the registry's output_dtype_input field can identify this source input.
There are no static numeric parameters or implicit broadcasts/casts.

At each requested position, condition must be exactly 0 or 1. Zero selects
when_false; one selects when_true. Other UInt8 values fail with InvalidArgument,
FailureReason::InvalidDomain and diagnostic tag InvalidCondition, global coordinate and actual byte. Output copies the
selected element's exact bits, including NaN payload, signaling NaN, infinity
and signed zero. This is copying: it does not apply arithmetic NaN quieting.
All three CPU versions are bitwise equivalent; platform-specific keys require
their named CPU platform.

## Staged dependency and invalidation contract

Both branches' dtype/shape metadata is checked at compile/preflight even when
one branch later has no selected samples. Runtime evaluation is staged:

1. For nonempty requested Q, read condition exactly on Q as Control support,
   plus any recognized typed-validation closure. Retain the condition witness
   and verify its 0/1 values before dependent branch reads.
2. Define T={q in Q: condition[q]=1} and F=Q\T. Request when_true Data exactly
   on T and when_false Data exactly on F. Add each selected branch's required
   typed-semantic Validation support separately. Empty sets request no branch.
3. Copy the selected bits into owned output fragments covering Q with correct
   global Region and storage origins. Publish only after the observation's
   required reads and validations succeed.

Empty Q reads none of the three inputs. No unselected branch arithmetic is
requested, and an error at an unselected position has no effect unless that
position is separately required by a selected typed-validation closure. Source
operators with indivisible Whole execution remain subject to their own support
contracts; select cannot promise to hide failures inside an upstream evaluation
that is actually required to obtain selected data.

Retain exact condition decisions and branch/validation witnesses. A change in
condition at q invalidates output q and requires replanning the selected branch;
a change in a selected branch invalidates its selected positions. Changes in
unselected branch Data do not invalidate an observation absent another retained
validation dependency. Do not replace irregular T/F with their bounding boxes
or Whole branch reads silently. Partition/fragment representations must preserve
exact sets; budget failure is ResourceExhausted rather than broadened reads.

## Storage, resources and errors

Read legal immutable strided/offset/unaligned arrays. Own packed output fragments;
no writable alias or implicit gap value is published. Copying sNaN uses integer
or byte transport without accidental floating arithmetic/traps. Generic output
does not remove the obligations attached to recognized input facets.

Work is O(|Q|) plus actual source/validation and set-representation work. Account
condition decisions, T/F runs or indices, retained source owners, output bytes,
fragment metadata and scratch. Alternating conditions can require O(|Q|) set
metadata; no uniform-condition memory assumption is allowed. Use bounded staged
chunks if needed while preserving observation publication/retention rules.
Check cancellation at least every 64 simple elements and each demand/refinement
stage. Charge all actual allocations and release unpublished state on failure;
published owners remain valid after context destruction until final release.

Inherit [copy lifetime/resource conventions](NUM-03B_broadcast.md) where applicable,
without adding its view/dense parameter. Compile/preflight rejects metadata
errors; runtime reports InvalidCondition, upstream/typed failures, budget
exhaustion or cancellation with scope matching the requested observation. No
partial output is published for a failed observation.

## Acceptance and implementation status

Conceptual fixture: condition=[1,0,1], when_true=[10,20,30],
when_false=[1,2,3] -> [10,2,30]. An instrumented upstream must witness true
reads {0,2}, false reads {1}, and condition reads {0,1,2}. Test unselected numeric
failures and invalid typed-validation closure separately; verify that changing
condition changes the next request's exact branch support. Cover all-true,
all-false, alternating/disjoint conditions, invalid byte 2, all four branch
dtypes, distinct sNaN payload bits, source strides, low metadata/output budgets,
cancellation, cache-off and output lifetime after context destruction.

The manual public workflow uses independent bit selection and instrumented
source reads. On 2026-09-14, AppleClang 21 strict/Apple and Ubuntu WSL Clang 18
strict/x86 runs passed, as did the installed consumer. Checks include exact
true/false support, unselected source-error isolation, invalid condition Atom
attribution, four dtype bit patterns, selected typed-validation closure, changed
condition/cache support, strides and composition after `numeric.less`.

The diagnostic identity distinguishes scalar condition/bit choice and the target
ISA scratch store for one observed sample. Fixed padded scratch is not a claim
of four independently selected samples or a performance improvement. No numeric
integration-test registration is included. The specification remains Proposed.
