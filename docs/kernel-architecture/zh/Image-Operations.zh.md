# Float32 图像算子

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

## S2 Gaussian、蒙版与合成

默认 registry 和受维护 ABI9 C 包还提供：

| Operation | 有序输入 | 必填静态参数 | Region 规则 |
| --- | --- | --- | --- |
| `image.gaussian_blur` | RGBA 图像 | `radius:Int64 [1,64]`、`sigma:Float64 [0.1,64]` | 从 radius 解析 Halo，完整 RGBA |
| `image.mask` | RGBA 图像、Float32 `{H,W}` 蒙版 | 无 | Elementwise，蒙版映射相同 H/W |
| `image.source_over` | 前景 RGBA、相同 shape 的背景 RGBA | 无 | Elementwise、MatchAllInputs |

三个算子均为 CPU、确定且无副作用，保留图像逻辑 shape 和上述 profile。蒙版带有 `encode_semantic(coverage_semantics())`，
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

示例目录也可作为独立 find_package(Photospider 0.7) 消费者。test_installed_consumer
针对隔离 static/shared 安装构建并运行它，分别使用内置算子和单独构建的 C module。
唯一可选参数为可信 module 的精确路径。

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

## S4 原生 Metal 实现

package 0.9 / operation ABI 9 的内置适配与独立 C11 模块通过相同宿主 GPU 服务实现
八个算子，共用 image.metal 与参数转换。CMake 在构建目录生成 shader 字符串头；
安装消费者不依赖源码路径或 Objective-C++ 配置。

PlanningOptions::execution_mode 默认 CpuExact；显式 MetalFp32 允许近似原生实现。
ExecutionContextConfig::gpu_enabled=true 尝试建立真实 Apple Silicon 设备；不支持时
按算子回退 CPU。原生数值域保守：图像/mask 非零样本绝对值至少 1e-20、至多
FLT_MAX/1024；mask 乘数、gain/opacity、圆章颜色/alpha 非零值至少 1e-8；Gaussian
正系数低于 1e-8 回退。空间尺寸须适合 uint32；原生视图须有非负步长，byte offset
与步长须按四字节对齐，各轴 storage origin 不得超过 demand offset（图像通道 origin
为零）。其他合法原生前驱按次调用回退。这些规则只选择实现，其他合法输入
继续使用完整 CPU 契约，非法输入仍失败。

关闭 fast-math/contraction，使用补偿累加、宿主 double 系数。每算子和代表链对独立
oracle 的 atol=1e-6、rtol=1e-5 不构成 CPU 位相同或与图规模无关的总误差保证。
圆章由宿主 double 行区间保持大坐标覆盖，GPU 颜色计算保留圆外像素位型。

test_metal_images 与插件版本覆盖八算子正值与 signed/HDR whole/tile/非零 ROI、
alpha 0/1/1e-10、非法数值/facet/association、radius 64、factor 16、
HDR/subnormal 回退和大坐标圆章。公开 fixture 位于 examples/s4_gpu_workflow/image_fixture.hpp。
运行对应构建目标后，以 MTL_DEBUG_LAYER=1 MTL_SHADER_VALIDATION=1 执行 ctest
-R '^test_metal_images'。无硬件明确 skip，不宣称原生验收成功。CPU 精确默认保持。

可独立安装消费的 examples/s4_gpu_workflow 通过公开 WorkflowDocument、compile、execute
运行同一 signed 场景：

```sh
cmake --build build/issue257-static --target photospider_s4_gpu_workflow -j 8
build/issue257-static/examples/s4_gpu_workflow/photospider_s4_gpu_workflow --scenario all-operations --backend cpu
build/issue257-static/examples/s4_gpu_workflow/photospider_s4_gpu_workflow --scenario all-operations --backend metal --require-native
```

追加 `--module /absolute/path/to/libphotospider_rgba32f_ops.so` 使用 C 包。预期输出包含
`operations=8`、`signed_hdr=passed`、`oracle=passed`。CPU 和符合资格的原生执行
报告 `fallback_count=0`，原生执行必须报告非零 dispatches。无设备时 Metal 模式报告
真实正数 fallback count；传入 `--require-native` 还会以 77 退出。每个场景检查所有需求样本和 typed facet。例如 signed exposure
在 `(y=0,x=1)` 将 `[-2.125,4,-.125,.5]` 以 gain 2 转为 `[-4.25,8,-.25,.5]`。
修改 image_fixture.hpp 的 foreground、background、brush 输入或 scene() 参数即可组合
其他实验，同时更新独立 oracle。整图与 2x3 tile 的非零 ROI 共用数学 oracle；RGB
结果不会仅因负号被夹紧或回退 CPU。

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

## 局部 Navier-Stokes 修补（PNT-05A）

[`PNT-05A_local_inpaint_navier_stokes.md`](../../built-in_ops/09-composite/op_specs/PNT-05A_local_inpaint_navier_stokes.md)
修订 0.3.0 冻结了 profile `opencv_4_12_ns_f32_planar_v1`：OpenCV 4.12.0
`INPAINT_NS` 按 R、G、B 顺序作用于三个单通道 Float32 平面。必须提供两个显式 key，
不带后缀的 `image.local_inpaint_navier_stokes` 只是语义族名称，不是第三个注册或别名。

| 算子 | 输入 0 | 输入 1 | 参数 | 具名输出 |
| --- | --- | --- | --- | --- |
| `image.local_inpaint_navier_stokes_openCV` | Image | Float32 `{H,W}` 规范 coverage | 必填 Int64 `radius`，闭区间 `[1,32]` | `image`，Float32 `{H,W,4}` |
| `image.local_inpaint_navier_stokes_native_apple_silicon` | Image | 同上 | 同上 | 同上 |

图像端口是类型化 `SemanticKind::Image` 的 Float32 三阶约束：固定 `RgbaFloat32`
端口无法表达 profile 允许的 scene 或 display reference。callback 校验其余精确语义：
规范线性 sRGB D65、coverage 预乘 association、四通道及其规范角色、恰好一个 facet，
reference 为 `scene` 或 `display`。其他 association、primaries、白点、transfer、
model、通道集合或多余 facet 均以 `TypeMismatch` 失败；发布结果逐字节保留输入 facet。

洞 mask 保持精确的规范 `Float32Mask` 端口。两个输入都必须有限，alpha 必须恰为 1，
mask 采样必须恰为 `0` 或 `1`；两种符号零都是已知采样，`0.5` coverage 虽然合法但不是
本 profile 的合法输入。全零 mask 的 identity 与算法进入之前都会校验完整逻辑域，因此
洞内非有限 RGB 占位、非不透明 alpha 与非二值 coverage 在 noop mask 下同样失败。
`K=0` 返回未改动输入位，`K=H*W` 以 `OperationFailed` 失败，空间尺寸小于 3 以
`TypeMismatch` 失败，任一轴超过 32768 以 `ResourceExhausted` 失败，逻辑网格不一致的
mask 被拒绝。只写入洞内 RGB 采样；未遮蔽采样与全部 alpha 采样保持位相同，洞内结果
不做 `[0,1]` 截断。

Region 规则为 Whole：任意合法非空查询都有 `need_image=All([H,W,4])` 与
`need_hole_mask=All([H,W])`，非零输出 Region 是同一完整计算的裁剪，任何输入改动都会
使整个输出失效。radius 不是传递 halo。两个算子都只支持 CPU，不做隐式 GPU 回退。
两个变体共享校验、洞置零、0/255 mask 生成、平面打包、写回、发布以及下文的算术异常契约。

### 算术异常契约

有限的洞内结果可能掩盖非有限中间量，例如相邻 ±1e30 使 `VectorLength(gradI)` 溢出。
两个变体在每个通道求解前清除 `FE_INVALID | FE_OVERFLOW | FE_DIVBYZERO`，求解后检查，
使该通道以 `OperationFailed` 失败而不是发布看似有限的值。外层 image scope 恢复调用者
完整浮点环境，因此调用者的舍入模式与粘滞异常标志保持不变，干净调用不留下任何标志。

### 取消观测频率

校验、输出拷贝、mask 生成、平面打包、洞检查与原生 guard 网格初始化都按逻辑采样计数，
每 4096 个采样（含全部 RGBA 与 mask 读取）至多观测一次取消。每个按采样计数的 helper 在
进入时观测一次取消，输出分配前后各观测一次，被取消的通道先报告 `Cancelled` 再报告脏算术
状态，因此 helper 边界与阶段切换都是观测点，不存在跨越两个阶段的未计数尾部。直接
registry 调用会原样返回该 callback 状态，因此 callback 内的顺序是必要的。原生初始化对十字膨胀窄带
构造的每次 guard 网格访问计数，并在同一受检循环中重置每通道标志网格，而不是整块存储。
移植的前沿每 64 次 pop 或 4096 次候选循环访问（取先到者）观测一次，并在每次大分配、
通道切换与发布前后观测。按行取模不满足该界限，未被使用。OpenCV adapter 无法中断库
调用，在打包前、最后一个打包分块之后以及每个通道调用之后观测，并声明该限制而不声称
达到前沿观测频率。

### 变体边界

OpenCV adapter 每通道打包一个 Float32 平面、将洞采样置零、每通道调用一次库。库自身的
guard 网格与堆向量分配不计入 invocation allocator；`estimated_external_bytes` 报告该
估算（`f`/`band`/`mask`/`t` 每 padded 采样 7 字节，插入序堆向量按 2 倍几何增长容量每
padded 采样最多 32 字节），并且从不算作宿主执行预算。库拒绝分配（`cv::Error::StsNoMem`、
宿主 `std::bad_alloc`）映射为 `ResourceExhausted`，其他库失败映射为 `OperationFailed`。

原生 Apple Silicon 变体是固定单通道 `icvNSInpaintFMM<float>` 前沿、`FastMarching_solve`
与窄带构造的授权独立移植，保留 Intel License Agreement 声明，且不含任何 OpenCV 头、
符号或链接；使用 `-DPHOTOSPIDER_ENABLE_OPENCV_INPAINT=OFF` 得到 native-only 构建。
其全部 scratch 来自 invocation allocator：打包工作平面（4N）、内部 UInt8 mask（N）、
padded state/band/flags 三元组（3P）、padded 到达时间（4P）以及 16 字节条目
`{float T, int32 y, int32 x, int32 order}` 的有界堆（16P），即 `5N+23P` 字节全部计入
宿主，并在任何失败路径释放。

固定的索引分支、guard 网格、`1.0e6f` 初始化与插入顺序都保留。`FastMarching_solve`
保留原 `double a11, a22, m12` 局部变量及其 binary64 比较、混合项与 `1+m12` 求和，只有
返回值收窄为 Float32。前沿保留原 binary32 的 `dst`、梯度、模长与 `(double)Ia/s`
表达式，使用 round-to-nearest 与渐进下溢。

规范 0.3.1 绑定唯一的 arm64 OpenCV 4.12.0 参考构建，使用 `-fno-fast-math
-frounding-math -ffp-contract=off`；两个变体都与该参考逐位一致：验收集合的 5100 个
洞内采样差异为 0，独立公共 harness 对每个变体均为 64/64，包括其 `large-frontier`
512x512 随机洞用例。移植保真度另有直接证据：把原 `inpaint.cpp` 原样编入一个诊断翻译
单元并使用本内核浮点标志，在该 fixture（每通道 65262 个洞内采样）上与已注册原生算子
比较，三个通道的差异采样数均为 0。两个变体对相同调用也逐位可重复。

历史记录：包管理器分发的 `libopencv_photo` 启用了 FMA 收缩（`icvInpaint` 内 48 条
`fmadd`、18 条 `fmsub`）。同一源码以 `-ffp-contract=fast` 编译可逐位复现该分发版本，
而不收缩版本在同一 512x512 fixture 上与其偏差可达冻结容差的 15 倍，因为顺序洞依赖会
放大末位差异。该测量正是 profile、adapter 链接与原生移植统一绑定同一非收缩参考、而不
使用分发版本的原因；原生翻译单元使用共享算子标志，没有单独覆盖。

### 证据

[`test_local_inpaint_navier_stokes.cpp`](../../../tests/integration/test_local_inpaint_navier_stokes.cpp)
覆盖冻结验收矩阵，复现协调方 harness 的 fixture（radius 1/3/8/32 的块状洞、超出上界的
尺寸、display reference、非有限中间量、异常标志恢复），并以同一源码重建为已安装
consumer。映射、命令、证据与当前缺口记录在
[`examples/inpaint_ns_workflow/README.md`](../../../examples/inpaint_ns_workflow/README.md)。
独立 oracle 直接调用固定库处理 Float32 平面，洞采样置零并还原未遮蔽采样，绝不调用生产
helper，且仅在 `pkg-config opencv4` 报告 4.12.0 时链接。

```sh
cmake -S . -B build/inpaint-ns -G Ninja -DCMAKE_BUILD_TYPE=RelWithDebInfo \
  -DBUILD_TESTING=ON -DPHOTOSPIDER_ENABLE_METAL=OFF -DBUILD_SHARED_LIBS=OFF \
  -DCMAKE_OSX_ARCHITECTURES=arm64
cmake --build build/inpaint-ns --target test_local_inpaint_navier_stokes \
  photospider_inpaint_ns_workflow -j 3
ctest --test-dir build/inpaint-ns \
  -R '^(test_local_inpaint_navier_stokes|example_inpaint_ns_workflow)$' \
  --output-on-failure
```

`-DCMAKE_OSX_ARCHITECTURES=arm64` 是本 profile 的必要条件：已提供的 OpenCV 4.12.0
构建仅含 arm64，翻译运行的 x86_64 配置无法链接。`pkg-config opencv4` 缺失或版本不符时
adapter 以警告禁用，原生变体不受影响。
