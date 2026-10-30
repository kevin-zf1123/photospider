---
spec_schema_version: 1
id: NUM-common
kind: shared_operator_contract
category: 01-numeric
status: Proposed
document_maturity: D1_draft
implementation_status: implemented
---

# NUM: shared numeric contract

The [FMT explicit-configuration contract](../../02-format-color/op_specs/FMT_common_contract.md) keeps domain interpretation outside Result/Tensor. Numeric operations receive structure and samples; any color, alpha, sampling or LUT interpretation required by an operation is a declared static parameter. Outputs carry no domain description.

The [relative-coordinate scale contract](../../02-format-color/op_specs/FMT_relative_coordinate_scale.md) sets native CIELAB/CIELCh lightness to l=L*/100. NUM/CRV consumers use TensorDescription v4/v5 logical tuple parameters with that convention; there is no implicit L* compatibility.

Numeric profile: strict is the exact reference defined by each operation. Floating arithmetic in accelerated profiles follows the shared [final FP32 four-ULP contract](NUM_accelerated_contract.md). Discrete results, copies, selected endpoints and special values are exact in every profile.

This contract is inherited by the NUM-01..NUM-15 and CRV-02 files in this directory, including their family contracts. An individual operation's explicit rule takes precedence over its family contract and this one. Related-file links alone do not import another operation's ports or numerical semantics. The [operator template](../../00-foundation/spec-template.md) and [foundation contracts](../../00-foundation/contracts.md) define the document requirements.

## Static structure and unchecked premises

Compilation and static preparation infer and check schema identity/version, member directories, dtype, rank, extents, axes, shape relations, parameter types and syntax, and fixed read/write signatures. Static capacity arithmetic is checked.

Runtime payload structure (counts, offsets, indices, identities, associations, canonical padding/encoding) and numerical/geometric conditions are unchecked caller premises. A numerical parameter's presence at preparation time does not turn its finite/range/order/positivity condition into a structural check. Producers construct results according to the contract without a final validation scan; consumers trust those premises without rescanning or comparing payloads. Outside a mathematical or payload premise no diagnostic, successful result or recovery is promised.

Precision refinement, candidate selection, explicit clipping, error measurement and an explicitly requested quality gate remain their named computations. A quality gate consumes the supplied outcome; it does not revalidate the report or its association with other inputs. Raw IEEE rules for NaN, infinities and signed zeros remain as specified. This boundary applies throughout NUM/CRV, including SDK reports.

## Registration and numeric profiles

Front matter separates specification status, document maturity and implementation status. D1_draft identifies a draft of the D1 mathematical scope. Shared contracts register no operation.

Each primitive has three independently registered CPU entries: strict, Apple Silicon CPU accelerated, and x86-64 CPU accelerated. No implicit mode dispatcher, unsuffixed alias, GPU path or new runtime dtype is defined. Strict is portable across the CPU targets supported by a conforming build. An accelerated key on an incompatible target fails BackendUnavailable.

The [accelerated contract](NUM_accelerated_contract.md) defines final FP32-scaled accuracy for floating arithmetic throughout NUM and CRV. Copies, predicates, indices and selected endpoints are exact. Strict formulas keep their specified rounding boundaries. Finite rounding defaults to nearest/ties-to-even with gradual underflow. The caller's floating environment, including prior flags, is preserved.

## Interface and descriptor conventions

Runtime operation ports are Results, required unless explicitly stated otherwise. A numeric input normally contains one tensor; individual contracts define the allowed schema and any field-only reports. Input and output order is the listed order; single-output specifications use values unless explicitly named otherwise. Each file supplies the shape, dtype and static parameters or names the family from which it inherits them.

Rank is 1..8, extents are positive and generic arrays have at most 2^40 logical elements; the smaller count/K limits in generator specs take precedence. Shape products use checked integer arithmetic during static inference. Dynamic indices and address-control values satisfy unchecked caller premises. Outputs have no attached domain description or implicit physical units, colors or alpha roles.

Static parameters are supplied explicitly in direct WorkflowDocument nodes; authoring defaults are written by constructors. An empty parameter set means no static operation parameters. Unknown, missing or wrongly typed parameters are rejected before sample reads.

Canonical decimal list Strings use ASCII base-10 entries separated by single commas, without whitespace, plus signs, leading zeros, empty entries or a trailing comma. An entry is 0 or starts with 1..9; shape/count entries must be positive, axis entries may be zero. Constructors serialize this form; direct noncanonical input is rejected. Axis sets are sorted increasingly by constructors; permutations and shapes keep their meaningful list order. This grammar does not apply to expression source text or coefficient identifiers.

Compile/preflight validates all declared input descriptors, including unused edges. Output descriptors depend only on input descriptors and declared static parameters; no numeric producer is executed to infer a descriptor. A scalar or an axis array carrying runtime values is Data/Control, not descriptor metadata. A changed shape/dtype/parameter requires new inference.

## Demand, errors and returned observations

Q denotes requested logical output coordinates, independent of physical stride. Individual files define exact per-port Data and Control sets. Descriptor demand covers the static input metadata needed by compilation. Empty Q introduces no runtime payload reads. Dynamic controls retain witnesses even when they do not change returned values. An operator does not enlarge its support to a bounding gap. Missing coverage is not an implicit zero.

The following default mapping applies where an individual file has not supplied a more specific Status. Names follow the [public status types](../../../../include/photospider/core/status.hpp).

| Failure | Phase | Code / reason | Attribution |
| --- | --- | --- | --- |
| Missing, unknown or malformed static parameter; invalid structural axis/shape parameter | Compile or direct preflight | InvalidArgument / InvalidDomain | Schema origin |
| Unsupported port dtype, rank, shape relation or output descriptor | Compile or direct preflight | TypeMismatch / None | Schema origin |
| Violated runtime structure or numerical/geometric premise | Outside the operator contract | No validation or diagnostic guarantee | Caller responsibility |
| Unsupported accelerated target | Capability check | BackendUnavailable / None | Backend origin |
| Admitted capacity, work or stage exhausted | Admission or execution | ResourceExhausted / CapacityLimit, WorkLimit or StageLimit | Host resource scope |
| Cancellation, stale input or upstream failure | Respective host phase | Preserve original Status | Original origin, scope and source |

A failed call publishes no partial successful Result. Results already delivered from earlier retired calls remain valid; a failed Run returns no final RET map.

## SDK numeric reports

`NumericDiagnostics` and its classification codec are declared in `photospider/ops/numeric/diagnostics.hpp` and belong to `Photospider::ops_sdk`. `register_numeric_diagnostics` registers the fixed 588-byte codec for explicit `DiagnosticAccumulator` use. Operations do not automatically emit per-operation numeric reports. Payload identity/counter consistency and canonical encoding are caller premises. `read_numeric_report` decodes a supplied complete 588-byte payload without checking it; `merge_numeric_diagnostics` computes totals and attribution, returning counter-capacity exhaustion without mutating the destination.

```cpp
// payload is a complete SDK numeric report supplied under the caller premises.
auto decoded = ps::read_numeric_report(payload);
if (!decoded.ok()) return decoded.status();
auto status = ps::merge_numeric_diagnostics(&total, decoded.value());
if (!status.ok()) return status;
```

Reports describe actual arithmetic work when explicitly collected. Their values do not certify sample domains, report association or a numerical accuracy bound.

## Acceptance

Each primitive's fixture supplies a minimal public workflow. Strict results are compared by exact bits, accelerated results by the explicitly allowed ULP rule, against an independent integer/rational, bit-pattern or directed-precision reference. Required tile-read closure is verified separately from numeric equality. Relevant shape, special-value and layout cases are listed in the primitive and family contracts.
