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

`protocol_version` is an integer. `id` is a nonempty UTF-8 string no longer
than 128 bytes. `method` is a nonempty string and `params` is an object.
Unknown top-level fields are ignored for forward compatibility. Request ids
correlate one response; they are not idempotency keys.

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
publishing an owned value. Daemon/session opaque ids must be exactly 32
lowercase hexadecimal characters; `graph.load` must echo the requested display
session name. It never automatically retries or reconnects,
especially for `graph.load`. `disconnect()` is idempotent and never closes a
daemon-owned graph.

Every integer is decoded according to its signed or unsigned JSON storage and
must fit the exact public destination type before a value is published. Frame,
JSON, envelope, correlation, and error-object violations close the connection
because message synchronization is no longer trustworthy. Once a correlated
result object has been consumed, a typed result-shape or range failure returns a
local Protocol status while leaving the synchronized connection open.

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

| Domain | Code | Name |
| --- | ---: | --- |
| protocol | -32700 | `parse_error` |
| protocol | -32600 | `invalid_request` |
| protocol | -32601 | `method_not_found` |
| protocol | -32602 | `invalid_params` |
| daemon | -32603 | `internal_error` |
| protocol | -32001 | `unsupported_protocol` |
| protocol | -32002 | `response_too_large` |

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
remain available in `OperationStatus`. Allocation failure may propagate
`std::bad_alloc`; other recoverable failures are returned as statuses.

## Daemon Metadata

At process start, the daemon generates one 128-bit lowercase hexadecimal
`server_instance_id` from operating-system entropy. `daemon.ping` takes `{}`
and returns:

```json
{"pong":true,"server_instance_id":"32-lowercase-hex"}
```

`daemon.version` takes `{}` and returns protocol version 1, service name
`photospiderd`, the CMake project version, the same instance id, transport
`unix`, and the sorted exact eight-method list. These metadata calls do not
acquire the Host mutex.

## Opaque Graph Sessions

`graph.load` requires a safe `session_name` and absolute `root_dir`. The name is
one nonempty path component other than `.` or `..`; slash, backslash, and NUL
are forbidden. Optional `yaml_path`, `config_path`, and `cache_root_dir` must be
absolute when nonempty; absence maps to empty Host strings.

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

`graph.close` resolves only the opaque id. Successful Host close removes all
three active indexes. Host `NotFound` also removes the stale mapping while preserving the
failure response; other Host failures retain it. Disconnecting a client does
not close sessions. Shutdown attempts to close every active Host session after
joining connection workers.

## Inspection Values

Inspection resolves the opaque id and calls only `ps::Host`:

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
reads and writes never hold it. Shutdown closes the listener, shuts down
tracked client descriptors to wake reads, joins all workers, closes sessions,
removes its socket while the lifecycle lock remains held, releases the lock,
and then destroys Host state. The persistent lock file intentionally remains.

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
  test_ipc_daemon public_header_self_containment -j
ctest --test-dir build --output-on-failure \
  -R '^(FrameCodec|ProtocolEnvelope|IntegerCodec|ProtocolErrors|ProtocolParams|ProtocolGraphLoad|InspectionJson|SessionRegistry|ClientLifecycle|ClientResultValidation|IpcDaemon|StaticProductConsumerSmoke|IpcDisabledInstallSmoke|PublicHeaderSelfContainment)'
```

`StaticProductConsumerSmoke` verifies the installed backend plus a second
client-only consumer. `IpcDisabledInstallSmoke` verifies an IPC-disabled clean
install has no IPC forwarder, header, target, archive, or daemon while the
embedded consumer remains usable. Real-process tests have CTest timeouts and
bounded SIGTERM/SIGKILL/waitpid cleanup. These are long-lived product tests;
they create no retained `tests/results`, replay, provenance, or migration gate.
