#!/bin/sh
# Run from a patched source checkout; a failed regression is not suppressed.
set -eu
root=$(CDPATH= cd -- "$(dirname -- "$0")/../.." && pwd)
build=${1:-"$root/build-fmt11"}
: "${CC:=clang}" "${CXX:=clang++}" "${JOBS:=4}"
export CC CXX
if [ ! -f "$root/third_party/sleef/src/libm/sleefsimddp.c" ]; then
  echo 'Place the supplied SLEEF 3.9.0 source at third_party/sleef before configuring.' >&2
  exit 2
fi
cmake -S "$root" -B "$build" -G Ninja -DCMAKE_BUILD_TYPE=Release -DBUILD_TESTING=ON \
  -DCMAKE_C_COMPILER="$CC" -DCMAKE_CXX_COMPILER="$CXX"
cmake --build "$build" --target test_model_math test_model_conversion test_model_metadata \
  test_model_rational test_numeric_neighbors test_matrix_certificate \
  test_metadata_assignment test_channel_assembly test_channel_editing \
  test_channel_extraction test_numeric_conversion test_numeric_operations \
  photospider_model_conversion_performance --parallel "$JOBS"
ctest --test-dir "$build" --output-on-failure \
  -R '^test_(model_math|model_conversion|model_metadata|model_rational|numeric_neighbors|fmt11_package_version|matrix_certificate|metadata_assignment|channel_assembly|channel_editing|channel_extraction|numeric_conversion|numeric_operations)$'
