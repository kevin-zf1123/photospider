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

可移植 CPU allocation guarantee 仍是 64-byte row-start alignment；128-byte alignment 不属于
当前契约。

把不可变 descriptor 与可写 payload view 分开，可以防止并行 tile callback 竞态替换 ownership
或 device metadata。公共 view 使用 `PixelRect`，也能避免私有 OpenCV geometry 成为 operation ABI
的一部分。

## 实现与验证入口

- `include/photospider/core/image_buffer.hpp`
- `include/photospider/plugin/op_contract.hpp`
- `src/lib/core/image_buffer.cpp`
- `src/lib/compute/image_buffer.hpp`
- `src/lib/adapters/opencv/buffer_adapter_opencv.*`
- `src/lib/ipc/output_store.*`
- `tests/unit/test_image_buffer_contracts.cpp`
- `tests/integration/test_compute_service_split.cpp`
- `tests/integration/test_ipc_daemon.cpp`
