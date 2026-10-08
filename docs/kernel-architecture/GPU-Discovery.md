# Bounded GPU request discovery

## Result tensor discovery

### Scope and ownership

Result GPU discovery lets an operation use a native GPU callback to inspect already supplied tensor samples and report a bounded set of additional tensor regions. The executor owns the table, drains native work, validates each record, and attaches the resulting typed Needs to the operation's yielded `ResultProgramNeed`. Discovery does not trace arbitrary shader reads. Both the C Result ABI 2 service and the C++ `ResultProgramPhase::discover` entry use this Result path. `Value` can provide private backing storage, but it does not expose a separate public dependency-discovery phase.

A C Result operation calls `ps_result_services_v2::discover` from its ordinary `poll` callback. The discovery callback receives only current Need reads, native atlas access, scratch, work, cancellation, and GPU dispatch services. It returns an ordinary status and must submit real native work. The outer operation callback returns `PS_RESULT_NEED_V2` when the receipt contains requests. The host rejects publication while discovered Needs remain unresolved. An empty receipt adds no Need and can be followed by further callback work.

```c
uint64_t receipt = 0;
if (services->discover(services->context, capacity, candidates,
                       compute_requests, user, &receipt))
  return 1;
return PS_RESULT_NEED_V2;
```

The callback stores the receipt handle in operation state when it needs the normalized records during a later poll. It releases that handle after reading the records. `discovery_requests` supports a count query with `output == NULL` and `capacity == 0`, followed by a copy into initialized records. The C handle remains owned by that plugin instance across polls until explicit release or operation destruction. The C++ `ResultProgramPhase::discover` API returns a `shared_ptr<const ResultDiscoveryReceipt>` containing the same kind of typed footprint metadata. A receipt retains no source payload.

### Data layout and memory

The Result v2 table preserves the 16-byte header and 144-byte records. The header contains four little-endian `uint32_t` values: attempted record count, overflow flag, and two zero reserved words. Each record contains four `uint32_t` values followed by eight `uint64_t` offsets and eight `uint64_t` extents:

| Offset | Contents |
| --- | --- |
| 0..3 | Input port index |
| 4..7 | Data, Control, and Validation role mask |
| 8..11 | Full sample rank |
| 12..15 | Tensor slot index |
| 16..79 | `uint64_t offsets[8]` |
| 80..143 | `uint64_t extents[8]` |

The rank and coordinates use the tensor's complete sample shape: batch axes first, then cell axes. Each raw rectangle must fit that selected slot and satisfy its complete tuple/channel closure before grouping. Roles are a nonempty combination of Data, Control, and Validation. The decoder groups records by input port, tensor slot, and role mask, then normalizes their rectangles under shared work and metadata bounds.

The Result helper is `PS_RESULT_GPU_DISCOVERY_MSL_V2`, which defines this Metal function signature:

```metal
bool ps_result_discovery_emit(device atomic_uint* table, uint capacity,
                              uint input, uint slot, uint roles, uint rank,
                              thread const ulong* offsets,
                              thread const ulong* extents);
```

It writes the record fields in the order input, roles, rank, and tensor slot. The legacy `PS_GPU_DISCOVERY_MSL_V11` helper remains available and writes zero in the fourth record word instead of a tensor slot. Both helpers use the same header and record size, but only the Result v2 record selects a tensor member.

For capacity `K`, the logical table size is:

$$
S = 16 + 144K.
$$

The executor charges `candidates + 2S` work before allocating the zeroed Root table. The table uses Root-accounted native storage, not the operation's workspace allocation. The device's rounded table capacity is admitted separately. The host freezes the table only after the synchronous callback and its submitted GPU work drain, then decodes it. No table or service pointer survives the discovery callback.

### Execution and Need state

```mermaid
flowchart TD
    P[Poll with current supplied Needs] --> D[Operation calls Result discover]
    D --> A[Charge candidates and allocate Root table]
    A --> G[Read authorized input and emit records in GPU callback]
    G --> X[Submit native dispatch]
    X --> F[Drain dispatch and freeze table]
    F --> V{Validate and normalize records}
    V -->|error| E[Sticky failure]
    V -->|empty| C[Continue callback]
    V -->|nonempty| N[Attach tensor Needs to yielded Need]
    N --> S[Supply unioned payload samples and role evidence]
    S --> P
```

The discovery callback is synchronous and GPU-only. It may read tensor samples covered by the current Need, acquire an atlas for that Need, allocate or release scratch, charge work, check cancellation, and submit GPU commands. The host requires at least one actual native dispatch for a successful discovery call. A missing device, table overflow, malformed record, invalid slot, out-of-domain rectangle, tuple-closure omission, or resource-limit failure returns a typed failure; it never widens coverage to a bounding box.

The executor attaches the discovered Needs to the `ResultProgramNeed` returned by the operation's poll callback. The operation can request more Needs in that same yielded Need. The combined stage entry count is limited by the smaller of 65,536 and the dependency set's `maximum_boxes`. Same-input, same-slot tensor Needs with multiple roles share one payload supply over the union of payload-readable samples. Each role's original coverage remains separately validated and recorded as dependency evidence. This avoids rereading the same slot while preserving role-specific history.

A C receipt contains normalized `ps_result_discovery_request_v2` records with input, slot, role mask, and sample-space region. Use a count-only query when the record count is not known, then provide initialized `struct_size` values and enough capacity for the copy. Receipt metadata is immutable and carries no tensor bytes. Each `candidates` value bounds one table, while work and metadata are charged across discovery calls in the active poll. The actor's discovery-work allowance persists across polls; each call also consumes Run and Root work. Duplicate records still consume emission and normalization work.

### Limits and failure handling

`capacity` is positive and cannot exceed 65,536, `DependencyLimits::maximum_gpu_requests`, or the applicable `maximum_boxes`. A zero `maximum_gpu_requests` disables discovery. `candidates` is positive and bounds every emit attempt, including duplicates and attempts beyond table capacity. The shader must not dispatch more emitter invocations than that bound. The table sets overflow only when an attempted record has no free slot; filling the last slot is valid. The host rejects overflow and also checks attempted count against `candidates`.

Table bytes, rounded native capacity, candidate work, decoding work, and request metadata are charged independently to their Root and dependency bounds. Read and atlas access remain limited to current Needs. Discovery cannot issue another Need, publish output, enter a block callback, or recursively invoke discovery. A native callback that submits no work fails with `OperationFailed`. Submitted dispatches drain before table owners retire.

## Registered workflow and decoder coverage

The current C discovery fixture is [`tests/fixtures/gpu_result_discovery_plugin.c`](../../tests/fixtures/gpu_result_discovery_plugin.c), loaded by [`tests/integration/gpu/test_gpu_discovery_workflow.cpp`](../../tests/integration/gpu/test_gpu_discovery_workflow.cpp). The fixture binds rank-one Float32 data and Int64 control Results, acquires the current Control Need through an atlas, emits selected data intervals, and publishes only after the next poll supplies those intervals. Its independent expected outputs are `8` and `24`; the Metal path asserts four native dispatches. A Control edit changes the selected Data support, while a frozen execution retains its captured Result. The same integration source exercises malformed records, overflow, missing device, failed publication, cancellation, Root cleanup and retry.

The decoder and normalization source tests are [`tests/unit/test_gpu_discovery.cpp`](../../tests/unit/test_gpu_discovery.cpp) and [`tests/unit/test_result_gpu_discovery.cpp`](../../tests/unit/test_result_gpu_discovery.cpp). They cover rank-8 coordinates, nonzero tensor slots, batch/channel closure, role grouping, malformed ranges and resource limits. CMake registers `installed_gpu_discovery_workflow` in `tests/consumer/CMakeLists.txt`; it uses the installed package and returns the configured skip code when native Metal is unavailable. These registrations identify executable coverage and do not report a previous run result.

Discovery must run on the active callback entry thread. In C++, `ResultProgramPhase::consume_work` may charge from worker threads; C discovery services remain entry-thread-only. Work and metadata bounds persist across calls in an active poll, and each call also consumes Run and Root work. Duplicate records still consume emission and normalization work.

The capacity, candidate, Root and dependency bounds are independent. Reads and atlas access remain limited to current Needs. Discovery cannot issue another Need, publish output, enter a block callback or recursively invoke discovery. A native callback that submits no work fails with `OperationFailed`. Submitted dispatches drain before the table owners retire.
## MSL discovery emitters and wire compatibility

`PS_RESULT_GPU_DISCOVERY_MSL_V2` is the Result emitter and writes input, tensor slot, roles, and rank into the first four words of each record. `PS_GPU_DISCOVERY_MSL_V11` remains as a wire-compatible emitter for slot-zero records: its fourth word is always zero. The v11 macro defines the record layout; execution and Need attachment use the Result APIs above.

Both emitters use the 16-byte header and 144-byte records described above. The Result decoder interprets the fourth word as a tensor slot and validates the input, slot, rank, roles, extents, unused axes, sample-domain bounds, and tuple/channel closure against the selected Result schema. A v11-emitted record therefore addresses slot 0; operations that need another tensor slot use the Result v2 emitter. Table capacity, candidate bounds, overflow handling, native-work draining, resource accounting, and receipt ownership follow the Result discovery contract above.

The native GPU service is independently versioned as `PS_GPU_ABI_VERSION_1`; it supplies the synchronous buffer and dispatch operations used by discovery. Result discovery itself remains part of Operation Plugin ABI 2. Vulkan support is optional at build time. Native GPU behavior depends on an enabled backend and a usable device; protocol mocks cover validation and failure paths but do not establish native execution.
