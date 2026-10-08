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
python3 oracle/ops/filter/run_oracles.py --self-test --no-diagnostics --report build/filter-oracle/self-test.json
```

安装诊断依赖后，移除 `--no-diagnostics` 可运行完整自测。报告由命令生成在忽略的
`build/` 中；仓库不保存预生成向量或报告。SMAA 1x、BM3D、CBM3D 缺少固定第三方对照与
真实像素结果，相关案例保持 pending。

## 参考程序

| 文件 | 作用 |
| --- | --- |
| `oracles/core.py` | Fraction、直接 IEEE 舍入、形状与边界、矩阵解、最终四 ULP 判定 |
| `oracles/bounds.py` | MPFR 定向区间和有界 refinement |
| `oracles/spatial.py`、`oracles/transcend.py` | 空间滤波、导数、Gaussian、双边、Gabor 等小图公式 |
| `oracles/spectral.py`、`oracles/multiscale.py` | DFT/DCT、窗、频响、小波和金字塔 |
| `oracles/restoration.py` | 恢复、噪声、迭代和去雾数学参考 |
| `oracles/comparison_protocol.py` | 第三方对照 manifest 的结构检查，不执行算法 |
| `tests.py`、`run_oracles.py` | 解析案例、性质检查、自测及可选本地报告生成 |

通用 API 主要使用二维 plane list；颜色、batch、metadata、Region 与生命周期需要未来
适配器验证。DFT 参考限制 64 pixels、DCT 与 local Laplacian 限制 36 pixels，
这些是 oracle 的工作量限制。MPFR 区间未在预算内闭合时报告 Inconclusive；
RES-07C/F 的均值域逆只提供诊断参考，不能当 strict golden。

## 覆盖边界

内部 Python 数学函数的便捷默认参数不代表公开 authoring 构造器默认值；规格要求的
参数必须由公开节点显式提供。`oracles/special.py` 覆盖标量乘积归约、分位数和融合
颜色窗口的 NUM 特殊值规则；卷积和分位数入口使用这些规则。其余 Fraction/MPFR
函数主要覆盖有限输入，遇到尚未支持的非有限值报告 `Inconclusive`，不表示合法输入
应在产品中报域错误。浮点最终溢出返回 Inf，迭代参考使用主输入 dtype 的阶段舍入。

尚未覆盖：全部成员的非有限值/payload 组合、早停判据与并发调度、band 延迟载荷的
运行时表示、实际第三方 BM3D/CBM3D/SMAA 对照以及图像质量评分。未覆盖范围不能从成员出现在测试索引中推导为已通过。

可选均值逆诊断依赖可以隔离安装：

```sh
python3 -m venv build/filter-oracle-venv
build/filter-oracle-venv/bin/python -m pip install -r oracle/ops/filter/requirements-diagnostic.txt
build/filter-oracle-venv/bin/python oracle/ops/filter/run_oracles.py --self-test --report build/filter-oracle/full-self-test.json
```

## Gaussian 原生运行时对照

`check_gaussian_coefficients_runtime.py` 通过原位 IEEE 传输对照实际 C++ baked64
builder 与独立 MPFR 定向区间；`check_gaussian_runtime.py` 对照公开 CPU Whole、tiled 或 GPU
workflow 与 MPFR 系数、Fraction 完整二维表达式和直接 IEEE 舍入。

```sh
python3 oracle/ops/filter/check_gaussian_coefficients_runtime.py --runner build/kernel-dev/test_gaussian_coefficients
python3 oracle/ops/filter/check_gaussian_runtime.py --runner build/kernel-dev/test_gaussian_workflow
python3 oracle/ops/filter/check_gaussian_runtime.py --runner build/kernel-dev/test_gaussian_workflow --tiled
python3 oracle/ops/filter/check_gaussian_runtime.py --runner build/kernel-dev/test_gaussian_gpu
```

前者包含 312 个系数比较，后者包含 94 个 workflow、707 个输出位比较，覆盖两种
dtype、五种边界、特殊值、零半径、系数下溢和不能提前窄化的 Float64 cval。
参数错误、owner、非平凡布局、work/capacity 预算和取消由原生 integration tests
单独验证。`--tiled` 运行实际 Regional 实现。GPU 校验使用 `--runner build/kernel-dev/test_gaussian_gpu`，要求原生 dispatch 且无 fallback。
这些检查不构成所有输入的完备证明。使用 Vulkan kernel build 时，同一 runner
选择 SPIR-V；配置 kernel 时传入 `-DPHOTOSPIDER_ENABLE_VULKAN=ON`。

Vulkan Gaussian 在 FreeBSD Intel UHD 770 上通过真实 public workflow。94 个
workflow 的 707 个输出 bit 均匹配 MPFR 系数、Fraction 表达式和直接 IEEE 舍入；
focused test 也通过。Validation-layer log 无 VUID、Validation Error 或
SYNC-Hazard。相同 `test_gaussian_gpu` runner 可在 Vulkan 内核构建中执行 SPIR-V；
Intel 验证记录见 `out/gpu-whole-tiled/raw/gaussian-vulkan-intel-oracle.json`、
`gaussian-vulkan-intel-tests.log` 和 `gaussian-vulkan-oracle-validation.log`。
Metal focused test 也通过，见 `gaussian-vulkan-metal-test.log`。此证据限定于
记录的有限 fixture；NVIDIA 和 Linux Gaussian Vulkan 尚未验证，oracle 对照本身
不测性能。
