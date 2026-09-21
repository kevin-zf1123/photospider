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
implementation_branch: numeric-optimize
implementation_base_commit: eb0e90c8
implementation_updated: 2026-09-21
---

# NUM-01: sample_expression

Numeric profile: strict retains the exact reference defined below. Floating
arithmetic in accelerated profiles follows the shared
[final FP32 four-ULP contract](NUM_accelerated_contract.md), including its
range/fallback rules. Discrete results, copies, selected endpoints and special
values remain exact.

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
index `i` in a nonempty Whole values request, compute its coordinate and the valid neighbor coordinates
`i-1` and `i+1` for this validation only. Reject equality with either neighbor
as insufficient sampling-coordinate precision. Neighbor function values are
not evaluated by the neighbor check. Whole values evaluates all N samples
before projection; axis-only performs no coordinate-distinctness scan. The same rule applies to ascending and descending intervals.

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
- `accelerated`: platform-specific SIMD implementations with a final-output
  bound of four FP32-scaled ULP under the shared acceleration contract, including
  Float64 outputs. The reference is the complete strict stepwise RN64 AST.
  Propagate conservative enclosures through each node and final dtype conversion.
  Reject an uncertain domain, zero-denominator or overflow decision and replay
  the affected sample strictly. Internal FMA or mixed precision is permitted
  only when the enclosure still covers the strict reference.

An accelerated mathematical step that cannot guarantee its error bound for the
actual operands may use strict evaluation. Report the selected accelerated
platform and strict-step fallback. A call to a platform-specific accelerated key
on a mismatched platform returns `BackendUnavailable`; it does not silently
select another operator. Cancellation, allocation/work failure and upstream
failure are not capability fallback conditions and retain their original errors.
Whole callbacks retain their explicit operation/profile identities but do not
expose per-atom strict-call or fallback counters. Such counters are unavailable;
do not infer them from successful output count. Numerical core diagnostics may
be collected by a separate targeted driver without changing public outputs.

The final output uses the shared FP32-scaled bound. The reference consumes the
original input bits and applies each strict RN64 step. Sensitive expressions
such as `1/(exp(x)-a)` preserve strict failure classification: uncertainty
triggers strict replay of the affected sample.

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

## 6. Whole execution

A nonempty values request collects all active scalar inputs and computes all N
samples before projecting the immutable owner to the requested global indices.
Every coordinate and expression must satisfy sections 2 and 4. A positive-only
projection of `ln(x)` therefore fails if another coordinate in the full domain
is zero. No partially successful values output is published.

Axis remains independently requestable. Syntax validates statically, but axis
runtime skips coefficients, expression evaluation and coordinate-distinctness
scanning. Both outputs exclude end at runtime for count=1; its metadata still
validates during compilation. Constant expressions still validate their interval,
and algebraically cancelled coefficients still participate in values input reads.

| Selected output | Complete runtime input set | Validation/computation |
| --- | --- | --- |
| values, N>=2 | start/end and all named coefficients | finite scalars, interval and all N coordinate/expression results |
| values, N=1 | start and all named coefficients | start/coefficient finite, one expression result |
| axis, N>=2 | start/end | finite interval and representable nonzero step |
| axis, N=1 | start | finite start, `[start,start,+0]` |
| Empty | none | static metadata and parameter checks only |

### Invalidation, mapping, ownership and cache

An active endpoint change invalidates all observations of the selected output.
A coefficient edit invalidates all values and never axis; count=1 end edits
invalidate neither output. Static projections enter compiled identity. Transitive
upstream witnesses remain authoritative even if numbers coincide after an edit.

Values retain their `[N]` sample identities and axis retains one trailing-axis
Atomic tuple. Callbacks produce complete packed owners; consumers receive their
requested global Region without rebasing coordinates. Both outputs have empty
facets. Partial requests own count*4/8 bytes for values or 24 bytes for axis, plus
fixed arithmetic workspace. Published storage survives invocation/context lifetime;
unpublished output and scratch release on failure/cancellation. Outputs have no
Result ObjectId association; consumers must connect the intended values/axis pair.

Scalar inputs accept legal offset, signed/zero-stride and unaligned layouts.
Cache identity retains exact parameters, metadata and active input witnesses.
Profiles remain separate. Cache-off preserves ownership and arithmetic. Numeric
failure uses Domain/Run with no Atom, retaining failing global sample, x and AST
span text. Upstream, capacity/work, cancellation and stale status retain their
original categories. Failure is limited to the selected Whole output; an axis
success does not certify values success.

### Reference algorithms, resources and cancellation

Compile the bounded grammar once into an immutable AST with canonical name
binding and source spans for diagnostics. Coordinate computation uses bounded
exact binary-rational arithmetic or an equivalent implementation that proves
the same RN64 result. Validate active scalars once, then per sample compute x and
neighbor coordinates, then evaluate the AST in the specified order into a
Float64 scratch table and convert the final value once.

Strict mathematical functions require a correctly rounded implementation.
One permissible reference strategy refines outward-rounded high-precision bounds
until both bounds round to the same Float64 result; exact boundary cases require
their own resolution. Fixed high precision or an unchanged residual alone is not
a correct-rounding proof. Refinement work and storage remain budgeted and may
fail ResourceExhausted rather than guess a rounded value.

Accelerated CPU implementations can evaluate all N independent samples in
SIMD lanes and scheduled batches. Each platform publishes its concrete math
implementation/version, supported operand domains, error justification and
strict fallback predicates. Unverified argument domains use strict evaluation.
SIMD tails must not evaluate indices beyond N. No private thread
pool or unaccounted persistent per-run cache is introduced.

Let N be the complete sample count, A<=256 AST nodes, K<=256 coefficients, and b=4 or 8
output bytes. The ordinary expression traversal is O(K+N*A); coordinate
and strict-math precision/refinement costs are separate measured and budgeted
terms. Excluding external consumers, plan storage and upstream ownership:

| Storage/work | Bound or accounting rule |
| --- | --- |
| Whole values payload | `b*N`; full output at maximum N is 4 MiB or 8 MiB |
| Axis payload | 24 bytes if requested |
| Scalar/evaluation scratch per active sample or admitted lane | `8*(K+2)+8*A <= 4112` bytes, plus coordinate state |
| Shared packed input scalars | At most `8*(K+2)` bytes when deduplicated; no sharing is required for correctness |
| Coordinate arithmetic | Charge every actual temporary capacity; bounded by binary64 operand exponents and the count bound |
| Correct-rounding fallback | Charge actual limb buffers and every refinement/primitive work unit to the existing root budget |
| Scheduler/stage/certificate metadata | Existing host limits apply; not included in the payload-only numbers above |

Reserve before allocation, account old/new buffers simultaneously when growing,
and bound live batch width by the admitted resources. No temporary disk backing
or persistent runtime state is required by this Value operator. Every nonempty values projection requires complete output capacity, so a small
ROI cannot avoid that payload admission. No universal RSS bound is claimed.
There are no per-sample transport stages or certificates. Host limits never
permit silently truncating count, expression or coefficient set.

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

Numeric failure has Domain origin and Run scope for the selected Whole output.
Diagnostics retain the failing global sample index and exact-round-trip x when
available, together with AST source span/function or input name. Pre-coordinate
errors state x unavailable. No partial values are published. Bounded status text
may truncate detail; structured origin/scope remains authoritative.

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
node's outputs and dynamic executions. Direct invocation without a handle prepares once; execution reuses the sealed
plan owner without reparsing. Static preparation storage is outside runtime scratch admission;
its source/node bounds are explicit. There is no global AST cache.

Runtime coordinates use the exact NUM-02 limb machinery, with NUM-01's stricter
interval and neighbor validation. Mixed Float32 inputs widen by IEEE fields,
and final Float32 conversion rounds once. The strict evaluator uses the shared exact
and certified mathematical backend at each Float64 primitive. Every intermediate
is checked before its parent executes. Accelerated evaluation batches four consecutive samples and propagates RN64
reference enclosures through the AST, using SLEEF 3.9.0 on admitted domains.
Rejected samples replay through the strict evaluator when the final bound is not certified. See
[mathematical implementation](../math-implementation.md) for rounding proofs and
fixed-refinement resource limits.

Static preparation sets the values input projection to all active scalars and
axis to its active endpoints. Both outputs execute CPU Whole callbacks; no
per-index Need, continuation or custom dependency pieces remain. Axis allocates
its smaller exact-coordinate state without the transcendental arena; the common
workspace admission reserves the maximum values state. Work/capacity/cancellation
checks remain within exact arithmetic and before publication.

Package 0.17 and OperationTraits15 supply sealed synchronous preparation and
Whole tuple/static projection support; C ABI9, document schema2 and canonical
framing14 are unchanged. C++ consumers must rebuild for the package boundary.
The old positive-step SampledSignal consumer remains a separate legacy contract;
new sampling-aware LUT consumers connect the explicit values and axis ports.
Specification acceptance remains Proposed.

## 9. Acceptance contract

Strict acceptance uses an independent correctly rounded per-primitive oracle,
including exact rational sampling coordinates and literal conversion. Compare
strict result bits, metadata/axis bits and the first failing AST span in the
specified evaluation order. A fixed-precision approximation with no rounding
resolution is insufficient for hard-to-round fixtures.

For accelerated expressions, compare the final output with the independent
strict stepwise reference under the shared FP32-scaled bound. Primitive checks
are additional diagnostics, not a replacement for final-error acceptance.
Test exact zeros, sensitive cancellation and subnormal boundaries explicitly.

| ID | Fixture and independent expected behavior |
| --- | --- |
| T01 | `2*x+1`, start=0, end=1, N=5: values `[1,1.5,2,2.5,3]`, axis `[0,1,0.25]` |
| T02 | Same expression, start=1, end=0: values `[3,2.5,2,1.5,1]`, axis `[1,0,-0.25]` |
| T03 | `x*x`, N=1, start=2: values `[4]`, axis `[2,2,0]`; a failing end producer is never invoked |
| T04 | `a*x+b`, mixed Float32/Float64 scalar ports; reuse one compiled plan with a=2,b=1 then a=3,b=-1; check both runs and unchanged axis |
| T05 | `x*x`, interval [0,1], N=3: `[0,0.25,1]`; hypothetical linear table query .25 gives .125, whereas direct continuous evaluation gives .0625 |
| T06 | N=2 exact endpoint bits; extreme opposite endpoints with N=3 avoid intermediate subtraction overflow; an unrepresentable derived step fails explicitly |
| T07 | start=1, end=nextafter(1,+inf), N=3: coordinate rounding creates adjacent equality and the complete values request fails; axis-only request does not scan values |
| T08 | `ln(x)`, [0,1], N=3: any nonempty values request fails at sample=0, x=0 with Run scope; axis-only execution reads no coefficient/function data |
| T09 | Both dtypes, final Float32 overflow, subnormal/zero conversion, signed-zero rules and caller rounding-mode restoration |
| T10 | Precedence examples, `0^0=1`, exp(0), ln(1), sin(0), cos(0), unary nesting, exact AST/source limits and one-past-limit rejection |
| T11 | Missing/extra/duplicate/reserved coefficient names, zero coefficients, wrong shape/dtype, malformed syntax and direct/compiled parity |
| T12 | `1/0`, `sqrt(-1)`, `ln(0)`, `(-1)^0.5`, `min(exp(1000),1)`; verify code/reason/source span and no successful failing sample publication |
| T13 | Nonzero ROI and disjoint indices equal corresponding full-result bits within one numeric profile; SIMD tails preserve the full-domain result before projection |
| T14 | Dirty/cache tests: coefficient changes affect values only; N=1 end changes affect neither; N>=2 endpoint changes affect both; profile changes cannot reuse another profile's numeric entry |
| T15 | Small ROI requires full values capacity; rejected allocation/work limits; cancellation during AST and strict refinement; all unpublished ownership released |
| T16 | Cache-off, multiple consumers, context destruction with live output Values, and release by the final owner |
| T17 | Strict bit equality on supported Apple Silicon and x86-64 builds; accelerated final FP32-scaled ULP checks, input-domain fallback, wrong-platform BackendUnavailable and unavailable Whole counters |
| T18 | Sensitive `1/(exp(x)-a)` boundary: require strict-equivalent failure classification and final-error acceptance |

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
AST, dtype, requested M, cache state, public counters N/A or separate core fallback counts, admitted peak resources
and timing distribution. Compare against strict on polynomial and transcendental
fixtures at N=256, 65536 and 1048576, including small ROIs. No platform speedup
claim is accepted without measurement on that platform; a numeric success alone
does not demonstrate acceleration.

### Actual validation

The public `photospider_numeric_expression` and `photospider_numeric_prepared`
manual targets cover mixed bindings, plan reuse, ascending/descending/singleton
axes, sparse/dirty/cache behavior, exact spans, invalid schemas/names, unused
failing producers, work/cancellation/capacity failures, caller fenv, strided
storage, Whole Run failures, unpublished-owner release and retry. They are excluded from default builds and have no CTest/integration
registration. The independent Python oracle combines exact rational coordinates,
stepwise integer/Fraction rounding and MPFR mathematical enclosures. Platform
results, native timing and remaining limits are recorded in
[the implementation notes](../math-implementation.md).

## 10. Related requirements

- [Numeric category and NUM-01](../core.md).
- [Curve and LUT category](../curves.md).
- [Operator specification template](../../00-foundation/spec-template.md).
- [Common data and execution contracts](../../00-foundation/contracts.md).

Whole migration checks and performance: [expression Whole](../expression-whole.md).
