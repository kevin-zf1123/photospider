# 过时文档

本目录保存已不再定义当前软件行为的开发历史。这里的文件可能包含过时名称、API、中间设计或
未完成实验；它们只用于说明历史决策。

## 已归档的内核材料

`kernel-architecture/` 保存历史报告和迁移材料。下列文件已于 2026-07-14 从维护中的内核架构
文档集合移出：

- `kernel-architecture/Compute-Service-Split.md`：已完成的 compute-service 重组计划；
- `kernel-architecture/Benchmark-Spikes.md`：尚未形成稳定架构结论的拟议实验。

对应的中文读者副本保存在 `kernel-architecture/zh/`。没有中文源副本的历史文档继续只作为
历史材料，不会被追溯认定为维护中文档。

## 当前事实来源

- 产品使用：`readme.md` 与 `manual.md`；
- 当前内核行为：`docs/kernel-architecture/zh/README.zh.md`；
- 已接受的长期决策：`docs/adr/zh/`；
- 已接受的未来架构：`docs/roadmap/zh/Kernel-Evolution.zh.md`；
- 维护中的开发验证指南：`docs/development/zh/Testing-and-Validation.zh.md`。

使用归档陈述前，必须对照当前代码与上述维护中文档核验。
