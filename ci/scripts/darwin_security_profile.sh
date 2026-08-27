#!/usr/bin/env bash

set -Eeuo pipefail

# @file darwin_security_profile.sh
# @brief Bind one verified Darwin host to disjoint protected-control and
#   candidate-data checkouts before running one native security profile.
# @note The caller verifies the hosted runner from the protected checkout
#   before candidate checkout. This wrapper then rebinds both Git HEADs,
#   physical directory identities, retained runner identity, inventory, and a
#   fresh runner.temp work root. Every executable profile/parser/platform
#   helper is loaded from the protected control root; candidate bytes are used
#   only as the CMake source tree. The roots and clean Git identities are
#   checked again after the profile, including failed profiles.

SCRIPT_DIR=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)
SCRIPT_ROOT=$(cd -- "$SCRIPT_DIR/../.." && pwd -P)

# @brief Print the exact environment-owned Darwin profile interface.
# @return Always zero.
usage() {
  cat <<'EOF'
Usage: CI_SECURITY_PROFILE=<sanitizer-asan|sanitizer-tsan|fuzz-codecs> \
  CI_CONTROL_ROOT=<protected checkout> CI_CANDIDATE_ROOT=<candidate checkout> \
  CI_INVENTORY_DIR=<downloaded inventory> CI_RUNNER_IDENTITY_FILE=<retained json> \
  CI_WORK_ROOT=<fresh runner.temp child> CI_CANDIDATE_COMMIT=<sha> \
  CI_WORKFLOW_COMMIT=<sha> CI_RUNNER_TEMP=<runner.temp> CI_JOBS=<positive> \
  ci/scripts/darwin_security_profile.sh
EOF
}

if (($# != 0)); then
  usage >&2
  exit 2
fi

required_environment=(
  CI_CANDIDATE_COMMIT
  CI_CANDIDATE_ROOT
  CI_CONTROL_ROOT
  CI_INVENTORY_DIR
  CI_JOBS
  CI_RUNNER_IDENTITY_FILE
  CI_RUNNER_TEMP
  CI_SECURITY_PROFILE
  CI_WORKFLOW_COMMIT
  CI_WORK_ROOT
)
for required_name in "${required_environment[@]}"; do
  if [[ -z ${!required_name:-} ]]; then
    echo "Darwin security wrapper lacks required environment: $required_name" >&2
    usage >&2
    exit 2
  fi
done

case "$CI_SECURITY_PROFILE" in
  sanitizer-asan)
    profile_script=ci/scripts/sanitizer_test.sh
    sanitizer=asan
    ;;
  sanitizer-tsan)
    profile_script=ci/scripts/sanitizer_test.sh
    sanitizer=tsan
    ;;
  fuzz-codecs)
    profile_script=ci/scripts/fuzz_smoke.sh
    sanitizer=
    ;;
  *)
    echo "Unsupported Darwin security profile: $CI_SECURITY_PROFILE" >&2
    exit 2
    ;;
esac

if [[ ! "$CI_CANDIDATE_COMMIT" =~ ^[0-9a-f]{40}$ ||
  ! "$CI_WORKFLOW_COMMIT" =~ ^[0-9a-f]{40}$ ||
  ! "$CI_JOBS" =~ ^[1-9][0-9]*$ ]]; then
  echo "Darwin security wrapper received a malformed identity." >&2
  exit 1
fi

# @brief Resolve one canonical real directory without accepting a path alias.
# @param $1 Absolute directory pathname.
# @param $2 Stable diagnostic role.
# @return The identical physical path on stdout.
# @throws Nothing; relative, linked, control-bearing, missing, or aliased paths
#   return nonzero before candidate build semantics execute.
real_directory() {
  local path=$1
  local role=$2
  local physical
  if [[ "$path" != /* || ! -d "$path" || -L "$path" ||
    "$path" == *$'\n'* || "$path" == *$'\r'* || "$path" == *$'\t'* ]]; then
    echo "$role is not an absolute real directory: $path" >&2
    return 1
  fi
  physical=$(cd -- "$path" && pwd -P)
  if [[ "$physical" != "$path" ]]; then
    echo "$role is not a canonical physical path: $path" >&2
    return 1
  fi
  printf '%s\n' "$physical"
}

# @brief Return the retained device/inode/mode identity of one real directory.
# @param $1 Canonical directory pathname.
# @return One colon-delimited identity on stdout.
# @throws Python exits nonzero for a link, replacement, or non-directory.
# @note The value remains in the parent shell and is compared after all
#   candidate execution; no candidate-owned file stores this authority.
directory_identity() {
  python3 - "$1" <<'PY'
import os
import stat
import sys

metadata = os.lstat(sys.argv[1])
if not stat.S_ISDIR(metadata.st_mode) or stat.S_ISLNK(metadata.st_mode):
    raise SystemExit("security root is not one real directory")
print(f"{metadata.st_dev}:{metadata.st_ino}:{metadata.st_mode}:{metadata.st_nlink}")
PY
}

# @brief Require one checkout to remain at an exact clean commit.
# @param $1 Canonical checkout root.
# @param $2 Expected lowercase full commit.
# @param $3 Stable diagnostic role.
# @return Zero only for one real .git directory, exact HEAD, and clean tree.
# @throws Nothing; unreadable, linked, dirty, or drifted Git state is nonzero.
verify_checkout() {
  local root=$1
  local expected=$2
  local role=$3
  local actual
  local status
  if [[ ! -d "$root/.git" || -L "$root/.git" ]]; then
    echo "$role lacks a real Git metadata directory." >&2
    return 1
  fi
  actual=$(GIT_CONFIG_NOSYSTEM=1 GIT_CONFIG_GLOBAL=/dev/null \
    git -C "$root" rev-parse --verify 'HEAD^{commit}') || return 1
  if [[ "$actual" != "$expected" ]]; then
    echo "$role HEAD '$actual' differs from '$expected'." >&2
    return 1
  fi
  status=$(GIT_CONFIG_NOSYSTEM=1 GIT_CONFIG_GLOBAL=/dev/null \
    git -C "$root" status --porcelain=v1 --untracked-files=all \
      --ignore-submodules=none) || return 1
  if [[ -n "$status" ]]; then
    echo "$role checkout is not clean." >&2
    return 1
  fi
}

control_root=$(real_directory "$CI_CONTROL_ROOT" "Protected control root")
candidate_root=$(real_directory "$CI_CANDIDATE_ROOT" "Candidate source root")
inventory_root=$(real_directory "$CI_INVENTORY_DIR" "Security inventory root")
runner_temp=$(real_directory "$CI_RUNNER_TEMP" "Runner temporary root")
if [[ "$control_root" != "$SCRIPT_ROOT" ]]; then
  echo "Darwin security wrapper is not executing from its declared protected checkout." >&2
  exit 1
fi
if [[ "$control_root" == "$candidate_root" ||
  "$control_root" == "$inventory_root" ||
  "$candidate_root" == "$inventory_root" ]]; then
  echo "Darwin security control, candidate, and inventory roots overlap." >&2
  exit 1
fi
for left in "$control_root" "$candidate_root" "$inventory_root"; do
  for right in "$control_root" "$candidate_root" "$inventory_root"; do
    [[ "$left" == "$right" ]] && continue
    if [[ "$left" == "$right"/* || "$right" == "$left"/* ]]; then
      echo "Darwin security control, candidate, and inventory roots overlap." >&2
      exit 1
    fi
  done
done
if [[ "$runner_temp" == "$control_root" || "$runner_temp" == "$candidate_root" ||
  "$runner_temp" == "$inventory_root" || "$runner_temp" == "$control_root"/* ||
  "$runner_temp" == "$candidate_root"/* || "$runner_temp" == "$inventory_root"/* ||
  "$control_root" == "$runner_temp"/* || "$candidate_root" == "$runner_temp"/* ||
  "$inventory_root" == "$runner_temp"/* ]]; then
  echo "Darwin security runner.temp overlaps a control or data root." >&2
  exit 1
fi

if [[ "$CI_RUNNER_IDENTITY_FILE" != /* ||
  ! -f "$CI_RUNNER_IDENTITY_FILE" || -L "$CI_RUNNER_IDENTITY_FILE" ||
  "$CI_RUNNER_IDENTITY_FILE" == *$'\n'* ||
  "$CI_RUNNER_IDENTITY_FILE" == *$'\r'* ||
  "$CI_RUNNER_IDENTITY_FILE" == *$'\t'* ]]; then
  echo "Darwin retained runner identity is not a safe regular file." >&2
  exit 1
fi
identity_parent=$(real_directory "${CI_RUNNER_IDENTITY_FILE%/*}" \
  "Retained runner identity parent")
identity_file=$identity_parent/$(basename -- "$CI_RUNNER_IDENTITY_FILE")
if [[ "$identity_parent" != "$runner_temp" ||
  "$identity_file" != "$CI_RUNNER_IDENTITY_FILE" ]]; then
  echo "Darwin retained runner identity is not an exact runner.temp child." >&2
  exit 1
fi

verify_checkout "$control_root" "$CI_WORKFLOW_COMMIT" \
  "Protected control"
verify_checkout "$candidate_root" "$CI_CANDIDATE_COMMIT" \
  "Candidate source"
control_identity=$(directory_identity "$control_root")
candidate_identity=$(directory_identity "$candidate_root")

work_parent=${CI_WORK_ROOT%/*}
if [[ "$CI_WORK_ROOT" != /* || "$work_parent" == "$CI_WORK_ROOT" ||
  -e "$CI_WORK_ROOT" || -L "$CI_WORK_ROOT" ]]; then
  echo "Darwin security work root is residual or has no explicit parent." >&2
  exit 1
fi
work_parent=$(real_directory "$work_parent" "Darwin security work parent")
if [[ "$work_parent" != "$runner_temp" ]]; then
  echo "Darwin security work root is not a direct runner.temp child." >&2
  exit 1
fi
mkdir -m 700 -- "$CI_WORK_ROOT"
work_root=$(real_directory "$CI_WORK_ROOT" "Darwin security work root")
mkdir -m 700 -- "$work_root/build" "$work_root/home" "$work_root/results" \
  "$work_root/runner-temp" "$work_root/tmp"

{
  printf 'schema=photospider-darwin-security-host-wrapper-v1\n'
  printf 'profile=%s\n' "$CI_SECURITY_PROFILE"
  printf 'candidate_commit=%s\n' "$CI_CANDIDATE_COMMIT"
  printf 'workflow_commit=%s\n' "$CI_WORKFLOW_COMMIT"
} > "$work_root/results/darwin-security-host-wrapper.env"

if (
  export BUILD_DIR=$work_root/build
  export CMAKE_BUILD_TYPE=RelWithDebInfo
  export CI_ARTIFACT_DIR=$work_root/results
  export CI_INVENTORY_DIR=$inventory_root
  export CI_RUNNER_IDENTITY_FILE=$identity_file
  export CI_RUNNER_TEMP=$work_root/runner-temp
  export CI_SOURCE_ROOT=$candidate_root
  export HOME=$work_root/home
  export PYTHONDONTWRITEBYTECODE=1
  export TMPDIR=$work_root/tmp
  if [[ -n "$sanitizer" ]]; then
    export SANITIZER=$sanitizer
  fi
  bash "$control_root/$profile_script"
); then
  profile_status=0
else
  profile_status=$?
fi

post_control_identity=$(directory_identity "$control_root")
post_candidate_identity=$(directory_identity "$candidate_root")
verify_checkout "$control_root" "$CI_WORKFLOW_COMMIT" \
  "Protected control after profile"
verify_checkout "$candidate_root" "$CI_CANDIDATE_COMMIT" \
  "Candidate source after profile"
if [[ "$post_control_identity" != "$control_identity" ||
  "$post_candidate_identity" != "$candidate_identity" ]]; then
  echo "Darwin security checkout directory identity changed during profile execution." >&2
  exit 1
fi
if ((profile_status != 0)); then
  echo "Darwin security profile failed with status $profile_status." >&2
  exit "$profile_status"
fi
if [[ ! -s "$work_root/results/summary.log" ]]; then
  echo "Darwin security profile produced no success evidence." >&2
  exit 1
fi
