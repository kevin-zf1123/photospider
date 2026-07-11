# Photospider 本地 IPC 协议版本 1

本文是首个本地 `photospiderd` 协议切片的权威维护契约。OpenSpec change 记录引入原因；
本文定义长期产品行为。

## 产品与范围

当 `CMAKE_SYSTEM_NAME` 精确为 `Darwin` 或 `Linux` 时，
`PHOTOSPIDER_BUILD_IPC` 默认 `ON`，并构建：

- `photospider_ipc_client`：已安装 static library，导出为
  `Photospider::photospider_ipc_client`；
- `photospider_ipc_server_internal`：不安装的 server/router library；
- `photospiderd`：已安装的 foreground executable。

Public client header 是 `photospider/ipc/protocol.hpp` 与
`photospider/ipc/client.hpp`。它们只暴露 typed owned value，不暴露 JSON type、socket
descriptor、`sockaddr_un`、backend model、runtime service 或 mutable backend ownership。
Client library 不链接 `photospider` backend。IPC disabled 时，不安装该 target 与 public
header；在其他任何 CMake system name 下强制开启 IPC 会使 configure 明确失败。

版本 1 精确实现八个方法：

1. `daemon.ping`
2. `daemon.version`
3. `graph.load`
4. `graph.close`
5. `graph.list`
6. `inspect.graph`
7. `inspect.node`
8. `inspect.dependency_tree`

它不实现 compute、polling、cancellation、image、scheduler、plugin、event、dirty
inspection、graph reload/save、`daemon.shutdown`、TCP、Windows named pipe 或
`graph_cli --connect`；这些仍属于后续工作。现有 `graph_cli` 继续创建 embedded Host，
本地命令语义不变。

## Transport 与 Frame

Daemon 只监听 Unix domain stream socket。每个 frame 为：

```text
network byte order 的 uint32 payload_size
payload_size 字节 UTF-8 JSON object text
```

`payload_size` 合法闭区间为 `1..16,777,216`。实现会在分配 payload 前验证四字节
header，循环处理 partial read/write，并重试 `EINTR`。任何 header byte 到达前的 clean
EOF 会正常结束该 client；header/body 中途 EOF、zero length 或 oversized length 只关闭该
connection，不读取不可信 body，也不伪造无关联 response。Linux send 使用 `MSG_NOSIGNAL`，
macOS 使用 socket-local `SO_NOSIGPIPE`，不会修改 process-global SIGPIPE policy。

JSON object key 在 escape decoding 后必须唯一。Duplicate key 属 invalid request，绝不采用
last-value-wins。

## 关联 Envelope

合法 request：

```json
{
  "protocol_version": 1,
  "id": "client-chosen-id",
  "method": "daemon.ping",
  "params": {}
}
```

`protocol_version` 必须是精确 integer；`id` 与 `method` 都必须是非空 UTF-8 string，且各自
最多 128 bytes；`params` 必须是 object。Byte 限制在 JSON escape 解码后计算。完成解析后，
为保持 forward compatibility，任一 typed object 的未知 member 均会忽略；缺失、类型错误、
non-finite、溢出或超限的已知 request member 会在 session 解析或 Host access 前被拒绝。
Request id 只关联一次 response，不是 idempotency key。

成功 response 只含一个 result branch：

```json
{"protocol_version":1,"id":"client-chosen-id","result":{}}
```

失败 response 只含一个 error branch：

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

Invalid JSON 或无法恢复有效 id 的 envelope 使用 `"id": null`。不支持的 integer version
返回 `protocol/-32001/unsupported_protocol`，并包含 `supported_versions: [1]`。超过
frame 上限的 response 会替换成同 id 的 bounded `response_too_large` error，绝不写 truncated
JSON。

Move-only typed `ps::ipc::Client` 每次只允许一个 outstanding call，不能被并发调用；独立
client 可以并发使用。它会在发布 owned value 前验证 frame size、JSON uniqueness、response
version、shape 与 id。每个 daemon-generated opaque id 都遵守下文统一的
32-lowercase-hexadecimal 表示；`graph.load` 必须回显请求的 display session name。它绝不
自动 retry/reconnect，尤其不会重试 `graph.load`。
`disconnect()` 是 idempotent，且不会关闭 daemon-owned graph。

每个 integer 都会按其 signed/unsigned JSON storage 解码；只有能精确放入 public 目标类型时才会
发布 value。Frame、JSON、envelope、correlation 或 error-object violation 会关闭 connection，
因为此时已不能信任消息同步。消费完关联正确的 result object 后，typed result shape/range failure
会返回本地 Protocol status，同时保持已同步的 connection 打开。

## Typed Value 与成帧前边界

所有 integer field 都按其 signed 或 unsigned JSON integer storage 解码，并且必须精确落入目标
宽度。负 unsigned、fractional number、non-finite number 与 overflow 均会被拒绝；value 绝不
通过 floating point 中转，也不会 wrap、truncate 或 clamp。

Enum 只使用精确 lowercase snake-case string；known enum field 不接受 integer spelling、大小写
折叠或未知 value。Pixel ROI 是包含精确 integer `x`、`y`、`width`、`height` member 的 object，
每个 member 都必须能由 public `int`-backed `PixelRect` 表示。Composite value codec 会先验证到
临时 owned value，失败时不发布任何部分结果。

Opaque server-instance、session、compute、output、delivery 与 cursor identifier 都精确使用
32 个 lowercase hexadecimal character。它们是 semantic token，绝不是 path、pointer、display
name 或 backend handle。Request 中 malformed opaque id 属 invalid input；daemon result 中
malformed opaque id 属本地 Protocol result-shape failure。

| Value | 上限 |
| --- | ---: |
| Request id 或 method name | 各 128 UTF-8 bytes |
| Session display name、scheduler type、precision、operation/plugin key、event name/source | 各 1,024 UTF-8 bytes |
| Filesystem path 或 scheduler/plugin description | 各 4,096 UTF-8 bytes |
| Diagnostic message | bounded truncation 后 4,096 UTF-8 bytes |
| Node YAML text 或一个 copied parameter/source string | 各 8 MiB（8,388,608 UTF-8 bytes） |
| Directory/path input array | 256 entries |
| General wire page | 4,096 entries |

String 上限会在 JSON decoding 后对每个已知 string member 分别应用。已知入站 array/page
count 会在 session 解析或 Host access 前检查；未知 member 只能在 JSON parser 将其实体化后
再忽略。Host 返回的 string、array、page 与 composite row 会在发布 cursor 或构造 response
value 前完成校验。因此，parser 与临时 container 的 allocation 可能先于 typed field validation。
Page limit 必须是大于零、且不超过 schema-specific maximum 的精确 integer；zero、negative、
fractional 或会溢出的 offset/limit 均非法。16 MiB frame bound 仍然具有最终约束力，因此
component 合法但不可拆分的 response 仍可能成为 `response_too_large`。

## 稳定 Error

`OperationStatus` 是 embedded Host 与 IPC product 共用的唯一 public status 表示，
`OperationErrorDomain` 是其稳定的顶层分类。Canonical success 为 `ok=true`、
`domain=None`、`code=0` 且 `name`/`message` 为空。Error 来源为：

| Domain | 来源 |
| --- | --- |
| Transport | 始终由 client 在本地产生 |
| Protocol | 由 client 本地验证产生，或从 daemon response 解码 |
| Graph | 从 daemon response 解码 |
| Daemon | 从 daemon response 解码 |

每个 remote Protocol、Graph 或 Daemon failure 都有 string `domain`、signed integer `code`、
string `name` 与 diagnostic `message`。其 `code`/`name` mapping 在版本 1 内稳定。本地产生的
Protocol status 使用同一套版本 1 validation category；message 不是 branching contract。

Envelope error 包含 `domain`、`code`、`name`、`message`，且绝不表示 success。Nested
`OperationStatus` value 还包含 `ok`。其唯一 canonical success 是
`{ok:true, domain:"none", code:0, name:"", message:""}`；failure 使用 `ok:false`、非 `none`
domain、一个 signed code、stable name 与 bounded diagnostic。顶层 envelope error 与 nested
operation outcome 是不同 shape，但共享相同的 domain/code/name mapping。未知 object member
保持 forward-compatible，malformed known member 则拒绝整个 value。

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

Host failure 使用 graph domain 与显式 `GraphErrc` mapping：

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

本地 connect/read/write/peer failure 使用 `OperationErrorDomain::Transport`，绝不转换成
graph IO error；daemon 不能发送 transport domain。Transport domain 是可编程稳定区分的分类，
但其本地 numeric code/name 只是诊断类别；client 不得依赖或持久化某个长期 Transport
code/name mapping。未知的未来 remote numeric code/name 仍会保留在 `OperationStatus` 中。
每个当前已知 wire code 都必须使用其 canonical version 1 name；known code/name mismatch 属
malformed；当前已知 name 同样只能与其 canonical code 配对。只有 numeric code 与非空 name
都位于当前 table 之外时，才会将两者一起保留以保持 forward compatibility，而不会被折叠或
猜测。Future Graph value 不会被强制转换成 `GraphErrc`；只有显式的当前 1..9 mapping 能通过
checked conversion。Wire diagnostic 连同任何 truncation marker 在内必须保持合法 UTF-8，且
最多 4,096 bytes。Allocation failure 可以传播 `std::bad_alloc`；
其他 recoverable failure 均返回 status。Client 绝不根据 `message` 分支。

## Daemon Metadata

Process 启动时，daemon 从 operating-system entropy 生成一个 128-bit lowercase hex
`server_instance_id`。`daemon.ping` 接受当前没有 known member 的 object，忽略 unknown
member，并返回：

```json
{"pong":true,"server_instance_id":"32-lowercase-hex"}
```

`daemon.version` 应用相同的 params 规则，返回 protocol version 1、service name
`photospiderd`、CMake project version、同一 instance id、transport `unix` 与精确排序后的
八方法列表。Metadata call 不获取 Host mutex。

## Opaque Graph Session

`graph.load` 要求不超过 1,024 UTF-8 bytes 的 safe `session_name`，以及不超过 4,096 UTF-8
bytes 的 absolute `root_dir`。Name 必须是一个非空 path component，不能是 `.`/`..`，也不能
包含 slash、backslash 或 NUL。可选 `yaml_path`、`config_path`、`cache_root_dir` 在非空时必须
absolute，且各自不超过 4,096 UTF-8 bytes；缺省会映射为空 Host string。超限 value 会在
reservation、session resolution 或 Host access 前返回 `invalid_params`。

Daemon 将 caller name 原样传给 `GraphLoadRequest.session`，因此既有 `<root>/<session>` 与
cache path 语义不变；另行 reserve 一个 collision-checked 128-bit opaque id。Mutex-protected
registry 会追踪 loading reservation，以及 active opaque、精确 Host-id 与 display-name index：

```text
opaque session_id <-> Host 返回的精确 GraphSessionId
```

Host load failure 或异常会删除 reservation；registry publication failure 会先删除 reservation，
再尝试关闭刚加载的 Host session。成功只暴露：

```json
{"session_id":"32-lowercase-hex","session_name":"caller-name"}
```

`graph.list` 调用 `Host::list_graphs()` 并与 committed mapping reconcile。Disagreement 属
daemon invariant error，不泄漏 untracked Host name。返回 row 按 `session_name`、再按
`session_id` 排序。

`graph.close` 只通过 session lifecycle gate 解析 opaque id。它会先把 mapping 标记为 closing，
让所有新的 session-scoped Host 或 compute admission 返回 Graph `not_found`，并且在不持有 Host
mutex 的情况下等待已经 admitted 的 Host call 与 queued/running job。随后它才获取 Host mutex 并
调用 `Host::close_graph`。Host close 成功会删除三个 active index；Host `NotFound` 也会删除 stale
mapping，但保留 failure response；其他 Host failure 会原子地重新开放 mapping。Closing row 不会
出现在 `graph.list` 结果中，但仍参与 Host/registry invariant reconciliation。Client disconnect 不会
关闭 session。

## Private Compute Lifecycle

当前八方法 wire inventory 仍不暴露 compute method。不过 server 已经拥有 compute routing 使用的
private lifecycle boundary：一个显式启动且 joinable 的 `ComputeRequestRegistry` worker，以及一套
共享 session-admission gate。这些基础设施不是第二套 public Host 或 wire API。

Queue commit 之前，submission 会 admit active session，执行全局最多 64 个 queued/running record
的限制，验证并 collision-check 一个 daemon-generated 32-lowercase-hex compute id，并预留全部
queue/terminal bookkeeping。成功 commit 的 snapshot 始终是 `queued` 且
`cancellable: false`。唯一 FIFO worker 会把 record 推进到 `running`，按 `status` 或 `image`
mode 精确调用一次匹配的 synchronous Host callback，然后发布恰好一个 immutable
`succeeded` 或 `failed` terminal status。Host call 使用同一个 daemon Host mutex；image
publication 在 callback 释放 Host mutex 后执行。

Status read 是 non-destructive，且不获取 Host mutex。Result 与 release 只接受 terminal record。
格式正确但 absent、expired、released 或 evicted 的 id 映射为 daemon `job_not_found`；对
queued/running job 执行 result 或 release 则映射为 daemon `job_not_ready`。Host failure 保留为精确
nested terminal status。Queue commit 之后的任何异常，包括 allocation 或 output publication
failure，都会使用 commit 前已分配的 daemon `internal_error` fallback status，因此 worker 可以继续
执行后续 job。

最多保留 256 个 terminal record。发布新的 terminal record 时只 evict 最早的 terminal
publication；active work 永不 evict。Terminal age 从完整 publication 开始，使用 injectable
monotonic clock，在 15 分钟时 expire，并且 lookup 不刷新 age。Output ownership 是 private
move-only reference，其 exact-once cleanup 在 registry mutex 外执行；wire-visible protected output
store 不属于当前 method inventory。

## Inspection Value

Inspection 会在获取 Host mutex 前原子地 admit opaque session，并且只调用 `ps::Host`。并发 close
会先标记 session，拒绝新的 inspection admission，并等待已经 admitted 的 inspection：

- `inspect.graph` 返回 `{session_id, nodes}`；
- `inspect.node` 要求能由 public `int`-backed `NodeId` 表示的 nonnegative integer
  `node_id`，返回 `{session_id, node}`；
- `inspect.dependency_tree` 接受相同范围的 optional nonnegative integer/null `node_id` 与
  optional boolean `include_metadata`，然后返回
  `{session_id, ...flattened tree fields}`。

Node JSON 以 snake-case field 镜像 `NodeInspectionView`：integer `id`、`name`、
`type`、`subtype`、由字符串值组成的 JSON object `parameters`、
`has_cached_output`，以及始终出现且 nullable 的 `source_label`、`debug`、
`space`。Spatial matrix 有九项。Dependency edge 包含 integer
`from_node_id`/`to_node_id`、`kind`（`image_input` 或 `parameter_input`）、
output/input name 与 nonnegative `input_index`。Optional value 使用 JSON null；
非 finite public double 编码为 null，typed client 恢复为 quiet NaN。

Typed client 会在发布 graph、node 或 dependency-tree snapshot 前，对每个 node id、tree depth、
edge input index、debug timestamp/duration、worker id 与 spatial extent/rectangle component
执行范围检查。Signed/unsigned overflow 属本地 Protocol result-shape failure，绝不窄化。
同一组 reusable enum、`PixelRect`、bounded-string、array、page、opaque-id 与 nested-status codec
会递归应用于 composite value；decode 失败时 caller-visible destination 保持未发布。

Payload 中不会出现 backend class、address、pointer、cache handle、service object、closure 或
mutable reference。

## Socket 与 Process Lifecycle

`photospiderd` 保持 foreground。`--socket ABSOLUTE_PATH` 选择 explicit socket；否则优先使用
合法 uid-owned protected `XDG_RUNTIME_DIR`。若最终 path 无法放入 `sun_path`，则使用
`/tmp/photospider-<uid>/photospiderd-v1.sock`。Daemon-created direct runtime directory 为
`0700`，socket 与持久 `${socket}.lock` regular file 均为 `0600`。

检查或回收 socket path 前，daemon 使用 `O_NOFOLLOW|O_CLOEXEC` 打开持久 lock，验证
regular-file identity、owner、single link 与精确 `0600` mode，然后获取
`flock(LOCK_EX|LOCK_NB)`。该 lock 会一直持有到 socket cleanup 完成且永不 unlink，因此并发
starter 会在同一个稳定 inode 上串行，process death 也会自动释放 ownership。持锁期间 daemon
使用 `lstat` 拒绝 symlink、non-socket 与其他 uid 所有的 path。Bounded nonblocking probe 会保留
live same-owner listener；只有 connection refused 的 same-owner socket 才会在按
device/inode/owner 重新验证后作为 stale 删除。Cleanup 记录 created device/inode/owner，在释放
lifecycle lock 前只 unlink 精确匹配的 socket。

Listener 最多跟踪 32 个 joinable client worker。每个 connection 内 request 顺序执行，不同
client 的 frame/JSON 工作可以并发。Public Host 不承诺 thread-safe，因此每个 Host call 都使用
一个 daemon mutex；socket read/write 绝不持有它。Shutdown 会先停止所有 session 与 compute
admission，关闭 listener，shutdown tracked client descriptor 以唤醒 read，并 join 全部 connection
worker。随后它会 drain 所有 accepted compute job，join 唯一 compute worker，释放 terminal output
ownership，关闭 Host session，并清空 session mapping。最后在 lifecycle lock 仍持有时移除 socket，
释放 lock，再销毁 Host state；持久 lock file 会有意保留。

安装 SIGINT/SIGTERM handler 前，`photospiderd` 会创建 nonblocking close-on-exec self-pipe。
Handler 只保存 `errno`、写一个 byte，再恢复 `errno`；正常 control flow 执行 cleanup。正常 signal
shutdown 以 zero 退出；option/startup failure 输出 diagnostic 并 nonzero 退出。协议没有 shutdown
method，进程也不会 fork daemonize。

## 验证

Focused local command：

```bash
cmake -S . -B build -DPHOTOSPIDER_BUILD_IPC=ON -DBUILD_TESTING=ON
cmake --build build --target photospider_ipc_client \
  photospider_ipc_server_internal photospiderd test_ipc_protocol \
  test_compute_request_registry test_ipc_daemon \
  public_header_self_containment -j
ctest --test-dir build --output-on-failure \
  -R '^(FrameCodec|ProtocolEnvelope|IntegerCodec|ProtocolErrors|ProtocolParams|ProtocolGraphLoad|ProtocolGraphClose|InspectionJson|SessionRegistry|ComputeRequestRegistry|ClientLifecycle|ClientResultValidation|IpcDaemon|StaticProductConsumerSmoke|IpcDisabledInstallSmoke|PublicHeaderSelfContainment)'
```

`StaticProductConsumerSmoke` 验证 installed backend 与第二个 client-only consumer；
`IpcDisabledInstallSmoke` 验证 IPC-disabled clean install 不含 IPC forwarder、header、target、archive
或 daemon，同时 embedded consumer 仍可用。Real-process test 有 CTest timeout 与 bounded
SIGTERM/SIGKILL/waitpid cleanup。这些都是长期 product test；不会创建 retained `tests/results`、
replay、provenance 或 migration gate。
