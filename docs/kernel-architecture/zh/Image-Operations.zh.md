# 图像 operations

## 范围与当前可用性

结构化 `Result` 是内置图像 operation 的 semantic、input、output、publication 和 ownership 契约。当前内置图像 operation 包括 `image.exposure_gain`、`image.opacity`、`image.brush_circle`、`image.mask`、`image.source_over`、`image.mix`、`image.gaussian_blur`、`image.downsample_box`、`mask.downsample_box`、`image.split_horizontal`、`image.stmap` 和两个 local inpainting operation。它们都使用 Result operation API。Result schema 声明由 typed tensor storage 支持的 typed image slots；存储可使用 private `PlanarImage` backing 或 immutable affine `CpuStorage`，也可以声明 primitive fields。普通非图像 numeric `Value` 仍然有效。

下列 built-in image operations 通过 typed Result image slots 执行。各 operation 声明支持的 schema、region rule、backend 与 publication 行为；注册本身不代表支持所有 structural image layout。

## Production 源码与 runtime 状态

| Family | 当前源码与可用性 | 当前 Result 图像状态 |
| --- | --- | --- |
| FMT alpha | 构建 `alpha_operations.cpp`、`alpha_common.hpp` 与 `alpha_authoring.cpp`；安装 `photospider/format/alpha.hpp`。 | 三个 CPU profiles 共注册 associate、unassociate、set 九个 native Result ABI 2 keys。`extract_alpha` 与 `remove_alpha` 仍是基于已注册 operations 的 helper 组合。 |
| FMT channel extraction | `channel_extraction.cpp` 注册六个 `channel.extract_index_<profile>` 与 `channel.extract_named_<profile>` CPU Result operation；`format::split_channels` 展开 index nodes。Public header `photospider/format/channel.hpp` 单独安装。 | 对 generic 和 spatial Result tensor 提供精确分量访问及 Dependency-v2 support。这是 tensor operation，不是像素转换。 |
| FMT literal-like fill | `channel_literal_like.cpp` 注册三个 `channel.literal_like_<profile>` Result CPU key。 | 单输入 Result；仅请求 Descriptor role 8，不读来源 samples；按请求输出重复同 dtype raw bits。可用于 FMT-05B opaque lowering，但不实现 `extract_alpha`。 |
| FMT scalar literal | `channel_assembly.cpp` 注册三个 `channel.scalar_literal_<profile>` Result CPU key。 | 无输入 Whole primitive；以 prepared native bits 发布一个 generic shape `[1]` tensor，供 FMT-03 scalar sources 使用。 |
| FMT channel assembly/editing | `channel_assembly.cpp` 注册 A/B/C CPU Result keys；已安装 assembly/editing headers 提供 A/B/C 与 swizzle/replace helpers。 | Result operation ABI 2；单 tensor 输入输出、七种逐位 dtype、样本数量有界、所有输入均做 Descriptor 检查、Data 按映射请求。行为测试见 [`test_channel_assembly.cpp`](../../../tests/integration/test_channel_assembly.cpp) 与 [`test_channel_editing.cpp`](../../../tests/integration/test_channel_editing.cpp)。 |
| FMT metadata assignment | `metadata_assignment.cpp` 注册 `metadata.assign_<profile>` keys；`format::remove_metadata` 是 lowering helper。Public header `photospider/format/metadata.hpp` 单独安装。 | Result operation ABI 2；输入恰有一个 tensor member 且没有 fields。保留 schema、tensor key、batch axes、layout 和样本位。这是通用 Result tensor operation，不是专用图像 operation family。 |
| FMT RGB basis conversion | `rgb_basis.cpp`、`rgb_basis_math.cpp` 注册 A/B/C CPU Result keys；`photospider/format/rgb_basis.hpp` 已安装。 | 三个 CPU profiles 共注册九个 Result ABI 2 keys。`format::convert_linear_rgb` 事务式组合已注册 stages，不增加 D key。这是通用 tensor operation，不是 image-slot operation。 |
| FMT model conversion | `model_conversion.cpp`、`model_math.cpp` 和 `model_simd.cpp` 注册 FMT-11 Result operations；`photospider/format/model_conversion.hpp` 提供 graph helpers。 | A-R/T 在三个 CPU profiles 下共注册 57 个 keys；FMT-11S 降低为已注册的 MASK threshold operation。 |
| FMT 数值格式转换与 transfer | `numeric_conversion.cpp` 注册 `numeric.convert_format_strict`；`transfer.cpp` 注册 `color.transfer_encode_<profile>` 与 `color.transfer_decode_<profile>`。`photospider/format/transfer.hpp` 单独安装。 | 两者都是通用 Result tensor operation，不是图像专用 family。数值转换执行同坐标 dtype/range conversion；transfer 对选中的 semantic RGB/Gray group 或显式 raw components 执行标量曲线转换。 |
| Exposure and grade | `image.exposure_gain` 使用内置 Result 图像路径。`grade.levels` 仍是 numeric field operation。 | Exposure 接收 Result 图像和 `[0, 16]` 内的单例 Float32 Result 控制值；它缩放 RGB 并保留 alpha。`grade.levels` 仍为 numeric operation。 |
| Gaussian | `image_program.cpp` 实现 typed `image.gaussian_blur`；`gaussian.cpp` 和 `gaussian_gpu.cpp` 实现独立的 generic numeric Gaussian operations。 | 图像 Gaussian 接收 `photospider.image` Results。在空间边界 clamp，并请求 radius halo；保留图像语义。CPU 与 Metal 使用不同的舍入契约。 |
| STMap and dependency sampling | `dependency_sampling.cpp` 通过 Result API 注册 `image.stmap` 以及 generic `numeric.radius_gather` / `numeric.radius_scatter`。 | STMap 接收 RGBA `photospider.image` Result 和 cell shape 为 `{height, width, 2}` 的 generic Float64 map tensor；map 可以无 batch，也可以使用与 source 相同的 batch axes。Radius gather/scatter 接收无 batch 的 rank-one Float64 values 和 shape 相同的 Int64 radii，并发布 generic Float64 Result。这些都是 CPU Dependency-v2 operations；radius operations 不使用 image slot。 |
| Split and box downsampling | `image.split_horizontal`、`image.downsample_box` 和 `mask.downsample_box` 使用内置 Result 图像路径。 | 各 operation 发布 typed Result image slots。Split 发布 `full`、`left` 和 `right`；downsampling 将输出区域映射到相应输入区域。 |
| Composite and drawing | `image.brush_circle`、`image.mask`、`image.mix`、`image.opacity` 和 `image.source_over` 使用 Result。Native 和可选 OpenCV local Navier-Stokes operation 使用同一 Result API，并以 typed `photospider.image` 作为输入和输出。 | Inpainting 验证图像和 mask 的完整 batch 域，并发布完整图像 Result，保留接受的图像 schema 和 facets。它使用 Whole 需求，且没有 GPU backend。 |
| Numeric LUT and structured LUT Results | `lut3d_bake_geometry.cpp` 提供 Whole numeric Value geometry operations。`lut3d_bake_results.cpp` 中 Pack 与 Measure 发布 structured Results；Unpack 与 Gate 发布 Values。`lut1d_application.cpp`、`lut3d_application.cpp`、`lut_apply_1d.cpp` 和 `field_apply_lut_1d.cpp` 消费 numeric Value/table 契约。 | 这些 numeric 和非图像 structured operation 不是 Result 图像实现。 |
| Perlin and numeric generation | `perlin.cpp` 及其 GPU 实现接受并发布泛型 numeric Result tensor；Whole/GPU 使用整张量依赖，CPU tiled 使用 Result Dependency-v2 映射。 | 属于数值生成，不是 Result 图像算子。GPU 行为入口为 [`test_perlin_gpu.cpp`](../../../tests/integration/test_perlin_gpu.cpp)；该源码本身不能证明某一设备上的 native execution。 |
| RGBA Metal shader support | `plugins/ops/rgba32f/image_gpu.h` 和 `image.metal` 提供共享 native image helper 与 Metal shader；CMake 将 shader 嵌入生成源码。产品不再保留独立的 `image_plugin.c` DSO。 | 八个 built-in Result operations 执行 native Metal：exposure、opacity、Gaussian blur、mask、source-over、color downsample、mask downsample 和 brush。Mix 与 split 仍只有 CPU operation。 |
| PixelOE | `plugins/ops/PixelOE/src/workflow.cpp` 通过 installed plugin 构建 public Result workflow；十二个 Result ABI 2 keys 提供 CPU Whole、CPU tiled、Metal 和 Vulkan profiles。 | Workflow 声明 CPU、CPU-tiled、Metal 和 Vulkan profiles。Runtime validation 必须对应具体 backend 和 oracle run；本页不声明平台测试通过。 |

## RGBA 图像的原生 Metal operations

`plugins/ops/rgba32f/image_gpu.h` 中的共享 helper 通过 Result native GPU service 提交 `image.metal`。CMake 读取 shader 并生成嵌入式 header；旧的独立 C DSO 不属于当前产品接口。`image.exposure_gain`、`image.opacity`、`image.gaussian_blur`、`image.mask`、`image.source_over`、`image.downsample_box`、`mask.downsample_box` 和 `image.brush_circle` 已注册 native Metal 实现。`image.mix` 和 `image.split_horizontal` 只有 CPU 实现。当前 native image 行为入口为 [`tests/integration/image/test_result_image_native.cpp`](../../../tests/integration/image/test_result_image_native.cpp) 和 [`tests/integration/test_metal_images.cpp`](../../../tests/integration/test_metal_images.cpp)。`installed_image_domain` 与 `installed_image_domain_native` 是已注册的 package consumer 检查；硬件相关测试在没有 native device 时会 skip。

`tests/integration/test_metal_images.cpp` 覆盖无效 image/coverage binding、schema 拒绝、aligned 与 unaligned owner view、native dispatch 和 CPU fallback、transfer accounting，以及 context 退休后读取输出。其断言说明当前 owner-alignment 与 transfer 行为；native validation 需要可用的 Metal device。

RGBA operands 使用 `photospider.image` Result，其中有一个名为 `pixels` 的 spatial Float32 HWC tensor，并且恰有两个 batch axes：frame 与 layer。Coverage operands 使用相同 schema，但 tensor 为 spatial Float32 HW，batch axes 相同。RGB samples 可以是有限的 signed/HDR 值。RGBA 语义为 linear、premultiplied：alpha 必须有限且处于 [0,1]；alpha 为零时 RGB 必须为零。Coverage samples 必须是 [0,1] 内的有限值。每个 frame/layer image 独立处理；多图像 operations 要求 batch extent 一致。

Image Gaussian 要求 Int64 `radius` 位于 [1,64]，Float64 `sigma` 位于 [0.1,64]。Preparation 使用 Float64 `exp` 计算并归一化 Gaussian weights。CPU 按递增 tap 顺序进行 Float64 乘加，水平 pass 舍入一次到 Float32，再由垂直 pass 舍入一次到 Float32。Metal 将准备好的权重转换为 Float32，并在 shader 中使用补偿求和。 [`test_result_image_native.cpp`](../../../tests/integration/image/test_result_image_native.cpp) 使用的 CPU/Metal 比较 oracle 为 `abs(gpu - cpu) <= 1e-6 + 1e-5 * abs(cpu)`；这不构成 CPU/Metal 逐位相同的契约。两条路径都会在图像边缘 clamp tap，并请求所需 halo。 Gaussian 对 Empty demand 使用 stateless continuation，不产生 source observations；完整 batch/sample cardinality 溢出时，只要所请求的小 ROI 仍可表示，仍可执行。

GPU 实现遇到保守 native arithmetic domain 之外的有限输入时，可返回 `BackendUnavailable`。八个同时有 CPU 和 GPU 实现的 operation traits 允许 CPU fallback；执行条件与 poll-time retry 限制见[Compiler 和本地执行](Compiler-and-Execution.zh.md)。其他失败不会触发 fallback。

## 原生 alpha Result operations

已安装的 `photospider/format/alpha.hpp` 可向普通 workflow 追加 FMT-04A/B 与
FMT-05A nodes。九个 native keys 使用 Result ABI 2；FMT-05B/C 仍是基于已注册
extraction、literal-like fill、mapped assembly 和 metadata assignment keys 的
authoring compositions。这些是通用 single-tensor operations，不是专用的
`photospider.image` slots。它们保留 batch prefix，按 cell-relative axes 处理，
并限制 full sample rank 不超过 8、count 不超过 2^40。输入必须是无 fields 的单
tensor；算术使用同 dtype Float32/Float64，提取和移除可逐位处理七种支持的 dtype。

对于 FMT-04 semantic operations，参与计算的 color 和被消费的 alpha samples
具有 Data 与 Validation support；只透传的 samples 不增加额外 sample validation。
Raw association 只按 arithmetic 请求 Data，不附带 semantic-domain Validation。
FMT-05A Set 对选中的 source colors 请求 Data 与 Validation；为这些 color outputs
读取新 alpha source 时只增加 Validation support；发布的新 alpha channel 则请求其
source 的 Data 与 Validation。所有连接输入均请求 Descriptor，这些 operations 不用
Control。

FMT-04 associate/unassociate 始终 materialize，且没有 layout 参数。在原生 alpha operations 中，只有 FMT-05A Set
接受 layout policy。Generic Set view 要求完整 declared map 可证明使用单个
`CpuStorage` owner 和单一 affine relation；spatial Set view 仅支持 internal identity。
Set 的 `auto` 只在物理 `ViewUnavailable` 时 materialize。Materialization 在 Root
budgets 与 cancellation 约束下事务发布；空输出请求不保留本次执行的载荷状态。这些
FMT operations 没有新增 GPU backend。

当前 `02-format-color` 中已有的 built-in FMT families 均提供 Result CPU operations，或提供可降低为已注册 operations 的 helper。FMT-01、FMT-02/03、FMT-04/05A alpha、FMT-06 numeric conversion、FMT-08、FMT-09 transfer、FMT-10 RGB basis、literal-like fill 与 scalar literal 使用 Result operation ABI 2。Public headers `photospider/format/channel.hpp`、`channel_assembly.hpp`、`channel_editing.hpp`、`alpha.hpp`、`metadata.hpp`、`transfer.hpp` 和 `rgb_basis.hpp` 和 `model_conversion.hpp` 分别单独安装。FMT-05B/C 是现有 Result operations 的可执行组合；FMT-04/05A、FMT-09 和 FMT-10 已有 native keys。其他 production operations 可能仍注册，但其 structural-image Value 契约仍在 Result 图像路径之外。注册本身不能证明具备 Result 图像支持。

## 内置 Result 图像 operations

`image.exposure_gain`、`image.opacity`、`image.brush_circle`、`image.mask`、`image.source_over`、`image.mix`、`image.gaussian_blur`、`image.downsample_box`、`mask.downsample_box` 和 `image.split_horizontal` 接收结构化 `Result` 并发布结构化 `Result`。图像 slot 使用 schema `photospider.image` version 1，包含一个名为 `pixels` 的 tensor 且不包含 fields。每个 slot 恰有两个 batch axes，分别表示 frame 和 layer，并声明 spatial layout。RGBA operation 要求 Float32 HWC 四通道 tensor 和 canonical linear premultiplied RGBA 语义；coverage 输入为带 coverage 语义的 Float32 HW tensor。多图像输入的 frame/layer 数量和空间尺寸必须一致。

Local Navier-Stokes inpainting 使用同一 Result API 和 typed image schema。图像和 coverage 输入要求 frame/layer batch 与空间尺寸匹配。它验证完整的 `[N,L,H,W,4]` 图像域和 `[N,L,H,W]` mask 域，再发布完整图像 Result，并保留接受的图像 schema 和语义 facets。该 profile 要求 canonical linear-sRGB premultiplied RGBA、不透明 alpha 和 canonical coverage，并允许声明的 scene 或 display reference。不接受 CMYK 或 ICC ColorArray 输入，也不会为无类型 numeric tensor 推断图像语义。输出可以继续输入其他 typed image operation。

图像 tensor 的 descriptor axes 表示 cell 轴。坐标 `{frame, layer, y, x, channel}` 定位一个 RGBA 样本，`{frame, layer, y, x}` 定位一个 coverage 样本。HWC 的 channel 在逻辑上是一个轴；`PlanarImage` 可以把各 channel 存为不同物理平面。所有这些 operation 都保留 frame 和 layer batch axes。

`image.exposure_gain` 按 `[0, 16]` 内的控制值缩放 RGB 并保留 alpha。`image.opacity` 按 `[0, 1]` 内的控制值缩放四个 premultiplied RGBA channel。`image.brush_circle` 在图像后接收 `x`、`y`、`radius`、`red`、`green`、`blue`、`alpha` 七个控制值，按像素中心与圆心的距离绘制；半径必须是正的 normal Float32，alpha 范围为 `[0, 1]`。

`image.mask` 将 premultiplied RGBA 与 coverage 相乘。`image.source_over` 按前景、背景顺序输入。`image.mix` 接收两张 RGBA 图像和一张作为插值系数的 coverage 图像。`image.downsample_box` 与 `mask.downsample_box` 要求整数参数 `factor` 在 `[1, 16]` 内，对每个 box 求平均，边缘不完整的 box 按实际样本数计算，输出高度和宽度使用向上取整。`image.split_horizontal` 要求 `split_x` 严格位于输入宽度内部，并通过复制 HWC tensor samples 发布 `full`、`left`、`right`。它接受七种受支持的 tensor dtype，不要求 RGBA semantics，保留输入 tensor schema，并逐样本复制完整字节。集成测试检查 UInt8、UInt16 和 Float64 samples，以及 split output 超过 temporary workspace 时仍能发布。Downsample 测试还覆盖靠近 `UINT64_MAX` 的 factor-16 边界。

每个标量控制值都是只有一个 unbatched Float32 tensor 的 Result，sample shape 为 `{1}`。输入契约选择 schema 中唯一的 tensor member，因此其 tensor key 可由调用方选取。该 singleton sample 支持未对齐 stride，包括 `INT64_MIN`，callback 通过 Result tensor 接口读取。Exposure、opacity、brush 半径和 alpha 的范围由控制输入契约验证。直接绑定中的越界或非有限值以 `InvalidArgument` 失败，即使图像输出 footprint 为 Empty；上游 callback 生成的无效值在执行时以 `OperationFailed` 失败，且不会发布输出。

Scheduler 为依赖验证请求完整标量样本，因此改变标量值或其验证状态可能使依赖它的所有输出像素变脏。图像输入按 operation 映射的空间 support 请求；例如 box downsampling 会为每个输出区域请求对应的输入 box。Empty 输出 footprint 不请求图像样本。这些描述只适用于当前内置实现。原生 Metal 支持范围仅为上文 RGBA Metal 一节列出的八个 operations；本表其他 operation 不因这些契约而获得 GPU 支持。

## Result 图像契约

Typed image slot 带有描述单个 frame/layer 的 descriptor、planar 物理布局、语义 facets，以及有界的 frame/layer 数量。其逻辑坐标为 `{frame, layer, descriptor axes...}`；frame 和 layer 是独立轴，不是 color channels。一个 Result 可在同一 schema 中混合 typed image slots 和 primitive fields。不得把图像 bytes 编码为 `ResultFieldSpec` records。

Compiler 解析选中的 named output 及其 input projection。Result continuation 只请求声明过的 inputs；callback 可先读取 Control samples，再请求这些值所选择的 Data image support。发布的 relation 同时记录两者。Dirty transpose 使用同一 relation，因此已消费的 control 可以切换到新的 support，而无关输入不会被误作 dependency。`Exact`、`Conservative` 和 `Unknown` 保持各自声明的证明语义。

Result owner 保留 schema、已认证 descriptor facts、backing、relations 和已消费 input owners。Work、stages、I/O、relation/maps、payload 与 owners 共用 execution root 进行 admission。Publication 失败具有 sticky 语义。Cancellation 和 stale execution 会等待活动 callbacks 退出后再释放其 phase state。参见 [Structured Results and image slots](Global-Results.zh.md)、[Dependency Data](Dependency-Data.zh.md) 和 [Tensor Storage and Region Access](../../kernel-specs/zh/Tensor-Storage-and-Region-Access.zh.md)。

## 公开 workflow 与行为测试

[`examples/image_vertical`](../../../examples/image_vertical/README.md) 与 [`examples/regional_image_vertical`](../../../examples/regional_image_vertical/README.md) 是带独立 sample oracle 的可运行 Result workflow。注册行为测试按家族组织在 [`tests/integration/image`](../../../tests/integration/image)：格式和语义校验、backing 与 view ownership、relations 和 dirty mapping、execution、filters、transforms、composites 以及 native image 行为。共享 Result contract 由 [`test_result_image_contracts.cpp`](../../../tests/integration/test_result_image_contracts.cpp) 覆盖；[`test_metal_images.cpp`](../../../tests/integration/test_metal_images.cpp) 检查 image binding 和 native owner 行为。GPU 测试在所需设备不可用时有显式 skip status。这些源码定义当前可执行覆盖范围；本节不报告历史通过数或 dispatch 总数。

[`examples/unified_result_workflow`](../../../examples/unified_result_workflow/README.md) 提供使用 test/example-only minimal operations 的 public Result workflow。它选择 image Result output 与 numeric output、请求 image regions，并检查动态 support 变化。普通 numeric Value 和非 image structured Result 仍可用。

## FMT-11 颜色模型转换

FMT-11 A-R 和 T 在 strict、Apple Silicon 与 x86-64 profiles 下共注册 57 个 CPU keys。已安装的 authoring helpers 追加一个 key 并返回包含一个 tensor 的 `values` output；S 降低为 `mask.threshold_channel_<profile>`，不增加 color key。输入为无 fields 的单 Float32 或 Float64 tensor。完整 sample rank 不超过 8；sample count 受 Result schema 可表示范围和执行资源限制约束。Axis 相对 cell axes 编号，不包含 batch prefix。

选中的语义 samples 请求 Data 与 Validation；raw T 验证 two-level selector；raw S 与 bypass samples 请求 Data；R 的常量输出仅需 Descriptor。Empty request 无运行状态。整个请求区域满足物理 view 约束时，Q 支持 generic 与 spatial view。其他成员物化输出，即使只请求 bypass 也拒绝 forced view。输出保留 schema id、tensor key 和 batches，更新选中的模型描述，并将 `atomic_trailing_axes` 设为零。行为源码包括 [`test_model_conversion.cpp`](../../../tests/integration/test_model_conversion.cpp)、[`test_alpha_operations.cpp`](../../../tests/integration/test_alpha_operations.cpp) 与 [`test_alpha_model_interop.cpp`](../../../tests/integration/test_alpha_model_interop.cpp)。这些是通用 tensor operations，不是 typed image-slot operations。
