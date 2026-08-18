# ImageBuffer 内存契约

`ImageBuffer` 是公共 operation 与 Host value 契约。Operation、plugin、adapter、cache code、
Host implementation 和调试工具可以依赖本文档中的字段和不变量；scheduler contract 不检查
image payload。

## 结构

| 字段 | 含义 |
| --- | --- |
| `width` | 图像宽度，单位像素。 |
| `height` | 图像高度，单位像素。 |
| `channels` | 每像素通道数。 |
| `type` | 通道数据类型。 |
| `device` | 权威数据所在位置。 |
| `step` | 行步长，单位字节。 |
| `data` | CPU 可访问的数据 owner 或 view。 |
| `context` | 后端特定资源 owner 或 handle。 |

公共 tile 契约使用 `InputTileView` 与 `OutputTileView`。二者都携带借用的
`const ImageBuffer*` 和 backend-neutral `PixelRect`。Input view 是只读的；output view 允许
adapter 从保留的 payload 暴露可写像素，但 callback 不能替换 descriptor dimension、device
identity、payload ownership 或 backend context。公共 `TiledOperation` callback 以
`const OutputTileView&` 接收 output，并以 `OperationTileInputView` value 接收 input。

私有 compute 层另行使用 `InputTile`、`OutputTile` 与 `TileTask`。这些只属于 backend 的 value
也携带零基 storage-relative `PixelRect`；私有 `OutputTile` 在把 Host-owned output storage
接到 adapter 时借用 immutable `DenseImageOutputPlan` 与 active
`HostOutputWriteGrant`。Grant Region 则使用 plan 的有符号 logical data-window domain。
OpenCV adapter 会先通过 checked arithmetic 加上 plan origin，再把 ROI endpoint 与 grant
比较，但 byte offset 与 matrix construction 仍使用零基 ROI。它不会通过这些 value 保留或返回
`cv::Rect`。它们不会跨越公共 operation 或 Host contract。

## CPU 缓冲区契约

对于内核拥有的 CPU 缓冲区：

- `device == Device::CPU`。
- 图像尺寸非零时，`data != nullptr`。
- `step >= width * channels * bytes_per_channel(type)`。
- `step` 可以包含 padding。
- 基指针必须 64 字节对齐。
- 每一行起点必须 64 字节对齐。

每一行起点对齐意味着：

```text
address(row y) = data + y * step
address(row y) % 64 == 0
```

因此，内核拥有的分配必须对 `step` 做 padding，使行起点即使在紧凑行大小不是 64 的倍数时仍保持对齐。

这些对齐和可变性规则只适用于由内核分配并拥有的 CPU buffer。`ImageBuffer::data` 是共享生命周期
handle，而不是通用的可写内存承诺。Producer 可以返回只读 CPU snapshot，但必须记录这一边界。

Installed IPC Host 的 `compute_and_get_image` 结果就是这类 snapshot。它会在 delivery lease 保护
result-to-open 的期间校验同用户 private artifact，再使用 `PROT_READ|MAP_PRIVATE` 映射精确的
tight-row 文件。Mapping base 具备平台 page alignment，但 `step` 是 packed row width，因此后续
row start 不承诺满足 64 字节对齐。Descriptor 副本共享同一个 mapping；最后一个引用会且只会
unmap 并 close 其保留的 descriptor。通过该 mapping 写入不属于契约，且可能触发 fault。需要可写
storage 或内核所有的逐行对齐时，consumer 必须分配适当的 CPU buffer，并使用 `step` 逐行复制。

## ARM Mac 对齐

64 字节行对齐是当前可移植最低要求。该契约在 ARM Mac 或其他平台上都不提供 128 字节保证。

## 步长感知访问

消费者遍历行时必须使用 `step`。不得假设缓冲区紧密排列。

正确的行访问模式：

```cpp
auto* row = static_cast<unsigned char*>(buffer.data.get()) + y * buffer.step;
```

错误假设：

```cpp
auto* row = base + y * width * channels * bytes_per_channel;
```

OpenCV 适配器必须通过使用提供的 `step` 构造 `cv::Mat` 来保持步长。

## 内核 CPU Buffer 原语

当前仅依赖标准库的 operation runtime 拥有以下最小 CPU buffer 原语：

- `validate_image_buffer()` 校验已声明 enum、规范空状态、非空 descriptor 的正尺寸、
  shared owner 一致性、CPU payload 要求、packed-row stride，以及 descriptor byte
  算术是否可表示。Opaque backend 的 allocation capacity 仍由 provider 负责。
- `image_buffer_row_bytes()` 计算不含 padding 的 active packed-row byte；
  `image_buffer_row_data()` 则通过 `step` 返回只读 CPU row。
- `fill_image_buffer_region()` 只填充 `OutputTileView` 的 active byte。ROI 之外的 pixel
  与 row padding 保持不变。
- `copy_image_buffer_region()` 逐行复制 shape 和 format 相同的
  `InputTileView`/`OutputTileView` region。当 payload 可能 alias 时，它会在第一次写 destination
  前完整快照 source 的 active byte，因此重叠 view 具有 value-copy 语义。经证明互相独立的
  payload 会在 validation 后直接复制；validation/allocation 失败不会改变 destination。

每次调用只借用 tile view；这些原语不会保留 descriptor、添加同步、推断 backend mapping，
也不会把 producer 提供的只读 snapshot 变成可写内存。Row/pixel access 要求非空且拥有 CPU
payload。由于 `shared_ptr` 不公开 allocation capacity，producer 必须保证 storage 覆盖每一条
已声明 active row。合法的 context-only 或 non-CPU descriptor 会在不解引用的情况下被拒绝。
Copy 与 fill 会在修改前完整校验 descriptor 和 ROI，并且绝不把 padding 当成 pixel。

当前 tiled `image_mixing` 的 crop/pad normalization 会组合 aligned allocation、zero fill 与
region copy。Shape 完全匹配的 input 继续作为 descriptor 透传。Resize 与 channel conversion
仍仅在真实 algorithm call 处使用 OpenCV，并返回保留结果的 `ImageBuffer`。Compute metrics
recorder 不再创建或 reshape `cv::Mat`：启用 timing statistics 时，它会通过 `step` 遍历 active
CPU scalar value、排除 padding，并记录 range/non-finite diagnostic。Active payload 全为 NaN
时，继续保留此前正/负无穷的 empty-range sentinel。Opaque non-CPU resource 继续保留 provider
提供的 diagnostic，因为只有对应 device adapter 可以映射它们。

## V-3 Value-backed CPU Ownership Bridge

dependency-neutral operation runtime 现在也实现 installed `Value`、`DenseTensorView` 与
`ImageView` 的 CPU 子集，以及 installed `BufferHandle` 与 `ValueBuilder`。`BufferHandle`
是在一个 process-local allocation identity 上受检、非空的 byte range。Consumer 获取会保留
所有权的 `ReadLease`；只有 `ValueBuilder` 能获取唯一 move-only `WriteLease`。Builder 在该
lease 仍存活时拒绝 seal，并在以全新 process-local `ValueRevisionId` 发布 immutable byte 后
撤销后续写权限。

Concrete shape 与 element fact 和 optional explicit Image Facet、`StridedLayout` 保持分离。
Producer layout 要求 byte offset 为零且使用精确 envelope 的正 stride。在 allocation 或
WriteLease 之前，builder 会按 byte stride 递增顺序处理所有非 singleton axis。受检
covered span 从 element byte width 开始；每个后续 stride 必须不小于该 span，随后才把本
axis 的贡献加入 span。这项按 rank 通用的归纳证明无需枚举 element，即可证明 writable
coordinate slab 不重叠。Singleton axis 不增加备选地址，零 extent 仍是无效 descriptor。
Contiguous、padded、transposed 及其他 axis-permuted layout 会通过；单轴或跨轴 byte
collision，以及不可表示的 span 算术，会在 authority 逸出前失败。通过 sealed handle
构造的 immutable Value 则可以使用受界限约束的 byte offset 以及正、零或负 signed byte
stride。Checked view 同时保留完整 Value 和 read lease，且不暴露 writable pointer。

内建 `image_process:invert_dense` operation 在生产路径证明这项 surface：

1. 当前 monolithic callback 校验一个非空 CPU 图像 input；
2. private runner 要求规范的 named `image` Value，绝不回退到正式 compatibility storage；
3. pure inference 只接收 deep-owned effective parameter snapshot 与 logical
   DenseTensor/Image descriptor，不接收 Node output/cache state；
4. execute 接收 checked ImageView，遵循显式 x/y/channel stride，并返回独立拥有的 padded
   sealed unsigned-8 Value；
5. runner 将完整 result descriptor 与 Image Facet 和 inference 比较，并把该精确 result
   allocation/revision 发布为 `NodeOutput` 的 named `image` Value。当前 external consumer
   只在其显式 adapter 处派生 use-scoped compatibility snapshot。

Malformed caller input 映射为 `GraphErrc::InvalidParameter`；unsupported 或 mismatched
operation result 映射为 `GraphErrc::ComputeError`；`std::bad_alloc` 原样传播。这项 bridge
与 compatibility adapter 只在 source tree 内部使用。Operation ABI v2 与尚未转换的其他
operation 继续使用 ImageBuffer；入站 result 会在 private return 前完成规范化，正式 HP cache
只存储 sealed named Value。

## DI-2 Host 所有的输出授权

DI-2 不新增 public `ImageBuffer` 字段，也不改变 operation ABI v2。它在 allocation 前冻结一个
source-private `DenseImageOutputPlan`。该 plan 持有规范 output name、完整
DenseTensor/ImageFacet metadata、正向 interleaved Strided layout、精确 storage envelope、
要求为二次幂的 alignment 与完整 image Region。所有 extent、stride、row-span、offset、
alignment、range 与 overflow 检查都在 producer 可以看到 mutable byte 前完成。

`HostOutputBinding` 通过 `ValueBuilder` 恰好 allocation 一次，并保留唯一 builder
`WriteLease`。Producer 只能获得 move-only、可撤销的 whole grant 或 tile grant。tile grant
暴露经过检查的 active row span；live grant 必须在逻辑与 byte 层面均互不重叠。invalid
overlap/range/alignment、exception、cancellation、explicit failed retirement、duplicate
retirement，或者 active 状态下析构，都会记录首个 sticky failure 并撤销所有 grant。任何 grant
仍 active 时 seal 都会失败，且不能通过重试转为成功。完成精确的成功 retirement 后，seal 会
关闭 grant issuance、撤销写访问，并以计划好的 allocation 和唯一 fresh revision 发布一个
Ready Value。第二次 seal 不能铸造另一 revision。

Tile grant authorization 与 callback geometry 有意使用两种坐标表示。
`HostOutputWriteGrant::image_region()` 是被 `ImageFacet::data_window` 包含的有符号 logical
`ImageRect`；`OutputTile::roi` 是被 plan width/height 包含的零基 storage rectangle。二者只有
经过 checked origin translation 后才描述同一批像素。Grant span offset 与 stride 保持
allocation-relative，因此 negative 或 nonzero logical origin 不会改变 storage view construction。

当前 ABI v2 tiled adapter 可以在 active grant 上创建一个 callback-local full-image
`ImageBuffer` alias，因为 v2 无法编码 row-span capability。该 alias 不会被存储、不能活过
callback return，并且是 DI-3 的具名删除边。由于 callback 返回 `void` 且只能 seal 这份
canonical image binding，所有 tiled 注册面都会在 registry mutation 前拒绝
`produces_image=false` 或任何 declared generic/parameter output。CPU monolithic result 与 codec
input 会通过 plan/binding 路径复制。Opaque non-CPU plugin result 会成为 imported external
Value binding，其 owner 保留 payload 与 DSO lifetime；它不会使 staging ImageBuffer 成为
runtime authority。

## DI-1 普通 DenseImage 坐标与解释契约

Issue #129 不向 `ImageBuffer` 添加字段。普通 image-faceted Value 现在必须具有有符号、
非空、半开的 `data_window`，可以携带独立 `display_window`，也可以携带有界稳定
channel/group、声明 sample-domain 与 color 记录。Data-window 跨度与显式 x/y tensor
axes 精确一致；负原点与非零逻辑原点合法。`ImageView::channel_data()` 仍是零基存储
索引 API，而 `channel_data_at()` 会校验有符号逻辑坐标并减去 data-window 原点。
无需 payload readiness 即可检查 bounds 元数据；两种 view constructor 仍要求 Ready、
host-readable payload。

兼容投影是有意且非对称的：

- `ImageBuffer -> Value` 创建 `[0,width) x [0,height)`，且没有 display、稳定
  channel/group、sample-domain 或 color 事实，因为 ImageBuffer 不提供它们；
- `Value -> ImageBuffer` 复制活跃 extent 与 element，但无法保留有符号原点或丰富解释；
  把该 snapshot 转回时会创建有文档说明的零原点投影；
- 纯内建 DenseImage descriptor inference 复制完整 ImageFacet，而无法编码现存元数据的
  有界 edge 会在 allocation 或 callback entry 前拒绝它，不会静默删除字段。

Storage-representable range、quantization、声明 sample domain、color 与观测 statistics
保持分离。具体而言，没有任何 ImageBuffer type 或 channel 名称可以隐含 normalized range、
RGB、alpha、transfer function 或 primaries。Provider-defined OpenEXR Deep window 仍位于
该普通 DenseImage 权威之外。

## GPU 缓冲区契约

对于 GPU 缓冲区：

- `device` 标识后端，例如 `Device::GPU_METAL`。
- `data` 可以为空。
- `context` 携带后端资源。
- 适配器定义如何上传、下载和解释后端资源。

公共契约是 `device` 加 `context` 的关系。`context` 中存储的具体对象由后端决定。

## Metal 缓冲区

Metal buffer adapter 已在 `src/lib/adapters/metal/buffer_adapter_metal.{hpp,mm}` 中实现，
但当前未在核心库构建中启用，也未接入生产
compute 路径。`CMakeLists.txt` 通过 `plugins/ops/metal/perlin_noise_metal.mm` 和
`plugins/ops/metal/metal_ops_loader.cpp` 单独构建当前 Metal operation 路径；只有当 loader
目录被手动加入 `plugin_dirs` 后，该 op 路径才会注册
`image_generator:perlin_noise_metal`。

已实现的 adapter 行为：

- 上传支持 1 或 4 通道的 `FLOAT32` 缓冲区。
- 上传后的缓冲区使用 `Device::GPU_METAL`。
- `context` 拥有 Metal texture holder。
- 下载返回新的 CPU `ImageBuffer`。

插件、调度器和核心 compute 代码不得把 Metal buffer adapter 视为生产运行边界。当前生产
Metal operation 路径不使用该 adapter，也不保留 `ImageBuffer::context` payload。Reserved start
之后，Metal Perlin provider 会借用进程 executor 的 command queue、invocation-scoped allocator
与经过校验的 pipeline cache，编码 compute 加显式 texture-to-shared-buffer blit、安装 native
completion handler、commit 且不等待，并通过 source-private 路径返回 pending host-visible
Value。`TaskSubmissionPlan` 只会在该规范 Value 进入 Ready 后释放 dependant；它不会创建
ImageBuffer peer。Provider 不拥有独立 native lifecycle，不调用 `waitUntilCompleted` 或
`getBytes`，也不能把 `ImageBuffer::context` 变成可移植内存契约。

## 边界与原理

`ImageBuffer` 是当前二维图像 payload 和 operation DSO 契约。其 channel count 在结构上不限制为
四，`FLOAT64` 也是已声明 scalar type，但这些事实不承诺每个 loader、operation、cache 或
adapter 都提供端到端支持。

该 payload 不是通用 graph value 模型。Operation result 会把 generic non-image Value 保存在
`NodeOutput::named_values`，并把 parameter result 保存在独立 `data` map；这两个类别与 opaque
backend `context` 都不会让 `ImageBuffer` 变成任意 payload carrier。新增 Value kind、
representation、descriptor、handle 或 Region 必须经过独立的带版本设计。

当前限制必须明确：

- built-in operation 可能只实现部分 1/3/4-channel conversion，或假设 RGBA role；
- 部分 operation 和 image-loading 路径使用 float32 计算；
- FP4 无法表示，因为 scalar size 和 row addressing 假设每个 channel element 占整数个 byte；
- rank、N 维 shape/stride、quantization、named channel role、Deep Image sample 和 vector object
  均未表示；
- `context` 不能替代 planning、cache key、ROI 或 synchronization 所需的 descriptor fact。

因此，`ImageBuffer` 本身不能被宣传为 8/16 通道图像或 FP64 的完整 framework contract。
V-12 验证 image-faceted 通用 Value 与 CPU Value/ImageBuffer bridge 会为 1/3/4/8/16 通道、
FP32/FP64（包括带 padding 的源 layout）保留 active logical element；它不会扩大 image-only
operation、selected-precision codec 或 Host surface。`ImageBuffer` 不表示 FP4、latent Tensor、
Deep Image 或 vector-scene value。通用 `Value`、descriptor、handle 和 region 方向记录在精确的
[通用数据与 Region 目标](../../roadmap/zh/Kernel-Evolution.zh.md#通用数据与-region)中。

### Readiness、交付与持久化彼此独立

对于已安装 Value bridge，`ReadyFence::Ready` 表示 producer access 已停止，payload 可以
进入 checked access plan。它不表示包围它的 `ComputeRun` 已提交 Graph state，也不表示
缓存文件已写入或用户可见输出已经 durable。

Readiness 也不会授权由 provider 选择的 output shape。Issue #130 要求精确 staged output 匹配
Host-frozen authority：canonical image 保留 descriptor、ImageFacet、Strided layout、identity 与
trusted-extent 校验；每个 declared generic Value 保留其精确 name、revision/producer identity、
受支持 representation/layout 与每个非空 indexed `StorageBinding`，但不要求 image facet。
Provider output 不能扩大任一 named category，也不能把 generic Value 移入 parameter data。有
监督的 native producer 可以在精确 named Value 仍为 Pending 时返回它们。Run 随后安装
non-inline、Run-scoped continuation，在不占用 worker 或释放 dependant 的情况下保持 owner
存活，并确定性地串行衔接多个 Pending name。只有全部相同精确 publication 都进入 Ready 后，
才可继续 formal commit；inline/sequential 与 direct formal dirty 路径会同步拒绝 Pending。
Failed、ProducerCancelled、cancelled、stale 或 replaced state 会在不修改 Graph/RT 的情况下
关闭；callback registration 与 retained-context drainage 会防止重复 terminal publication 或
遗留 callback owner。

当前 IPC image-result 路径会在私有 daemon `OutputStore` 中物化 tight-row CPU artifact，
调用文件 `fsync`、执行禁止覆盖的原子 rename、重新校验文件系统 identity，并返回受进程级
delivery lease 保护的 metadata。该 store 不同步包含目录，也不持久化 record/index；
lease/TTL cleanup 可以 unlink artifact。由此得到的 `ImageBuffer` mapping 是有效的自有
delivery state，不是 crash-durable output receipt。

旧 `io/save` 操作会在 provider 执行期间独立调用 `cv::imwrite`。成功返回只报告该 codec
调用；副作用可能先于 Run commit，且没有 OutputStore transaction。上述当前限制与独立目标
输出权威由
[ADR 0009](../../adr/zh/0009-compute-io-durability-and-completion-semantics.zh.md)
固定。

### 已实现的 V-3/V-4/V-6/V-8/V-9/V-12/V-13/V-14/V-15 关系与剩余目标

[ADR 0008](../../adr/zh/0008-generic-values-memory-bindings-and-regions-are-explicit-versioned-contracts.zh.md)
接受以下完整替换：

```text
ImageBuffer -> Value + ImageFacet + ImageView
DataType    -> ElementSemantics + StorageEncoding + QuantizationSchema
Device      -> DeviceBackend + DeviceId + MemoryDomain
PixelRect   -> RegionSet atom ImageRect
```

目标把逻辑 `DataDescriptor` 与物理 `StorageBinding` 分离，并且只通过已检查的
`BufferHandle` range 与 lease 访问内存。V-2 引入上述有界 operation bridge 使用的 CPU
DenseTensor descriptor、ImageFacet、positive producer `StridedLayout`、immutable byte
ownership 与 checked ImageView。V-3 新增 public checked BufferHandle range、保留所有权的
read lease、独占 builder write lease、process-local allocation/revision identity、byte offset、
受界限约束的 signed/zero-stride immutable view，以及正式 HP cache entry 中的 allocation
identity authority。

V-4 新增已安装、dependency-neutral 的 Region MVP。精确内建 ImageRect 可以在 legacy edge
直接投影为 logical-coordinate `PixelRect`。Compute/dirty storage projection 是另一项 checked
operation：它会 clip 到显式 `ImageBounds` 并减去其 origin，反向转换则在 storage containment
之后加回 origin。TensorSlice、custom domain、multi-atom clause、uncertainty 与 overflow 会被
拒绝；Whole 只允许进入带显式有限 bounds 的 storage projection。Region-aware core dense operation 会复制
未选中的 byte，并通过 checked stride 只修改选中的逻辑 coordinate。在各自后续切片完成迁移
之前，ImageBuffer structure、device field、operation DSO ABI、tiled write、codec 与 Host/IPC
v2 rectangle 仍是角色区分明确的 compatibility contract。正式 CPU image cache entry 只携带
有效 sealed Value；compatibility snapshot 仅限 use scope，永远不会成为
allocation/revision authority。

V-6 在不修改 `ImageBuffer` 的前提下为 Value 新增 `ReadyFence`。同步 CPU Value 初始即为
Ready；pending Value 保留 immutable metadata，但 `buffer_handle()` 与 checked view 会在
Ready 前拒绝 payload access。Source-private producer 会在每次 terminal state 前退役其
mutable capability，而 source-private transfer task 只通过 executor-queued work 复制一份
独立 CPU allocation。

V-8 在不向 `ImageBuffer` 新增字段的前提下加入显式 `DeviceBackend`、`DeviceId`、
`MemoryDomain`、`StorageBinding`、producer identity 与 `AccessPlan`。Value binding 可以是
host-visible 或 device-local；metadata observation 不授予 pointer，host access 会失败而不是
启动隐式 transfer。CPU/Metal transfer 为同一逻辑 revision 生成独立 binding。精确且仍属于
current generation 的 completion 会原子发布 Ready 与进程 residency；stale completion 会在
dependant 能看到 Ready 前发布 typed failure。Lineage row 会在 coordinator submission 前被
预跟踪，但此时没有 managed current identity。Accepted current publication 会在 currentness
可观察前指派精确 generation，包括 coordinate 授权的数值下降；之后启动的 stale Run
不能替换该 exact identity。Standalone lineage 另行保持 numeric-maximum ordering。
Run settlement 本身不会使合格 replica 失效；publication pressure 下，
manager 默认的 64-entry 上限会释放 revision 最低的强 native/provider owner。这个 entry
数量不是 device-byte 或 scratch admission。当前 source-private Metal 路径同时实现
buffer-to-texture upload 与 texture-to-buffer download。

Issue #102 保持该公共 `Value`/`BufferHandle` 契约与 `ImageBuffer` 不变。其源码私有
isolated-CPU adapter 只接受 Ready、Host-visible、未量化的 NativeScalar Strided
DenseTensor input，保留经检查的 read lease，并把精确 physical storage envelope 复制到一个
invocation-local、只读的 shared-memory descriptor range。`BufferHandle`、
`AllocationIdentity`、`ValueRevisionId`、pointer 与 lease 永远不会跨 wire。每个 output
capability 只授予一个已规划、为正、经检查且互不重叠的 descriptor range。Host 会把每个
physical input descriptor-range byte 纳入其 request content binding，在 child 正常退出后重新
验证返回的 capability，通过 `ValueBuilder` 进行复制，并在 seal Host `Value` 前针对实际 fresh
snapshot 验证 binding。Descriptor 可寻址的 padding 参与 content binding。Darwin 按 page
取整的 POSIX
shared-memory slack 位于所有 descriptor range 之外，但其精确 physical capability size 仍受
header 与 resource declaration 绑定。该切片不引入 `ImageBuffer` adaptation，也不扩大公共
memory contract。其当前仅支持 axis 的 image record 只接受零原点且没有
display/channel-schema/sample/color 元数据的普通 ImageFacet；丰富元数据会在 request
encoding 前失败，而不是被省略。

Issue #86 / V-9 在不修改 `ImageBuffer` 或公共 operation 与 Host 契约的前提下，新增
source-private device resource accounting。唯一 service ledger 只为 fixed registry 中具有
executor 的已配置非 CPU `DeviceId` 创建隔离的 memory/scratch account。Perlin 与
CPU-to-Metal upload 会在 allocation 前预留完整 native size/alignment plan，根据
`allocatedSize` 校准，并在 command submission 前提交精确 actual byte。Persistent memory
lease 随 native Value/residency owner 延续，scratch lease 随精确 completion 延续；Run
settlement 不释放仍被 owner 持有的 allocation，而 residency entry count 仍只是 retention
bound，不是 byte authority。

Issue #89 / V-12 现在会在不改变 `ImageBuffer` 的前提下验证通用矩阵。长期
dependency-neutral 用例覆盖 1/3/4/8/16 通道 FP32/FP64 image Value、rank-one 至 rank-five
FP32/FP64 latent Value、正向 padded producer layout、negative/zero-stride 不可变 view、
ImageRect/TensorSlice merge、显式 CPU 与注入式 external-device transfer，以及有界
compute-I/O retention。CPU/device transfer 会在不同 binding 中保留完整正向 producer envelope
与精确逻辑 revision；Region merge 保留逻辑上选中/未选中的 element，同时可以发布新的
contiguous allocation。signed/zero layout 保持为不可变 view 事实，并在作为 transfer producer
layout 时被显式拒绝。Rank-one fixture 的唯一 stride 大于 element width；独立 direct-offset
byte oracle 会证明精确 storage span、active value 与未被修改的 padding sentinel。CPU-copy 与
external-device preparation 会复用同一个 core 正向、零 offset、精确 envelope、non-overlap
validator。External rejection 发生在保留 destination owner、生成
allocation/revision/producer 事实、创建 fence、调用 provider 或发布 Pending destination 之前；
通用 native publisher 不会被收紧，仍可发布经过检查的 signed immutable alias。

Issue #90 / V-13 同样不会修改 `ImageBuffer`，而是在其旁边安装一条真实 packed path。
Four-bit E2M1 storage 与 finite-positive row-major block scale 是彼此独立的 descriptor fact；
version-1 `BlockedLayout` 携带 nibble-aligned bit stride、absolute bit offset 与显式 nibble
order。`PackedDenseTensorView` 提供 checked encoded/dequantized read；有界 TensorSlice copy
只接受完整 block-aligned selection，会直接复制 code/scale，并发布 fresh packed CPU Value。
`DenseTensorView`、`ImageView` 与 `dense_tensor_element_bytes()` 继续拒绝 Blocked FP4，而不会
假装一个 nibble 就是一个 byte。

显式 CPU 与注入式 external-device transfer 会保留 packed byte、unused nibble bit、
descriptor/quantization/layout fact 与逻辑 revision，不经过 `ImageBuffer` adaptation。正式 HP
memory cache 可以保留该 Value 与精确 TensorSlice validity。Image-only disk cache 是一条显式
compatibility boundary：packed、quantized 或 latent 正式 Value 会在 executor admission、
filesystem mutation 或 codec call 前以 `GraphError{InvalidParameter}` 失败；不会发生 widening、
metadata-only fallback 或通用 durable format 写入。

Issue #117 / V-14 同样保持 `ImageBuffer` 不变。它新增独立的 provider-defined `Value`
表示，其中 `ProviderDefinedLayout` 通过受界限约束的 buffer envelope 命名一个或多个经过检查的
`BufferHandle` range。通用 Host validation 会在调用匹配的精确 generation provider 前，证明每个
index、非零 role、offset、length 和 checked end。Provider-defined Value 只暴露带 index 的
`ProviderReadLease`；每次 read 都同时保留选中的 allocation 和 provider generation。
DenseTensor byte/view/layout accessor 与现有 transfer task 会拒绝这种表示，而不会通过
`ImageBuffer` 适配它或假定只有一个 buffer。

V-14 纯 C definition suite 只会为显式 semantic validation 与 canonical logical-content
traversal 接收 payload。Property、DataSpec 和 Region evaluation 只能看到 buffer size 和 identity，
payload pointer 均为空，并且没有 mapping、transfer、conversion、device 或 executor 权限。
原子 provider replacement 与 unload 会移除新的 interpretation visibility，而旧 Value、read 和
provider owner 会保留正在退役的 generation 与 module。当 provider 发出相同 logical stream 时，
canonical ContentDigest 会排除 physical buffer order、padding 与 offset。Artifact-envelope
serialization 会保留 metadata 和未知 extension byte，但不会创建 filesystem、cache 或
`ImageBuffer` persistence path。

Issue #118 / V-15 保持这份 memory contract 与 `ImageBuffer` 不变，同时把一个可选 OpenEXR
codec 绑定到该 contract。具体 deep Layout 会为每个逻辑 pixel 保存一个 row-major `uint32`
count、一个长度为 site-count 加一的 `uint64` prefix-offset array（首项为零，末项为 declared
deep-sample count），并在 sample 存在时为每个显式 channel identity 提供一条 tightly packed
FP32 sample buffer。
每个 offset 都必须单调且在范围内；每个 channel buffer 都必须恰好包含相同的 shared declared
sample count。有符号 data/display window 继续作为 descriptor 事实，不会变成负 storage offset。
文件 channel sampling 必须为 one-by-one，channel 名只用于诊断。依据未改变的 V-14
nonempty-envelope 不变量，shared sample total 为零时只使用 count 与 offset 两个 physical buffer；
channel identity 与 role 仍保留在 descriptor/Layout metadata 中，且不会发布 sentinel payload、
零长度 envelope 或虚假 sample identity。

Codec staging 只发生在一项已准入的 source-private adapter call 内。它在转换通用 buffer 时使用
indexed `ProviderReadLease`，不会发布 OpenEXR pointer 或 exception type，并会先通过 active
registry 构造解码后的 result 再返回。Provider generation、Value、transaction token 与 path copy
会保留到完整 I/O task 结束；它们都不会成为 public memory binding 或第二套 ownership authority。

V-15 仍不实现其他 quantization formula 或 packed format、未对齐 requantizing slice、通用
Map/Import provider、剩余 provider ABI suite、public device registry、device queue/in-flight
accounting、通用 graph/cache Value persistence、deep-tiled 或 multipart OpenEXR，或通用命名
graph Value output。Issue #87 的
compute-I/O durability 决策与 Issue #88 首条有界 cache/codec execution 垂直路径继续是当前
行为：process executor 会保留 transaction lifetime 并预算 work，但不改变 `ImageBuffer` 或
codec ABI。V-12 I/O observation 证明已准入 task 对通用 Value 的 retention，而不是 lossless
artifact format。Run publication 之后的 cache outcome 与 durable output 仍是未来工作。
`ImageBuffer` 仍是 operation ABI v2、tiled write、现有 image codec 与 Host surface 的
compatibility contract；V-15 adapter 不会让其 provider-defined Value 经过这套 compatibility
representation。

Issue #94 保持 `ImageBuffer` 与全部 installed memory contract 不变。其 source-private
progressive RT branch 使用 `exact_box_average_factor_four_region()`，从原始 2048x2048
source 创建对齐的 512x512 RGBA FP32 preview。每个 4x4 channel sum 先完成累加，再只进行一次
binary32 result rounding，并在每条退出路径恢复 caller 的 floating-point environment。首次
写入之前，source/destination 共享 owner、二者经检查的 active storage-envelope 半开
`uintptr_t` 区间重叠，或端点不可表示，都会以 fail-closed 方式被拒绝。这既覆盖同一 owner 下
起始地址偏移的别名，也覆盖 owner 不同但地址区间重叠的情况，且不会对无关指针进行关系比较。
生成的 proxy storage 被封装为不可变 rank-three HWC `Value`，具有自身 revision、binding、
allocation、`ImageFacet`、layout 与精确 storage-byte envelope。Final Value 则从原始 full-
resolution source 独立计算。

I2 Host 会对每个 visible preview/final Value 记录两次 Direct access plan，并要求复用相同
revision、binding、allocation 与 byte count，且 transfer 为零。存在已配置 Metal executor 时，
第一次 acquisition 通过进程自有 registry、residency manager 与 ledger 上传紧密步幅的 rank-three
HWC Value。原生 R32 texture 只在 Metal 边界把 channel 展平进 row width，而发布的 device Value
保持原始 descriptor、facet、layout、logical revision 与 byte envelope。第二次 acquisition
必须复用该精确 residency，不能再次 transfer 或 allocation；全程不发生 readback。缺少可用
Metal executor 时，只有 device component 为 N/A，Host 与 no-I/O evidence 不得放宽。

可移植 CPU allocation guarantee 仍是 64-byte row-start alignment；128-byte alignment 不属于
当前契约。

把不可变 descriptor 与可写 payload view 分开，可以防止并行 tile callback 竞态替换 ownership
或 device metadata。让当前 image-only `PixelRect` view 与 Region 保持区分，也能避免私有
OpenCV geometry 或 TensorSlice reinterpretation 进入 operation ABI。

## 实现与验证入口

- `include/photospider/core/image_buffer.hpp`
- `src/lib/core/image_buffer_processing.hpp`
- `src/lib/core/image_buffer_storage.hpp`
- `src/lib/core/exact_box_downsample.cpp`
- `include/photospider/core/device.hpp`
- `include/photospider/memory/access_plan.hpp`
- `include/photospider/memory/buffer_handle.hpp`
- `include/photospider/memory/ready_fence.hpp`
- `include/photospider/data/value.hpp`
- `include/photospider/data/extension.hpp`
- `include/photospider/data/image_view.hpp`
- `include/photospider/data/packed_dense_tensor_view.hpp`
- `include/photospider/memory/blocked_layout.hpp`
- `include/photospider/data/region.hpp`
- `include/photospider/memory/strided_layout.hpp`
- `include/photospider/plugin/op_contract.hpp`
- `include/photospider/plugin/data_definition_registry.hpp`
- `include/photospider/plugin/data_provider_api.h`
- `src/lib/core/image_buffer.cpp`
- `src/lib/core/pending_value.hpp`
- `src/lib/core/value.cpp`
- `src/lib/core/extension.cpp`
- `src/lib/core/packed_dense_tensor.cpp`
- `src/lib/execution/transfer/value_transfer_task.*`
- `src/lib/execution/device/metal_device_executor.{mm,stub.cpp}`
- `src/lib/compute/execution/execution_service.*`
- `src/lib/benchmark/i2/i2_host.hpp`
- `src/lib/execution/device/device_completion.*`
- `src/lib/execution/device/residency_manager.*`
- `src/lib/plugin/data_definition_registry.cpp`
- `src/lib/adapters/openexr/openexr_deep_contract.hpp`
- `src/lib/adapters/openexr/openexr_deep_scanline_adapter.*`
- `plugins/data/openexr_deep_scanline_provider.cpp`
- `src/lib/execution/device/metal_device_executor.*`
- `src/lib/core/value_image_adapter.*`
- `src/lib/core/region.*`
- `src/lib/core/region_image_adapter.*`
- `src/lib/core/cpu_dense_image_operation.*`
- `src/lib/compute/image_buffer.hpp`
- `src/lib/adapters/opencv/buffer_adapter_opencv.*`
- `src/lib/ipc/output_store.*`
- `plugins/ops/save_op.cpp`
- `tests/unit/test_image_buffer_contracts.cpp`
- `tests/integration/test_compute_service_split.cpp`
- `tests/unit/test_region_contracts.cpp`
- `tests/integration/test_stride_aware_compute_paths.cpp`
- `tests/integration/test_ipc_daemon.cpp`
- `tests/integration/test_cpu_dense_tensor_image_operation.cpp`
- `tests/integration/test_packed_fp4_dense_tensor.cpp`
- `tests/integration/test_variable_sample_field_extensions.cpp`
- `tests/integration/test_openexr_deep_scanline_provider.cpp`
