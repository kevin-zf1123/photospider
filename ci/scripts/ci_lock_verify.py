#!/usr/bin/env python3
"""Validate every repository-protected CI lock and active consumer.

This durable verifier is intentionally independent of candidate product code.
It rejects floating workflow actions/runners/images, malformed protected locks,
and Docker installation paths that bypass the immutable snapshot or hash locks.
Run it from any directory; ``--repo-root`` is primarily for fixture tests.
"""

from __future__ import annotations

import argparse
import json
import re
import sys
from pathlib import Path
from typing import Any, Iterable


class ContractError(ValueError):
    """Report one fail-closed protected-CI contract violation."""


def _unique_object(pairs: list[tuple[str, Any]]) -> dict[str, Any]:
    """Build a JSON object while rejecting duplicate member names."""
    result: dict[str, Any] = {}
    for key, value in pairs:
        if key in result:
            raise ContractError(f"duplicate JSON member: {key}")
        result[key] = value
    return result


def _load_json(path: Path) -> Any:
    """Load strict UTF-8 JSON and preserve duplicate-key rejection."""
    try:
        return json.loads(path.read_text(encoding="utf-8"), object_pairs_hook=_unique_object)
    except (OSError, UnicodeError, json.JSONDecodeError, ContractError) as error:
        raise ContractError(f"cannot read strict JSON {path}: {error}") from error


def _exact_keys(value: dict[str, Any], expected: set[str], context: str) -> None:
    """Require an object to contain exactly the documented fields."""
    actual = set(value)
    if actual != expected:
        raise ContractError(
            f"{context} fields differ: missing={sorted(expected - actual)}, "
            f"unknown={sorted(actual - expected)}"
        )


def _require_sorted_unique(values: Iterable[str], context: str) -> list[str]:
    """Return values after enforcing bytewise canonical order and uniqueness."""
    result = list(values)
    if result != sorted(result):
        raise ContractError(f"{context} is not bytewise sorted")
    if len(set(result)) != len(result):
        raise ContractError(f"{context} contains duplicates")
    return result


def _read_actions(path: Path) -> dict[str, tuple[str, str]]:
    """Read the canonical action/release/full-SHA lock table."""
    entries: dict[str, tuple[str, str]] = {}
    ordered: list[str] = []
    for line_number, raw_line in enumerate(path.read_text(encoding="utf-8").splitlines(), 1):
        if not raw_line or raw_line.startswith("#"):
            continue
        fields = raw_line.split("\t")
        if len(fields) != 3:
            raise ContractError(f"{path}:{line_number}: expected three tab-separated fields")
        action, release, commit = fields
        if not re.fullmatch(r"[a-z0-9_.-]+/[a-z0-9_.-]+", action):
            raise ContractError(f"{path}:{line_number}: invalid action name {action!r}")
        if not re.fullmatch(r"v[1-9][0-9]*", release):
            raise ContractError(f"{path}:{line_number}: invalid release {release!r}")
        if not re.fullmatch(r"[0-9a-f]{40}", commit):
            raise ContractError(f"{path}:{line_number}: action commit is not a full SHA")
        if action in entries:
            raise ContractError(f"{path}:{line_number}: duplicate action {action}")
        entries[action] = (release, commit)
        ordered.append(action)
    _require_sorted_unique(ordered, str(path))
    if not entries:
        raise ContractError(f"{path}: action lock is empty")
    return entries


def _verify_workflows(root: Path, actions: dict[str, tuple[str, str]]) -> None:
    """Verify active workflow action, runner, and container identities."""
    workflow_root = root / ".github" / "workflows"
    uses_pattern = re.compile(r"^\s*(?:-\s*)?uses:\s*([^#\s]+)(?:\s+#\s*(\S+))?\s*$")
    image_pattern = re.compile(r"^\s*image:\s*(.*?)\s*$")
    runner_pattern = re.compile(r"^\s*runs-on:\s*([^#\s]+)")
    used_actions: set[str] = set()
    for path in sorted(workflow_root.glob("*.yml")):
        for line_number, line in enumerate(path.read_text(encoding="utf-8").splitlines(), 1):
            action_match = uses_pattern.match(line)
            if action_match:
                reference, release_comment = action_match.groups()
                if reference.startswith("./"):
                    continue
                if "@" not in reference:
                    raise ContractError(f"{path}:{line_number}: action has no identity")
                action, commit = reference.rsplit("@", 1)
                locked = actions.get(action)
                if locked is None:
                    raise ContractError(f"{path}:{line_number}: unlisted action {action}")
                release, expected_commit = locked
                if commit != expected_commit:
                    raise ContractError(
                        f"{path}:{line_number}: {action} uses {commit}, expected {expected_commit}"
                    )
                if release_comment != release:
                    raise ContractError(
                        f"{path}:{line_number}: {action} must retain '# {release}' review hint"
                    )
                used_actions.add(action)
            image_match = image_pattern.match(line)
            if image_match:
                image = image_match.group(1)
                if "${{" in image:
                    trusted_outputs = (
                        "needs.ci-image-identity.outputs.image",
                        "steps.identity.outputs.image",
                    )
                    if not any(output in image for output in trusted_outputs):
                        raise ContractError(
                            f"{path}:{line_number}: dynamic image is not the trusted identity output"
                        )
                elif not re.search(r"@sha256:[0-9a-f]{64}$", image):
                    raise ContractError(f"{path}:{line_number}: container image is not digest-bound")
            runner_match = runner_pattern.match(line)
            if runner_match:
                runner = runner_match.group(1)
                if runner.endswith("-latest"):
                    raise ContractError(f"{path}:{line_number}: latest runner alias is mutable")
                if runner.lower().startswith("windows"):
                    raise ContractError(f"{path}:{line_number}: Windows is outside the maintained platform set")
    unused = sorted(set(actions) - used_actions)
    if unused:
        raise ContractError(f"action lock contains unused identities: {unused}")


def _verify_packages(path: Path) -> None:
    """Validate the exact, sorted top-level Ubuntu package lock."""
    packages: list[str] = []
    for line_number, line in enumerate(path.read_text(encoding="utf-8").splitlines(), 1):
        if not line or line.startswith("#"):
            continue
        if not re.fullmatch(r"[a-z0-9+.-]+=[^\s=]+", line):
            raise ContractError(f"{path}:{line_number}: invalid exact package lock")
        packages.append(line.split("=", 1)[0])
    _require_sorted_unique(packages, str(path))
    if "ca-certificates" not in packages:
        raise ContractError(f"{path}: CA bootstrap package must be locked")


def _verify_requirements(path: Path) -> None:
    """Require exact Python versions and SHA-256 hashes for every requirement."""
    logical_lines: list[str] = []
    pending = ""
    for raw_line in path.read_text(encoding="utf-8").splitlines():
        stripped = raw_line.strip()
        if not stripped or stripped.startswith("#"):
            continue
        pending += stripped.removesuffix("\\").strip() + " "
        if not stripped.endswith("\\"):
            logical_lines.append(pending.strip())
            pending = ""
    if pending:
        raise ContractError(f"{path}: unterminated requirement continuation")
    names: list[str] = []
    for line in logical_lines:
        match = re.match(r"([A-Za-z0-9_.-]+)==([^\s]+)(?:\s+|$)", line)
        hashes = re.findall(r"--hash=sha256:([0-9a-f]{64})(?:\s|$)", line)
        if not match or not hashes:
            raise ContractError(f"{path}: requirement lacks exact version or SHA-256: {line!r}")
        names.append(match.group(1).lower().replace("_", "-"))
    _require_sorted_unique(names, str(path))


def _verify_image_lock(root: Path, actions: dict[str, tuple[str, str]]) -> None:
    """Validate image input schema, paths, digests, and builder identity."""
    path = root / "ci/locks/ci-image-lock.json"
    lock = _load_json(path)
    if not isinstance(lock, dict):
        raise ContractError(f"{path}: root must be an object")
    _exact_keys(
        lock,
        {"schema", "apt_snapshot", "base_image", "builder", "github_cli", "input_paths", "published_image"},
        str(path),
    )
    if lock["schema"] != "photospider-ci-image-lock-v1":
        raise ContractError(f"{path}: unknown schema")
    if not re.fullmatch(r"[0-9]{8}T[0-9]{6}Z", str(lock["apt_snapshot"])):
        raise ContractError(f"{path}: invalid immutable snapshot ID")
    base = lock["base_image"]
    _exact_keys(base, {"name", "tag", "digest"}, f"{path}:base_image")
    if not re.fullmatch(r"sha256:[0-9a-f]{64}", str(base["digest"])):
        raise ContractError(f"{path}: base digest is not canonical")
    builder = lock["builder"]
    _exact_keys(builder, {"action", "release"}, f"{path}:builder")
    if actions.get(builder["action"], (None, None))[0] != builder["release"]:
        raise ContractError(f"{path}: builder is not action-lock bound")
    cli = lock["github_cli"]
    _exact_keys(cli, {"version", "amd64_sha256", "arm64_sha256"}, f"{path}:github_cli")
    if not re.fullmatch(r"[0-9]+\.[0-9]+\.[0-9]+", str(cli["version"])):
        raise ContractError(f"{path}: invalid GitHub CLI version")
    for architecture in ("amd64", "arm64"):
        if not re.fullmatch(r"[0-9a-f]{64}", str(cli[f"{architecture}_sha256"])):
            raise ContractError(f"{path}: invalid GitHub CLI {architecture} hash")
    input_paths = lock["input_paths"]
    if not isinstance(input_paths, list) or not all(isinstance(item, str) for item in input_paths):
        raise ContractError(f"{path}: input_paths must be a string array")
    _require_sorted_unique(input_paths, f"{path}:input_paths")
    required = {"Dockerfile.ci", ".dockerignore", str(path.relative_to(root))}
    if not required.issubset(input_paths):
        raise ContractError(f"{path}: real image inputs are missing")
    for relative in input_paths:
        candidate = root / relative
        if relative.startswith("/") or ".." in Path(relative).parts:
            raise ContractError(f"{path}: unsafe image input {relative!r}")
        if not candidate.is_file() or candidate.is_symlink():
            raise ContractError(f"{path}: image input is not a regular repository file: {relative}")

    workflow_path = root / ".github/workflows/build-ci-image.yml"
    workflow_lines = workflow_path.read_text(encoding="utf-8").splitlines()
    trigger_paths: list[str] = []
    in_paths = False
    for line in workflow_lines:
        if line == "    paths:":
            if in_paths:
                raise ContractError(f"{workflow_path}: duplicate push path list")
            in_paths = True
            continue
        if in_paths and line.startswith("      - "):
            trigger_paths.append(line.removeprefix("      - "))
            continue
        if in_paths:
            break
    if len(trigger_paths) != len(set(trigger_paths)) or sorted(trigger_paths) != input_paths:
        raise ContractError(
            f"{workflow_path}: push paths differ from canonical image inputs"
        )
    workflow_text = workflow_path.read_text(encoding="utf-8")
    source_contract = (
        "publish-source-commit",
        '--workflow-commit "${{ github.sha }}"',
        '--source-commit "${{ steps.source.outputs.commit }}"',
        "org.opencontainers.image.revision=${{ steps.source.outputs.commit }}",
        "CI_IMAGE_SOURCE_COMMIT=${{ steps.source.outputs.commit }}",
        "name: ci-image-input-${{ steps.source.outputs.commit }}",
    )
    for fragment in source_contract:
        if workflow_text.count(fragment) != 1:
            raise ContractError(
                f"{workflow_path}: canonical publisher source contract differs at {fragment!r}"
            )
    direct_head_identities = (
        '--source-commit "${{ github.sha }}"',
        "org.opencontainers.image.revision=${{ github.sha }}",
        "CI_IMAGE_SOURCE_COMMIT=${{ github.sha }}",
        "name: ci-image-input-${{ github.sha }}",
    )
    for fragment in direct_head_identities:
        if fragment in workflow_text:
            raise ContractError(
                f"{workflow_path}: publisher bypasses canonical source guard with {fragment!r}"
            )
    guard_index = workflow_text.index("publish-source-commit")
    for publication_marker in (
        "uses: docker/login-action@",
        "uses: docker/build-push-action@",
    ):
        marker_index = workflow_text.find(publication_marker)
        if marker_index < 0 or guard_index > marker_index:
            raise ContractError(
                f"{workflow_path}: source identity guard must precede {publication_marker}"
            )
    published = lock["published_image"]
    _exact_keys(
        published,
        {"locator", "source_repository", "source_workflow", "input_manifest_label", "source_commit_label"},
        f"{path}:published_image",
    )
    if not str(published["locator"]).endswith(":latest"):
        raise ContractError(f"{path}: published locator must be an explicit locator tag")

    dockerfile = (root / "Dockerfile.ci").read_text(encoding="utf-8")
    exact_docker_values = (
        f"FROM ubuntu:{base['tag']}@{base['digest']}",
        f"ARG APT_SNAPSHOT={lock['apt_snapshot']}",
        f"ARG GH_CLI_VERSION={cli['version']}",
        f"ARG GH_CLI_AMD64_SHA256={cli['amd64_sha256']}",
        f"ARG GH_CLI_ARM64_SHA256={cli['arm64_sha256']}",
    )
    for exact in exact_docker_values:
        if dockerfile.count(exact) != 1:
            raise ContractError(f"Dockerfile.ci does not consume exact image lock value {exact!r}")


def _verify_dockerfile(root: Path) -> None:
    """Check Dockerfile consumption of immutable image and dependency locks."""
    text = (root / "Dockerfile.ci").read_text(encoding="utf-8")
    required_fragments = (
        "FROM ubuntu:24.04@sha256:",
        "ubuntu-24.04-packages.lock",
        "requirements-ci.txt",
        "apt-get -S \"$APT_SNAPSHOT\"",
        "--require-hashes",
        "GH_CLI_AMD64_SHA256",
        "GH_CLI_ARM64_SHA256",
        "org.photospider.ci.input-manifest-sha256",
    )
    for fragment in required_fragments:
        if fragment not in text:
            raise ContractError(f"Dockerfile.ci does not consume {fragment!r}")
    forbidden = ("APT_MIRROR", "PIP_INDEX_URL", "pip install --upgrade", "ubuntu:latest")
    for fragment in forbidden:
        if fragment in text:
            raise ContractError(f"Dockerfile.ci contains mutable installation path {fragment!r}")


def _verify_darwin_lock(root: Path) -> None:
    """Validate the exact GitHub-hosted Darwin image and vcpkg registry identity."""
    path = root / "ci/locks/darwin-runner-lock.json"
    lock = _load_json(path)
    expected = {
        "schema", "architecture", "image_os", "image_version",
        "runner_label", "triplet", "vcpkg_commit",
    }
    if not isinstance(lock, dict):
        raise ContractError(f"{path}: root must be an object")
    _exact_keys(lock, expected, str(path))
    exact_values = {
        "schema": "photospider-darwin-runner-lock-v1",
        "architecture": "x86_64",
        "image_os": "macos15",
        "runner_label": "macos-15",
        "triplet": "x64-osx",
    }
    for field, expected_value in exact_values.items():
        if lock[field] != expected_value:
            raise ContractError(f"{path}: unexpected {field} identity")
    if not re.fullmatch(r"20[0-9]{6}\.[0-9]{4}\.[0-9]+", str(lock["image_version"])):
        raise ContractError(f"{path}: malformed GitHub runner image version")
    if not re.fullmatch(r"[0-9a-f]{40}", str(lock["vcpkg_commit"])):
        raise ContractError(f"{path}: vcpkg identity is not a full commit SHA")
    integration = (root / ".github/workflows/ci-integration.yml").read_text(encoding="utf-8")
    if "runs-on: macos-15" not in integration or "security-darwin:" not in integration:
        raise ContractError("CI integration does not consume the protected Darwin runner identity")


def _verify_linux_runner_lock(root: Path) -> None:
    """Validate the hosted Linux builder lock and canonical image-manifest coverage."""
    path = root / "ci/locks/linux-runner-lock.json"
    lock = _load_json(path)
    expected = {"schema", "architecture", "image_os", "image_version", "runner_label"}
    if not isinstance(lock, dict):
        raise ContractError(f"{path}: root must be an object")
    _exact_keys(lock, expected, str(path))
    exact_values = {
        "schema": "photospider-linux-runner-lock-v1",
        "architecture": "x86_64",
        "image_os": "ubuntu24",
        "runner_label": "ubuntu-24.04",
    }
    for field, expected_value in exact_values.items():
        if lock[field] != expected_value:
            raise ContractError(f"{path}: unexpected {field} identity")
    if not re.fullmatch(r"20[0-9]{6}\.[0-9]{3,4}\.[0-9]+", str(lock["image_version"])):
        raise ContractError(f"{path}: malformed GitHub runner image version")
    image_lock = _load_json(root / "ci/locks/ci-image-lock.json")
    if "ci/locks/linux-runner-lock.json" not in image_lock.get("input_paths", []):
        raise ContractError("Linux builder identity is absent from canonical image inputs")


def _verify_fuzz_job_timeout(root: Path) -> None:
    """Require matrix job bounds to wrap the complete generic fuzz execution."""
    runner_path = root / "ci/scripts/fuzz_smoke.sh"
    timeout_path = root / "ci/scripts/ci_command_timeout.py"
    if not timeout_path.is_file() or timeout_path.is_symlink():
        raise ContractError(f"{timeout_path}: portable timeout helper is unavailable or unsafe")
    runner = runner_path.read_text(encoding="utf-8")
    required = (
        "read_fuzz_profile job_timeout",
        "CI_FUZZ_JOB_TIMEOUT_ACTIVE",
        'exec python3 "$SCRIPT_DIR/ci_command_timeout.py"',
        '--timeout-minutes "$job_timeout_minutes"',
        '-- bash "$SCRIPT_DIR/fuzz_smoke.sh" --bounded-worker',
        '"-timeout=$timeout"',
    )
    for fragment in required:
        if fragment not in runner:
            raise ContractError(
                f"{runner_path}: matrix fuzz timeout contract differs at {fragment!r}"
            )


def verify(root: Path) -> None:
    """Run the complete protected lock and active-consumer validation."""
    actions = _read_actions(root / "ci/locks/actions.lock")
    _verify_packages(root / "ci/locks/ubuntu-24.04-packages.lock")
    _verify_requirements(root / "ci/locks/requirements-ci.txt")
    _verify_image_lock(root, actions)
    _verify_dockerfile(root)
    _verify_darwin_lock(root)
    _verify_linux_runner_lock(root)
    _verify_fuzz_job_timeout(root)
    _verify_workflows(root, actions)


def main() -> int:
    """Parse CLI arguments and return zero only for a valid protected surface."""
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(
        "--repo-root",
        type=Path,
        default=Path(__file__).resolve().parents[2],
        help="repository root (defaults to this script's repository)",
    )
    arguments = parser.parse_args()
    try:
        verify(arguments.repo_root.resolve())
    except ContractError as error:
        print(f"ci lock verification failed: {error}", file=sys.stderr)
        return 1
    print("ci lock verification passed")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
