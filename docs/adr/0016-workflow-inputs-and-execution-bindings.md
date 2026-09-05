# ADR 0016: Workflow Inputs and Per-Run Image Bindings

- Status: Accepted; target contract, not yet implemented
- Date: 2026-09-05
- Accepted scope: adjusted development direction, Float32 image inputs and ordinary per-run parameters, explicitly approved by the maintainer in this task
- Acceptance record: 2026-09-05; the maintainer explicitly confirmed the operation ABI v3 upgrade and companion contract in the current task.
- Task: [kernel #256](https://github.com/kevin-zf1123/photospider/issues/256)
- Verified source: `main@369da60bdcf7aa26eefbd7a99f7e5d1a8afd79e8`
- Implementation: absent; belongs to #257 and #258
- Reader mirror: [Chinese](zh/0016-workflow-inputs-and-execution-bindings.zh.md)

The maintainer accepted the [adjusted direction](../development/Refactor-Development-Plan.md).
This revision replaces the earlier UInt8-only candidate with one coherent
Float32 image/ordinary-scalar contract. No direction confirmation is pending.
The exact ABI and companion contract have been explicitly accepted;
no installed header, runtime version or GitHub status is changed by this file.

ADR 0015 retains product ownership. This ADR supersedes only
the operation-v2 and minimum-element statements in ADRs 0015/0012/0008, and
extends ADR 0014 identity rules as stated below. It does not authorize S2
partial storage, S3 cache/recovery, native GPU residency, or daemon wire work.
C++17 remains the actual baseline; C++20 is an independently evaluated task.

## Context and selected design

Current WorkflowDocument edges only refer to nodes. Current execute accepts
no runtime bindings; Value has UInt8/Int64/Float64, while Elementwise/Halo
require every input shape to match the output. A graph image plus a rank-one
ordinary parameter needs explicit port semantics. Existing operation ABI v2
checks exact C structure sizes, so adding descriptor fields in place is invalid.

| Topic | Revised target | Reason and rejected alternative |
| --- | --- | --- |
| Dynamic data | Image and bounded scalar Values in one immutable ExecutionBindings snapshot. | Reuse plans for pixels/gain/opacity without converting them into source constants. |
| Port contract | Typed port kind, scalar interval, and image-specific demand validation in copied traits. | Whole-only callbacks are conservative but lose the existing useful requested-Region signal; silently broadcasting every scalar would change unmarked operations. |
| Plugin ABI | One explicit operation ABI v3, with port schemas for both C++ and installed C plugins. | Keeping only a Float32 flag in v2 cannot publish the full port schema; C++-only validation would leave #258's plugin contract incomplete. No v2 compatibility adapter. |
| Storage | Fixed shape, dense owned CPU bytes and whole materialization for S1. | Later partial/device Storage gets a separate explicit decision. |
| Outputs | Named outputs fixed in the document; output Regions chosen during planning. | Replan optimized IR for changed demand; no execute-time plan mutation. |
| Image profile | Linear-sRGB premultiplied RGBA Float32 with explicit numeric and facet rules. | Narrow S1 profile, no color-management or file-codec product. |

## Exact proposed source and execution API

The following are replacement/additional declarations in namespace `ps`, using
existing `ValueDescriptor`, `Region`, `StridedLayout`, `ValueFacet`, `Value`,
`CancellationToken`, `ExecutionOptions`, and `Result`. Public declarations
receive the existing `PHOTOSPIDER_API` export and complete Doxygen in #257.

```cpp
struct WorkflowNodeOutput final {
  std::uint64_t source_node = 0;
  std::string source_port = "value";
};
struct WorkflowInputReference final {
  std::uint64_t input_id = 0;
};
using WorkflowInput =
    std::variant<WorkflowNodeOutput, WorkflowInputReference>;

struct WorkflowInputDeclaration final {
  std::uint64_t id = 0;
  std::string name;
  ValueDescriptor descriptor;
  Region region;
  StridedLayout layout;
  std::vector<ValueFacet> facets;
};
struct WorkflowDocument final {
  std::uint32_t schema_version = 2;
  std::vector<WorkflowInputDeclaration> inputs;
  std::vector<WorkflowNode> nodes;
  std::vector<WorkflowOutput> outputs;
};
struct ExecutionBinding final {
  std::string name;
  Value value;
};
struct ExecutionBindings final {
  std::vector<ExecutionBinding> inputs;
};

// Replaces the existing member declaration of ExecutionContext.
Result<ExecutionResult> execute(
    const ExecutionPlan& plan, ExecutionBindings bindings = {},
    const CancellationToken& cancellation = CancellationToken(),
    const ExecutionOptions& options = {});
```

`WorkflowNode.inputs` retains its vector name and position semantics, using
the new tagged `WorkflowInput`. There is no zero-id sentinel for an external
input, invented operation key, or old-struct compatibility alias. Node ids and
input ids have independent namespaces; variant tags disambiguate equal
numbers. `WorkflowOutput` continues to reference an operation node's `value`
port. Direct input-to-output passthrough without an operation is not added.

Declarations are unique by nonzero id and name. Names are exact, case-sensitive
1..128-byte printable ASCII strings without spaces (bytes 0x21..0x7e), with no
Unicode normalization. There are 0..4096 declarations. Every declaration must
be bound exactly once, including one not referenced by a node. Declaration
order and binding order are not semantic; canonical declaration order is id.
Existing node, edge, parameter, and output limits continue to apply.

The binding vector has 0..4096 entries. A vector retains duplicate bindings
for validation. A map that silently
replaces an earlier value cannot serve as the public validation boundary.
Bindings resolve exact names to declaration ids before any callback starts.
There are no optional bindings, defaults, implicit conversions, resizes,
layout repacking, facet coercions, or hidden file loading.

### Input descriptor, Region, layout, and facets

The descriptor fixes one of UInt8, Int64, Float64 or Float32 and nonzero rank-1..8 shape.
The declaration Region must equal `Region::whole(shape)`. Layout must have
byte offset zero and canonical positive row-major strides: last-axis stride
is the element width and each preceding stride multiplies the next stride by
the next axis extent. Singleton axes use this same canonical stride rule.
All products, signed strides, addressed tails and host-size conversions are
checked; a valid dense byte count B must satisfy `B > 0`, `B - 1 <= INT64_MAX`,
and `B <= SIZE_MAX`. No payload buffer is allocated to validate a declaration; metadata
canonicalization may allocate.

The bound Value must be valid and match the exact descriptor, whole Region,
zero offset and canonical strides; `bytes().size()` must equal B. Negative or
zero strides, prefix/trailing padding, partial/empty coverage, and a different
logical shape are rejected at this binding boundary even when they form valid
general Values elsewhere. General `Value::create` behavior stays intact.

Facets are a closed exact set, canonicalized by key. Reuse Value's existing
limits: at most 64 records, unique 1..256-byte printable-ASCII keys, positive
versions, at most 64 KiB payload bytes per facet and 1 MiB aggregate payload.
Key, version and payload bytes must all match. Missing/extra facets, version
changes, and payload changes fail; an empty declaration set requires an empty
Value set. Facets carry compile-time semantics, not a per-run extension bag.

### Compile-time facts and per-run data

Shape/halo/specialization `WorkflowNode.parameters` remain compile-time
constants with the current variant and Float64 bit rules. Ordinary gain and
opacity are scalar Value inputs and never enter that parameter map. Operation-generated source
Values retain the existing source-operation model. This change adds no
compile-time arbitrary Value literal or constant/dynamic mode switch.

Declarations contain metadata only; graphs, IR and plans never retain a
binding Value, data address or dynamic payload. A copied `Value` in
`ExecutionBindings` belongs only to its execution. Replacing payload bytes
requires a new immutable Value and a new binding snapshot, without replacing
the graph or recompiling its unchanged plan.

## Exact port and image contract

Add `ElementType::Float32 = 4` (4 bytes, IEEE-754 binary32); values 1..3 keep
meaning. Generic Value validation remains structural and accepts finite or
non-finite Float32 bit patterns, subject to ordinary buffer rules. No new
scalar accessor is required: construct with Value::create and copy scalar
bytes with memcpy; do not use an unaligned float pointer.

```cpp
enum class OperationPortKind : std::uint32_t {
  Value = 1,
  Float32Scalar = 2,
  LinearPremultipliedRgbaFloat32 = 3,
};
struct OperationPortConstraint final {
  OperationPortKind kind = OperationPortKind::Value;
  float minimum = 0.0F;
  float maximum = 0.0F;
};
// OperationTraits additions; version becomes 3.
std::vector<OperationPortConstraint> input_schema;
OperationPortConstraint output_schema;
```

`input_schema.size()` must equal input_count, bounded by 1024; zero-input
operations use an empty schema. A Value port keeps existing shape/Region
behavior. A Float32Scalar port must directly reference a workflow input,
requires exact Float32 shape {1}, whole Region, offset 0, stride {4}, 4 bytes,
and no facets. A computed scalar source is InvalidArgument during analyze.
Its finite inclusive interval has finite binary32 endpoints with minimum <=
maximum; all non-scalar constraint bounds must have positive-zero bits.
Image output requires output_element_type=Float32. Image inputs under
Elementwise/Halo require image output kind; Whole can serve a generic statistic
output. Output kind is Value or LinearPremultipliedRgbaFloat32; scalar output kind is
rejected in this S1 schema. MatchAllInputs cannot contain a Float32Scalar port;
PreserveFirstInput requires a non-scalar first port. Unknown kinds, schema
count errors and invalid combinations fail registration before publication.

An image port requires Float32 shape {H,W,4}, H/W positive and compile-time
fixed, and exactly one facet: key `photospider.image`, version 1, payload equal
to ASCII `rgba;linear-srgb;premultiplied;hwc` without NUL (length 34 bytes).
Channel order is R,G,B,A, axis order H,W,C, row-major. RGB is finite and
nonnegative, alpha finite in [0,1]; alpha zero requires RGB zero. HDR RGB may
exceed 1 or alpha. Signed numeric zero is accepted; pixel values are not
normalized. RGB values are already linear sRGB; no gamma conversion, color
transform, unpremultiplication or implicit clamp is performed.

A LinearPremultipliedRgbaFloat32 output requires PreserveFirstInput and an
image first input. It carries the exact same profile, type and shape. Analysis
propagates this declared profile to an image consumer and rejects a generic
producer with no such output guarantee. The host revalidates all image inputs
before invocation and image output before publication. A bound image's pixel
content and every direct scalar interval are checked during complete Run
preflight, before the first callback; a later computed invalid image is a
callback contract failure. Generic Value ports do not acquire image semantics.

| Operation | Ordered ports | Output and arithmetic |
| --- | --- | --- |
| `image.exposure_gain` | image; Float32Scalar gain in [0,16] | Image; RGB multiplied by gain, alpha copied. Gain is a dimensionless multiplier, not exposure stops. |
| `image.opacity` | image; Float32Scalar opacity in [0,1] | Image; all four channels multiplied by opacity. |

Both operations have two inputs, empty source parameter schemas,
PreserveFirstInput, Elementwise, deterministic/side-effect-free/cacheable CPU
traits and no GPU candidate in S1. Each multiplication rounds to binary32,
round-to-nearest ties-to-even before the next operation; no contraction,
fast-math or flush-to-zero changes are permitted. Alpha copying preserves bits.
A non-finite computed output is OperationFailed, with no result publication.
General test oracle tolerance is `abs(actual-ref) <= 1e-7 + 2e-6*abs(ref)`
per finite channel against independently rounded reference values; the binary
fraction fixture below requires bit-exact finite values. Both mathematical
operations publish the exact profile facet explicitly, not through inferred
PreserveFirstInput behavior.

Scalar bytes are ordinary run data. Shape, halo radius, specialization and
source ParameterValue stay compile-time facts. A declaration may feed several
scalar ports; preflight checks every consuming interval (their intersection),
and an empty intersection fails analyze as InvalidArgument. The same source
cannot satisfy incompatible image/scalar descriptors. Changing source facts
requires analyze/optimize; changing only pixel or scalar bytes does not.

## Stage lowering and identity

Each stage publishes a copied canonical `input_declarations()` vector. The
public `SemanticNode.inputs` becomes `std::vector<WorkflowInput>`, with tagged
producer references in input-port order.
The physical plan replaces `PlanStep.input_steps` with the following tagged
index vector so an external input cannot be mistaken for a dependency task:

```cpp
struct PlanStepInput final { std::size_t step_index = 0; };
struct PlanWorkflowInput final { std::size_t declaration_index = 0; };
using PlanInput = std::variant<PlanStepInput, PlanWorkflowInput>;
// PlanStep member replacing input_steps:
std::vector<PlanInput> inputs;
// New const member on SemanticGraphIR, OptimizedGraphIR and ExecutionPlan:
const std::vector<WorkflowInputDeclaration>& input_declarations() const noexcept;
```

Step references must point to earlier plan steps; declaration indexes must be
in bounds. Only step references contribute ready-task dependency counts.
External descriptors participate in Scalar/Fixed/Preserve/Match validation
without running an input callback. Existing graph/registry currentness checks
remain mandatory, including when stage digests match.

| Identity | Included input facts | Excluded run facts |
| --- | --- | --- |
| SemanticGraphDigest | Schema domain; declarations sorted by id: id, exact name, element enum, shape, Region intervals, byte offset/strides, sorted facet key/version/payload; ordered tagged node sources and existing parameter/trait/output facts. | Binding container order, dynamic payload bytes, memory addresses, Value storage owners. |
| OptimizedGraphDigest | Parent semantic digest, optimizer identity, normalized declaration table and optimized tagged topology. | All per-run bindings and scheduling observations. |
| ExecutionPlanDigest | Parent optimized digest, canonical declarations, tagged step/declaration indexes, chosen backends, modeled step bytes and every output/input demand. | Payload, runtime allocation, cancellation, timings, transfer observations, daemon ids. |
| PlanCacheKey | New cache domain and complete physical plan digest. | Binding data; no payload-derived plan specialization. |

All counts, ids, ranks, extents, enums, versions and indexes encode as checked
uint64 little-endian fields; signed strides encode their uint64 two's-complement
representation. Strings and facet payloads are length-prefixed raw bytes.
Source tags encode 1 for node/step and 2 for declaration. Existing parameter
encoding, node topological ordering, named-output ordering and trait fields
remain as defined by ADR 0014. No raw C++ object padding is hashed.

Use domains `semantic-graph-ir-v3`, `optimizer-v3-canonical-noop`,
`physical-plan-v3`, and `plan-cache-key-v3`. Reordering declarations/bindings
alone preserves identity; renaming an input, changing its id/constraint,
rewiring a tagged input, or changing a compile-time scalar changes the
corresponding stage identities. An invalid constraint fails before a digest
is published. Changes to the final normalized per-step demands preserve semantic/optimized
identities but change plan/cache identities. Multiple names selecting one node
are merged before identity encoding; changing one requested sub-Region while
another name already demands the whole node can leave the normalized plan and
its identity unchanged. No promise is made that every request-text edit changes
a physical identity. A cache hit still requires full validation;
a stale/mismatched entry is discarded and rebuilt. There is no runtime result
cache keyed only by the reusable plan.


Input/output port schemas, kinds and scalar intervals are copied compile-time
traits, included in each identity in input order. Endpoint binary32 bits are
zero-extended into canonical uint64 fields. Profile facets and scalar
declarations are static facts; both image and scalar runtime bytes are
excluded. Changing only runtime bytes or binding order preserves every stage
identity. Result digests describe output only, so a parameter change producing
the same output may preserve that digest; future result-cache keys are separate.

## Operation ABI v3 and per-port demand

For Value ports, keep the existing Whole/Elementwise/Halo rules exactly.
Float32Scalar ports always demand whole {1}, independent of the operation
Region rule and halo radius. Image ports follow Whole or Elementwise on H/W;
Halo expands/clips only H/W. Every demanded image Region must cover channels
{offset=0, extent=4}; a partial-channel request is InvalidArgument at planning.
Ranks and image type/profile are checked before any demand propagation.
Complete whole Values are still produced, including for smaller spatial
requests; empty output demand is InvalidArgument. Named output selection stays
in WorkflowDocument, and PlanningOptions.output_regions is the only demand
entry. Changed requests replan optimized IR, with no full semantic recompile.

The operation C ABI makes one breaking migration from v2 to v3. Rename every
operation-owned `_v2`/`_V2` type, constant and get_api entrypoint to `_v3`/`_V3`,
preserving existing field order, widths, callbacks, result codes and parameter
semantics except for the explicit additions below. Existing unversioned flag
values and `ps_operation_plugin_get_abi_version` name remain unchanged. Add Float32
code 4 in `ps_operation_element_type_v3`. No v2 aliases, readers or adapters
remain. New host rejects an ABI-2 plugin before looking up get_api_v3; old host
rejects ABI-3 before invoking its callbacks. All maintained fixtures and SDK
consumers migrate together in #257.

```c
#define PS_OPERATION_ABI_VERSION_3 3U
#define PS_OPERATION_PORT_VALUE_V3 1U
#define PS_OPERATION_PORT_FLOAT32_SCALAR_V3 2U
#define PS_OPERATION_PORT_LINEAR_PREMULTIPLIED_RGBA_FLOAT32_V3 3U

typedef struct ps_operation_port_constraint_v3 {
  uint32_t struct_size;
  uint32_t kind;
  uint32_t minimum_bits;
  uint32_t maximum_bits;
} ps_operation_port_constraint_v3;

/* Insert after parameter_count/parameters, before execute/user_data,
   in the mechanically renamed ps_operation_descriptor_v3. */
uint32_t input_schema_count;
const ps_operation_port_constraint_v3* input_schema;
ps_operation_port_constraint_v3 output_schema;

uint32_t ps_operation_plugin_get_abi_version(void);
const ps_operation_plugin_api_v3* ps_operation_plugin_get_api_v3(void);
```

This mechanical mapping plus the insertion defines the exact new descriptor
layout; all nested callback/table references use v3 types. C declarations
retain `PS_OPERATION_EXPORT` and C linkage. The constraint struct has four
uint32 fields; endpoints store IEEE binary32 bit patterns as numeric uint32
values, decoded with memcpy. input_schema_count equals input_count <= 1024;
the pointer is null exactly for count zero, otherwise naturally aligned and
valid for the plugin lifetime. The host checks exact struct_size for each
record and output_schema, kind, count, bounds and combinations, copies all
records before publication, and rejects partial registry publication on error.
The C and C++ constraint validators and invocation semantics are identical.
No callbacks or plugin pointers enter copied semantic facts or stage identity.

Data-provider ABI v1 retains its static schema layout and entrypoints; its
closed element decoder adds 4. A new host accepts old 1..3 schemas; an old host
rejects a new Float32 schema. Providers have no runtime Value callback and do
not define an alternate image/port contract. Value construction, facet bounds,
exception fencing, single-output sink, CPU fallback result rules and exact
cleanup remain intact across the operation version change.

## Validation, lifetime, concurrency, and cancellation

Validation is fail-before-publication. Analyze validates declaration count,
ids/names, duplicate identities, then descriptors, dense layout/Region/facets
and tagged references before semantic publication. Binding validation first
checks the exact name multiset, then Values in declaration-id order. For a
single Value check validity, element, shape, Region, layout/storage length,
then facets. No registry callback, transfer, or result publication precedes
successful validation of the complete binding set and all direct port constraints.

| Failure | Stable ErrorCode / effect |
| --- | --- |
| Missing, extra, duplicate or malformed binding name; invalid/default Value | InvalidArgument; no callback and no partial result. |
| Wrong valid element type, shape, Region, layout, byte length, or facet set/version/payload | TypeMismatch; no callback and no partial result. |
| Invalid declaration id/name/count/schema/Region/layout/facet structure or duplicate id/name | InvalidArgument during analyze; no semantic IR. |
| Unknown tagged source reference | NotFound during analyze; no semantic IR. |
| Type/shape conflict with an operation trait | TypeMismatch during analyze; no semantic IR. |
| Dense products, signed strides, host-size conversion or bounded resource exhaustion | ResourceExhausted; no partial stage/result. |
| Default, stale or foreign-registry plan | Existing Stale plan-entry failure, before reading bindings or observing the token. |
| Observed cancellation or stale graph after plan entry succeeds | Cancelled before Stale before an ordinary run failure, under the existing publication checks. |

When several name errors coexist, validation checks bounded/name format, then
duplicates, extra names, and missing names, in that order. Within each class,
choose lexical name order for diagnostics. Diagnostic text remains non-stable;
callers branch on `ErrorCode`. The existing entry check rejects a non-current/default or foreign-registry
plan as Stale first, even with a cancelled token. After entry succeeds, observe
cancellation/currentness before binding validation
and again when selecting a binding failure, before admission and at final
publication. Use the existing no-throw stop-selection fallback. Allocation
failure may still throw `std::bad_alloc` where the current public execute
contract permits it, including C++ by-value argument construction.

`execute` is synchronous. By-value bindings give the call its own names and
Value metadata. Copying a Value shares its immutable owned byte vector. The
caller must not mutate a source container concurrently with argument copying;
a concurrent wrapper must own its arguments until copying finishes. Plan,
cancellation and options references remain valid and unmodified for the call.
`ExecutionContext` cannot be destroyed concurrently with `execute`.

Each Run retains the snapshot until all admitted callbacks have retired,
including non-preemptible callbacks after cooperative cancellation. Reject
late cancelled/stale publication, then release Run-owned references exactly
once. Returned Values own their bytes and may outlive the Run; destroying one
caller's Value copy or one result does not invalidate another owner. No new
public release method is added to the kernel.

The same plan may execute concurrently with distinct snapshots in the same
or different matching ExecutionContexts while its GraphContext and frozen
registry remain current. Runs never store bindings in shared plan/registry
state, never share mutable result slots, and do not deduplicate work by plan
digest. Graph replacement/destruction preserves existing stale behavior.

Inputs start with the Run-local CPU backend label. A GPU consumer uses the
existing explicit immutable transfer and fallback mechanism. Retaining a
caller-owned immutable input does not consume `maximum_live_bytes`; that
limit and `peak_live_bytes` keep their current modeled callback-byte meaning.
They do not bound total retained inputs, intermediate results or process RAM.
Admission counts waiting callbacks, not declaration references or synchronous
caller threads. The embedding controls input allocations and concurrent calls.


Scalar NaN/infinity/range errors and bound-image pixel-domain errors are
InvalidArgument during preflight. Wrong descriptors/profile facets/port
connections are TypeMismatch; malformed schemas or computed sources for
bounded scalar ports are InvalidArgument. Computed invalid image data,
non-finite results or wrong output profile are OperationFailed. Preflight
checks every declaration/direct-consumer constraint before the first callback;
operation boundaries still validate each input/output. Long image scans
periodically observe cancellation/currentness, preserving stop priority at
preflight completion, failure selection and publication.

## Compatibility, benchmark, and downstream impact

#257 updates package 0.2.0 to 0.3.0, source schema 1 to 2 and the named stage
domains above. A schema-1 document fails explicit version validation; the
updated default writer emits 2. There is one source/API implementation, with
all callers migrated. Existing `execute(plan, token, options)` calls become
`execute(plan, {}, token, options)`. Public C++ struct/type/signature changes
require rebuilding consumers; existing SameMinorVersion package matching
must reject a 0.2 consumer against 0.3. Update public header/export and both
static/shared isolated `find_package(Photospider)` consumers in #257. This
accepted decision changes no CMake version or installed header now.
OperationTraits version becomes 3 and operation ABI becomes 3; provider ABI
remains 1 with element code 4 added.

Raw plan identity excludes payload; existing result identity describes output
Values and may differ between inputs, without promising collision-free or
persistent identity. Correctness compares the actual bytes with an independent
named oracle. Keep backend, transfer, resource, timing, plan/result identity
and correctness observations separate. No input artifact or evidence identity
is introduced. #257 adds `ExecutionBindings bindings;` to
`RawBenchmarkOptions`; each existing independently compiled sample receives
that snapshot. The runner copies bindings once at entry and reuses that immutable snapshot
for its samples; the caller must not mutate options during the call. Its
existing iteration semantics remain unchanged. #258 proves
compile-once/two-payload reuse through direct compile/execute calls and can
also run the existing benchmark once per payload with a matching captured
oracle. A general benchmark-mode redesign is outside this decision.

[Daemon #10](https://github.com/kevin-zf1123/photospider-daemon/issues/10)
starts after this decision is accepted. It must explicitly decide Session
immutability and the bounded public binding projection, retain/copy points,
IPC/client versions and cancellation/release rules. Recommendation: one
immutable Session document/plan and independent per-Job snapshots; close and
create a new Session for changed declarations. Kernel `GraphContext::replace`
does not create a daemon update method. No wire encoding, bulk transport,
internal IR serialization or daemon implementation is specified here.


Daemon features follow the accepted demand-driven direction. Kernel 0.3 still
needs a minimal coordinated consumer/package migration or separately approved
version/CI selection policy; the existing 0.2 consumer and main-following CI
cannot silently remain compatible. #10 keeps its own Session/wire decision.

## Named fixtures and image oracle

These are future #257/#258 acceptance requirements, not executed runtime
results. Declarations are id1/name=image, id2/name=gain and id3/name=opacity.
Image: Float32 {2,2,4}, whole Region, offset0, strides {32,16,4}, 64 bytes and
exact image facet. Each scalar: Float32 {1}, whole Region, offset0, stride4,
4 bytes, no facets. Node10 image.exposure_gain takes id1/id2; node20
image.opacity takes node10/id3. Named result selects node20, source parameters
are empty. Compile once, bind A/B sequentially and concurrently, two CPU
callbacks per run, no CPU-only transfer. Requested output Region is
{0,1,0}/{1,1,4}, pixel (0,1); return the complete image.

| Case | Four input RGBA pixels | Gain / opacity | Four expected output RGBA pixels |
| --- | --- | --- | --- |
| A | (1/8,1/4,0,1/2); (1/4,1/8,1/8,1/2); (0,1/4,1/2,1); (0,0,0,0) | 2 / 1/2 | (1/8,1/4,0,1/4); (1/4,1/8,1/8,1/4); (0,1/4,1/2,1/2); (0,0,0,0) |
| B | (1/4,0,1/8,1/2); (1/8,1/4,1/8,1); (1/2,1/4,0,1); (0,0,0,0) | 1/2 / 1/4 | (1/32,0,1/64,1/8); (1/64,1/32,1/64,1/4); (1/16,1/32,0,1/4); (0,0,0,0) |

Oracle name: `s1-rgba32f-exposure-opacity-v1`. Independently round each stage
to binary32; all these inputs are binary fractions so require bit-exact
outputs. General inexact values use the tolerance specified earlier.

| Fixture | Required observation |
| --- | --- |
| S1Image.DynamicBindings | Same compiled plan, A/B below, exact pixels and unchanged compile count; two CPU callbacks per run. |
| S1Image.SingleBindingChanges | Change only image, only gain, only opacity, then opacity zero; no stale binding reuse. Equal all-zero outputs may share result digest. |
| S1Image.ConcurrentSnapshots | Gate two Runs with independent bindings/tokens; release caller owners while callbacks remain active, obtain each correct result. |
| S1Bindings.Names | Missing, extra, duplicate, malformed and default-invalid Value cases each fail InvalidArgument before callbacks; reordering valid entries preserves results. |
| S1Bindings.Descriptors | Valid mismatching element, shape, partial/empty Region, negative/broadcast/padded strides, nonzero offset or trailing bytes: TypeMismatch. |
| S1Bindings.Facets | Missing/extra/key/version/payload mismatch: TypeMismatch; duplicate/invalid facet structures fail Value creation or analyze. |
| S1Scalar.Validation | Wrong type/shape/facets: TypeMismatch; NaN, infinity, gain below 0/above 16, opacity outside [0,1]: InvalidArgument, zero callbacks. |
| S1Scalar.Schema | Computed scalar source or empty interval intersection fails analyze; duplicate references satisfy every consuming interval; unknown kind/count/bounds fail registration. |
| S1Image.PixelDomain | Negative/non-finite RGB, invalid alpha or nonzero RGB at alpha zero in direct bindings fail InvalidArgument in preflight; generic Float32 Value remains constructible. |
| S1Image.AlphaUnderflow | Valid HDR pixel (1,0,0,2^-149) with opacity0.5 rounds to (0.5,0,0,0), violating the alpha-zero RGB rule: OperationFailed even though all channels are finite. |
| S1Image.OutputFailure | Overflow or malformed computed image/profile fails OperationFailed without result publication or backend retry. |
| S1Image.Demand | Request offsets {0,1,0}, extents {1,1,4}; both image ports receive it, both scalar ports receive {0}/{1}, whole 64-byte result remains. |
| S1Image.Halo | On {3,3,4}, request {0,0,0}/{1,1,4}, radius 1: image demand {0,0,0}/{2,2,4}, scalar demand {0}/{1}; channel halo is absent. |
| S1Image.DemandFailures | Empty/out-of-bounds/partial-channel/unknown-name demand fails planning; an image consumer rejects an unprofiled producer in analyze. |
| S1Bindings.Identity | Static schema/profile/bounds/source changes affect identities; runtime image/scalar bytes and order do not; merged unchanged final demands preserve plan identity. |
| S1Bindings.StopsAndBounds | Default/foreign/stale plan, cancellation before/after admission, queue rejection and checked dense/2B overflow preserve specified status priority and exact owner cleanup. |
| S1Abi.VersionAndSchema | ABI2 rejected before ABI3 table access; malformed ABI3 port schema rejected atomically; C and C++ valid image cases agree. |
| S1Abi.ProviderFloat32 | Provider-v1 code4 accepted only by the updated host; old1..3 remain valid, unknown elements still fail. |
| S1Bindings.StaticSharedConsumer | Installed schema2, Float32, ABI3 C plugin and C++ consumer execute the same oracle in static/shared variants; 0.2 consumers reject package0.3. |

For declared image outputs, planned invocation bytes are
`max(traits.estimated_bytes, 2*B)`, where B is dense output bytes. The 2*B
model conservatively includes callback output and host sink copy; check
multiplication overflow. Generic Value outputs retain the old estimated-byte
semantics. The fixture reserves 128 modeled bytes per step, with peak128 in
an isolated sequential CPU run. This still excludes complete input,
intermediate-result and transfer allocations and is not a process-memory
limit. Include this rule in plan identity and resource tests; it does not
promise S2's real storage budget.

## Decision status and delivery

The maintainer accepted the direction, Float32, complete port schema and single
operation ABI v3 above by explicitly confirming the concrete ABI/companion-
contract question in this task on 2026-09-05. These decisions need no repeated
confirmation. The accepted target is unimplemented; #257/#258 own code and
runtime acceptance.

#256 tracks public document delivery and Issue/Project settlement. The
maintainer separately authorized the two documentation PRs, CI-gated merge
and #256 settlement. Decision acceptance does not report implementation or
automatically start #257.
