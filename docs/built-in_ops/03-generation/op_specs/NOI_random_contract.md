---
spec_schema_version: 1
id: NOI-random
status: AcceptedDesign
implementation_status: not_implemented
registration_status: packing_and_mapping_gate
---

# 随机共享契约：Philox4x64-10

继承[GEN-common](GEN_common_contract.md)与[已确认D08](../decisions.md)。
算法已选定；具体packing、输出word与浮点网格取位仍须技术验证后冻结。
本文不保留旧Philox4x32-10算术、packing-v1或KAT作为新规范。

## 地址与身份

x/y为已验证signed32全局整数格坐标。frame/draw/stream各为0..2^32-1，公开参数由Int64承载。
channel暂为0..65535，内置domain为8-bit，不允许作者覆写成员标签。seed保留64-bit位模式意图。
184-bit地址直接无损编码至256-bit counter；seed注入128-bit key的具体布局待冻结。
候选c0=(x,y)、c1=(frame,draw)、c2=(stream,channel,domain)、c3=0尚非稳定序列。
字节序、未使用位、字段顺序和输出word顺序必须明确；禁止截断、回绕或有损hash代替注入编码。

地址来自全局像素/格点和逻辑channel，不来自浮点坐标反推、SIMD lane、thread、tile、时钟或地址。
点候选ordinal/iteration/attempt到地址的映射须明确并检查上限，不挪用其他domain。
frame是离散序列编号；连续time成员是另一个数学坐标，不逐帧换seed。

## domain用途

| 标签 | 用途 |
| --- | --- |
| 1 | 均匀白噪声 |
| 2 | Box–Muller |
| 3 | jitter |
| 4 | 有限候选拒绝 |
| 5 | FIFO Bridson |
| 6 | seeded二维格点梯度 |
| 7 | cellular站点 |
| 8 | Poisson quantile |
| 9 | integer looks |
| 10 | 艺术grain白源 |
| 11 | 显式资源随机位移候选 |

标签分工保留，实际word映射及每个draw的消费方式须与新packing一起冻结。
整数核心跨CPU/GPU位一致；浮点变换服从具体profile。地址无别名不保证单样本不重复或跨key统计独立。

## 有限浮点网格与数值目标

保留下列数学网格目标，具体从64-bit输出提取哪组位尚未冻结，不得提前宣称新golden：
Float32 halfopen24为j/2^24；Float64 halfopen53为j/2^53；j覆盖对应整数范围。
open52为(2j+1)/2^53，j=0..2^52-1，严格位于(0,1)且可精确表示。
Box–Muller每channel独立取open52与halfopen53角度，cos分支，不跨邻点共享配对。
有限网格尾部和统计偏差须披露，不能宣称理想连续分布。

均匀整数选择采用明确的无偏rejection；候选word宽度、阈值及draw推进规则待新核心规范冻结。
draw耗尽不回绕，算法上限失败与宿主ResourceExhausted区分。
GEN-08/NOI-05的站点发布保持半开cell：显式RN64后若到上界，取nextDown上界；
无可表示内部点则拒绝。这是站点lattice规则，不是D06的周期坐标舍入规则。

## 冻结与验收

需固定官方算法版本/十轮参数、packing、seed注入、网格取位及拒绝采样，建立独立4x64 KAT。
再验证边界值、超界拒绝、不同frame/stream/channel/domain、非零origin、扩大画布、ROI和乱序同位。
缓存包括全部算法/布局/映射版本及参数，seed不是完整身份。
旧附件Python oracle与4x32 KAT不验证本规格；具体注册在门禁完成前阻断。

官方参考：[Random123](https://github.com/DEShawResearch/random123)、
[Philox4x64定义](https://thesalmons.org/john/random123/releases/latest/docs/structr123_1_1Philox4x64__R.html)。
