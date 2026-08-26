#!/usr/bin/env python3
"""Resolve one reviewed GitHub-hosted runner image into retained provenance.

GitHub's ``ubuntu-24.04`` and ``macos-15`` labels are mutable and an image
rollout can serve two exact versions concurrently. The protected locks hold a
small, reviewed rollout set. This verifier reads the process environment once,
selects exactly one approved member, cross-checks the runtime and workflow
label, and writes a canonical resolved identity file for every later consumer
in the same job. Unknown versions and malformed or non-canonical locks fail
closed before candidate work executes.
"""

from __future__ import annotations

import argparse
import json
import os
import platform
import re
import stat
import sys
from pathlib import Path
from typing import Any


class RunnerError(ValueError):
    """Report missing, malformed, drifted, or ambiguously resolved identity."""


_VERSION_PATTERN = re.compile(r"20[0-9]{6}\.[0-9]{3,4}\.[0-9]+")
_COMMIT_PATTERN = re.compile(r"[0-9a-f]{40}")
_RESOLVED_SCHEMA = "photospider-runner-runtime-identity-v1"


def _unique_object(pairs: list[tuple[str, Any]]) -> dict[str, Any]:
    """Build a JSON object while rejecting duplicate members."""
    result: dict[str, Any] = {}
    for key, value in pairs:
        if key in result:
            raise RunnerError(f"duplicate JSON member: {key}")
        result[key] = value
    return result


def _lock_bytes(value: Any) -> bytes:
    """Return the exact readable canonical encoding required for runner locks."""
    return (
        json.dumps(value, sort_keys=True, indent=2, ensure_ascii=True) + "\n"
    ).encode("utf-8")


def canonical_identity_bytes(value: Any) -> bytes:
    """Return the compact canonical encoding used by retained runtime records."""
    return (
        json.dumps(value, sort_keys=True, separators=(",", ":"), ensure_ascii=True)
        + "\n"
    ).encode("utf-8")


def _read_regular_bytes(path: Path) -> bytes:
    """Read one regular non-link file through a nonblocking safe descriptor.

    Args:
        path: Exact protected lock or retained runtime identity path.

    Returns:
        Bytes read from one opened regular-file descriptor.

    Raises:
        RunnerError: The platform lacks ``O_NOFOLLOW``, ``O_NONBLOCK``, or
            ``O_CLOEXEC``; or the path cannot be opened as one stable regular
            file without following a link or blocking on a special file.

    Note:
        Linux and Darwin are the only maintained platforms, and all three flags
        are mandatory there. ``O_NONBLOCK`` makes a hostile FIFO open bounded
        so that ``fstat`` can reject it; ``O_CLOEXEC`` prevents retained lock or
        identity descriptors from leaking into a child process.
    """
    required_flags = ("O_NOFOLLOW", "O_NONBLOCK", "O_CLOEXEC")
    missing_flags = [
        name for name in required_flags if not isinstance(getattr(os, name, None), int)
    ]
    if missing_flags:
        raise RunnerError(
            "required safe-open flags are unavailable for runner identity input: "
            + ", ".join(missing_flags)
        )
    flags = (
        os.O_RDONLY
        | getattr(os, "O_NOFOLLOW")
        | getattr(os, "O_NONBLOCK")
        | getattr(os, "O_CLOEXEC")
    )
    try:
        descriptor = os.open(path, flags)
    except OSError as error:
        raise RunnerError(
            f"cannot open regular runner identity {path}: {error}"
        ) from error
    try:
        metadata = os.fstat(descriptor)
        if not stat.S_ISREG(metadata.st_mode):
            raise RunnerError(f"runner identity is not a regular file: {path}")
        chunks: list[bytes] = []
        byte_count = 0
        while True:
            chunk = os.read(descriptor, 1024 * 1024)
            if not chunk:
                break
            chunks.append(chunk)
            byte_count += len(chunk)
        after = os.fstat(descriptor)
        if (
            after.st_dev != metadata.st_dev
            or after.st_ino != metadata.st_ino
            or after.st_size != metadata.st_size
            or after.st_mtime_ns != metadata.st_mtime_ns
            or after.st_ctime_ns != metadata.st_ctime_ns
            or byte_count != metadata.st_size
        ):
            raise RunnerError(f"runner identity changed while being read: {path}")
        return b"".join(chunks)
    finally:
        os.close(descriptor)


def _decode_object(path: Path, *, lock_encoding: bool) -> dict[str, Any]:
    """Decode one unique-member JSON object and enforce canonical bytes."""
    raw = _read_regular_bytes(path)
    try:
        value = json.loads(raw.decode("utf-8"), object_pairs_hook=_unique_object)
    except (UnicodeError, json.JSONDecodeError, RunnerError) as error:
        raise RunnerError(f"cannot decode runner identity {path}: {error}") from error
    if not isinstance(value, dict):
        raise RunnerError(f"{path}: runner identity root must be an object")
    expected = _lock_bytes(value) if lock_encoding else canonical_identity_bytes(value)
    if raw != expected:
        raise RunnerError(f"{path}: runner identity JSON is not bytewise canonical")
    return value


def _nonempty_string(value: Any, context: str) -> str:
    """Return one nonempty string or reject the named lock field."""
    if not isinstance(value, str) or not value:
        raise RunnerError(f"{context} must be one nonempty string")
    return value


def load_runner_lock(root: Path, requested: str) -> dict[str, Any]:
    """Load and validate one finite reviewed runner-image rollout set.

    Args:
        root: Repository root containing ``ci/locks``.
        requested: Exact maintained platform, ``Linux`` or ``Darwin``.

    Returns:
        Strict canonical lock with a nonempty sorted unique approved set.

    Raises:
        RunnerError: Schema, fields, values, record order, uniqueness, or bytes
            differ from the maintained v2 contract.
    """
    if requested == "Linux":
        path = root / "ci/locks/linux-runner-lock.json"
        expected_fields = {
            "approved_image_versions",
            "architecture",
            "image_os",
            "runner_label",
            "schema",
        }
        expected_schema = "photospider-linux-runner-lock-v2"
    elif requested == "Darwin":
        path = root / "ci/locks/darwin-runner-lock.json"
        expected_fields = {
            "approved_images",
            "architecture",
            "image_os",
            "runner_label",
            "schema",
            "triplet",
        }
        expected_schema = "photospider-darwin-runner-lock-v2"
    else:
        raise RunnerError(f"unsupported runner platform: {requested}")
    lock = _decode_object(path, lock_encoding=True)
    if set(lock) != expected_fields or lock.get("schema") != expected_schema:
        raise RunnerError(f"{path}: missing, unknown, or version-mismatched fields")
    variable_fields = {"schema", "approved_image_versions", "approved_images"}
    for field in expected_fields - variable_fields:
        _nonempty_string(lock[field], f"{path}: {field}")

    if requested == "Linux":
        versions = lock["approved_image_versions"]
        if not isinstance(versions, list) or not versions:
            raise RunnerError(f"{path}: approved_image_versions must be nonempty")
        if not all(
            isinstance(item, str) and _VERSION_PATTERN.fullmatch(item)
            for item in versions
        ):
            raise RunnerError(f"{path}: approved Linux image version is malformed")
        if versions != sorted(versions, key=lambda item: item.encode("utf-8")):
            raise RunnerError(f"{path}: approved Linux image versions are not bytewise sorted")
        if len(versions) != len(set(versions)):
            raise RunnerError(f"{path}: approved Linux image versions are not unique")
    else:
        records = lock["approved_images"]
        if not isinstance(records, list) or not records:
            raise RunnerError(f"{path}: approved_images must be nonempty")
        versions: list[str] = []
        commits: list[str] = []
        for index, record in enumerate(records):
            if not isinstance(record, dict) or set(record) != {
                "image_version",
                "vcpkg_commit",
            }:
                raise RunnerError(f"{path}: approved Darwin record {index} is malformed")
            version = record["image_version"]
            commit = record["vcpkg_commit"]
            if not isinstance(version, str) or _VERSION_PATTERN.fullmatch(version) is None:
                raise RunnerError(f"{path}: approved Darwin image version is malformed")
            if not isinstance(commit, str) or _COMMIT_PATTERN.fullmatch(commit) is None:
                raise RunnerError(f"{path}: approved Darwin vcpkg commit is malformed")
            versions.append(version)
            commits.append(commit)
        if versions != sorted(versions, key=lambda item: item.encode("utf-8")):
            raise RunnerError(f"{path}: approved Darwin records are not bytewise sorted")
        if len(versions) != len(set(versions)) or len(commits) != len(set(commits)):
            raise RunnerError(f"{path}: approved Darwin versions and commits must be unique")
    return lock


def resolve_approved_identity(
    root: Path, requested: str, image_version: str
) -> dict[str, str]:
    """Resolve one exact approved image version without reading runtime state."""
    lock = load_runner_lock(root, requested)
    if requested == "Linux":
        matches = [
            version
            for version in lock["approved_image_versions"]
            if version == image_version
        ]
        if len(matches) != 1:
            raise RunnerError(f"Linux image version {image_version!r} is not uniquely approved")
        return {
            "architecture": lock["architecture"],
            "image_os": lock["image_os"],
            "image_version": matches[0],
            "platform": "Linux",
            "runner_label": lock["runner_label"],
            "schema": _RESOLVED_SCHEMA,
        }
    matches = [
        record for record in lock["approved_images"] if record["image_version"] == image_version
    ]
    if len(matches) != 1:
        raise RunnerError(f"Darwin image version {image_version!r} is not uniquely approved")
    return {
        "architecture": lock["architecture"],
        "image_os": lock["image_os"],
        "image_version": matches[0]["image_version"],
        "platform": "Darwin",
        "runner_label": lock["runner_label"],
        "schema": _RESOLVED_SCHEMA,
        "triplet": lock["triplet"],
        "vcpkg_commit": matches[0]["vcpkg_commit"],
    }


def validate_resolved_identity(
    root: Path, requested: str, identity: Any
) -> dict[str, str]:
    """Require a resolved record to equal one current approved lock member."""
    if not isinstance(identity, dict):
        raise RunnerError("resolved runner identity must be an object")
    version = identity.get("image_version")
    if not isinstance(version, str):
        raise RunnerError("resolved runner image version must be a string")
    expected = resolve_approved_identity(root, requested, version)
    if identity != expected:
        raise RunnerError("resolved runner identity differs from its approved lock member")
    return expected


def load_resolved_identity(
    path: Path, root: Path, requested: str
) -> dict[str, str]:
    """Load a retained canonical runtime identity and rebind it to the lock."""
    return validate_resolved_identity(
        root, requested, _decode_object(path, lock_encoding=False)
    )


def _write_new_identity(path: Path, identity: dict[str, str]) -> None:
    """Create one mode-0600 retained identity without following residual state.

    Args:
        path: Fresh job-owned output path.
        identity: Validated canonical runtime identity to retain.

    Raises:
        RunnerError: ``O_NOFOLLOW`` or ``O_CLOEXEC`` is unavailable, the output
            path already exists or aliases residual state, or the exact record
            cannot be written completely.

    Note:
        ``O_EXCL`` makes every pre-existing regular, link, FIFO, or device path
        a hard failure. ``O_NONBLOCK`` is unnecessary because this function
        creates a fresh regular file rather than opening an existing object.
    """
    required_flags = ("O_NOFOLLOW", "O_CLOEXEC")
    missing_flags = [
        name for name in required_flags if not isinstance(getattr(os, name, None), int)
    ]
    if missing_flags:
        raise RunnerError(
            "required safe-open flags are unavailable for runner identity output: "
            + ", ".join(missing_flags)
        )
    path.parent.mkdir(parents=True, exist_ok=True)
    flags = (
        os.O_WRONLY
        | os.O_CREAT
        | os.O_EXCL
        | getattr(os, "O_NOFOLLOW")
        | getattr(os, "O_CLOEXEC")
    )
    try:
        descriptor = os.open(path, flags, 0o600)
    except OSError as error:
        raise RunnerError(f"cannot create retained runner identity {path}: {error}") from error
    try:
        value = canonical_identity_bytes(identity)
        written = 0
        while written < len(value):
            count = os.write(descriptor, value[written:])
            if count <= 0:
                raise RunnerError(f"short write for retained runner identity {path}")
            written += count
        os.fsync(descriptor)
    finally:
        os.close(descriptor)


def verify(root: Path, requested: str, runner_label: str) -> dict[str, str]:
    """Measure the runtime once and select one exact approved identity.

    Args:
        root: Repository root containing the protected platform lock.
        requested: Exact maintained platform name, ``Darwin`` or ``Linux``.
        runner_label: Protected-workflow ``runs-on`` label to cross-bind.

    Returns:
        One canonical resolved hosted-runner identity. Darwin also includes the
        exact triplet and vcpkg commit mapped to the measured image version.

    Raises:
        RunnerError: Runtime, environment, label, or approved-set resolution
            fails. ``ImageOS`` and ``ImageVersion`` are each read exactly once.
    """
    lock = load_runner_lock(root, requested)
    actual_system = platform.system()
    actual_architecture = platform.machine()
    actual_image_os = os.environ.get("ImageOS", "")
    actual_image_version = os.environ.get("ImageVersion", "")
    if actual_system != requested:
        raise RunnerError(f"runtime platform {actual_system!r} differs from requested {requested!r}")
    if actual_architecture != lock["architecture"]:
        raise RunnerError(
            f"runtime architecture {actual_architecture!r} differs from protected "
            f"{lock['architecture']!r}"
        )
    if runner_label != lock["runner_label"]:
        raise RunnerError(
            f"runner label {runner_label!r} differs from protected {lock['runner_label']!r}"
        )
    if actual_image_os != lock["image_os"]:
        raise RunnerError(
            f"runner image OS {actual_image_os or 'unset'!r} differs from protected "
            f"{lock['image_os']!r}"
        )
    return resolve_approved_identity(root, requested, actual_image_version)


def main() -> int:
    """Verify the current job and create its retained canonical identity file."""
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--repo-root", type=Path, default=Path(__file__).resolve().parents[2])
    parser.add_argument("--platform", choices=("Darwin", "Linux"), required=True)
    parser.add_argument("--runner-label", required=True)
    parser.add_argument("--output", type=Path, required=True)
    arguments = parser.parse_args()
    try:
        result = verify(
            arguments.repo_root.resolve(), arguments.platform, arguments.runner_label
        )
        _write_new_identity(arguments.output, result)
    except RunnerError as error:
        print(f"CI runner verification failed: {error}", file=sys.stderr)
        return 1
    sys.stdout.buffer.write(canonical_identity_bytes(result))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
