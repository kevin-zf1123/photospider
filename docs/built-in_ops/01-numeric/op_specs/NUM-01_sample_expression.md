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
---

# NUM-01: sample_expression

This Proposed specification defines three explicit CPU profiles: strict, accelerated Apple Silicon and accelerated x86-64. The unsuffixed `numeric.sample_expression` has its own static start/step and indexed-coefficient interface. It is not an alias for this family.

The operation samples a real-valued function of one variable on an endpoint-inclusive grid. It returns named `values` and `axis` Results, usable independently by downstream numeric consumers or LUT authoring. Count, expression, coefficient names and output dtype are static; endpoint and coefficient payloads can change between Runs.

## Inputs and outputs

Inputs are ordered start, end, then named coefficients in bytewise sorted order. Each Result contains exactly one tensor member, under any schema ID or member key, with complete sample shape `[1]` and dtype Float32 or Float64. Dtypes may mix. Float32 widens exactly to Float64. Static checks cover every input even when its payload is excluded.

| Output | Schema and member | Shape and dtype |
| --- | --- | --- |
| values | photospider.tensor v1 / samples | ordinary `[count]`, Float32 or Float64 |
| axis | photospider.tensor v1 / samples | atomic Float64 `[3]`, `[start,end,step]` |

Count is required in `[1,1048576]`. The helper defaults dtype to Float64 and writes it explicitly; direct nodes supply the required string. Output sample axes inherit no input batch axes or physical units. Function arguments use radians where applicable.

## Sampling coordinates

For N=count>1, let input endpoint bits denote exact binary rational values. With RN64 meaning round-to-nearest, ties-to-even:

```text
d = (exact(end)-exact(start))/(N-1)
axis = [start,end,RN64(d)]
x_0 = start
x_(N-1) = end
x_i = RN64(((N-1-i)*exact(start)+i*exact(end))/(N-1))
y_i = f(x_i)
```

Intermediate subtraction/product overflow must not alter these formulas. Interior coordinates use both endpoints and their global index, without accumulating rounded axis.step. Both ascending and descending intervals are supported. Endpoints must differ for N>1. A selected axis requires a finite, nonzero rounded step with the corresponding sign. A selected values coordinate must differ from each valid adjacent coordinate. These numerical conditions are caller preconditions; runtime performs no neighbor scan or rejection guarantee.

For N=1, x=start and axis is `[start,start,+0]`. Neither output reads end's payload or executes its producer solely for this call. The end schema remains checked. For larger counts both endpoints are active even when only an endpoint values tile is selected. Only computed tile coordinates are evaluated; unselected expression samples impose no expression-domain requirement.

## Numeric evaluation

Active inputs, evaluated intermediates and computed outputs must be finite and within the applicable real mathematical domains as unchecked caller preconditions. Float32 outputs require a representable final narrowing. Underflow to subnormal or signed zero is valid. Domain violations have no guaranteed diagnostic, sample index or source-span report. Operational failures still prevent successful call publication.

Strict evaluates every primitive in left-to-right postorder with RN64 before its parent. The complete expression is not reassociated into one exact-real calculation: `(a+b)-a` retains both rounding boundaries. Decimal literals convert once from the complete token using RN64. Mathematical functions use correctly rounded primitives, with bounded directed refinement where necessary. Unresolved refinement returns ResourceExhausted rather than an uncertified value. Float32 output is one further RN conversion of the final Float64 result.

Accelerated profiles enclose the complete strict stepwise reference and apply the shared [final FP32-scaled four-ULP contract](NUM_accelerated_contract.md), including Float64 outputs. They process up to four selected samples together. A sample whose final bound cannot be accepted replays strictly. Internal interval/domain checks serve accuracy selection, not a general domain-validation contract. Unavailable platform keys return BackendUnavailable; they do not select another operation key. Arithmetic normalizes and restores floating modes and exception flags.

The caller ensures nonzero divisors, nonnegative sqrt operands, positive ln operands and valid real powers. `0^0=1`; a negative nonzero base requires an integer Float64 exponent, and zero to a negative exponent is outside the domain. Every evaluated subtree must satisfy the preconditions even under min/max or algebraic cancellation. Basic operations retain nearest-even signed-zero rules. `sin(-0)`, `tan(-0)` and `sqrt(-0)` return -0; cos(±0)=exp(±0)=1 and ln(1)=+0. A negative-zero base to an odd positive integer power returns -0, otherwise a positive power returns +0. Tan uses the actual Float64 argument without an epsilon pole test.

## Expression language

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

- Source is nonempty and at most 4096 bytes, with at most 256 AST nodes and AST
  height 32. A leaf has height one. The separate 256-name coefficient limit is
  a declared ceiling; the AST node limit can impose a lower feasible number of
  distinct free identifiers.
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
- Evaluate the AST in left-to-right postorder. Preserve the specified evaluation order; algebraic simplification must not change its stepwise result. `min`/`max` evaluate both
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

## Fixed execution and dependency scope

Static preparation parses once and retains an immutable AST across the call's outputs and independent Runs. It checks expression/names/count/dtype and all input schemas. Registration permits up to 258 inputs; the 256-node AST bound can impose a smaller reachable coefficient count. A balanced sum of 128 coefficients has 255 nodes and 130 declared ports.

Each selected values tile has one fixed CPU task reading its active scalar inputs and writing an independent dense tile. Axis has one separate atomic task. Excluded inputs retain schema-only references. Input values remain active even when the expression is constant or a named coefficient cancels algebraically.

| Selection | Payload inputs | Computation |
| --- | --- | --- |
| values, N>1 | start, end, all coefficients | only selected tile coordinates and their AST values |
| values, N=1 | start, all coefficients | f(start) |
| axis, N>1 | start, end | complete axis tuple |
| axis, N=1 | start | `[start,start,+0]` |
| Empty | none | no task |

Axis evaluates no AST and reads no coefficients. Axis-only selection therefore imposes no expression or coefficient numerical precondition. Values and axis are independently selectable, but selected tasks share one IR call's success/failure outcome. When both are selected, a values task or its required producer failing prevents successful publication of that call's outputs.

Preparation verifies both public output schemas, the selected canonical Host write, exact ordered scalar read slots, empty task parameters and scratch. Structural validation remains checked. Input views may have permitted offsets, unaligned addresses and singleton signed/zero strides when the Plan's physical layout admits them.

## Resources, concurrency and errors

Values scratch is `sizeof(SampleState<true>)`, containing fixed scalar, four-lane, exact-sampling and certified-math state. Axis uses `sizeof(SampleState<false>)`. Runtime constructs both in preallocated invocation memory, with no dynamic AST or payload allocation. Values storage is the sum of selected tile bytes; axis is 24 bytes. Root reserves the maximum task scratch per CPU lane, selected output storage and executor metadata. Static preparation is bounded host metadata outside Run scratch admission.

For K coefficients, A AST nodes and M computed samples per task, ordinary work is O(K+M*A); coordinate and refinement costs are additional bounded arithmetic. Cancellation is queried per scalar, coordinate/batch, AST node and refinement step. There is no per-value numeric reporting, sample work counter or content cache in this execution path.

Concurrent Runs share only immutable preparation; scratch and output allocations are private. Successful Result owners can outlive ExecutionContext. Accepted tasks drain before failed outputs are released. Invalid static syntax, bindings and complexity limits return InvalidArgument; incompatible schema/dtype or task signatures return TypeMismatch. Resource exhaustion, unavailable backend, source failure, cancellation and stale Plan retain their operational statuses. Numerical precondition violations have no promised error classification.

## Implementation and acceptance

`numeric_expression.cpp` registers the three keys and implements the public `sample_expression_node` helper. The iterative parser bounds source bytes, node count and AST height. Decimal parsing uses 16384-bit compile-time ratio scratch, including late digits in a 4096-byte literal. Sampling uses the exact endpoint machinery, and arithmetic uses the shared elementary and certified backend. Four-lane profile evaluation certifies complete strict-stepwise results and replays uncertain samples. See [fixed expression execution](../expression-execution.md) and [mathematical implementation](../math-implementation.md).

Use an independent Fraction/MPFR reference for coordinates, literal conversion and each primitive. A fixed-precision approximation without a rounding decision is insufficient for hard-to-round cases. Compare strict result bits and accelerated final bounds separately from graph-equivalence checks.

| Fixture | Expected behavior |
| --- | --- |
| `2*x+1`, [0,1], N=5 | values `[1,1.5,2,2.5,3]`, axis `[0,1,0.25]` |
| Same expression, [1,0] | values reversed, axis step -0.25 |
| `x*x`, start=2, N=1 | value 4; failing end producer omitted |
| `a*x+b`, mixed dtypes | changing coefficients reuses preparation and changes values, preserving axis |
| `(a+b)-a`, a=2^53,b=1 | two RN64 boundaries produce zero |
| `ln(x)`, [0,1], sparse positive tiles | only selected positive coordinates evaluate; omitted zero endpoint imposes no function-domain check |
| axis-only with failing coefficients | coefficient producers omitted, complete axis returned |
| max count, sparse tail | global coordinate formula and selected storage capacity |
| precedence and literals | `-2^2=-4`, `2^-2=.25`, `2^3^2=512`, `0^0=1`; correct long midpoint literal conversion |

Exercise both dtypes, signed zero, subnormals, sensitive cancellation, negative intervals, nonzero origins and short SIMD tails. Include a balanced 128-coefficient sum, exact source/node/height limits and one-past rejection, malformed syntax, missing/extra/reserved names and invalid excluded-input schemas. Verify scalar offsets and strides, artifact round trips, independent concurrent Runs, rebinding, cancellation, resource admission, malformed preparation and retained output owners. Empty selection performs no payload work. Performance and platform claims require actual runtime evidence for the specified profile and hardware.

Related contracts: [Numeric category](../core.md), [curve/LUT category](../curves.md), [operator template](../../00-foundation/spec-template.md).
