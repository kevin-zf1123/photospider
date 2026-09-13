---
spec_schema_version: 1
id: NUM-11F
parent_id: NUM-11
function: reduce_variance
proposed_operation_keys:
  - numeric.reduce_variance_strict
  - numeric.reduce_variance_accelerated_apple_silicon
  - numeric.reduce_variance_accelerated_x86_64
category: 01-numeric
kind: primitive
status: Proposed
document_maturity: D1_draft
implementation_status: target_contract_not_implemented
repository_branch: ops-specs
repository_commit: 30478d33
---

# NUM-11F: reduce_variance

Inherit the [NUM baseline](NUM_common_contract.md) for specification status,
registration, shared execution and acceptance requirements; explicit rules below
and in the named family contract take precedence.

Inherit the [reduction contract](NUM-11_reduction_contract.md) for input/values,
axes/keepdims, shapes, generic output, exact selected-group support, typed
validation, NaN mapping, resources, invalidation and ownership. Input supports
all four dtypes, output Float32/Float64. Required static String dtype is
float32/float64, default float64 in constructors. Required static Int64 ddof is
nonnegative, default 0 in constructors. Direct nodes provide both parameters.
Group size N is known from shape/axes and must exceed ddof.

For finite inputs, let mu=sum(x)/N exactly. Compute

    V = sum((x-mu)^2) / (N-ddof)
      = (N*sum(x^2)-sum(x)^2) / (N*(N-ddof))

as an exact mathematical rational, then correctly round V directly to output
dtype. No rounded intermediate mean, square or partial sum defines the result.
All three CPU profiles are bitwise equivalent. Integer sources participate
exactly without conversion to float. The exact numerator is nonnegative;
clamping a negative approximate variance to zero is not an accuracy proof.

## Nonfinite values and numeric range

First propagate the group's first NaN with the shared sign/payload conversion.
If no NaN exists but any input is infinite, return the fixed positive quiet NaN,
including all-equal infinities. Finite constant groups, including mixtures of
signed zeros, have variance +0. All generated non-NaN results are nonnegative.
Finite positive variance may round to +Inf or underflow to +0 successfully.
A singleton finite group with ddof=0 yields +0; a singleton infinity yields NaN.

Invalid ddof (negative or N<=ddof) fails compile/preflight with InvalidArgument,
FailureReason::InvalidDomain and diagnostic InvalidDegreesOfFreedom, before
input sample evaluation. Unsupported dtype/axes and other metadata violations
use the common errors. Generic nonfinite inputs are numeric outcomes, not Status
failures; typed input validation remains required for selected groups.

## Algorithms, resources and acceptance

Use exact moments or a certified equivalent with one final rounding. Account
sum/sum-of-squares accumulators, exact products/division and all temporary limb
capacity; do not allocate a full group to form deviations. Refinement that cannot
resolve destination rounding within available work/memory returns ResourceExhausted.
Apply shared block cancellation, exact group demand, owner/cache and publication
requirements. Parallel merging must preserve exact moments and logical NaN order.

Conceptual fixture: input=[1,2,3], axes="0", dtype="float64": ddof=0 yields
[RN_Float64(2/3)], ddof=1 yields [1]. Use an independent exact rational oracle;
include large common offsets with tiny differences, Int64 extrema, constant
inputs, subnormals, all nonfinite patterns, NaN payload conversion, N=1 and
invalid ddof. Source groups outside requested output must remain unobserved.

Exercise public WorkflowDocument execution plus shared strided/region/resource/
lifetime cases when implemented. Current numeric.variance uses rounded ordered
Float64 passes as documented in the shared contract. It does not establish this
multi-axis exact variance contract. No target runtime test is claimed here.
