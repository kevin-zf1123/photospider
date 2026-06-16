# ImageBuffer 内存契约

`ImageBuffer` 是公共内核契约。算子、调度器、插件、适配器、缓存代码和调试工具可以依赖本文档中的字段和不变量。

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

`InputTile` 是指向上游 `ImageBuffer` 加 `cv::Rect` ROI 的只读非拥有 view。它携带
`const ImageBuffer*`，因此 tiled 算子不能通过 tile API 替换或修改上游 buffer
元数据。`OutputTile` 是面向目标区域的可写对应类型，携带可变 `ImageBuffer*`。
`TileTask` 将一个 `OutputTile` 与零个或多个 `InputTile` view 组合起来，交给 tiled
算子回调执行。

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

## ARM Mac 对齐

64 字节行对齐是可移植最低要求。ARM Mac 高性能路径可能需要或受益于 128 字节对齐。这是优化目标，应在成为更严格默认值前做基准。

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

当前 Metal 适配器行为：

- 上传支持 1 或 4 通道的 `FLOAT32` 缓冲区。
- 上传后的缓冲区使用 `Device::GPU_METAL`。
- `context` 拥有 Metal texture holder。
- 下载返回新的 CPU `ImageBuffer`。

插件和调度器代码应优先使用适配器 API 访问 Metal。直接解释 `context` 是性能敏感的逃生口，不应在没有基准证据的情况下成为必需路径。

## 基准问题

收紧 Metal 访问策略前，应基准测试：

- 16x16 和 64x64 tile 上的适配器 API 与直接 context 访问。
- copy、add 和简单 curve transform 等轻量操作。
- 上传/下载密集工作负载。
- ARM Mac 上的 64 字节与 128 字节行对齐。
