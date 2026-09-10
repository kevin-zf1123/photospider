# G4 workflow

The current `data` scenario runs a public WorkflowDocument/Compiler/
ExecutionContext identity workflow over a logical billion-element source.
It requests coordinates 1 and 999999998 separately through the existing
regional executor, constructs exact ValueFragments, checks that the middle hole
cannot be read, checks a per-output certificate transpose, and imports a generic
Int64 snapshot. Only two actual source samples are read. This scenario exercises
the data foundation; staged scheduling scenarios are added with their runtime
implementation.

```sh
cmake --build build/issue257-static --target photospider_g4_workflow -j 8
build/issue257-static/examples/g4_workflow/photospider_g4_workflow
```

Expected checked output:

```text
data: values=[1,999999998], source_reads=2, hole=rejected, transpose={1}, generic_snapshot=ok
```

It also builds as a standalone installed public package consumer:

```sh
cmake --install build/issue257-static --prefix "$PWD/out/g4-install-static"
cmake -S examples/g4_workflow -B out/g4-consumer-static \
  -DCMAKE_PREFIX_PATH="$PWD/out/g4-install-static"
cmake --build out/g4-consumer-static -j 8
out/g4-consumer-static/photospider_g4_workflow
```
