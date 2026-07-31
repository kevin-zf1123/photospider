# 内核缓存模型

内核在每个 `Node` 上拥有正式 HP 内存缓存，在 `RealtimeProxyGraph` 中拥有临时 RT proxy
状态，并在图缓存根目录下拥有磁盘缓存文件。本文档定义当前缓存语义。

## 正式缓存与临时状态

| 位置 | 状态 | 含义 |
| --- | --- | --- |
| `Node::cached_output_high_precision` | 正式缓存 | 完整质量 HP output owner；whole-output reuse 还要求 complete `hp_region`。 |
| `RealtimeProxyGraph` node state | 临时 RT proxy | 低分辨率交互式预览/更新输出。 |

只有高精度输出是正式可复用缓存。这意味着只有 HP 输出可以作为后续 HP 计算、磁盘缓存、其他
可复用缓存行为或另行请求的 output transaction 的权威来源。RT proxy 输出是临时交互式状态，
不能被视为权威缓存，不能作为磁盘缓存同步来源，也不能直接作为 output-commit input。正式 HP
cache 与 disk cache 本身都不是 durable user-output authority。

## HP 缓存

HP 计算写入 `cached_output_high_precision`。HP 缓存是节点的权威完整质量 result
owner。Dirty publication 可以保留 partial formal output，但只有 `hp_region` 能证明
complete coverage 时，`ComputeCachePolicy` 才会把它暴露给 whole-output consumer。
对于带 image facet 的 sealed Value，complete compatibility ImageRect 或 complete
rank-general TensorSlice 都是可接受的证明。

相关字段：

| 字段 | 含义 |
| --- | --- |
| `hp_version` | HP 输出变化的版本计数器。 |
| `hp_region` | 正式 HP output 中已知有效的规范化逻辑 Region。 |

## RT 状态

RT 计算写入 `RealtimeProxyGraph`。每个 proxy node 以原 graph node id 为 key，只保存低分辨率输出、HP-space Region metadata、version 和 RT dirty-source generation。它不复制 Node 参数、输入、拓扑、cache 或正式 HP 状态。当观察到的 graph topology generation 改变时，同步会重置 live proxy entries，而不是按复用 node id 保留 state，因此 reload/edit workflow 不会暴露上一份 graph 的陈旧低分辨率输出。

Dirty RT execution 不会写 graph-owned RT 字段。Worker task 会先把代理输出、Region metadata、版本计数和 dirty-source commit generation stage 到 `RealtimeProxyWriteBuffer`，然后在 RT dirty work set drain 后把 staged state 提交到 `RealtimeProxyGraph`。Dirty HP execution 同样会先把 HP 输出 stage 到 `HighPrecisionDirtyWriteBuffer`，再提交到 `GraphModel`，因此 HP/RT sibling 可以并发计算，同时保持 RT-first commit 顺序。

相关字段：

| Proxy 字段 | 含义 |
| --- | --- |
| `version` | RT proxy 输出变化的版本计数器。 |
| `region_hp` | RT 更新所代表的规范化 HP-space ImageRect Region。 |
| `dirty_source_generation` | 用于 stale source 检查的 RT dirty source generation。 |

## 磁盘缓存

`GraphCacheService` 处理 `GraphModel::cache_root` 下的磁盘缓存文件。节点缓存条目描述缓存类型和位置。
图像缓存文件保存为图像文件，命名的 `NodeOutput::data` 条目保存为图像文件旁边的 YAML 元数据。
内存中的命名 data 始终是脱离 adapter 的 `plugin::ParameterMap`；
`GraphCacheService` 绝不构造 YAML value。

对于 CLI 加载的 graph，`GraphModel::cache_root` 会在 graph load 前由 `cache_root_dir`
配置决定，并解析为 `<cache_root_dir>/<graph_name>`。相对 `cache_root_dir` 按进程当前工作目录解析。
未提供 cache root 的直接 `Kernel::load_graph` 调用继续使用 `<root_dir>/<graph_name>/cache`。

磁盘缓存精度当前支持 `int8` 和 `int16` 保存路径。加载的图像缓存数据会转换为浮点图像缓冲区。

当前 disk format 不持久化 Region metadata，因此只有 complete HP output 可以被保存，
或在 synchronization 时保护 configured disk artifact。Partial formal output 不会被
disk load 覆盖，也绝不会被重新标记为 complete。保存或同步 partial node 时，会移除较旧的
configured image/YAML artifact，而不是编码 partial byte。成功 disk load 会为 fresh
decoded output 派生 complete validity。

图像字节通过私有、依赖中立的 `ImageArtifactCodec` 契约。`Kernel` 从产品组合根取得一个配置好的
共享 codec，并将其注入 `GraphCacheService`；Graph/cache 代码只提供 path、`ImageBuffer` 与
规范化整数精度。OpenCV 启用时，已配置 adapter 使用 OpenCV imgcodecs，并把 provider failure
翻译为 `GraphErrc::Io`，同时让 OpenCV `StsNoMem` 保持为 `std::bad_alloc`。OpenCV 禁用时，
已配置 unavailable codec 会在不发现或导出 OpenCV 的情况下返回 `GraphErrc::Io`。测试会注入确定性 fake，
在不读取或写入真实图像格式的情况下验证调用顺序、生命周期保持、精度选择、可恢复错误与资源耗尽。

Named value 独立通过私有、依赖中立的 `CacheMetadataCodec` contract，只交换 path 和脱离
adapter 的 `ParameterMap` value。`Kernel` 注入、`GraphCacheService` 保留第二个不可变 shared
owner，且其 service lifetime 与 image codec 相同。Cache policy 仍负责推导同级 `.yml` path、
创建目录、选择 entry、记录计时和诊断、保持 HP 权威性，以及移除陈旧文件。只有已配置的
`YamlCacheMetadataCodec` 拥有 YAML node、递归 value conversion、stream IO 与 provider
exception translation。Null document 解码为空 map；无效 representation 变成
`GraphErrc::InvalidYaml`，可恢复 write/emission failure 变成 `GraphErrc::Io`，
`std::bad_alloc` 原样传播。确定性 fake 会验证精确 path、value、保留生命周期、error category
与资源耗尽，而 cache code 不声明 YAML type。

磁盘缓存加载尝试会保留既有 try-load 布尔返回契约，同时通过 GraphModel 私有的 disk-cache
diagnostic store 记录最新诊断。该 store 独占 optional value 与 no-throw mutex，因此 worker
record、reader snapshot、clear/reload reset、compute clone 与 staged publication 无法绕过同一套
同步契约。每项操作都通过私有、不可复制的 scoped guard 持有 mutex；snapshot copy 抛异常时会在
stack unwinding 中释放锁，双 store publication 则按 `std::less` 地址全序获取锁，并按 guard 析构
的逆序释放。每个 store 都是且仅是一个 live 或 staged `GraphModel` 的直接成员，本身不拥有 worker
lifetime。凡是可能访问该 store 的 runtime compute-request work、graph-state work 与 scheduler
worker，都必须在所属 model 销毁前排空并 join；任何访问都不得与 member teardown 竞态。调用方检查
独立 snapshot，而不直接读取可变 storage。该诊断结果会区分跳过的尝试、真实 miss、命中以及读取/
解析错误。损坏的图像文件、无效的 YAML 元数据和文件系统失败会被记录为带错误码和消息的错误，而不
是与普通 cache miss 混在一起。

## 当前耐久性与失败边界

当前 cache save 不是 atomic cache-entry transaction。`GraphCacheService` 会创建目录，并针对
最终的两个同级 path 调用已配置 image 与 metadata codec。因此 image payload 与 YAML metadata
可以分别成功或失败。该 service 不提供 entry-level staging rename、rollback、manifest-last
publication、file 或 directory synchronization receipt、retry protocol 或 crash recovery。

`cache_all_nodes` 统计存在 HP output、因而尝试过 save path 的 node；该计数并不证明每个 node
都配置了 artifact，也不证明存在 durable cache entry。Cache load diagnostic 是进程内关于最近
一次尝试的 observation，不是 durable audit record。

当前 product compute commit policy 会在精确 revision/generation validation 后、no-throw live
Graph swap 前，执行符合条件的 changed-HP cache write。Live predicate 通过后，它现在会把
该 staged save mechanism 提交到 process-owned `ComputeIoExecutor`。Admission 会在 lazy
codec/task payload construction 或 filesystem 副作用之前，同时计入 task 数与 checked
planned-byte estimate。I/O task 会保留 prepared Graph transaction，graph-state policy owner
则等待 typed completion 并应用测得的 I/O time。CPU compute worker 不能执行该等待。因此
rejection、cache codec、filesystem 或 allocation failure 仍可能让该 `ComputeRun` 失败，并保持
live Graph/RT state 不变。这种顺序是当前行为，不表示 cache 属于 user-output commit。
[ADR 0009](../../adr/zh/0009-compute-io-durability-and-completion-semantics.zh.md)
接受不同的目标顺序：cache persistence 在 Run publication 后拥有独立 typed outcome。

## 缓存命令

| 操作 | 效果 |
| --- | --- |
| Clear drive cache | 删除磁盘缓存目录内容并重建根目录。 |
| Clear memory cache | 清理 `GraphModel` 跟踪的内存 HP 缓存。 |
| Clear cache | 同时清理磁盘和内存缓存。 |
| Cache all nodes | 在配置允许时将具有 complete HP 输出的节点保存到磁盘；partial node 会清理 stale configured artifact。 |
| Free transient memory | 清理非终点节点的内存缓存状态。 |
| Synchronize disk cache | 保存 complete HP 输出，并为没有 complete validity 的节点移除陈旧磁盘文件。 |

磁盘缓存保存、加载和同步只使用 `cached_output_high_precision`。RT proxy 输出不会保护陈旧磁盘文件，也不会被提升为磁盘缓存状态。

## 边界与原理

- HP 路径写入 `cached_output_high_precision`。
- RT 路径将 `RealtimeProxyGraph` 写为临时交互式状态；dirty worker 写入必须先经过
  `RealtimeProxyWriteBuffer`，再提交到 proxy。
- 正式缓存的保存、加载、同步行为、后续 HP 计算与另行请求的 output creation 必须使用 HP
  输出，不能把 RT 输出提升为权威缓存或 durable-output authority。
- 长期测试分别验证 HP graph cache 和 RT proxy graph state。

`GraphInspectService` 只从 HP cache 选择 node-local 显示 metadata。当前 Host inspection surface
不会把 RT proxy state 提升到 `GraphModel`，也不会将其作为权威 cache metadata 暴露。

只有一个正式缓存权威，可以防止低分辨率 preview 静默变成 HP dependency 或 persistence source。
Request-local staging 会让尚未组装完成的 dirty output 保持不可见，直到相应 domain 的工作 settle。

当前私有 disk-cache 实现既不调用 OpenCV image codec，也不调用 YAML API。它依赖注入的
`ImageArtifactCodec` 与 `CacheMetadataCodec` contract；已配置的私有 adapter 拥有 provider
decode/encode、递归 conversion、stream IO 与 exception translation。Issue #62 完成了这条
runtime/cache value 边界。Issue #63 增加由 capability 选择的真实或 unavailable adapter：默认
product 发现并链接 yaml-cpp/OpenCV；dependency-disabled product 两者都不发现，并在显式
representation IO 时返回 `GraphErrc::Io`。
[ADR 0002](../../adr/zh/0002-external-libraries-are-kernel-adapters.zh.md)
和精确的[依赖中立内核目标](../../roadmap/zh/Kernel-Evolution.zh.md#依赖中立内核)描述最终 adapter 与
document boundary。

## V-3 Runtime Allocation 与 Revision Identity

正式 HP `NodeOutput` 可以同时携带 `image_value` 与 `image_buffer`。对于非空 CPU 图像，
有效 sealed Value 是 allocation/revision identity authority；ImageBuffer 是独立拥有的
compatibility snapshot。普通 HP publication 与 cache-load boundary 会在 output 成为正式
cache 前，根据当前 CPU ImageBuffer 补齐缺失的 Value。复制正式 cache entry 会保留其
`AllocationIdentity` 与 `ValueRevisionId`。

可变 dirty work 不能保留旧 authority。其 clone 会在任何 ImageBuffer 写入前清除
`image_value`，再在 HP commit 前把 settle 后的 byte seal 为全新的 allocation 与 revision。
Replacement output 与 disk decode 同样创建新 identity。RT proxy output 继续保持 transient，
不会成为正式 cache identity source。

Disk save 会优先使用已存在的 sealed Value，并从其 checked image view 派生临时 ImageBuffer
snapshot，因此之后对 compatibility snapshot 的 mutation 不会改变持久化 byte。现有 image
与 YAML format 仍只持久化 representation byte 与 named metadata：
`AllocationIdentity` 和 `ValueRevisionId` 都不会被序列化、从 path 重建或用作持久 cache/task
key。两类 token 都是 opaque、process-local runtime identity；disk reload 必然铸造新 token。

## V-4 Region Validity

`Node::hp_region` 是唯一正式 HP cache authority 的规范化逻辑 validity metadata；它不是另一份
output、allocation identity、Value revision、disk path 或 persistence key。Full compute、
sequential publication、result commit 与成功 disk load 会发布 complete Region（`ImageRect`、
TensorSlice bounds，或仅已知 non-image named data 时的 Whole）。

Dirty HP staging 会一起携带 output、Region、version 与 source generation。
对于 exact core Region bridge，compatible complete-shaped result 只贡献 selected
byte：selected coordinate 会替换 prior byte，unselected coordinate 继续来自 staged
output。即使 existing complete validity 的证明是 ImageRect，而 update 是 TensorSlice，
该 merge 也会保留 complete validity。精确且可表示的 union 会保留累计 partial validity。
当两个 partial update 无法由有界 one-clause contract 表示时，staging 保留新鲜的精确
update 作为安全 under-approximation，而不是发布错误的 bounding superset。现有
revision/current-generation predicate 会原子地发布或丢弃完整 staged state。

Fresh partial publication 仍是 formal state，但不能满足 whole-output reuse 或当前 disk
persistence。Normal Whole computation 会替换它并派生 complete validity。Generic ABI v2
monolithic callback 继续替换 complete output；selected-byte merge 只用于 source-private
exact core Region implementation。

每条 whole-output execution route 都会通过 `ComputeCachePolicy` 应用这条规则，包括
empty-plan validation、monolithic 与 tiled runner cache guard、committed upstream
dependency resolution，以及 final target return。Dependency 已完成的 current-request
temporary result 可以流向 downstream，但 raw partial persistent output 的存在绝不能抑制
它已规划的 recomputation，也不能向 whole-output consumer 暴露其 byte。

Dirty planning 绝不会把 exact 旧 output 解释为“当前 dirty snapshot 指定的 Region 已经是最新”的
证据。Planning-time cache observation 后，callback-free target cone 仍会保留；无论旧 cache
在 selection 前保持 exact、被删除或变为 partial，每个 dirty-selected node 都保持 executable。
既有 byte 可以 seed request-local write buffer 并保留未选中的坐标；它们是 merge base，不是
dirty work satisfaction boundary。普通 full HP planning 仍可立即消费同一个 exact cache。Dirty
selection 自身只会从 current-request external result 形成 satisfaction，绝不会使用旧 formal
cache。

RT proxy state 使用 HP-space `region_hp`，但仍只支持 image。Checked adapter 只会从一个精确
内建 ImageRect 派生当前 rectangular downsample/inspection metadata。TensorSlice 与 Whole
不会进入 RT 或 downsample rectangle boundary。Region value 与 Tensor axis 会计入
retained-memory accounting。

### 当前有界机制与未来持久化关系

[ADR 0008](../../adr/zh/0008-generic-values-memory-bindings-and-regions-are-explicit-versioned-contracts.zh.md)
把未来 persistence 分成 graph document、canonical descriptor envelope、artifact/cache
manifest 与 chunk，以及绝不持久化的 runtime state。`DescriptorDigest`、`ContentDigest`、
`StorageLayoutDigest`、`ArtifactId` 与 `ValueRevisionId` 回答不同 identity 问题；
device/allocation identity、fence、lease、access plan 与 residency replica 永远不会进入持久
logical content identity。
当前 V-3 的 process-local `ValueRevisionId` 是 runtime publication identity，不是任何未来
canonical descriptor、content、layout 或 artifact digest。

该目标不会改变上文描述的当前 cache format 或 authority。HP output 仍是唯一正式可复用 cache，
RT proxy output 继续保持 transient，当前注入的 artifact/metadata codec 继续作为实现边界，
直到后续切片迁移 cache manifest 与 payload。未来 residency replica 也不会成为第二个 cache
authority。

[ADR 0009](../../adr/zh/0009-compute-io-durability-and-completion-semantics.zh.md)
还会把可丢弃 cache persistence 与 durable user-output commit 分离。Issue #88 现已实现上文
描述的有界 mechanism 与 staged HP cache-save 垂直路径；同步 cache administration 与 load
保持不变。当前不可拆分的 image-codec call 整体运行在 I/O worker 上；未来拆分后的 codec
contract 必须把独立准入的 CPU-heavy phase 送回 CPU domain。Executor 绝不拥有 cache
eligibility、path、output commit policy、Graph 文档 transaction、daemon state、retry、receipt
或 durability。未来 Run publication 之后的 cache outcome 与 `OutputStore` commit authority
继续与该 executor 分离。

## 实现与验证入口

- `src/lib/core/image_artifact_codec.hpp`
- `src/lib/core/cache_metadata_codec.hpp`
- `src/lib/adapters/opencv/image_artifact_codec_opencv.*`
- `src/lib/adapters/yaml/yaml_cache_metadata_codec.*`
- `src/lib/adapters/yaml/parameter_value_yaml.*`
- `src/lib/providers/configured_image_artifact_codec.*`
- `src/lib/providers/configured_persistence_adapters.*`
- `src/lib/core/value_image_adapter.*`
- `include/photospider/data/region.hpp`
- `src/lib/core/region.*`
- `src/lib/core/region_image_adapter.*`
- `src/lib/graph/graph_cache_service.*`
- `src/lib/execution/compute_io_executor.*`
- `src/lib/graph/graph_model.*`
- `src/lib/runtime/kernel_compute.cpp`
- `src/lib/ipc/output_store.*`
- `src/lib/compute/compute_cache_policy.*`
- `src/lib/compute/compute_node_task_runner.*`
- `src/lib/compute/compute_task_dispatcher.*`
- `src/lib/compute/realtime_proxy_graph.*`
- `src/lib/compute/dirty_write_buffers.*`
- `tests/integration/test_cpu_dense_tensor_image_operation.cpp`
- `tests/unit/test_region_contracts.cpp`
- `tests/integration/test_disk_cache_diagnostic_concurrency.cpp`
- `tests/unit/test_compute_io_executor.cpp`
- `tests/integration/test_kernel_contracts.cpp`
- `tests/integration/test_compute_service_split.cpp`
- `tests/integration/test_host_adapter.cpp`
- `tests/integration/dependency_disabled_install_smoke.py`
