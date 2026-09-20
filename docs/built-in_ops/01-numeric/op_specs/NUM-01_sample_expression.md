---
spec_schema_version: 1
id: NUM-01
function: sample_expression
operation_family: numeric.sample_expression
proposed_operation_keys:
  - numeric.sample_expression_strict
  - numeric.sample_expression_accelerated_apple_silicon
  - numeric.sample_expression_accelerated_x86_64
category: 01-numeric
kind: primitive
status: Proposed
spec_revision: 0.2.0
document_maturity: D1_draft
implementation_status: implemented
verification_status: public_workflows_and_independent_oracles
repository_branch: ops-specs
repository_commit: 30478d33
---

# NUM-01: sample_expression

Inherit the [NUM baseline](NUM_common_contract.md) for specification status,
registration, shared execution and acceptance requirements; explicit rules below
and in the named family contract take precedence.

This specification records the selected expression contract. Its Proposed status
is independent of the maintained implementation and validation recorded below.

## 1. Confirmed purpose

Generate numeric inputs for other workflow operations by sampling a real-valued
function of one variable. The initial requirement names polynomial, trigonometric,
exponential and natural-logarithm expressions, a sampling interval, and a
one-dimensional array output. LUT construction is an explicit use case.

Confirmed scope: evaluate one scalar function `y = f(x)` on a uniform sampling
axis and return one rank-1 numeric array `[N]` as the main output. A separate
named axis output carries sampling information so downstream consumers can
recover the independent-variable coordinates.
Multi-function/channel output and an arbitrary query-position input are outside
this operator's scope. Output dtype and axis transport are specified in section 4.

The separately requested control-point curve generator receives its own operator
specification after this operator is clarified. The maintainer clarified the
original `spine` wording as a one-dimensional `y=f(x)` adjustment curve. Geometric
paths, path parameterization and skeletal animation are outside this requirement.

## 2. Confirmed sampling contract

Sampling is specified by `start`, `end`, and integer `count = N`. Both endpoints
are included. The supported count is `1 <= N <= 1048576`. `count` must be
specified explicitly; no domain-dependent inference or floating-point loop
determines the output length. For `N >= 2`, the mathematical sampling rule is

```text
step = (end - start) / (N - 1)
x_i = start + i * step,  i = 0, ..., N - 1
y_i = f(x_i)
```

`step` is derived rather than independently supplied. The target API replaces the
current `start/step/count` parameterization with `start/end/count`.

For `N = 1`, return `[f(start)]` and ignore `end` when choosing the sample
coordinate. There is no requirement that `end == start`. Emit
`axis = [start, start, 0]`. Do not request or numerically validate the `end`
payload or execute its upstream producer solely for this generator. An invalid
or failing unused `end` producer cannot fail this generator's requested outputs.
Static graph/schema validation still checks the declared end edge, following
the binding convention in section 5. The two-endpoint rule applies to `N >= 2`.

For `N >= 2`, both `start < end` and `start > end` are supported. Samples follow
the supplied direction; a descending interval has a negative derived step.
Equal endpoints are excluded for `N >= 2` by the selected interval policy.
Reversing the interval changes sample order and sampling metadata, not the
definition of `f`.

The coordinate contract is common to strict and accelerated versions. Let
`RN64` denote round-to-nearest/ties-to-even to Float64. Treat the input Float64
endpoints as exact binary rational numbers when applying this definition:

```text
d = (exact(end) - exact(start)) / (N - 1)
axis.step = RN64(d)
x_0 = start
x_(N-1) = end
x_i = RN64(((N-1-i)*exact(start) + i*exact(end)) / (N-1)),  0 < i < N-1
```

This specifies rounding of coordinates, not an expression-language operation
sequence. An implementation must avoid intermediate subtraction/product
overflow in these definitions. The rounded `axis.step` must be finite and
nonzero with the declared direction for `N>=2`; otherwise sampling fails.
An interior coordinate is derived from both endpoints and its global index,
not by accumulating the rounded step. Consumers reconstructing the same grid
must use both endpoint fields and N; `start+i*axis.step` is not the exact rule.
Singleton coordinates preserve the supplied start, and endpoint coordinates
preserve endpoint bits, including a signed zero where the interval is legal.

Adjacent sampled coordinates must remain distinguishable in Float64. For each
requested index `i`, compute its coordinate and the valid neighbor coordinates
`i-1` and `i+1` for this validation only. Reject equality with either neighbor
as insufficient sampling-coordinate precision. Neighbor function values are
not evaluated. No scan of unrequested coordinates elsewhere in the domain is
required. The same rule applies to ascending and descending intervals.

## 3. Dynamic workflow inputs

Coefficients, `start`, and `end` must accept upstream numeric outputs and may
change between executions of the same compiled workflow. The expression
structure and `count` remain static for this design; output shape stays `[N]`.
Each coefficient has a name and its own upstream scalar connection. For example,
`a*x+b` binds `a` and `b` independently, allowing separate upstream statistics
outputs to drive them. An indexed coefficient-array interface is outside this
selected design. Binding conventions are defined in section 5.

Every numeric input (`start`, `end`, and each coefficient) accepts a Float32 or
Float64 Value with exact shape `[1]`. Ports may use different floating dtypes.
Float32 values widen exactly to Float64 before arithmetic. Integer inputs
require an explicit upstream cast; there is no implicit integer conversion.
Use the existing generic Value port validation for any attached input facets;
recognized semantic descriptors and actually read typed data remain validated.
Facets do not participate in unit inference and are not propagated to outputs.
Valid immutable signed-stride, offset and unaligned `[1]` views are supported.

Sampling coordinates, generated values, and sampling-axis metadata must reflect
the current execution's inputs together. Reusing a previous run's origin or
step after changing the interval is invalid. Fixed shape alone does not make
sampling metadata static. Exact metadata transport and downstream validation
must obey the named-output contract in section 4.

## 4. Numeric representation

The output dtype is a static choice of `Float32` or `Float64`, defaulting to
`Float64`. Expression evaluation uses Float64 in both modes; Float32 output
requires a final conversion. All evaluated intermediates and final output
samples must be finite. Invalid function domains, division by zero and numeric
overflow fail this evaluation, with the sample index, sampling coordinate and
failing subexpression identified. No partial successful array is returned and
there is no automatic clipping or NaN/Inf preservation mode. This also applies
when Float64 evaluation is finite but the selected Float32 result overflows.

For example, `ln(x)` at zero and `1/x` at zero fail. A failing intermediate in
`min(exp(1000), 1)` cannot be hidden by the outer minimum. Every requested input
scalar must be finite. Gradual underflow is enabled: subnormals and a correctly
rounded zero are allowed, with no flush-to-zero mode. Float32 narrowing uses
round-to-nearest/ties-to-even. Restore the caller's floating-point environment.

Power uses the selected convention `0^0 = 1`. A negative base with a noninteger
exponent is outside the real domain; a zero base with a negative exponent also
fails. These checks apply to the actual Float64 operands. The convention keeps
`x^0` equal to one at zero, without suppressing failures in either operand's
evaluated subexpression.

### Strict and accelerated versions

The selected family has a strict version and independently named accelerated
versions for each supported platform. The keys in this draft are
`numeric.sample_expression_strict`,
`numeric.sample_expression_accelerated_apple_silicon`, and
`numeric.sample_expression_accelerated_x86_64`.
Both accelerated versions are CPU implementations; SIMD and multithreading are
permitted. Metal/GPU execution is outside the selected platform scope.
There is no target generic accelerated dispatcher key or runtime `mode` switch.
The existing unsuffixed key is recorded only as a current implementation fact;
it is not a selected compatibility alias for the new target interfaces.

All versions share the selected sampling, binding, output and failure contracts
unless an explicit numerical-profile difference is documented.

- `strict`: fixed stepwise Float64 round-to-nearest/ties-to-even, including
  correctly rounded mathematical functions, giving identical result bits across
  supported CPU implementations. Each primitive consumes already rounded
  Float64 operands. This is not exact-real evaluation of the complete expression
  followed by one final rounding; `(a+b)-a` retains both arithmetic roundings.
- `accelerated`: platform-specific implementations. The selected
  accelerated bound is: each `sin`, `cos`,
  `tan`, `exp`, `ln` or power result is within four representable Float64 steps
  of its correctly rounded Float64 reference. Composite expression acceptance
  propagates the per-step bounds rather than applying a universal absolute
  tolerance. Basic arithmetic retains the strict operation order and Float64
  rounding; SIMD across samples and scheduling across requested samples must not
  introduce expression reassociation, FMA contraction or Float32 intermediates.

An accelerated mathematical step that cannot guarantee its error bound for the
actual operands may use strict evaluation. Report the selected accelerated
platform and strict-step fallback. A call to a platform-specific accelerated key
on a mismatched platform returns `BackendUnavailable`; it does not silently
select another operator. Cancellation, allocation/work failure and upstream
failure are not capability fallback conditions and retain their original errors.
The implementation must expose an aggregate per-node execution diagnostic
containing operation key, concrete numeric profile/version, platform/ISA path,
number of strict mathematical calls, and fallback function/reason counts.
Reports describe work actually performed; a result-cache hit does not invent
new strict calls. This is execution diagnostics, not a third data output. The
current callback/reporting API adaptation is an explicit implementation dependency.

Four representable steps are measured by ordered finite-value ULP distance,
including near zero; this is not four times a relative epsilon at all values.
The strict mathematical reference evaluates each function at its actual rounded
Float64 operands, not at an idealized unrounded expression input.

Each profile makes domain, zero-denominator and overflow decisions from its
own actual intermediate operands. The maintainer explicitly permits different
success/failure results near numerically sensitive boundaries: for example,
`1/(exp(x)-a)` can fail in strict and succeed in accelerated when `a` equals
the strict rounded exponential. The four-ULP bound is per function call, not
a bound on the final expression relative to strict and not a shared failure
predicate. At identical operands, a domain error or nonfinite reference result
must fail; the approximation allowance cannot turn it into a finite success.

For deterministic signed-zero behavior, `sin(-0)`, `tan(-0)` and `sqrt(-0)`
return `-0`; `cos(±0)=1`, `exp(±0)=1`, and `ln(1)=+0`. A zero base to a positive
power returns `-0` only for a negative-zero base and an odd integer exponent,
and `+0` otherwise. Basic arithmetic uses nearest-even signed-zero rules.
`tan` receives the actual Float64 angle; there is no epsilon-based pole test
around a rounded approximation of pi/2.

The dtype default is supplied by the authoring constructor, which writes
`dtype="float64"` explicitly. Direct WorkflowDocument nodes supply dtype;
the target does not depend on implicit registry insertion of this default.
Scalar input dtype acceptance and widening are specified in section 3.

### Named outputs

The selected transport uses two independently connectable Value outputs:

| Name | Shape | Meaning |
| --- | --- | --- |
| `values` | `[N]` | Samples `f(x_i)` in the selected Float32/Float64 dtype |
| `axis` | `[3]` | `[start, end, step]` for `N >= 2`; `[start, start, 0]` for `N = 1` |

Both shapes are known at compilation. Runtime sampling coordinates are payload
data in `axis`, rather than run-varying Value facets. Numeric consumers can
connect only `values`; sampling-aware consumers connect both outputs.
The axis dtype is always Float64. The zero singleton step describes the lack
of a sample interval and is not fed to the existing positive-step facet validator.

Both outputs are generic numeric arrays with empty facets. The generator does
not infer or propagate pixel, time, arc-length, color, width or other physical
units. Interpretation belongs to the workflow author and consuming operation.
Trigonometric function arguments are interpreted as radians. No unit labels or
dimensional-analysis mode are included.

## 5. Expression language

The selected language is a bounded pure mathematical expression, with one
independent variable `x` and separately bound named scalar coefficients.

| Group | Selected syntax/capability |
| --- | --- |
| Arithmetic | Addition, subtraction, multiplication, division, power, parentheses and unary signs |
| Constants | `pi`, `e` and numeric literals |
| Trigonometry | `sin`, `cos`, `tan`, with arguments in radians |
| Exponential/logarithm | `exp`, `ln` (natural logarithm) |
| Other functions | `sqrt`, `abs`, `min`, `max` |

Comparisons and conditional branches are outside this version. There is no
script execution, loop, file I/O, implicit time input or randomness.

### Grammar and binding conventions

- Source is nonempty and at most 4096 bytes, with at most 256 AST nodes, AST
  height 32 and 256 distinct named coefficients. A leaf has height one.
  Parentheses and whitespace do not add AST nodes; unary signs do.
- Zero coefficients are legal: `x^2`, `sin(x)` and `3` require no dummy input.
- Numeric literals use decimal/scientific syntax such as `.5`, `1.`, `2e-3`.
  Convert once with RN64; reject overflowing literals, hexadecimal literals,
  NaN/Inf names, implicit multiplication and locale-dependent separators.
- Identifiers are case-sensitive ASCII `[A-Za-z_][A-Za-z0-9_]*`, at most 63
  bytes. `x`, `pi`, `e`, function names and interface names `start`, `end`,
  `count`, `dtype`, `values`, `axis`, `coefficient_names` are reserved.
- Free identifiers identify coefficient inputs. An explicit static
  `coefficient_names` list must match these identifiers exactly, without
  duplicates or unused bindings. The authoring constructor derives and sorts
  this list bytewise. A canonical space-separated String encodes it in the
  WorkflowDocument parameter; the empty string represents zero coefficients.
- Ordered numeric inputs are `start`, `end`, then coefficients in that list
  order. The authoring API presents the coefficients as named connections.
  Both endpoint edges are statically present; with N=1 the `end` edge has no
  runtime demand. A singleton convenience constructor can connect `start` to
  both endpoint slots without computing a separate value.
- `ln` is the sole natural-log spelling in the target language; no `log` alias
  or indexed `c[...]` interface is retained in these new keys.
- Function calls require parentheses. `min`/`max` have two arguments; the other
  selected functions have one. Constants `pi` and `e` are correctly rounded
  Float64 constants in both numeric profiles.
- Power uses `^`, is right-associative and binds above unary signs; then follow
  `* /`, then `+ -`, with left associativity for the latter two levels.
  Thus `-2^2=-4`, `2^-2=0.25`, `2^3^2=512`.
- Evaluate the AST in left-to-right postorder. Do not remove a potentially
  failing subtree through algebraic simplification. `min`/`max` evaluate both
  arguments. Mixed signed-zero ties return -0 for min and +0 for max;
  same-sign zeros retain that sign, matching NUM-05 minimum/maximum. Other
  numerical ties return the common value; `abs(-0)` is `+0`. This does not
  relax the expression generator's finite-only input/intermediate contract.
  A mathematical-function approximation to an exact zero must preserve the
  strict zero result and sign; the ULP allowance does not authorize replacing
  a mathematical zero by a nonzero subnormal.

### Static parameter table

| Name | Type and domain | Default |
| --- | --- | --- |
| `expression` | String, grammar and limits above | Required |
| `count` | Int64 in `[1,1048576]` | Required |
| `dtype` | String `float32` or `float64` | Constructor writes `float64`; direct nodes specify it |
| `coefficient_names` | Canonical String of sorted free identifiers | Constructor derives it; direct nodes specify it |

No count, endpoint or coefficient value is implicitly synthesized by the
registry. Changing expression, names, count or dtype requires compilation;
changing numeric input values does not.

## 6. Demand-driven evaluation

`values` supports requested index sets. Evaluate only requested samples, using
their global indices in the full `[N]` domain. A request for a positive-coordinate
subset of `ln(x)` may succeed even when the complete domain contains zero;
requesting the zero-coordinate sample fails that observation.

Strict numeric failure in section 4 applies to evaluated expressions. It does
not create a full-array validation scan. An assembled array request containing
a failing sample does not become a partially successful array; independent
observation outcomes follow the kernel's applicable execution API.

`axis` is independently requestable and does not evaluate the expression.
Expression syntax remains statically validated. Its runtime data and validation
demands include the sampling interval, but no coefficient payloads. For `N=1`,
neither output requests the ignored `end` payload.

### Per-port support and invalidation

All nonempty `values` requests validate finite start, and, for N>=2, finite end,
unequal endpoints and a finite nonzero derived step. This remains true when
the expression does not use x. Each requested sample also validates its own
coordinate separation as defined in section 2.

| Requested output | Data/control support | Validation support | Not requested |
| --- | --- | --- | --- |
| `values[i]`, N>=2 | Start/end where used to form x; each declared coefficient evaluated by the AST | Start/end, derived-step conditions, local coordinate neighbors, read coefficient semantics and finite values, all evaluated AST results | All other function samples |
| `values[0]`, N=1 | Start where x is used; declared coefficients | Start and read coefficient semantics/finite values, evaluated AST results | End payload and its producer |
| `axis`, N>=2 | Start/end | Finite start/end, unequal endpoints, finite nonzero derived step | Coefficient payloads, function samples, full coordinate-distinctness scan |
| `axis`, N=1 | Start | Finite start | End, coefficients and function samples |
| Empty request | None | Static graph, parameter and schema checks only | All runtime inputs |

Output descriptors depend on static count and dtype; axis shape/dtype are fixed.
Input descriptor/schema compatibility is checked at compilation even for an
undemanded payload. Values' per-observation source sets are exact for the
specified evaluation and validation protocol; algebraic cancellation does not
erase an evaluated operand's validation dependency.

Changing a coefficient can invalidate every `values` observation, but never
`axis`. Changing start, or changing end for N>=2, invalidates both outputs
through their data or validation support. Changing end for N=1 invalidates
neither output. The host preserves the upstream transitive witness; equal
result numbers do not erase a changed validation dependency. Static changes
produce a new compiled identity.

`values` uses per-index dependency execution, without a blanket Whole source
fallback. `axis` is a Whole three-element Value, independent of values. A
nonempty request to any axis component may materialize the full 24-byte tuple.
Joint execution may share interval and scalar transport while preserving each
output's own demands and failures. Axis-only success does not certify that all
N function samples or all adjacent coordinates are valid.

### Returned mapping, lifetime and cache

Requested `values` indices stay in the original `[N]` logical coordinate system;
nonzero requests are not rebased or renormalized. Publish packed owned fragments
with their global Region/storage origin and element-size stride, covering only
requested values. The full-array collection path packs N samples in order.
Axis is a packed Float64 `[3]` Value at origin zero. Both outputs have empty
facets and no implicit missing samples or ExplicitZero fill.

All published bytes are immutable and retain allocator ownership after the
invocation or ExecutionContext ends. Cancellation and failure release unpublished
storage. The two generic outputs have no Result ObjectId association: sampling
consumers must connect values and axis from the intended generator and frozen
input bundle; equal shape alone cannot validate an arbitrary pairing.

Semantic cache identity includes the selected operation/profile version, static
parameters, exact input descriptors and observed input bits. Accelerated
implementation/library/ISA selection that can change value bits is part of
that profile identity. Strict and accelerated entries are not interchangeable.
Execution chunk size, sample scheduling and SIMD width must not affect results
within a fixed accelerated profile. Failed observations are not successful cache
entries. Cache-off retains active output ownership and does not alter semantics.

### Reference algorithms, resources and cancellation

Compile the bounded grammar once into an immutable AST with canonical name
binding and source spans for diagnostics. Coordinate computation uses bounded
exact binary-rational arithmetic or an equivalent implementation that proves
the same RN64 result. Per sample, validate required scalar inputs, compute x and
neighbor coordinates, then evaluate the AST in the specified order into a
Float64 scratch table and convert the final value once.

Strict mathematical functions require a correctly rounded implementation.
One permissible reference strategy refines outward-rounded high-precision bounds
until both bounds round to the same Float64 result; exact boundary cases require
their own resolution. Fixed high precision or an unchanged residual alone is not
a correct-rounding proof. Refinement work and storage remain budgeted and may
fail ResourceExhausted rather than guess a rounded value.

Accelerated CPU implementations can evaluate independent requested samples in
SIMD lanes and scheduled batches. Each platform publishes its concrete math
implementation/version, supported operand domains, error justification and
strict fallback predicates. Unverified argument domains use strict evaluation.
SIMD tails must not evaluate extra unrequested expressions. No private thread
pool or unaccounted persistent per-run cache is introduced.

Let M be demanded values, A<=256 AST nodes, K<=256 coefficients, and b=4 or 8
output bytes. The ordinary expression traversal is O(M*(A+K)); coordinate
and strict-math precision/refinement costs are separate measured and budgeted
terms. Excluding external consumers, plan storage and upstream ownership:

| Storage/work | Bound or accounting rule |
| --- | --- |
| Demanded values payload | `b*M`; full output at maximum N is 4 MiB or 8 MiB |
| Axis payload | 24 bytes if requested |
| Scalar/evaluation scratch per active sample or admitted lane | `8*(K+2)+8*A <= 4112` bytes, plus coordinate state |
| Shared packed input scalars | At most `8*(K+2)` bytes when deduplicated; no sharing is required for correctness |
| Coordinate arithmetic | Charge every actual temporary capacity; bounded by binary64 operand exponents and the count bound |
| Correct-rounding fallback | Charge actual limb buffers and every refinement/primitive work unit to the existing root budget |
| Scheduler/stage/certificate metadata | Existing host limits apply; not included in the payload-only numbers above |

Reserve before allocation, account old/new buffers simultaneously when growing,
and bound live batch width by the admitted resources. No temporary disk backing
or persistent runtime state is required by this Value operator. A full output
may fail the budget while a small ROI succeeds. No universal RSS bound is claimed.
Per-observation discovery reads only required scalar ports; poll input requests
in batches respecting the host's limit. Limits do not permit silently truncating
count, the expression or the coefficient set.

Poll cancellation before input work, at least every 32 AST nodes, between
coordinate/mathematical refinement steps and before publication. SIMD/batch size
must preserve this bounded polling obligation. Host read/allocation/work/stage
failures are sticky and are never converted into an accelerated fallback success.

### Error contract

| Trigger | Phase and Status |
| --- | --- |
| Bad/missing static parameters, grammar, binding names or declared complexity limits | Compile/direct preflight; `InvalidArgument`, schema origin |
| Unsupported numeric input dtype/shape or invalid output inference | Compile/direct preflight; `TypeMismatch`; malformed recognized facets retain their existing validation status |
| Nonfinite demanded input, equal N>=2 endpoints, zero/unrepresentable step or duplicate adjacent coordinate | Request; `OperationFailed`, `InvalidDomain` or `ArithmeticOverflow` as applicable |
| Divide by ±0 or zero to a negative power | Requested AST evaluation; `OperationFailed` / `DivideByZero` |
| sqrt negative, ln nonpositive, negative-base noninteger power | Requested AST evaluation; `OperationFailed` / `InvalidDomain` |
| Nonfinite intermediate or final dtype overflow | Request; `OperationFailed` / `ArithmeticOverflow` |
| Wrong accelerated platform | Capability check; `BackendUnavailable` |
| Capacity/work/stage exhaustion | `ResourceExhausted` with the corresponding existing reason |
| Cancellation or stale input/plan | Preserve the existing cancellation/stale status and origin |
| Upstream failure | Preserve upstream node/input identity, code, reason and scope |

Values' local numeric failures identify the actual output atom. Shared interval
or input failures are not retrospectively widened to a domain that revokes
previous successes. Axis failures affect the requested axis output. Diagnostics
include the global sample index and exact-round-trip x when available, together
with the AST source span/function or input name. Domain failures before an x is
available say so instead of inventing one. Existing bounded diagnostic storage
may truncate explanatory text; structured origin and scope remain authoritative.

## 7. Legacy implementation comparison

The unsuffixed legacy key was inspected at `ops-specs@30478d33`. It remains a
separate interface; the maintained suffixed keys implement the contract above.

| Area | Current behavior |
| --- | --- |
| Registration | Default registry key `numeric.sample_expression`; CPU callback |
| Input | One generic, unfaceted Float64 `[K]` coefficient array; `1 <= K <= 256`; all coefficients must be finite, including unused entries |
| Parameters | Required static `expression: String`, `start: Float64`, `step: Float64`, `count: Int64`; no registry defaults |
| Sampling | Finite start, positive finite step, count in `[1,1048576]`; `x_i = fma(i, step, start)`; finite last coordinate, greater than start for count > 1 |
| Output | Packed Float32 `[count]`, SampledSignal metadata; dimensionless axis and values, with declared origin and step |
| Expressions | Literals, `x`, `c[index]`, parentheses, unary signs, `+ - * / ^`, `abs sqrt exp log sin cos min max` |
| Evaluation | Float64 intermediates; nonfinite intermediates and values outside finite Float32 fail |
| Execution | Whole input/output; output allocation plus 4096 bytes of allocator-owned scratch; cancellation during evaluation and before publication |
| Publication | One immutable Value on success; numeric failure does not publish a partial successful Value |

Primary implementation sources:

- [Registration and callback](../../../../plugins/ops/01-numeric/numeric_sample_expression.cpp).
- [Expression parser and evaluator](../../../../src/lib/plugin/expression.cpp).
- [Implemented expression/LUT contract](../../../kernel-architecture/Expression-and-LUT-Operations.md).
- [Public integration workflows](../../../../tests/integration/test_expression_operations.cpp).

## 8. Maintained implementation and numerical boundary

`plugins/ops/01-numeric/numeric_expression.cpp` registers all three selected
keys. `photospider/numeric/expression.hpp` provides `sample_expression_node`;
it validates the free-name/connection map and writes all four static parameters.
Zero coefficients use a fixed start prefix plus a repeated `[end,coefficients...]`
group. Pure metadata preparation accepts mixed floating scalar dtypes.

The bounded iterative parser retains left-to-right postorder, source spans and
canonical names. Decimal literals are converted from their complete token using
16384-bit exact ratio scratch, including a nonzero digit at the far end of a
4096-byte midpoint literal. This compile-time scratch is not per-sample storage.
A registry-sealed `PreparedOperation` owns one immutable AST across the compiled
node's outputs and dynamic executions. Direct invocation/joint preflight also
prepare once. Static preparation storage is outside runtime scratch admission;
its source/node bounds are explicit. There is no global AST cache.

Runtime coordinates use the exact NUM-02 limb machinery, with NUM-01's stricter
interval and neighbor validation. Mixed Float32 inputs widen by IEEE fields,
and final Float32 conversion rounds once. The evaluator uses the shared exact
and certified mathematical backend at each Float64 primitive. Every intermediate
is checked before its parent executes. Ordinary accelerated transcendental
steps currently use a reported strict fallback; this implementation supplies
strict bits without claiming a faster approximate backend. See
[mathematical implementation](../math-implementation.md) for rounding proofs and
fixed-refinement resource limits.

For at most 16 total scalar ports, the prepared values contract uses at most
three compact static pieces (first/interior/last). One session reads the required
scalars once and evaluates the requested coordinates into owned fragments.
Larger signatures retain bounded 16-port stages per Atom. Sixteen is an
implementation batching choice, not a host ABI limit. Both paths retain exact
Data/Validation roles and per-Atom failure delivery; `execute_atoms` isolates
samples, while ordinary regional failure publishes no partial result. Axis has
its own smaller continuation and never allocates the transcendental arena.
Per-session and Run work budgets both apply; large regional requests need
explicit fuel sufficient for all their mathematical work.

Diagnostics report actual strict mathematical-call attempts and an 8-by-4
function/reason fallback matrix. Failed attempts retain consumed work/counts;
cache hits add none. Controlled coordinator allocation failures return a
`ResourceExhausted` status and retire unpublished owners/flights, allowing a
subsequent small request in the same context.

Package 0.15 requires C++ consumers to rebuild. C++ OperationTraits/semantic
framing 14, C operation ABI 9, document schema 2 and provider ABI 1 remain.
The old positive-step SampledSignal consumer remains a separate legacy contract;
new sampling-aware LUT consumers connect the explicit values and axis ports.
Specification acceptance remains Proposed.

## 9. Acceptance contract

Strict acceptance uses an independent correctly rounded per-primitive oracle,
including exact rational sampling coordinates and literal conversion. Compare
strict result bits, metadata/axis bits and the first failing AST span in the
specified evaluation order. A fixed-precision approximation with no rounding
resolution is insufficient for hard-to-round fixtures.

For accelerated functions, compare each actual primitive operand/result pair
with the independently correctly rounded reference and enforce <=4 finite
representable-step distance. `sqrt` and basic arithmetic still require strict
rounding. Test exact zeros, extrema and subnormal boundaries explicitly.
Composite acceptance propagates outward-rounded allowed intervals through the
AST, splitting at domain boundaries/extrema where necessary; it is not a global
`atol/rtol` comparison with strict. Primitive checks are required even when a
composite enclosure becomes wide or crosses an allowed failure boundary.
Testing supplies measured evidence; it does not fabricate a CertifiedBound
QualityReport or prove an untested library domain.

| ID | Fixture and independent expected behavior |
| --- | --- |
| T01 | `2*x+1`, start=0, end=1, N=5: values `[1,1.5,2,2.5,3]`, axis `[0,1,0.25]` |
| T02 | Same expression, start=1, end=0: values `[3,2.5,2,1.5,1]`, axis `[1,0,-0.25]` |
| T03 | `x*x`, N=1, start=2: values `[4]`, axis `[2,2,0]`; a failing end producer is never invoked |
| T04 | `a*x+b`, mixed Float32/Float64 scalar ports; reuse one compiled plan with a=2,b=1 then a=3,b=-1; check both runs and unchanged axis |
| T05 | `x*x`, interval [0,1], N=3: `[0,0.25,1]`; hypothetical linear table query .25 gives .125, whereas direct continuous evaluation gives .0625 |
| T06 | N=2 exact endpoint bits; extreme opposite endpoints with N=3 avoid intermediate subtraction overflow; an unrepresentable derived step fails explicitly |
| T07 | start=1, end=nextafter(1,+inf), N=3: coordinate rounding creates adjacent equality and affected requested values fail; axis-only request does not scan values |
| T08 | `ln(x)`, [0,1], N=3: request indices {1,2} succeeds; index 0 fails with x=0; axis-only execution reads no coefficient/function data |
| T09 | Both dtypes, final Float32 overflow, subnormal/zero conversion, signed-zero rules and caller rounding-mode restoration |
| T10 | Precedence examples, `0^0=1`, exp(0), ln(1), sin(0), cos(0), unary nesting, exact AST/source limits and one-past-limit rejection |
| T11 | Missing/extra/duplicate/reserved coefficient names, zero coefficients, wrong shape/dtype, malformed syntax and direct/compiled parity |
| T12 | `1/0`, `sqrt(-1)`, `ln(0)`, `(-1)^0.5`, `min(exp(1000),1)`; verify code/reason/source span and no successful failing sample publication |
| T13 | Nonzero ROI and disjoint indices equal corresponding full-result bits within one numeric profile; SIMD tails, sample order and optional joint execution do not widen input demand |
| T14 | Dirty/cache tests: coefficient changes affect values only; N=1 end changes affect neither; N>=2 endpoint changes affect both; profile changes cannot reuse another profile's numeric entry |
| T15 | Small ROI under a budget too small for full values; rejected allocation/work/stage limits; cancellation during AST and strict refinement; all unpublished ownership released |
| T16 | Cache-off, multiple consumers, context destruction with live output Values, and release by the final owner |
| T17 | Strict bit equality on supported Apple Silicon and x86-64 builds; accelerated primitive ULP checks, input-domain fallback, wrong-platform BackendUnavailable and fallback diagnostics |
| T18 | Sensitive `1/(exp(x)-a)` boundary: verify each profile against its actual intermediates, permitting the selected success/failure difference |

All three operation keys require a public WorkflowDocument -> Compiler ->
ExecutionContext test, not only parser or callback unit tests. Use declared
input bindings and named output edges, request both full and regional results,
and inspect actual producer read counts and returned bytes.

### Public workflow example

The public helper and executable implement this DAG:

```text
start: Float32[1] = [0] ----+
end:   Float64[1] = [1] ---+
a:     Float64[1] = [2] --+|
b:     Float32[1] = [1] -+||
                        |||
numeric.sample_expression_strict
  expression = "a*x+b"
  coefficient_names = "a b"
  ordered inputs = [start, end, a, b]
  count = 5
  dtype = "float64"
  values -> numeric consumer or returned result: [1,1.5,2,2.5,3]
  axis   -> sampling-aware consumer or returned result: [0,1,0.25]
```

Changing only a to 3 and b to -1 yields `[-1,-0.25,0.5,1.25,2]` with the same
axis and plan. Repeat with the appropriate accelerated key on each target CPU.
The [editable public example](../../../../examples/numeric_workflow/README.md)
contains the constructor, execution commands and checked expected outputs.

Performance acceptance records hardware, OS/compiler, math library/profile, N,
AST, dtype, requested M, cache state, fallback counts, admitted peak resources
and timing distribution. Compare against strict on polynomial and transcendental
fixtures at N=256, 65536 and 1048576, including small ROIs. No platform speedup
claim is accepted without measurement on that platform; a numeric success alone
does not demonstrate acceleration.

### Actual validation

The public `photospider_numeric_expression` and `photospider_numeric_prepared`
manual targets cover mixed bindings, plan reuse, ascending/descending/singleton
axes, sparse/dirty/cache behavior, exact spans, invalid schemas/names, unused
failing producers, work/cancellation/stage/capacity failures, metadata failure
recovery, caller fenv, strided storage, multi-box ownership and isolated Atom
outcomes. They are excluded from default builds and have no CTest/integration
registration. The independent Python oracle combines exact rational coordinates,
stepwise integer/Fraction rounding and MPFR mathematical enclosures. Platform
results, native timing and remaining limits are recorded in
[the implementation notes](../math-implementation.md).

## 10. Related requirements

- [Numeric category and NUM-01](../core.md).
- [Curve and LUT category](../curves.md).
- [Operator specification template](../../00-foundation/spec-template.md).
- [Common data and execution contracts](../../00-foundation/contracts.md).
