#!/usr/bin/env bash

# @brief Install the complete immutable Photospider CI image toolchain.
#
# The installer validates the image manifest and source identities, replaces
# every base-image APT source with the one protected Canonical snapshot, applies
# the checksum-locked offline TLS bootstrap, installs the exact package/Python
# locks, and downloads GitHub CLI from its exact release URL before checking the
# architecture-specific SHA-256 and installing it.
#
# @param No positional arguments are accepted. Required protected identities
#        arrive through APT_SNAPSHOT, GH_CLI_VERSION,
#        GH_CLI_{AMD64,ARM64}_SHA256, CI_IMAGE_INPUT_MANIFEST_SHA256,
#        CI_IMAGE_SOURCE_COMMIT, and VENV.
# @return Zero only after every locked install and cleanup step succeeds.
# @throws Returns nonzero for malformed identity, unsupported architecture,
#         snapshot/package resolution failure, download/hash drift, or install
#         failure. Bash strict mode propagates every command or pipeline error.
# @note This script is executed exactly once by Dockerfile.ci. Its full bytes
#       and active-statement contract are protected independently; it must not
#       be sourced, wrapped, or partially invoked. The package lock is consumed
#       only after apt's ``--`` option terminator, so a validated package token
#       cannot be reinterpreted as an installer option.

set -Eeuo pipefail
umask 022

# @brief Execute the one canonical CI-image installation transaction.
# @param No positional arguments are accepted.
# @return Zero after immutable installation and cleanup, otherwise nonzero.
# @throws Returns nonzero on any identity, architecture, network, hash,
#         package, extraction, installation, or cleanup failure.
# @note The function is invoked once at end of file so required statements
#       cannot remain defined but unreachable.
ci_image_install_main() {
  if (($# != 0)); then
    printf '%s\n' "ci_image_install.sh accepts no arguments." >&2
    return 1
  fi
  if [[ ! ${CI_IMAGE_INPUT_MANIFEST_SHA256:-} =~ ^[0-9a-f]{64}$ ]]; then
    printf '%s\n' "CI image input manifest digest is malformed." >&2
    return 1
  fi
  if [[ ! ${CI_IMAGE_SOURCE_COMMIT:-} =~ ^[0-9a-f]{40}$ ]]; then
    printf '%s\n' "CI image source commit is malformed." >&2
    return 1
  fi
  if [[ ! ${APT_SNAPSHOT:-} =~ ^[0-9]{8}T[0-9]{6}Z$ ]]; then
    printf '%s\n' "APT snapshot identity is malformed." >&2
    return 1
  fi
  if [[ ! ${GH_CLI_VERSION:-} =~ ^[0-9]+\.[0-9]+\.[0-9]+$ ]]; then
    printf '%s\n' "GitHub CLI version is malformed." >&2
    return 1
  fi
  if [[ ! ${GH_CLI_AMD64_SHA256:-} =~ ^[0-9a-f]{64}$ ||
    ! ${GH_CLI_ARM64_SHA256:-} =~ ^[0-9a-f]{64}$ ]]; then
    printf '%s\n' "GitHub CLI archive digest is malformed." >&2
    return 1
  fi
  if [[ ${VENV:-} != /opt/venv ]]; then
    printf '%s\n' "CI virtual environment path is not canonical." >&2
    return 1
  fi

  local architecture
  local gh_archive
  local gh_sha256
  architecture=$(dpkg --print-architecture)
  case "$architecture" in
    amd64)
      gh_sha256=$GH_CLI_AMD64_SHA256
      ;;
    arm64)
      gh_sha256=$GH_CLI_ARM64_SHA256
      ;;
    *)
      printf 'Unsupported CI image architecture: %s\n' "$architecture" >&2
      return 1
      ;;
  esac

  sed "s/@APT_SNAPSHOT@/$APT_SNAPSHOT/g" \
    /tmp/ci-locks/ubuntu-24.04-snapshot.sources.in \
    > /tmp/ci-locks/ubuntu.sources
  rm -f /etc/apt/sources.list
  find /etc/apt/sources.list.d -mindepth 1 -maxdepth 1 -delete
  install -m 0644 /tmp/ci-locks/ubuntu.sources \
    /etc/apt/sources.list.d/ubuntu.sources
  dpkg --install "/tmp/ci-bootstrap/openssl-$architecture.deb" \
    /tmp/ci-bootstrap/ca-certificates.deb
  apt-get update
  sed -e '/^#/d' -e '/^[[:space:]]*$/d' \
    /tmp/ci-locks/ubuntu-24.04-packages.lock \
    | xargs apt-get install -y --no-install-recommends --

  python3 -m venv "$VENV"
  "$VENV/bin/pip" install \
    --disable-pip-version-check \
    --no-deps \
    --only-binary=:all: \
    --require-hashes \
    -r /tmp/ci-locks/requirements-ci.txt

  gh_archive="gh_${GH_CLI_VERSION}_linux_${architecture}.tar.gz"
  curl --fail --location --proto '=https' --tlsv1.2 \
    --output "/tmp/$gh_archive" \
    "https://github.com/cli/cli/releases/download/v${GH_CLI_VERSION}/$gh_archive"
  printf '%s  %s\n' "$gh_sha256" "/tmp/$gh_archive" \
    | sha256sum --check --strict -
  tar -C /tmp -xzf "/tmp/$gh_archive"
  install -m 0755 "/tmp/gh_${GH_CLI_VERSION}_linux_${architecture}/bin/gh" \
    /usr/local/bin/gh
  ln -s "$VENV/bin/cpplint" /usr/local/bin/cpplint

  rm -rf /var/lib/apt/lists/* /tmp/ci-bootstrap /tmp/ci-locks \
    "/tmp/$gh_archive" "/tmp/gh_${GH_CLI_VERSION}_linux_${architecture}"
  rm -f /tmp/ci-image-install.sh
}

ci_image_install_main "$@"
