# 实施依赖与执行路线

本页概述当前可复用能力和仍未交付的算法范围。规格状态、注册状态、oracle 覆盖和平台支持分别记录；Proposed 不表示当前未注册，Accepted 也不代表实现完成。

## 仍需专用决策的内容

- PathSet fill/stroke 的 AA、几何误差、宽度自变量与 overlap，以及索引和 dirty 范围。
- 更多采样核、缩小预滤波和负瓣 alpha 策略；Lab/LCh gamut mapping 和颜色管理资源。
- 迭代 solver 的成功/近似消费、停止条件、真实误差证据；有限 residual 不推导 CertifiedBound。
- RAW 处理状态、模型版本/许可、视频 timebase 与 Deep 合成的数据和算法契约。
