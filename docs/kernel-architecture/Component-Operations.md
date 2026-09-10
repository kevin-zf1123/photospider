# Threshold and connected components

The default operation registry provides five CPU Whole operations through public
WorkflowDocument/compile/execute APIs. The contract is accepted in
[ADR 0020](../adr/0020-composable-operation-foundations.md); see the
[Chinese mirror](zh/Component-Operations.zh.md) for the same implementation.

| Key | Input | Output | Required static parameter |
| --- | --- | --- | --- |
| `mask.threshold` | Finite Float32 `[H,W]` typed ScalarField | Float32 `[H,W]` canonical coverage mask, exactly 0 or 1 | Finite Float64 `threshold`, constructor default `.5` |
| `mask.components` | Float32 `[H,W]` canonical coverage mask, exactly 0 or 1 | Int64 `[H,W]` typed labels | Int64 `capacity` |
| `component.count` | Int64 `[H,W]` typed labels | Generic Int64 `[1]` | Int64 `capacity` |
| `component.area` | Same labels | Generic Int64 `[capacity+1]` | Int64 `capacity` |
| `component.bbox` | Same labels | Generic Int64 `[capacity+1,4]` | Int64 `capacity` |

All dimensions are nonzero. Each capacity is independently in `[1,2^53-1]`, the
existing exact bounded Int64 parameter range. Constructors explicitly supply it;
no runtime parameter defaults are inserted. The shared output axes add the
checked constant offset one to capacity. Dense output arithmetic is validated
before callback/IR publication, and execution budget admission precedes allocation.

Threshold compares `sample >= threshold`; signed finite values are legal, and
the threshold is expressed in the input field's sample unit. It establishes a
canonical dimensionless coverage mask and drops the field interpretation. The
float comparison runs with nearest rounding and gradual underflow. A generic
array or an image does not become a scalar field by having a matching shape.

Components use four-neighbor connectivity. A row-major scan starts one queue
per previously unvisited foreground component; neighbors are marked when queued.
Labels increase from one in first-pixel order, background is zero. Capacity
limits **final connected components**, so `[[1,0,1],[1,1,1]]` succeeds at capacity
one. Planning tiles do not split connectivity: Whole demands cover the complete
mask, and a component may cross any tile boundary. There is no connectivity
parameter or eight-connected mode in this first version.

The label descriptor is canonical ScalarField with Int64 `[H,W]`, channel
name/role `component_label`, and overall/channel unit `dimensionless`. It has no
capacity facet. Attribute ports require these exact facets. Each consumer checks
all sample labels against its own `[0,capacity]`; a producer capacity ten with
actual labels one and two may feed a consumer capacity five. Sparse label IDs
are permitted. Count returns the number of distinct nonzero IDs, not the maximum
ID. It does not relabel or reinterpret disconnected pixels with the same ID.

Area counts pixels for each ID. Bbox records are
`x_min,y_min,x_max_exclusive,y_max_exclusive` in logical pixel coordinates.
Background record zero and all unused records are zero, even for empty foreground;
empty output tables retain the statically declared nonzero capacity. Area/bbox
are generic integer arrays without copied label facets. All five nodes retain
the existing one-output contract; request separate named outputs for attributes.

The implementation reads logical coordinates using byte offset, storage origin
and signed/zero strides through memcpy, including unaligned views. Components
allocate an accounted `8*S`-byte queue for S input pixels; count allocates a hash
table of at most `32*S` bytes, independent of capacity. Area/bbox need no separate
workspace. Output initialization, scanning, queue processing and hash probing
check cancellation. Every error releases unpublished output/workspace; there is
no partial successful result. Negative/out-of-capacity labels, nonbinary coverage,
area/coordinate overflow fail with `OperationFailed` and a sample index.
Invalid parameters are `InvalidArgument`, incompatible metadata is `TypeMismatch`,
and cancellation/resource failures retain their own codes. Direct invalid typed
binding values follow the existing preflight error contract.

## Public workflow and verification

[test_component_operations.cpp](../../tests/integration/test_component_operations.cpp)
contains runnable `run()`, `threshold_cases()`, `component_cases()` and
`attribute_cases()` workflows. The public immutable input producer in `run()`
allows strided Value examples. An ordinary dense input can instead use a
WorkflowDocument declaration and per-run ExecutionBindings. For a bound mask
input 1, the reusable nodes are:

```cpp
ps::WorkflowNode labels{
    1, "mask.components", {ps::WorkflowInputReference{1}},
    {{"capacity", std::int64_t{5}}}};
ps::WorkflowNode area{
    2, "component.area", {ps::WorkflowNodeOutput{1, "value"}},
    {{"capacity", std::int64_t{5}}}};
```

Use `encode_semantic(coverage_semantics())` on the bound Float32 mask. Connect
component.count/bbox to the same labels output, or prepend mask.threshold to a
Float32 typed ScalarField. Changing capacity or threshold changes static plan
parameters and requires compilation; changing same-descriptor values can use
per-run bindings. All Region dependencies are Whole.

```sh
cmake --build build/issue257-static --target test_component_operations -j 8
ctest --test-dir build/issue257-static -R '^test_component_operations$' --output-on-failure
```

Exit zero checks: empty foreground gives zero labels/count and zero nonempty
tables; the bridge gives one component, area five, bbox `[0,0,3,2]`; a 3x3
checkerboard gives stable IDs one through five under four-connectivity. Sparse
labels `[[0,2,2],[5,0,5]]` give count two, area `[0,0,2,0,0,2]`, bbox ID2
`[1,0,3,1]`, ID5 `[0,1,3,2]`; capacity four rejects ID5. Further oracles cover
independent capacities, subnormal nonbinary input, large exact IDs, unaligned
negative/zero strides, cancellation and small allocation budgets. Static/shared
installed consumers compile the same source against installed public targets.
