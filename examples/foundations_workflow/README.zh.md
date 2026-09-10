# 可组合基础 workflow

本目录是自包含 C++17 示例，只消费安装后的 Photospider 0.7。可复制到任意目录，使用
安装前缀构建；仅依赖 photospider/photospider.hpp 与公开 WorkflowDocument、Compiler、
ExecutionContext 等 API，不需要 tests/internal helper、媒体文件、daemon 或插件源码。
[英文说明](README.md)为权威文档。

## 构建与运行

从内核仓库根目录配置、构建、安装内核，再使用安装包构建示例：

```sh
cmake -S . -B build/foundations-kernel -DCMAKE_BUILD_TYPE=RelWithDebInfo \
  -DBUILD_TESTING=OFF
cmake --build build/foundations-kernel --target photospider -j 8
cmake --install build/foundations-kernel --prefix "$PWD/build/foundations-prefix"
cmake -S examples/foundations_workflow -B build/foundations-example \
  -DCMAKE_PREFIX_PATH="$PWD/build/foundations-prefix"
cmake --build build/foundations-example -j 8
build/foundations-example/photospider_foundations_workflow --scenario all
```

共享内核在内核 configure 命令增加 `-DBUILD_SHARED_LIBS=ON`。将示例目录复制到仓库外
后，进入复制后的目录，使用安装包的绝对路径构建：

```sh
cmake -S . -B build -DCMAKE_PREFIX_PATH=/absolute/path/to/foundations-prefix
cmake --build build -j 8
build/photospider_foundations_workflow --scenario all
```

无需运行时数据文件。退出零表示所选独立 oracle 全部通过；失败输出原因并返回一。
`--help` 列举场景，默认 all。此示例使用 CPU exact；真实 GPU 验收见 [S4](../s4_gpu_workflow)。

## 场景与结果

用 `--scenario NAME` 独立运行：

| Name / 文件 | 输入与组合 | 可检查结果 |
| --- | --- | --- |
| cast-range / numeric.cpp | 全部 UInt8 0..255 range/cast 往返，Float64 halfway/边界/非有限，Float32 减法与归约 | 256 bytes 精确；ties `[-2,-2,0,0,2,2]`；溢出拒绝/显式 clip；负 ramp `[-1,-2,-3]`；mean=2、variance=2/3 |
| channels / color.cpp | Premul linear-D65 RGBA `[-2,3,4,.5]`，extract→multiply red→merge；BGR 往返 | Identity 精确，红翻倍 `[-4,3,4,.5]`，其余不变；BGR 往返精确 |
| alpha-color / color.cpp | Signed/HDR straight RGBA，alpha=.5/1e-30，associate/unassociate，alpha0 隐藏色，D65 RGB/XYZ/Lab 与 D50 XYZ/Lab | round-trip 容差 1e-5 scale，alpha bits 不变；隐藏色归零；错误 unit/facet/association 拒绝 |
| expression-lut / generators.cpp | Float64 系数，x² 在 0,.5,1 采样，LUT query=.25，同 key count=5 | `[0,.25,1]`、`.125`、`[1,1.5,2,2.5,3]`；非法 AST/非有限子表达式拒绝 |
| generator-gain / generators.cpp | Snapshot RGBA，动态 Float64 系数，count=1 的 c[0]*2→gain，同 plan 顺序/并发 | 独立绑定结果，实际 warm cache hit；fresh/cached 非法 gain 和 NaN 系数进入 gain callback 零次 |
| components / components.cpp | 空 mask、bridge、3x3 对角棋盘、signed field→threshold→labels→count | 空 labels/count 零及非空零表；bridge area=5/bbox `[0,0,3,2]`；稳定 IDs `[1,0,2,0,3,0,4,0,5]`；超容量拒绝 |

All 最后一行 `Foundations scenarios=6 oracle=passed backend=cpu`。每行成功信息在
对独立常量/分数 oracle 核查后打印，cache-hit 数值为实际诊断。

所有新增算子 Whole，组件场景即使规划 1x1 tile 仍跨整幅连接。每个 Value shape 非零且
完整覆盖。既有 gain 保持 image Region 规则并在消费前验证完整 Float32 `{1}`。
Signal 的采样轴与样本单位分别声明；示例均 dimensionless。颜色保留 signed，不隐式
夹 gamut 或适应参考白。

## 修改与组合

workflow.hpp 的 document()/bindings()/evaluate() 分别创建公开输入声明、每 Run
绑定、compile/execute。Compile-once 参考 generator_gain()，保留 GraphContext、plan
与 ExecutionContext，替换同 descriptor 的 Value。用 WorkflowInputReference 接输入，
WorkflowNodeOutput 接上游 value；doc.outputs 指定命名结果。

静态参数、系数表 shape、count、merge 目标、组件容量变化须重编译；动态系数/像素内容
可复用 plan。用 encode_semantic、semantic_parameter、channel_indices_parameter 构造
元数据，不手写 payload。每节点单输出，labels 分支到独立 count/area/bbox；各 capacity
独立，未用槽全零。

Cast 显式 dtype/rounding=ties_even/overflow=reject 或 clip；range 增加递增的源/目标
端点；算术 Float32/64 同 dtype/shape，无隐式 broadcast。内建 merge 接受 2..4 个同
dtype/shape 的 HW 输入并显式建立 target descriptor：Image 要求 Float32 和 3/4 通道，
VectorField 接受 Float32/64 和 2/3 通道，ComplexField 接受 Float32/64 和 2 通道。
expression 必填 expression/count/start/step，LUT 必填 out_of_domain=reject 或 clip。
Threshold 显式 .5；capacity 为 Int64 `[1,2^53-1]`，registry 不补默认。

Gain 场景的公开 registry wrapper 只计 callback 次数，保留原 traits 并委托默认 registry。
Snapshot 支持区域缓存，紧凑 Whole 系数符合 2048-byte direct Value 资格。单独预热合法
signal 20 后，作为 gain 时违反 `[0,16]` 并拒绝。

完整参数/错误契约与 focused tests 见[基础 workflow 指南](../../docs/kernel-architecture/zh/Foundations-Workflow.zh.md)。
本示例交付到 ops 线；daemon 0.6 迁移、G4/G6、FFT、完整路径、ICC/OCIO 是独立工作。
