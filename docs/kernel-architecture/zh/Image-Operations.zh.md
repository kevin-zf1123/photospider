# 已退休的 Float32 图像算子契约

Package 0.19 已退休下述 packed 图像执行契约。以下声明、数值规则和 S1–S4 命令
仅供迁移参考，不是当前可执行图像 API。图像算子必须显式具备 planar 存储能力；
不支持的旧调用明确失败，不提供兼容回退。当前 CPU 接口和可运行 workflow 见
[张量存储与区域访问](../../kernel-specs/zh/Tensor-Storage-and-Region-Access.zh.md)。
通用标量和非图像张量能力继续遵循各自契约。

默认 registry 包含 CPU 算子，实现位于
[`plugins/ops/README.md`](../../../plugins/ops/README.md)。
下列两个 S1 算子都有两个有序 runtime Value input，无 compile-time parameter 或隐式默认值，
输出一个由 workflow 命名的区域图像。

| Operation | Input 0 | Input 1 | Output |
| --- | --- | --- | --- |
| `image.exposure_gain` | 图像 | Float32 gain scalar，闭区间 [0,16] | RGB 乘 gain，alpha 逐 bit 复制 |
| `image.opacity` | 图像 | Float32 opacity scalar，闭区间 [0,1] | 全部 RGBA channel 乘 opacity |

图像声明必须为 dense Float32 {H,W,4}，H/W 为正，whole Region，零 offset，canonical
row-major stride；运行视图带有显式 origin、stride 和有效 Region。两者精确包含一个 facet：key `photospider.image`，version 2，payload 由
`encode_semantic(rgba_semantics())` 生成；拒绝 image-v1 元数据，调用者使用公开 typed helper。
RGB 必须 finite，允许 signed，alpha 必须 finite 且位于 [0,1]，alpha 为零时 RGB 必须全零。
HDR RGB 可以超过 1 或 alpha，接受 signed zero。Caller 提供 linear sRGB/Rec.709、
D65、scene-referred relative RGB 与 dimensionless coverage alpha，使用
coverage-premultiplied association；不执行 color conversion、gamma、clamp 或 unpremultiplication。

Scalar input 可以直接引用 workflow declaration，也可连接上游 Float32 `{1}`。
允许无 facet、一个 dimensionless Scalar 或一个 dimensionless 单样本 SampledSignal。
采样轴单位/域与样本值单位独立并完整保留；拒绝其他 typed/opaque facet。直接绑定继续
要求 whole dense declaration（零 offset、stride `{4}`、四字节）。Computed view 要求
完整 `{1}` coverage，可使用 padding、非对齐、broadcast 或负 stride；C++、C 与 Metal
参数转换均按逻辑样本零安全读取字节，不隐式 cast/clamp。逐 Run 标量字节不改变编译计划
身份，符合缓存资格的 result key 同时包含数值字节与允许的语义 facet。

两者为 deterministic、side-effect-free、cacheable、PreserveFirstInput 和 Elementwise。
Image input demand 为请求的空间 output demand，完整包含四个 channel；scalar demand
始终为 whole {1}。小范围 demand 只返回所请求 Region，保留逻辑 descriptor。
每个 image step 经宿主分配输出，无重复 sink copy；完整预留包含保留中间结果和 scratch，
调用方已有输入和进程 RSS 单列。

每个运算阶段按 IEEE binary32 nearest、ties-to-even 舍入，保留 gradual underflow。
Host schema/numeric validation 和 image callback scope 保存并恢复 thread 浮点环境，
避免继承的 rounding 或 flush-to-zero 模式改变结果。计算出的 non-finite pixel、错误
profile 或 alpha-zero/nonzero-RGB output 返回 OperationFailed。绑定 pixel/scalar
数值域错误返回 InvalidArgument：直接 scalar 在执行前检查；computed scalar 数值错误
在每个消费 callback 前返回 OperationFailed，包含缓存与共享生产者结果。元数据失配为
TypeMismatch。Pixel 在消费 callback 前检查，未读取像素不扫描。

八算子的 C++、C 与 Metal 均实现本 image-v2 契约，显式声明 PreserveInput 语义并发布
首输入的精确 facet；box 算子仅改变逻辑 H/W。端口要求 canonical RGBA 或 typed
coverage mask。Straight alpha、RGB-only、重排通道及其他颜色模型须先显式转换。

## 可复用算子包与可执行示例

[`plugins/ops/rgba32f`](../../../plugins/ops/rgba32f/CMakeLists.txt) 仅通过
`Photospider::operation_sdk` 构建受维护的 ABI9 C module `photospider_rgba32f_ops`。
它实现相同图像算子和 profile，使用严格浮点编译选项。ABI9 host 在进入 callback
之前验证 port 并建立 nearest/gradual-underflow 浮点环境。Callback 向宿主申请输出并发布同一 buffer；成功后冻结为只读，失败时释放且不发布。将可信包加载到空
registry，随后 freeze 再编译；default registry 已有相同 operation key。

[`examples/image_vertical/image_fixture.hpp`](../../../examples/image_vertical/image_fixture.hpp)
是测试、安装消费者和配套 daemon vertical 共用的公开 fixture contract，固定
[ADR 0016](../../adr/0016-workflow-inputs-and-execution-bindings.md#named-fixtures-and-image-oracle)
中的声明、算子链、A/B 值、shape/layout/facet、请求像素 (0,1) 和输出表。
有界 CPU oracle `s1-rgba32f-exposure-opacity-v1` 从每份 binding snapshot 独立计算
16 个 channel，每阶段舍入到 binary32，先核对冻结输出表，再精确比较具名 `result`
的完整逻辑 descriptor 和所请求像素的 16 字节。Oracle 不调用算子 callback。

[`photospider_image_vertical`](../../../examples/image_vertical/main.cpp) 编译一次，
以同一个 plan 执行 A/B。每次要求两个成功 CPU callback（node 10、20）、相同 plan
identity、预期的不同 result digest、零 transfer/byte/fallback 和 48 bytes 实际分配
峰值。两个 image input demand 和 step output demand 均为 offsets {0,1,0}/extents
{1,1,4}；scalar demand 为 whole {1}，结果为逻辑 shape {2,2,4} 的单像素 Region。程序分行输出具名
输入/输出 Value、descriptor/Region/layout/facet、plan/result digest、编译/执行/算子
耗时、选用 backend、传输/资源观测及 correctness。耗时可以为零。Digest 用于诊断，
correctness 比较实际 byte。

随后每份 payload 执行两个 raw benchmark sample，分别捕获匹配的 CPU oracle。
这些 sample 保留 `RawBenchmarkRunner` 每次独立编译的语义，与编译一次的直接执行
分开报告。任何不匹配返回非零。无参数时使用内置算子，可选参数为可信 native module
的精确路径。

```sh
cmake --build build/issue257-static --target photospider_image_vertical test_bindings -j 8
build/issue257-static/examples/image_vertical/photospider_image_vertical
ctest --test-dir build/issue257-static -R '^test_(image_vertical|image_vertical_plugin|bindings|installed_consumer)$' --output-on-failure
```

`test_image_vertical_plugin` 向同一程序传入 generator 解析的包路径。`test_bindings`
保留精确 binding/output-demand 负例、独立并发快照、数值/浮点环境边界、取消和资源
检查，正常 DSO 路径改用受维护的算子包。故意发布错误输出的 DSO 仅留在测试中。

安装内核后，两个源码目录也可以独立构建：

```sh
cmake -S plugins/ops/rgba32f -B build/rgba32f-package -DCMAKE_PREFIX_PATH=/absolute/kernel-prefix -DCMAKE_BUILD_TYPE=RelWithDebInfo
cmake --build build/rgba32f-package --target photospider_rgba32f_ops -j 8
cmake -S examples/image_vertical -B build/image-example -DCMAKE_PREFIX_PATH=/absolute/kernel-prefix -DCMAKE_BUILD_TYPE=RelWithDebInfo
cmake --build build/image-example --target photospider_image_vertical -j 8
build/image-example/photospider_image_vertical /absolute/path/to/native-module
```

隔离安装消费者通过 installed SDK 构建同一算子源码包，在 shared bridge 中运行 A/B，
并以默认算子和 module 分别运行相同示例。Static/shared 内核均验证此路径、package
0.7 消费及 0.6 拒绝，参见[测试与验证](../../development/zh/Testing-and-Validation.zh.md)。

## S2 蒙版与合成

默认 registry 保留 `image.mask` 与 `image.source_over`。前者接收 RGBA 图像及
同 H/W 的 Float32 coverage 蒙版，以 `[0,1]` 有限样本乘前景各通道。
后者接收形状一致的前景与背景，按预乘通道计算
`F + B * (1 - F.alpha)`。两者是 CPU elementwise 算子。旧内建
`image.gaussian_blur` 已移除，拟议替代契约见
[05-filter](../../built-in_ops/05-filter/spatial.md)。

## S3 box 缩小与圆章

package 0.9 / operation ABI 9 的内建与 C 模块提供 image.downsample_box、
mask.downsample_box、image.brush_circle。前两者分别接收既有 RGBA 图像和 HW 蒙版，
必需静态 Int64 factor 为 [1,16]，无隐式默认。输出 H/W 除以 factor 向上取整，
反向需求为裁剪后的整数 box。按行/列 binary64 累加，以实际覆盖样本数平均并舍入
binary32。因子 1 保留数值。不转换 gamma 或解除预乘。应用代理默认 factor 4。

image.brush_circle 输入依次为 image、x、y、radius、red、green、blue、alpha；后七项
均为必需运行期 Float32 {1} 绑定，无静态参数。输出保持图像形状，使用 Elementwise。
x/y 为任意有限 Float32，radius 为正 normal Float32 至 FLT_MAX；非预乘线性 RGB 为
[-FLT_MAX,FLT_MAX]，alpha 为 [0,1]。使用 binary64 平方距离判断闭圆内像素中心；圆内 RGB
先以 binary32 乘 alpha，再无融合地对预乘背景执行 source-over；圆外保留原位。
一事件一硬边圆章，不抗锯齿、不补点、不处理压力或设备。应用规划裁剪包围 ROI
并将结果作为快照 patch。

test_s3_operations [trusted-module] 通过公开 compile/execute 使用独立 box 分配
和圆公式验证，覆盖奇数尺寸、边缘 ROI、因子 1/2/4/16 与无效标量。可复用交互
示例由 #275/#277 跟踪。

## S4 原生 Metal 边界

独立构建的 [`rgba32f` 算子模块](../../../plugins/ops/rgba32f) 有自己的 C ABI 注册与
shader 包，与仓库默认内建 registry 分开。移除内建 05-filter 实现不会注册替代滤镜，
也不表示任何拟议 FIL 成员已经有原生后端。默认 registry 的图像与 Metal 行为
应以当前注册算子和 planar 执行测试核对。

## Computed scalar 组合

[test_computed_scalar.cpp](../../../tests/integration/test_computed_scalar.cpp) 注册公开
coefficient.scale producer，并通过 WorkflowDocument 把输出接到 exposure、opacity 或
brush。同一编译计划在顺序/并发 Run 中改变 coefficient binding。Exposure 中 coefficient
1 生成 gain 2，coefficient 3 生成 gain 6，因此同一源像素 RGB 变为三倍，alpha 保持。
缓存值 1.5 可作 gain，但不能作 opacity；generic NaN 仍是合法独立 Value，却不能进入
bounded consumer。Fixture 覆盖 Scalar/Signal、五种布局、field/opaque 拒绝和独立共享取消。

```sh
cmake --build build/issue257-static --target test_computed_scalar -j 8
MTL_DEBUG_LAYER=1 MTL_SHADER_VALIDATION=1 ctest --test-dir build/issue257-static -R '^test_computed_scalar' --output-on-failure
```

C++ 与 C consumer 均报告 layouts=5 semantic_kinds=3、oracle=passed；原生硬件可用时
必须执行 45 次 dispatch。无硬件时验证 CPU/fallback 并报告零 native dispatch。修改
fixture 的 coefficient binding 或纯 producer callback 可继续组合，expression 解析由后续
算子切片完成。
