---
spec_schema_version: 1
id: CRV-04A
parent_id: CRV-04
function: bake_lut1d_expression
proposed_template_names:
  - bake_lut1d_expression
category: 01-numeric
kind: composite_workflow
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

# CRV-04A: bake_lut1d_expression

Numeric profile: strict retains the exact reference defined below. Floating
arithmetic in accelerated profiles follows the shared
[final FP32 four-ULP contract](NUM_accelerated_contract.md), including its
range/fallback rules. Discrete results, copies, selected endpoints and special
values remain exact.

Inherit the [baking template contract](CRV-04_bake_lut1d.md) in full for authoring
profile/defaults, count/dtype, named outputs, Whole execution, resources, errors,
owner lifetime and public graph-equivalence acceptance. This is a workflow
construction template; its name is not an OperationRegistry key.

## Inputs, parameters and expansion

Expose dynamic start, end, followed by the named coefficient inputs in the
expression contract. Input shapes/dtypes and constraints follow the
[source specification](NUM-01_sample_expression.md); start/end are independent
Float32/Float64 [1] scalars. Authoring requires expression and count, with
count in [1,1048576]. dtype defaults to float64; profile is
strict/apple_silicon/x86_64, default strict. The constructor derives canonical
coefficient_names from the expression under NUM-01's rules, rather than requiring
the caller to repeat the identifiers. The expanded node receives all required
expression, coefficient_names, count and dtype parameters explicitly. For example,
expression="a*x+b" derives the ordered coefficient names a,b and matching edges.

Create numeric.sample_expression with the selected profile suffix. Connect the dynamic
inputs in its documented port order and pass its explicit static parameters.
Export that node's values and axis directly. No adapter or additional sampling
node is inserted.

Each exported shape/dtype is inferred by its expanded source. This single-function
template returns values[count] without a component axis. Axis is always Float64
[3]. For count=1 it is [start,start,0]
and no end payload is evaluated. The source's remaining domain, finite-value,
precision and query-support restrictions are not normalized or weakened.

## Acceptance and status

Executed analytic fixture: expression="x^2", start=[0], end=[1], count=3 -> values=[0,0.25,1], axis=[0,1,0.5].

Construct this template and its explicit expansion with the same bindings,
parameters and output demands. Compare descriptors, numerical quality and exact
read/dirty witnesses according to the source spec. Repeat each valid profile,
values-only/axis-only/joint requests, changed source bindings, singleton count,
partial output, low shared budgets, cancellation, cache-off and exported-owner
lifetime. Templates do not compute on construction, freeze results or create files.
Use source mathematical fixtures independently of graph equivalence, which alone
could reproduce a shared numerical bug. The maintained public constructor in `photospider/numeric/lut1d.hpp` and
`examples/numeric_workflow/baking.cpp` execute this fixture. Current native Clang
strict/Apple passed the Whole template manual acceptance described in [CRV-04](CRV-04_bake_lut1d.md). The example is
excluded from default builds and CTest/integration registration. See the
[numeric workflow README](../../../../examples/numeric_workflow/README.md#lut1d-baking-templates-crv-04)
for build/run commands and editable public-API use.
