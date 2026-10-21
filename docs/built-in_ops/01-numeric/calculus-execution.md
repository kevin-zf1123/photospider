# Fixed calculus execution

The six `numeric.derivative_1d_<profile>` and `numeric.integrate_1d_<profile>` keys use fixed CPU tasks and `ExactCalculus` arithmetic. Samples have complete Float32/Float64 sample shape `[N]`, with N at most 2^40. Step and initial are same-dtype `[1]` tensors; each Result has one tensor member under any key. Output `values` uses `photospider.tensor` v1/member `samples`, the source dtype and ordinary `[N]` axes.

## Derivative stencils

Derivative requires N>=2. Each selected output tile has one task. For an output interval `[a,b)`, the lower stencil samples occupy `[max(0,a-1),max(1,b-1))` and the upper samples occupy `[min(a+1,N-1),min(b,N-1)+1)`. Lowering declares their union in the source's own tile grid, plus step tile zero. This includes only the two stencil images before source tile rounding. With extent one and an ordinary source axis, an interior output's source center tile is absent. Atomic source tiles can cover both the stencil and its center.

Runtime evaluates the one-sided endpoint quotient or central quotient for each selected coordinate. Source signed zeros, NaNs and infinities follow [NUM-15A](op_specs/NUM-15A_derivative_1d.md); only the two stencil samples contribute. Each task owns an independent output allocation and `sizeof(ExactCalculus)` scratch. Arithmetic does not require a complete output or a collected source buffer.

## Exact integration prefixes

An output tile whose entire region is `[0,1)` has a separate task that copies initial raw bits. It reads only initial and records schema-only references to samples and step, with zero scratch. This occurs for N=1 at any extent, or for the first tile at extent one. Both omitted input schemas remain fully checked. Their producers are unnecessary when only that task is retained.

All other selected output tiles share one prefix task. If the largest selected tile ends at E, that task reads source `[0,E)`, step and initial. It adds each source sample once to an exact prefix and converts only selected positive output coordinates. The exact carry survives each conversion and no rounded output feeds a later result. If an output tile includes both zero and positive coordinates, it belongs to this prefix task and also copies initial into its zero coordinate. Details of weighted prefixes, NaN priority and zero signs appear in [NUM-15B](op_specs/NUM-15B_integrate_1d.md).

The prefix task declares `sizeof(ExactCalculus)` scratch regardless of E and allocates only its selected output tiles. Selecting a saved Plan retains all reads, writes and scratch of each retained task. A newly compiled narrower request can shorten the prefix or remove its task. Empty selection creates no task or payload dependency.

## Preconditions, ownership and validation

An active step is finite and nonzero as a caller numerical precondition. Negative step is valid; diagnostics outside the domain are not guaranteed. The arithmetic uses this nonzero-step premise directly without a denominator-domain guard. All IEEE sample and initial bit patterns are accepted. Initial-only tasks impose no step payload condition. Strict uses the exact discrete formula with one final RN conversion; accelerated results follow the shared final FP32 error budget. Nonzero exact underflow keeps its sign and final overflow produces signed infinity. Invocations restore floating modes and exception flags.

Prepare validates all input schemas, the ordinary output schema, canonical Host writes, sorted write slots, exact source/control tile membership, empty parameters and exact scratch. Derivative has one write; integration keeps the zero-only tile separate from multiwrite prefixes. All storage is admitted before task entry. Runtime borrows immutable input views and preallocated output/scratch allocations, and creates no payload or carry allocation. Root reserves the maximum task scratch per CPU lane plus selected output storage and executor metadata.

Cancellation is checked during source traversal, exact arithmetic and output iteration. Each Run has private scratch and output owners. Operational failure prevents successful publication of the IR call, including when initial-only and prefix tasks are both selected; accepted work drains before their storage is released. Successful Result owners can outlive the execution context. Resource, source, backend and cancellation failures retain their operational status categories.

## Behavior validation

The retained [NUM15 Fraction oracle](../../../oracle/ops/numeric/num15/reference.py) specifies the discrete formulas and IEEE cases. Temporary public checks compare selected outputs and actual source tile closures, including center omission, tail tiles, disjoint prefixes, source atomic geometry, negative/zero strides and unaligned offsets. Exercise negative and subnormal step, finite cancellation, initial raw NaNs, source NaN priority, infinities, and exact prefixes across unselected outputs.

Also verify initial-only producer omission at N=1 and at larger N with extent one, saved Plan selection, full static validation of excluded inputs, artifact round trips, concurrent Runs, binding replacement, resource rejection, cancellation and owners surviving context destruction. Compile numerical checks with `-fno-fast-math -frounding-math -ffp-contract=off`; use only actually executed profiles in validation reports. Exact rounding of a discrete formula does not establish the derivative or integral of an unknown continuous function.
