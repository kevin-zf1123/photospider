# Photospider Local IPC Protocol Version 1

This document is the authoritative maintained contract for the first local
`photospiderd` protocol slice. The OpenSpec change records why it was added;
this document defines the long-lived product behavior.

## Products and Scope

When `CMAKE_SYSTEM_NAME` is exactly `Darwin` or `Linux`,
`PHOTOSPIDER_BUILD_IPC` defaults to `ON` and builds:

- `photospider_ipc_client`, an installed static library exported as
  `Photospider::photospider_ipc_client`;
- `photospider_ipc_server_internal`, a non-installed server/router library;
- `photospiderd`, an installed foreground executable.

The public client headers are `photospider/ipc/protocol.hpp` and
`photospider/ipc/client.hpp`. They expose typed owned values and no JSON type,
socket descriptor, `sockaddr_un`, backend model, runtime service, or mutable
backend ownership. The client library does not link the `photospider` backend.
When IPC is disabled, neither its target nor its public headers are installed.
Forcing IPC on for any other CMake system name fails configuration.

`daemon.version.methods` is sourced from one centralized advertisement table,
independent of route dispatch matchers, and reports this exact eight-name
metadata subset:

1. `daemon.ping`
2. `daemon.version`
3. `graph.load`
4. `graph.close`
5. `graph.list`
6. `inspect.graph`
7. `inspect.node`
8. `inspect.dependency_tree`

The request router also accepts these 47 additional daemon-routed typed
methods:

1. `cache.cache_all_nodes`
2. `cache.clear_all`
3. `cache.clear_drive`
4. `cache.clear_memory`
5. `cache.free_transient`
6. `cache.synchronize_disk`
7. `compute.last_error`
8. `compute.last_io_time`
9. `compute.release`
10. `compute.result`
11. `compute.status`
12. `compute.submit`
13. `compute.timing`
14. `dirty.begin`
15. `dirty.end`
16. `dirty.update`
17. `events.drain`
18. `graph.clear`
19. `graph.node_yaml.get`
20. `graph.node_yaml.set`
21. `graph.reload`
22. `graph.save`
23. `inspect.ending_nodes`
24. `inspect.node_ids`
25. `inspect.roi_backward`
26. `inspect.roi_forward`
27. `inspect.compute_planning`
28. `inspect.dirty_region`
29. `inspect.recent_compute_planning`
30. `inspect.traversal_details`
31. `inspect.traversal_orders`
32. `inspect.trees_containing_node`
33. `plugins.load_report`
34. `plugins.ops_combined_keys`
35. `plugins.ops_combined_sources`
36. `plugins.ops_sources`
37. `plugins.seed_builtins`
38. `plugins.unload_all`
39. `scheduler.configure_defaults`
40. `scheduler.description`
41. `scheduler.info`
42. `scheduler.load`
43. `scheduler.loaded_plugins`
44. `scheduler.replace`
45. `scheduler.scan`
46. `scheduler.trace`
47. `scheduler.types`

The installed typed `ps::ipc::Client` exposes calls only for the eight-name
metadata subset and has no public raw-JSON escape hatch. The 47 additional
schemas are daemon request-router behavior. Of those methods,
`compute.status`, `compute.result`, and `compute.release` operate only on the
daemon job registry. `compute.submit` admits a registry job whose worker later
performs exactly one matching Host compute call. The other 43 methods route
their direct or first-page request through matching Host operations; a stable
collection continuation reads only its frozen snapshot registry record.

Version 1 now exposes daemon-owned compute-job submission, polling, terminal
result, release, and protected metadata-only image delivery at the router
boundary. Image bytes stay in private artifacts; a terminal nonempty image
result returns only the specified artifact metadata under one stable delivery
lease. Version 1 exposes no compute cancellation, `daemon.shutdown`, TCP,
Windows named pipe, or `graph_cli --connect`.
Process-global operation-plugin control and sorted views are available at the
daemon router boundary. The installed typed Client currently does not expose
these plugin methods.
Scheduler-plugin discovery/defaults and per-session scheduler
inspection/replacement are likewise available only at the daemon router
boundary; the installed typed Client does not expose them. The bounded
`events.drain` and `scheduler.trace` observation routes remain daemon-router
only. The existing `graph_cli` continues to create an embedded Host and keeps
its local command semantics.

## Transport and Frame

The daemon listens only on a Unix domain stream socket. Each frame is:

```text
uint32 payload_size in network byte order
payload_size bytes of UTF-8 JSON object text
```

`payload_size` is in the inclusive range `1..16,777,216`. The implementation
validates the four-byte header before allocating the payload, loops over
partial reads and writes, and retries `EINTR`. Clean EOF before any header byte
ends that client normally. EOF inside a header or body, zero length, and an
oversized length close only that connection without reading an untrusted body
or inventing an uncorrelated response. Linux sends use `MSG_NOSIGNAL`; macOS
uses socket-local `SO_NOSIGPIPE`. No process-global SIGPIPE policy changes.

JSON object keys must be unique after escape decoding. Duplicate keys are an
invalid request; they are never resolved with last-value-wins behavior.

## Correlated Envelopes

A valid request is:

```json
{
  "protocol_version": 1,
  "id": "client-chosen-id",
  "method": "daemon.ping",
  "params": {}
}
```

`protocol_version` is an exact integer. `id` and `method` are nonempty UTF-8
strings no longer than 128 bytes each, and `params` is an object. Byte limits
are measured after JSON escape decoding. After parsing, unknown members of any
typed object are ignored for forward compatibility. Missing, wrong-type,
non-finite, overflowing, or over-limit known request members are rejected
before session resolution or Host access. Request ids correlate one response;
they are not idempotency keys.

A successful response contains exactly one result branch:

```json
{"protocol_version":1,"id":"client-chosen-id","result":{}}
```

A failure contains exactly one error branch:

```json
{
  "protocol_version": 1,
  "id": "client-chosen-id",
  "error": {
    "domain": "graph",
    "code": 2,
    "name": "not_found",
    "message": "diagnostic text"
  }
}
```

Invalid JSON or an envelope from which no valid id can be recovered uses
`"id": null`. An unsupported integer version returns
`protocol/-32001/unsupported_protocol` with `supported_versions: [1]`.
Responses larger than the frame bound are replaced with a correlated bounded
`response_too_large` error; truncated JSON is never written.

The typed move-only `ps::ipc::Client` allows one outstanding call and is not
concurrently callable. Independent clients may run concurrently. It validates
frame size, JSON uniqueness, response version, response shape, and id before
publishing an owned value. Every daemon-generated opaque id follows the shared
32-lowercase-hexadecimal representation defined below; `graph.load` must echo
the requested display session name. It never automatically retries or reconnects,
especially for `graph.load`. `disconnect()` is idempotent and never closes a
daemon-owned graph.

Every integer is decoded according to its signed or unsigned JSON storage and
must fit the exact public destination type before a value is published. Frame,
JSON, envelope, correlation, and error-object violations close the connection
because message synchronization is no longer trustworthy. Once a correlated
result object has been consumed, a typed result-shape or range failure returns a
local Protocol status while leaving the synchronized connection open.

## Typed Values and Pre-framing Bounds

All integer fields are decoded from their signed or unsigned JSON integer
storage and must fit the exact destination width. Negative unsigned values,
fractional numbers, non-finite numbers, and overflow are rejected. Values are
never routed through floating point, wrapped, truncated, or clamped.

Enums use exact lowercase snake-case strings; integer spellings, case folding,
and unknown values are invalid for a known enum field. A pixel ROI is an object
with exact integer `x`, `y`, `width`, and `height` members, each representable
by the public `int`-backed `PixelRect`. Composite value codecs validate into
temporary owned values and publish no partial result on failure.

Opaque server-instance, session, compute, output, delivery, and cursor
identifiers use exactly 32 lowercase hexadecimal characters. They are semantic
tokens, never paths, pointers, display names, or backend handles. A malformed
opaque id in a request is invalid input; one in a daemon result is a local
Protocol result-shape failure.

| Value | Limit |
| --- | ---: |
| Request id or method name | 128 UTF-8 bytes each |
| Session display name, scheduler type, precision, operation/plugin key, event name/source | 1,024 UTF-8 bytes each |
| Filesystem path or scheduler/plugin description | 4,096 UTF-8 bytes each |
| Diagnostic message | 4,096 UTF-8 bytes after bounded truncation |
| Node YAML text or one copied parameter/source string | 8 MiB (8,388,608 UTF-8 bytes) each |
| Directory/path input array | 256 entries |
| General wire page | 4,096 entries |
| One stable collection snapshot | 262,144 entries and 64 MiB |
| Active collection snapshots | 64 records and 256 MiB total |
| Compute-event ring | 8,192 entries per session |
| Scheduler-trace ring | 65,536 entries per session |

String limits apply after JSON decoding and to every known string member.
Known inbound array and page counts are checked before session resolution or
Host access; unknown members are ignored only after the JSON parser has
materialized them. Host-returned strings, arrays, pages, and composite rows are
validated before a cursor is published or a response value is constructed.
Parser and temporary container allocation can therefore precede typed field
validation. A page limit must be a positive exact integer no greater than its
schema-specific maximum; zero, negative, fractional, or overflowing offsets
and limits are invalid. The 16 MiB frame bound remains authoritative, so a
component-valid indivisible response can still become `response_too_large`.

## Stable Errors

`OperationStatus` is the sole public status representation shared by embedded
Host and IPC products, and `OperationErrorDomain` is its stable top-level
category. Canonical success is `ok=true`, `domain=None`, `code=0`, and empty
`name`/`message`. Error origins are:

| Domain | Origin |
| --- | --- |
| Transport | always produced locally by the client |
| Protocol | produced by local client validation or a daemon response |
| Graph | decoded from a daemon response |
| Daemon | decoded from a daemon response |

Every remote Protocol, Graph, or Daemon failure has string `domain`, signed
integer `code`, string `name`, and diagnostic `message`. Its `code`/`name`
mapping is stable for version 1. Locally produced Protocol statuses use the
same version 1 validation categories; messages are not a branching contract.

An envelope error contains `domain`, `code`, `name`, and `message` and never
represents success. A nested `OperationStatus` value additionally contains
`ok`. Its only canonical success is
`{ok:true, domain:"none", code:0, name:"", message:""}`. Failure uses
`ok:false`, a non-`none` domain, one signed code, a stable name, and a bounded
diagnostic. Top-level envelope errors and nested operation outcomes are
distinct shapes but share the same domain/code/name mapping. Unknown object
members remain forward-compatible, while malformed known members reject the
whole value.

| Domain | Code | Name |
| --- | ---: | --- |
| protocol | -32700 | `parse_error` |
| protocol | -32600 | `invalid_request` |
| protocol | -32601 | `method_not_found` |
| protocol | -32602 | `invalid_params` |
| daemon | -32603 | `internal_error` |
| protocol | -32001 | `unsupported_protocol` |
| protocol | -32002 | `response_too_large` |
| daemon | -32010 | `job_not_found` |
| daemon | -32011 | `job_not_ready` |
| daemon | -32012 | `capacity_exceeded` |
| daemon | -32013 | `artifact_not_found` |
| daemon | -32014 | `artifact_limit_exceeded` |
| daemon | -32015 | `cursor_not_found` |

Host failures use the graph domain and an explicit `GraphErrc` mapping:

| Code | Name |
| ---: | --- |
| 1 | `unknown` |
| 2 | `not_found` |
| 3 | `cycle` |
| 4 | `io` |
| 5 | `invalid_yaml` |
| 6 | `missing_dependency` |
| 7 | `no_operation` |
| 8 | `invalid_parameter` |
| 9 | `compute_error` |

Local connect/read/write/peer failures use
`OperationErrorDomain::Transport` and are never converted to graph IO errors.
A daemon cannot send the transport domain. The Transport domain is
programmatically stable, but its local numeric code and name are diagnostic
classifications; clients must not persist or branch on a promised long-term
Transport code/name mapping. Unknown future remote numeric codes and names
remain available in `OperationStatus`. For every currently known wire code,
the name must be its canonical version 1 name; a known code/name mismatch is
malformed. A currently known name likewise appears only with its canonical
code. A numeric code and nonempty name that are both outside the current table
are preserved together for forward compatibility rather than collapsed or
guessed. Future Graph values are not coerced to `GraphErrc`; checked conversion
succeeds only for the explicit current 1..9 mapping. A wire diagnostic,
including any truncation marker, is valid UTF-8 and at most 4,096 bytes.
Allocation failure may propagate `std::bad_alloc`; other recoverable failures
are returned as statuses. Clients never branch on `message`.

## Daemon Metadata

At process start, the daemon generates one 128-bit lowercase hexadecimal
`server_instance_id` from operating-system entropy. `daemon.ping` takes an
object with no currently known members, ignores unknown members, and returns:

```json
{"pong":true,"server_instance_id":"32-lowercase-hex"}
```

`daemon.version` applies the same params rule and returns protocol version 1,
service name `photospiderd`, the CMake project version, the same instance id,
transport `unix`, and the sorted exact eight-name metadata subset listed under
Products and Scope. These metadata calls do not acquire the Host mutex.

## Opaque Graph Sessions

`graph.load` requires a safe `session_name` of at most 1,024 UTF-8 bytes and an
absolute `root_dir` of at most 4,096 UTF-8 bytes. The name is one nonempty path
component other than `.` or `..`; slash, backslash, and NUL are forbidden.
Optional `yaml_path`, `config_path`, and `cache_root_dir` must be absolute and
at most 4,096 UTF-8 bytes when nonempty; absence maps to empty Host strings.
An over-limit value returns `invalid_params` before reservation, session
resolution, or Host access.

The daemon passes the caller name unchanged as `GraphLoadRequest.session`, so
existing `<root>/<session>` and cache paths do not change. Separately it
reserves a collision-checked 128-bit opaque id. The mutex-protected registry
tracks loading reservations plus active opaque, exact Host-id, and display-name
indexes:

```text
opaque session_id <-> exact Host-returned GraphSessionId
```

Host load failure or exception removes the reservation. Registry publication
failure removes the reservation before attempting to close the newly loaded
Host session. A successful result exposes only:

```json
{"session_id":"32-lowercase-hex","session_name":"caller-name"}
```

`graph.list` calls `Host::list_graphs()` and reconciles it with committed
mappings. A disagreement is a daemon invariant error and leaks no untracked
Host name. Returned rows are sorted by `session_name`, then `session_id`.

`graph.close` resolves only the opaque id through the session lifecycle gate.
It first marks the mapping closing, rejects every new session-scoped Host or
compute admission with Graph `not_found`, and waits for already-admitted Host
calls plus queued/running jobs without holding the Host mutex. It then acquires
the Host mutex and calls `Host::close_graph`. Successful Host close removes all
three active indexes. Host `NotFound` also removes the stale mapping while
preserving the failure response; any other Host failure atomically reopens the
mapping. Closing rows are omitted from `graph.list` results while remaining
part of Host/registry invariant reconciliation. Disconnecting a client does
not close sessions.

## Host-Routed Graph State and Diagnostics

Every method in this section validates all known parameters before resolving
the opaque `session_id`. A malformed known field returns
`protocol/-32602/invalid_params` without Host access. A well-formed unknown or
closing session returns Graph `not_found`. Unknown object members are ignored.
After admission, the daemon holds the common Host mutex for exactly one
matching `ps::Host` call and releases it before response encoding. No mutation
is automatically retried, including when the Host mutation completed but its
copied result cannot be encoded.

The following status-only methods return the unique success value `result:{}`:

- `graph.reload` and `graph.save` require `session_id` plus a nonempty absolute
  `yaml_path` of at most 4,096 UTF-8 bytes with no NUL;
- `graph.clear` requires only `session_id` and preserves the active opaque
  session mapping after clearing graph model state;
- `graph.node_yaml.set` requires `session_id`, a nonnegative exact `int`
  `node_id`, and string `yaml_text` of at most 8 MiB;
- `cache.clear_all`, `cache.clear_drive`, `cache.clear_memory`, and
  `cache.free_transient` require only `session_id`;
- `cache.cache_all_nodes` and `cache.synchronize_disk` additionally require a
  string `precision` of at most 1,024 UTF-8 bytes.

Empty `yaml_text` and `precision` strings are valid protocol values. Their
semantic validity belongs to the matching Host method, whose exact Graph
failure is returned. A path is validated by the protocol because its absolute,
nonempty, NUL-free shape is part of the wire schema.

`graph.node_yaml.get` requires `session_id` and nonnegative exact `node_id`.
Success returns:

```json
{
  "session_id": "32-lowercase-hex",
  "node_id": 7,
  "yaml_text": "complete Host-returned YAML"
}
```

The returned YAML must be valid UTF-8 and at most 8 MiB; it is never truncated.
`inspect.node_ids` and `inspect.ending_nodes` require only `session_id` and
return `{session_id,node_ids}` or `{session_id,ending_node_ids}` respectively.
Their arrays use the stable collection-page metadata described below, with at
most 4,096 nonnegative exact `int` values per page and up to 262,144 in the
admitted snapshot. Host order and duplicate ids are preserved; the router
neither sorts, deduplicates, nor truncates them.

`inspect.roi_forward` requires `start_node_id`, `start_roi`, and
`target_node_id`. `inspect.roi_backward` requires `target_node_id`,
`target_roi`, and `source_node_id`. Node ids are nonnegative exact `int` values;
each ROI has exact `int` `x`, `y`, `width`, and `height` members. Non-positive
width or height remains representable. Success returns `{session_id,roi}` and
preserves the Host rectangle without swapping nodes or axes.

`dirty.begin` and `dirty.update` require `session_id`, nonnegative exact
`node_id`, `domain`, and `source_roi`; `dirty.end` requires the same fields
except `source_roi`. `domain` is exactly `high_precision` or `real_time`.
Success returns the copied dirty snapshot as:

```text
{
  session_id, graph_generation,
  sources: [{node_id, domain, lifecycle, generation, source_rois}],
  dirty_tiles: [{node_id, domain, tile_x, tile_y, tile_size, pixel_roi}],
  dirty_monolithic_nodes:
      [{node_id, domain, pixel_roi, whole_output}],
  actual_dirty_rois: [{node_id, rois}],
  edge_mappings:
      [{from_node_id, to_node_id, domain, from_roi, to_roi, direction}]
}
```

Every top-level dirty array and every nested ROI array contains at most 4,096
entries. Vector order is preserved; `actual_dirty_rois` follows ascending
integer map-key order. Dirty lifecycle labels are `idle`, `updating`, and
`settled`; edge directions are `forward_affected` and `backward_demand`.

`compute.timing` requires only `session_id` and returns
`{session_id,node_timings,total_ms}`. Each ordered timing row contains
`node_id`, `name`, `elapsed_ms`, and `source`; at most 4,096 rows are returned.
Names are at most 1,024 UTF-8 bytes and sources at most 8 MiB each.
`compute.last_io_time` returns `{session_id,last_io_time_ms}`. Non-finite timing
or IO doubles encode as JSON null.

After the single Host call returns and the Host mutex is released, the timing
and dirty codecs validate every component bound and then preflight one
overflow-safe compact-byte budget for the complete success response before
allocating result JSON arrays or objects. The budget includes the actual
request and session ids, fixed envelope/schema bytes, exact JSON string
escaping, exact integer decimal widths, and exact boolean/null widths. Finite
timing doubles contribute a one-byte lower bound instead of a worst-case width.
Every addition checks remaining capacity before arithmetic. A proven size over
16 MiB throws `std::length_error`, which the router maps to correlated protocol
`response_too_large`; an uncertain near-boundary value proceeds to the final
serializer, so preflight cannot reject a response that would actually fit.

`compute.last_error` returns `{session_id,status}` in a successful response.
The nested value uses the canonical `OperationStatus` schema. An observed Host
failure is diagnostic data for this method and is not converted into a
top-level request failure; an unknown or closing session still fails at the
top level before Host access.

A failed Host result from every other method becomes the exact top-level Graph
error. Returned YAML, rows, or collections above their byte/count limits
become `response_too_large` after the single Host call. A negative returned
node id, unknown returned enum, or invalid returned UTF-8 is a daemon
`internal_error`. These failures publish no partial result and never cause a
second Host invocation. Resource exhaustion remains exceptional and is not
rewritten into a status.

## Bounded Event and Scheduler Trace Observations

`events.drain` requires an opaque `session_id` and exact integer `limit` in
`1..1024`. It invokes `Host::drain_compute_events` exactly once under the
common Host mutex. The result is:

```json
{
  "session_id": "32-lowercase-hex",
  "events": [
    {
      "sequence": 1,
      "node_id": 7,
      "name": "node",
      "source": "compute",
      "elapsed_ms": 1.25
    }
  ],
  "next_sequence": 2,
  "has_more": false,
  "dropped_count": 0
}
```

The Host event API is destructive and applies its limit before removal. A
successful call removes only the returned oldest page and atomically returns
and resets the shared per-session drop count, including on an empty page.
Independent clients therefore share one drain position and one drop counter;
an invalid call changes neither. `has_more` is the post-removal state observed
under the ring lock. `next_sequence` is one past the last returned event, the
next publication sequence on an empty pre-exhaustion page, or `UINT64_MAX`
after exhaustion with no retained later event.

Before event name or source text enters retained storage, each value must be
canonical UTF-8 and at most 1,024 bytes. Invalid or oversized text drops the
whole publication without truncation or repair. While a valid sequence
remains, that attempt consumes it and increments the shared drop count exactly
once. A full ring similarly consumes the sequence, evicts exactly the oldest
event, retains the newest event, and increments once. After exhaustion, only
the exhaustion path increments once. All increments saturate at `UINT64_MAX`.

`scheduler.trace` requires `session_id`, exact unsigned `after_sequence`, and
an exact integer `limit` in `1..4096`. Zero starts at the oldest retained trace;
the router invokes `Host::scheduler_trace` exactly once and returns:

```json
{
  "session_id": "32-lowercase-hex",
  "events": [
    {
      "sequence": 9,
      "epoch": 3,
      "node_id": -1,
      "worker_id": -1,
      "action": "rethrow_exception",
      "timestamp_us": 1234
    }
  ],
  "next_sequence": 9,
  "has_more": false,
  "dropped_count": 8
}
```

Trace pages are non-destructive and contain only events whose sequence is
greater than the exclusive cursor. A nonempty pre-terminal page returns its
last sequence; an empty pre-exhaustion page preserves the input cursor. Only a
page that observes the final valid sequence `UINT64_MAX-1` with no later entry,
or an exhausted empty observation, returns the `UINT64_MAX` sentinel with
`has_more:false`. `dropped_count` is the exact saturating sum of missing
retained history after the cursor and unsequenced attempts after exhaustion.
Repeating a cursor can therefore reproduce the same retained page, while
advancing through `next_sequence` loses or duplicates no retained event.

Each returned trace event uses a nonnegative `node_id` and `worker_id` when a
specific node or worker applies. `-1` is the sole sentinel for no specific
node or worker; any value below `-1` is malformed Host output. The router
rejects the complete response as daemon `internal_error` after that one Host
call and never retries the observation.

There is no `max_entries` field in either method. Unknown fields remain
forward-compatible but cannot replace a missing or invalid known `limit`.
Both routes use the Host's bounded observation APIs directly and never reserve
a stable collection snapshot or create a daemon cursor.

## Compute Job Lifecycle

The request router exposes `compute.submit`, `compute.status`,
`compute.result`, and `compute.release` over the daemon-owned lifecycle. The
installed direct Client does not yet wrap these methods and exposes no raw JSON
call; that public typed API is a later slice. The server owns one explicitly
started, joinable `ComputeRequestRegistry` worker and one shared
session-admission gate. This infrastructure remains private implementation of
the typed wire methods rather than a second public Host.

`compute.submit` requires `session_id`, nonnegative `node_id`, and
`result_mode` equal to `status` or `image`. It accepts the existing Host request
as nested values:

```json
{
  "session_id": "32-lowercase-hex",
  "node_id": 37,
  "cache": {
    "precision": "float16",
    "force_recache": true,
    "disable_disk_cache": true,
    "nosave": true
  },
  "execution": {"parallel": true, "quiet": true},
  "telemetry": {"enable_timing": true},
  "intent": "real_time_update",
  "dirty_roi": {"x": -4, "y": 5, "width": 6, "height": 7},
  "result_mode": "status"
}
```

The three nested option objects and their known members are optional and retain
the public `HostComputeRequest` defaults when absent. `intent` and `dirty_roi`
may be absent or JSON null. Every present known value is type-, range-, UTF-8-,
and byte-bound-checked before session lookup or registry admission; unknown
members are ignored for forward compatibility.

Before a queue commit, submission admits the active session, enforces a global
maximum of 64 queued/running records, validates and collision-checks a
daemon-generated 32-lowercase-hex compute id, and reserves all queue/terminal
bookkeeping. A successful commit snapshot is always `queued` with
`cancellable: false`. The sole FIFO worker advances a record to `running`,
invokes exactly one matching synchronous Host callback for `status` or `image`
mode, and publishes exactly one immutable `succeeded` or `failed` terminal
status. Host calls use the same daemon Host mutex; image publication occurs
after that callback releases the Host mutex.

Submit, status, and result use one stable result schema:

```json
{
  "compute_id": "32-lowercase-hex",
  "session_id": "32-lowercase-hex",
  "state": "queued|running|succeeded|failed",
  "cancellable": false,
  "status": null,
  "output": null
}
```

`status` is null while queued/running and is the canonical exact nested
`OperationStatus` after terminal publication. Submit, status, status-mode
result, empty-image result, and failed result keep `output` null. Only
`compute.result` for a terminal successful nonempty image returns this exact
object in `output`:

```json
{
  "output_id": "32-lowercase-hex",
  "delivery_id": "32-lowercase-hex",
  "path": "/protected/socket.outputs/instance-id/output-id.bin",
  "width": 2,
  "height": 2,
  "channels": 1,
  "data_type": "uint8",
  "device": "cpu",
  "row_step": 2,
  "byte_size": 4,
  "filesystem_device": 16777234,
  "inode": 12345
}
```

The path is the protected absolute artifact path, not a backend cache path or
caller-selected output. The registry's opaque reference is published only as
the normalized `output.output_id`; no extra `output_reference` field or backend
handle enters JSON.
Successful terminal release has the separate stable acknowledgement
`{"compute_id":"...","released":true}`. `compute.release` accepts an optional
`delivery_id`; when present it must use the exact opaque-id shape before any
mutation. Unknown other fields remain forward-compatible.

Status reads are non-destructive and do not acquire the Host mutex. Result and
release accept terminal records only, and release checks/removes the record in
one registry operation. A well-formed absent, expired, released, or evicted id
maps to daemon `job_not_found`; a queued/running result or release maps to
daemon `job_not_ready`. Host failures remain exact nested terminal statuses.
Every accepted Host failure, invalid image, output quota denial, or publication
failure remains an immutable failed job in successful status/result envelopes;
quota denial is nested daemon `artifact_limit_exceeded`, while unexpected
worker/publication failure is nested daemon `internal_error`. Only malformed
params, submit/admission failure, absent jobs, premature result/release, and
result-time missing or identity-mismatched artifacts are top-level failures.
The last case is exactly daemon `artifact_not_found`; it does not change the
immutable terminal job, which remains releasable. Any exception after queue
commit uses a fallback `internal_error` allocated before commit, so the worker
continues to later jobs.

At most 256 terminal records are retained. Publishing another terminal record
evicts only the oldest terminal publication; active work is never evicted.
Terminal age starts at complete publication, uses a monotonic injected clock,
expires at 15 minutes, and is not refreshed by lookup. Output ownership is a
private move-only reference with optional lease-aware exact-once cleanup outside
the registry mutex. It is backed by the private protected store described
below. Result exposes its reference only as the revalidated delivery's
normalized `output_id`, never as an extra field or backend handle. Release
without a matching delivery removes job ownership only; release with a
matching id removes job ownership and its active lease in the same OutputStore
critical section.

## Protected Output Store and Image Result

The wire surface exposes image metadata only from terminal `compute.result` and
uses optional lease-aware `compute.release`; it adds no separate delivery
method. Behind the router, one private `OutputStore` is bound to the joined
compute publisher and socket lifecycle. It starts after the Unix socket and
persistent lifecycle lock are owned but before session, compute, or client
admission. A startup failure therefore admits no request.

A canonical empty CPU image publishes no artifact. A nonempty result must be a
valid CPU `ImageBuffer` with a defined data type, positive dimensions and
channels, non-null data, a source step at least as large as its checked tight
row, and overflow-safe row/total byte counts. The store copies only tight rows;
source padding and backend context never enter the artifact. Validation,
allocation, write, rename, or identity failure becomes nested daemon
`internal_error` through the already-accepted compute job. Quota denial becomes
nested daemon `artifact_limit_exceeded`.

Production limits are 64 retained artifacts, one GiB total retained bytes, and
512 MiB per artifact. Admission includes job-owned and lease-only artifacts and
is transactional: a failed publication retains no record, quota reservation,
or safely removable partial file. Every controlled stage and final basename is
`output-<32-lowercase-hex>.bin`, so restart cleanup can recognize a stage left
by process death. Complete data is atomically renamed without replacement and
then revalidated before a record becomes visible.

The store base is the socket-specific same-owner exact-mode `0700`
`<socket>.outputs`; one process writes below the exact-mode `0700`
`instance-<server_instance_id>` child. Artifact files are same-owner regular
files opened with `O_NOFOLLOW|O_CLOEXEC`, exact mode `0600`, and one link.
Metadata retains only output id, absolute path, dimensions, data type, CPU
device, tight row step, byte size, filesystem device, and inode. Live access
revalidates base/instance ancestry plus file owner, type, exact mode, link
count, device, inode, and size through held directory descriptors. A missing or
mismatched record/file returns top-level daemon `artifact_not_found` from
`compute.result` while leaving the terminal compute record releasable; an
unsafe replacement is never followed or removed.

Each published artifact owns one stable 32-hex delivery id. Successful
`compute.result` validates the artifact and, in the same store critical section,
creates, reuses, or reactivates its sole lease with expiry set to that
operation's monotonic `now + 60 seconds`. Repeated access returns the same id
and refreshes that deadline. If response allocation or encoding later fails,
the router does not blindly revoke that stable lease because an earlier
successful result may still rely on it; explicit release or the bounded lease
TTL remains responsible for cleanup. Compute release accepts an optional wire
delivery id: matching release atomically removes job ownership and the lease;
eviction, the 15-minute compute/job TTL, or release without a match removes only
job ownership. Quota and the file remain while an active lease survives. A
matching `(compute_id, delivery_id)` can still release that orphaned lease after
the compute record disappeared. Unlink occurs only after neither owner nor
lease remains and only after identity revalidation.

Restart cleanup runs while the socket lifecycle lock proves no live cooperating
daemon owns the socket. It opens the real base and recognized instance children
without following symlinks, scans through directory descriptors, and unlinks
only controlled same-owner exact-`0600` one-link regular files. It removes only
empty recognized instance directories. Unknown names, symlinks, non-regular
entries, wrong modes/owners, hard links, and replaced entries remain untouched;
no persisted output registry is required.

The persistent lifecycle lock serializes cooperating daemon instances. Portable
POSIX has no compare-device/inode-and-unlink primitive, so each name-based
unlink or empty-directory removal is immediately preceded by held-fd and path
identity revalidation. This is the supported boundary against concurrent
replacement: an observed mismatch or inconclusive system error is preserved,
while a same-uid hostile process can still race the final validation and
`unlinkat` instructions at the platform level.

Shutdown immediately stops new or refreshed delivery leases while accepted
image jobs may still publish during compute drain. After the joined compute
worker exits, terminal cleanup removes job ownership, the store stops
publication, waits for active leases to be explicitly released or reach their
injected monotonic TTL, identity-cleans unowned artifacts, and closes its
directories before Host sessions are closed. Quotas, both TTLs, clock, and id
source are injectable for deterministic filesystem and race tests.

## Private Stable Collection Snapshots

The router owns a private type-erased `CollectionSnapshotRegistry` and starts,
stops, and clears it with daemon runtime lifecycle. Public Host collection APIs
remain full-value APIs; paging is a daemon-owned wire concern and adds no Host
page API or public ABI type. `graph.list`, `inspect.node_ids`,
`inspect.ending_nodes`, `inspect.graph`, `inspect.dependency_tree`,
`inspect.traversal_orders`, `inspect.traversal_details`,
`inspect.trees_containing_node`, `inspect.recent_compute_planning`,
`plugins.ops_sources`, `plugins.ops_combined_keys`, and
`plugins.ops_combined_sources` use this registry. No separate page or
cursor-release method is advertised.

A first request accepts optional integer `limit` in `1..4096`; absence means
4,096. It must not contain `cursor` or `offset`. A continuation calls the same
method with its original typed non-page parameters plus required 32-lowercase-
hex `cursor`, exact next nonnegative `offset`, and integer `limit` in
`1..4096`. Unknown fields remain forward-compatible. The result retains the
method's collection field and adds integer `offset`, boolean `has_more`, and
`cursor`, which is the stable string while more rows remain and JSON null on
the only or final page. Inspection collections preserve Host order and
duplicates. Operation-plugin views sort by public key before publication and
preserve duplicate keys in that sorted order.

The private `reserve()` operation atomically reserves one record and the
complete 64 MiB per-snapshot allowance. Production admission retains at most
64 records and 256 MiB total, and reports private `CapacityExceeded` when that
quota is unavailable. `publish()` accepts the caller's complete collection,
exact recursive public-entry count, exact measured byte count, binding,
requested page limit, and a frame-safe page ceiling. More than 262,144 entries
or 64 MiB reports protocol `response_too_large`; the reservation is rolled back
and no cursor or retained snapshot is published. Admission exhaustion reports
daemon
`capacity_exceeded` before Host access. An admitted multi-page value is moved
into the registry, and the worst-case reservation is transactionally replaced
by its measured byte count before another admission observes the quota. Empty
and single-page values return directly and retain no record.

The entry count is recursive rather than the number of page rows. It counts
every element of the outer session, node-id, node, dependency, traversal, or
planning-history vector/map; every node-parameter map entry; the 27 elements
of the three public 3x3 spatial matrices when spatial metadata is present;
dependency roots and flattened entries plus collections in their nested nodes;
each traversal branch's nested node vector; and every recent-planning
`planned_node_sample`, `task_sample`, and task `dependency_task_ids` element.
Scalar/object members and the presence of an optional value do not add an
entry. The registry separately retains the actual top-level row count for
paging/type invariants and rejects a reported recursive count smaller than
that row count, including under injected test limits.

After the one Host call, dependency roots/entries are pre-scanned before a
shared header, root copy, or row vector is allocated, and traversal maps are
pre-scanned before map-to-vector transformation. A recursive over-count is
therefore `response_too_large` with no cursor or quota leak. Snapshot-byte
measurement counts each retained variable-width public collection exactly
once. In particular, dependency roots and scalar tree fields form the shared
page header, so dependency measurement replaces that encoded header's empty
`entries` token (`[]`) with the measured complete entries array. Its
calculation is
`header_bytes - 2 + entries_array_bytes` rather than counting the empty token
twice.

A multi-page publication receives one collision-checked 32-lowercase-hex
cursor and freezes the exact method, optional opaque session id, and canonical
identity of the original non-page parameters. Before publication the router
measures rows one at a time without building a complete JSON DOM and computes
the largest fixed page ceiling whose every contiguous page fits the 16 MiB
frame. The calculation includes a maximally escaped legal 128-byte request id,
the 32-byte cursor, maximum offset text, and the method-specific header. A
caller may request fewer rows on a continuation but cannot exceed that frozen
safe ceiling. One indivisible row that cannot fit reports
`response_too_large` before cursor publication.

A continuation lookup must provide the frozen identity and exact next offset.
Binding/type/offset mismatch and unknown or expired well-formed cursors report
daemon `cursor_not_found` without advancing the record. A malformed cursor,
zero/over-limit page, or offset arithmetic overflow reports protocol
`invalid_params` and is likewise non-destructive. Continuation pages read only
the retained value, perform no Host call or live-session lookup, and therefore
remain readable after `graph.close`. Copy failure also leaves the next offset
unchanged.

The final page atomically erases the record and releases its measured quota.
Otherwise the fixed 15-minute monotonic TTL, measured from cursor publication,
releases it and is never refreshed by paging. Beginning daemon shutdown stops
new reservations while preserving active reservations and published records;
after client workers join, final snapshot shutdown clears all records and
reservations before Host sessions are closed. A subsequent runtime start
enables admission on the empty registry. Limits, clock, and id source are
injectable for deterministic capacity, final-page, expiry, and shutdown races.

Collection fields are:

- `graph.list`: `sessions` rows;
- `inspect.node_ids`: `node_ids`; `inspect.ending_nodes` and
  `inspect.trees_containing_node`: `ending_node_ids`;
- `inspect.graph`: `nodes`;
- `inspect.dependency_tree`: the scalar tree header and complete bounded
  `root_node_ids` are repeated, while flattened `entries` are paged;
- `inspect.traversal_orders`: `orders` rows shaped as
  `{ending_node_id,node_ids}`;
- `inspect.traversal_details`: `branches` rows shaped as
  `{ending_node_id,nodes}`, where each node contains `node_id`, `name`,
  `has_memory_cache`, and `has_disk_cache`;
- `inspect.recent_compute_planning`: `snapshots`.
- `plugins.ops_combined_keys`: sorted string `keys`;
- `plugins.ops_sources` and `plugins.ops_combined_sources`: sorted `sources`
  rows shaped as `{key,source}`.
- `scheduler.types`: sorted string `types`;
- `scheduler.loaded_plugins`: sorted diagnostic string `plugins`.

The installed typed Client accepts the additional page metadata as unknown
fields and returns complete small single-page graph/tree values. It does not
currently issue cursor continuations; callers that need multi-page router
values must use the typed wire schema directly.

## Operation Plugin Control and Views

The six operation-plugin methods are process-global and do not define, read, or
resolve `session_id`; a supplied `session_id` is only an unknown member and is
ignored. Unknown object members remain forward-compatible. Every first view
request reserves stable-snapshot quota before acquiring the common Host mutex
and invoking exactly one matching `ps::Host` method. Every mutation also
invokes exactly one matching Host method under that mutex and is never
automatically retried. No loader, registry, factory, callback, DSO handle, or
mutable ownership value enters JSON.

`plugins.load_report` requires:

```json
{"directories":["relative/or/absolute/plugin/path-or-pattern"]}
```

The required array contains at most 256 entries and may be empty. Each entry
must be a nonempty, NUL-free valid UTF-8 string of at most 4,096 bytes;
relative paths and Host-supported patterns are preserved rather than rewritten
by the router. Success returns the exact copied Host report:

```json
{
  "attempted": 2,
  "loaded": 1,
  "errors": [
    {"path":"","code":4,"name":"io","message":"bounded diagnostic"}
  ],
  "new_op_keys": ["namespace:operation"]
}
```

`attempted` and `loaded` are nonnegative exact Host integers;
`loaded + errors.size()` equals `attempted`. `errors` and `new_op_keys` each
contain at most 4,096 entries. An error path may be empty, otherwise remains an
exact valid UTF-8 value of at most 4,096 bytes. Error `code` is one of the nine
current `GraphErrc` numeric values and `name` is its canonical lowercase name;
this row is report data, not a nested or top-level `OperationStatus`. Error
messages use the common repair/truncate-to-4,096-byte diagnostic policy. Every
new operation key is nonempty valid UTF-8 of at most 1,024 bytes. Error and key
order are preserved exactly; an aggregate that cannot fit the 16 MiB frame is
rejected as `response_too_large`. The status-only `Host::plugins_load()` IPC
mapping calls this same method, validates the complete successful report, and
only then discards it; there is no second wire alias.

`plugins.unload_all` defines no known params and returns `{"unloaded":N}` with
the exact nonnegative Host count. `plugins.seed_builtins` likewise defines no
known params and returns `{}`. Unknown members of either params object are
ignored. Repeated seeding preserves the Host's idempotent process-owner
behavior; it cannot replace a live plugin override. Host failures from either
mutation retain their exact Graph-domain mapping.

The three copied views use the common first-page and continuation controls:

- `plugins.ops_combined_keys` returns sorted string `keys`;
- `plugins.ops_sources` and `plugins.ops_combined_sources` return sorted
  `sources` rows shaped as `{key,source}`.

Keys are nonempty valid UTF-8 of at most 1,024 bytes. A copied source is valid
UTF-8 of at most 8 MiB and is never interpreted as a path or ownership token.
Before the one Host call the router reserves one snapshot slot and 64 MiB;
after that call it validates every row, sorts by key, measures the complete
value, and moves it into the stable snapshot registry. Continuations bind to
the exact global method and offset, read only the frozen copy, and do not call
Host. The general 4,096-row page, 262,144-entry/64-MiB snapshot,
64-record/256-MiB aggregate, 15-minute TTL, cursor mismatch, expiry, and
shutdown rules above apply unchanged.

Successful plugin libraries remain owned by the Host's unique process-global
plugin owner. The load is visible across independent socket clients, graph
sessions, and Host adapter lifetimes. Disconnecting a client or destroying one
Host adapter does not unload it. Only explicit process-global
`plugins.unload_all` removes/restores active plugin keys; all three later views
then observe the same state. The router does not expose or shorten callback or
returned-value library leases.

## Scheduler Plugin Discovery and Session Control

Eight scheduler methods complement the existing `scheduler.trace` observation
route. They are daemon request-router schemas only: the installed typed Client
and the exact eight-name `daemon.version.methods` advertisement remain
unchanged. Unknown members of every params object are ignored for forward
compatibility.

The six process-global methods do not define, read, or resolve `session_id`:

- `scheduler.types` uses the common first-page/continuation controls and
  returns sorted scheduler type strings in `types`;
- `scheduler.description` requires `{"type":"scheduler_type"}` and returns
  `{"type":"scheduler_type","description":"display text"}`;
- `scheduler.scan` requires
  `{"directories":["scheduler/plugin/directory"]}` and returns
  `{"loaded":N}` with the exact nonnegative Host count;
- `scheduler.load` requires `{"path":"scheduler/plugin/library"}` and
  returns `{}`;
- `scheduler.loaded_plugins` uses the common page controls and returns sorted
  copied diagnostic strings in `plugins`;
- `scheduler.configure_defaults` requires
  `{"hp_type":"hp","rt_type":"rt","worker_count":N}` and returns `{}`.

A scan directory array is required, may be empty, and contains at most 256
nonempty NUL-free valid UTF-8 strings of at most 4,096 bytes each. A load path
has the same per-string rules. Scheduler type inputs and Host-returned type
names are nonempty valid UTF-8 of at most 1,024 bytes. Descriptions and loaded
plugin labels are valid UTF-8 of at most 4,096 bytes; a description may be
empty, while a plugin label may not. `worker_count` is an exact integer in
`0..UINT_MAX`; zero retains the public automatic-worker selection meaning.
Configuration changes the defaults for subsequently loaded graph sessions
only; existing sessions retain their scheduler objects until explicit
replacement.

The two session methods require a valid opaque daemon `session_id` and an
`intent` equal to `global_high_precision` or `real_time_update`:

```json
{"session_id":"0123456789abcdef0123456789abcdef","intent":"global_high_precision"}
```

`scheduler.info` returns:

```json
{
  "session_id":"0123456789abcdef0123456789abcdef",
  "intent":"global_high_precision",
  "scheduler_name":"cpu_work_stealing",
  "stats":"backend-defined display text"
}
```

The returned scheduler name is nonempty valid UTF-8 of at most 1,024 bytes;
statistics are display-only valid UTF-8 of at most 4,096 bytes and may be
empty. The returned intent must equal the request intent. A Host value that
violates these invariants is a daemon `internal_error`, never a partial value.
`scheduler.replace` additionally requires a nonempty bounded `type`:

```json
{
  "session_id":"0123456789abcdef0123456789abcdef",
  "intent":"global_high_precision",
  "type":"serial_debug"
}
```

Successful replacement returns `{}`. Every known value is validated before
opaque-session admission. Missing or closing sessions retain the common daemon
session-status mapping; unavailable scheduler types and other Host failures
retain their exact Graph-domain mapping.

Each `scheduler.types` or `scheduler.loaded_plugins` first page reserves the
common bounded snapshot quota before exactly one Host call, validates and
sorts the complete copied list, preserves duplicates, measures it, and freezes
it in the collection registry. Continuations bind to the exact global method
and offset, read only that frozen value, and never call Host. All general page,
entry/byte quota, cursor, TTL, and shutdown rules apply unchanged.

Every direct scheduler request and every first-page Host access uses the same
daemon Host mutex as compute and the other routed families. Per-session info
and replacement additionally remain admitted through the opaque session while
the embedded Host serializes scheduler copy/replacement with compute and graph
close. Mutations are invoked once and are never retried. Successful scheduler
DSOs remain process-owned across client disconnects and graph sessions; JSON
never exposes a scheduler, factory, registry, loader, callback, DSO handle, or
mutable ownership token. `scheduler.trace` retains its independent bounded,
non-destructive sequence-page contract.

## Inspection Values

Inspection atomically admits the opaque session before acquiring the Host mutex
and calls only `ps::Host`. A concurrent close marks the session first, rejects
new inspection admission, and waits any already-admitted inspection:

- `inspect.graph` returns `{session_id, nodes}`;
- `inspect.node` requires a nonnegative integer `node_id` representable by the
  public `int`-backed `NodeId` and returns
  `{session_id, node}`;
- `inspect.dependency_tree` accepts an optional nonnegative integer/null
  `node_id` with the same range and optional boolean `include_metadata`, then
  returns `{session_id, ...flattened tree fields}`;
- `inspect.traversal_orders` and `inspect.traversal_details` return the copied
  ending-node keyed Host maps through the page rows defined above;
- `inspect.trees_containing_node` requires a nonnegative `node_id` and returns
  copied ending-node ids;
- `inspect.dirty_region` returns the current copied dirty-region snapshot;
- `inspect.compute_planning` returns `{session_id,planning}`, where `planning`
  is null before a planning result exists or an indivisible copied planning
  object;
- `inspect.recent_compute_planning` returns stable pages of copied planning
  snapshots.

Node JSON mirrors `NodeInspectionView` with snake-case fields: integer `id`,
`name`, `type`, `subtype`, a string-valued JSON object `parameters`,
`has_cached_output`, and always-present nullable `source_label`, `debug`, and
`space`. Spatial matrices contain nine entries. Dependency edges contain
integer `from_node_id`/`to_node_id`, `kind` (`image_input` or
`parameter_input`), output/input names, and nonnegative `input_index`. Optional
values use JSON null. Non-finite public doubles encode as null and the typed
client restores quiet NaN.

The typed client range-checks every node id, tree depth, edge input index,
debug timestamp/duration, worker id, and spatial extent/rectangle component
before publishing graph, node, or dependency-tree snapshots. Signed/unsigned
overflow is a local Protocol result-shape failure; it is never narrowed.
Direct `inspect.node`, ROI, dirty-region, and current-planning results do not
publish cursors. Collection methods use the stable metadata above without
changing their copied Host contracts. Planning values encode `ComputeIntent`
and `DirtyDomain` as stable lowercase snake-case labels, nested optional values
as null, and all counts as exact nonnegative integers. The same reusable enum,
`PixelRect`, bounded-string, array, page, opaque-id, and nested-status codecs
apply recursively to composite values. A failed decode leaves caller-visible
state unpublished.

No backend class, address, pointer, cache handle, service object, closure, or
mutable reference enters a payload.

## Socket and Process Lifecycle

`photospiderd` remains in the foreground. `--socket ABSOLUTE_PATH` selects an
explicit socket. Otherwise a valid uid-owned protected `XDG_RUNTIME_DIR` is
preferred; if its final path cannot fit `sun_path`, the daemon uses
`/tmp/photospider-<uid>/photospiderd-v1.sock`. Daemon-created direct runtime
directories are `0700`; the socket and its persistent `${socket}.lock` regular
file are `0600`.

Before inspecting or reclaiming the socket path, the daemon opens the
persistent lock with `O_NOFOLLOW|O_CLOEXEC`, verifies regular-file identity,
owner, single link, and exact `0600` mode, then takes `flock(LOCK_EX|LOCK_NB)`.
The lock is held through socket cleanup and is never unlinked, so simultaneous
starters serialize on one stable inode and process death releases ownership.
While holding it, the daemon uses `lstat` to refuse symlinks, non-sockets, and
paths owned by another uid. A bounded nonblocking probe preserves a live
same-owner listener. Only a same-owner socket whose connection is refused is
revalidated by device/inode/owner and removed as stale. Cleanup records the
created device/inode/owner and unlinks only that exact socket before releasing
the lifecycle lock.

The listener tracks at most 32 joinable client workers. Requests are sequential
per connection; frame/JSON work may occur across clients. Because public Host
does not promise thread safety, every Host call uses one daemon mutex. Socket
reads and writes never hold it. Shutdown first stops all session, snapshot, and
compute admission plus new output leases, closes the listener, shuts down
tracked client descriptors to wake reads, and joins all connection workers. It
then drains every accepted compute job, joins the sole compute worker, releases
terminal job ownership, clears retained collection snapshots, waits active
delivery leases through explicit release or TTL, and closes the OutputStore
before Host sessions and session mappings. Finally it removes its socket while
the lifecycle lock remains held, releases the lock, and destroys Host state.
The persistent lock file intentionally remains.

Before installing SIGINT/SIGTERM handlers, `photospiderd` creates a nonblocking
close-on-exec self-pipe. The handler only preserves `errno`, writes one byte,
and restores `errno`. Normal control flow performs cleanup. Normal signal
shutdown exits zero; option/startup failures produce a diagnostic and nonzero
status. There is no protocol shutdown method and no daemonizing fork.

## Validation

Focused local commands are:

```bash
cmake -S . -B build -DPHOTOSPIDER_BUILD_IPC=ON -DBUILD_TESTING=ON
cmake --build build --target photospider_ipc_client \
  photospider_ipc_server_internal photospiderd test_ipc_protocol \
  test_compute_request_registry test_collection_snapshot_registry \
  test_output_store test_event_stream_boundaries test_ipc_daemon \
  public_header_self_containment -j
ctest --test-dir build --output-on-failure \
  -R '^(FrameCodec|ProtocolEnvelope|IntegerCodec|ProtocolErrors|ProtocolParams|ProtocolGraphLoad|ProtocolGraphClose|ProtocolOperationPlugins|HostRoutedGraphStateProtocolTest|StableInspectionPagingProtocolTest|InspectionJson|SessionRegistry|ComputeRequestRegistry|CollectionSnapshotRegistry|OutputStore|ComputeEventRing|SchedulerTraceRing|ClientLifecycle|ClientResultValidation|IpcDaemon|IpcDaemonOperationPlugins|IpcDaemonSchedulers|IpcObservationFixtureDaemon|StaticProductConsumerSmoke|IpcDisabledInstallSmoke|PublicHeaderSelfContainment)'
```

`StaticProductConsumerSmoke` verifies the installed backend plus a second
client-only consumer. `IpcDisabledInstallSmoke` verifies an IPC-disabled clean
install has no IPC forwarder, header, target, archive, or daemon while the
embedded consumer remains usable. Real-process tests have CTest timeouts and
bounded SIGTERM/SIGKILL/waitpid cleanup. `test_ipc_daemon` starts the product
`photospiderd` for embedded-Host behavior and also starts the non-installed
`ipc_output_fixture_daemon` as a separate process for deterministic image and
bounded-observation outcomes. The fixture still uses the production internal
Server/router/OutputStore/Unix-socket/worker stack; it is only a build
dependency of the test, has no independent CTest entry, and does not change
product startup or seed plugins early. A fixture-only protected fixed-width
control file supplies a cross-process manual monotonic clock, while the private
internal Server composition overload receives small versions of the existing
snapshot, compute-registry, and OutputStore policies plus deterministic id
sources. These controls are not `photospiderd` flags, environment failpoints,
or wire methods. These are long-lived product-behavior tests; they create no
retained `tests/results`, replay, provenance, or migration gate.
