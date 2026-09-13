# 外存轴 FFT 运行时

英文权威文档：[External-FFT.md](../External-FFT.md)。

Package 0.10 在 `plugin/fft_operation.hpp` 提供 C++ 工厂，C operation ABI
保持 9。实现使用 structured protocol 2 和公开 managed Result/临时存储 API，
不依赖外部 FFT 库、GPU provider 或完整单轴 RAM 分配。

## 身份与阶段

`fft_spectrum_spec(H,W,packing,atol,rtol)` 构造封闭 HW profile：变换轴和存储
顺序为 `[0,1]`，shift/origin 为零，step 为一，坐标单位为 `sample`，正变换
采用负号且 Unscaled。packing 支持 Full 或沿宽度轴的 R2CHalf；策略固定为
RealProjectionMeasured，容差有限且非负。不支持的 rank、axes/order、shift、
sign、normalization、origin/step/unit 或 policy 均明确拒绝。H/W 为正，且
`H*W <= (INT64_MAX-4095)/16`。

| 算子 | 输入 | 输出 |
| --- | --- | --- |
| `fft.forward_real` | 无 facet、有限 Float64 HW | 完整 Spectrum |
| `fft.import_response` | 无 facet、有限 Float64 HW2 或 HK2 | 按显式指定基底解释的完整 Spectrum |
| `fft.multiply` | 两个 schema 完全一致的完整 Spectrum | 完整逐点复数乘积 |
| `fft.inverse_real` | 完整 Spectrum | 完整实部投影和实测虚部残差 |

静态验证在 source 读取和 continuation 分配前比较完整 schema。W=4、W=5
的 packed K 都是 3，不能仅凭样本数判定身份。公开 registry 也在启动生产者
前比较推导输出和调用者给出的输出 metadata。频率响应是显式数值乘子，不隐含
空间重采样、shift 或归一化转换。

正变换在整个 HW 域上执行不缩放的负号 DFT；逆变换两轴使用正号，最后把各
分量除以转成 binary64 的 HW，且只除一次。运算使用 binary64 最近偶数舍入、
渐进下溢、无 FMA 收缩，恢复调用者浮点状态。复乘检查四次乘法和两次加减。
非有限输入返回 InvalidDomain；非有限算术返回 ArithmeticOverflow。

`photospider.fft_real_output` 包含 H*W 个 Float64 `pixels` 和一个 Float64
`imaginary_residual`。`fft_inverse_identity_v1` facet 保存完整正变换 Spectrum
身份。pixels 是逆变换实部；残差是整个域上 max(abs(imag(inverse/HW)))，只是
有限的实测诊断，不是认证误差上界。该 recipe 不自动统一正负零。

## 外存 DIF recipe

每条轴长度写为 L=2^s*m，其中 m 为奇数。在两个完整复数临时数组 A/B 中逐轴
处理逻辑连续数据；每条记录 16 字节，以固定宽度寻址，不驻留逐页目录。

1. 把有限源样本写入 A，或把半谱展开到 A。
2. 对 span=L,L/2,...,2*m 执行原位 DIF 蝶形。先读两个原半部，再写
   `(a+b)` 和 `(a-b)*exp(sign*2*pi*i*j/span)`。大 span 使用两个有限窗口；
   小 span 把多个完整组批处理到一个连续窗口。
3. 对自然频率 k，读取 `bitreverse(k mod 2^s,s)` 号奇数叶，并执行频率
   `floor(k/2^s)` 的 direct DFT。累加跨页保持叶样本递增顺序。所有叶读取
   未改变的 A，结果按自然频率顺序写入 B，不覆盖后续输入。m=1 只需重排。
4. 通过有界 gather 把 B 转置到 A，再变换另一个轴。最终写入 Result 时把
   转置存储 gather 回声明的 HW 顺序。H=1 省略不必要的第二轴。

逐层 DIF 选择频率低位，得到 `k=bitreverse(leaf,s)+2^s*q`。L=12 时，物理
leaf/frequency 枚举的自然频率为 `0,4,8,2,6,10,1,5,9,3,7,11`。纯奇数轴
使用平方复杂度 direct DFT，不声称任意长度都有 O(L log L) 复杂度。

两个临时文件分别 Create，再 Extend 已检查的 16*HW 字节，随后才能依赖新
范围读写。Extend 是独立 coordinator action，实际使用 4096 字节清零暂存和
对齐磁盘准入。文件保持可写的私有 scratch，代际之间不 seal。所有写入完成
后才改变角色。每个窗口最多 `min(user_page_bytes,1024)` 字节，至少能容纳
16 字节复数记录。声明的 callback workspace 为 4096 字节；读取回复、窗口
metadata 和保留 buffer 还会按实际所有权计入根预算。奇数叶输出 buffer 和
gather 批次均有显式、有限的所有权。

完整奇数叶能放入窗口时，一个阶段请求最多 16 个自然输出频率，并限制全部
叶回复合计不超过一个窗口。各频率保留原累加顺序和正零初值。批次可跨逻辑
轴行，每个请求重新计算行基址；更大的奇数叶继续逐段累加。transpose/output
的 gather 每阶段也最多处理 16 条记录。

算术工作量为 O(HW*(log2(W/mw)+mw+log2(H/mh)+mh))，奇数叶直接求和可能
占主要成本。每遍扫描、蝶形、累加、初始化和 I/O 都计账。强制 scratch 为两
份分别准入的完整代，加上最终 Result 和仍存活的上游 owner。stage、work、
I/O 或容量不足时明确失败，不退款已经提交的工作。默认示例使用有限的
200000 阶段上限；独立的 1024×1024 Full/Half 用例对每个生产者使用 1000000
阶段及有限 work/I/O 预算。通用默认 4096 阶段不保证容纳所有大变换。
输出的根 `issued_stages` 累计所有生产者的 coordinator action，与各生产者
的 poll 上限不同。

## 实数策略与发布

K=floor(W/2)+1，缺失项为 `F[u,v]=conj(C[(-u) mod H,W-v])`，同时反射两个
频率轴。Nyquist 列不必全实；只有两个轴都自共轭的点必须实数。不插入系数
投影来隐藏 Hermitian 缺陷。

Spectrum 保持现有 CompleteBundle 契约。运行时在 observer 或消费者可见
之前检查所有有限分量及适用共轭对是否满足声明的 atol/rtol。复乘后重新验证；
两个输入分别通过同一绝对容差，不保证乘积仍通过。拒绝 Spectrum 时返回
TypeMismatch、Association scope。浮点验收通过不产生 Exact 或 CertifiedBound。

私有 scratch 在最后复制页写入完成后释放，为额外 Spectrum 验证窗口留下
容量。验证也分页，但反射访问可能重复加载窗口，不能计为一次顺序扫描。
Conservative 全局变换关系保留完整源和 descriptor 支持。Result association、
cache-off 共享 owner 及逃逸读取窗口共同保留强制存储，直到最后一个 owner
释放。

[公开 workflow](../../../examples/fft_workflow/README.md) 提供运行命令、独立
direct DFT 和循环位移参考、半谱/全谱/Nyquist、超过工作区的轴、有界失败及
生命周期检查。数值差异是 fixture 实测符合；资源峰值是产品 managed-capacity
计账，不是进程 RSS 硬上界。
