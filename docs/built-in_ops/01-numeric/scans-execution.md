# NUM-13 fixed scan execution

The six `numeric.prefix_sum_<profile>` and `numeric.integral_image_<profile>` keys lower to ABI 8 CPU tasks. Each operation accepts one tensor member under any input key. The output port is `values`, with schema `photospider.tensor` and member `samples`. Output shape starts from the input's complete `sample_shape()`, including batch axes, and extends each selected scan axis by one. Output axes are ordinary axes; input batch and atomic topology does not become output topology. Profiles are `strict`, `accelerated_apple_silicon` and `accelerated_x86_64`; availability is checked during static preparation. These definitions require Host CPU placement.

## Physical tasks and source coverage

Lowering groups requested output tiles by their exact origin and extent on every non-scan axis. Each group becomes one task with multiple independent output allocations. For each scan axis, its source interval starts at zero and ends just before the largest positive boundary required by the group's output tiles. The task declares every input tile intersecting that bounding prefix or rectangle. Input reads are complete canonical tiles, so tile granularity can admit source samples outside the mathematical prefix. Multiple requested integral rectangles in the same group may also enlarge their bounding source rectangle.

A tile whose coordinates all have a zero boundary on at least one scan axis contributes no source extent. A group made only of these tiles has a schema reference, no input payload reads and zero arithmetic scratch. This includes a union of zero rows and zero columns. If an output tile also contains positive boundaries, its positive coordinates determine the source requirement. Empty selection emits no task.

These rules apply when compiling the requested output selection. `ExecutionPlan::select_outputs` closes the already compiled task graph: keeping any output from a multi-write task preserves all of that task's reads, writes and scratch. It does not split or relower a task. Recompile with smaller `PlanningOptions::output_requests` when a narrower scan prefix is required. Final Result coverage remains limited to the selected outputs.

Preparation checks the kernel contract, static schema and parameters, sorted unique output tile slots, common non-scan region, complete required input tile slots and exact scratch size. Prepared state owns immutable metadata and slot maps. Each accepted invocation borrows fixed input views, independent output allocations and private scratch. Affine input views support negative, zero and noncontiguous strides. Concurrent Runs share no mutable accumulator state.

## Exact arithmetic and storage

`prefix_sum` visits each source line in logical axis order, maintaining an exact source aggregate. Only a boundary belonging to a declared write is converted. Conversion uses a separate snapshot, so rounded output, floating overflow and generated NaN never become source carry. The leading boundary is integer zero or floating +0; it is not an arithmetic contribution. A nonempty prefix of only negative zeros yields -0. Source NaN priority follows the original source order.

`integral_image` processes the lower-numbered selected axis as outer rows and the higher-numbered axis as inner columns. At each source cell it merges the current row's exact prefix with the saved exact rectangle from previous rows at that column. These terms are disjoint. Column carry records exact magnitude/sign, first source NaN, infinity flags and all-negative-zero state before destructive conversion. Previous rows precede the current row for NaN priority. All columns reset for each independent plane.

Each visited source sample is read once per group. Prefix scratch is `sizeof(ScanState)` for a pair of exact workspaces. Integral scratch is `sizeof(ScanState) + W * sizeof(SumCarry)`, where W is the required source prefix width on the higher-numbered scan axis. A zero-only group uses no scratch. Output storage consists of the declared dense tiles. No complete internal output or runtime payload allocation is required. The executor admits scratch and output allocations before task invocation.

Integer representability of every declared write is an unchecked caller precondition. This includes all writes retained by `select_outputs`. A freshly compiled sparse request does not convert unrequested boundaries. For Int64 `[INT64_MAX,1,-1]`, a request whose output tile contains only boundary 3 can return `INT64_MAX`; a tile containing boundary 2 violates the integer precondition. Floating overflow yields infinity and preserves the exact source carry for later cancellation. Floating special values retain the sum contract's deterministic classification and payload rules.

Cancellation is checked while initializing output chunks, resetting carries, reading each sample and performing exact arithmetic. Operational failures prevent successful call retirement and publication. There is no per-sample work limit, NumericDiagnostics report, completed-result cache or persistent checkpoint in these fixed invocations. Escaped successful Results own their immutable output storage after the execution context retires.

## Public use and validation

Public helpers `prefix_sum_node` and `integral_image_node` are declared in `photospider/ops/numeric/scans.hpp`. Given an existing workflow input:

```cpp
auto prefix = ps::numeric::prefix_sum_node(
    1, input, 0, ps::ElementType::Int64);
auto integral = ps::numeric::integral_image_node(
    2, input, {0, 1}, ps::ElementType::Int64);
```

Temporary numerical validation uses independent exact prefix and rectangle enumeration followed by destination rounding. Cover same-domain dtypes, nonadjacent axes, batch/atomic layouts, negative and zero strides, rank 1 through 8, source NaN order, infinities, negative zero and exact cancellation after floating overflow. Check small tiles, partial and empty requests, zero-only unions, artifact roundtrips, concurrent Runs, caller floating environment and integer representability for every retained write. A generated source that fails in its tail distinguishes a freshly compiled early prefix from a complete scan. Check preparation rejection for incomplete reads, mismatched writes, scratch and contracts, together with cancellation. Compile temporary checks with `-fno-fast-math`, `-frounding-math` and `-ffp-contract=off`; delete the checkers after validation. Native x86 execution requires a matching host; CPU profile names establish no GPU execution support.
