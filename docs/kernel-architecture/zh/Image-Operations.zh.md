# 图像算子

默认 registry 注册了带 legacy `Value` callback 的图像和蒙版 key。注册这些算子不代表 workflow 可以绑定 packed image，或将 structural `PlanarImageLayout` 传给这些算子。没有 planar layout 的声明若带 `photospider.image` facet，会被输入校验以 `InvalidArgument` 拒绝，错误为 `image declaration requires planar layout`。若算子的 `OperationTraits::planar_storage_capable` 为 false，compiler 也会拒绝 structural planar image；下列 legacy 图像算子均未设置此能力。Operation plugin C ABI 11 与 planar extension ABI 3 是不同接口，不会改变这些检查。

## 已注册的图像 Value 算子

| Key | Legacy Value 输入与输出 | Region 行为 |
| --- | --- | --- |
| `image.exposure_gain`、`image.opacity` | `RgbaFloat32` 图像与 Float32 scalar；输出保留图像 schema | Elementwise |
| `image.mask` | `RgbaFloat32` 图像与 Float32 mask；输出图像 | Elementwise |
| `image.source_over` | 前景和背景 `RgbaFloat32` 图像；输出图像 | Elementwise |
| `image.mix` | 两个 `RgbaFloat32` 图像与 Float32 mask；输出保留首图像 schema | Elementwise |
| `image.downsample_box`、`mask.downsample_box` | 图像或 mask 与范围 `[1,16]` 的必需 Int64 `factor`；输出空间尺寸缩小 | Shrink |
| `image.brush_circle` | 图像及圆心、半径、颜色、alpha 顺序排列的 Float32 scalar 输入 | Elementwise |
| `image.stmap` | 图像和坐标 map Value | Dependency |
| `image.split_horizontal` | Typed Float32 rank-3 图像与必需 Int64 `split_x`；端口为 `full`、`left`、`right` | Dependency，可选 joint 执行 |
| `image.local_inpaint_navier_stokes_native_apple_silicon`；可选 `image.local_inpaint_navier_stokes_openCV` | 图像、mask 和算子设置 | Whole |

这些已注册 callback 使用 `Value` 存储，并按端口 schema 接受的 descriptor、facet、stride 和 Region 处理数据。它们没有实现 planar 页面访问。图像 callback 都是 `planar_storage_capable=false`，包括 `image.exposure_gain`、`image.opacity`、`image.mix` 和 `image.split_horizontal`。Workflow 若声明 packed `photospider.image` 输入，会在输入校验阶段返回 `InvalidArgument`；structural planar 声明进入算子 capability 检查后，会在编译阶段返回 `TypeMismatch`。

对 legacy RGBA Value，`image.exposure_gain` 使用 `[0,16]` 内的 Float32 gain 乘 RGB 并复制 alpha；`image.opacity` 使用 `[0,1]` 内的 Float32 opacity 乘全部四个 channel。`image.mask` 将四个前景 channel 乘匹配的 Float32 coverage 样本。对已关联的前景和背景，`image.source_over` 计算 `F + B * (1 - F.alpha)`。`image.mix` 对四个 channel 计算 `(1-M)A+MB`，并保留首图像 semantic facet。Box downsample 算子要求 `[1,16]` 内的 Int64 factor；输出尺寸是输入尺寸除以 factor 后向上取整。`image.brush_circle` 按 x、y、radius、red、green、blue、alpha 顺序接收 scalar；中心和 RGB 均为有限 Float32，radius 为正 normal Float32，alpha 位于 `[0,1]`。`image.split_horizontal` 要求 `0 < split_x < W`；其 `full`、`left`、`right` 输出分别将 `(y,x,c)` 映射到来源 `(y,x,c)`、`(y,x,c)` 和 `(y,x+split_x,c)`。每个端口有自己的输出坐标和 dependency 请求。Joint execution 可为被请求的端口共享传输，但不会授予 planar 存储访问能力。

独立构建的 [`rgba32f` C module](../../../plugins/ops/rgba32f) 提供自己的 legacy operation callback 和可选 Metal shader。该包通过 operation ABI 11 作为可信原生代码加载。它不导出 planar operation extension；使用 C 实现或通过 Metal dispatch 都不会增加 planar 访问能力。GPU 执行需要匹配的 module、所选 backend 和可用设备。

## Planar 图像执行边界

`PlanarImageLayout` 用不同物理存储表示各 channel plane，同时保留逻辑 tensor shape。上表默认图像 callback 未声明 compiler 消费此结构布局所需的 capability。当前公开 planar 数据、Region 和分配行为见[张量存储与区域访问](../../kernel-specs/zh/Tensor-Storage-and-Region-Access.zh.md)。`test_planar_image_workflow` 检查内部 planar copy 路径，不证明这些内建图像 key 支持 planar。

只有 workflow 提供端口 schema 接受的 legacy Value 表示时，才能调用相应 legacy Value 路径。若要在公开 planar 存储上执行图像工作，应组合明确声明 planar capability 且实现 planar callback 的算子。Registry 注册项、共同的 `image.*` 命名、semantic facet 和 Metal 实现均不能替代该声明。

## 检查与示例

[`examples/multi_output_workflow`](../../../examples/multi_output_workflow/README.zh.md) 可以构建，但运行 split fixture 时，将 packed image facet 加入 workflow input declaration 会失败。其 README 说明该示例不是当前运行验收。[`examples/s3_image_workflow`](../../../examples/s3_image_workflow/main.cpp) 是另一个源码示例，CMake 未为它注册 focused CTest target。`test_basic_operations` 不测试 `image.mix`。

```sh
cmake --build build --target photospider_multi_output_workflow test_multi_output_execution -j 8
ctest --test-dir build -R '^test_multi_output_execution$' --output-on-failure
```

`test_multi_output_execution` 使用测试定义的算子校验 named-output 宿主基础设施，不验证 `image.split_horizontal` 对 planar 输入的运行。可选 OpenCV 和原生 GPU 路径需要各自的构建与 backend 证据。
