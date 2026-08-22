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
| `region_hp` | RT 更新所代表的规范化有符号 logical HP ImageRect Region；它不是 RT/storage ROI。 |
| `dirty_source_generation` | 用于 stale source 检查的 RT dirty source generation。 |

## 磁盘缓存

`GraphCacheService` 处理 `GraphModel::cache_root` 下的磁盘缓存文件。节点缓存条目描述缓存类型和位置。
现在，每个受支持的 configured location 都命名一个 portable named-Value transaction：

| 路径 | 当前职责 |
| --- | --- |
| configured location | 供外部检查使用的可选 image-codec projection；绝不是 replay authority。 |
| `<location>.yml` | 可选、脱离 Value 的 `NodeOutput::data` parameter metadata。 |
| `<location>.values` | 包含全部正式 named Value 的 canonical public `NamedValueArtifactSet` archive。 |
| `<location>.manifest` | 最后写入的 versioned transaction record；它绑定 archive/metadata 数量、byte size、generation-derived SHA-256 digest 与一个随机 writer generation。 |

Graph-document location 是不受信任的 path input，必须是仅使用 ASCII 字母、数字、点、下划线和
连字符的单个非空 portable leaf。Absolute/rooted 或 multi-component path、`.`/`..`、separator、
其他 punctuation/control/non-ASCII byte、reserved device basename（包括由 ASCII 边界一并拒绝的
Unicode superscript COM/LPT spelling）、trailing dot/space、自身 derived-sibling alias，以及不同
configured image entry 之间的 alias，都会在受支持的持久化路径上于 capture、codec 或 filesystem
effect 前被拒绝。GraphCache 磁盘持久化当前仅支持 POSIX。Cache owner 会保留 no-follow root/node
directory descriptor，并通过 `*at` operation 执行 archive/manifest read、write 与受控 delete。
Symlink、directory、device、FIFO、hard-link alias、foreign owner、无法证明的 identity 与 sparse
replay file 都会被拒绝。当前 dependency-neutral image/metadata codec interface 仍按 path 调用，
因此其前后会校验 directory/leaf identity；这会收窄外部 path-only codec 内恶意 same-owner
replacement race，但不声称彻底消除该竞态。

在 Windows 上，所有可能 save、load、read、write、cleanup、synchronize 或 clear 磁盘状态且 cache
root 非空的 GraphCache 请求，都会在 capture、codec、filesystem、executor admission、Graph/cache
mutation、timing 或 diagnostic publication 前，以稳定 typed `GraphErrc::InvalidParameter` platform
error fail closed。实现不会调用 Win32 filesystem API，也不会创建 root、directory 或 file。空 root
仍表示 no-disk intent：load 保留既有 Skipped diagnostic，clear-drive 返回零结果，cache-all/
synchronize 保留 HP-node count，combined clear 仍可清理 memory。`skip_save_cache` 同样保持 no-op，
纯 memory 与 derived-statistics API 继续可用。Windows 磁盘持久化是 future target，而不是部分支持的
HANDLE 实现。

Archive 是唯一持久化 Value authority。它保留精确有序名称、descriptor 与 Facet record、layout 与
binding fact、buffer role 与 envelope、payload byte/digest，以及适用的 descriptor/content/layout
identity；不保留 process-local allocation、Value revision、producer、fence、mapping、device 或
lease identity。内存中的 parameter data 仍是脱离 adapter 的 `plugin::ParameterMap`；
`GraphCacheService` 绝不构造 YAML value。

解析到同一个 normalized cache root 的每个 `GraphCacheService` instance 会共享一个弱保留的进程
coordinator。Save、load、stale cleanup、synchronization 与 drive clear 都在该 root 下串行执行。
每次逻辑 mutation 都推进 checked epoch：在较新的 save、partial cleanup、synchronization 或 clear
之前已 prepare 并 admitted 的 asynchronous writer 会观察到自己已被 supersede，且不执行 filesystem
work。最后一项 operation 结束后，weak registry 不保留 root；同 root callback reentry 会在取锁前
失败，不同 root 则可独立推进。

对于 CLI 加载的 graph，`GraphModel::cache_root` 会在 graph load 前由 `cache_root_dir`
配置决定，并解析为 `<cache_root_dir>/<graph_name>`。相对 `cache_root_dir` 按进程当前工作目录解析。
未提供 cache root 的直接 `Kernel::load_graph` 调用继续使用 `<root_dir>/<graph_name>/cache`。

磁盘缓存的 image projection precision 当前支持 `int8` 与 `int16` 保存路径。该显式 conversion
只影响辅助 image file；portable replay 会重建 archive 中的精确 Value representation。

当前 disk format 不持久化 Region metadata，因此只有 complete HP output 可以被保存，
或在 synchronization 时保护 configured disk artifact。Partial formal output 不会被
disk load 覆盖，也绝不会被重新标记为 complete。保存或同步 partial node 时，会移除较旧的
projection、metadata、archive 与 manifest，而不是编码 partial byte。成功 disk load 会为 fresh
reconstructed output 派生 complete validity。

显式 Empty/Whole validity 会在解释 formal Value 前完成分类。任何 finite provider-defined
canonical-image validity 都会被保守判定为 incomplete，因为 core 不能通过错误的 representation
accessor 推导 DenseTensor bounds。此类 output，以及所有 partial packed、quantized 或其他
provider-incompatible canonical image，只会运行受控 predecessor cleanup：不会构造 `ImageView`、
捕获 artifact、查询 provider、调用 codec，也不可能在 restart 后命中。Whole provider-defined
canonical-image validity 在此分类中仍为 complete，并会在 filesystem effect 前到达既有的
unsupported-image preflight。

可选 ordinary-image projection 通过私有、依赖中立的 `ImageArtifactCodec` 契约。`Kernel` 从产品组合根取得一个配置好的
共享 codec，并将其注入 `GraphCacheService`；Graph/cache 代码只提供 path、精确的普通 image Value
以及显式 decode/encode sample request。OpenCV 启用时，已配置 adapter 对非 OpenEXR format 使用
OpenCV imgcodecs，并把 provider failure 翻译为 `GraphErrc::Io`，同时让 OpenCV `StsNoMem` 保持为
`std::bad_alloc`。其封闭 write matrix 接受：JPEG UINT8 1/3-channel；PNG/TIFF/JPEG2000
UINT8/UINT16 1/3/4-channel；BMP UINT8 1/3-channel；WebP UINT8 3/4-channel；PGM
UINT8/UINT16 1-channel；PPM UINT8/UINT16 3-channel；PNM UINT8/UINT16 1/3-channel；
以及 PAM UINT8 1/3-channel。尤其是 WebP grayscale、BMP alpha、PBM、PAM alpha 与 PAM
UINT16 都会在 destination mutation 前被拒绝。可选的普通 OpenEXR codec 保留独立 signed
data/display window。两种 codec 都禁用时，已配置 unavailable codec 会在不发现或导出它们的
情况下返回 `GraphErrc::Io`。测试会注入确定性 fake，在不读取或写入真实图像格式的情况下验证
调用顺序、生命周期保持、精度选择、可恢复错误与资源耗尽。真实 codec 测试会 encode/decode
每个允许的 OpenCV tuple，并验证 depth、channel 与 shape。

脱离 Value 的 parameter 独立通过私有、依赖中立的 `CacheMetadataCodec` contract，只交换 path 与
`ParameterMap` value。`Kernel` 注入、`GraphCacheService` 保留第二个不可变 shared
owner，且其 service lifetime 与 image codec 相同。Cache policy 仍负责推导同级 `.yml` path、
创建目录、选择 entry、记录计时和诊断、保持 HP 权威性，以及移除陈旧文件。只有已配置的
`YamlCacheMetadataCodec` 拥有 YAML node、递归 value conversion、stream IO 与 provider
exception translation。Null document 解码为空 map；无效 representation 变成
`GraphErrc::InvalidYaml`，可恢复 write/emission failure 变成 `GraphErrc::Io`，
`std::bad_alloc` 原样传播。确定性 fake 会验证精确 path、value、保留生命周期、error category
与资源耗尽，而 cache code 不声明 YAML type。

每次 load 都会收到从 frozen `PlannedOutputAuthority` 复制而来的
`ValueDiskCacheOutputSchema`：是否计划 canonical image、精确 parameter-result 名称，以及精确的
generic named-Value 名称。Transaction 必须同时具备 archive 与 manifest；退役的 image/YAML pair
或任何 partial transaction 都会在 publication 前成为 incompatible miss。Reader 会先校验 manifest
version/flag/count、archive byte size 与 digest、canonical archive framing、精确 planned name、每项
descriptor/Facet/layout/binding fact 与 payload digest，以及 provider generation，再重建本地 candidate。
`Kernel` 拥有一个 process-domain `DataDefinitionRegistry`，把同一个 borrowed authority 注入
`GraphCacheService`，并在 cache service 之前声明它，从而让 registry 存活时间覆盖每次 replay。
Registry 现有 generation/lease synchronization 继续作为 provider thread-safety 与 DSO lifetime
boundary。因此 provider-defined multi-buffer Value 可沿真实 embedded/CLI Kernel 与 GraphRuntime
composition replay；缺失或不兼容 provider 属于 typed error。

每个 writer 会生成一个不可预测的 128-bit generation，并把它重复写入每个 archive envelope 的
owner-supplied commit join。Archive 与 metadata 的 manifest record 都使用
`SHA256(generation || canonical_byte_size || raw_file_digest)`，所以来自不同 writer、raw digest 各自
有效的 file 也无法组成同一 generation。计划 parameter output 时，manifest 还会绑定精确 metadata byte。Reader 会在 codec decode 前后验证
这些 byte、比较精确 decoded key set，并在最后重新读取 manifest。Manifest/payload race、tamper、
mixed generation、stale name、missing file 或任一 Value 失败，都不会发布 candidate 的任何部分。Hit
只会整体 move 一次完整 `NodeOutput`，并为每个重建 Value 铸造 fresh runtime identity。可选 image
projection 从不参与 replay，因此不会成为第二个 Value authority。

磁盘缓存加载尝试会保留既有 try-load 布尔返回契约，同时通过 GraphModel 私有的 disk-cache
diagnostic store 记录最新诊断。该 store 独占 optional value 与 no-throw mutex，因此 worker
record、reader snapshot、clear/reload reset、compute clone 与 staged publication 无法绕过同一套
同步契约。每项操作都通过私有、不可复制的 scoped guard 持有 mutex；snapshot copy 抛异常时会在
stack unwinding 中释放锁，双 store publication 则按 `std::less` 地址全序获取锁，并按 guard 析构
的逆序释放。每个 store 都是且仅是一个 live 或 staged `GraphModel` 的直接成员，本身不拥有 worker
lifetime。凡是可能访问该 store 的 runtime compute-request work、graph-state work 与 scheduler
worker，都必须在所属 model 销毁前排空并 join；任何访问都不得与 member teardown 竞态。调用方检查
独立 snapshot，而不直接读取可变 storage。该诊断结果会区分跳过的尝试、真实 miss、命中以及读取/
解析错误。无效 manifest、archive/metadata digest mismatch、missing provider、无效 YAML metadata 与
filesystem failure 都会被记录为带 error code 和 message 的错误，而不是与普通 cache miss 混在一起。

## 当前耐久性与失败边界

当前 cache save 提供 manifest-last publication 与 all-or-nothing replay，但不是 crash-durable 或
atomic filesystem transaction。`GraphCacheService` 会先把每个 named Value 捕获并验证为脱离 runtime
的 archive byte，随后才进行 planned-byte admission、task construction、filesystem mutation 或 codec
invocation。不受支持的 Value 会以 typed `InvalidParameter` 失败；`skip_save_cache` 以及 absent/
unsupported cache entry 则会在该 validation 前保留既有 no-op policy。

构造会冻结 GraphCache-specific resource limit，且这些 limit 独立于更宽的 public artifact framing
ceiling。当前默认值至多允许 512 MiB canonical archive、16 MiB detached metadata、512 MiB
auxiliary projection，以及 528 MiB archive-plus-metadata replay。实现会先校验 manifest fact 与 checked
aggregate arithmetic；随后在 archive allocation 或 digest traversal 前校验 no-follow regular-file
type、single-link ownership、physical non-sparse storage、精确 size 与 service limit。Archive read 会
一边填充唯一 file-byte owner 一边 hash；public decode 后释放该 owner，并在每个 Value 重建后逐项
释放 artifact payload owner，以限制重叠，而不是同时保留 8 GiB archive 与多份 payload set。
`std::bad_alloc` 继续原样传播。

一次 complete save 会依次写入可选 image projection、可选 parameter metadata、canonical archive，
移除被排除的 projection/metadata predecessor，捕获精确 archive/metadata record，验证 metadata codec
round-trip，并在最后写入和回读 versioned manifest。Partial output 会移除全部四个文件。Failure 可能
留下 partial 或 mixed generation，但 replay 只有在 manifest-bound size/digest、stable generation、
精确 name 与完整 artifact set 全部通过验证后才可发布，因此这些残留不可复用。该 service 不提供
temporary-file rename、rollback、file/directory synchronization barrier、durability receipt、retry
protocol 或 crash recovery。Sequential save、parallel committer、compute-I/O executor、cache-all 与
synchronization 全部汇聚到同一 mechanism。

`cache_all_nodes` 统计存在 HP output、因而尝试过 save path 的 node；该计数并不证明每个 node
都配置了 artifact，也不证明存在 durable cache entry。Cache load diagnostic 是进程内关于最近
一次尝试的 observation，不是 durable audit record。

当前 product compute commit policy 会在精确 revision/generation validation 后、no-throw live
Graph swap 前，执行符合条件的 changed-HP cache write。Live predicate 通过后，它现在会把
该 staged save mechanism 提交到 process-owned `ComputeIoExecutor`。通过 limit check 后，会在
lazy codec/task payload construction 或 filesystem 副作用之前，暂时预留 task 数与 checked
planned-byte estimate。Factory 抛异常、返回空 callback 或 task/queue-entry allocation 失败时，
reservation 会回滚且不签发 Accepted event。Construction 成功后，Accepted 要么与 queue
ownership 一起发布，要么在外部 shutdown 已获胜时与其关联的 Cancelled settlement 原子发布，
且 callback 不会进入。I/O task 会保留 prepared Graph transaction，graph-state policy owner 则
等待 typed completion 并应用测得的 I/O time。CPU compute worker 不能执行该等待。因此
rejection、cache codec、filesystem 或 allocation failure 仍可能让该 `ComputeRun` 失败，并保持
live Graph/RT state 不变。这种顺序是当前行为，不表示 cache 属于 user-output commit。
[ADR 0009](../../adr/zh/0009-compute-io-durability-and-completion-semantics.zh.md)
接受不同的目标顺序：cache persistence 在 Run publication 后拥有独立 typed outcome。

## 缓存命令

| 操作 | 效果 |
| --- | --- |
| Clear drive cache | 在 POSIX 上删除磁盘缓存目录内容并重建根目录；Windows 非空 root 请求在 revision、coordination 或 filesystem mutation 前被拒绝。 |
| Clear memory cache | 清理 `GraphModel` 跟踪的内存 HP 缓存。 |
| Clear cache | 在 POSIX 上同时清理磁盘和内存缓存；Windows 非空 root 请求在两层 cache 改变前被拒绝，空 root 仍会清理 memory。 |
| Clear derived image statistics | 通过内部 cache-service API 移除保留的 statistics result；不会隐式取消已经接受的 in-flight work。 |
| Cache all nodes | 在 POSIX 配置允许时将 complete HP 输出保存到磁盘；Windows 非空 root 请求在副作用前被拒绝，空 root 返回既有 HP-node count。 |
| Free transient memory | 清理非终点节点的内存缓存状态。 |
| Synchronize disk cache | 在 POSIX 上保存 complete HP 输出并移除 stale file；Windows 非空 root 请求在副作用前被拒绝，空 root 返回既有 HP-node count。 |

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

正式 HP `NodeOutput` 携带按规范顺序保存的 named Value。永久 `image` entry 是唯一 image
payload/allocation/readiness/revision authority；每个 declared generic entry 都是对应 non-image
Value authority，并与 `NodeOutput::data` 保持分离。正式 cache 只包含这些 named Value authority，
没有 compatibility peer 或 staging field。复制正式 cache entry 会保留每个 Value 的 revision、producer、
representation、indexed storage binding 与 Ready state；provider-defined multi-buffer Value 不会
被折叠成单一 image allocation identity。

带 revision 的 generic-name vector 会参与 planned-route equality、implementation replacement 与
task-graph cache identity。Retained-memory accounting 会核算 metadata/authority vector、string
payload 与 `named_values` map node。物理 allocation byte 继续由现有 allocation/cache owner
核算，不会作为 route metadata 再次计费。

可变 dirty/tiled work 不能保留或暴露旧 authority。它会创建一个未发布的 Host binding，通过
checked grant seed 保留的 byte，并在所有 executable grant retirement 后恰好一次完成 seal。
Replacement output 与 disk decode 同样创建新 identity。RT proxy output 是带独立 HP-generation
projection version 的全新 sealed Value；它继续保持 transient，不会成为正式 cache identity
source。

Disk save 要求每个 sealed named Value，并通过 public portable artifact boundary 捕获它们。一份
canonical archive 会持久化 ordinary rich image、generic built-in Value，以及兼容的
provider-defined multi-buffer Value；可选 image-codec projection 与脱离 Value 的 parameter metadata
都不是并行 Value authority。`AllocationIdentity` 与 `ValueRevisionId` 都不会被序列化、从 path
重建或用作 persistent cache/task key。这两类 token 都是 opaque process-local runtime identity；
每次 replay 每个 archived Value 时都必然铸造新 token。

已配置 artifact path 仍为 `cache_root/node_id/location`，其中不包含带 revision 的 output-schema
component。因此，每次 read 都会携带完整 frozen image/parameter/generic shape，而不是信任 path。
Versioned sibling manifest 与 public archive 必须精确匹配该 shape；generic named Value 是普通
archive member，不再是 incompatible miss。旧 image/YAML-only entry、partial transaction、名称变更、
tampered payload、mixed generation 与 missing provider 都无法成为 hit。这样便通过一个 portable
transaction 闭合 schema replacement，同时不会让 path、auxiliary projection 或 detached metadata
成为第二个 Value identity。

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
persistence。Normal Whole computation 会替换它并派生 complete validity。Generic operation ABI v1
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

RT proxy state 使用有符号 logical HP `region_hp`，但仍只支持 image。Checked adapter 会把
一个精确内建 logical ImageRect 传入 downsample，再减去已提交 HP Value 的 data-window
origin，得到零基
pixel ROI。`RealtimeProxyGraph::NodeState::region_hp` 保留 logical Region；downscaled
proxy payload 与 `roi_rt` 保持为零基 RT storage。TensorSlice 与 Whole staged validity 不会创建 partial
downsample request。Region value 与 Tensor axis 会计入 retained-memory accounting。

## V-13 Packed Memory Cache 与 Portable Disk Boundary

正式 HP `NodeOutput` 可以通过既有 Value authority 保留完整 immutable packed FP4 Value。
普通 cache copy 会保留 descriptor、block-scale quantization、Blocked layout、精确 byte
envelope、allocation、逻辑 revision 与 Ready state。`Node::hp_region` 会独立保留精确
TensorSlice validity；两类事实都不会从缩减图像 snapshot 重建，也不会从 storage 推断。这是 runtime memory-cache retention，
不是新的 persistent identity 或 cache format。

已配置 disk mechanism 现在会通过 public artifact archive 捕获每个受支持的 named Value，但保留的
canonical `image` slot 仍必须生成已配置 ordinary-image projection。在 planned byte 被
`ComputeIoExecutor` 准入、executor callback 被创建，以及 filesystem 或 codec 工作发生前，该 image
必须为 Ready、host-readable、image-faceted、Strided、unquantized，并与所选 codec 的显式
whole-byte storage set 兼容。因此 packed FP4 canonical image 会以
`GraphError{InvalidParameter}` 失败。只有 public artifact capture 与 active provider generation
验证了每项 descriptor、layout、binding 与 payload fact，image slot 之外的 generic 或
provider-defined multi-buffer Value 才会被接受。没有有效 nonempty image cache entry 的节点会保留
历史 no-op 行为，不会进入这条 validation boundary。

拒绝绝不会丢弃 metadata、widen packed byte、伪造 image facet 或静默跳过 named Value。当前
`ImageArtifactCodec` ABI 仍是 auxiliary projection boundary；public archive 与 versioned cache
manifest 拥有 portable replay。

## DI-1/DI-2 观测 Statistics Cache 边界

Issue #129 定义有界观测 min/max 与 histogram query/result/cache-key value。DI-2 将
`ImageStatisticsStore` 安装为 `GraphCacheService` 内部有界、受 mutex 保护的 derived-result
owner。完整 key 包含有效 process-local `ValueRevisionId`、可选
`ContentDigest`、精确 normalized `RegionSet`、恰好一个稳定 `ChannelId` 或
`ChannelGroupId`、算法、正算法版本与有界算法参数。不同 revision、content identity、
Region、selection、算法、版本或 histogram 参数对应不同派生请求。

每个 accepted task 会保留精确 Ready image Value，只通过 `ImageView` 扫描、验证 result，并在
single publication 之前仲裁 cancellation。注入的内部 scheduler 恰好一次接收一个 task，可以
inline 或 asynchronous 执行；store 不拥有 worker 或 execution policy。精确 cache hit 会直接返回
ready future，而不调度 task。scan failure 或 cancellation 不发布 entry。确定性的 oldest-entry
eviction、精确 revision invalidation 与显式 clear 只影响 derived data；已经接受的 in-flight task
在请求未被显式取消时，仍可在 clear 后发布。

Statistics 不是 `Value`、`ImageFacet`、正式 HP cache entry、descriptor/content identity、
disk-cache path 或 artifact manifest 的字段。创建、重新计算或驱逐 result 不能修改 Value
revision、canonical digest、HP validity 或持久表示。store 使用完整 key；allocation identity、
graph revision、HP/RT generation 或 descriptor digest 本身都不充分。
Content digest 是可选的，因为有效 runtime revision 可能在请求 content traversal 前就被观测。

### 当前有界机制与未来持久化关系

[ADR 0008](../../adr/zh/0008-generic-values-memory-bindings-and-regions-are-explicit-versioned-contracts.zh.md)
把未来 persistence 分成 graph document、canonical descriptor envelope、artifact/cache
manifest 与 chunk，以及绝不持久化的 runtime state。`DescriptorDigest`、`ContentDigest`、
`StorageLayoutDigest`、`ArtifactId` 与 `ValueRevisionId` 回答不同 identity 问题；
device/allocation identity、fence、lease、access plan 与 residency replica 永远不会进入持久
logical content identity。
当前 V-3 的 process-local `ValueRevisionId` 是 runtime publication identity，不是任何未来
canonical descriptor、content、layout 或 artifact digest。

DI-4 定义、当前实现也已使用上文描述的 public named-Value archive 与 versioned manifest，且不会
改变正式 cache authority。HP output 仍是唯一正式可复用 memory cache，RT proxy
output 继续保持 transient；注入的 image/metadata codec 仍分别是 optional projection 与 detached-
parameter implementation boundary。Residency replica、projection、manifest path 或 persisted runtime
identity 都不会成为第二个 cache authority。

V-15 不会改变这套 cache format 或 authority。其可选 OpenEXR deep adapter 可以在 caller 选择的
path 读取或写入一个 provider-defined Value，但两项操作都不是 graph-cache load/save、
manifest/chunk transaction 或正式 HP publication。Descriptor、storage-layout 与 provider
选择的 ContentDigest 继续作为通用 semantic identity；adapter 不会把它们提升为 cache key、
path、receipt 或 durability evidence。Provider replacement 与 read lease 只保护 interpretation
lifetime。Eligibility、overwrite/commit policy，以及后续任何 cache 或 output outcome，均由 caller
而不是 provider 或 `ComputeIoExecutor` 拥有。

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
- `src/lib/core/{sample_conversion,value_artifact}.*`
- `src/lib/adapters/{opencv,openexr}/`
- `include/photospider/data/packed_dense_tensor_view.hpp`
- `include/photospider/memory/blocked_layout.hpp`
- `include/photospider/data/region.hpp`
- `src/lib/core/region.*`
- `src/lib/core/region_image_adapter.*`
- `src/lib/graph/graph_cache_service.*`
- `src/lib/execution/device/compute_io_executor.*`
- `src/lib/adapters/openexr/openexr_deep_scanline_adapter.*`
- `tests/integration/test_openexr_deep_scanline_provider.cpp`
- `src/lib/graph/graph_model.*`
- `src/lib/runtime/kernel_compute.cpp`
- `src/lib/ipc/output_store.*`
- `src/lib/compute/request/compute_cache_policy.*`
- `src/lib/compute/dispatch/compute_node_task_runner.*`
- `src/lib/compute/dispatch/compute_task_dispatcher.*`
- `src/lib/compute/dirty/realtime_proxy_graph.*`
- `src/lib/compute/dirty/dirty_write_buffers.*`
- `tests/integration/test_cpu_dense_tensor_image_operation.cpp`
- `tests/integration/test_packed_fp4_dense_tensor.cpp`
- `tests/unit/test_region_contracts.cpp`
- `tests/integration/test_disk_cache_diagnostic_concurrency.cpp`
- `tests/unit/test_compute_io_executor.cpp`
- `tests/integration/test_kernel_contracts.cpp`
- `tests/integration/test_compute_service_split.cpp`
- `tests/integration/test_host_adapter.cpp`
- `tests/integration/dependency_disabled_install_smoke.py`
