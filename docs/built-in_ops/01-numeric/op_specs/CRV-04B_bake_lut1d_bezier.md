---
spec_schema_version: 1
id: CRV-04B
parent_id: CRV-04
function: bake_lut1d_bezier
proposed_template_names:
  - bake_lut1d_bezier
category: 01-numeric
kind: composite_workflow
status: Proposed
document_maturity: D1_draft
implementation_status: implemented
clarification_status: complete
---

# CRV-04B: bake_lut1d_bezier

Numeric profile: strict retains the exact reference defined below. Floating
arithmetic in accelerated profiles follows the shared
[final FP32 four-ULP contract](NUM_accelerated_contract.md), including its
range/fallback rules. Discrete results, copies, selected endpoints and special
values remain exact.

Inherit the [baking template contract](CRV-04_bake_lut1d.md) in full for authoring
profile/defaults, count/dtype, named outputs, fixed source execution, resources, errors,
owner lifetime and public graph-equivalence acceptance. This is a workflow
construction template; its name is not an OperationRegistry key.

## Inputs, parameters and expansion

Expose dynamic anchors, handles, start and end. Input shapes/dtypes and source-specific constraints
follow the [source specification](CRV-02_sample_bezier_function.md), with start/end supplied as independent
Float32/Float64 [1] scalars. Required source/sampling parameters are degree, count, dtype and out_of_domain.
The template adds authoring profile=strict/apple_silicon/x86_64, default strict.
Count is explicitly required in [1,1048576], dtype defaults to float64, and
out_of_domain where present defaults to reject. Source parameters with no stated
default remain required. Constructors write expanded-node parameters explicitly.

Create curve.sample_bezier_function with the selected profile suffix. Connect the dynamic
inputs in its documented port order and pass its explicit static parameters.
Export that node's values and axis directly. No adapter or additional sampling
node is inserted.

Each exported shape/dtype is inferred by its expanded source. This scalar
template returns values[count] without a component axis. Axis is always
Float64 [3]. For count=1 it is [start,start,0]
and no end payload is evaluated. The source's remaining domain, finite-value,
precision and query-support restrictions are not normalized or weakened.

## Fixed execution and consumer requirements

Values and axis are independently selected fixed tasks of the same source call. Axis-only excludes controls; count one excludes end from both outputs and uses zero scratch for axis. Values compute only selected tiles with fixed arithmetic scratch and declare complete controls. Global x topology, selected numerical domains, reconstruction/output representability and sampling precision are caller preconditions from CRV-02. Joint selection shares the call's publication outcome.

When connected to CRV-05 with L>1, the consumer declares all table rows, so baking must satisfy its requirements across every selected producer row even for a sparse LUT query. L=1 can omit the sampler's axis task and end payload; its values still require controls and start. Consumers retain their own axis and domain preconditions. Discrete baking supplies no error bound relative to the continuous function.

## Acceptance and status

Analytic fixture: quadratic anchors=[[0,0],[1,1]], handles=[[[0.5,0]]], start=[0], end=[1], count=3 -> values=[0,0.25,1], axis=[0,1,0.5].

Construct this template and its explicit expansion with the same bindings,
parameters and output demands. Compare descriptors, numerical quality and exact
physical read support according to the source spec. Repeat each valid profile,
values-only/axis-only/joint requests, changed source bindings, singleton count,
partial output, explicit resource budgets, cancellation and exported-owner
lifetime. Templates do not compute on construction, freeze results or create files.
Use source mathematical fixtures independently of graph equivalence, which alone
could reproduce a shared numerical bug.
