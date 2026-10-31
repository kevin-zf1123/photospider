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
clarification_status: complete
---

# CRV-04A: bake_lut1d_expression

Numeric profile: strict retains the exact reference defined below. Floating arithmetic in accelerated profiles follows the shared [final FP32 four-ULP contract](NUM_accelerated_contract.md). Discrete results, copies, selected endpoints and special values remain exact.

Inherit the [baking template contract](CRV-04_bake_lut1d.md) in full for authoring profile/defaults, count/dtype, named outputs and graph-equivalence acceptance. This is a workflow construction template; its name is not an OperationRegistry key.

## Interface and expansion

Dynamic inputs are start, end, followed by the named coefficient inputs in the expression contract. Input shapes/dtypes and constraints follow the [source specification](NUM-01_sample_expression.md); start/end are independent Float32/Float64 [1] scalars. Authoring requires expression and count, with count in [1,1048576]. dtype defaults to float64; profile is strict/apple_silicon/x86_64/fast, default strict. The constructor derives canonical coefficient_names from the expression under NUM-01's rules, rather than requiring the caller to repeat the identifiers. The expanded node receives all required expression, coefficient_names, count and dtype parameters explicitly. For example, expression="a*x+b" derives the ordered coefficient names a,b and matching edges.

The expansion is one numeric.sample_expression node with the selected profile suffix, with the dynamic inputs connected in its documented port order and its explicit static parameters. That node's values and axis are exported directly. No adapter or additional sampling node is inserted.

Each exported shape/dtype is inferred by its expanded source. This single-function template returns values[count] without a component axis. Axis is always Float64 [3]. For count=1 it is [start,start,0] and no end payload is evaluated. The source's remaining domain, finite-value, precision and query-support restrictions are not normalized or weakened.

## Native fast numerical profile

`CpuNumericProfile::Fast` selects this template's fast node expansion and native coordinate/source formulas in the [family profile](CRV-04_bake_lut1d.md#native-fast-numerical-profile). The constructor remains a graph helper and adds no runtime bake key.

## Acceptance

Analytic fixture: expression="x^2", start=[0], end=[1], count=3 -> values=[0,0.25,1], axis=[0,1,0.5].

Construct this template and its explicit expansion with the same bindings, parameters and output demands, and compare descriptors, numerical quality and active input dependencies according to the source spec. Repeat each valid profile, values-only/axis-only/joint requests, singleton count and partial output. Templates do not compute on construction, freeze results or create files. Source mathematical fixtures are checked independently of graph equivalence, which alone could reproduce a shared numerical bug.
