# 图像 operations

## 范围与当前可用性

下列 built-in image operations 通过 typed Result image slots 执行。各 operation 声明支持的 schema、region rule、backend 与 publication 行为；注册本身不代表支持所有 structural image layout。

## Production 源码与 runtime 状态

| Family | 当前源码与可用性 | 当前 Result 图像状态 |
| --- | --- | --- |
| FMT alpha | 构建 `alpha_operations.cpp`、`alpha_common.hpp` 与 `alpha_authoring.cpp`；安装 `photospider/ops/format/alpha.hpp`。 | 三个 CPU profiles 共注册 associate、unassociate、set 九个 native Result ABI 3 keys。`extract_alpha` 与 `remove_alpha` 仍是基于已注册 operations 的 helper 组合。 |
| FMT channel extraction | `channel_extraction.cpp` 注册六个 `channel.extract_index_<profile>` 与 `channel.extract_named_<profile>` CPU Result operation；`format::split_channels` 展开 index nodes。Public header `photospider/ops/format/channel.hpp` 单独安装。 | 对 generic 和 spatial Result tensor 提供精确分量访问及 Dependency-v2 support。这是 tensor operation，不是像素转换。 |
| FMT literal-like fill | `channel_literal_like.cpp` 注册三个 `channel.literal_like_<profile>` Result CPU key。 | 单输入 Result；仅请求 Descriptor role 8，不读来源 samples；按请求输出重复同 dtype raw bits。可用于 FMT-05B opaque lowering，但不实现 `extract_alpha`。 |
| FMT scalar literal | `channel_assembly.cpp` 注册三个 `channel.scalar_literal_<profile>` Result CPU key。 | 无输入 Whole primitive；以 prepared native bits 发布一个 generic shape `[1]` tensor，供 FMT-03 scalar sources 使用。 |
| FMT channel assembly/editing | `channel_assembly.cpp` 注册 A/B/C CPU Result keys；已安装 assembly/editing headers 提供 A/B/C 与 swizzle/replace helpers。 | Result operation ABI 3；单 tensor 输入输出、七种逐位 dtype、样本数量有界、所有输入均做 Descriptor 检查、Data 按映射请求。行为测试见 [`test_channel_assembly.cpp`](../../../tests/integration/test_channel_assembly.cpp) 与 [`test_channel_editing.cpp`](../../../tests/integration/test_channel_editing.cpp)。 |
| FMT metadata assignment | `metadata_assignment.cpp` 注册 `metadata.assign_<profile>` keys；`format::remove_metadata` 是 lowering helper。Public header `photospider/ops/format/metadata.hpp` 单独安装。 | Result operation ABI 3；输入恰有一个 tensor member 且没有 fields。保留 schema、tensor key、batch axes、layout 和样本位。这是通用 Result tensor operation，不是专用图像 operation family。 |
| FMT RGB basis conversion | `rgb_basis.cpp`、`rgb_basis_math.cpp` 注册 A/B/C CPU Result keys；`photospider/ops/format/rgb_basis.hpp` 已安装。 | 三个 CPU profiles 共注册九个 Result ABI 3 keys。`format::convert_linear_rgb` 事务式组合已注册 stages，不增加 D key。这是通用 tensor operation，不是 image-slot operation。 |
| FMT model conversion | `model_conversion.cpp`、`model_math.cpp` 和 `model_simd.cpp` 注册 FMT-11 Result operations；`photospider/ops/format/model_conversion.hpp` 提供 graph helpers。 | A-R/T 在三个 CPU profiles 下共注册 57 个 keys；FMT-11S 降低为已注册的 MASK threshold operation。 |
| FMT 数值格式转换与 transfer | `numeric_conversion.cpp` 注册 `numeric.convert_format_strict`；`transfer.cpp` 注册 `color.transfer_encode_<profile>` 与 `color.transfer_decode_<profile>`。`photospider/ops/format/transfer.hpp` 单独安装。 | 两者都是通用 Result tensor operation，不是图像专用 family。数值转换执行同坐标 dtype/range conversion；transfer 对选中的 semantic RGB/Gray group 或显式 raw components 执行标量曲线转换。 |
| Numeric LUT and structured LUT Results | `lut3d_bake_geometry.cpp` 提供 Whole numeric Value geometry operations。`lut3d_bake_results.cpp` 中 Pack 与 Measure 发布 structured Results；Unpack 与 Gate 发布 Values。`lut1d_application.cpp`、`lut3d_application.cpp`、`lut_apply_1d.cpp` 和 `field_apply_lut_1d.cpp` 消费 numeric Value/table 契约。 | 这些 numeric 和非图像 structured operation 不是 Result 图像实现。 |
| PixelOE | `plugins/ops/PixelOE/src/workflow.cpp` 通过 installed plugin 构建 public Result workflow；十二个 Result ABI 3 keys 提供 CPU Whole、CPU tiled、Metal 和 Vulkan profiles。 | Workflow 声明 CPU、CPU-tiled、Metal 和 Vulkan profiles。Runtime validation 必须对应具体 backend 和 oracle run；本页不声明平台测试通过。 |

## 原生 alpha Result operations

已安装的 `photospider/ops/format/alpha.hpp` 可向普通 workflow 追加 FMT-04A/B 与
FMT-05A nodes。九个 native keys 使用 Result ABI 3；FMT-05B/C 仍是基于已注册
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

当前 `02-format-color` 中已有的 built-in FMT families 均提供 Result CPU operations，或提供可降低为已注册 operations 的 helper。FMT-01、FMT-02/03、FMT-04/05A alpha、FMT-06 numeric conversion、FMT-08、FMT-09 transfer、FMT-10 RGB basis、literal-like fill 与 scalar literal 使用 Result operation ABI 3。Public headers `photospider/ops/format/channel.hpp`、`channel_assembly.hpp`、`channel_editing.hpp`、`alpha.hpp`、`metadata.hpp`、`transfer.hpp` 和 `rgb_basis.hpp` 和 `model_conversion.hpp` 作为只含头文件的 `Photospider::ops_headers` target 的一部分分别安装；使用方将它与 `Photospider::kernel` 一起链接。FMT-05B/C 是现有 Result operations 的可执行组合；FMT-04/05A、FMT-09 和 FMT-10 已有 native keys。其他 production operations 可能仍注册，但其 structural-image Value 契约仍在 Result 图像路径之外。注册本身不能证明具备 Result 图像支持。

## Result 图像契约

Typed image slot 带有描述单个 frame/layer 的 descriptor、planar 物理布局、语义 facets，以及有界的 frame/layer 数量。其逻辑坐标为 `{frame, layer, descriptor axes...}`；frame 和 layer 是独立轴，不是 color channels。一个 Result 可在同一 schema 中混合 typed image slots 和 primitive fields。不得把图像 bytes 编码为 `ResultFieldSpec` records。

Compiler 解析选中的 named output 及其 input projection。Result continuation 只请求声明过的 inputs；callback 可先读取 Control samples，再请求这些值所选择的 Data image support。发布的 relation 同时记录两者。Dirty transpose 使用同一 relation，因此已消费的 control 可以切换到新的 support，而无关输入不会被误作 dependency。`Exact`、`Conservative` 和 `Unknown` 保持各自声明的证明语义。

Result owner 保留 schema、已认证 descriptor facts、backing、relations 和已消费 input owners。Work、stages、I/O、relation/maps、payload 与 owners 共用 execution root 进行 admission。Publication 失败具有 sticky 语义。Cancellation 和 stale execution 会等待活动 callbacks 退出后再释放其 phase state。参见 [Structured Results and image slots](Global-Results.zh.md)、[Dependency Data](Dependency-Data.zh.md) 和 [Tensor Storage and Region Access](../../kernel-specs/zh/Tensor-Storage-and-Region-Access.zh.md)。

普通 numeric Value 和非 image structured Result 仍可用。

## FMT-11 颜色模型转换

FMT-11 A-R 和 T 在 strict、Apple Silicon 与 x86-64 profiles 下共注册 57 个 CPU keys。已安装的 authoring helpers 追加一个 key 并返回包含一个 tensor 的 `values` output；S 降低为 `mask.threshold_channel_<profile>`，不增加 color key。输入为无 fields 的单 Float32 或 Float64 tensor。完整 sample rank 不超过 8；sample count 受 Result schema 可表示范围和执行资源限制约束。Axis 相对 cell axes 编号，不包含 batch prefix。

选中的语义 samples 请求 Data 与 Validation；raw T 验证 two-level selector；raw S 与 bypass samples 请求 Data；R 的常量输出仅需 Descriptor。Empty request 无运行状态。整个请求区域满足物理 view 约束时，Q 支持 generic 与 spatial view。其他成员物化输出，即使只请求 bypass 也拒绝 forced view。输出保留 schema id、tensor key 和 batches，更新选中的模型描述，并将 `atomic_trailing_axes` 设为零。行为源码包括 [`test_model_conversion.cpp`](../../../tests/integration/test_model_conversion.cpp)、[`test_alpha_operations.cpp`](../../../tests/integration/test_alpha_operations.cpp) 与 [`test_alpha_model_interop.cpp`](../../../tests/integration/test_alpha_model_interop.cpp)。这些是通用 tensor operations，不是 typed image-slot operations。
