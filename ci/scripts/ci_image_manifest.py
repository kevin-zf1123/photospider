#!/usr/bin/env python3
"""Create and verify the canonical Photospider CI-image input manifest.

The manifest hashes every repository-protected build input named by the image
lock and binds those bytes to the exact source commit, repository, base image,
and full-SHA builder action. Before a workflow may publish, the same helper
requires the workflow commit to equal the newest canonical image-input commit;
this makes the producer's implicit GitHub attestation source digest identical
to the source identity expected by consumers. The output is canonical JSON
with a final newline. No candidate-owned script or generated value is trusted
as an input list.
"""

from __future__ import annotations

import argparse
import hashlib
import json
import os
import re
import subprocess
import sys
from pathlib import Path
from typing import Any


class ManifestError(ValueError):
    """Report malformed, unsafe, or mismatched image-manifest state."""


def _unique_object(pairs: list[tuple[str, Any]]) -> dict[str, Any]:
    """Reject duplicate JSON members while loading protected input."""
    result: dict[str, Any] = {}
    for key, value in pairs:
        if key in result:
            raise ManifestError(f"duplicate JSON member: {key}")
        result[key] = value
    return result


def _load_json(path: Path) -> Any:
    """Load strict UTF-8 JSON with duplicate-member rejection."""
    try:
        return json.loads(path.read_text(encoding="utf-8"), object_pairs_hook=_unique_object)
    except (OSError, UnicodeError, json.JSONDecodeError, ManifestError) as error:
        raise ManifestError(f"cannot read strict JSON {path}: {error}") from error


def _canonical_bytes(value: Any) -> bytes:
    """Serialize a value using the repository's canonical JSON encoding."""
    return (json.dumps(value, sort_keys=True, separators=(",", ":"), ensure_ascii=True) + "\n").encode()


def _sha256_bytes(value: bytes) -> str:
    """Return a lowercase SHA-256 hex digest."""
    return hashlib.sha256(value).hexdigest()


def _sha256_file(path: Path) -> str:
    """Hash one regular file without following an input-list symlink."""
    if not path.is_file() or path.is_symlink():
        raise ManifestError(f"image input is not a regular file: {path}")
    digest = hashlib.sha256()
    with path.open("rb") as handle:
        for chunk in iter(lambda: handle.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()


def _read_builder_commit(root: Path, action: str, release: str) -> str:
    """Resolve the declared builder to exactly one protected action-lock row."""
    matches: list[str] = []
    for raw_line in (root / "ci/locks/actions.lock").read_text(encoding="utf-8").splitlines():
        if not raw_line or raw_line.startswith("#"):
            continue
        fields = raw_line.split("\t")
        if len(fields) != 3:
            raise ManifestError("malformed action lock row")
        if fields[:2] == [action, release]:
            matches.append(fields[2])
    if len(matches) != 1 or not re.fullmatch(r"[0-9a-f]{40}", matches[0]):
        raise ManifestError(f"builder identity {action}@{release} is missing or ambiguous")
    return matches[0]


def create_manifest(root: Path, source_commit: str, repository: str) -> dict[str, Any]:
    """Create the canonical manifest value for one exact protected source state."""
    if not re.fullmatch(r"[0-9a-f]{40}", source_commit):
        raise ManifestError("source commit must be a lowercase full SHA")
    if not re.fullmatch(r"[A-Za-z0-9_.-]+/[A-Za-z0-9_.-]+", repository):
        raise ManifestError("repository must be an owner/name identity")
    lock_path = root / "ci/locks/ci-image-lock.json"
    lock = _load_json(lock_path)
    if not isinstance(lock, dict) or lock.get("schema") != "photospider-ci-image-lock-v1":
        raise ManifestError("unknown CI image lock schema")
    paths = lock.get("input_paths")
    if not isinstance(paths, list) or not all(isinstance(item, str) for item in paths):
        raise ManifestError("input_paths must be a string array")
    if paths != sorted(paths) or len(paths) != len(set(paths)):
        raise ManifestError("input_paths must be sorted and unique")
    inputs: list[dict[str, Any]] = []
    for relative in paths:
        if relative.startswith("/") or ".." in Path(relative).parts:
            raise ManifestError(f"unsafe image input path: {relative!r}")
        input_path = root / relative
        inputs.append(
            {
                "path": relative,
                "sha256": _sha256_file(input_path),
                "size": input_path.stat().st_size,
            }
        )
    builder = lock.get("builder")
    if not isinstance(builder, dict) or set(builder) != {"action", "release"}:
        raise ManifestError("builder lock is malformed")
    builder_commit = _read_builder_commit(root, builder["action"], builder["release"])
    runner = _load_json(root / "ci/locks/linux-runner-lock.json")
    if not isinstance(runner, dict) or set(runner) != {
        "schema", "architecture", "image_os", "image_version", "runner_label"
    }:
        raise ManifestError("Linux builder runner lock is malformed")
    if runner["schema"] != "photospider-linux-runner-lock-v1":
        raise ManifestError("Linux builder runner schema is unknown")
    return {
        "apt_snapshot": lock["apt_snapshot"],
        "base_image": lock["base_image"],
        "builder": {
            "action": builder["action"],
            "commit": builder_commit,
            "release": builder["release"],
        },
        "builder_runner": runner,
        "inputs": inputs,
        "repository": repository,
        "schema": "photospider-ci-image-input-v1",
        "source_commit": source_commit,
    }


def _git_source_commit(root: Path) -> str:
    """Return the newest commit that changed any canonical image input path."""
    lock = _load_json(root / "ci/locks/ci-image-lock.json")
    paths = lock.get("input_paths") if isinstance(lock, dict) else None
    if not isinstance(paths, list) or not paths:
        raise ManifestError("image input path lock is empty or malformed")
    command = ["git", "-C", str(root), "log", "-1", "--format=%H", "--", *paths]
    completed = subprocess.run(command, check=False, text=True, capture_output=True)
    commit = completed.stdout.strip()
    if completed.returncode != 0 or not re.fullmatch(r"[0-9a-f]{40}", commit):
        detail = completed.stderr.strip() or "no exact image-input commit found"
        raise ManifestError(f"cannot resolve image source commit: {detail}")
    return commit


def _write_output(path: Path, value: bytes) -> None:
    """Write one output atomically without accepting a symlink destination."""
    if path.exists() and path.is_symlink():
        raise ManifestError(f"refusing symlink output: {path}")
    path.parent.mkdir(parents=True, exist_ok=True)
    temporary = path.with_name(path.name + f".tmp.{os.getpid()}")
    temporary.write_bytes(value)
    temporary.replace(path)


def _verify_manifest_file(path: Path) -> tuple[dict[str, Any], str]:
    """Load a canonical manifest and return its value and SHA-256 digest."""
    value = _load_json(path)
    if not isinstance(value, dict) or value.get("schema") != "photospider-ci-image-input-v1":
        raise ManifestError("unknown image input manifest schema")
    canonical = _canonical_bytes(value)
    if path.read_bytes() != canonical:
        raise ManifestError("image input manifest is not canonical JSON")
    return value, _sha256_bytes(canonical)


def _command_create(arguments: argparse.Namespace) -> None:
    """Create a canonical manifest and optional digest sidecar."""
    manifest = create_manifest(arguments.repo_root, arguments.source_commit, arguments.repository)
    canonical = _canonical_bytes(manifest)
    _write_output(arguments.output, canonical)
    digest = _sha256_bytes(canonical)
    if arguments.digest_output:
        _write_output(arguments.digest_output, (digest + "\n").encode())
    print(digest)


def _command_source_commit(arguments: argparse.Namespace) -> None:
    """Print the exact last image-input-changing commit."""
    print(_git_source_commit(arguments.repo_root))


def _command_publish_source_commit(arguments: argparse.Namespace) -> None:
    """Emit a publishable source only when the workflow can attest it exactly.

    The GitHub artifact attestation records the workflow checkout commit as its
    source digest. Consumers instead derive the last commit touching canonical
    image inputs. Publishing is therefore safe only when those identities are
    equal; a manual run or multi-commit push from a later unrelated HEAD fails
    before registry authentication or image publication.
    """
    if not re.fullmatch(r"[0-9a-f]{40}", arguments.workflow_commit):
        raise ManifestError("workflow commit must be a lowercase full SHA")
    source_commit = _git_source_commit(arguments.repo_root)
    if source_commit != arguments.workflow_commit:
        raise ManifestError(
            "canonical image source commit "
            f"{source_commit} differs from workflow commit {arguments.workflow_commit}; "
            "refusing an unattestable publication"
        )
    print(source_commit)


def _command_verify(arguments: argparse.Namespace) -> None:
    """Recreate and compare a manifest against expected repository state."""
    actual, actual_digest = _verify_manifest_file(arguments.manifest)
    expected = create_manifest(arguments.repo_root, arguments.source_commit, arguments.repository)
    if actual != expected:
        raise ManifestError("manifest does not match current protected inputs and expected identity")
    if arguments.expected_digest and actual_digest != arguments.expected_digest:
        raise ManifestError(
            f"manifest digest {actual_digest} does not match expected {arguments.expected_digest}"
        )
    print(actual_digest)


def _command_verify_labels(arguments: argparse.Namespace) -> None:
    """Verify inspected OCI labels against an exact manifest and source commit."""
    _, manifest_digest = _verify_manifest_file(arguments.manifest)
    labels = _load_json(arguments.labels_json)
    if not isinstance(labels, dict) or not all(
        isinstance(key, str) and isinstance(value, str) for key, value in labels.items()
    ):
        raise ManifestError("inspected OCI labels must be a string object")
    lock = _load_json(arguments.repo_root / "ci/locks/ci-image-lock.json")
    published = lock.get("published_image") if isinstance(lock, dict) else None
    if not isinstance(published, dict):
        raise ManifestError("published image lock is malformed")
    expected = {
        published["input_manifest_label"]: manifest_digest,
        published["source_commit_label"]: arguments.source_commit,
    }
    for label, expected_value in expected.items():
        if labels.get(label) != expected_value:
            raise ManifestError(
                f"OCI label {label!r} is {labels.get(label)!r}, expected {expected_value!r}"
            )
    print(manifest_digest)


def build_parser() -> argparse.ArgumentParser:
    """Build the explicit subcommand CLI used by workflows and tests."""
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(
        "--repo-root",
        type=Path,
        default=Path(__file__).resolve().parents[2],
        help="repository root",
    )
    subparsers = parser.add_subparsers(dest="command", required=True)
    create = subparsers.add_parser("create", help="create a canonical image input manifest")
    create.add_argument("--source-commit", required=True)
    create.add_argument("--repository", required=True)
    create.add_argument("--output", type=Path, required=True)
    create.add_argument("--digest-output", type=Path)
    create.set_defaults(handler=_command_create)
    source = subparsers.add_parser("source-commit", help="find the last image-input commit")
    source.set_defaults(handler=_command_source_commit)
    publish_source = subparsers.add_parser(
        "publish-source-commit",
        help="require the workflow commit to equal the last image-input commit",
    )
    publish_source.add_argument("--workflow-commit", required=True)
    publish_source.set_defaults(handler=_command_publish_source_commit)
    verify = subparsers.add_parser("verify", help="verify a canonical manifest")
    verify.add_argument("--source-commit", required=True)
    verify.add_argument("--repository", required=True)
    verify.add_argument("--manifest", type=Path, required=True)
    verify.add_argument("--expected-digest")
    verify.set_defaults(handler=_command_verify)
    labels = subparsers.add_parser("verify-labels", help="verify exact OCI identity labels")
    labels.add_argument("--manifest", type=Path, required=True)
    labels.add_argument("--labels-json", type=Path, required=True)
    labels.add_argument("--source-commit", required=True)
    labels.set_defaults(handler=_command_verify_labels)
    return parser


def main() -> int:
    """Run the selected manifest operation and normalize diagnostics."""
    parser = build_parser()
    arguments = parser.parse_args()
    arguments.repo_root = arguments.repo_root.resolve()
    try:
        arguments.handler(arguments)
    except (ManifestError, OSError) as error:
        print(f"ci image manifest failed: {error}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
