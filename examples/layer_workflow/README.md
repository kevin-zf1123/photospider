# Layer and weighted-result workflow

This standalone public C++ consumer exercises RegionalSource -> compiled DAG ->
ordinary RGBA sink, plus dynamic contribution count -> canonical weighted sum ->
OptionalLayer. See [the runtime contract](../../docs/kernel-architecture/Layer-Runtime.md)
for exact operations, arithmetic, resource scope and installation commands.

```sh
cmake --build build/issue257-shared --target photospider_layer_workflow -j8
build/issue257-shared/photospider_layer_workflow
```

Independent expected output includes RGBA `[12.5,-2,1.25,1]`, midpoint count
3/5/7 numerator zero, Na=W for varying opaque weights, and an empty validity bit
for zero total weight. Assertions also check negative/insufficient-budget cases,
strict underflow, scratch cancellation, shared duplicate outputs, and backing
retirement after the last owner. No optional result cache is configured.
