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

Numeric profile: strict retains the exact reference defined below. Floating arithmetic in accelerated profiles follows the shared [final FP32 four-ULP contract](NUM_accelerated_contract.md). Discrete results, copies, selected endpoints and special values remain exact.

Inherit the [baking template contract](CRV-04_bake_lut1d.md) in full for authoring profile/defaults, count/dtype, named outputs and graph-equivalence acceptance. This is a workflow construction template; its name is not an OperationRegistry key.

## Interface and expansion

Dynamic inputs are anchors, handles, start and end. Input shapes/dtypes and source-specific constraints follow the [source specification](CRV-02_sample_bezier_function.md), with start/end supplied as independent Float32/Float64 [1] scalars. Required source/sampling parameters are degree, count, dtype and out_of_domain. The template adds authoring profile=strict/apple_silicon/x86_64, default strict. Count is required in [1,1048576], dtype defaults to float64, and out_of_domain defaults to reject. Source parameters with no stated default remain required. Constructors write expanded-node parameters explicitly.

The expansion is one curve.sample_bezier_function node with the selected profile suffix, with the dynamic inputs connected in its documented port order and its explicit static parameters. That node's values and axis are exported directly. No adapter or additional sampling node is inserted.

Each exported shape/dtype is inferred by its expanded source. This scalar template returns values[count] without a component axis. Axis is always Float64 [3]. For count=1 it is [start,start,0] and no end payload is evaluated. The source's remaining domain, finite-value, precision and query-support restrictions are not normalized or weakened.

## Behavior

Values and axis are independent outputs of the same sampler. Axis-only needs no controls; count one needs no end for either output. Values depend on the complete controls. Global x topology, selected numerical domains, reconstruction/output representability and sampling precision are caller preconditions from CRV-02.

When connected to CRV-05 with L>1, the consumer reads all table rows, so baking must satisfy its requirements across every producer row even for a sparse LUT query. L=1 can omit the sampler's axis and end payload; its values still require controls and start. Consumers retain their own axis and domain preconditions. Discrete baking supplies no error bound relative to the continuous function.

## Acceptance

Analytic fixture: quadratic anchors=[[0,0],[1,1]], handles=[[[0.5,0]]], start=[0], end=[1], count=3 -> values=[0,0.25,1], axis=[0,1,0.5].

Construct this template and its explicit expansion with the same bindings, parameters and output demands, and compare descriptors, numerical quality and active input dependencies according to the source spec. Repeat each valid profile, values-only/axis-only/joint requests, singleton count and partial output. Templates do not compute on construction, freeze results or create files. Source mathematical fixtures are checked independently of graph equivalence, which alone could reproduce a shared numerical bug.
