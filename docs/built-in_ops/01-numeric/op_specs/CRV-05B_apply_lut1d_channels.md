---
spec_schema_version: 1
id: CRV-05B
parent_id: CRV-05
function: apply_lut1d_channels
operation_family: curve.apply_lut1d_channels
proposed_operation_keys:
  - curve.apply_lut1d_channels_strict
  - curve.apply_lut1d_channels_accelerated_apple_silicon
  - curve.apply_lut1d_channels_accelerated_x86_64
category: 01-numeric
kind: primitive
status: Proposed
document_maturity: D1_draft
implementation_status: implemented
verification_status: focused_result_math_ctest_and_installed_consumer
clarification_status: complete
repository_branch: ops-specs
repository_commit: current working tree
---

# CRV-05B: apply_lut1d_channels

Numeric profile: strict retains the exact reference defined below. Floating
arithmetic in accelerated profiles follows the shared
[final FP32 four-ULP contract](NUM_accelerated_contract.md), including its
range/fallback rules. Discrete results, copies, selected endpoints and special
values remain exact.

## Confirmed channel interface

Inputs in order are Result tensors input[...,C], table[L,C] and a shared Float64
axis[3]. Each Result contains one tensor member under any schema id/version/key;
shapes use complete `sample_shape()` including batch axes. The final input axis is
the channel axis and its length equals the table's second extent. Each input[...,c]
uses only table[:,c]. Output port `values` is a Result using schema
`photospider.tensor` v1/member `samples`, preserving the complete input shape,
including C=1, with selected dtype and empty facets.
It does not map one scalar to C outputs; that is the separate color-ramp family.

Inherit [single-table CRV-05A](CRV-05A_apply_lut1d.md) for floating dtype rules,
output default matching the authoring `input_type` hint (independent of compiled
edge validation), complete shared-axis validation, descending axes,
singleton tables, three domain modes, strict whole-formula interpolation, exact
zero/selection rules and the shared accelerated final FP32 bound. Each input Result's tensor has one dtype; channels do not carry independently
selected dtypes or axes. Input and table tensor shapes come from full
`sample_shape()` including batch axes. Output port `values` uses
`photospider.tensor` v1/member `samples`, with the complete input shape, selected
Float32/Float64 dtype and empty facets.
Require C>=1, 1<=L<=1048576, positive input extents and input rank 1..8.
A rank-1 input [C] is one vector. Input and table logical element counts must
each be <=2^40, with products checked without overflow. There is no additional
small C limit. Input Float32/Float64 and table Float32/Float64 may differ.

## Static parameters, inference and demand

Static dtype and out_of_domain inherit CRV-05A's required String encodings,
constructor defaults and explicit direct-node parameters. There is no channel_axis,
per-channel dtype, per-channel policy or independent-axis mode. Validate the
table/input C relation at compile/preflight; output shape/dtype are static.

Every nonempty Whole request requests complete input, table and axis Results
with Data, Validation and Descriptor (role 13), including typed validation and
upstream failures. It validates the complete reconstructed axis first, then every
finite/domain query before table arithmetic. It computes every output element
and channel and publishes one immutable packed dense Result of the complete input shape,
with full certified coverage and global coordinates. Its association records
actual source ObjectIds. Empty reads no payload; static metadata checks still
apply.

Mathematical table selection is unchanged: knot/clamp/singleton converts one
entry; interpolation/extrapolation uses its adjacent pair, independently per
channel. Generic entries outside all evaluated stencils receive no additional
finite scan. Typed validation and upstream collection cover the complete table.
Errors in otherwise-unrequested queries or evaluated channels can fail the Run.
Any input/table/axis edit invalidates recorded output demand; dirty mapping
follows that recorded demand. Cache identity retains all input versions, profile,
metadata and parameters. There is no
per-channel Atom success isolation. Output dtype, shape and empty facets remain
unchanged; arbitrary input offsets, unaligned and signed/zero strides remain
legal. The complete immutable owner survives context destruction.

## Algorithm, resources and errors

For M input elements, work is O(L+M log L) plus exact arithmetic and typed
validation. Singleton lookup is constant time per query. `UniformAxis` stores
the directly reconstructed Float64 grid in a Root-owned, host-accounted 8*L-byte
buffer (up to 8 MiB), plus allocator/metadata overhead; fixed exact workspace
and O(rank) coordinate state are also admitted. Input/table reads use authorized zero-copy Root windows over source storage; the operation does not
create a dense copy of the complete table merely to collect it. Root accounts
actual retained owners and provided windows. The operator retains no per-output certificates, rows or
lookup table. Output costs dtype_bytes*M, regardless of requested coverage.

The complete axis uses endpoint-weighted RN64 coordinates. One caller-preserving
floating environment covers axis reconstruction and curve evaluation; unresolved
accelerated bounds still use the same exact fallback. Work/cancellation is checked
in axis generation, input reads, lookup and exact arithmetic, and before publishing.
Resource failure never authorizes skipping axis validation or weaker arithmetic.
Unpublished output/workspace is released; the output Result transaction is
all-or-nothing.
Numeric InvalidDomain/ArithmeticOverflow failures have Run scope. Typed, upstream,
resource, stale, backend and cancellation failures retain their categories.
Whole does not expose per-value fallback counters; report those as unavailable.

Each channel may have a different input query; sharing a row prefix does not
permit sharing the selected table index. Apply the unchanged scalar formula
with that channel's coordinates and entries. No color conversion is inferred.

## Acceptance and implementation status

Conceptual fixture: table=[[0,10],[2,8]], axis=[0,1,1],
input=[[0,1],[0.25,0.5]] -> values=[[0,8],[0.5,9]]. This detects an incorrect
implementation that shares a query/index across all channels in an input row.
Use independent per-channel exact rational interpolation and coordinate checks.
Strict matches result bits; accelerated results meet the shared FP32 bound; run shared endpoint, signed-zero,
domain, singleton-table, descending-axis and mixed-dtype cases for each channel.

Compare selected channels with independent scalar CRV-05A nodes and check exact
read/dirty witnesses. Include C=1, single vector input, large sparse logical
arrays, wrong C, remote bad table columns, shared bad axis, fragmented/strided
sources, low budgets, cancellation, cache-off and owner lifetime. The public
WorkflowDocument fixture must connect a multi-function CRV-04 table/axis and
inspect requested output channels through Compiler/ExecutionContext.

The maintained constructor preserves the channel shape, including C=1 and
rank-1 vectors, in the six Whole Result profile keys. The Result manual fixture
passes seven groups under Strict and locally available Apple profiles, and its
independent Fraction driver passes 1,416 bit-equal cases per profile. Manual
channel coverage includes independent queries and table columns, all-port
reversed unaligned layouts, zero strides, mixed dtypes, typed RGBA rejection of
an invalid unrequested alpha, upstream failure propagation and all six CRV-04
baking chains. Sparse demand returns full Whole output coverage, while dirty
mapping follows the recorded query region. Replacing source Results retains the
static preparation and refreshes cache associations.

The focused root selection also runs the existing `test_numeric_result_math`
integration fixture, which checks channel batch shape [2,2,2], custom Result
schemas, scalar Fraction golden bits, axis/query/table failure precedence, and
the original 1 MiB sparse-output and zero-stride table resource cases. These
belong to the existing integration fixture, not the new manual workflow.

An L=1,048,576 dynamic axis grid succeeds with the 8*L-byte grid charged to
Metadata and no dense table copy charged to Payload. A sparse output request with
2^39 channels is rejected at the LUT node with `CapacityLimit`. A retained
channel Result and authorized read window share one 16-byte Payload owner after
all sources and the execution context retire; releasing them returns Root
resources to zero. The full maximum physical channel-table allocation and x86
numeric execution were not run. The focused root selection passed the manual
LUT1D and existing math integration tests 2/2 in 4.74 seconds. The installed
consumer compiled the same source against package 0.32.0 and passed 1/1 under
Strict in 0.38 seconds; its direct Apple run passed all seven groups. See the
[family verification section](CRV-05_apply_lut1d.md#maintained-implementation-and-validation)
and [numeric workflow README](../../../../examples/numeric_workflow/README.md#lut1d-application-crv-05)
for test identities and run evidence. Historical package 0.18 Value-adapter
measurements do not establish Result performance; specification status remains
Proposed.

- [Family decisions](CRV-05_apply_lut1d.md).
- [Operator template](../../00-foundation/spec-template.md).
