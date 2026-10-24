# 05-filter 独立数学 oracle

此目录提供[空间滤波](../../../docs/built-in_ops/05-filter/spatial.md)与
[频域、恢复](../../../docs/built-in_ops/05-filter/frequency-restoration.md)拟议规格的小尺寸数学参考。
它不调用已退役的内建滤镜，也不模拟完整的 Photospider 端口、Region、owner 或资源契约。
各成员规格列出其关联自测和证明等级；运行时验收要求见
[FILTER_oracle_protocol](../../../docs/built-in_ops/05-filter/op_specs/FILTER_oracle_protocol.md)。

## 运行

需要 Python 3.11+、系统 MPFR 4.2+ 与 LP64 ABI。可用
`PHOTOSPIDER_ORACLE_MPFR` 指向 MPFR 动态库的完整路径。诊断案例另需 `mpmath`；
NumPy/SciPy 只用于可跳过的非权威对照。Windows LLP64 不在当前 ctypes 适配范围。

在项目根目录运行：

```sh
python3 -m oracle.ops.filter.run_oracles --self-test --no-diagnostics --report build/filter-oracle/self-test.json
```

安装诊断依赖后，移除 `--no-diagnostics` 可运行完整自测。报告由命令生成在忽略的
`build/` 中；仓库不保存预生成向量或报告。BM3D、CBM3D 的第三方对照仍待补齐。FIL-19B 已有固定 native
参考和真实 GPU shader 对照；通用 manifest 演示案例的 pending 状态不代表该
专门对照不存在，见下文。

## 参考程序

| 文件 | 作用 |
| --- | --- |
| `common/core.py` | Fraction、直接 IEEE 舍入、形状与边界、矩阵解、最终四 ULP 判定 |
| `common/bounds.py` | MPFR 定向区间和有界 refinement |
| `fil*/spatial.py`、`fil*/transcend.py` | 空间滤波、导数、Gaussian、双边、Gabor 等小图公式 |
| `common/spectral.py`, `frq*/spectral.py`、`fil15/multiscale.py`, `fil17/multiscale.py`, `frq10/multiscale.py` | DFT/DCT、窗、频响、小波和金字塔 |
| `common/restoration.py`, `fil16/restoration.py`, `res*/restoration.py` | 恢复、噪声、迭代和去雾数学参考 |
| `common/comparison_protocol.py` | 第三方对照 manifest 的结构检查，不执行算法 |
| `tests.py`、`run_oracles.py` | 解析案例、性质检查、自测及可选本地报告生成 |

通用 API 主要使用二维 plane list；颜色、batch、metadata、Region 与生命周期需要未来
适配器验证。DFT 参考限制 64 pixels、DCT 与 local Laplacian 限制 36 pixels，
这些是 oracle 的工作量限制。MPFR 区间未在预算内闭合时报告 Inconclusive；
RES-07C/F 的均值域逆只提供诊断参考，不能当 strict golden。

## 覆盖边界

内部 Python 数学函数的便捷默认参数不代表公开 authoring 构造器默认值；规格要求的
参数必须由公开节点显式提供。`common/special.py` 覆盖标量乘积归约、分位数和融合
颜色窗口的 NUM 特殊值规则；卷积和分位数入口使用这些规则。其余 Fraction/MPFR
函数主要覆盖有限输入，遇到尚未支持的非有限值报告 `Inconclusive`，不表示合法输入
应在产品中报域错误。浮点最终溢出返回 Inf，迭代参考使用主输入 dtype 的阶段舍入。

尚未覆盖：全部成员的非有限值/payload 组合、早停判据与并发调度、固定 Plan 下按 band
选择的 SDK/workflow 表示、实际第三方 BM3D/CBM3D 对照。SMAA 的固定 14 图像对照和 RMSE/PSNR
范围已单独记录，不能推广为所有输入或主观画质保证。
未覆盖范围不能从成员出现在测试索引中推导为已通过。非法参考输入的诊断仅属于 oracle；
生产执行以数值、编码及关联前提成立为条件，不增加输入或输出 payload 校验。

可选均值逆诊断依赖可以隔离安装：

```sh
python3 -m venv build/filter-oracle-venv
build/filter-oracle-venv/bin/python -m pip install -r oracle/ops/filter/requirements-diagnostic.txt
build/filter-oracle-venv/bin/python -m oracle.ops.filter.run_oracles --self-test --report build/filter-oracle/full-self-test.json
```

## Gaussian 参考输入与数值

`fil04/coefficients.py` 保留 baked64 系数的固定输入和独立 MPFR
定向区间期望值；`fil04/samples.py` 保留 MPFR 系数、Fraction 完整二维表达式
和直接 IEEE 舍入。通过 `cases()` 获取输入，`expected(case)` 计算参考值，
`reference_cases()` 遍历输入与参考值。

系数集包含 312 个样本；二维集包含 94 个输入、707 个输出，覆盖两种 dtype、
五种边界、特殊值、零半径、系数下溢和不能提前窄化的 Float64 cval。
需要验证实现时现场编写适配器；参考模块不启动外部程序，也不报告实现通过。


## FIL-17..19 的扩展参考

`fil17/sparse.py` 的 `OneLevel` 按需计算 Local Laplacian 单层参考，
允许在 1024²、4096² 图像上检查边界和内部坐标，而不运行完整的二次规模
小图参考。原有 `fil17/multiscale.py` 继续负责多层小图参考。两者保留
FIL-15 每个公开阶段的 RN，不能用普通 band gain 替代。

`fil19/smaa.py` 的 `Reference(height, width, color, guide=None, components=1)`
接受像素回调；components 为 3 时按明确的 RGB 顺序计算编码域 luma。
`edge(y,x)`、`weight(y,x)` 和 `output(y,x,c=0)` 暴露三个参考阶段。
它直接使用固定 lookup 字节，并在规定节点进行 RN32，与生产 C++ 实现独立。

`fil19/samples.py` 将这个参考接入通用 runner。`evaluate(image, guide=None, coordinates=None)` 接受 H/W/C 数组（C 为 1 或 3）及可选 H/W 外部 guide，返回所选坐标的 luma、edge、weight、output。自测覆盖 Gray 退化尺寸与负零复制、阈值相等及相邻 Float32 值、局部对比抑制、四种 RGB 图案、外部 guide 和非零混合。向量保存各阶段的 Float32 位模式，可供临时生产适配器读取；`--replay` 重新计算独立参考。

```sh
python3 -B -m oracle.ops.filter.run_oracles --member FIL-19B --no-diagnostics --report build/filter-oracle/smaa.json --vectors-out build/filter-oracle/smaa-vectors.json
python3 -B -m oracle.ops.filter.run_oracles --member FIL-19B --replay build/filter-oracle/smaa-vectors.json
```

`--member` 可重复指定，省略时运行全部 oracle 用例。报告的 `scope` 仍为独立 Python oracle；生产运行时与实际 GPU 对照需另行执行。`real_golden_smaa` 的 pending 表示此 runner 未运行 GPU helper，固定资源检查单独记录为 `smaa_pinned_resources`。

`fil19/fixtures.py` 定义第三方比较的 14 个输入、RGB 像素生成函数和
`verify_pins()`。先核对资源 hash；原始 shader、lookup 及许可证不得改写。
`fil19/pinned_low.py` 提供固定 HLSL 函数体的机械 CPU 转写，供诊断使用；
它的归一化软件插值与真实硬件 texture 结果有差异，不能替代 GPU 对照。

`fil19/gpu_reference.hpp` 是可选 macOS 离线参考。调用
`photospider_oracle::smaa_gpu::upstream_gpu(image, false, areaBytes,
searchBytes, shaderPath)`，其中 image 提供 width、height 和连续的
四分量 Float32 pixels。返回 edge、weight、output 三个 Float32 readback。
使用 C++17、`-framework OpenGL`，且关闭 fast-math 与隐式 FMA contraction。
该辅助代码编译并运行未改动的 pinned GLSL；生产算子没有 OpenGL 依赖。

[明确的 native profile、资源、采样和对照容差](../../../docs/built-in_ops/05-filter/op_specs/FIL-19B_smaa_1x_native.md)
将 native 逐位验证和第三方图像误差比较分别定义。14 个图像的实际 Apple M5
GPU 比较覆盖 32²、128²、37×53、1024² 和 4096²。归一化坐标、硬件插值及
方向平局会带来局部差异；通过 suite RMSE 不意味着逐像素四 ULP 或 bit identity。

FIL-17..19 已通过临时公开运行时适配器检查；适配器与运行报告不作为 oracle
源码保存。数学参考本身仍不模拟完整的 Plan、owner、取消与资源契约。
