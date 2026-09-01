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

## Archive-only 边界

本目录中的文件只提供历史背景。Active index 不会把它们链接成产品权威，也不得
使用它们恢复已删除领域。当前事实来源是：

- 产品 surface：`readme.md`；
- 当前 kernel 行为：`docs/kernel-architecture/zh/README.zh.md`；
- 最高产品边界决策：
  `docs/adr/zh/0015-breaking-product-boundary-scope-reset.zh.md`；
- 维护中的验证指南：`docs/development/zh/Testing-and-Validation.zh.md`。

引用归档陈述前，必须对照当前代码与这些维护文档核验。本目录中的 Job/service、
worker-process、policy、trust、isolation、durable-result 与 evidence material 始终
属于 archive-only。
