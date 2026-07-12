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

Version 1 implements exactly eight methods:

1. `daemon.ping`
2. `daemon.version`
3. `graph.load`
4. `graph.close`
5. `graph.list`
6. `inspect.graph`
7. `inspect.node`
8. `inspect.dependency_tree`

It does not implement compute, polling, cancellation, images, scheduler,
plugins, events, dirty inspection, graph reload/save, `daemon.shutdown`, TCP,
Windows named pipes, or `graph_cli --connect`. Those remain later work. The
existing `graph_cli` continues to create an embedded Host and keeps its local
command semantics.

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
transport `unix`, and the sorted exact eight-method list. These metadata calls
do not acquire the Host mutex.

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

## Private Compute Lifecycle

The current eight-method wire inventory still exposes no compute method. The
server nevertheless owns the private lifecycle boundary used by compute
routing: one explicitly started, joinable `ComputeRequestRegistry` worker and
one shared session-admission gate. This infrastructure is not a second public
Host or wire API.

Before a queue commit, submission admits the active session, enforces a global
maximum of 64 queued/running records, validates and collision-checks a
daemon-generated 32-lowercase-hex compute id, and reserves all queue/terminal
bookkeeping. A successful commit snapshot is always `queued` with
`cancellable: false`. The sole FIFO worker advances a record to `running`,
invokes exactly one matching synchronous Host callback for `status` or `image`
mode, and publishes exactly one immutable `succeeded` or `failed` terminal
status. Host calls use the same daemon Host mutex; image publication occurs
after that callback releases the Host mutex.

Status reads are non-destructive and do not acquire the Host mutex. Result and
release accept terminal records only. A well-formed absent, expired, released,
or evicted id maps to daemon `job_not_found`; a queued/running result or release
maps to daemon `job_not_ready`. Host failures remain exact nested terminal
statuses. Any exception after queue commit, including allocation or output
publication failure, uses a fallback daemon `internal_error` status allocated
before commit, so the worker continues to later jobs.

At most 256 terminal records are retained. Publishing another terminal record
evicts only the oldest terminal publication; active work is never evicted.
Terminal age starts at complete publication, uses a monotonic injected clock,
expires at 15 minutes, and is not refreshed by lookup. Output ownership is a
private move-only reference with optional lease-aware exact-once cleanup outside
the registry mutex. It is backed by the private protected store described below;
neither its result metadata nor its release arguments are part of the current
method inventory.

## Private Protected Output Store

The current eight-method wire inventory exposes no image result or delivery
operation. Behind the router, one private `OutputStore` is nevertheless bound
to the joined compute publisher and socket lifecycle. It starts after the Unix
socket and persistent lifecycle lock are owned but before session, compute, or
client admission. A startup failure therefore admits no request.

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
mismatched record/file returns daemon `artifact_not_found` from this private
result boundary while leaving the terminal compute record releasable; an unsafe
replacement is never followed or removed.

Each published artifact owns one stable 32-hex delivery id. Successful private
result access validates the artifact and, in the same store critical section,
creates, reuses, or reactivates its sole lease with expiry set to that
operation's monotonic `now + 60 seconds`. Repeated access returns the same id
and refreshes that deadline. Compute release accepts an internal optional
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
  returns `{session_id, ...flattened tree fields}`.

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
The same reusable enum, `PixelRect`, bounded-string, array, page, opaque-id,
and nested-status codecs apply recursively to composite values. A failed decode
leaves the caller-visible destination unpublished.

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
reads and writes never hold it. Shutdown first stops all session and compute
admission plus new output leases, closes the listener, shuts down tracked client
descriptors to wake reads, and joins all connection workers. It then drains
every accepted compute job, joins the sole compute worker, releases terminal
job ownership, waits active delivery leases through explicit release or TTL,
and closes the OutputStore before Host sessions and session mappings. Finally
it removes its socket while the lifecycle lock remains held, releases the lock,
and destroys Host state. The persistent lock file intentionally remains.

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
  test_compute_request_registry test_output_store test_ipc_daemon \
  public_header_self_containment -j
ctest --test-dir build --output-on-failure \
  -R '^(FrameCodec|ProtocolEnvelope|IntegerCodec|ProtocolErrors|ProtocolParams|ProtocolGraphLoad|ProtocolGraphClose|InspectionJson|SessionRegistry|ComputeRequestRegistry|OutputStore|ClientLifecycle|ClientResultValidation|IpcDaemon|StaticProductConsumerSmoke|IpcDisabledInstallSmoke|PublicHeaderSelfContainment)'
```

`StaticProductConsumerSmoke` verifies the installed backend plus a second
client-only consumer. `IpcDisabledInstallSmoke` verifies an IPC-disabled clean
install has no IPC forwarder, header, target, archive, or daemon while the
embedded consumer remains usable. Real-process tests have CTest timeouts and
bounded SIGTERM/SIGKILL/waitpid cleanup. These are long-lived product tests;
they create no retained `tests/results`, replay, provenance, or migration gate.
