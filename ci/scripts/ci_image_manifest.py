#!/usr/bin/env python3
"""Create and verify the canonical Photospider CI-image input manifest.

The manifest hashes every repository-protected build input named by the image
lock and binds those bytes to the exact source commit, repository, base image,
full-SHA builder action, and version/full-SHA identities of the installer and
suite-gate helpers. Before a workflow may publish, the same helper
requires the workflow commit to equal the newest canonical image-input commit;
this makes the producer's implicit GitHub attestation source digest identical
to the source identity expected by consumers. The output is canonical JSON
with a final newline. Promotion freshness uses the same protected path lock and
manifest builder against a freshly fetched branch-tip worktree, never a copied
path list or candidate-provided identity. No candidate-owned script or generated
value is trusted as an input list.
"""

from __future__ import annotations

import argparse
import hashlib
import json
import os
import re
import subprocess
import sys
import tempfile
from pathlib import Path, PurePosixPath
from typing import Any

_SCRIPT_DIRECTORY = Path(__file__).resolve().parent
if str(_SCRIPT_DIRECTORY) not in sys.path:
    sys.path.insert(0, str(_SCRIPT_DIRECTORY))
from ci_runner_verify import (
    RunnerError,
    canonical_identity_bytes,
    load_resolved_identity,
    resolve_approved_identity,
    validate_resolved_identity,
)


class ManifestError(ValueError):
    """Report malformed, unsafe, or mismatched image-manifest state."""


_IMAGE_LOCK_RELATIVE_PATH = "ci/locks/ci-image-lock.json"
_IMAGE_LOCK_FIELDS = frozenset(
    {
        "apt_bootstrap",
        "apt_snapshot",
        "base_image",
        "builder",
        "github_cli",
        "input_paths",
        "protected_helpers",
        "published_image",
        "schema",
    }
)


def _unique_object(pairs: list[tuple[str, Any]]) -> dict[str, Any]:
    """Reject duplicate JSON members while loading protected input."""
    result: dict[str, Any] = {}
    for key, value in pairs:
        if key in result:
            raise ManifestError(f"duplicate JSON member: {key}")
        result[key] = value
    return result


def _load_json(path: Path) -> Any:
    """Load one regular, non-symlink strict JSON file without duplicate keys.

    Args:
        path: Exact manifest, lock, label, or evidence path to decode.

    Returns:
        The decoded strict JSON value.

    Raises:
        ManifestError: The path is not a regular non-symlink file, cannot be
            read, is not UTF-8/JSON, or contains a duplicate object member.
    """
    try:
        if not path.is_file() or path.is_symlink():
            raise ManifestError("path is not a regular non-symlink file")
        return _load_json_bytes(path.read_bytes(), str(path))
    except (OSError, ManifestError) as error:
        raise ManifestError(f"cannot read strict JSON {path}: {error}") from error


def _load_json_bytes(value: bytes, context: str) -> Any:
    """Decode strict UTF-8 JSON bytes with duplicate-member rejection.

    Args:
        value: Exact bytes read from a retained file or Git tree object.
        context: Stable source identity used in diagnostics.

    Returns:
        The decoded JSON value.

    Raises:
        ManifestError: UTF-8, JSON syntax, or object-member uniqueness fails.
    """
    try:
        return json.loads(
            value.decode("utf-8"), object_pairs_hook=_unique_object
        )
    except (UnicodeError, json.JSONDecodeError, ManifestError) as error:
        raise ManifestError(f"cannot read strict JSON {context}: {error}") from error


def _image_input_paths(lock: Any, context: str) -> list[str]:
    """Return the sole canonical CI-image input inventory after strict checks.

    Args:
        lock: Decoded candidate CI-image lock.
        context: Stable tree/path identity used in diagnostics.

    Returns:
        The nonempty, bytewise sorted, unique canonical POSIX path list.

    Raises:
        ManifestError: The lock schema, field set, self-inclusion, ordering, or
            any path identity is malformed or unsafe.

    Note:
        Requiring the lock to include its own path makes every input-set
        evolution observable. Base/head classification consumes the union of
        two independently validated revisions, so removing an old input cannot
        hide that input's simultaneous deletion or replacement.
    """
    if (
        not isinstance(lock, dict)
        or set(lock) != _IMAGE_LOCK_FIELDS
        or lock.get("schema") != "photospider-ci-image-lock-v1"
    ):
        raise ManifestError(f"{context}: unknown CI image lock schema")
    paths = lock.get("input_paths")
    if not isinstance(paths, list) or not paths or not all(
        isinstance(item, str) for item in paths
    ):
        raise ManifestError(f"{context}: input_paths must be a nonempty string array")
    if paths != sorted(paths) or len(paths) != len(set(paths)):
        raise ManifestError(f"{context}: input_paths must be sorted and unique")
    for relative in paths:
        pure = PurePosixPath(relative)
        if (
            not relative
            or "\0" in relative
            or "\\" in relative
            or pure.is_absolute()
            or relative != str(pure)
            or relative in (".", "..")
            or ".." in pure.parts
        ):
            raise ManifestError(f"{context}: unsafe image input path: {relative!r}")
    if _IMAGE_LOCK_RELATIVE_PATH not in paths:
        raise ManifestError(f"{context}: image input lock must include itself")
    return paths


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


def _protected_helpers(
    root: Path, lock: dict[str, Any], input_paths: list[str]
) -> dict[str, dict[str, str]]:
    """Validate and return the exact versioned protected-helper identities.

    Args:
        root: Repository root containing the helper files.
        lock: Strict CI-image lock object.
        input_paths: Canonical image-input path inventory.

    Returns:
        The two canonical helper identity records for manifest inclusion.

    Raises:
        ManifestError: A helper record/path/version/hash is malformed, absent
            from image inputs, aliased, or differs from the protected bytes.

    Note:
        The explicit helper identity supplements, rather than replaces, the
        ordinary per-input hash. This binds executable role/version and bytes.
    """
    helpers = lock.get("protected_helpers")
    expected_names = {"ci-image-installer", "integration-suite-gate"}
    if not isinstance(helpers, dict) or set(helpers) != expected_names:
        raise ManifestError("protected helper identity set is malformed")
    result: dict[str, dict[str, str]] = {}
    for name in sorted(expected_names):
        record = helpers[name]
        if not isinstance(record, dict) or set(record) != {
            "path",
            "sha256",
            "version",
        }:
            raise ManifestError(f"protected helper {name!r} record is malformed")
        path = record["path"]
        sha256 = record["sha256"]
        version = record["version"]
        if not isinstance(path, str) or path.startswith("/") or ".." in Path(path).parts:
            raise ManifestError(f"protected helper {name!r} path is unsafe")
        if path not in input_paths:
            raise ManifestError(f"protected helper {name!r} is absent from image inputs")
        if version != "v1" or not isinstance(sha256, str) or re.fullmatch(
            r"[0-9a-f]{64}", sha256
        ) is None:
            raise ManifestError(f"protected helper {name!r} identity is malformed")
        if _sha256_file(root / path) != sha256:
            raise ManifestError(f"protected helper {name!r} bytes differ from the lock")
        result[name] = {"path": path, "sha256": sha256, "version": version}
    return result


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


def create_manifest(
    root: Path,
    source_commit: str,
    repository: str,
    builder_runner: dict[str, str],
) -> dict[str, Any]:
    """Create the manifest for exact source bytes and measured builder runtime.

    Args:
        root: Exact repository state supplying canonical image inputs.
        source_commit: Last commit that changed one canonical image input.
        repository: Locked GitHub ``owner/name`` source identity.
        builder_runner: Retained Linux runtime record produced by
            ``ci_runner_verify.py`` in the actual build job.

    Returns:
        Canonical manifest object whose input list binds the complete reviewed
        rollout lock while ``builder_runner`` binds the one selected member.

    Raises:
        ManifestError: Source, repository, inputs, builder action, or retained
            runner identity is malformed or no longer approved by this source.
    """
    if not re.fullmatch(r"[0-9a-f]{40}", source_commit):
        raise ManifestError("source commit must be a lowercase full SHA")
    if not re.fullmatch(r"[A-Za-z0-9_.-]+/[A-Za-z0-9_.-]+", repository):
        raise ManifestError("repository must be an owner/name identity")
    lock_path = root / "ci/locks/ci-image-lock.json"
    lock = _load_json(lock_path)
    paths = _image_input_paths(lock, str(lock_path))
    protected_helpers = _protected_helpers(root, lock, paths)
    inputs: list[dict[str, Any]] = []
    for relative in paths:
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
    try:
        runner = validate_resolved_identity(root, "Linux", builder_runner)
    except RunnerError as error:
        raise ManifestError(f"Linux builder runtime identity is invalid: {error}") from error
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
        "protected_helpers": protected_helpers,
        "repository": repository,
        "schema": "photospider-ci-image-input-v1",
        "source_commit": source_commit,
    }


def _git_source_commit(root: Path) -> str:
    """Return the newest commit that changed any canonical image input path."""
    lock_path = root / _IMAGE_LOCK_RELATIVE_PATH
    lock = _load_json(lock_path)
    paths = _image_input_paths(lock, str(lock_path))
    command = ["git", "-C", str(root), "log", "-1", "--format=%H", "--", *paths]
    completed = subprocess.run(command, check=False, text=True, capture_output=True)
    commit = completed.stdout.strip()
    if completed.returncode != 0 or not re.fullmatch(r"[0-9a-f]{40}", commit):
        detail = completed.stderr.strip() or "no exact image-input commit found"
        raise ManifestError(f"cannot resolve image source commit: {detail}")
    return commit


def _git_bytes(root: Path, *arguments: str) -> bytes:
    """Run Git without a shell and return its exact stdout bytes.

    Args:
        root: Explicit repository whose objects and refs are authoritative.
        *arguments: Git arguments passed as separate non-shell argv entries.

    Returns:
        Exact stdout bytes from one successful Git process.

    Raises:
        ManifestError: Git cannot execute or returns nonzero.

    Note:
        Byte output preserves NUL path records and filenames not decodable as
        ordinary text; diagnostics alone use replacement-safe UTF-8 rendering.
    """
    try:
        completed = subprocess.run(
            ["git", "-C", str(root), *arguments],
            check=False,
            capture_output=True,
        )
    except OSError as error:
        raise ManifestError(f"cannot execute Git {' '.join(arguments)}: {error}") from error
    if completed.returncode != 0:
        detail = completed.stderr.decode("utf-8", errors="backslashreplace").strip()
        raise ManifestError(
            f"Git {' '.join(arguments)} failed: {detail or 'no diagnostic'}"
        )
    return completed.stdout


def _tree_image_input_paths(root: Path, revision: str) -> list[str]:
    """Load the strict canonical input lock from one immutable Git tree.

    Args:
        root: Repository containing the requested tree object.
        revision: Verified full commit identity used by ``git show``.

    Returns:
        The validated, self-including canonical input paths.

    Raises:
        ManifestError: The tree, lock blob, schema, or paths are invalid.
    """
    lock_bytes = _git_bytes(root, "show", f"{revision}:{_IMAGE_LOCK_RELATIVE_PATH}")
    context = f"{revision}:{_IMAGE_LOCK_RELATIVE_PATH}"
    return _image_input_paths(_load_json_bytes(lock_bytes, context), context)


def _changed_path_bytes(
    root: Path, base: str, head: str | None
) -> tuple[list[bytes], list[str], list[str]]:
    """Return one exact diff plus independently validated base/head authorities.

    Args:
        root: Repository containing the comparison objects and optional worktree.
        base: Commit used directly for a worktree diff or as the merge-base input.
        head: Commit comparison head, or ``None`` for the current worktree.

    Returns:
        Changed Git path bytes, validated base paths, and validated head paths.

    Raises:
        ManifestError: A ref, lock, diff, or path record is unavailable or
            malformed. No false route is emitted on failure.
    """
    try:
        base_commit_text = _git_bytes(
            root, "rev-parse", "--verify", f"{base}^{{commit}}"
        ).decode("ascii").strip()
    except UnicodeError as error:
        raise ManifestError("comparison base is not an ASCII commit") from error
    if re.fullmatch(r"[0-9a-f]{40}", base_commit_text) is None:
        raise ManifestError("comparison base did not resolve to one full commit")
    if head is None:
        authoritative_base = base_commit_text
        base_paths = _tree_image_input_paths(root, authoritative_base)
        head_lock = _load_json(root / _IMAGE_LOCK_RELATIVE_PATH)
        head_paths = _image_input_paths(
            head_lock, f"worktree:{_IMAGE_LOCK_RELATIVE_PATH}"
        )
        raw_paths = _git_bytes(
            root, "diff", "--no-renames", "--name-only", "-z", authoritative_base
        )
    else:
        try:
            head_commit = _git_bytes(
                root, "rev-parse", "--verify", f"{head}^{{commit}}"
            ).decode("ascii").strip()
        except UnicodeError as error:
            raise ManifestError("comparison head is not an ASCII commit") from error
        if re.fullmatch(r"[0-9a-f]{40}", head_commit) is None:
            raise ManifestError("comparison head did not resolve to one full commit")
        try:
            authoritative_base = _git_bytes(
                root, "merge-base", base_commit_text, head_commit
            ).decode("ascii").strip()
        except UnicodeError as error:
            raise ManifestError("comparison merge base is not ASCII") from error
        if re.fullmatch(r"[0-9a-f]{40}", authoritative_base) is None:
            raise ManifestError("comparison merge base is not one full commit")
        base_paths = _tree_image_input_paths(root, authoritative_base)
        head_paths = _tree_image_input_paths(root, head_commit)
        raw_paths = _git_bytes(
            root,
            "diff",
            "--no-renames",
            "--name-only",
            "-z",
            f"{authoritative_base}..{head_commit}",
        )
    if not raw_paths:
        return [], base_paths, head_paths
    if not raw_paths.endswith(b"\0"):
        raise ManifestError("Git changed-path inventory is not NUL terminated")
    changed = raw_paths[:-1].split(b"\0")
    if any(not path for path in changed):
        raise ManifestError("Git changed-path inventory contains an empty record")
    return changed, base_paths, head_paths


def _command_detect_changed(arguments: argparse.Namespace) -> None:
    """Classify one comparison from the union of strict base/head lock paths.

    Args:
        arguments: Parsed repository, base, head/worktree, and diagnostic output
            options from the protected command line.

    Returns:
        None after writing the JSON-quoted diagnostic path log and printing one
        exact ``true`` or ``false`` route value.

    Raises:
        ManifestError: Any revision, lock, path inventory, or Git operation is
            unavailable or malformed. No route value is printed on failure.
    """
    changed, base_paths, head_paths = _changed_path_bytes(
        arguments.repo_root,
        arguments.base,
        None if arguments.worktree else arguments.head,
    )
    canonical = {
        relative.encode("utf-8") for relative in set(base_paths) | set(head_paths)
    }
    image_changed = any(path in canonical for path in changed)
    log_lines = [
        json.dumps(os.fsdecode(path), ensure_ascii=True) for path in changed
    ]
    arguments.changed_files_output.write_text(
        "\n".join(log_lines) + ("\n" if log_lines else ""), encoding="utf-8"
    )
    print("true" if image_changed else "false")


def _run_git(root: Path, *arguments: str) -> subprocess.CompletedProcess[str]:
    """Run one Git command against the explicit repository without a shell.

    Args:
        root: Exact repository whose objects and refs are authoritative.
        *arguments: Git arguments passed as distinct argv entries.

    Returns:
        The completed successful Git process with captured text streams.

    Raises:
        ManifestError: Git returns nonzero or the repository cannot execute Git.

    Note:
        Branch names and URLs remain argv data. Diagnostics are bounded to Git's
        captured stderr and no fetched checkout content is executed.
    """
    try:
        completed = subprocess.run(
            ["git", "-C", str(root), *arguments],
            check=False,
            text=True,
            capture_output=True,
        )
    except OSError as error:
        raise ManifestError(f"cannot execute Git freshness command: {error}") from error
    if completed.returncode != 0:
        detail = completed.stderr.strip() or completed.stdout.strip() or "Git failed"
        raise ManifestError(f"Git freshness command failed: {detail}")
    return completed


def _fetch_branch_tip(
    root: Path, repository: str, branch_name: str, destination_ref: str
) -> str:
    """Fetch one exact GitHub branch into an isolated local freshness ref.

    Args:
        root: Trusted candidate repository receiving the fetched objects.
        repository: Canonical ``owner/name`` GitHub repository identity.
        branch_name: Git-validated full branch name without ``refs/heads/``.
        destination_ref: Private local ref owned by this measurement.

    Returns:
        The fetched branch-tip commit as one lowercase full SHA.

    Raises:
        ManifestError: Fetch, ref resolution, or canonical SHA validation fails.

    Note:
        The source URL is derived solely from the validated repository identity.
        A literal refspec argv prevents branch content from becoming shell input.
    """
    remote_url = f"https://github.com/{repository}.git"
    _run_git(
        root,
        "fetch",
        "--no-tags",
        "--no-recurse-submodules",
        "--no-write-fetch-head",
        "--force",
        remote_url,
        f"+refs/heads/{branch_name}:{destination_ref}",
    )
    commit = _run_git(root, "rev-parse", "--verify", f"{destination_ref}^{{commit}}")
    value = commit.stdout.strip()
    if re.fullmatch(r"[0-9a-f]{40}", value) is None:
        raise ManifestError("freshly fetched branch tip is not a lowercase full SHA")
    return value


def _command_promotion_freshness(arguments: argparse.Namespace) -> None:
    """Compare a candidate with one stable, freshly fetched branch-tip identity.

    Args:
        arguments: Candidate/source/manifest identity, exact branch/repository,
            scratch root, output path, and repository root parsed by argparse.

    Returns:
        None after writing canonical evidence and printing ``current`` or
        ``superseded``.

    Raises:
        ManifestError: Identity syntax, checkout binding, ancestry, fetch,
            worktree creation, manifest measurement, or ref stability fails.

    Note:
        The branch ref is fetched before and after measurement. A change across
        that window fails before mutable registry state can be touched. Exact
        manifest equality allows later documentation-only commits; a later
        image-input source commit is explicitly ``superseded``.
    """
    candidate_commit = arguments.candidate_commit
    candidate_source_commit = arguments.candidate_source_commit
    candidate_manifest_digest = arguments.candidate_manifest_digest
    candidate_builder_image_version = arguments.candidate_builder_image_version
    branch_name = arguments.branch
    repository = arguments.repository
    if re.fullmatch(r"[0-9a-f]{40}", candidate_commit) is None:
        raise ManifestError("promotion candidate commit must be a lowercase full SHA")
    if candidate_source_commit != candidate_commit:
        raise ManifestError("promotion candidate and image source commits must match")
    if re.fullmatch(r"[0-9a-f]{64}", candidate_manifest_digest) is None:
        raise ManifestError("promotion candidate manifest digest is malformed")
    try:
        builder_runner = resolve_approved_identity(
            arguments.repo_root, "Linux", candidate_builder_image_version
        )
    except RunnerError as error:
        raise ManifestError(f"promotion builder runtime is not approved: {error}") from error
    if re.fullmatch(r"[A-Za-z0-9_.-]+/[A-Za-z0-9_.-]+", repository) is None:
        raise ManifestError("promotion repository must be an owner/name identity")
    if branch_name != "main" and not branch_name.startswith("CI/"):
        raise ManifestError("promotion branch must be main or CI/**")
    branch_ref = f"refs/heads/{branch_name}"
    _run_git(arguments.repo_root, "check-ref-format", branch_ref)
    checkout_commit = _run_git(
        arguments.repo_root, "rev-parse", "--verify", "HEAD^{commit}"
    ).stdout.strip()
    if checkout_commit != candidate_commit:
        raise ManifestError("promotion checkout differs from the candidate commit")

    scratch_root = arguments.scratch_root
    if scratch_root.is_symlink() or not scratch_root.is_dir():
        raise ManifestError("promotion scratch root must be one real directory")
    with tempfile.TemporaryDirectory(
        prefix="photospider-ci-promotion-", dir=str(scratch_root)
    ) as temporary_text:
        temporary = Path(temporary_text)
        token = temporary.name
        observed_ref = f"refs/photospider/promotion/{token}/observed"
        verified_ref = f"refs/photospider/promotion/{token}/verified"
        worktree = temporary / "branch-tip"
        worktree_added = False
        try:
            branch_tip = _fetch_branch_tip(
                arguments.repo_root, repository, branch_name, observed_ref
            )
            ancestry = subprocess.run(
                [
                    "git",
                    "-C",
                    str(arguments.repo_root),
                    "merge-base",
                    "--is-ancestor",
                    candidate_commit,
                    branch_tip,
                ],
                check=False,
                text=True,
                capture_output=True,
            )
            if ancestry.returncode != 0:
                detail = ancestry.stderr.strip()
                suffix = f": {detail}" if detail else ""
                raise ManifestError(
                    "candidate is not an ancestor of the current branch tip"
                    f"{suffix}"
                )
            _run_git(
                arguments.repo_root,
                "-c",
                "core.hooksPath=/dev/null",
                "worktree",
                "add",
                "--detach",
                str(worktree),
                branch_tip,
            )
            worktree_added = True
            branch_source_commit = _git_source_commit(worktree)
            branch_manifest = create_manifest(
                worktree,
                branch_source_commit,
                repository,
                builder_runner,
            )
            branch_manifest_digest = _sha256_bytes(
                _canonical_bytes(branch_manifest)
            )
            verified_tip = _fetch_branch_tip(
                arguments.repo_root, repository, branch_name, verified_ref
            )
            if verified_tip != branch_tip:
                raise ManifestError(
                    "branch tip changed during promotion freshness measurement"
                )
        finally:
            if worktree_added:
                subprocess.run(
                    [
                        "git",
                        "-C",
                        str(arguments.repo_root),
                        "worktree",
                        "remove",
                        "--force",
                        str(worktree),
                    ],
                    check=False,
                    text=True,
                    capture_output=True,
                )
            for local_ref in (observed_ref, verified_ref):
                subprocess.run(
                    [
                        "git",
                        "-C",
                        str(arguments.repo_root),
                        "update-ref",
                        "-d",
                        local_ref,
                    ],
                    check=False,
                    text=True,
                    capture_output=True,
                )

    if branch_source_commit == candidate_source_commit:
        if branch_manifest_digest != candidate_manifest_digest:
            raise ManifestError(
                "current branch manifest differs for the candidate source commit"
            )
        status = "current"
    else:
        status = "superseded"
    evidence = {
        "branch": branch_name,
        "branch_manifest_digest": branch_manifest_digest,
        "branch_source_commit": branch_source_commit,
        "branch_tip_commit": branch_tip,
        "candidate_commit": candidate_commit,
        "candidate_manifest_digest": candidate_manifest_digest,
        "candidate_source_commit": candidate_source_commit,
        "candidate_builder_image_version": candidate_builder_image_version,
        "repository": repository,
        "schema": "photospider-ci-image-promotion-freshness-v1",
        "status": status,
    }
    _write_output(arguments.output, _canonical_bytes(evidence))
    print(status)


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
    try:
        builder_runner = load_resolved_identity(
            arguments.builder_runner_identity, arguments.repo_root, "Linux"
        )
    except RunnerError as error:
        raise ManifestError(f"cannot consume retained builder identity: {error}") from error
    manifest = create_manifest(
        arguments.repo_root,
        arguments.source_commit,
        arguments.repository,
        builder_runner,
    )
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
    try:
        builder_runner = load_resolved_identity(
            arguments.builder_runner_identity, arguments.repo_root, "Linux"
        )
    except RunnerError as error:
        raise ManifestError(f"cannot consume retained builder identity: {error}") from error
    expected = create_manifest(
        arguments.repo_root,
        arguments.source_commit,
        arguments.repository,
        builder_runner,
    )
    if actual != expected:
        raise ManifestError("manifest does not match current protected inputs and expected identity")
    if arguments.expected_digest and actual_digest != arguments.expected_digest:
        raise ManifestError(
            f"manifest digest {actual_digest} does not match expected {arguments.expected_digest}"
        )
    print(actual_digest)


def _published_image_lock(root: Path) -> dict[str, str]:
    """Return the exact protected OCI discovery and identity-label contract."""
    lock = _load_json(root / "ci/locks/ci-image-lock.json")
    published = lock.get("published_image") if isinstance(lock, dict) else None
    expected_fields = {
        "builder_image_version_label",
        "input_manifest_label",
        "locator",
        "source_commit_label",
        "source_repository",
        "source_workflow",
    }
    if (
        not isinstance(published, dict)
        or set(published) != expected_fields
        or not all(isinstance(value, str) and value for value in published.values())
    ):
        raise ManifestError("published image lock is malformed")
    return published


def _command_builder_label(arguments: argparse.Namespace) -> None:
    """Print the exact approved builder image version for one retained record."""
    try:
        identity = load_resolved_identity(
            arguments.builder_runner_identity, arguments.repo_root, "Linux"
        )
    except RunnerError as error:
        raise ManifestError(f"cannot consume retained builder identity: {error}") from error
    print(identity["image_version"])


def _command_builder_from_labels(arguments: argparse.Namespace) -> None:
    """Resolve one untrusted OCI builder label into a retained approved record."""
    labels = _load_json(arguments.labels_json)
    if not isinstance(labels, dict) or not all(
        isinstance(key, str) and isinstance(value, str) for key, value in labels.items()
    ):
        raise ManifestError("inspected OCI labels must be a string object")
    published = _published_image_lock(arguments.repo_root)
    version = labels.get(published["builder_image_version_label"])
    if not isinstance(version, str):
        raise ManifestError("OCI builder image-version label is absent")
    try:
        identity = resolve_approved_identity(arguments.repo_root, "Linux", version)
    except RunnerError as error:
        raise ManifestError(f"OCI builder image version is not approved: {error}") from error
    _write_output(arguments.output, canonical_identity_bytes(identity))
    print(version)


def _command_verify_labels(arguments: argparse.Namespace) -> None:
    """Verify inspected OCI labels against exact manifest/source/builder state."""
    manifest, manifest_digest = _verify_manifest_file(arguments.manifest)
    labels = _load_json(arguments.labels_json)
    if not isinstance(labels, dict) or not all(
        isinstance(key, str) and isinstance(value, str) for key, value in labels.items()
    ):
        raise ManifestError("inspected OCI labels must be a string object")
    published = _published_image_lock(arguments.repo_root)
    builder_runner = manifest.get("builder_runner")
    try:
        builder_runner = validate_resolved_identity(
            arguments.repo_root, "Linux", builder_runner
        )
    except RunnerError as error:
        raise ManifestError(f"manifest builder identity is invalid: {error}") from error
    expected = {
        published["builder_image_version_label"]: builder_runner["image_version"],
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
    create.add_argument("--builder-runner-identity", type=Path, required=True)
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
    detect = subparsers.add_parser(
        "detect-changed",
        help="classify a Git comparison from strict base/head image-input locks",
    )
    detect.add_argument("--base", required=True)
    comparison = detect.add_mutually_exclusive_group(required=True)
    comparison.add_argument("--head")
    comparison.add_argument("--worktree", action="store_true")
    detect.add_argument("--changed-files-output", type=Path, required=True)
    detect.set_defaults(handler=_command_detect_changed)
    freshness = subparsers.add_parser(
        "promotion-freshness",
        help="compare a candidate with the freshly fetched live branch identity",
    )
    freshness.add_argument("--candidate-commit", required=True)
    freshness.add_argument("--candidate-source-commit", required=True)
    freshness.add_argument("--candidate-manifest-digest", required=True)
    freshness.add_argument("--candidate-builder-image-version", required=True)
    freshness.add_argument("--repository", required=True)
    freshness.add_argument("--branch", required=True)
    freshness.add_argument("--scratch-root", type=Path, required=True)
    freshness.add_argument("--output", type=Path, required=True)
    freshness.set_defaults(handler=_command_promotion_freshness)
    verify = subparsers.add_parser("verify", help="verify a canonical manifest")
    verify.add_argument("--source-commit", required=True)
    verify.add_argument("--repository", required=True)
    verify.add_argument("--builder-runner-identity", type=Path, required=True)
    verify.add_argument("--manifest", type=Path, required=True)
    verify.add_argument("--expected-digest")
    verify.set_defaults(handler=_command_verify)
    builder_label = subparsers.add_parser(
        "builder-label", help="print the image version from a retained builder identity"
    )
    builder_label.add_argument("--builder-runner-identity", type=Path, required=True)
    builder_label.set_defaults(handler=_command_builder_label)
    builder_from_labels = subparsers.add_parser(
        "builder-from-labels", help="resolve OCI labels to a retained builder identity"
    )
    builder_from_labels.add_argument("--labels-json", type=Path, required=True)
    builder_from_labels.add_argument("--output", type=Path, required=True)
    builder_from_labels.set_defaults(handler=_command_builder_from_labels)
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
