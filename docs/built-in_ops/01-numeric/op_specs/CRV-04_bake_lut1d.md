---
spec_schema_version: 1
id: CRV-04
kind: shared_workflow_contract
category: 01-numeric
status: Proposed
document_maturity: D1_draft
implementation_status: not_implemented
clarification_status: complete
repository_branch: ops-specs
repository_commit: 6617c78c
---

# CRV-04: bake_lut1d workflow templates

## Confirmed scope and composition boundary

Provide named composite workflow authoring templates for baking a function into
sampled table values and a sampling axis. Reuse the already specified samplers
instead of registering a duplicate numeric primitive. Mathematical behavior
and precision are inherited from the expanded operations, not redefined by an
opaque bake callback or a new runtime function-object type.

Available building blocks include expression sampling (NUM-01), a Bezier
function sampler (CRV-02), and linspace query generation followed by CRV-01
interpolation. The six concrete source templates inherit this completed contract.
This target does not imply that the proposed building-block
keys already execute or that the legacy LUT consumer accepts the new axis input.

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

Authoring names in individual files identify templates, not OperationRegistry
keys. Expansion creates ordinary WorkflowDocument nodes with distinct node IDs,
explicit static parameters, declared dynamic edges and named WorkflowOutput
port references. profile maps to key suffix _strict,
_accelerated_apple_silicon or _accelerated_x86_64 for every expanded node.
It adds no profile parameter to their operation schemas. Constructors allocate
collision-free node identifiers under the host's normal authoring convention.

Construction performs no numerical evaluation. values and axis remain independently
demanded outputs with optional ordinary host joint execution. A caller requesting
a complete table explicitly requests the entire values output and whichever axis
data it needs. The template does not save a file, freeze a result, attach persistent
identity or force evaluation during construction. Dynamic control changes use
normal invalidation/reexecution. All exported arrays are generic Values; consumers
must enforce their own LUT domain, ordering, channel and dtype compatibility.

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
sample_expression strict is stepwise Float64; linear strict/accelerated are
whole-formula bit-identical; PCHIP and Bezier accelerated follow their respective
final-error contracts. A shared authoring profile does not equate those formulas.
Baking discrete samples supplies no automatic bound on the later LUT consumer's
interpolation error relative to a continuous function.

## Demand, resources, errors and lifetime

Compose the exact per-port Data/Control/Validation/Descriptor demand of the
expanded graph. Axis-only queries do not read expression coefficients, Bezier
controls or interpolation x/y. Interpolator values queries request only the
needed linspace values; values failure does not redefine a separately requested
axis as failed. No template-only Whole pass or joint-output dependency is added.
Source-global x/topology checks remain required exactly where their own specs
declare them. Dirty mapping is the ordinary composition of those witnesses.

Budget the simultaneous nodes, source owners, Float64 query fragments, control
indices, exact arithmetic and returned buffers through the same host root.
For M demanded interpolation query positions, the query payload can be 8M bytes,
plus the source node's stated scratch and table outputs; deduplicate row queries
across requested columns. The graph need not retain a full count-sized query
array for sparse demand. No separate private cache or disk backing is required.
Mandatory active ownership survives cache-off. Returned owners and any exported
axis owner obey underlying post-context lifetime and final-release rules.

Missing authoring parameters or invalid profile fail template construction with
InvalidArgument; missing required runtime bindings and invalid expanded-node
schemas fail existing compiler/preflight validation. Numerical, backend, resource,
cancellation, stale and upstream errors retain their producing node/input,
reason and scope. Do not rename a source failure into a generic BakeFailed
enum or claim per-observation isolation unavailable to the compiled plan.
Cancellation reaches every expanded node under ordinary scheduling; failed
observations publish no partial successful Value. Earlier independent outcomes
retain host terminal semantics.

## Acceptance and implementation boundary

For every template, construct both the generated graph and the explicit graph
listed above through public WorkflowDocument APIs. Compare output descriptors,
requested result bits/tolerances, read witnesses, errors and dirty support for
each profile. Test all outputs alone/jointly, full/partial requests, changed dynamic
inputs, count=1 with failing end, source-specific domain constraints, mixed dtype,
cache-off, low root budget, cancellation and exported-owner lifetime.

Check source sampler fixtures as well as downstream approximation explicitly:
sampling x^2 at [0,0.5,1] yields [0,0.25,1], but a later piecewise-linear LUT
evaluation at 0.25 yields 0.125 rather than the continuous value 0.0625. This is
expected discretization error, not a failure of correctly rounded sample values.
No file should be created and no input producer run by template construction.

The template authoring APIs, the expanded target registry implementations and
actual runnable public fixtures are not implemented by these documents. Delivery
must provide real build/run commands and observed results. These six specifications
complete clarification while remaining Proposed; no runtime/performance success
is inferred from a conceptual DAG or a mathematical oracle check.

- [NUM-01 expression](NUM-01_sample_expression.md).
- [NUM-02A linspace](NUM-02A_linspace.md).
- [CRV-01 interpolation family](CRV-01_interpolate.md).
- [CRV-02 Bezier function](CRV-02_sample_bezier_function.md).
- [Operator template](../../00-foundation/spec-template.md).
