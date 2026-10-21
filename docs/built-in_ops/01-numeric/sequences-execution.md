# NUM-02 fixed sequence execution

The six `numeric.{linspace,arange}_{strict,accelerated_apple_silicon,accelerated_x86_64}` keys lower to ABI 8 CPU tasks. Inputs each contain one tensor under any schema/member key with complete `sample_shape()` `[1]`. Required static Int64 count is in `[1,1048576]`; String dtype is Float32/Float64 for linspace and also Int64 for arange. Floating inputs may independently be Float32 or Float64; integer mode requires both Int64.

Output `values` is `photospider.tensor`/`samples`, shape `[count]`, with selected dtype and ordinary axes. Output `axis` has shape `[3]` and `atomic_trailing_axes=1`, using Int64 in integer mode and Float64 otherwise. Each selected values tile has one task and an independent dense allocation. Axis has one task writing all three components. Only selected outputs are computed; selecting axis alone allocates no values tiles.

## Inputs and arithmetic

Every nonempty task reads start. For count one, the second port has only a schema reference and its producer has no payload dependency. Both complete schemas are still validated. For larger counts every task reads both scalar inputs, including endpoint-only values requests. Empty output selections emit no tasks or payload reads.

Values use the global tile origin plus the local sample index. Linspace converts each endpoint separately and evaluates interior `((N-1-i)*start+i*end)/(N-1)` exactly before destination rounding. Arange evaluates exact `start+i*step`; integer products may exceed Int64 before cancellation. Float32 values round directly from the exact state. No rounded step or already-rounded sample becomes carry.

Axis is `[start,end,RN64((end-start)/(N-1))]` for floating linspace, and `[start,last,step]` for arange with last computed in the axis dtype. Count one gives `[start,start,+0]` without reading the second input. Endpoint conversions preserve signed zero. Interior exact zero is negative only when all active additive zero terms are negative; exact nonzero underflow preserves sign. Equal linspace endpoints produce a positive-zero axis step.

The arithmetic uses two inline 68-limb ExactBinarySum values and 68 product words in private preallocated SequenceMath scratch. The existing scalar/NEON/AVX2 multiplication helpers preserve exact products and carry order. Current accelerated implementations use the same exact result calculation; their public accuracy promise remains the shared accelerated contract. Profile availability is checked before invocation, with no runtime key substitution.

## Unchecked domain and call publication

Dynamically read floating inputs must be finite. Every computed floating output component must round to a finite value, and every computed integer output component must fit Int64. These are caller preconditions; kernels do not scan samples or promise numerical diagnostics outside that domain. Subnormals and gradual underflow are valid. Unselected outputs impose no output-range requirement. A sparse values selection need not make later unselected arange values representable; requesting axis additionally requires its complete tuple to satisfy the precondition.

Values and axis have separately selectable coverage, but selected tasks belong to one IR call. Successful call publication requires all selected writes to succeed. An operational failure of a selected task prevents successful publication of that call's outputs; the old independent-output failure isolation is removed. Separate Runs may select outputs independently.

## Preparation, resources and checks

Prepare validates both exact output schemas, the symbol's output port and canonical tile, dtype, Host domain, full active input read slots, empty task parameters and exact SequenceMath scratch. In particular, axis cannot be presented as a partial three-component view. Prepared metadata is immutable; each invocation constructs private exact state in admitted scratch and writes only its output tile. No runtime payload allocation, continuation, per-sample work accounting or content cache is used. Cancellation is checked before scalar reads and each sample; inner arithmetic loops have fixed bounds. FloatingEnvironment is established before Float32 widening and restored on every return. Successful Results own output storage beyond context destruction.

Temporary checks use an independent Fraction/IEEE rounding oracle, covering mixed input dtypes, direct Float32 rounding, endpoint zero signs, integer intermediate-product cancellation, extreme finite values, output masks, sparse selections, count one/max count, atomic axis, source offsets/strides, artifact roundtrips and concurrent Runs. Exclude configurations violating the numerical preconditions. Verify that a failing second producer is eliminated at count one and required at larger counts, that excluded input schemas still validate, and that malformed prepare signatures reject. Execute the Float32 shaper helpers that construct zero/one through count-one linspace. Remove temporary checkers after validation; unavailable x86/GPU paths remain separate evidence limits.
