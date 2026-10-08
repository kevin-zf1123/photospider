---
spec_schema_version: 1
id: CRV-05
kind: shared_operator_contract
category: 01-numeric
status: Proposed
document_maturity: D1_draft
implementation_status: implemented
verification_status: focused_result_math_ctest_and_installed_consumer
clarification_status: complete
repository_branch: ops-specs
repository_commit: current working tree
---

# CRV-05: apply_lut1d family

Numeric profile: strict retains the exact reference defined below. Floating
arithmetic in accelerated profiles follows the shared
[final FP32 four-ULP contract](NUM_accelerated_contract.md), including its
range/fallback rules. Discrete results, copies, selected endpoints and special
values remain exact.

## Confirmed split

Provide two independent operations, both preserving input shape:

- Single-table application: table[L] maps every numeric element of input.
- Per-channel multi-table application: table[L,C] column c maps input[...,c].

Both target interfaces are fully clarified. Mapping one
scalar to a vector/color ramp is CRV-06 rather than this same-shape LUT family.
Both receive explicit axis[3], use fixed linear interpolation and inherit their
specified finite-value, exact-rounding and mathematical table-selection contracts. These
operations use six explicit CPU-profile keys and public constructors, with
numerical and workflow acceptance recorded in the individual specifications.
Specification acceptance remains Proposed.

- [Single-table draft](CRV-05A_apply_lut1d.md).
- [Per-channel table specification](CRV-05B_apply_lut1d_channels.md).
- [Baking templates](CRV-04_bake_lut1d.md).
- [Curve category](../curves.md).
- [Operator template](../../00-foundation/spec-template.md).

## Whole execution

Each input is a Result containing one tensor member under any schema id/version
and member key. Its complete `sample_shape()` supplies the input/table/axis shape.
The Whole Result program requests all three inputs with Data, Validation and
Descriptor (role 13), including recognized typed validation and upstream failure
handling. It validates the complete reconstructed axis, then every finite/domain
query before table arithmetic. It computes every output element/channel and
publishes an immutable packed dense Result using `photospider.tensor` v1/member `samples`,
with the complete input shape, selected Float32/Float64 dtype and empty facets.
The Result association records actual source ObjectIds. A nonempty request returns
full certified coverage with global sample coordinates. Empty reads no payload;
all static metadata checks still apply.

Mathematical table selection is unchanged: knot/clamp/singleton converts one
entry; interpolation/extrapolation uses its adjacent pair, independently per
channel. Generic entries outside all evaluated stencils receive no additional
finite scan. Typed validation and upstream collection cover the complete table.
Errors in otherwise-unrequested queries or evaluated channels can fail the Run.
Any input/table/axis edit invalidates the recorded output demand; dirty mapping
follows that recorded demand. Cache identity
retains all input versions, profile, metadata and parameters. There is no
per-channel Atom success isolation. Output dtype, shape and empty facets remain
unchanged; arbitrary input offsets, unaligned and signed/zero strides remain
legal. The complete immutable owner survives context destruction.

For M input elements, work is O(L+M log L) plus exact arithmetic and typed
validation. Singleton lookup is constant time per query. `UniformAxis` stores
the directly reconstructed Float64 grid in a Root-owned, host-accounted 8*L-byte
buffer (up to 8 MiB), plus allocator/metadata overhead; fixed exact workspace
and O(rank) coordinate state are also admitted. Input/table reads use authorized zero-copy
Root windows over their source storage; the operator does not create a dense copy
of the complete table merely to collect it. Root accounts actual retained owners
and provided windows. The operator retains no per-output certificates, rows or
lookup table. Output costs dtype_bytes*M, regardless of requested coverage.

The complete axis uses endpoint-weighted RN64 coordinates. One caller-preserving
floating environment covers axis reconstruction and curve evaluation; unresolved
accelerated bounds still use the same exact fallback. Work/cancellation is checked
in axis generation, input reads, lookup and exact arithmetic, and before publishing.
Resource failure never authorizes skipping axis validation or weaker arithmetic.
Unpublished output/workspace is released; the Result output transaction is
all-or-nothing.
Numeric InvalidDomain/ArithmeticOverflow failures have Run scope. Typed, upstream,
resource, stale, backend and cancellation failures retain their categories.
Whole does not expose per-value fallback counters; report those as unavailable.

## Maintained implementation and validation

`plugins/ops/01-numeric/lut1d_application.cpp` registers the six scalar/channel
profile keys as Whole Result operations. `UniformAxis` validates all raw axis
values and step, then stores the directly reconstructed grid in an 8*L-byte
Root allocation charged to Metadata by the default resource allocator.
Queries are checked before table arithmetic, and descending pairs reorder x/y
together before `ExactCurve` applies its positive-denominator formula. Input and
table data are read through authorized Result windows; the operation does not
copy the entire table into dense storage. The Whole continuation publishes the
complete packed Result, records current source ObjectIds, and releases temporary
state on failure or cancellation.

The Result manual fixture passes seven groups under Strict and locally available
Apple profiles. Its independent Fraction driver passes 1,416 bit-equal cases per
profile. Manual coverage includes scalar and channel Float32/Float64, all-port
reversed unaligned inputs, zero-stride aliases, caller and worker floating-point
modes, recognized typed RGBA validation of an unrequested alpha, upstream Result
failure propagation, static metadata bounds, Empty, dirty mapping and full Whole
coverage after sparse queries. It checks same-demand Result identity, cache hits
for fresh equivalent source Results with associations updated to those sources,
and reuse of static preparation after input/table/axis replacement. All six CRV-04
baking templates feed the Result application nodes.

The same focused selection also runs the existing `test_numeric_result_math`
integration fixture; its LUT1D groups cover custom Result schemas, channel batch
shape [2,2,2], eight scalar Fraction golden bits, axis/query/table failure
precedence, cancellation inside a 352-limb `ExactCurve` slot, and the existing
1 MiB resource cases. Those cases reject a sparse request from input[262144]
because its complete Float64 output needs 2 MiB, and accept a zero-stride
Float32 table [262145] with only an 8-byte output increase. These assertions
belong to the integration fixture, not the new manual workflow.

Resource checks exercise cancellation and work limits during the continuation,
capacity rejection for an output and scratch, and release of all Root resources.
An L=1,048,576 dynamic-grid case succeeds; Root diagnostics show the 8*L-byte
axis allocation charged to Metadata while Payload remains below 8*L, so the
operator does not materialize a dense table copy. A sparse request with 2^39
channels fails with `ResourceExhausted/CapacityLimit` at the LUT node. Escaped
scalar and channel Results each retain 16 Payload bytes while a read window is
alive; releasing both handles returns every Root resource to zero. The full
maximum physical channel-table allocation and x86 numerical execution were not
run.

The focused root CTest selection passed `test_numeric_lut1d_result` and
`test_numeric_result_math` 2/2 in 4.74 seconds. The existing math integration
test took 4.42 seconds; the new manual test took 0.31 seconds. The separate
`installed_numeric_lut1d_result` test compiled and linked the same manual source
against the installed 0.32.0 `Photospider::kernel` package, then passed 1/1 under
Strict in 0.38 seconds (0.39 seconds total); its direct Apple run passed all
seven groups. `test_numeric_result_math` and the manual/installed tests are
distinct fixtures. Reproduction commands are in the [numeric workflow
README](../../../../examples/numeric_workflow/README.md#lut1d-application-crv-05).
The old package 0.18 Value-adapter measurements in the [math implementation
notes](../math-implementation.md#crv-05-dynamic-axis-lut1d) are historical and do
not establish Result performance.
