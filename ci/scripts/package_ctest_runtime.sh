#!/usr/bin/env bash

set -Eeuo pipefail

# @file package_ctest_runtime.sh
# @brief Package one configured CTest tree without compiler increment state.
#
# The archive keeps runtime libraries, plugins, executables, generated package
# configuration, and CTest control files. Object files, CMakeFiles trees,
# Ninja dependency/log databases, prior Testing output, and the two registered
# transient nested-smoke work roots remain outside the runtime archive. The
# same object-free archive can therefore serve runtime-label jobs and build
# smokes that create fresh nested build trees. Downstream smoke changes are not
# saved back into either immutable producer handoff cache.
#
# @param $1 Existing configured and completely built CMake binary directory.
# @param $2 Destination path for the physical ctest-runtime.tar.gz file.
# @return Zero after validating and atomically moving the archive into place,
#   then reporting its exact physical byte and entry counts.
# @throws Nothing; invalid arguments, missing runtime roots, tar failures, or a
#   forbidden archive entry terminate the script with a nonzero status.
# @note The destination must be outside the packaged build directory. The
#   script never modifies the input tree and creates no background work;
#   excluding a transient smoke work root does not delete that root.

if (($# != 2)); then
  echo "Usage: $0 <build-dir> <ctest-runtime.tar.gz>" >&2
  exit 2
fi

build_dir=$1
archive_path=$2
if [[ ! -d "$build_dir" ]]; then
  echo "CTest build directory does not exist: $build_dir" >&2
  exit 2
fi
if [[ ! -f "$build_dir/CTestTestfile.cmake" ||
  ! -f "$build_dir/CMakeCache.txt" ]]; then
  echo "CTest build directory is not configured: $build_dir" >&2
  exit 2
fi

build_parent=$(cd -- "$(dirname -- "$build_dir")" && pwd)
build_name=$(basename -- "$build_dir")
build_root="$build_parent/$build_name"
archive_parent=$(dirname -- "$archive_path")
mkdir -p "$archive_parent"
archive_parent=$(cd -- "$archive_parent" && pwd)
archive_path="$archive_parent/$(basename -- "$archive_path")"
case "$archive_path" in
  "$build_root" | "$build_root"/*)
    echo "CTest runtime archive must be outside the build tree." >&2
    exit 2
    ;;
esac

temporary_root=$(mktemp -d \
  "${TMPDIR:-/tmp}/photospider-ctest-runtime.XXXXXX")
temporary_archive="$temporary_root/ctest-runtime.tar.gz"
archive_listing="$temporary_root/archive.list"
trap 'rm -rf -- "$temporary_root"' EXIT

tar -C "$build_parent" -czf "$temporary_archive" \
  --exclude='*.o' \
  --exclude='*.obj' \
  --exclude="$build_name/tests/image_artifact_codec_dependency_disabled" \
  --exclude="$build_name/tests/image_artifact_codec_dependency_disabled/*" \
  --exclude="$build_name/tests/optional_opencv_provider_disabled" \
  --exclude="$build_name/tests/optional_opencv_provider_disabled/*" \
  --exclude='*/CMakeFiles' \
  --exclude='*/CMakeFiles/*' \
  --exclude='*/.ninja_deps' \
  --exclude='*/.ninja_log' \
  --exclude='*/Testing' \
  --exclude='*/Testing/*' \
  -- "$build_name"

tar -tzf "$temporary_archive" > "$archive_listing"
if grep -Eq \
  '(^|/)(CMakeFiles|Testing)(/|$)|(^|/)\.ninja_(deps|log)$|\.(o|obj)$' \
  "$archive_listing"; then
  echo "CTest runtime archive contains forbidden build residue." >&2
  exit 1
fi

for excluded_smoke_root in \
  "$build_name/tests/image_artifact_codec_dependency_disabled" \
  "$build_name/tests/optional_opencv_provider_disabled"; do
  if grep -Fqx -- "$excluded_smoke_root" "$archive_listing" ||
    grep -Fq -- "$excluded_smoke_root/" "$archive_listing"; then
    echo \
      "CTest runtime archive contains transient smoke root: $excluded_smoke_root" \
      >&2
    exit 1
  fi
done

for required_entry in \
  "$build_name/CTestTestfile.cmake" \
  "$build_name/CMakeCache.txt" \
  "$build_name/PhotospiderConfig.cmake"; do
  if ! grep -Fqx -- "$required_entry" "$archive_listing"; then
    echo "CTest runtime archive is missing $required_entry." >&2
    exit 1
  fi
done

for required_root in \
  "$build_name/bin/" \
  "$build_name/lib/" \
  "$build_name/plugins/" \
  "$build_name/tests/"; do
  if ! grep -Fq -- "$required_root" "$archive_listing"; then
    echo "CTest runtime archive is missing runtime root $required_root." >&2
    exit 1
  fi
done

archive_bytes=$(wc -c < "$temporary_archive" | tr -d '[:space:]')
archive_entries=$(wc -l < "$archive_listing" | tr -d '[:space:]')
mv -- "$temporary_archive" "$archive_path"
printf 'Packaged CTest runtime: %s\n' "$archive_path"
printf 'CTest runtime archive bytes: %s\n' "$archive_bytes"
printf 'CTest runtime archive entries: %s\n' "$archive_entries"
