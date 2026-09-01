# ADR 0008：Value、Layout 与 Region 是显式验证契约

- 状态：已接受，由 ADR 0015 收窄
- 日期：2026-09-01 边界修订

## 背景

Typed compilation 与 local heterogeneous execution 需要一种 runtime value，不能从 raw
pointer 推断 logical shape 或 addressed bytes。Breaking baseline 有意让该契约保持足够
小，从而可以完整验证。

## 决策

一个 immutable dense `Value` 精确包含：

- `ValueDescriptor`：一个 closed `ElementType` 与 rank 1..8 的 nonzero shape；
- 一个 rank-matching logical `Region`，由 unsigned half-open interval 组成；
- 一个 `StridedLayout`，包含 byte offset 和每个 axis 的 signed byte stride；
- 0..64 个 versioned `ValueFacet` record，具有 unique printable-ASCII key 和 bounded
  opaque payload；
- 一个共享的 immutable owned byte vector。

Closed element vocabulary 为 `UInt8`、`Int64`、`Float64`。只有完整 addressed-range
validation 证明每个 element byte 都位于 owned vector 内时，signed stride 才允许 reversed
或 broadcast view。

`Value::create` 在原子 publication 前检查 rank、nonzero extent、Region
rank/containment、element vocabulary、stride count、signed multiply/add overflow、offset
bound、element tail、完整 addressed byte range、facet key/version uniqueness、per-facet
payload size 与 aggregate facet payload size。Facet 在 publication 前按 key 排序。
Failure 不返回 partial Value。副本共享 immutable bytes，且不暴露 writable pointer。

`Region` 是 rank-general logical coverage，不是 byte range。构造与 containment 使用 checked
unsigned addition；`element_count` 使用 checked multiplication。任一 empty interval 会让完整
Region 为空。

### Identity 与 local transfer

Logical descriptor/Region/layout fact 与 allocation address、backend label、optional
reproducibility digest 相互独立。`Value` 本身没有 durable 或 daemon identity。

当 local execution 跨 backend label 时，会用复制的 immutable bytes 创建另一个 validated
Value。Transfer/residency observation 位于 owning `ExecutionRun`，不进入 persistent Value
registry。

### Data definition

Data-definition ABI 可以为 operation/Value semantics 注册 bounded schema key、element type
与 maximum rank。Registry record 在 startup 被复制并 freeze。该 ABI 不增加 storage 或
construction service。

## 边界

当前契约没有 blocked layout、readiness fence、writable producer binding、general
serialization API、durable artifact、receipt、retention、recovery 或 storage-service
semantics。Facet 是 bounded semantic record，不是 memory owner 或 extension-code handle。

## 结果

- Compiler、operation 与 runtime check 共享一个小型 dense Value model。
- Malformed 或 duplicate facet 在 Value publication 前失败。
- Negative/broadcast stride 通过完整 range validation 保持安全。
- Cross-backend copy 不能发布 malformed layout 或 stale result state。
- Value 或 digest 绝不意味着 persistence 或 authorization。
