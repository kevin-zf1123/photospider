# Bounded GPU request discovery

### Scope and ownership

GPU discovery is run-time selection inside an instruction's compiled grants. The planner fixes the instruction's input grants before execution; a data-dependent operation declares a finite static superset of everything its GPU code might read. During the poll, a GPU shader computes which granted tiles it actually needs and writes them into a bounded request table. The executor owns that table, drains native work, validates each record against the compiled grants, and returns an immutable receipt. Discovery does not trace arbitrary shader reads, does not start producers, and adds no plan instruction or dependency. Both the C Result ABI 7 service and the C++ `ResultProgramPhase::discover` entry use this path. `Tensor` can provide private backing storage, but it does not expose a separate public dependency-discovery phase.

A C Result operation calls `ps_result_services_v7::discover` from its ordinary `poll` callback. The discovery callback receives only reads of the current grants, native atlas access, scratch, work, cancellation, and GPU dispatch services. It returns an ordinary status and must submit real native work. Every selected input is already ready, so the operation can read the selected tiles, compute and publish in the same poll. Returning `PS_RESULT_NEED_V7` with the selected records is optional and only asserts that they lie inside the grants.

```c
uint64_t receipt = 0;
if (services->discover(services->context, capacity, candidates,
                       compute_requests, user, &receipt))
  return PS_RESULT_STATUS_FAILURE_V7;
/* Read records with discovery_requests, then read the selected granted
   tiles and publish in this poll. */
```

The callback stores the receipt handle in operation state when it needs the normalized records during a later poll. It releases that handle after reading the records. `discovery_requests` supports a count query with `output == NULL` and `capacity == 0`, followed by a copy into initialized records. The C handle remains owned by that plugin instance across polls until explicit release or operation destruction. A receipt retains no source payload and grants no additional reads.

### Data layout and memory

For capacity `K`, the logical table size is:

$$
S = 16 + 32K.
$$

The executor charges `candidates + 2S` work before allocating the zeroed Root table. The table uses Root-accounted native storage, not the operation's workspace allocation. The device's rounded table capacity is admitted separately. The host freezes the table only after the synchronous callback and its submitted GPU work drain, then decodes it. No table or service pointer survives the discovery callback.

Each wire record occupies 32 bytes: four uint32 fields `input, kind, tensor_slot, reserved`, then uint64 `begin, end`. Payload kind 0 names a nonempty valid member-local half-open tile interval. Descriptor kind 1 requires zero slot and bounds. Reserved bits must be zero. An empty receipt contains zero records.

### Execution and grant checks

The discovery callback is synchronous and GPU-only. It may read tensor samples covered by the current grants, acquire an atlas for a granted input, allocate or release scratch, charge work, check cancellation, and submit GPU commands. The host requires at least one actual native dispatch for a successful discovery call and fails with `OperationFailed` when the callback submitted none.

After decoding, the executor checks every selected record against the instruction's `PlanTensorRead` grants: the record must name a granted input, and a payload record must lie inside a payload grant for the same member. A record that selects a tile outside the grant fails the poll with a protocol `InvalidArgument` ("GPU selection exceeds compiled input grants"). This includes a tile in a hole of a fragmented grant: when the mapper granted tiles `[0,2)` and `[4,6)`, a record for tile 3 fails even though it lies between granted tiles. A descriptor record is accepted for any input with a compiled grant. The check charges one unit of discovery work per record and grant compared.

Use a count-only query when the record count is not known, then provide initialized `struct_size` values and enough capacity for the copy. Receipt metadata is immutable and carries no tensor bytes. Each `candidates` value bounds one table. The instruction's discovery-work allowance (`DependencyLimits::maximum_work`) persists across its polls, and each call also consumes Run and Root work. Duplicate records still consume emission and normalization work.

### Limits and failure handling

`capacity` is positive and cannot exceed 65,536, `DependencyLimits::maximum_gpu_requests`, or the applicable `maximum_boxes`. A zero `maximum_gpu_requests` disables discovery. `candidates` is positive and bounds every emit attempt, including duplicates and attempts beyond table capacity. The shader must not dispatch more emitter invocations than that bound. The table sets overflow only when an attempted record has no free slot; filling the last slot is valid. The host rejects overflow and also checks attempted count against `candidates`.

Table bytes, rounded native capacity, candidate work, decoding work, and request metadata are charged independently to their Root and dependency bounds. Discovery must run on the active callback entry thread for a GPU-placed instruction; it cannot be nested, cannot run inside a block callback, and cannot use checkpoints. Entering discovery vetoes CPU fallback for the attempt. Submitted dispatches drain before table owners retire. In C++, `ResultProgramPhase::consume_work` may charge from worker threads; C discovery services remain entry-thread-only.

## Registered workflow and decoder coverage

The current C discovery fixture is [`tests/fixtures/gpu_result_discovery_plugin.c`](../../tests/fixtures/gpu_result_discovery_plugin.c), loaded by [`tests/integration/kernel/gpu/test_gpu_discovery_workflow.cpp`](../../tests/integration/kernel/gpu/test_gpu_discovery_workflow.cpp). Its mapping hook grants every tile of the Data input as the static superset and the matching tile of the Control input; the shader selects Data tiles inside that grant. Its independent expected sums are `8` and `24`; a Control edit changes the selected Data support, while a frozen execution retains its captured Result. The same integration source exercises malformed records, overflow, missing device, failed publication, cancellation, Root cleanup and retry.

The decoder and normalization source tests are [`tests/unit/kernel/test_result_discovery_services.cpp`](../../tests/unit/kernel/test_result_discovery_services.cpp) and [`tests/unit/kernel/test_result_discovery_decoder.cpp`](../../tests/unit/kernel/test_result_discovery_decoder.cpp). These registrations identify executable coverage and do not report a previous run result.

The native GPU service is independently versioned as `PS_GPU_ABI_VERSION_1`; it supplies the synchronous buffer and dispatch operations used by discovery. Result discovery itself is part of Result operation ABI 7. Vulkan support is optional at build time. Native GPU behavior depends on an enabled backend and a usable device; protocol mocks cover validation and failure paths but do not establish native execution. Ordinary GPU steps and GPU shared stages both initialize Run-native uploads.
