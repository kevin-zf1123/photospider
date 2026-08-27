#!/usr/bin/env bash

set -Eeuo pipefail

# @file linux_security_profile.sh
# @brief Verify one Linux hosted runner on the real host, authenticate/pull an
#   exact CI image, and execute one protected security profile inside it.
# @note The caller must create the retained runner identity before checking out
#   candidate data. This protected helper receives that same regular file and
#   mounts it read-only beside a read-only candidate source tree, a nested
#   read-only protected ``ci`` control tree, and read-only profile inventory.
#   Only the fresh job-owned ``/work`` mount is writable. The container runs as
#   the same strictly measured positive numeric UID/GID that owns that mode-0700
#   host directory, and a protected entrypoint proves the identity plus a real
#   create/read/remove cycle before candidate work. The GHCR token is used by
#   the host login command and is never forwarded into the container.

SCRIPT_DIR=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)
SCRIPT_ROOT=$(cd -- "$SCRIPT_DIR/../.." && pwd -P)

# @brief Print the exact environment-owned Linux profile interface.
# @return Always zero.
usage() {
  cat <<'EOF'
Usage: CI_SECURITY_PROFILE=<sanitizer-asan|sanitizer-tsan|fuzz-codecs> \
  CI_CONTROL_ROOT=<protected checkout> CI_CANDIDATE_ROOT=<candidate checkout> \
  CI_INVENTORY_DIR=<downloaded inventory> CI_RUNNER_IDENTITY_FILE=<retained json> \
  CI_WORK_ROOT=<fresh runner.temp child> CI_IMAGE_REF=<digest-qualified image> \
  CI_IMAGE_DIGEST=<sha256 digest> CI_GHCR_USERNAME=<actor> CI_GHCR_TOKEN=<token> \
  CI_CANDIDATE_COMMIT=<sha> CI_WORKFLOW_COMMIT=<sha> \
  CI_RUNNER_TEMP=<runner.temp> GITHUB_REPOSITORY=<owner/name> \
  ci/scripts/linux_security_profile.sh
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
  CI_GHCR_TOKEN
  CI_GHCR_USERNAME
  CI_IMAGE_DIGEST
  CI_IMAGE_REF
  CI_INVENTORY_DIR
  CI_JOBS
  CI_RUNNER_IDENTITY_FILE
  CI_RUNNER_TEMP
  CI_SECURITY_PROFILE
  CI_WORKFLOW_COMMIT
  CI_WORK_ROOT
  GITHUB_REPOSITORY
)
for required_name in "${required_environment[@]}"; do
  if [[ -z ${!required_name:-} ]]; then
    echo "Linux security wrapper lacks required environment: $required_name" >&2
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
    echo "Unsupported Linux security profile: $CI_SECURITY_PROFILE" >&2
    exit 2
    ;;
esac

if [[ ! "$CI_CANDIDATE_COMMIT" =~ ^[0-9a-f]{40}$ ||
  ! "$CI_WORKFLOW_COMMIT" =~ ^[0-9a-f]{40}$ ||
  ! "$CI_IMAGE_DIGEST" =~ ^sha256:[0-9a-f]{64}$ ||
  ! "$GITHUB_REPOSITORY" =~ ^[A-Za-z0-9_.-]+/[A-Za-z0-9_.-]+$ ||
  ! "$CI_JOBS" =~ ^[1-9][0-9]*$ ]]; then
  echo "Linux security wrapper received a malformed identity." >&2
  exit 1
fi
expected_image_ref="ghcr.io/$GITHUB_REPOSITORY/photospider-ci@$CI_IMAGE_DIGEST"
if [[ "$CI_IMAGE_REF" != "$expected_image_ref" ]]; then
  echo "Linux security image reference differs from its exact digest identity." >&2
  exit 1
fi

# @brief Resolve one real directory and reject commas/control bytes that would
#   make Docker's structured bind-mount grammar ambiguous.
# @param $1 Existing directory.
# @param $2 Stable diagnostic role.
# @return Physical directory path on stdout.
# @throws Nothing; missing, linked, comma-bearing, control-bearing, or
#   unresolvable directories return nonzero before login/pull/candidate work.
real_mount_directory() {
  local path=$1
  local role=$2
  if [[ ! -d "$path" || -L "$path" || "$path" == *','* ||
    "$path" == *$'\n'* || "$path" == *$'\r'* || "$path" == *$'\t'* ]]; then
    echo "$role is not an unambiguous real directory: $path" >&2
    return 1
  fi
  (cd -- "$path" && pwd -P)
}

control_root=$(real_mount_directory "$CI_CONTROL_ROOT" "Protected control root")
candidate_root=$(real_mount_directory "$CI_CANDIDATE_ROOT" "Candidate source root")
inventory_root=$(real_mount_directory "$CI_INVENTORY_DIR" "Security inventory root")
runner_temp=$(real_mount_directory "$CI_RUNNER_TEMP" "Runner temporary root")
if [[ "$control_root" != "$SCRIPT_ROOT" ]]; then
  echo "Linux security wrapper is not executing from its declared protected checkout." >&2
  exit 1
fi
for left in "$control_root" "$candidate_root" "$inventory_root"; do
  for right in "$control_root" "$candidate_root" "$inventory_root"; do
    [[ "$left" == "$right" ]] && continue
    if [[ "$left" == "$right"/* || "$right" == "$left"/* ]]; then
      echo "Linux security control, candidate, and inventory roots overlap." >&2
      exit 1
    fi
  done
done
if [[ ! -f "$CI_RUNNER_IDENTITY_FILE" || -L "$CI_RUNNER_IDENTITY_FILE" ||
  "$CI_RUNNER_IDENTITY_FILE" == *','* ||
  "$CI_RUNNER_IDENTITY_FILE" == *$'\n'* ||
  "$CI_RUNNER_IDENTITY_FILE" == *$'\r'* ||
  "$CI_RUNNER_IDENTITY_FILE" == *$'\t'* ]]; then
  echo "Linux retained runner identity is not a safe regular file." >&2
  exit 1
fi
identity_file=$(cd -- "$(dirname -- "$CI_RUNNER_IDENTITY_FILE")" && pwd -P)/$(basename -- "$CI_RUNNER_IDENTITY_FILE")

candidate_head=$(GIT_CONFIG_NOSYSTEM=1 GIT_CONFIG_GLOBAL=/dev/null \
  git -C "$candidate_root" rev-parse --verify HEAD^{commit})
workflow_head=$(GIT_CONFIG_NOSYSTEM=1 GIT_CONFIG_GLOBAL=/dev/null \
  git -C "$control_root" rev-parse --verify HEAD^{commit})
if [[ "$candidate_head" != "$CI_CANDIDATE_COMMIT" ||
  "$workflow_head" != "$CI_WORKFLOW_COMMIT" ]]; then
  echo "Linux security checkout commit differs from its protected identity." >&2
  exit 1
fi

work_parent=${CI_WORK_ROOT%/*}
if [[ "$work_parent" == "$CI_WORK_ROOT" || -z "$work_parent" ||
  -e "$CI_WORK_ROOT" || -L "$CI_WORK_ROOT" ]]; then
  echo "Linux security work root is residual or has no explicit parent." >&2
  exit 1
fi
work_parent=$(real_mount_directory "$work_parent" "Linux security work parent")
if [[ "$work_parent" != "$runner_temp" ]]; then
  echo "Linux security work root is not a direct runner.temp child." >&2
  exit 1
fi
mkdir -m 700 -- "$CI_WORK_ROOT"
work_root=$(real_mount_directory "$CI_WORK_ROOT" "Linux security work root")
mkdir -m 700 -- "$work_root/build" "$work_root/home" "$work_root/results" \
  "$work_root/runner-temp" "$work_root/tmp"
container_uid=$(/usr/bin/id -u)
container_gid=$(/usr/bin/id -g)
if [[ ! "$container_uid" =~ ^[1-9][0-9]*$ ||
  ! "$container_gid" =~ ^[1-9][0-9]*$ ]]; then
  echo "Linux hosted runner has no safe positive numeric UID/GID." >&2
  exit 1
fi

# Authenticate on the host only. The protected command uses password-stdin;
# the secret is removed from this process before pull/run argv are constructed.
printf '%s' "$CI_GHCR_TOKEN" |
  docker login ghcr.io --username "$CI_GHCR_USERNAME" --password-stdin
unset CI_GHCR_TOKEN
docker pull "$CI_IMAGE_REF"

container_arguments=(
  run
  --rm
  --read-only
  --network none
  --cap-drop ALL
  --security-opt no-new-privileges
  --pids-limit 4096
  --user "$container_uid:$container_gid"
  --tmpfs /tmp:rw,nosuid,nodev
  --mount "type=bind,src=$candidate_root,dst=/workspace/photospider,readonly"
  --mount "type=bind,src=$control_root/ci,dst=/workspace/photospider/ci,readonly"
  --mount "type=bind,src=$inventory_root,dst=/inputs/profile,readonly"
  --mount "type=bind,src=$identity_file,dst=/inputs/runner-identity.json,readonly"
  --mount "type=bind,src=$work_root,dst=/work"
  --workdir /workspace/photospider
  --env BUILD_DIR=/work/build
  --env CMAKE_BUILD_TYPE=RelWithDebInfo
  --env CI_ARTIFACT_DIR=/work/results
  --env "CI_CONTAINER_GID=$container_gid"
  --env "CI_CONTAINER_UID=$container_uid"
  --env CI_CONTAINER_WORK_ROOT=/work
  --env CI_INVENTORY_DIR=/inputs/profile
  --env "CI_JOBS=$CI_JOBS"
  --env CI_RUNNER_IDENTITY_FILE=/inputs/runner-identity.json
  --env CI_RUNNER_TEMP=/work/runner-temp
  --env HOME=/work/home
  --env TMPDIR=/work/tmp
)
if [[ -n "$sanitizer" ]]; then
  container_arguments+=(--env "SANITIZER=$sanitizer")
fi
container_arguments+=(
  "$CI_IMAGE_REF"
  bash
  ci/scripts/linux_security_container_entrypoint.sh
  "$profile_script"
)

{
  printf 'schema=photospider-linux-security-host-wrapper-v1\n'
  printf 'profile=%s\n' "$CI_SECURITY_PROFILE"
  printf 'candidate_commit=%s\n' "$CI_CANDIDATE_COMMIT"
  printf 'workflow_commit=%s\n' "$CI_WORKFLOW_COMMIT"
  printf 'image_digest=%s\n' "$CI_IMAGE_DIGEST"
  printf 'container_uid=%s\n' "$container_uid"
  printf 'container_gid=%s\n' "$container_gid"
} > "$work_root/results/linux-security-host-wrapper.env"

docker "${container_arguments[@]}"
if [[ ! -s "$work_root/results/summary.log" ]]; then
  echo "Linux security container produced no profile success evidence." >&2
  exit 1
fi
