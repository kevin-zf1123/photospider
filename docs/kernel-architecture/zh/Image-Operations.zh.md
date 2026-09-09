# Float32 图像算子

默认 registry 包含 CPU 算子，实现位于
[`plugins/ops/image_operations.cpp`](../../../plugins/ops/image_operations.cpp)。
下列两个 S1 算子都有两个有序 runtime Value input，无 compile-time parameter 或隐式默认值，
输出一个由 workflow 命名的区域图像。

| Operation | Input 0 | Input 1 | Output |
| --- | --- | --- | --- |
| `image.exposure_gain` | 图像 | Float32 gain scalar，闭区间 [0,16] | RGB 乘 gain，alpha 逐 bit 复制 |
| `image.opacity` | 图像 | Float32 opacity scalar，闭区间 [0,1] | 全部 RGBA channel 乘 opacity |

图像声明必须为 dense Float32 {H,W,4}，H/W 为正，whole Region，零 offset，canonical
row-major stride；运行视图带有显式 origin、stride 和有效 Region。两者精确包含一个 facet：key `photospider.image`，version 1，payload
`rgba;linear-srgb;premultiplied;hwc`，共 34 个 ASCII byte，不含 NUL。
RGB 必须 finite 且非负，alpha 必须 finite 且位于 [0,1]，alpha 为零时 RGB 必须全零。
HDR RGB 可以超过 1 或 alpha，接受 signed zero。Caller 提供已转换为 linear-sRGB
premultiplied 的值；不执行 color conversion、gamma、clamp 或 unpremultiplication。

Scalar input 必须直接引用 workflow declaration：Float32 {1}、whole Region、零 offset、
stride {4}、四字节且无 facet。每次运行的 gain/opacity byte 不属于 source parameter，
也不改变 compiler identity。

两者为 deterministic、side-effect-free、cacheable、PreserveFirstInput 和 Elementwise。
Image input demand 为请求的空间 output demand，完整包含四个 channel；scalar demand
始终为 whole {1}。小范围 demand 只返回所请求 Region，保留逻辑 descriptor。
每个 image step 经宿主分配输出，无重复 sink copy；完整预留包含保留中间结果和 scratch，
调用方已有输入和进程 RSS 单列。

每个运算阶段按 IEEE binary32 nearest、ties-to-even 舍入，保留 gradual underflow。
Host schema/numeric validation 和 image callback scope 保存并恢复 thread 浮点环境，
避免继承的 rounding 或 flush-to-zero 模式改变结果。计算出的 non-finite pixel、错误
profile 或 alpha-zero/nonzero-RGB output 返回 OperationFailed。绑定 pixel/scalar
数值域错误返回 InvalidArgument：scalar 在执行前检查，pixel 在消费 callback 前检查，
未读取像素不扫描。

## 可复用算子包与可执行示例

[`plugins/ops/rgba32f`](../../../plugins/ops/rgba32f/CMakeLists.txt) 仅通过
`Photospider::operation_sdk` 构建受维护的 ABI4 C module `photospider_rgba32f_ops`。
它实现相同图像算子和 profile，使用严格浮点编译选项。ABI4 host 在进入 callback
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
0.6 消费及 0.5 拒绝，参见[测试与验证](../../development/zh/Testing-and-Validation.zh.md)。

## S2 Gaussian、蒙版与合成

默认 registry 和受维护 ABI4 C 包还提供：

| Operation | 有序输入 | 必填静态参数 | Region 规则 |
| --- | --- | --- | --- |
| `image.gaussian_blur` | RGBA 图像 | `radius:Int64 [1,64]`、`sigma:Float64 [0.1,64]` | 从 radius 解析 Halo，完整 RGBA |
| `image.mask` | RGBA 图像、Float32 `{H,W}` 蒙版 | 无 | Elementwise，蒙版映射相同 H/W |
| `image.source_over` | 前景 RGBA、相同 shape 的背景 RGBA | 无 | Elementwise、MatchAllInputs |

三个算子均为 CPU、确定且无副作用，保留图像逻辑 shape 和上述 profile。蒙版无 facet，
样本有限且在 `[0,1]`，逐像素缩放前景全部 RGBA。Source-over 按预乘值对每个通道计算
`F + B * (1 - F.alpha)`，遵循 [W3C 公式](https://www.w3.org/TR/compositing-1/#porterduffcompositingoperators_srcover)。
减法、乘法和加法分别舍入到 Float32，不使用 FMA。

Gaussian 按 `-radius..radius` 顺序计算归一化 binary64 `exp(-tap²/(2*sigma²))` 系数。
先横向再纵向，每遍按该顺序累计 binary64 乘积，再将该遍输出舍入到 Float32。边缘 clamp
到完整逻辑图像边界，不在 tile 边界单独 clamp。radius/sigma 属于源码参数，修改需要重新
编译。Workspace 上限为固定 1032 字节系数加需求输入字节数的一倍；横向 scratch 只保留
需求行和输出列。系数、scratch 和输出全部通过宿主分配器申请。C++/C 均禁用 fast-math
和 FMA contraction。

[`photospider_regional_image_vertical`](../../../examples/regional_image_vertical/main.cpp)
通过公开 compile/execute/execute_stream 运行
`foreground -> Gaussian -> exposure -> mask -> source-over(background)`。
`S2Image.RegionAndTiles` 验证手算均匀场景（RGB .3125、alpha .625）、独立整图二维
Gaussian oracle（`atol=1e-6, rtol=1e-5`），以及整图与 1x1/2x3/5x7/128x128 tile 的逐位
一致。覆盖非零 ROI、边缘、不可整除 tile、radius 64、sigma .1、透明/HDR、蒙版 0/1、
逐次 gain 复用，以及非法参数/蒙版/shape。独立 oracle 不复用算子 callback 或两遍实现。

65536x65536 程序化源场景以九个 tile 流式处理 5x7 ROI，核对样本、9900 字节源读取和 1808 字节实际分配峰值，
验证 3840 字节保守预留恰好足够和少一字节。此处证明受控缓冲区上限，不代表进程 RSS。
区域源、fan-out、并发 Run、取消、stale 和 sink 失败覆盖位于 test_regional_execution
与 test_memory_liveness。

```sh
cmake --build build/issue257-static --target photospider_regional_image_vertical -j 8
build/issue257-static/examples/regional_image_vertical/photospider_regional_image_vertical
ctest --test-dir build/issue257-static -R '^test_(s2_vertical|s2_vertical_plugin|regional_execution|installed_consumer)$' --output-on-failure
```

示例目录也可作为独立 find_package(Photospider 0.6) 消费者。test_installed_consumer
针对隔离 static/shared 安装构建并运行它，分别使用内置算子和单独构建的 C module。
唯一可选参数为可信 module 的精确路径。

## S3 box 缩小与圆章

package 0.6 / operation ABI 6 的内建与 C 模块提供 image.downsample_box、
mask.downsample_box、image.brush_circle。前两者分别接收既有 RGBA 图像和 HW 蒙版，
必需静态 Int64 factor 为 [1,16]，无隐式默认。输出 H/W 除以 factor 向上取整，
反向需求为裁剪后的整数 box。按行/列 binary64 累加，以实际覆盖样本数平均并舍入
binary32。因子 1 保留数值。不转换 gamma 或解除预乘。应用代理默认 factor 4。

image.brush_circle 输入依次为 image、x、y、radius、red、green、blue、alpha；后七项
均为必需运行期 Float32 {1} 绑定，无静态参数。输出保持图像形状，使用 Elementwise。
x/y 为任意有限 Float32，radius 为正 normal Float32 至 FLT_MAX；非预乘线性 RGB 为
[0,FLT_MAX]，alpha 为 [0,1]。使用 binary64 平方距离判断闭圆内像素中心；圆内 RGB
先以 binary32 乘 alpha，再无融合地对预乘背景执行 source-over；圆外保留原位。
一事件一硬边圆章，不抗锯齿、不补点、不处理压力或设备。应用规划裁剪包围 ROI
并将结果作为快照 patch。

test_s3_operations [trusted-module] 通过公开 compile/execute 使用独立 box 分配
和圆公式验证，覆盖奇数尺寸、边缘 ROI、因子 1/2/4/16 与无效标量。可复用交互
示例由 #275/#277 跟踪。

## S4 原生 Metal 实现

package 0.6 / operation ABI 6 的内置适配与独立 C11 模块通过相同宿主 GPU 服务实现
八个算子，共用 image.metal 与参数转换。CMake 在构建目录生成 shader 字符串头；
安装消费者不依赖源码路径或 Objective-C++ 配置。

PlanningOptions::execution_mode 默认 CpuExact；显式 MetalFp32 允许近似原生实现。
ExecutionContextConfig::gpu_enabled=true 尝试建立真实 Apple Silicon 设备；不支持时
按算子回退 CPU。原生数值域保守：图像/mask 非零样本绝对值至少 1e-20、至多
FLT_MAX/1024；mask 乘数、gain/opacity、圆章颜色/alpha 非零值至少 1e-8；Gaussian
正系数低于 1e-8 回退。空间尺寸须适合 uint32。这些规则只选择实现，其他合法输入
继续使用完整 CPU 契约，非法输入仍失败。

关闭 fast-math/contraction，使用补偿累加、宿主 double 系数。每算子和代表链对独立
oracle 的 atol=1e-6、rtol=1e-5 不构成 CPU 位相同或与图规模无关的总误差保证。
圆章由宿主 double 行区间保持大坐标覆盖，GPU 颜色计算保留圆外像素位型。

test_metal_images 与插件版本覆盖八算子、whole/tile/非零 ROI、radius 64、factor 16、
HDR/subnormal 回退和大坐标圆章。公开 fixture 位于 examples/s4_gpu_workflow/image_fixture.hpp。
运行对应构建目标后，以 MTL_DEBUG_LAYER=1 MTL_SHADER_VALIDATION=1 执行 ctest
-R '^test_metal_images'。无硬件明确 skip，不宣称原生验收成功。CPU 精确默认保持。
