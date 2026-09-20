---
spec_schema_version: 1
id: CRV-04E
parent_id: CRV-04
function: bake_lut1d_linear_multi
proposed_template_names:
  - bake_lut1d_linear_multi
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

# CRV-04E: bake_lut1d_linear_multi

Numeric profile: strict retains the exact reference defined below. Floating
arithmetic in accelerated profiles follows the shared
[final FP32 four-ULP contract](NUM_accelerated_contract.md), including its
range/fallback rules. Discrete results, copies, selected endpoints and special
values remain exact.

Inherit the [baking template contract](CRV-04_bake_lut1d.md) in full for authoring
profile/defaults, count/dtype, named outputs, lazy demand, resources, errors,
owner lifetime and public graph-equivalence acceptance. This is a workflow
construction template; its name is not an OperationRegistry key.

## Inputs, parameters and expansion

Expose dynamic x, y, start and end. Input shapes/dtypes and source-specific constraints
follow the [source specification](CRV-01C_interpolate_linear_multi.md), with start/end supplied as independent
Float32/Float64 [1] scalars. Required source/sampling parameters are count, dtype and out_of_domain.
The template adds authoring profile=strict/apple_silicon/x86_64, default strict.
Count is explicitly required in [1,1048576], dtype defaults to float64, and
out_of_domain where present defaults to reject. Source parameters with no stated
default remain required. Constructors write expanded-node parameters explicitly.

Create numeric.linspace with the selected profile suffix, start/end inputs,
count and fixed dtype="float64". Create curve.interpolate_linear_multi with the same profile
suffix; connect x/y and linspace.values to query, and pass the final dtype and
out_of_domain. Export interpolator.values and linspace.axis. No full query
materialization or implicit domain adjustment is introduced by this expansion.

Each exported shape/dtype is inferred by its expanded source. Scalar values
retain [count]; multi-function values retain [count,C], including C=1. This
template uses the applicable source shape without inserting or removing a
component axis. Axis is always Float64 [3]. For count=1 it is [start,start,0]
and no end payload is evaluated. The source's remaining domain, finite-value,
precision and query-support restrictions are not normalized or weakened.

## Acceptance and status

Executed analytic fixture: x=[0,1], y=[[0,10],[2,8]], start=[0], end=[1], count=3 -> values=[[0,10],[1,9],[2,8]], axis=[0,1,0.5].

Construct this template and its explicit expansion with the same bindings,
parameters and output demands. Compare descriptors, numerical quality and exact
read/dirty witnesses according to the source spec. Repeat each valid profile,
values-only/axis-only/joint requests, changed source bindings, singleton count,
partial output, low shared budgets, cancellation, cache-off and exported-owner
lifetime. Templates do not compute on construction, freeze results or create files.
Use source mathematical fixtures independently of graph equivalence, which alone
could reproduce a shared numerical bug. The maintained public constructor in `photospider/numeric/lut1d.hpp` and
`examples/numeric_workflow/baking.cpp` execute this fixture. Native Clang
strict/Apple, WSL Clang strict/AVX2 and installed consumers passed the shared
manual acceptance described in [CRV-04](CRV-04_bake_lut1d.md). The example is
excluded from default builds and CTest/integration registration. See the
[numeric workflow README](../../../../examples/numeric_workflow/README.md#lut1d-baking-templates-crv-04)
for build/run commands and editable public-API use.
