# 外存轴 FFT 运行时

英文权威版本：[External-FFT.md](../External-FFT.md)。

## 1. 模块边界与职责

安装后的 CPU operation factory 使用必需临时存储和有界窗口实现二维实数 FFT。`fft_operation.hpp` 提供公共工厂；coordinator 负责 Result 发布、Root 资源准入和输入关联。变换期间 operation 持有两代复数临时数据；每个输出 Result 拥有其发布的 backing。

## 2. 核心数据结构与内存布局

```cpp
Result<SpectrumSpec> fft_spectrum_spec(
    std::uint64_t height, std::uint64_t width,
    SpectrumPacking packing = SpectrumPacking::R2CHalf,
    double atol = 1e-10, double rtol = 1e-12);
Result<OperationDefinition> make_fft_operation(
    FftOperation operation, const SpectrumSpec& spectrum);
```

工厂要求 H、W 为正数，并检查 `H*W <= (INT64_MAX-4095)/16`。它固定两个变换轴及存储顺序 `[0,1]`、零 shift 和 origin、单位采样步长、`sample` 单位、负号且不缩放的正变换，以及 full 或沿 width 轴的 R2CHalf 存储。实数策略为带有限非负 Hermitian 容差的 `RealProjectionMeasured`。rank、axes、order、shift、sign、normalization、origin、step、unit、packing 或 policy 不匹配时会报错，不会静默重解释。

Spectrum samples 是 Float64 实部/虚部对，按声明的慢到快轴顺序存储。注册的四个 key 如下：

| Key | 输入 | 输出 |
| --- | --- | --- |
| `fft.forward_real` | 一个 unbatched、无 facet、无 fields 的 Float64 HW tensor Result | 完整 Spectrum |
| `fft.import_response` | 一个 unbatched、无 facet、无 fields 的 Float64 HW2 或 HK2 tensor Result | 按显式频率基底构造的完整 Spectrum |
| `fft.multiply` | 两个描述完全一致的完整 Spectrum | 完整逐点复数乘积 |
| `fft.inverse_real` | 完整 Spectrum | 实部投影及实测虚部残差 |

ForwardReal 与 ImportResponse 的数值输入依据 tensor type 和精确 shape 验证，不要求固定的输入 Result schema ID。Multiply 和 InverseReal 的 Spectrum 输入必须匹配完整声明 schema。

`photospider.fft_real_output` 保存 H*W 个 Float64 像素和一个 Float64 `imaginary_residual`。`fft_inverse_identity_v1` facet 保留完整 Spectrum 契约。残差为 `max(abs(imag(inverse/HW)))`，是实测诊断值，不是误差证书。

## 3. 调度与状态机

Producer 在 source 读取或 continuation 分配前检查完整 Spectrum identity。Width 为 4 和 5 时 packing 后列数都是 3，因此 packed 样本数不能证明 identity 相同。Registry 也会在 producer 启动前检查推导的输出 metadata。Whole-transform dependency support 为 Conservative；输入编辑可以使所有输出变脏。Tensor support 按逻辑 tensor sample 定位，Field support 按字段行定位，metadata 使用独立 Descriptor support。每个 Spectrum field row 存储一对实部/虚部，因此 relation 计数使用复数行数，而非单独 Float64 分量数。

```text
variant Result binding
       |                                      |
       v                                      v
pixels source --Need--> Float64 HW Result   response source --Need--> Float64 HW2/HK2 Result
       |                                      |
       +-------------------+------------------+
                           v
 real source --forward：spool A -> transform -> transpose A/B -> 第二轴--> Spectrum
 response Result --import_response：直接有界复制 ------------------------> Spectrum
Spectrum + Spectrum --multiply：有界逐点复数乘法 ---------------------> Spectrum
Spectrum --inverse：展开半谱 -> transform -> transpose A/B -> 投影 ---> spatial Result
Spectrum/Result 校验 --------------------------------------------+-----> 发布
                                                                  +-----> 拒绝
```

Forward 和 inverse 创建两代完整复数临时数据，各扩展到经检查的 `16*H*W` 字节。Import 和 multiply 不运行 DIF。创建、扩展、依赖写入和 drain 是不同 coordinator stage。窗口上限为 `min(user_page_bytes,1024)`，且至少能容纳一个 16 字节复数记录。Callback workspace 为 4096 字节；读回复、保留的窗口 metadata、输出批次和 gather buffer 另计入 Root。Transpose、odd-leaf 输出和最终 gather 分批处理，不覆盖源 generation 后续仍需读取的值。每个 producer 的 stage 上限由工厂声明的 1,000,000、`DependencyLimits::maximum_stages`（默认 4096）和 Root stage budget 共同约束。运行未完成时不会发布完整 Result。必需 scratch 在最后一次复制后释放。Result association 只存储源 ObjectId，不保活源 payload。已加载的 field `CpuStorage` 会保留其 read plan 和 Result 实现，使字段 backing 在 context 和 Result wrapper 释放后仍可读取，直到最后一个 window 释放。

## 4. 算法与数学

每条变换轴长为 `L=2^s*m`，其中 `m` 为奇数。Forward/inverse 对 span `L,L/2,...,2*m` 执行 DIF radix-2 阶段，然后按递增样本序计算奇数长度叶变换。自然频率 `k` 对应叶索引 `bitreverse(k mod 2^s,s)` 和奇数频率 `floor(k/2^s)`。Producer 从未修改的 generation A 读取，按自然序写入 B，再把 B 转置回 A 以处理另一轴。H=1 时跳过第二轴。纯奇数轴使用直接 DFT，复杂度为二次；整体算法不保证普遍的 `O(L log L)`。

对实数输入 `x[y,x]`，Forward 使用负号且不缩放：

$$
F[u,v]=\sum_{y=0}^{H-1}\sum_{x=0}^{W-1}x[y,x]e^{-2\pi i(uy/H+vx/W)}.
$$

Inverse 使用正号，并且只将每个分量除以一次转成 binary64 的 `H*W`。输出 `photospider.fft_real_output` Result 保存 H*W 个 Float64 像素和一个 Float64 `imaginary_residual=max(abs(imag(inverse/HW)))`；`fft_inverse_identity_v1` facet 保留完整 Spectrum 契约。Residual 是测量值，不是误差证书。

算术采用 binary64 最近偶数舍入、渐进下溢、不收缩 FMA，并恢复调用者浮点环境。复数乘法检查四个乘积和两次加法。非有限输入返回 `InvalidDomain`；非有限算术返回 `ArithmeticOverflow`。

R2CHalf 中 `K=floor(W/2)+1`，省略值满足 `F[u,v]=conj(C[(-u) mod H,W-v])`，反射同时作用于两轴。Nyquist 列不必整体为实数；两轴都自共轭的点必须为实数。运行时不会投影系数来掩盖 Hermitian 缺陷。ExactHermitian 直接比较有限实部/虚部。RealProjectionMeasured 将存储值 `a` 与镜像值 `m` 的共轭比较，并按下式接受：

$$
|a-\overline{m}| \le atol + rtol\max(|a|,|m|)。
$$

相对项使用分离指数的 binary64 fraction 计算以避免溢出。CompleteBundle 在 consumer 取得 Spectrum 前校验有限分量和适用镜像对。两个通过校验的输入相乘后仍可能产生不合格结果。任何接受模式都不认证变换误差。

## 5. 限制与非目标

- 实现是分页 CPU recipe，不依赖外部 FFT 库或 GPU provider。
- 奇数轴的直接变换可能主导耗时；算法并非普遍的 `O(N log N)`。
- Work、I/O、窗口、磁盘或根容量耗尽也会使运行失败；schema 准入通过不代表所需资源已预留。
- Spectrum 容差和 inverse residual 是接受/诊断测量值，不是误差界。
- 镜像校验可能重读反射窗口，其 I/O 不保证是单次顺序扫描。
- Managed-capacity 计账不构成进程 RSS 保证。

公共入口与可运行用法见 [FFT workflow](../../../examples/fft_workflow/README.md)。
