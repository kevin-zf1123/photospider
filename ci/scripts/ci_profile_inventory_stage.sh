#!/usr/bin/env bash

set -Eeuo pipefail

# @file ci_profile_inventory_stage.sh
# @brief Rebuild the exact generated profile identity artifact for downstream
#   trusted readers without retaining any output from an earlier invocation.
# @note The three identity filenames and the resolved manifest are the only
#   permitted entries in the dedicated output directory. Existing regular
#   entries are removed before source inspection; links, special files, an
#   aliased source/output directory, or unrelated entries fail closed. A
#   partial generated set is copied and then deliberately rejected by the
#   version-aware reader before the artifact can be published.

SCRIPT_DIR=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)
REPO_ROOT=$(cd -- "$SCRIPT_DIR/../.." && pwd)
SOURCE_INVENTORY=${1:-}
OUTPUT_INVENTORY=${2:-}
if [[ -z "$SOURCE_INVENTORY" || -z "$OUTPUT_INVENTORY" || $# -ne 2 ]]; then
  echo "Usage: $0 <generated-inventory-dir> <artifact-inventory-dir>" >&2
  exit 2
fi

identity_files=(
  build_profile_matrix_v1.tsv \
  build_profile_matrix_v1.tsv.sha256 \
  ci_security_roles_v1.tsv
)
output_files=("${identity_files[@]}" resolved-security-profiles.json)

if [[ -L "$SOURCE_INVENTORY" || ! -d "$SOURCE_INVENTORY" ]]; then
  echo "Generated profile inventory must be a real directory: $SOURCE_INVENTORY" >&2
  exit 1
fi
if [[ -L "$OUTPUT_INVENTORY" ]]; then
  echo "Profile artifact output directory must not be a symlink: $OUTPUT_INVENTORY" >&2
  exit 1
fi
if [[ -e "$OUTPUT_INVENTORY" && ! -d "$OUTPUT_INVENTORY" ]]; then
  echo "Profile artifact output is not a directory: $OUTPUT_INVENTORY" >&2
  exit 1
fi
mkdir -p "$OUTPUT_INVENTORY"

source_real=$(cd -- "$SOURCE_INVENTORY" && pwd -P)
output_real=$(cd -- "$OUTPUT_INVENTORY" && pwd -P)
if [[ "$source_real" == "$output_real" ]]; then
  echo "Generated and artifact profile inventory directories must differ." >&2
  exit 1
fi

# @brief Return success only for one exact artifact filename owned by staging.
# @param $1 Basename found directly under the output inventory directory.
# @return Zero for the three versioned inputs or resolved manifest; one for all
#   unrelated names.
is_staged_output_name() {
  case ${1:-} in
    build_profile_matrix_v1.tsv | \
      build_profile_matrix_v1.tsv.sha256 | \
      ci_security_roles_v1.tsv | \
      resolved-security-profiles.json)
      return 0
      ;;
    *)
      return 1
      ;;
  esac
}

while IFS= read -r -d '' existing_output; do
  output_name=${existing_output##*/}
  if ! is_staged_output_name "$output_name"; then
    echo "Unexpected entry in dedicated profile artifact directory: $output_name" >&2
    exit 1
  fi
  if [[ -L "$existing_output" || ! -f "$existing_output" ]]; then
    echo "Unsafe existing staged profile artifact: $output_name" >&2
    exit 1
  fi
done < <(find "$OUTPUT_INVENTORY" -mindepth 1 -maxdepth 1 -print0)

for output_file in "${output_files[@]}"; do
  if [[ -e "$OUTPUT_INVENTORY/$output_file" || -L "$OUTPUT_INVENTORY/$output_file" ]]; then
    rm -f -- "$OUTPUT_INVENTORY/$output_file"
  fi
done

for identity_file in "${identity_files[@]}"; do
  if [[ -e "$SOURCE_INVENTORY/$identity_file" || -L "$SOURCE_INVENTORY/$identity_file" ]]; then
    if [[ ! -f "$SOURCE_INVENTORY/$identity_file" || -L "$SOURCE_INVENTORY/$identity_file" ]]; then
      echo "Generated profile identity is not a regular file: $identity_file" >&2
      exit 1
    fi
    cp "$SOURCE_INVENTORY/$identity_file" "$OUTPUT_INVENTORY/$identity_file"
  fi
done
python3 "$SCRIPT_DIR/ci_profile_manifest.py" \
  --repo-root "$REPO_ROOT" \
  --inventory-dir "$OUTPUT_INVENTORY" \
  --output "$OUTPUT_INVENTORY/resolved-security-profiles.json"
