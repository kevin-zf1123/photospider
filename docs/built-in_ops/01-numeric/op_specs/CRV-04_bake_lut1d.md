---
spec_schema_version: 1
id: CRV-04
kind: shared_workflow_contract
category: 01-numeric
status: Proposed
document_maturity: D1_draft
implementation_status: implemented
implementation_branch: numeric-optimize
implementation_base_commit: eb0e90c8
implementation_updated: 2026-09-21
verification_status: manual_public_graph_equivalence
clarification_status: complete
repository_branch: ops-specs
repository_commit: 6617c78c
---

# CRV-04: bake_lut1d workflow templates

Numeric profile: strict retains the exact reference defined below. Floating
arithmetic in accelerated profiles follows the shared
[final FP32 four-ULP contract](NUM_accelerated_contract.md), including its
range/fallback rules. Discrete results, copies, selected endpoints and special
values remain exact.

## Confirmed scope and composition boundary

Provide named composite workflow authoring templates for baking a function into
sampled table values and a sampling axis. Reuse the already specified samplers
instead of registering a duplicate numeric primitive. Mathematical behavior
and precision are inherited from the expanded operations, not redefined by an
opaque bake callback or a new runtime function-object type.

Available building blocks include expression sampling (NUM-01), a Bezier
function sampler (CRV-02), and linspace query generation followed by CRV-01
interpolation. The six concrete source templates inherit this completed contract.
These building blocks and the six public constructors are implemented. Legacy
LUT consumers retain their own interfaces; dynamic-axis consumption is tracked
separately under CRV-05.

## Confirmed source templates

Provide six independent named source templates: expression, Bezier y=f(x),
single-function linear, single-function PCHIP, multi-function linear and
multi-function PCHIP. Each exposes its own underlying dynamic inputs and static
parameters. All name their exported outputs values and axis. CRV-03 parametric
curves are not a source for these ordinary one-dimensional function LUT templates.

## Confirmed common sampling surface

All six templates expose dynamic Float32/Float64 start/end scalar inputs and
static required count in [1,1048576]. Static output dtype selects Float32 or
Float64, default Float64. Scalar sources emit values[N]; multi-function sources
emit values[N,C]. Axis is Float64 [start,end,step]. For count=1, sampling ignores
end and axis is [start,start,0], inheriting the source sampler's no-end-read rule.
All other source-specific parameters remain explicit; no inferred count or
conversion into a typed image/color LUT is introduced by the template.

## Confirmed authoring profile and query precision

The authoring template has static profile strict/apple_silicon/x86_64, default
strict. Expand each source node into the corresponding independently named
strict or CPU accelerated key. This is a workflow construction choice, not a
new primitive mode parameter. Interpolation templates generate query positions
with linspace dtype=float64 regardless of requested table dtype; their CRV-01
node performs the selected final table conversion.

## Template expansion and exported outputs

| ID and template | Runtime nodes | Exported values / axis |
| --- | --- | --- |
| [CRV-04A expression](CRV-04A_bake_lut1d_expression.md) | NUM-01 sample_expression | Sampler values / sampler axis |
| [CRV-04B Bezier function](CRV-04B_bake_lut1d_bezier.md) | CRV-02 sample_bezier_function | Sampler values / sampler axis |
| [CRV-04C linear](CRV-04C_bake_lut1d_linear.md) | NUM-02A linspace -> CRV-01A | Interpolator values / linspace axis |
| [CRV-04D PCHIP](CRV-04D_bake_lut1d_pchip.md) | NUM-02A linspace -> CRV-01B | Interpolator values / linspace axis |
| [CRV-04E linear multi](CRV-04E_bake_lut1d_linear_multi.md) | NUM-02A linspace -> CRV-01C | Interpolator values / linspace axis |
| [CRV-04F PCHIP multi](CRV-04F_bake_lut1d_pchip_multi.md) | NUM-02A linspace -> CRV-01D | Interpolator values / linspace axis |

Authoring names in individual files identify templates, not OperationRegistry keys. Expansion creates ordinary WorkflowDocument nodes with distinct node IDs, explicit static parameters, declared dynamic edges and named WorkflowOutput port references. Profile maps to key suffix `_strict`, `_accelerated_apple_silicon` or `_accelerated_x86_64` for every expanded node. It adds no profile parameter to the operation schemas. Constructors reserve existing and referenced node IDs, allocate the lowest unused positive IDs and enforce the 65,536-node document limit. They construct all nodes before mutating the caller's graph, so invalid authoring leaves the graph unchanged. The caller declares Result-backed workflow inputs and binds Results that satisfy the declared schemas; static schema hints do not replace compiler validation of bound Results.

Construction only authors metadata and node references. The returned `BakedLut1d` contains `WorkflowNodeOutput` references for `values` and `axis`; its `outputs` method creates caller-named `WorkflowOutput` declarations. The caller chooses the demand and executes the graph through the ordinary Result workflow. Baked outputs are generic Result tensors; consumers validate their own LUT domain, ordering, channel and dtype requirements.

## Inherited semantic differences

Template convenience does not introduce new numerical validation or silently
normalize incompatible source semantics. NUM-01/CRV-02 reject equal endpoints
for count>=2 and collapsed sampled coordinates under their exact rules.
Interpolation templates inherit linspace's allowed equal endpoints and repeated
rounded positions. All support descending sampling where their source permits
it. K remains an interpolation/Bezier control count, distinct from output count.
No template chooses a sample count based on an inferred quality threshold.

For interpolation, query[i] is the correctly rounded Float64 linspace coordinate,
then the selected interpolator evaluates at that exact input float. This is not
an unrounded rational query followed by a combined sampling/interpolation round.
The output axis describes the source grid, with its existing reconstruction rule;
repeatedly adding rounded axis.step is not generally equivalent. count=1 retains
axis=[start,start,0] and never reads end payload. Source metadata still validates.

Each expanded node preserves its own exactness/tolerance. In particular,
sample_expression strict is stepwise Float64; linear strict is whole-formula
correctly rounded. Linear, PCHIP and Bezier accelerated results follow the shared
final FP32 bound relative to their respective strict reference formulas. A shared authoring profile does not equate those formulas.
Baking discrete samples supplies no automatic bound on the later LUT consumer's
interpolation error relative to a continuous function.

## Demand, resources, errors and lifetime

All expanded formal source outputs use CPU Whole. A nonempty `values` request computes the complete baked table and returns its full certified coverage, even when the caller requests only a subset. Interpolation templates first materialize the complete Float64 linspace query array (`8*count` bytes), then the complete interpolation output (`count*C*dtype_size` bytes), in addition to active source owners and source-specific workspace. The host accounts these allocations through the same Root resource budget. A sparse request does not reduce the Whole output owner or numerical work.

`axis` is an independent Float64 `[3]` Result. Axis-only demand does not execute expression coefficients, Bezier controls or interpolation x/y. For `count=1`, expanded sources omit the end payload; for larger counts, even a first-value demand collects end and computes the complete grid. Each source preserves its formula, selection, typed validation and numeric allowance. Active Result inputs retain complete typed and upstream validation, so a failure outside the requested numerical region can still fail the Whole Run. Arithmetic errors retain the producing source operation's Run scope. Empty demand returns empty coverage without polling a value producer or reading payload. Any metadata processing needed to construct and seal that empty Result remains part of ordinary execution.

The Result association records active source ObjectIds, and dependency support maps the requested output region back to its source support. Editing a source invalidates the values demand according to that recorded mapping; function controls do not dirty the independent axis. Repeating an identical demand retains its completed Result identity. Equal-content source Results can reuse cached computation while the newly returned association points to the current active sources. For interpolation templates, the table Result directly associates with x, y and the current linspace query Result; that query Result and the separate axis Result associate with the current endpoint Results. The one-node expression/Bezier fixtures assert exactly two fresh-source cache hits for `values` and `axis`; interpolation fixtures export `query_values` and assert exactly three. Replacing bindings can therefore reuse static `PreparedOperation` objects while producing outputs for the new inputs.

The fixture's source arrays are local `Value` backing used to publish immutable source Results. It binds those Results through `ExecutionBindings`; execution returns `DemandResult.results`, and reads use Result tensor access. Baked `values` and `axis` have independent immutable Result owners. Their authorized read windows retain the corresponding Root allocations after the Result handles, source backing and execution context retire. Releasing the final window returns the corresponding live Root resources to zero.

The expanded source operations own numerical evaluation, validation, Result publication and source-level failures; the execution host owns scheduling, active associations, cache identity, shared resource admission and cancellation. If a Whole output exceeds the caller's resource budget, execution returns its resource failure before publishing partial table contents. Callers should budget for the full query and table, and should request `axis` only when needed.

## Acceptance and implementation boundary

For every template and CPU profile, construct the generated graph and its explicit WorkflowDocument expansion through the public authoring APIs. Compare output schemas and coverage, requested Result bits, analytic fixture values, source support and dirty mapping. Exercise both table dtypes and the four demand modes: full values, axis-only, joint values and axis, and sparse values. Keep template-equivalence checks distinct from the numeric oracles that validate the underlying samplers and interpolators.

Behavior coverage includes count one with a failing end producer, count greater than one with a failing end producer, empty demand with a real failing source continuation, source-specific equal-endpoint/domain rules, independent axis demand, mixed endpoint/table dtypes, fresh-source cache hits and current associations, repeated-demand Result identity, static preparation reuse after binding changes, work/payload rejection, cancellation after computation starts and recovery. Test reversed, unaligned and scalar zero-stride source layouts across every input port, and verify caller and worker floating-environment preservation. Exercise source owner retirement, independent values/axis ownership, Result and read-window lifetime, and release of all Root resources. A sparse million-row PCHIP request under a 1 MiB Payload budget returns `ResourceExhausted / CapacityLimit` at node 1 before the complete linspace query Result can be admitted; this checks full-query admission and does not claim successful maximum-size execution. Injected computation WorkLimit and cancellation also return with live Payload at zero.

The maintained `examples/numeric_workflow/baking.cpp` fixture declares Result schemas, binds immutable source Results backed by local `Value` storage and runs both generated and hand-authored graphs through `ExecutionContext`. For each profile it compares 48 graph pairs (six templates by two table dtypes by four demand modes) and separately checks the analytic values encoded in its fixtures. Its numeric oracle is separate. The fixture's seven groups also exercise source failure ordering, cache and association behavior, preparation reuse, authoring boundaries, layouts, cancellation and Result ownership. Direct Strict and Apple runs each passed all seven groups. The focused root CTest selection passed `test_numeric_baking_result` and `test_numeric_result_math` 2/2 in 5.22 seconds (0.38 and 4.83 seconds). The freshly compiled consumer against the reinstalled 0.32.0 package passed the installed baking/inverse/LUT3D selection 3/3 in 5.71 seconds, with 2.39 seconds for baking; its direct Apple run passed all seven groups. These CTest durations include work from concurrent CPU tests and are not performance measurements. These results document this implementation run and do not change the Proposed status of this contract. x86 and successful maximum-physical-size sampling were not run.

No additional numeric primitive is introduced. Numerical arithmetic and independent numeric oracles for the underlying operations remain in NUM-01/02 and CRV-01/02; generated/explicit graph equivalence alone does not prove their numerical contracts. Historical package 0.18 Value-path measurements are retained in the implementation notes as history and do not measure the current Result path. Other platforms and native GPU behavior require their own direct evidence.

See the maintained [public example and commands](../../../../examples/numeric_workflow/README.md#lut1d-baking-templates-crv-04).
The source specifications remain Proposed independently of implementation.
CRV-05 now executes the scalar/channels LUT consumer chains and their separate
discretization acceptance.

- [NUM-01 expression](NUM-01_sample_expression.md).
- [NUM-02A linspace](NUM-02A_linspace.md).
- [CRV-01 interpolation family](CRV-01_interpolate.md).
- [CRV-02 Bezier function](CRV-02_sample_bezier_function.md).
- [Operator template](../../00-foundation/spec-template.md).
