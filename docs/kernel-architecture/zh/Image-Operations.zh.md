# 图像 operations

## 范围与当前可用性

结构化 `Result` 是图像 samples 的 public semantic、input、output、publication 和 ownership 契约。Result schema 声明由 `PlanarImage` backing 的 typed image slots，也可以声明 primitive fields。普通非图像 numeric `Value` 仍然有效。看起来像图像的 `Value` facet 或 operation catalog 中存在注册项，都不会使 Value callback 成为 Result 图像 operation。

当前 kernel package 为 0.30.0，WorkflowDocument schema 为 4，OperationTraits 版本为 21。独立 planar callback table、planar image executor 和 planar workflow binding 已移除。旧 planar C 入口 v1-v3 会被拒绝。Production operation 源码尚未改写为 Result 图像契约。下表说明当前源码和 runtime 可用性；test-defined minimal operations 与 public example 可使用 Result 图像路径。

## Production 源码与 runtime 状态

| Family | 当前源码与可用性 | 当前 API 要求的 Result 契约 |
| --- | --- | --- |
| FMT alpha | `alpha_operations.cpp`、`alpha_authoring.cpp` 和 `alpha_common.hpp` 保留在源码中；FMT 注册与构建目标已停用。 | 声明图像 slots，并在 Result descriptors 与 relations 中保留 alpha association、channel roles、group closure 和 output-specific metadata。 |
| FMT channel construction and extraction | `channel_assembly.cpp`、`channel_literal_like.cpp` 和 `channel_extraction.cpp` 保留在源码中；共享 FMT 注册已停用，包括这些编译单元中的 numeric/scalar 分支。 | 图像端口使用 image slots；真正的 numeric ports 保持为 Values，并在 Result metadata 和 relations 中保留 channel/group 语义。 |
| FMT metadata assignment | `metadata_assignment.cpp` 保留在源码中；其 FMT 注册已停用。 | 将图像 metadata 与拥有它的 Result image slot 一同发布，使 descriptor facts 和 backing 指向同一 Result generation。 |
| FMT model conversion and RGB basis | `model_conversion.cpp`、`model_math.cpp`、`model_simd.cpp`、`rgb_basis.cpp` 和 `rgb_basis_math.cpp` 保留在源码中；注册项已停用。 | 将 color groups、encodings、channel roles 和 image backing 绑定到 typed Result slots，同时保持各 operation 声明的 numeric 行为。 |
| FMT numeric conversion and transfer | `numeric_conversion.cpp`、`transfer.cpp`、`transfer_definition.cpp` 和 helpers 保留在源码中；FMT 注册已停用。 | 分开 numeric Value ports 与 image Result ports，并通过 typed descriptors 携带 image facets 和物理布局。 |
| Exposure and grade | `image_exposure_gain.cpp` 仍注册，但 compiler 会拒绝其 image-Value 契约。`grade_levels.cpp` 仍是 numeric field operation。 | Exposure 必须声明 Result image input/output slots。除非其 public port 契约变化，`grade.levels` 保持 numeric Value。 |
| Gaussian | `gaussian.cpp` 和 `gaussian_gpu.cpp` 仍注册，但 compiler 会拒绝其 structural image Value 契约。 | 声明 Result image slots，并通过 structured dependency relation 发布 output coverage 和 sample support，同时仅保留已实现的 Whole、tile 和 GPU capabilities。 |
| STMap and dependency sampling | `dependency_sampling.cpp` 仍注册；其 STMap 图像路径使用旧 Value dependency protocol，不能作为 Result 图像 operation 执行。Numeric radius operation 仍是 Value operation。 | 声明 map/control dependencies，读取 Control samples 后发现所选 Data support，并在 Result relation 中发布已消费的 Control 与 Data support。 |
| Split and box downsampling | `image_split_horizontal.cpp`、`image_downsample_box.cpp` 和 `mask_downsample_box.cpp` 仍注册，但 compiler 会拒绝 structural image Value 契约。 | 将每个命名输出或缩小后的图像声明为 typed Result slot，并提供各自的 query、support 和 dirty relation。 |
| Composite and drawing | `image_brush_circle.cpp`、`image_local_inpaint_navier_stokes.cpp`、`image_mask.cpp`、`image_mix.cpp`、`image_opacity.cpp` 和 `image_source_over.cpp` 仍注册；其 image Value 契约不能通过 Result 路径执行。 | 使用 Result image slots，并保留 control/mask relations、color group facts、有效 coverage 和 Result owner associations。 |
| Numeric LUT and structured LUT Results | `lut3d_bake_geometry.cpp` 提供 Whole numeric Value geometry operations。`lut3d_bake_results.cpp` 中 Pack 与 Measure 发布 structured Results；Unpack 与 Gate 发布 Values。`lut1d_application.cpp`、`lut3d_application.cpp`、`lut_apply_1d.cpp` 和 `field_apply_lut_1d.cpp` 消费 numeric Value/table 契约。 | Numeric geometry 与 Value consumers 保持为 Values。Pack/Measure Results 使用 typed primitive fields。面向图像的端口需要 typed image slot；图像 payload 不是 primitive field byte array。 |
| Perlin and numeric generation | `perlin.cpp` 及其 GPU 实现发布泛型 numeric Values。调用方把 samples 解释为像素，不会自动形成图像契约。 | 非图像用途保持 numeric。图像生成需要显式声明 Result image output 和图像专用的 dependency/publication 契约。 |
| C `rgba32f` module | `plugins/ops/rgba32f/image_plugin.c` 及其 GPU helper 保留了使用基础 operation ABI 11 和 in-tree SDK 的可选 DSO target。Compiler 与 registry runtime 会拒绝使用其旧 `RgbaFloat32`/`Float32Mask` image Value ports 的 workflows 和 invocations，因为这些端口要求 Result 图像契约。其 standalone CMake consumer 请求 package 0.11，与当前 exact-version package 不匹配；此处不宣称 build 或 runtime 验证通过。 | 声明 image slots，并使用 Result C ABI 1 提供 selected outputs、dependency services、leases 和有界 publication。 |
| PixelOE | `plugins/ops/PixelOE/src/workflow.cpp` 及其 runtime 使用原 planar execution surface；尚未适配 Result image slots。 | 通过 Result inputs/outputs 绑定 image slots，再使用统一 Result execution path。 |

九个 FMT 注册 family 已从 built-in registry 和常规 kernel operation 构建中移除：alpha、channel literal-like、channel extraction、model conversion、numeric conversion、transfer、channel assembly、metadata assignment 和 RGB basis。其源码仍保留在仓库中。Public FMT header subtree 不会安装。其他 production operations 可能仍注册，但 compiler 会拒绝其旧 structural-image Value 契约。注册存在不等于 runtime 图像支持。

## Result 图像契约

Typed image slot 带有描述单个 frame/layer 的 descriptor、planar 物理布局、语义 facets，以及有界的 frame/layer 数量。其逻辑坐标为 `{frame, layer, descriptor axes...}`；frame 和 layer 是独立轴，不是 color channels。一个 Result 可在同一 schema 中混合 typed image slots 和 primitive fields。不得把图像 bytes 编码为 `ResultFieldSpec` records。

Compiler 解析选中的 named output 及其 input projection。Result continuation 只请求声明过的 inputs；callback 可先读取 Control samples，再请求这些值所选择的 Data image support。发布的 relation 同时记录两者。Dirty transpose 使用同一 relation，因此已消费的 control 可以切换到新的 support，而无关输入不会被误作 dependency。`Exact`、`Conservative` 和 `Unknown` 保持各自声明的证明语义。

Result owner 保留 schema、已认证 descriptor facts、backing、relations 和已消费 input owners。Work、stages、I/O、relation/maps、payload 与 owners 共用 execution root 进行 admission。Publication 失败具有 sticky 语义。Cancellation 和 stale execution 会等待活动 callbacks 退出后再释放其 phase state。参见 [Structured Results and image slots](Global-Results.zh.md)、[Dependency Data](Dependency-Data.zh.md) 和 [Tensor Storage and Region Access](../../kernel-specs/zh/Tensor-Storage-and-Region-Access.zh.md)。

## Public 可执行路径与限制

[`examples/unified_result_workflow`](../../../examples/unified_result_workflow/README.zh.md) 使用 test/example-only minimal operations 演示 public workflow。它选择 image Result output 和 numeric output，请求图像区域，并验证 dynamic support 变化。当前 built-in image operations 仍受上表限制；普通 numeric Values 和非图像 structured Results 不受影响。
