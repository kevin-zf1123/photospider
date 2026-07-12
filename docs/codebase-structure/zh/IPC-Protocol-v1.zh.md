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

`daemon.version.methods` 来自独立于 route dispatch matcher 的统一 advertisement table，
并报告以下精确八名称 metadata 子集：

1. `daemon.ping`
2. `daemon.version`
3. `graph.load`
4. `graph.close`
5. `graph.list`
6. `inspect.graph`
7. `inspect.node`
8. `inspect.dependency_tree`

Request router 还接受以下 39 个额外的 daemon-routed typed method：

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
39. `scheduler.trace`

已安装 typed `ps::ipc::Client` 只暴露八名称 metadata 子集的 call，且没有 public raw-JSON
escape hatch。新增 39 个 schema 属 daemon request-router behavior。其中
`compute.status`、`compute.result` 与 `compute.release` 只操作 daemon job registry；
`compute.submit` 会接纳一个 registry job，随后由其 worker 恰好执行一次匹配的 Host compute
call。另外 35 个 method 会让 direct request 或 first-page request 经由匹配的 Host operation；
stable collection continuation 只读取其 frozen snapshot registry record。

版本 1 现在会在 router boundary 暴露 daemon-owned compute-job submit、polling、terminal
result、release 与 protected metadata-only image delivery。Image byte 会保留在 private
artifact 中；terminal nonempty image result 只会在一个 stable delivery lease 下返回规定的
artifact metadata。版本 1 不暴露 compute cancellation、scheduler control、
`daemon.shutdown`、TCP、Windows named pipe 或 `graph_cli --connect`。Process-global
operation-plugin control 与有序 view 已在 daemon router boundary 可用。Installed typed
Client 当前不暴露这些 plugin method。Bounded `events.drain` 与 `scheduler.trace`
observation route 只在 daemon router boundary 可用。现有 `graph_cli` 继续创建 embedded
Host，本地命令语义不变。

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
| 单个 stable collection snapshot | 262,144 entries 且 64 MiB |
| Active collection snapshot | 64 records 且总计 256 MiB |
| Compute-event ring | 每个 session 8,192 entries |
| Scheduler-trace ring | 每个 session 65,536 entries |

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
`photospiderd`、CMake project version、同一 instance id、transport `unix`，以及“产品与范围”
中列出的精确排序八名称 metadata 子集。Metadata call 不获取 Host mutex。

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

## Host-Routed Graph State 与 Diagnostic

本节每个 method 都会在解析 opaque `session_id` 前验证全部 known parameter。Malformed known
field 会在 Host access 前返回 `protocol/-32602/invalid_params`；格式正确但 unknown 或 closing
的 session 返回 Graph `not_found`。Unknown object member 会被忽略。Admission 成功后，daemon
只会在公共 Host mutex 下调用恰好一次匹配的 `ps::Host` method，并在 response encoding 前释放
该 mutex。任何 mutation 都不会自动 retry，包括 Host mutation 已完成但其 copied result 无法编码
的情形。

以下 status-only method 的唯一 success value 是 `result:{}`：

- `graph.reload` 与 `graph.save` 要求 `session_id`，以及非空、absolute、无 NUL、最多
  4,096 UTF-8 bytes 的 `yaml_path`；
- `graph.clear` 只要求 `session_id`，清空 graph model state 后仍保留 active opaque session
  mapping；
- `graph.node_yaml.set` 要求 `session_id`、非负且精确落入 `int` 的 `node_id`，以及最多
  8 MiB 的 string `yaml_text`；
- `cache.clear_all`、`cache.clear_drive`、`cache.clear_memory` 与
  `cache.free_transient` 只要求 `session_id`；
- `cache.cache_all_nodes` 与 `cache.synchronize_disk` 还要求最多 1,024 UTF-8 bytes 的
  string `precision`。

空 `yaml_text` 与 `precision` 是合法 protocol value；其语义合法性属于匹配的 Host method，
router 会返回 Host 的精确 Graph failure。Path 的 absolute、非空、NUL-free shape 属 wire
schema，因此由 protocol 验证。

`graph.node_yaml.get` 要求 `session_id` 与非负精确 `node_id`。成功返回：

```json
{
  "session_id": "32-lowercase-hex",
  "node_id": 7,
  "yaml_text": "complete Host-returned YAML"
}
```

返回 YAML 必须是合法 UTF-8 且最多 8 MiB，绝不 truncate。`inspect.node_ids` 与
`inspect.ending_nodes` 只要求 `session_id`，分别返回 `{session_id,node_ids}` 与
`{session_id,ending_node_ids}`。这些 array 使用下文的 stable collection-page metadata；每页最多
4,096 个非负精确 `int` value，admitted snapshot 最多 262,144 个。Host order 与 duplicate id
都会保留，router 不排序、不去重，也不截断。

`inspect.roi_forward` 要求 `start_node_id`、`start_roi`、`target_node_id`；
`inspect.roi_backward` 要求 `target_node_id`、`target_roi`、`source_node_id`。Node id 是非负
精确 `int`；每个 ROI 都含精确 `int` 的 `x`、`y`、`width`、`height` member。非正 width 或
height 仍可表示。成功返回 `{session_id,roi}`，并原样保留 Host rectangle，不交换 node 或 axis。

`dirty.begin` 与 `dirty.update` 要求 `session_id`、非负精确 `node_id`、`domain`、
`source_roi`；`dirty.end` 要求除 `source_roi` 外的相同字段。`domain` 只能精确为
`high_precision` 或 `real_time`。成功返回以下 copied dirty snapshot：

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

每个顶层 dirty array 与每个 nested ROI array 最多 4,096 entries。Vector order 保留；
`actual_dirty_rois` 使用 ascending integer map-key order。Dirty lifecycle label 是 `idle`、
`updating`、`settled`；edge direction 是 `forward_affected` 与 `backward_demand`。

`compute.timing` 只要求 `session_id`，返回 `{session_id,node_timings,total_ms}`。每个 ordered
timing row 包含 `node_id`、`name`、`elapsed_ms`、`source`，最多返回 4,096 rows；name 最多
1,024 UTF-8 bytes，source 最多 8 MiB。`compute.last_io_time` 返回
`{session_id,last_io_time_ms}`。Non-finite timing 或 IO double 编码为 JSON null。

单次 Host call 返回且 Host mutex 释放后，timing 与 dirty codec 会先验证每个 component bound，
再在分配 result JSON array/object 前，对完整 success response 执行一次 overflow-safe compact-byte
budget preflight。该 budget 包含实际 request/session id、固定 envelope/schema byte、精确 JSON
string escape 长度、精确 integer decimal width，以及精确 boolean/null width。Finite timing double
只贡献一字节下界，而不是 worst-case width；每次累加都会在算术前检查 remaining capacity。若已能
证明总大小超过 16 MiB，则抛出 `std::length_error`，由 router 映射为同一 id 的 protocol
`response_too_large`；若 near-boundary 下界仍可能容纳，则交给最终 serializer 判定，因此 preflight
不会拒绝实际可放入 frame 的 response。

`compute.last_error` 在成功 response 中返回 `{session_id,status}`。Nested value 使用
canonical `OperationStatus` schema。被观察到的 Host failure 是该 method 的 diagnostic data，
不会转换成顶层 request failure；unknown 或 closing session 仍会在 Host access 前于顶层失败。

其他 method 的 Host failed result 会成为精确顶层 Graph error。Host 返回的 YAML、row 或
collection 超出 byte/count limit 时，会在单次 Host call 后成为 `response_too_large`。Host 返回
negative node id、unknown enum 或 invalid UTF-8 时成为 daemon `internal_error`。这些 failure
不发布 partial result，也不会触发第二次 Host invocation。Resource exhaustion 保持 exceptional，
不会重写成 status。

## 有界 Event 与 Scheduler Trace Observation

`events.drain` 要求 opaque `session_id`，以及 `1..1024` 内的精确 integer `limit`。它会在公共
Host mutex 下恰好调用一次 `Host::drain_compute_events`。Result 为：

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

Host event API 是 destructive 的，并在 removal 前应用 limit。Successful call 只移除返回的最旧
page，并且会原子返回并清零 shared per-session drop count，空 page 也遵守这一规则。因此，独立
client 共享一个 drain position 与一个 drop counter；invalid call 不改变两者。`has_more` 是在
ring lock 下观察到的 post-removal state。`next_sequence` 是最后 returned event 加一；
pre-exhaustion 空 page 使用 next publication sequence；exhaustion 后没有 retained later event
时使用 `UINT64_MAX`。

Event name 或 source text 进入 retained storage 前，每个 value 都必须是 canonical UTF-8，且最多
1,024 bytes。Invalid 或 oversized text 会 whole-drop publication，不 truncate 或 repair。仍有
valid sequence 时，该尝试会消费 sequence，并让 shared drop count 恰好递增一次。Full ring 同样
会消费 sequence、精确 evict oldest event、retain newest event，并递增一次。Exhaustion 后只有
exhaustion path 递增一次。所有递增都在 `UINT64_MAX` 饱和。

`scheduler.trace` 要求 `session_id`、精确 unsigned `after_sequence`，以及 `1..4096` 内的精确
integer `limit`。Zero 从 oldest retained trace 开始；router 会恰好调用一次
`Host::scheduler_trace`，并返回：

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

Trace page 是 non-destructive 的，并且只包含 sequence 大于 exclusive cursor 的 event。Nonempty
pre-terminal page 返回其最后 sequence；pre-exhaustion 空 page 保留 input cursor。只有观察到最后
valid sequence `UINT64_MAX-1` 且没有 later entry 的 page，或 exhausted empty observation，才会
返回 `UINT64_MAX` sentinel 与 `has_more:false`。`dropped_count` 是 cursor 之后 missing retained
history 与 exhaustion 后 unsequenced attempt 的精确饱和和。因此，重复 cursor 可以重现同一个
retained page；通过 `next_sequence` 前进不会丢失或重复 retained event。

当某个具体 node 或 worker 适用时，每个 returned trace event 的 `node_id` 与 `worker_id` 都是
非负值。`-1` 是表示没有具体 node 或 worker 的唯一 sentinel；任何小于 `-1` 的值都是 malformed
Host output。Router 会在这一次 Host call 后把整个 response 拒绝为 daemon `internal_error`，且
绝不重试该 observation。

这两个 method 都没有 `max_entries` field。Unknown field 保持 forward-compatible，但不能替代
缺失或非法的 known `limit`。两个 route 都直接使用 Host 的 bounded observation API，绝不 reserve
stable collection snapshot，也不创建 daemon cursor。

## Compute Job Lifecycle

Request router 会通过 daemon-owned lifecycle 暴露 `compute.submit`、`compute.status`、
`compute.result` 与 `compute.release`。Installed direct Client 尚未封装这些 method，也不暴露
raw JSON call；对应 public typed API 属后续切片。Server 拥有一个显式启动且 joinable 的
`ComputeRequestRegistry` worker，以及一套共享 session-admission gate。这些基础设施仍是 typed
wire method 的 private implementation，而不是第二套 public Host。

`compute.submit` 要求 `session_id`、非负 `node_id`，以及值为 `status` 或 `image` 的
`result_mode`。它通过以下 nested value 接收现有 Host request：

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

三个 nested option object 及其 known member 都是 optional；缺失时保留 public
`HostComputeRequest` default。`intent` 与 `dirty_roi` 可以缺失，也可以是 JSON null。每个存在的
known value 都会在 session lookup 或 registry admission 前完成 type、range、UTF-8 与 byte-bound
验证；unknown member 会为了 forward compatibility 被忽略。

Queue commit 之前，submission 会 admit active session，执行全局最多 64 个 queued/running record
的限制，验证并 collision-check 一个 daemon-generated 32-lowercase-hex compute id，并预留全部
queue/terminal bookkeeping。成功 commit 的 snapshot 始终是 `queued` 且
`cancellable: false`。唯一 FIFO worker 会把 record 推进到 `running`，按 `status` 或 `image`
mode 精确调用一次匹配的 synchronous Host callback，然后发布恰好一个 immutable
`succeeded` 或 `failed` terminal status。Host call 使用同一个 daemon Host mutex；image
publication 在 callback 释放 Host mutex 后执行。

Submit、status 与 result 使用同一份 stable result schema：

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

Queued/running 时 `status` 为 null；terminal publication 后，它是 canonical、精确的 nested
`OperationStatus`。Submit、status、status-mode result、empty-image result 与 failed result 的
`output` 保持 null。只有 terminal successful nonempty image 的 `compute.result` 才会在
`output` 中返回以下精确 object：

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

该 path 是 protected absolute artifact path，不是 backend cache path 或 caller-selected output。
Registry 的 opaque reference 只会作为规范化 `output.output_id` 发布；JSON 中不会出现额外
`output_reference` field 或 backend handle。Terminal release 成功时使用独立、stable 的
acknowledgement：`{"compute_id":"...","released":true}`。`compute.release` 接受 optional
`delivery_id`；如果存在，它必须在任何 mutation 前符合精确 opaque-id shape。其他 unknown field
保持 forward-compatible。

Status read 是 non-destructive，且不获取 Host mutex。Result 与 release 只接受 terminal record，
release 会通过一次 registry operation 检查并删除 record。格式正确但 absent、expired、released
或 evicted 的 id 映射为 daemon `job_not_found`；对 queued/running job 执行 result 或 release 则
映射为 daemon `job_not_ready`。Host failure 保留为精确 nested terminal status。每个 accepted
Host failure、invalid image、output quota denial 或 publication failure 都会在 successful
status/result envelope 中保持为 immutable failed job；quota denial 使用 nested daemon
`artifact_limit_exceeded`，unexpected worker/publication failure 使用 nested daemon
`internal_error`。只有 malformed params、submit/admission failure、absent job、premature
result/release，以及 result-time missing 或 identity-mismatched artifact 属 top-level failure。
最后一种情况精确使用 daemon `artifact_not_found`；它不会改变 immutable terminal job，job 仍可
release。Queue commit 后的任何异常都会使用 commit 前已分配的 `internal_error` fallback，因此
worker 可继续执行后续 job。

最多保留 256 个 terminal record。发布新的 terminal record 时只 evict 最早的 terminal
publication；active work 永不 evict。Terminal age 从完整 publication 开始，使用 injectable
monotonic clock，在 15 分钟时 expire，并且 lookup 不刷新 age。Output ownership 是 private
move-only reference，其 optional lease-aware exact-once cleanup 在 registry mutex 外执行。它由
下文的 private protected store 支撑。Result 只把该 reference 作为经过重新验证的 delivery 中的
规范化 `output_id` 暴露，绝不增加额外 field 或 backend handle。没有 matching delivery 的
release 只删除 job ownership；带 matching id 的 release 会在同一个 OutputStore critical section
中删除 job ownership 与 active lease。

## Protected Output Store 与 Image Result

Wire surface 只从 terminal `compute.result` 暴露 image metadata，并使用 optional lease-aware
`compute.release`；不会新增独立 delivery method。Router 背后有一个 private `OutputStore`，并与
joined compute publisher 和 socket lifecycle 绑定。它会在 Unix socket 与持久 lifecycle lock
已归属之后、session/compute/client admission 之前启动；因此 startup failure 不会放入任何
request。

Canonical empty CPU image 不发布 artifact。Nonempty result 必须是合法 CPU `ImageBuffer`：具有
defined data type、正 dimensions/channels、non-null data、至少等于 checked tight row 的 source
step，并且 row/total byte count 不溢出。Store 只复制 tight row；source padding 与 backend
context 不会进入 artifact。Validation、allocation、write、rename 或 identity failure 会通过已接受
compute job 成为 nested daemon `internal_error`；quota denial 则成为 nested daemon
`artifact_limit_exceeded`。

Production limit 为最多 64 个 retained artifact、总计一 GiB retained byte、单个 artifact 512
MiB。Admission 同时计入 job-owned 与 lease-only artifact，并且具有 transactional 语义：失败的
publication 不保留 record、quota reservation 或可安全删除的 partial file。每个 controlled stage
与 final basename 都使用 `output-<32-lowercase-hex>.bin`，因此 restart cleanup 能识别 process
death 遗留的 stage。完整数据通过禁止覆盖的 atomic rename 发布，并在 record 可见前重新验证。

Store base 是 socket-specific、same-owner、exact-mode `0700` 的 `<socket>.outputs`；每个 process
只写 exact-mode `0700` 的 `instance-<server_instance_id>` child。Artifact 是使用
`O_NOFOLLOW|O_CLOEXEC` 打开的 same-owner regular file，mode 精确为 `0600` 且只有一个 link。
Metadata 只保留 output id、absolute path、dimensions、data type、CPU device、tight row step、byte
size、filesystem device 与 inode。Live access 会通过持有的 directory descriptor 重新验证
base/instance ancestry，以及 file owner、type、exact mode、link count、device、inode 与 size。
Record/file 缺失或不匹配时，`compute.result` 返回 top-level daemon `artifact_not_found`，
terminal compute record 仍可 release；unsafe replacement 绝不会被 follow 或 remove。

每个 published artifact 拥有一个 stable 32-hex delivery id。`compute.result` 成功时，会在同一个
store critical section 内验证 artifact，并创建、复用或重新激活其唯一 lease，把 expiry 设置为该次
操作的 monotonic `now + 60 seconds`。重复 access 返回同一个 id 并刷新 deadline。如果后续 response
allocation 或 encoding 失败，router 不会盲目撤销该 stable lease，因为更早成功的 result 可能仍依赖
它；清理由 explicit release 或 bounded lease TTL 负责。Compute release 接受 optional wire
delivery id：matching release 会原子删除 job ownership 与 lease；eviction、
15-minute compute/job TTL，或没有匹配 lease 的 release 只删除 job ownership。Active lease 存在
期间，quota 与 file 都会保留。Compute record 消失后，matching `(compute_id, delivery_id)` 仍可
release orphaned lease。只有 owner 与 lease 都不存在，并且 identity 复验通过后才会 unlink。

Restart cleanup 在 socket lifecycle lock 已证明没有 live cooperating daemon 时运行。它不跟随
symlink 地打开 real base 与 recognized instance child，通过 directory descriptor 扫描，并且只
unlink controlled、same-owner、exact-`0600`、one-link regular file；只删除 empty recognized
instance directory。Unknown name、symlink、non-regular entry、错误 mode/owner、hard link 与
replaced entry 均保持不动，不需要 persisted output registry。

持久 lifecycle lock 会串行化 cooperating daemon instance。Portable POSIX 没有能原子执行
compare-device/inode-and-unlink 的 primitive，因此每次按 name 执行 unlink 或 empty-directory
removal 前，都会立即通过 held fd 与 path identity 重新验证。这是实现支持的 concurrent
replacement 防护边界：观察到 mismatch 或无法确定的 system error 时会保留 entry；但 same-uid
hostile process 在 platform 层仍可能竞态最终 validation 与 `unlinkat` 两条指令。

Shutdown 会立即停止新建或刷新的 delivery lease，同时 accepted image job 在 compute drain 期间仍可
publish。Joined compute worker 退出后，terminal cleanup 删除 job ownership，store 停止
publication，等待 active lease 被显式 release 或达到 injected monotonic TTL，按 identity 清理
unowned artifact，并在关闭 Host session 前关闭自己的 directory。Quota、两类 TTL、clock 与 id
source 均可注入，以执行 deterministic filesystem/race test。

## Private Stable Collection Snapshot

Router 拥有一个 private、type-erased 的 `CollectionSnapshotRegistry`，并随 daemon runtime
lifecycle 启动、停止和清空它。Public Host collection API 保持 full-value API；分页属于 daemon
拥有的 wire 行为，不新增 Host page API 或 public ABI type。`graph.list`、
`inspect.node_ids`、`inspect.ending_nodes`、`inspect.graph`、
`inspect.dependency_tree`、`inspect.traversal_orders`、
`inspect.traversal_details`、`inspect.trees_containing_node` 与
`inspect.recent_compute_planning`、`plugins.ops_sources`、
`plugins.ops_combined_keys` 与 `plugins.ops_combined_sources` 使用该 registry。
Wire 不广告独立 page method 或 cursor-release method。

首个 request 可带 `1..4096` 的 optional integer `limit`；缺省为 4,096，且不得包含
`cursor` 或 `offset`。Continuation 会调用同一 method，携带原始 typed non-page parameter，
再加 required 32-lowercase-hex `cursor`、精确的下一个 nonnegative `offset`，以及
`1..4096` 的 integer `limit`。Unknown field 保持 forward-compatible。Result 保留 method
原有 collection field，并新增 integer `offset`、boolean `has_more` 和 `cursor`；仍有后续 row
时 cursor 是 stable string，only/final page 时为 JSON null。Inspection collection 保留 Host order
与 duplicate。Operation-plugin view 会在发布前按 public key 排序，并在排序后保留 duplicate key。

Private `reserve()` operation 会原子预留一个 record 和完整的 64 MiB per-snapshot allowance。
Production admission 最多保留 64 records 与总计 256 MiB；quota 不可用时报告 private
`CapacityExceeded`。`publish()` 接受 caller 的完整 collection、精确 recursive public-entry
count、精确 measured byte count、binding、requested page limit 与 frame-safe page ceiling。超过
262,144 entries 或 64 MiB 会报告 protocol `response_too_large`，回滚 reservation，并且不发布
cursor 或 retained snapshot；
admission 耗尽会在 Host access 前报告 daemon `capacity_exceeded`。合法的 multi-page value 会
move 进 registry；worst-case reservation 会以事务方式替换成 measured byte count，另一次
admission 随后才会观察该 quota。Empty 与 single-page value 会直接返回，不保留 record。

Entry count 是递归计数，并非 page row 数量。它会计算 outer session、node-id、node、dependency、
traversal 或 planning-history vector/map 的每个 element；每个 node-parameter map entry；存在
spatial metadata 时三个 public 3x3 spatial matrix 的 27 个 element；dependency root 与 flattened
entry 及其 nested node 内的 collection；每个 traversal branch 的 nested node vector；以及每个
recent-planning `planned_node_sample`、`task_sample` 和 task `dependency_task_ids` element。
Scalar/object member 与 optional value 是否存在不额外计数。Registry 会另行保留真实 top-level
row count 作为 paging/type invariant，并拒绝小于该 row count 的 recursive count；注入的 test
limit 同样生效。

完成唯一一次 Host call 后，router 会在分配 shared header、复制 root 或建立 row vector 前预扫描
dependency root/entry，并在 map-to-vector transformation 前预扫描 traversal map。因此 recursive
over-count 会稳定返回 `response_too_large`，且不产生 cursor 或 quota leak。Snapshot-byte
measurement 会把每个 retained variable-width public collection 精确计量一次。Dependency root 与
scalar tree field 组成 shared page header，因此 dependency 计量会以 measured complete entries
array 替换该 encoded header 中的 empty `entries` token（`[]`）。公式为
`header_bytes - 2 + entries_array_bytes`，不会重复计算 empty token。

Multi-page publication 会获得一个经过 collision-check 的 32-lowercase-hex cursor，并冻结精确
method、optional opaque session id，以及 original non-page parameter 的 canonical identity。
发布前，router 会逐 row 测量且不构造完整 JSON DOM，并计算一个最大的 fixed page ceiling，确保
每个连续 page 都能放入 16 MiB frame。计算包含 maximally escaped 的合法 128-byte request id、
32-byte cursor、maximum offset text 与 method-specific header。Caller 可在 continuation 请求更少
row，但不能越过冻结的 safe ceiling。任何无法单独装入 frame 的 indivisible row 都会在 cursor
publication 前报告 `response_too_large`。

Continuation lookup 必须提供 frozen identity 与精确 next offset。Binding/type/offset mismatch，
以及 unknown/expired well-formed cursor，会报告 daemon `cursor_not_found`，且不会推进 record。
Malformed cursor、zero/over-limit page 或 offset arithmetic overflow 会报告 protocol
`invalid_params`，同样不会破坏状态。Continuation page 只读取 retained value，不执行 Host call
或 live-session lookup，因此在 `graph.close` 后仍可读取；copy failure 也不会改变 next offset。

Final page 会原子删除 record 并释放 measured quota。否则，由 cursor publication 时刻开始计算、
且不会因 paging 刷新的固定 15-minute monotonic TTL 会完成释放。Daemon 开始 shutdown 时停止新
reservation，同时保留 active reservation 与已经 published 的 record；client worker join 后，
final snapshot shutdown 会在关闭 Host session 前清空全部 record 与 reservation。下一次 runtime
start 会在 empty registry 上启用 admission。Limit、clock 与 id source 均可注入，用于
deterministic capacity、final-page、expiry 与 shutdown race test。

Collection field 如下：

- `graph.list` 使用 `sessions` row；
- `inspect.node_ids` 使用 `node_ids`；`inspect.ending_nodes` 与
  `inspect.trees_containing_node` 使用 `ending_node_ids`；
- `inspect.graph` 使用 `nodes`；
- `inspect.dependency_tree` 会在每页重复 scalar tree header 与完整 bounded
  `root_node_ids`，并分页 flattened `entries`；
- `inspect.traversal_orders` 使用 `{ending_node_id,node_ids}` 形状的 `orders` row；
- `inspect.traversal_details` 使用 `{ending_node_id,nodes}` 形状的 `branches` row，其中每个
  node 含 `node_id`、`name`、`has_memory_cache` 与 `has_disk_cache`；
- `inspect.recent_compute_planning` 使用 `snapshots`。
- `plugins.ops_combined_keys` 使用有序 string `keys`；
- `plugins.ops_sources` 与 `plugins.ops_combined_sources` 使用
  `{key,source}` 形状的有序 `sources` row。

Installed typed Client 会把新增 page metadata 作为 unknown field 接受，并为 small single-page
graph/tree value 返回完整结果；它目前不会发出 cursor continuation。需要 multi-page router value
的 caller 必须直接使用 typed wire schema。

## Operation Plugin Control 与 View

六个 operation-plugin method 都是 process-global，不定义、不读取、也不解析 `session_id`；
如果传入该字段，它只是会被忽略的 unknown member。Unknown object member 保持
forward-compatible。每个首页 view request 都会在获取公共 Host mutex
并恰好调用一次匹配的 `ps::Host` method 前预留 stable-snapshot quota。每个
mutation 也只会在该 mutex 下恰好调用一次匹配 Host method，且绝不自动 retry。
JSON 中不会出现 loader、registry、factory、callback、DSO handle 或 mutable ownership value。

`plugins.load_report` 要求：

```json
{"directories":["relative/or/absolute/plugin/path-or-pattern"]}
```

Required array 最多包含 256 个 entry，也可为空。每个 entry 必须是 nonempty、不含
NUL、有效 UTF-8 且最多 4,096-byte 的 string；router 保留 relative path 与 Host
支持的 pattern，不会自行重写。成功时返回精确复制的 Host report：

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

`attempted` 与 `loaded` 是非负的精确 Host integer；
`loaded + errors.size()` 等于 `attempted`。`errors` 与 `new_op_keys` 各至多包含
4,096 个 entry。Error path 可为空，否则必须是最多 4,096-byte 的精确有效 UTF-8
value。Error `code` 必须是当前九个 `GraphErrc` numeric value 之一，`name` 为对应的
canonical lowercase name；该 row 是 report data，不是 nested 或 top-level
`OperationStatus`。Error message 使用公共的 UTF-8 repair/截断到 4,096-byte diagnostic
policy。每个 new operation key 必须 nonempty、有效 UTF-8 且最多 1,024 byte。Error 与 key
order 会精确保留；无法放入 16 MiB frame 的 aggregate 会以 `response_too_large`
拒绝。Status-only `Host::plugins_load()` IPC mapping 会调用这一匹配 method，校验完整的
successful report，之后才丢弃它；不存在第二个 wire alias。

`plugins.unload_all` 不定义 known param，并以 `{"unloaded":N}` 返回精确的非负 Host
count。`plugins.seed_builtins` 同样不定义 known param，并返回 `{}`。两者 params object 中的
unknown member 均会被忽略。重复 seed 保留 Host 的幂等 process-owner 行为，不会替换
活跃的 plugin override。任一 mutation 的 Host failure 都保留其精确 Graph-domain mapping。

三个 copied view 使用公共的首页与 continuation control：

- `plugins.ops_combined_keys` 返回有序 string `keys`；
- `plugins.ops_sources` 与 `plugins.ops_combined_sources` 返回
  `{key,source}` 形状的有序 `sources` row。

Key 必须 nonempty、有效 UTF-8 且最多 1,024 byte。Copied source 必须是最多 8 MiB
的有效 UTF-8，且绝不会被解释成 path 或 ownership token。Router 在唯一一次 Host call
前预留一个 snapshot slot 和 64 MiB；该 call 返回后，router 校验每个 row，按 key
排序，测量完整 value，并将其 move 进 stable snapshot registry。Continuation 精确绑定
global method 与 offset，只读取 frozen copy，不调用 Host。上文的一般 4,096-row page、
262,144-entry/64-MiB snapshot、64-record/256-MiB aggregate、15-minute TTL、cursor
mismatch、expiry 与 shutdown 规则保持不变。

成功的 plugin library 仍归 Host 的唯一 process-global plugin owner 所有。Load 结果对独立
socket client、graph session 与 Host adapter lifetime 可见。Client 断开或销毁某个 Host
adapter 都不会 unload。只有显式的 process-global `plugins.unload_all` 才会移除/恢复
活跃 plugin key；之后三个 view 都会观察到一致状态。Router 不暴露也不缩短
callback 或 returned-value library lease。

## Inspection Value

Inspection 会在获取 Host mutex 前原子地 admit opaque session，并且只调用 `ps::Host`。并发 close
会先标记 session，拒绝新的 inspection admission，并等待已经 admitted 的 inspection：

- `inspect.graph` 返回 `{session_id, nodes}`；
- `inspect.node` 要求能由 public `int`-backed `NodeId` 表示的 nonnegative integer
  `node_id`，返回 `{session_id, node}`；
- `inspect.dependency_tree` 接受相同范围的 optional nonnegative integer/null `node_id` 与
  optional boolean `include_metadata`，然后返回
  `{session_id, ...flattened tree fields}`；
- `inspect.traversal_orders` 与 `inspect.traversal_details` 通过上述 page row 返回 Host 中以
  ending node 为 key 的 copied map；
- `inspect.trees_containing_node` 要求 nonnegative `node_id` 并返回 copied ending-node id；
- `inspect.dirty_region` 返回当前 copied dirty-region snapshot；
- `inspect.compute_planning` 返回 `{session_id,planning}`；尚无 planning result 时
  `planning` 为 null，否则为一个 indivisible copied planning object；
- `inspect.recent_compute_planning` 返回 copied planning snapshot 的 stable page。

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
Direct `inspect.node`、ROI、dirty-region 与 current-planning result 不发布 cursor。Collection
method 使用上述 stable metadata，不改变其 copied Host contract。Planning value 会把
`ComputeIntent` 与 `DirtyDomain` 编码为 stable lowercase snake-case label，把 nested optional
value 编码为 null，并把所有 count 编码为精确 nonnegative integer。同一组 reusable enum、
`PixelRect`、bounded-string、array、page、opaque-id 与 nested-status codec 会递归应用于
composite value；decode 失败时 caller-visible state 保持未发布。

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
一个 daemon mutex；socket read/write 绝不持有它。Shutdown 会先停止所有 session、snapshot 与
compute admission，关闭 listener，shutdown tracked client descriptor 以唤醒 read，并 join 全部
connection worker。随后它会 drain 所有 accepted compute job，join 唯一 compute worker，释放
terminal job ownership，清空 retained collection snapshot，并通过显式 release 或 TTL 等待 active
delivery lease；OutputStore 关闭后才关闭 Host session 并清空 session mapping。最后在 lifecycle
lock 仍持有时移除 socket，释放 lock，再销毁
Host state；持久 lock file 会有意保留。

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
  test_compute_request_registry test_collection_snapshot_registry \
  test_output_store test_event_stream_boundaries test_ipc_daemon \
  public_header_self_containment -j
ctest --test-dir build --output-on-failure \
  -R '^(FrameCodec|ProtocolEnvelope|IntegerCodec|ProtocolErrors|ProtocolParams|ProtocolGraphLoad|ProtocolGraphClose|ProtocolOperationPlugins|InspectionJson|SessionRegistry|ComputeRequestRegistry|CollectionSnapshotRegistry|OutputStore|ComputeEventRing|SchedulerTraceRing|ClientLifecycle|ClientResultValidation|IpcDaemon|IpcDaemonOperationPlugins|IpcObservationFixtureDaemon|StaticProductConsumerSmoke|IpcDisabledInstallSmoke|PublicHeaderSelfContainment)'
```

`StaticProductConsumerSmoke` 验证 installed backend 与第二个 client-only consumer；
`IpcDisabledInstallSmoke` 验证 IPC-disabled clean install 不含 IPC forwarder、header、target、archive
或 daemon，同时 embedded consumer 仍可用。Real-process test 有 CTest timeout 与 bounded
SIGTERM/SIGKILL/waitpid cleanup。`test_ipc_daemon` 会启动产品 `photospiderd` 以验证 embedded-Host
行为，也会把 non-installed `ipc_output_fixture_daemon` 作为独立进程启动，以提供 deterministic
image 与 bounded-observation outcome。该 fixture 仍使用 production internal
Server/router/OutputStore/Unix-socket/worker stack；它只是 test 的 build dependency，不拥有独立
CTest entry，也不会改变 product startup 或提前 seed plugin。Fixture-only protected fixed-width
control file 提供 cross-process manual monotonic clock；private internal Server composition overload
接收较小版本的既有 snapshot、compute-registry 与 OutputStore policy，以及 deterministic id
source。这些 control 不是 `photospiderd` flag、environment failpoint 或 wire method。这些都是
长期 product-behavior test；不会创建 retained `tests/results`、replay、provenance 或 migration gate。
