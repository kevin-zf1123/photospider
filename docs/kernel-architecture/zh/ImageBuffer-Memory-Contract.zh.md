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
也携带 `PixelRect`；私有 `OutputTile` 在把 task-owned output storage 接到 adapter 时使用可变
`ImageBuffer*`。OpenCV adapter 只能在创建 matrix view 时局部转换该 rectangle，不会通过这些
value 保留或返回 `cv::Rect`。它们不会跨越公共 operation 或 Host contract。

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
2. `NodeOutput::image_value` 有效时，private runner 直接复用该 sealed Value；否则，
   compatibility adapter 会把旧 `ImageBuffer` snapshot 到新的 sealed allocation，保留
   `[height, width, channels]`、`step` 与 active row，并独立初始化 padding；
3. pure inference 只接收 deep-owned effective parameter snapshot 与 logical
   DenseTensor/Image descriptor，不接收 Node output/cache state；
4. execute 接收 checked ImageView，遵循显式 x/y/channel stride，并返回独立拥有的 padded
   sealed unsigned-8 Value；
5. runner 将完整 result descriptor 与 Image Facet 和 inference 比较，要求可由当前 adapter
   处理的 interleaved layout，把该精确 result allocation/revision 发布为
   `NodeOutput::image_value`，再为当前 ABI、tiled-write、codec 与 Host 边界派生独立拥有的
   compatibility `ImageBuffer` snapshot。

Malformed caller input 映射为 `GraphErrc::InvalidParameter`；unsupported 或 mismatched
operation result 映射为 `GraphErrc::ComputeError`；`std::bad_alloc` 原样传播。这项 bridge
与 compatibility adapter 只在 source tree 内部使用。Operation ABI v2 与尚未转换的其他
operation 继续使用 ImageBuffer；私有正式 HP cache 会保留两种表示，并把有效 sealed Value
作为 allocation/revision identity authority。

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
Metal operation 路径独立拥有 backend-specific object。直接解释 `context` 属于后端特定行为，
不是可移植内存契约。

## 边界与原理

`ImageBuffer` 是当前二维图像 payload 和 operation DSO 契约。其 channel count 在结构上不限制为
四，`FLOAT64` 也是已声明 scalar type，但这些事实不承诺每个 loader、operation、cache 或
adapter 都提供端到端支持。

该 payload 不是通用 graph value 模型。Operation result 会把具名非图像 value 保存在单独的
data map 中；这些 value 与 opaque backend `context` 都不会让 `ImageBuffer` 变成任意 payload
carrier。新增通用 value kind、rank/shape model、descriptor、handle 或 region 必须经过独立的
带版本设计。

当前限制必须明确：

- built-in operation 可能只实现部分 1/3/4-channel conversion，或假设 RGBA role；
- 部分 operation 和 image-loading 路径使用 float32 计算；
- FP4 无法表示，因为 scalar size 和 row addressing 假设每个 channel element 占整数个 byte；
- rank、N 维 shape/stride、quantization、named channel role、Deep Image sample 和 vector object
  均未表示；
- `context` 不能替代 planning、cache key、ROI 或 synchronization 所需的 descriptor fact。

因此，8/16 通道图像和 FP64 不能被宣传为完整 framework contract；FP4、latent Tensor、
Deep Image 和 vector-scene value 不受 `ImageBuffer` 支持。通用 `Value`、descriptor、handle 和
region 方向记录在精确的
[通用数据与 Region 目标](../../roadmap/zh/Kernel-Evolution.zh.md#通用数据与-region)中。

### 已实现的 V-3/V-4/V-6 关系与剩余目标

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

V-4 新增已安装、dependency-neutral 的 Region MVP。精确内建 ImageRect 可以经过 checked
conversion 进入或离开 `PixelRect`；TensorSlice、Whole、custom domain、multi-atom clause、
uncertainty 与 overflow 都会在该 adapter 被拒绝。Region-aware core dense operation 会复制
未选中的 byte，并通过 checked stride 只修改选中的逻辑 coordinate。在各自后续切片完成迁移
之前，ImageBuffer structure、device field、operation DSO ABI、tiled write、codec 与 Host/IPC
v2 rectangle 仍是角色区分明确的 compatibility contract。对于同时携带两种形式的正式 CPU
image cache entry，有效 sealed Value（而不是 mutable compatibility snapshot）才是
allocation/revision identity authority。

V-6 在不修改 `ImageBuffer` 的前提下为 Value 新增 `ReadyFence`。同步 CPU Value 初始即为
Ready；pending Value 保留 immutable metadata，但 `buffer_handle()` 与 checked view 会在
Ready 前拒绝 payload access。Source-private producer 会在每次 terminal state 前退役其
mutable capability，而 source-private transfer task 只通过 executor-queued work 复制一份
独立 CPU allocation。

V-6 仍不实现 quantization、device identity 或 registry、通用 access/visibility planning、
residency、bidirectional device transfer、stale completion arbitration、provider ABI v3 或
通用命名 graph Value output。

可移植 CPU allocation guarantee 仍是 64-byte row-start alignment；128-byte alignment 不属于
当前契约。

把不可变 descriptor 与可写 payload view 分开，可以防止并行 tile callback 竞态替换 ownership
或 device metadata。让当前 image-only `PixelRect` view 与 Region 保持区分，也能避免私有
OpenCV geometry 或 TensorSlice reinterpretation 进入 operation ABI。

## 实现与验证入口

- `include/photospider/core/image_buffer.hpp`
- `include/photospider/memory/buffer_handle.hpp`
- `include/photospider/memory/ready_fence.hpp`
- `include/photospider/data/value.hpp`
- `include/photospider/data/image_view.hpp`
- `include/photospider/data/region.hpp`
- `include/photospider/memory/strided_layout.hpp`
- `include/photospider/plugin/op_contract.hpp`
- `src/lib/core/image_buffer.cpp`
- `src/lib/core/pending_value.hpp`
- `src/lib/core/value.cpp`
- `src/lib/execution/value_transfer_task.*`
- `src/lib/core/value_image_adapter.*`
- `src/lib/core/region.*`
- `src/lib/core/region_image_adapter.*`
- `src/lib/core/cpu_dense_image_operation.*`
- `src/lib/compute/image_buffer.hpp`
- `src/lib/adapters/opencv/buffer_adapter_opencv.*`
- `src/lib/ipc/output_store.*`
- `tests/unit/test_image_buffer_contracts.cpp`
- `tests/integration/test_compute_service_split.cpp`
- `tests/unit/test_region_contracts.cpp`
- `tests/integration/test_stride_aware_compute_paths.cpp`
- `tests/integration/test_ipc_daemon.cpp`
- `tests/integration/test_cpu_dense_tensor_image_operation.cpp`
