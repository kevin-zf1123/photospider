# 多输出算子

包 0.9 / operation ABI 9 提供编译时确定的命名结果。Workflow 边通过
`WorkflowNodeOutput{node, port}` 选择端口，根使用 `WorkflowOutput{name, node, port}`。
这些公开 API 已实现。未请求的纯端口不会执行。`ExecutionOptions::enable_joint`
控制可选 CPU 物理优化，不改变数值语义。

## `color.rgb_to_ycbcr420`

输入为 Float32 HWC Image，使用 linear sRGB D65 语义，无 alpha。通道角色允许
重新排列，保留 scene/display reference。每个实际读取的 RGB 样本必须有限且在
[0,1]。没有参数，也不隐式转换范围或 alpha。命名 Float32 输出：

| 端口 | shape | 角色 | 名义源像素中心（Y,X） | 步长（Y,X） |
| --- | --- | --- | --- | --- |
| `y` | `{H,W}` | luma Y′，[0,1] | `{0,0}` | `{1,1}` |
| `cb` | `{ceil(H/2),ceil(W/2)}` | 有符号蓝色差，名义 [-0.5,0.5] | `{0.5,0.5}` | `{2,2}` |
| `cr` | `{ceil(H/2),ceil(W/2)}` | 有符号红色差，名义 [-0.5,0.5] | `{0.5,0.5}` | `{2,2}` |

线性样本 L 在小于 0.018 时使用 `4.5 L`，否则使用
`1.099 L^0.45 - 0.099`。转换后的 RGB 使用：

```text
Y′ = 0.2126 R′ + 0.7152 G′ + 0.0722 B′
Cb = (B′ - Y′) / 1.8556
Cr = (R′ - Y′) / 1.5748
```

系数与 transfer 依据 [ITU-R BT.709-6](https://www.itu.int/dms_pubrec/itu-r/rec/bt/r-rec-bt.709-6-201506-i!!pdf-e.pdf)。
本项目 420 契约在 transfer 和矩阵计算后，对居中的 2×2 色度块按行优先 binary64
运算求均值。右侧或下侧奇数边缘仅平均有效样本，最终存储 Float32。没有 studio-range
整数偏移或缩放。

Y 读取一个完整源像素；每个色度 observation 读取自己的有效 2×2 源块。Data 与
Validation 关联按输出保留。联合执行复用就绪输出之间的转换后像素，各成员仍执行
自己的授权读取和范围检查。输出可以请求不同 Region 或独立作为下游输入。

`SemanticKind::ImagePlane` 表示 Float32 HW 颜色平面。规范 `photospider.semantic`
v1 payload 包含平面角色、BT.709 transfer、sRGB 原色、白点、reference 及 Y,X 顺序的
`plane_origin`/`plane_step`。采样位置是名义元数据，精确源支持由依赖证书表示。
既有 image-v2 继续要求完整像素 Float32 HWC；平面元数据不会使部分 HWC 通道成为
合法 image。

## `image.split_horizontal`

输入为 Float32 HWC Image，必填 Int64 参数 `split_x` 满足 `0 < split_x < W`，
没有默认切分位置。输出保留源颜色、alpha 和通道解释：

| 端口 | shape | 源映射 |
| --- | --- | --- |
| `full` | `{H,W,C}` | `(y,x,c)` |
| `left` | `{H,split_x,C}` | `(y,x,c)` |
| `right` | `{H,W-split_x,C}` | `(y,x+split_x,c)` |

各输出在自己的坐标中接受独立的完整像素 Region。分阶段读取精确声明映射后的源
像素。发布的 Value 是不可变视图，具有检查过的 origin-relative layout 并保留源
storage owner；收集成稠密结果时可能复制视图。联合成员在需求相等时共享源传输，
同时保留独立偏移证据。参数与尺寸减法在读取样本前验证。

集成测试请求三个不同偏移 ROI，在 joint 开关两种模式下检查源值、facet 和 owner
身份，验证 dirty 映射，并拒绝负数、零及超出宽度的切分位置。

## `image.convolve_channels` 与区域化 `field.convolve`

`image.convolve_channels` 按顺序接收 Float32 RGB Image 和 R、G、B 三个普通
Float32 HW kernel。RGB 角色可以在存储中重新排列。各存储 RGB 通道独立滤波，
不输出 alpha，也不隐式去预乘。`r/g/b` 输出为 Float32 HW ScalarField，保留对应
relative 通道角色。每通道必填 Int64 `{r,g,b}_anchor_y`、`{r,g,b}_anchor_x` 和
String `{r,g,b}_boundary`（`zero` 或 `clamp`），没有默认值。Anchor 必须位于
自己的 kernel 内。支持奇数、偶数及非对称 kernel，不添加归一化或 bias。

`field.convolve` 保留公开输入顺序、同 dtype Float32/Float64 HW kernel，以及
必填 `anchor_y`、`anchor_x`、`boundary` 参数；现在支持分阶段区域请求，也接受
ImagePlane 输入。输出为普通 HW field。`field.correlate` 保留现有 Whole 实现。

两条卷积路径均按固定 kernel 行优先顺序、binary64 累加计算
`sum K[ky,kx] * I[y+anchor_y-ky,x+anchor_x-kx]`。Zero 在外部补零，clamp 重复最近
有效边缘。输入、系数、中间值和最终 dtype 转换必须有限且可表示。每输出声明自己
完整的 kernel 及精确裁剪后的源邻域；image-v2 仍读取完整源像素。Data/Validation
角色明确，不读取兄弟 kernel 样本，因此修改 G kernel 只使 G 失效，R/B 保留缓存。
独立请求某通道时，不相关 kernel 的非法样本不会使该请求失败。

集成测试使用独立奇偶非对称 kernel 和 anchor，对照标量 oracle 检查两种执行模式，
仅改变 G kernel 并验证 3×4 图像有 24 个 R/B cache hit，同时验证有限 field ROI
不会读取远处 NaN。既有基本卷积结果由 `test_basic_operations` 覆盖。

## 当前可运行验证

```sh
cmake --build build/issue257-static --target test_multi_output_ops -j 8
ctest --test-dir build/issue257-static -R '^test_multi_output_ops$' --output-on-failure
```

公开 API 集成测试构造 3×5 RGB workflow，对照独立 long-double oracle 检查三端口，
比较 joint/singleton 位值，检查奇数边缘与 dirty 支持，单独请求 Y，拒绝非法样本域
及 alpha，并验证通道顺序和 reference 元数据。安装后的四场景示例由 M10（#313）交付。
