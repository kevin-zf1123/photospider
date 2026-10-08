# Local inpainting Result workflow

This standalone C++ example binds typed image and coverage Results to the public
`image.local_inpaint_navier_stokes_native_apple_silicon` operation, selects the
named `image` output, and verifies a constant-color result. The input is a 5x5
linear-sRGB premultiplied RGBA image with one center hole, radius 3, and an
opaque alpha channel. The check also confirms that the output retains the image
semantic facet.

The workflow uses Photospider package 0.32.0. Build it from the repository with:

```sh
cmake --build build/kernel-dev --target photospider_inpaint_ns_workflow -j 8
build/kernel-dev/examples/inpaint_ns_workflow/photospider_inpaint_ns_workflow
```

To build it as an installed-package consumer, configure with the directory that
contains `PhotospiderConfig.cmake`:

```sh
cmake -S examples/inpaint_ns_workflow -B build/inpaint-consumer \
  -DCMAKE_PREFIX_PATH="$PWD/build/kernel-dev/consumer-install"
cmake --build build/inpaint-consumer -j 8
build/inpaint-consumer/photospider_inpaint_ns_workflow
```

The executable accepts an operation key as its first argument. A build with the
optional OpenCV 4.12.0 adapter can select
`image.local_inpaint_navier_stokes_openCV`; the default selects the native CPU
implementation. Neither key selects a GPU backend.

The optional adapter was built against OpenCV 4.12.0 with
`-fno-fast-math -frounding-math -ffp-contract=off`. The native-only
consumer executable has no unresolved OpenCV symbols. These checks do not cover
deterministic frontier-cancellation injection or host Stale-priority injection;
OpenCV's internal allocations also remain outside the kernel Root's hard
capacity accounting.
