#!/usr/bin/env python3
"""Verify the exact maintained GitHub-hosted runner image identity.

The hosted labels are mutable even when their OS major is explicit. Protected
jobs call this reader before building, signing, resolving images, or executing
candidate-controlled commands. A published runner-image rotation therefore
fails closed until its protected lock is reviewed and refreshed.
"""

from __future__ import annotations

import argparse
import json
import os
import platform
import re
import sys
from pathlib import Path
from typing import Any


class RunnerError(ValueError):
    """Report missing, malformed, or drifted hosted-runner identity."""


def _unique_object(pairs: list[tuple[str, Any]]) -> dict[str, Any]:
    """Build a JSON object while rejecting duplicate members."""
    result: dict[str, Any] = {}
    for key, value in pairs:
        if key in result:
            raise RunnerError(f"duplicate JSON member: {key}")
        result[key] = value
    return result


def _load(path: Path) -> dict[str, Any]:
    """Load one exact runner lock with its platform-specific schema."""
    try:
        value = json.loads(path.read_text(encoding="utf-8"), object_pairs_hook=_unique_object)
    except (OSError, UnicodeError, json.JSONDecodeError, RunnerError) as error:
        raise RunnerError(f"cannot read runner lock {path}: {error}") from error
    if not isinstance(value, dict):
        raise RunnerError(f"{path}: runner lock root must be an object")
    return value


def verify(root: Path, requested: str, runner_label: str | None = None) -> dict[str, str]:
    """Validate runtime OS, architecture, hosted image, and bound runner label.

    Args:
        root: Repository root containing the protected platform lock.
        requested: Exact maintained platform name, ``Darwin`` or ``Linux``.
        runner_label: Optional protected-workflow ``runs-on`` label. Security
            consumers provide it explicitly so runtime and workflow identities
            are cross-bound before candidate-controlled execution.

    Returns:
        The canonical verified hosted-runner identity.

    Raises:
        RunnerError: If schema, runtime, image, architecture, or supplied label
            differs from the protected lock.
    """
    if requested == "Linux":
        path = root / "ci/locks/linux-runner-lock.json"
        expected_fields = {"schema", "architecture", "image_os", "image_version", "runner_label"}
        expected_schema = "photospider-linux-runner-lock-v1"
    elif requested == "Darwin":
        path = root / "ci/locks/darwin-runner-lock.json"
        expected_fields = {
            "schema", "architecture", "image_os", "image_version", "runner_label",
            "triplet", "vcpkg_commit",
        }
        expected_schema = "photospider-darwin-runner-lock-v1"
    else:
        raise RunnerError(f"unsupported runner platform: {requested}")
    lock = _load(path)
    if set(lock) != expected_fields or lock.get("schema") != expected_schema:
        raise RunnerError(f"{path}: missing, unknown, or version-mismatched fields")
    for field in expected_fields - {"schema"}:
        if not isinstance(lock[field], str) or not lock[field]:
            raise RunnerError(f"{path}: empty runner identity field {field}")
    if not re.fullmatch(r"20[0-9]{6}\.[0-9]{3,4}\.[0-9]+", lock["image_version"]):
        raise RunnerError(f"{path}: malformed runner image version")
    actual_system = platform.system()
    actual_architecture = platform.machine()
    if actual_system != requested:
        raise RunnerError(f"runtime platform {actual_system!r} differs from requested {requested!r}")
    if actual_architecture != lock["architecture"]:
        raise RunnerError(
            f"runtime architecture {actual_architecture!r} differs from protected {lock['architecture']!r}"
        )
    if runner_label is not None and runner_label != lock["runner_label"]:
        raise RunnerError(
            f"runner label {runner_label!r} differs from protected {lock['runner_label']!r}"
        )
    actual_image_os = os.environ.get("ImageOS", "")
    actual_image_version = os.environ.get("ImageVersion", "")
    if actual_image_os != lock["image_os"] or actual_image_version != lock["image_version"]:
        raise RunnerError(
            f"runner image {actual_image_os or 'unset'}/{actual_image_version or 'unset'} "
            f"differs from protected {lock['image_os']}/{lock['image_version']}"
        )
    return {
        "architecture": lock["architecture"],
        "image_os": lock["image_os"],
        "image_version": lock["image_version"],
        "platform": requested,
        "runner_label": lock["runner_label"],
    }


def main() -> int:
    """Parse CLI input and print canonical verified runner identity JSON."""
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--repo-root", type=Path, default=Path(__file__).resolve().parents[2])
    parser.add_argument("--platform", choices=("Darwin", "Linux"), required=True)
    parser.add_argument(
        "--runner-label",
        help="protected workflow runs-on label to cross-check",
    )
    arguments = parser.parse_args()
    try:
        result = verify(
            arguments.repo_root.resolve(), arguments.platform, arguments.runner_label
        )
    except RunnerError as error:
        print(f"CI runner verification failed: {error}", file=sys.stderr)
        return 1
    print(json.dumps(result, sort_keys=True, separators=(",", ":"), ensure_ascii=True))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
