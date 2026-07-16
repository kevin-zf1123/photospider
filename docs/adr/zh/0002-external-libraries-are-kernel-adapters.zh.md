# ADR 0002：外部库是内核 Adapter

## 状态

作为目标约束已接受；当前实现尚未完成该迁移。

## 背景

公共 operation 和 Host 契约已经使用 Photospider 值类型，但私有 Graph、ROI、dirty propagation、
planning、cache 和 runtime 代码仍使用 OpenCV geometry 与 image object。YAML 也同时承担图文件
格式和内部 runtime value model。这使图像处理库与序列化库成为内核语义的一部分。

该耦合阻塞了独立 geometry 优化、可替换 operation provider、in-memory graph definition、
通用数据类型，以及不依赖 OpenCV/yaml-cpp 的内核构建。

## 决策

内核拥有自己的语义类型和最小原语：

- checked geometry、ROI、extent、grid、scale 和 tile 数学；
- planning/execution 所需的 stride-aware buffer view 以及最小 copy/fill/validation operation；
- format-neutral parameter value、graph definition 和 error contract。

OpenCV 是可选 adapter/provider。OpenCV image view、算法、初始化、exception translation 和
codec 保持在 operation、buffer adapter 或 codec interface 后方。Graph、propagation、planning、
cache 或 runtime interface 不出现 OpenCV 类型。

Graph persistence 通过 format-neutral reader/writer contract 注入。YAML 继续作为一种受支持的
filesystem adapter，但 `YAML::Node` 不再作为 runtime parameter、output、cache metadata 或
graph-state value model。

所有权迁移必须完整完成：新旧语义类型不会通过永久 forwarding wrapper 共存。

## 结果

- 无外部依赖的内核构建成为架构验收项。
- 内核原语刻意保持很小；Photospider 不会重新实现通用图像处理库。
- 算法质量与 codec policy 在编排之外保持可替换。
- Graph load/reload/save 需要显式 transaction 和 error matrix。
- Operation provider 必须声明并发与资源行为，不能用隐藏的进程级库锁表达。
- 现有行为需要分阶段纵向迁移和回归测试；本 ADR 不表示迁移已经完成。
