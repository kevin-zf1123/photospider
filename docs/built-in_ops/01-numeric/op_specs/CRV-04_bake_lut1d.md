---
spec_schema_version: 1
id: CRV-04
kind: shared_workflow_contract
category: 01-numeric
status: Proposed
document_maturity: D1_draft
implementation_status: implemented
clarification_status: complete
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
All six public constructors author graph metadata and expand to fixed execution. LUT consumers have separate contracts.

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

Construction only authors metadata and node references. The returned `BakedLut1d` contains `WorkflowNodeOutput` references for `values` and `axis`; its `outputs` method creates caller-named `WorkflowOutput` declarations. The caller chooses the demand and executes the graph through the ordinary Result workflow. Baked outputs are generic Result tensors. Consumers check static channel/dtype compatibility; callers must also satisfy their numerical domain and ordering preconditions. CRV-05 L>1 declares all table rows for selected columns, so the producer must satisfy its own numerical requirements across that physical closure. Its L=1 path keeps the axis schema but omits axis payload; separately requested outputs can still require the axis producer.

## Inherited semantic differences

Template convenience does not introduce new numerical validation or silently
normalize incompatible source semantics. NUM-01 requires distinct endpoints and sufficient selected-coordinate precision as caller preconditions. CRV-02 requires global valid x topology and selected-coordinate/domain precision as caller preconditions.
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

The expression template inherits fixed NUM-01 execution. Each selected values tile is a task with independent dense storage and preallocated arithmetic scratch; axis is an atomic 24-byte task. Count one omits end payload, and axis-only omits all coefficients. Full static schemas remain checked. Larger counts read both endpoints even for the first values tile. Empty selection creates no pure payload task. Numerical domains and coordinate precision are caller preconditions; resource, source, backend and cancellation failures retain their operational categories. When both outputs are selected, they share one IR call's publication outcome.

Changing bindings reuses immutable static preparation and creates new Run-owned results. There is no content cache or repeated-demand identity guarantee. Callers bind immutable Result sources through `ExecutionBindings`, prepare the Plan, submit a Run and read named `ExecutionResult.results` after join. Successful Result owners and read windows can retain allocations beyond context lifetime.

Interpolation expansions use fixed linspace and curve tasks. Sparse values propagate only their required query tile closure to linspace and allocate selected curve output tiles. Axis-only selection excludes curve inputs and execution. Axis belongs to the linspace call and values to the curve call; they have separate call retirement outcomes. Bezier expansions use fixed values tasks with complete control support and a separate scalar-only axis task. Count one omits end, and axis-only omits controls. Selected outputs share the sampler call's publication outcome. Resource and dependency rules for executable families come from their expanded operation contracts; a constructor adds no hidden evaluation or storage.

## Acceptance and implementation boundary

For every template and CPU profile, construct the generated graph and its explicit WorkflowDocument expansion through the public authoring APIs. Compare output schemas and coverage, requested Result bits, analytic fixture values and fixed source-tile support. Exercise both table dtypes and the four demand modes: full values, axis-only, joint values and axis, and sparse values. Keep template-equivalence checks distinct from the numeric oracles that validate the underlying samplers and interpolators.

For expression, Bezier and interpolation expansions, cover count one with a failing end producer, empty selection, axis-only with failing coefficients, mixed dtypes, static preparation reuse after binding changes, sparse maximum count, resource rejection, cancellation and output lifetime. Compare generated and explicit graphs through actual fixed public execution. For Bezier, also cover axis-only with failing control producers and explicit graph equivalence.

No additional numeric primitive is introduced. Numerical arithmetic and independent numeric oracles for the underlying operations remain in NUM-01/02 and CRV-01/02; generated/explicit graph equivalence alone does not prove their numerical contracts. Other platforms and native GPU behavior require their own direct evidence.

The source specifications remain Proposed independently of implementation.
CRV-05 defines the scalar/channels LUT consumer chains and separate discretization acceptance.

- [NUM-01 expression](NUM-01_sample_expression.md).
- [NUM-02A linspace](NUM-02A_linspace.md).
- [CRV-01 interpolation family](CRV-01_interpolate.md).
- [CRV-02 Bezier function](CRV-02_sample_bezier_function.md).
- [Operator template](../../00-foundation/spec-template.md).
