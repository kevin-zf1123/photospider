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

This specification defines three explicit CPU profiles: strict, accelerated Apple Silicon and accelerated x86-64. The unsuffixed `numeric.sample_expression` has its own static start/step and indexed-coefficient interface and is not an alias for this family.

The operation samples a real-valued function of one variable on an endpoint-inclusive grid. It returns named `values` and `axis` Results, usable independently by downstream numeric consumers or LUT authoring. Count, expression, coefficient names and output dtype are static; endpoint and coefficient payloads can change between Runs. Inherits the [NUM baseline](NUM_common_contract.md).

## Interface

Inputs are ordered start, end, then named coefficients in bytewise sorted order. Each Result contains exactly one tensor member, under any schema ID or member key, with complete sample shape `[1]` and dtype Float32 or Float64. Dtypes may mix; Float32 widens exactly to Float64. Static checks cover every input even when its payload is not read.

| Output | Schema and member | Shape and dtype |
| --- | --- | --- |
| values | photospider.tensor v1 / samples | ordinary `[count]`, Float32 or Float64 |
| axis | photospider.tensor v1 / samples | atomic Float64 `[3]`, `[start,end,step]` |

Output sample axes inherit no input batch axes or physical units. Function arguments use radians where applicable.

| Name | Type and domain | Default |
| --- | --- | --- |
| `expression` | String, grammar and limits below | Required |
| `count` | Int64 in `[1,1048576]` | Required |
| `dtype` | String `float32` or `float64` | Helper writes `float64`; direct nodes specify it |
| `coefficient_names` | Canonical String of sorted free identifiers | Helper derives it; direct nodes specify it |

The public SDK helper is `sample_expression_node`. Changing expression, names, count or dtype requires recompilation; changing numeric input values does not. The registered input count is up to 258; the 256-node AST bound can impose a smaller reachable coefficient count (a balanced sum of 128 coefficients has 255 nodes and 130 declared ports). Invalid static syntax, bindings and complexity limits return InvalidArgument; incompatible schema or dtype returns TypeMismatch.

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

Intermediate subtraction/product overflow does not alter these formulas. Interior coordinates use both endpoints and their global index, without accumulating the rounded axis step. Ascending and descending intervals are both supported. Endpoints differ for N>1. A selected axis requires a finite, nonzero rounded step with the corresponding sign. A selected values coordinate differs from each valid adjacent coordinate. These numerical conditions are unchecked caller preconditions.

For N=1, x=start and axis is `[start,start,+0]`; the end payload is not read. For larger counts both endpoints are active even when only an endpoint values tile is selected. Only requested coordinates are evaluated; unselected expression samples impose no expression-domain requirement.

## Numeric evaluation

Active inputs, evaluated intermediates and computed outputs are finite and within the applicable real mathematical domains as unchecked caller preconditions. Float32 outputs require a representable final narrowing. Underflow to subnormal or signed zero is valid.

Strict evaluates every primitive in left-to-right postorder with RN64 before its parent. The complete expression is not reassociated into one exact-real calculation: `(a+b)-a` keeps both rounding boundaries. Decimal literals convert once from the complete token using RN64. Mathematical functions are correctly rounded. Float32 output is one further RN conversion of the final Float64 result.

Accelerated profiles apply the shared [final FP32-scaled four-ULP contract](NUM_accelerated_contract.md), including Float64 outputs, relative to the final result of the strict stepwise reference. Unavailable platform keys return BackendUnavailable.

The caller ensures nonzero divisors, nonnegative sqrt operands, positive ln operands and valid real powers. `0^0=1`; a negative nonzero base requires an integer Float64 exponent, and zero to a negative exponent is outside the domain. Every evaluated subtree satisfies the preconditions even under min/max or algebraic cancellation. Basic operations keep nearest-even signed-zero rules. `sin(-0)`, `tan(-0)` and `sqrt(-0)` return -0; cos(+-0)=exp(+-0)=1 and ln(1)=+0. A negative-zero base to an odd positive integer power returns -0, otherwise a positive power returns +0. Tan uses the actual Float64 argument without an epsilon pole test.

## Expression language

A bounded pure mathematical expression with one independent variable `x` and separately bound named scalar coefficients.

| Group | Syntax/capability |
| --- | --- |
| Arithmetic | Addition, subtraction, multiplication, division, power, parentheses and unary signs |
| Constants | `pi`, `e` and numeric literals |
| Trigonometry | `sin`, `cos`, `tan`, with arguments in radians |
| Exponential/logarithm | `exp`, `ln` (natural logarithm) |
| Other functions | `sqrt`, `abs`, `min`, `max` |

Comparisons and conditional branches are outside this version. There is no script execution, loop, file I/O, implicit time input or randomness.

### Grammar and binding conventions

- Source is nonempty and at most 4096 bytes, with at most 256 AST nodes and AST height 32. A leaf has height one. The 256-name coefficient limit is a declared ceiling; the AST node limit can impose a lower feasible number of distinct free identifiers. Parentheses and whitespace do not add AST nodes; unary signs do.
- Zero coefficients are legal: `x^2`, `sin(x)` and `3` require no dummy input.
- Numeric literals use decimal/scientific syntax such as `.5`, `1.`, `2e-3`. Each converts once with RN64. Overflowing literals, hexadecimal literals, NaN/Inf names, implicit multiplication and locale-dependent separators are rejected.
- Identifiers are case-sensitive ASCII `[A-Za-z_][A-Za-z0-9_]*`, at most 63 bytes. `x`, `pi`, `e`, function names and interface names `start`, `end`, `count`, `dtype`, `values`, `axis`, `coefficient_names` are reserved.
- Free identifiers identify coefficient inputs. The static `coefficient_names` list matches these identifiers exactly, without duplicates or unused bindings. The authoring helper derives and sorts this list bytewise. A canonical space-separated String encodes it in the WorkflowDocument parameter; the empty string represents zero coefficients.
- Ordered numeric inputs are `start`, `end`, then coefficients in that list order. Both endpoint edges are statically present; with N=1 the `end` edge has no runtime demand.
- `ln` is the sole natural-log spelling; there is no `log` alias or indexed `c[...]` interface.
- Function calls require parentheses. `min`/`max` have two arguments; the other functions have one. Constants `pi` and `e` are correctly rounded Float64 constants in both numeric profiles.
- Power uses `^`, is right-associative and binds above unary signs; then follow `* /`, then `+ -`, with left associativity for the latter two levels. Thus `-2^2=-4`, `2^-2=0.25`, `2^3^2=512`.
- The AST is evaluated in left-to-right postorder, and algebraic simplification does not change its stepwise result. `min`/`max` evaluate both arguments. Mixed signed-zero ties return -0 for min and +0 for max; same-sign zeros keep that sign, matching NUM-05 minimum/maximum. Other numerical ties return the common value; `abs(-0)` is `+0`. A mathematical-function approximation to an exact zero preserves the strict zero result and sign; the ULP allowance does not permit a nonzero subnormal in place of a mathematical zero.

## Behavior

Input values remain active even when the expression is constant or a named coefficient cancels algebraically. Axis evaluates no expression and reads no coefficients, so axis-only selection imposes no expression or coefficient numerical precondition.

| Selection | Payload inputs | Computation |
| --- | --- | --- |
| values, N>1 | start, end, all coefficients | only selected coordinates and their expression values |
| values, N=1 | start, all coefficients | f(start) |
| axis, N>1 | start, end | complete axis tuple |
| axis, N=1 | start | `[start,start,+0]` |
| Empty | none | none |

Values and axis are independently selectable, but they share one call's success or failure: a failure in either selected output prevents successful publication of that call's outputs.

## Acceptance

Reference: independent exact rational (Fraction/MPFR) evaluation of coordinates, literal conversion and each primitive. Compare strict result bits and accelerated final bounds.

| Fixture | Expected behavior |
| --- | --- |
| `2*x+1`, [0,1], N=5 | values `[1,1.5,2,2.5,3]`, axis `[0,1,0.25]` |
| Same expression, [1,0] | values reversed, axis step -0.25 |
| `x*x`, start=2, N=1 | value 4; end payload not read |
| `a*x+b`, mixed dtypes | changing coefficients changes values, axis is preserved |
| `(a+b)-a`, a=2^53,b=1 | two RN64 boundaries produce zero |
| `ln(x)`, [0,1], sparse positive tiles | only selected positive coordinates evaluate; the omitted zero endpoint imposes no function-domain check |
| axis-only with failing coefficients | coefficient producers not required, complete axis returned |
| max count, sparse tail | global coordinate formula |
| precedence and literals | `-2^2=-4`, `2^-2=.25`, `2^3^2=512`, `0^0=1`; correct long midpoint literal conversion |

Both dtypes, signed zero, subnormals, sensitive cancellation, negative intervals, nonzero origins. Exact source/node/height limits and one-past rejection, malformed syntax, missing/extra/reserved names and invalid schemas of unread inputs.

Related contracts: [Numeric category](../core.md), [curve/LUT category](../curves.md), [operator template](../../00-foundation/spec-template.md).
