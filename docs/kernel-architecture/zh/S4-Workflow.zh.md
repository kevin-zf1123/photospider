# S4：原生 Metal 图像工作流

[独立示例](../../../examples/s4_gpu_workflow) 只使用安装公开 API，覆盖 S1 至 S3
八个图像/mask 算子。默认 CPU 精确；显式 MetalFp32 使用 Apple Silicon shared
buffer，在设备或数值实现不适用时按算子回退，不承诺 CPU 位相同或自动加速。

## 构建与运行

从仓库根目录执行：

```sh
cmake --build build/issue257-static --target photospider -j 8
cmake --install build/issue257-static --prefix build/s4-prefix
cmake -S examples/s4_gpu_workflow -B build/s4-example-installed \
  -DCMAKE_PREFIX_PATH="$PWD/build/s4-prefix"
cmake --build build/s4-example-installed -j 8
cmake -S plugins/ops/rgba32f -B build/s4-module-installed \
  -DCMAKE_PREFIX_PATH="$PWD/build/s4-prefix"
cmake --build build/s4-module-installed -j 8
build/s4-example-installed/photospider_s4_gpu_workflow --backend cpu
build/s4-example-installed/photospider_s4_gpu_workflow --backend metal --no-cache --explain
build/s4-example-installed/photospider_s4_gpu_workflow --backend metal --layout tiled
build/s4-example-installed/photospider_s4_gpu_workflow --backend metal --layout roi
```

追加 --module /absolute/path/to/libphotospider_rgba32f_ops.so 使用可信 C 模块；
macOS 的 CMake MODULE 同样生成 .so，以实际路径为准。SDK 为纯 C，宿主拥有设备、
队列、pipeline 和 buffer 寿命。PHOTOSPIDER_ENABLE_METAL=OFF 验证 CPU 回退构建；
其他平台 GPU 可选。--require-native 在无原生硬件时返回 77，CTest 明确 skip；普通
示例仍验证并报告 CPU 回退结果。

## 可检查场景

| 场景 | 要求 |
| --- | --- |
| resident-chain --backend metal --no-cache | whole 17x13：5 dispatch、4 submission、4 次输入复制共 7960 字节、结果收集复制 3536 字节、零回退、oracle=passed |
| resident-chain --layout tiled | 4x4 tile，包含截断边缘，对 whole oracle |
| resident-chain --layout roi | y=[2,9)、x=[3,12)，完整 RGBA，区域样本正确 |
| all-operations | 八算子、whole/非零 ROI 分块，支持时有真实 dispatch |
| cache-edits | 热运行零 dispatch/上传，gain 保留 blur，局部圆章重算少于 20 个 blur tile，无关编辑零回调，原生保留容量有界 |
| preview-export | 9 个有序圆章、20 个固定导出 tile、最新全质量预览、3 次拒绝过期/质量降级/目标不符发布、独立输入与输出 oracle |
| fallback | 设备关闭与数值回退保持 CPU 结果/错误，大坐标圆章覆盖精确 |

用 --scenario NAME 选择；--layout、--no-cache、--explain 仅用于 resident-chain。
原生验收必须看到实际 dispatch。复制计数是写入原生 buffer 的实际工作，GPU 后继
复用。shared buffer 主机访问不新增 D2H 分配；收集 tile 到独立主机输出确实复制，
单独报告。explain 输出计划候选与容量，缓存/回退决定实际工作；设备时间、宿主
回调和 execute 时间分开，没有硬件耗时通过阈值。

## 修改与组合

image_fixture.hpp::scene(kind) 提供每算子的 WorkflowDocument、bindings 和独立
参考图。main.cpp 选择执行模式，workflow.hpp 复用 S3 编辑与固定导出协调器。
S3 Scene 可指定模式与 tile 大小，默认仍为 CPU exact、4x4。

最小调用依次为 GraphContext(scene.document)、Compiler(registry)、设置
PlanningOptions::execution_mode=MetalFp32、compile，随后以 gpu_enabled=true
构造 ExecutionContext 并 execute(plan, scene.bindings)。访问结果前检查 ok()，
输出名为 result。可变像素和普通标量绑定复用计划；静态 radius/sigma/factor 修改
文档并重编译；WorkflowNodeOutput 连接 DAG，Region/tile 是物理规划参数。
FrozenExecution 固定编辑前的输入。端口、范围、图像 profile、Region 和保守原生
数值域见 Image-Operations。CPU 精确缓存与 Metal 隔离，Metal 派生结果不写磁盘。

## 调试与限制

M5 验收实际使用了本机 Xcode 工具。在创建设备前设置 MTL_DEBUG_LAYER=1 和
MTL_SHADER_VALIDATION=1，例如运行上述独立示例的 all-operations --backend metal
--require-native。英文指南链接 Apple validation 与 GPU capture 官方文档。
Capture 是按问题选择的调试工具，不自动生成交付 artifact。

每个 context 一个原生队列，包含取消在内均在退役前等待已提交工作结束。
S4 仍逐算子等待并由 CPU 验证共享像素；减少这些成本、device-private storage、
其他后端和实测自动选址留到后续工作。

原生可用性包含 shared buffer 容量探针。S4 要求小 buffer 实际容量等于请求、较大
分配符合页面容量界限；其他分配模型走 CPU 回退。例如 macOS CI 的 Apple Paravirtual
设备对 3536 字节请求实际分配 16384 字节。其硬件用例在检查 CPU 回退后明确跳过；
真实 M5 dispatch 仍是原生验收依据。CPU 回退通过不表示支持这种不同的原生分配模型。
