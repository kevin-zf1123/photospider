# ADR 0006：文档分离事实、决策与交付状态

- 状态：已接受，由 ADR 0015 收窄
- 日期：2026-09-01 边界修订

## 背景

读者需要区分已实现行为、架构决策与当前交付状态。混合这些时间含义，会让未实现
target 看起来已是当前行为，或让 obsolete roadmap 继续授权已删产品领域。

## 决策

Active documentation 使用三个层次：

| 层次 | 权威 | 内容 |
| --- | --- | --- |
| 当前事实 | `docs/kernel-architecture/` | checked-out tree 中的行为、所有权、invariant、limitation 与 source/test entry point |
| 决策 | `docs/adr/` | 已接受边界、理由、后果、supersession 与显式 non-goal |
| 交付状态 | GitHub Issue/Project 加 active OpenSpec change | 具体 task、dependency、实际 verification、risk 与 completion state |

ADR 0015 是最高 active 产品边界决策。低层事实、Issue、Project field、archived
OpenSpec 或历史 tag 都不能覆盖它。Breaking reset 后不再有 active roadmap layer；
保留的 compiler 与 heterogeneous-execution 工作由维护中的 architecture、OpenSpec
和已收窄 live tracking 描述。

### Promotion workflow

把计划行为提升为当前事实需要一个连贯 change：

1. 实现行为与 long-lived test；
2. 更新相关英文 current-fact document 与中文镜像；
3. 只有决策改变时才更新受影响 ADR；
4. 使用实际 test result 更新 active OpenSpec task 与 live Issue/Project state。

Status checkbox 本身绝不证明当前行为。反过来，当 installed contract 与 maintained
documentation 仍描述旧边界时，代码也不算完成。

### Archive rule

Git 历史、annotated tag 与 archived OpenSpec change 可以保留历史文本。Active index
不得把 archive 链接成当前权威。被删领域标记为 removed、out of scope 或
archive-only；不得描述为 later、future、deferred、optional 或 default-disabled。

### 语言一致性

英文文档是权威来源。每个维护的 official document 和 OpenSpec artifact 都在同一
change 中更新忠实、面向读者的中文镜像。镜像不引入额外 requirement。

## 后果

- 阅读当前 architecture 无需重建迁移历史。
- Breaking retirement 显式呈现，而不是隐藏在 stale link 后。
- Delivery record 引用真实 code 与 test result，但不成为产品权威。
- 干净 primary clone 无需 personal workflow data 也能理解。
