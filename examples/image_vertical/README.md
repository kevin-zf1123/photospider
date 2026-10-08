# Image vertical Result workflows

This example runs built-in image operations through the public Result and execution APIs. It keeps source Results and execution under the same `ResourceBudget` root and uses no legacy planar image module or special module CLI.

## Build and run

From the repository root, build and run this example in an existing test build:

```sh
cmake --build build/kernel-dev --target photospider_image_vertical
build/kernel-dev/examples/image_vertical/photospider_image_vertical
```

This executable takes no arguments. It also supports an independent build against an installed Photospider package:

```sh
cmake -S examples/image_vertical -B build/image-vertical-consumer -DCMAKE_PREFIX_PATH=<install-prefix>
cmake --build build/image-vertical-consumer
sh
```

## Small full-graph and raw benchmark example

`photospider_image_vertical` builds an RGBA image Result with cell shape `{2,2,4}` and batch extents `{1,1}` for frame and layer, so its complete sample shape is `{1,1,2,2,4}`. It binds two unbatched `{1}` Float32 control Results, compiles one graph containing `image.exposure_gain` followed by `image.opacity`, and requests one output pixel with its complete four-channel tuple. The requested output is at frame 0, layer 0, y 0, x 1. The dependency report checks that source image support is exactly that pixel and that each scalar control uses its complete sample.

The fixture contains binary fractions and checks every output channel against an independent binary-fraction oracle. It executes two source/control bindings through the same compiled plan and requires different output digests. The raw benchmark then checks two repetitions for each binding against the same oracle; its four benchmark samples must retain the compiled plan and result identities. The program reports the output values, execution timing, peak payload observed by the shared root, and oracle result.

 See [Image operations](../../docs/kernel-architecture/Image-Operations.md) for the current built-in image contract.
