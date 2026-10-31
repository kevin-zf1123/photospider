---
spec_schema_version: 1
id: CRV-04C
parent_id: CRV-04
function: bake_lut1d_linear
proposed_template_names:
  - bake_lut1d_linear
category: 01-numeric
kind: composite_workflow
status: Proposed
document_maturity: D1_draft
implementation_status: implemented
clarification_status: complete
---

# CRV-04C: bake_lut1d_linear

Numeric profile: strict retains the exact reference defined below. Floating arithmetic in accelerated profiles follows the shared [final FP32 four-ULP contract](NUM_accelerated_contract.md). Discrete results, copies, selected endpoints and special values remain exact.

Inherit the [baking template contract](CRV-04_bake_lut1d.md) in full for authoring profile/defaults, count/dtype, named outputs and graph-equivalence acceptance. This is a workflow construction template; its name is not an OperationRegistry key.

## Interface and expansion

Dynamic inputs are x, y, start and end. Input shapes/dtypes and source-specific constraints follow the [source specification](CRV-01A_interpolate_linear.md), with start/end supplied as independent Float32/Float64 [1] scalars. Required source/sampling parameters are count, dtype and out_of_domain. The template adds authoring profile=strict/apple_silicon/x86_64, default strict. Count is required in [1,1048576], dtype defaults to float64, and out_of_domain defaults to reject. Source parameters with no stated default remain required. Constructors write expanded-node parameters explicitly.

The expansion is numeric.linspace with the selected profile suffix, the start/end inputs, count and fixed dtype="float64", followed by curve.interpolate_linear with the same profile suffix; x/y and linspace.values (as query) are connected, and the final dtype and out_of_domain are passed. The exports are interpolator.values and linspace.axis. Values depend on the Float64 linspace coordinates of the selected output rows; axis-only requests need only the linspace axis.

Each exported shape/dtype is inferred by its expanded source. This scalar template returns values[count] without a component axis. Axis is always Float64 [3]. For count=1 it is [start,start,0] and no end payload is evaluated. The source's remaining domain, finite-value, precision and query-support restrictions are not normalized or weakened.

## Native fast numerical profile

`CpuNumericProfile::Fast` selects this template's fast node expansion and native coordinate/source formulas in the [family profile](CRV-04_bake_lut1d.md#native-fast-numerical-profile). The constructor remains a graph helper and adds no runtime bake key.

## Acceptance

Analytic fixture: x=[0,1,2], y=[0,2,4], start=[0], end=[2], count=3 -> values=[0,2,4], axis=[0,2,1].

Construct this template and its explicit expansion with the same bindings, parameters and output demands, and compare descriptors, numerical quality and active input dependencies according to the source spec. Repeat each valid profile, values-only/axis-only/joint requests, singleton count and partial output. Templates do not compute on construction, freeze results or create files. Source mathematical fixtures are checked independently of graph equivalence, which alone could reproduce a shared numerical bug.
