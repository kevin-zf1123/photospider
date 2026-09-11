# G4 workflow

The current `data` scenario runs a public WorkflowDocument/Compiler/
ExecutionContext identity workflow over a logical billion-element source.
It requests coordinates 1 and 999999998 separately through the existing
regional executor, constructs exact ValueFragments, checks that the middle hole
cannot be read, checks a per-output certificate transpose, and imports a generic
Int64 snapshot. Only two actual source samples are read. This scenario exercises
the data foundation.

The `progressive` scenario registers a C++ staged operation through the public
registry, compiles a billion-element workflow and executes it with one CPU worker
and a 128-byte controlled allocation budget. Control samples 0, 1 and 3 discover
a payload read at 999999999. The source callbacks reject every unexpected read;
the independent expected result is 17.25 and exactly 32 source bytes.
No full input materialization occurs.

The `stmap` scenario uses the built-in sampler with an upstream computed map and
wrap boundary. Its source rejects every read except pixels 0 and 1023. The
`radius` scenario patches a distant Int64 radius through InputSnapshotStore;
scatter changes from 1 to 5, gather stays 1, and the frozen old execution stays 1.
Operator contracts are in [Dependency Sampling](../../docs/kernel-architecture/Dependency-Sampling.md).

```sh
cmake --build build/issue257-static --target photospider_g4_workflow -j 8
build/issue257-static/examples/g4_workflow/photospider_g4_workflow
```

Expected checked output:

```text
data: values=[1,999999998], source_reads=2, hole=rejected, transpose={1}, generic_snapshot=ok
progressive: value=17.25, controls=[0,1,3], payload=[999999999], source_bytes=32, budget=128
stmap: red=0.5, source_pixels=[0,1023], source_bytes=32
radius: scatter_before=1, scatter_after=5, gather=1, frozen=1
```

It also builds as a standalone installed public package consumer:

```sh
cmake --install build/issue257-static --prefix "$PWD/out/g4-install-static"
cmake -S examples/g4_workflow -B out/g4-consumer-static \
  -DCMAKE_PREFIX_PATH="$PWD/out/g4-install-static"
cmake --build out/g4-consumer-static -j 8
out/g4-consumer-static/photospider_g4_workflow
```
