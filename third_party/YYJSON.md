# Private CLI JSON dependency

yyjson 0.13.0, commit `6447536015f3d600f3d65323b10976103b337ca7`:
[upstream source](https://github.com/ibireme/yyjson/tree/6447536015f3d600f3d65323b10976103b337ca7)

## Prepare the source before configuring

`psc` and `psx` require the pinned, unmodified yyjson source when
`PHOTOSPIDER_BUILD_TOOLS=ON` (the default). Builders must place `yyjson.c`,
`yyjson.h` and `LICENSE` in `third_party/yyjson/`. This directory is ignored by
Git and is absent from repository checkouts and Git-generated source archives.
The preparation instructions in this file remain part of the repository.

Run these commands from the Photospider repository root:

```sh
mkdir -p third_party/yyjson
curl --fail --location \
  https://raw.githubusercontent.com/ibireme/yyjson/6447536015f3d600f3d65323b10976103b337ca7/src/yyjson.c \
  --output third_party/yyjson/yyjson.c
curl --fail --location \
  https://raw.githubusercontent.com/ibireme/yyjson/6447536015f3d600f3d65323b10976103b337ca7/src/yyjson.h \
  --output third_party/yyjson/yyjson.h
curl --fail --location \
  https://raw.githubusercontent.com/ibireme/yyjson/6447536015f3d600f3d65323b10976103b337ca7/LICENSE \
  --output third_party/yyjson/LICENSE
```

For an offline build, obtain these three files from the same pinned commit on a
connected machine and copy them into this directory before configuring. No
separate yyjson build or installation is required. CMake reads the local files
directly; it does not download them or verify their version or commit. Builders
are responsible for supplying the pinned source. Set
`PHOTOSPIDER_BUILD_TOOLS=OFF` to disable the tools and their yyjson requirement.

## Private integration

`cmake/CompilerTools.cmake` compiles yyjson into the private
`photospider_cli_json` static library. File helpers and non-standard JSON
extensions are disabled; reader and writer depth limits are 128. yyjson
declarations do not enter installed kernel headers or the exported kernel target.

License: `yyjson/LICENSE` (MIT). When tools are enabled, installation copies the
license to `share/licenses/Photospider/yyjson/LICENSE` under the default install
data directory.
