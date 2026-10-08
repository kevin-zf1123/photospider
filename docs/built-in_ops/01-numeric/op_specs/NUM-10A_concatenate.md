---
spec_schema_version: 1
id: NUM-10A
parent_id: NUM-10
function: concatenate
proposed_operation_keys:
  - array.concatenate_strict
  - array.concatenate_accelerated_apple_silicon
  - array.concatenate_accelerated_x86_64
category: 01-numeric
kind: primitive
status: Proposed
document_maturity: D1_draft
implementation_status: implemented_manual_acceptance
repository_branch: ops-specs
repository_commit: current working tree
---

# NUM-10A: concatenate

The strict key follows its exact numeric reference. Accelerated floating
results follow the shared [final FP32 four-ULP contract](NUM_accelerated_contract.md)
where arithmetic applies; raw copies and discrete results remain exact.

Inherit the [NUM baseline](NUM_common_contract.md) for specification status,
registration, shared execution and acceptance requirements. The rules below
specify the NUM-10 behavior.

Concatenate an ordered sequence of 2..256 Results along a static axis. Every
input Result contains exactly one tensor member; its key is unrestricted. All
members share one dtype (UInt8, Int64, Float32 or Float64), rank 1..8, and equal
extents on every non-concatenation axis. Their `sample_shape()` includes any
batch prefix. Each input and the output has at most 2^40 elements. The output
port `values` is a Result with schema `photospider.tensor`, member `samples`,
and the complete ordinary output shape. It has no facets or batch topology.

`axis` is a required static Int64 in `[0, rank)`. `layout` is a required static
String with values `view` and `dense`. There are no casts, broadcasting, or
inserted axes. The selected output extent is the checked sum of input extents.
All element bits, including signaling NaNs, are copied without arithmetic.

## Mapping and demand

Let `L[k]` be input k's extent on the concatenation axis and
`P[k] = sum(L[j] for j < k)`, with `P[0] = 0`. For output coordinate `o`,
choose the unique input `k` where `P[k] <= o[axis] < P[k] + L[k]`. Read that
input at `s[axis] = o[axis] - P[k]` and `s[j] = o[j]` for every other axis.

Static schemas and parameters are checked during specialization. For nonempty
Whole demand, the program requests every input with complete data, validation and
descriptor support (role 13), triggering typed-payload validation before
publication. A request that selects one output slab still prepares every port.
An active source edit dirties the complete output; Empty demand reads no payload
and performs no sample copying. Port reuse does not change the ordered mapping.

## Layout and resource rules

Dense copies the complete output into packed owned storage, costing
`N * element_size` bytes. View proves that all complete inputs form one global
affine map over the same physical owner. Compatible same-owner fragments may
join; singleton dimensions do not constrain strides, and the implementation can
infer a stride from neighboring anchors when the concatenation axis is
singleton in every input. Negative and zero global strides are valid when the
complete mapping is affine. The proof covers all inputs regardless of consumer
projection. An independent owner or incompatible map returns Domain/Run
`ViewUnavailable`; choose Dense when copying is intended. The view retains its
source storage owners and allocates no output payload. Both layouts read through
authorized input windows; Dense does not first pack each complete input.

Viewability depends on physical storage and cannot be established from content
identity, so concatenate is not content-cacheable. Dense and View share checked
static shape inference and Whole source support. The continuation accumulates
owning input capabilities in a Root-accounted map across Need envelopes, while
each poll borrows its phase tensor map only for that call. Each Need contains at
most 64 original ports; the five-stage bound allows four Need envelopes and the
final publication for 256 inputs. Work and cancellation are charged during
mapping and copying. A failed publication exposes no partial output.

## Errors and acceptance

Preflight rejects fewer than two or more than 256 inputs, dtype/rank/non-axis
shape mismatch, invalid axis or layout, and output element count above 2^40.
Source, typed-validation, resource and cancellation failures preserve their
status categories. `ViewUnavailable` reports a physical View geometry failure;
it does not imply that Dense copying is invalid.

The public workflow test checks a same-owner affine view and Dense result,
independent owners, unselected input failures and escaped Result/window lifetime.
The existing `test_numeric_result_math` integration fixture separately contains
singleton-fragment stride inference, a negative global stride, reordered inputs
under partial demand, Empty demand and 65-/256-input repeated-reference cases.
That broader fixture was not rerun for this Result migration. The multi-envelope
cases should not be read as current `indexing.cpp` default-workflow coverage.
Neither test covers every dtype, owner arrangement or concatenate geometry. See
[NUM-10 Whole execution](../indexing-whole.md) for the current test entry and
evidence boundary.
