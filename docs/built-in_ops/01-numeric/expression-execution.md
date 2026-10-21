# Fixed expression execution

The three `numeric.sample_expression_<profile>` keys prepare a bounded immutable AST and lower selected output tiles to fixed CPU tasks. Inputs are Float32/Float64 `[1]` tensors with independently chosen dtypes and arbitrary schema/member keys. Each Result contains exactly one tensor member. `values` uses ordinary `[count]` axes in the requested dtype; `axis` is one atomic Float64 `[3]` tuple. Both use `photospider.tensor` v1/member `samples`.

## Preparation and reads

Static preparation parses the expression, canonicalizes free names and checks all schemas. Limits are 4096 source bytes, 256 AST nodes, height 32 and count in `[1,1048576]`. Names match the explicit sorted `coefficient_names` parameter and input order `start,end,coefficients...`. The registry permits up to 258 input slots; the AST limit further bounds the number of reachable distinct coefficients. A balanced 128-coefficient sum uses 255 nodes and 130 declared inputs.

Each selected values tile has one task. It reads start, end when count>1, and every named coefficient. Axis has its own task reading only the active endpoints. Every excluded port has a schema-only reference. Thus count one excludes end's payload and producer, while axis excludes every coefficient producer. These projections apply even when an expression is constant or a coefficient cancels algebraically. Empty selection creates no tasks or payload reads.

Kernel preparation verifies the complete two-output schemas, canonical Host write region, public output port, dtype, exact ordered scalar reads, empty task parameters and exact scratch. The compiled static owner retains the AST through all PreparedKernel and Run lifetimes. Changes to numeric bindings reuse that AST; changing expression, names, count or dtype requires compilation.

## Arithmetic and numerical preconditions

For a selected global index, `ExactSampling` computes the endpoint-weighted RN64 coordinate. Endpoints preserve their bits after exact Float32 widening. Interior coordinates are rounded directly from the rational weighted sum; a rounded axis step is never used as carry. Axis is `[start,end,RN64((end-start)/(count-1))]`, or `[start,start,+0]` for count one.

Strict evaluates the AST in left-to-right postorder, rounding every primitive to Float64 before its parent. The final Float32 conversion is separate. Exact elementary and directed-refinement math use fixed inline state. Accelerated evaluation batches up to four coordinates within the output tile and propagates enclosures of the complete strict stepwise result. An uncertain final bound causes strict replay of that sample. These interval decisions enforce the profile's accuracy; they are not general input validation. A short tile or tail evaluates only its declared coordinates.

The caller supplies finite active inputs, valid function domains, finite intermediate results and representable computed output values. For count>1 endpoints must differ. Each selected values coordinate must remain distinguishable from its valid adjacent coordinates; only a selected axis imposes a finite, nonzero rounded step. These are unchecked numerical preconditions. The kernel does not scan neighboring coordinates, validate samples or promise a domain diagnostic. Unselected expression samples impose no computation or expression-domain requirement. Axis-only evaluation imposes no coefficient or expression-intermediate precondition.

Gradual underflow and specified signed-zero rules remain part of valid arithmetic. `FloatingEnvironment` normalizes the worker environment and restores rounding modes and flags after invocation. See [NUM-01](op_specs/NUM-01_sample_expression.md) for grammar and per-primitive semantics.

## Storage, concurrency and failure

A values task declares `sizeof(SampleState<true>)` scratch, containing coefficients, scalar intermediates, the four-lane enclosure table, exact sampling and certified-math state. Axis declares the smaller `sizeof(SampleState<false>)` scratch. Both construct state directly in admitted invocation storage. Runtime creates no payload allocation or dynamic AST. Scalar copies use raw bits and support permitted offsets and singleton strides. Each output tile has its own dense allocation; axis owns 24 bytes.

For M computed samples, A AST nodes and K coefficients, ordinary task work is O(K+M*A), with coordinate/refinement arithmetic bounded separately. Root reserves selected output storage and the maximum task scratch per CPU lane, plus executor metadata. Sparse selection avoids full `[count]` output admission. Static AST/preparation allocations are ordinary bounded host metadata, outside Run scratch admission. Per-value work counters, content caches and numeric diagnostics are not exposed by this path.

Cancellation is checked per active scalar, batch/coordinate, AST node and refinement step. Bounded unresolved mathematical refinement may fail ResourceExhausted. Resource, source, backend and cancellation failures retain their operational statuses. Selected values and axis tasks share one IR call's publication outcome. An operational failure prevents successful publication of that call, and accepted work drains before failed storage is released. Successful immutable output owners can outlive ExecutionContext. Concurrent Runs share immutable preparation and own separate scratch and outputs.

## Behavior validation

Use the retained [NUM01 Fraction/MPFR oracle](../../../oracle/ops/numeric/num01/reference.py) for independent coordinates, decimal literals and stepwise mathematical references. Fixed execution checks evaluate only selected tile coordinates; the oracle's complete-grid domain-validation wrapper must not impose domain checks on unselected samples.

Cover strict bits and accelerated final bounds, mixed input dtypes, reversed intervals, singleton count with failing end, axis-only with failing coefficients, empty selection, many coefficients, source/AST limits, precedence, signed zeros and subnormals. Exercise nonzero tile origins, SIMD tails, sparse maximum count, static metadata of excluded ports, malformed kernel signatures, raw scalar offsets, artifact round trips, binding replacement and concurrent Runs. Also verify floating environment, cancellation, resource admission and escaped output owners. Numerical success alone is not performance or native GPU evidence.
