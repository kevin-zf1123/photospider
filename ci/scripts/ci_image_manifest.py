#!/usr/bin/env python3
"""Create and verify the canonical Photospider CI-image input manifest.

The manifest hashes every repository-protected build input named by the image
lock and binds those bytes to the exact image-source commit, repository, base
image, full-SHA builder action, and version/full-SHA identities of the installer
and suite-gate helpers. The protected resolver separately proves that this
source is the sole ancestry-maximal canonical-input-changing ancestor in the
complete candidate DAG and that canonical inputs do not drift from source to
candidate. GitHub attestation source/signer identities remain independent
candidate/workflow commits; published discovery retains and rejects a saturated
finite raw bundle window before parsing verified evidence. The output is
canonical JSON with a final newline. Promotion freshness
uses the same protected path lock and manifest builder against a freshly fetched
branch-tip worktree, never a copied path list or candidate-provided identity.
No candidate-owned script or generated value is trusted as an input list.
"""

from __future__ import annotations

import argparse
import hashlib
import json
import os
import re
import stat
import subprocess
import sys
import tempfile
from pathlib import Path, PurePosixPath
from typing import Any, Callable, NamedTuple

_SCRIPT_DIRECTORY = Path(__file__).resolve().parent
if str(_SCRIPT_DIRECTORY) not in sys.path:
    sys.path.insert(0, str(_SCRIPT_DIRECTORY))
from ci_runner_verify import (
    RunnerError,
    canonical_identity_bytes,
    load_resolved_identity,
    load_resolved_identity_record,
    load_runner_lock_bytes,
    resolve_approved_identity,
    validate_resolved_identity,
    validate_resolved_identity_from_lock,
)


class ManifestError(ValueError):
    """Report malformed, unsafe, or mismatched image-manifest state."""


class _InputMeasurement(NamedTuple):
    """Retain one canonical input's exact bytes, digest, and descriptor size.

    Attributes:
        content: Byte-for-byte result agreed by both descriptor reads.
        sha256: Lowercase digest of ``content``.
        size: Stable ``fstat`` size cross-checked against each read count.

    Note:
        Instances exist only during one manifest construction and are never a
        second serialized authority beyond the resulting canonical manifest.
    """

    content: bytes
    sha256: str
    size: int


_MeasurementHook = Callable[[str, Path, int], None]
"""Only-test callback invoked at deterministic retained-measurement phases."""


_IMAGE_LOCK_RELATIVE_PATH = "ci/locks/ci-image-lock.json"
_ATTESTATION_FETCH_LIMIT = 30
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


def _measure_regular_input(
    path: Path,
    *,
    _test_hook: _MeasurementHook | None = None,
) -> _InputMeasurement:
    """Measure one retained canonical input exactly once and fail on drift.

    Args:
        path: Exact canonical repository input pathname.
        _test_hook: Optional deterministic test callback receiving phase, path,
            and retained descriptor. Production callers never provide it.

    Returns:
        Exact agreeing bytes, lowercase SHA-256, and descriptor-derived size.

    Raises:
        ManifestError: Linux/Darwin safe-open flags are unavailable; the path
            is missing, linked, special, unreadable, replaced, or modified; or
            two complete reads of the retained descriptor disagree.

    Note:
        ``O_NONBLOCK`` bounds FIFO/device opening before ``fstat`` rejects the
        type. Pathname and descriptor metadata are compared after open and at
        both read boundaries. The hook exists only to make replacement and
        in-place mutation races deterministic in direct ``create_manifest``
        tests; it is not reachable from the CLI.
    """
    required_flags = ("O_NOFOLLOW", "O_NONBLOCK", "O_CLOEXEC")
    missing_flags = [
        name for name in required_flags if not isinstance(getattr(os, name, None), int)
    ]
    if missing_flags:
        raise ManifestError(
            f"{path}: required safe-open flags are unavailable: "
            + ", ".join(missing_flags)
        )

    def stable_identity(value: os.stat_result) -> tuple[int, ...]:
        """Return metadata whose drift invalidates the retained measurement."""
        return (
            value.st_dev,
            value.st_ino,
            value.st_mode,
            value.st_nlink,
            value.st_size,
            value.st_mtime_ns,
            value.st_ctime_ns,
        )

    def require_stable(
        initial: os.stat_result, phase: str
    ) -> os.stat_result:
        """Rebind the current pathname and descriptor to the initial identity."""
        current = os.fstat(descriptor)
        try:
            pathname = path.lstat()
        except OSError as error:
            raise ManifestError(
                f"image input pathname is unavailable {phase}: {path}: {error}"
            ) from error
        if not stat.S_ISREG(current.st_mode) or not stat.S_ISREG(pathname.st_mode):
            raise ManifestError(f"image input is not a regular file {phase}: {path}")
        if (
            stable_identity(current) != stable_identity(initial)
            or stable_identity(pathname) != stable_identity(initial)
        ):
            raise ManifestError(f"image input identity changed {phase}: {path}")
        return current

    flags = (
        os.O_RDONLY
        | getattr(os, "O_NOFOLLOW")
        | getattr(os, "O_NONBLOCK")
        | getattr(os, "O_CLOEXEC")
    )
    descriptor = -1
    try:
        descriptor = os.open(path, flags)
        initial = os.fstat(descriptor)
        if not stat.S_ISREG(initial.st_mode):
            raise ManifestError(f"image input is not a regular file: {path}")
        require_stable(initial, "after open")
        if _test_hook is not None:
            _test_hook("after_open", path, descriptor)
        require_stable(initial, "after open hook")

        reads: list[bytes] = []
        for pass_number in (1, 2):
            os.lseek(descriptor, 0, os.SEEK_SET)
            chunks: list[bytes] = []
            byte_count = 0
            while True:
                chunk = os.read(descriptor, 1024 * 1024)
                if not chunk:
                    break
                chunks.append(chunk)
                byte_count += len(chunk)
            if byte_count != initial.st_size:
                raise ManifestError(
                    f"image input byte count changed during read: {path}"
                )
            require_stable(initial, f"after read {pass_number}")
            content = b"".join(chunks)
            reads.append(content)
            if _test_hook is not None:
                phase = (
                    "after_first_read" if pass_number == 1 else "after_second_read"
                )
                _test_hook(phase, path, descriptor)
            require_stable(initial, f"after read {pass_number} hook")
        if reads[0] != reads[1]:
            raise ManifestError(f"image input bytes changed between reads: {path}")
        return _InputMeasurement(
            content=reads[0],
            sha256=_sha256_bytes(reads[0]),
            size=initial.st_size,
        )
    except ManifestError:
        raise
    except OSError as error:
        raise ManifestError(f"cannot measure canonical image input {path}: {error}") from error
    finally:
        if descriptor >= 0:
            os.close(descriptor)


def _protected_helpers(
    lock: dict[str, Any],
    input_paths: list[str],
    measurements: dict[str, _InputMeasurement],
) -> dict[str, dict[str, str]]:
    """Validate and return the exact versioned protected-helper identities.

    Args:
        lock: Strict CI-image lock object.
        input_paths: Canonical image-input path inventory.
        measurements: One retained authority record per canonical input.

    Returns:
        The two canonical helper identity records for manifest inclusion.

    Raises:
        ManifestError: A helper record/path/version/hash is malformed, absent
            from image inputs, aliased, or differs from the protected bytes.

    Note:
        The explicit helper identity supplements, rather than replaces, the
        ordinary per-input hash. This binds executable role/version and bytes
        without reopening a helper pathname after its manifest measurement.
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
        measurement = measurements.get(path)
        if measurement is None or measurement.sha256 != sha256:
            raise ManifestError(f"protected helper {name!r} bytes differ from the lock")
        result[name] = {"path": path, "sha256": sha256, "version": version}
    return result


def _read_builder_commit(
    lock_bytes: bytes, action: str, release: str, context: str
) -> str:
    """Resolve a builder from one already-retained action-lock measurement.

    Args:
        lock_bytes: Exact retained canonical input bytes.
        action: Protected action identifier required by the image lock.
        release: Human-readable release identity paired with that action.
        context: Stable input path used for diagnostics.

    Returns:
        The unique lowercase full commit SHA from the matching action row.

    Raises:
        ManifestError: UTF-8, row shape, uniqueness, or commit identity fails.

    Note:
        This function never reopens ``actions.lock``; manifest digest/size and
        builder semantics share the caller's single retained measurement.
    """
    try:
        lines = lock_bytes.decode("utf-8").splitlines()
    except UnicodeError as error:
        raise ManifestError(f"cannot decode retained action lock {context}") from error
    matches: list[str] = []
    for raw_line in lines:
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
    *,
    _test_hook: _MeasurementHook | None = None,
) -> dict[str, Any]:
    """Create the manifest for exact source bytes and measured builder runtime.

    Args:
        root: Exact repository state supplying canonical image inputs.
        source_commit: Last commit that changed one canonical image input.
        repository: Locked GitHub ``owner/name`` source identity.
        builder_runner: Retained Linux runtime record produced by
            ``ci_runner_verify.py`` in the actual build job.
        _test_hook: Optional deterministic measurement hook for direct tests.

    Returns:
        Canonical manifest object whose input list binds the complete reviewed
        rollout lock while ``builder_runner`` binds the one selected member.

    Raises:
        ManifestError: Source, repository, safe retained input measurement,
            builder action, or runner identity is malformed or no longer
            approved by this exact measured source.

    Note:
        The self-including lock is decoded from its one retained measurement.
        Every remaining canonical path is opened exactly once, and all helper,
        action, runner-lock, digest, and size semantics reuse that path-to-record
        authority without a second pathname read.
    """
    if not re.fullmatch(r"[0-9a-f]{40}", source_commit):
        raise ManifestError("source commit must be a lowercase full SHA")
    if not re.fullmatch(r"[A-Za-z0-9_.-]+/[A-Za-z0-9_.-]+", repository):
        raise ManifestError("repository must be an owner/name identity")
    lock_path = root / "ci/locks/ci-image-lock.json"
    lock_measurement = _measure_regular_input(lock_path, _test_hook=_test_hook)
    lock = _load_json_bytes(lock_measurement.content, str(lock_path))
    paths = _image_input_paths(lock, str(lock_path))
    measurements = {
        _IMAGE_LOCK_RELATIVE_PATH: lock_measurement,
    }
    for relative in paths:
        if relative == _IMAGE_LOCK_RELATIVE_PATH:
            continue
        measurements[relative] = _measure_regular_input(
            root / relative, _test_hook=_test_hook
        )
    if set(measurements) != set(paths):
        raise ManifestError("canonical image input measurement set is incomplete")
    protected_helpers = _protected_helpers(lock, paths, measurements)
    inputs = [
        {
            "path": relative,
            "sha256": measurements[relative].sha256,
            "size": measurements[relative].size,
        }
        for relative in paths
    ]
    builder = lock.get("builder")
    if (
        not isinstance(builder, dict)
        or set(builder) != {"action", "release"}
        or not all(isinstance(value, str) and value for value in builder.values())
    ):
        raise ManifestError("builder lock is malformed")
    actions_path = "ci/locks/actions.lock"
    actions_measurement = measurements.get(actions_path)
    if actions_measurement is None:
        raise ManifestError("canonical action lock is absent from image inputs")
    builder_commit = _read_builder_commit(
        actions_measurement.content,
        builder["action"],
        builder["release"],
        actions_path,
    )
    runner_lock_path = "ci/locks/linux-runner-lock.json"
    runner_lock_measurement = measurements.get(runner_lock_path)
    if runner_lock_measurement is None:
        raise ManifestError("canonical Linux runner lock is absent from image inputs")
    try:
        runner_lock = load_runner_lock_bytes(
            runner_lock_measurement.content, "Linux", runner_lock_path
        )
        runner = validate_resolved_identity_from_lock(
            runner_lock, "Linux", builder_runner
        )
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


def _full_git_dag(
    root: Path, candidate_commit: str
) -> tuple[list[str], dict[str, tuple[str, ...]]]:
    """Load and validate the candidate's complete reachable commit DAG.

    Args:
        root: Repository containing the exact candidate and all of its history.
        candidate_commit: Lowercase full candidate SHA already resolved by the
            caller.

    Returns:
        A child-before-parent topological order and exact parent tuple for every
        reachable commit.

    Raises:
        ManifestError: The repository is shallow; Git output is empty,
            non-ASCII, malformed, duplicated, incomplete, self-referential, or
            not child-before-parent.

    Note:
        The traversal has no path limiter and explicitly requests full history,
        so Git's path history simplification cannot discard a side parent. The
        validated parent graph, rather than output or parent order, supplies the
        later ancestry partial order.
    """
    try:
        shallow = _git_bytes(
            root, "rev-parse", "--is-shallow-repository"
        ).decode("ascii").strip()
        raw = _git_bytes(
            root,
            "rev-list",
            "--full-history",
            "--topo-order",
            "--parents",
            candidate_commit,
        ).decode("ascii")
    except UnicodeError as error:
        raise ManifestError("image source DAG is not ASCII") from error
    if shallow != "false":
        raise ManifestError("image source DAG requires a complete non-shallow history")
    order: list[str] = []
    parents: dict[str, tuple[str, ...]] = {}
    for line_number, raw_line in enumerate(raw.splitlines(), start=1):
        fields = raw_line.split(" ")
        if not fields or any(
            re.fullmatch(r"[0-9a-f]{40}", field) is None for field in fields
        ):
            raise ManifestError(
                f"image source DAG line {line_number} is malformed"
            )
        commit, *commit_parents = fields
        if commit in parents:
            raise ManifestError(f"image source DAG repeats commit {commit}")
        if len(commit_parents) != len(set(commit_parents)) or commit in commit_parents:
            raise ManifestError(f"image source DAG parent set is malformed for {commit}")
        order.append(commit)
        parents[commit] = tuple(commit_parents)
    if not order or candidate_commit not in parents:
        raise ManifestError("image source DAG is empty or omits the candidate")
    positions = {commit: index for index, commit in enumerate(order)}
    for child, commit_parents in parents.items():
        for parent in commit_parents:
            if parent not in parents:
                raise ManifestError(
                    f"image source DAG omits parent {parent} of {child}"
                )
            if positions[child] >= positions[parent]:
                raise ManifestError(
                    f"image source DAG is not child-before-parent at {child}"
                )
    return order, parents


def _image_changing_commits(
    root: Path, order: list[str], paths: list[str]
) -> set[str]:
    """Return commits whose tree changes a canonical input against a parent.

    Args:
        root: Repository containing the validated complete DAG.
        order: Unique child-before-parent commit order.
        paths: Candidate-tree strict canonical input authority.

    Returns:
        The set of commits with at least one canonical path change against any
        parent, including a root commit's change against the empty tree.

    Raises:
        ManifestError: Batched parent-aware Git diff output is malformed,
            names an unknown commit/path, or cannot execute.

    Note:
        ``diff-tree --stdin --root -m --no-renames`` evaluates every commit in
        one Git process and emits a merge once per changing parent. A merge that
        introduces no canonical byte relative to either parent is therefore
        not invented as a source, while a real merge resolution remains a
        changing descendant. Rename heuristics never alter the path authority.
    """
    commit_tokens = {commit.encode("ascii"): commit for commit in order}
    path_tokens = {os.fsencode(path) for path in paths}
    if set(commit_tokens).intersection(path_tokens):
        raise ManifestError("canonical input path is ambiguous with a commit identity")
    diff = _git_bytes(
        root,
        "diff-tree",
        "--stdin",
        "--root",
        "-m",
        "--no-renames",
        "--name-only",
        "--abbrev=40",
        "-z",
        "-r",
        "--",
        *paths,
        input_bytes=("".join(f"{commit}\n" for commit in order)).encode("ascii"),
    )
    if diff and not diff.endswith(b"\0"):
        raise ManifestError("image source change inventory is not NUL terminated")
    current_commit: str | None = None
    changed: set[str] = set()
    for token in diff[:-1].split(b"\0") if diff else ():
        if token in commit_tokens:
            current_commit = commit_tokens[token]
            continue
        if token not in path_tokens or current_commit is None:
            raise ManifestError("image source change inventory is malformed")
        changed.add(current_commit)
    return changed


def _ancestry_maximal_changes(
    order: list[str],
    parents: dict[str, tuple[str, ...]],
    changed: set[str],
) -> set[str]:
    """Compute the ancestry-maximal changing commits from one validated DAG.

    Args:
        order: Child-before-parent topological commit order.
        parents: Exact complete-DAG parent mapping.
        changed: Commits that changed at least one canonical input.

    Returns:
        Every changing commit that has no changing descendant.

    Raises:
        ManifestError: A traversal identity is missing from the parent mapping.

    Note:
        Descendant-change reachability propagates from each child to every
        parent. This computes the partial-order maximal set without relying on
        rev-list output order between siblings or merge parent order.
    """
    has_changing_descendant = {commit: False for commit in order}
    maximal: set[str] = set()
    for commit in order:
        if commit not in parents:
            raise ManifestError(f"image source partial order omits {commit}")
        if commit in changed and not has_changing_descendant[commit]:
            maximal.add(commit)
        propagate = commit in changed or has_changing_descendant[commit]
        if propagate:
            for parent in parents[commit]:
                if parent not in has_changing_descendant:
                    raise ManifestError(
                        f"image source partial order omits parent {parent}"
                    )
                has_changing_descendant[parent] = True
    return maximal


def _git_source_commit(root: Path, candidate_commit: str = "HEAD") -> str:
    """Return the unique newest canonical-input-changing candidate ancestor.

    Args:
        root: Repository containing the immutable candidate and its history.
        candidate_commit: Exact commit/ref whose own strict image-input lock
            supplies the sole path authority for the history query.

    Returns:
        The sole ancestry-maximal lowercase full SHA that changed a canonical
        image input at or before the candidate.

    Raises:
        ManifestError: The candidate, its lock/path set, complete Git DAG,
            parent-aware change inventory, partial order, or unique maximal
            result is missing, malformed, shallow, or ambiguous.

    Note:
        The path list comes from the candidate tree, not from an untrusted
        caller argument. Full-DAG traversal and parent-aware tree comparison
        deliberately avoid default Git history simplification. Two
        incomparable changing ancestors fail even when their final bytes agree;
        neither output nor merge-parent order may select one arbitrarily.
    """
    try:
        resolved_candidate = _git_bytes(
            root, "rev-parse", "--verify", f"{candidate_commit}^{{commit}}"
        ).decode("ascii").strip()
    except UnicodeError as error:
        raise ManifestError("image candidate commit is not ASCII") from error
    if re.fullmatch(r"[0-9a-f]{40}", resolved_candidate) is None:
        raise ManifestError("image candidate did not resolve to one full commit")
    paths = _tree_image_input_paths(root, resolved_candidate)
    if paths is None:
        raise ManifestError("image candidate CI image lock is absent")
    order, parents = _full_git_dag(root, resolved_candidate)
    changed = _image_changing_commits(root, order, paths)
    maximal = _ancestry_maximal_changes(order, parents, changed)
    if len(maximal) != 1:
        rendered = ", ".join(sorted(maximal)) if maximal else "none"
        raise ManifestError(
            "image source requires exactly one ancestry-maximal canonical-input "
            f"change, observed: {rendered}"
        )
    return next(iter(maximal))


def _validate_candidate_source_binding(
    root: Path, candidate_commit: str, source_commit: str, *, require_head: bool
) -> list[str]:
    """Prove the four-identity model's candidate-to-image-source edge.

    Args:
        root: Repository containing both exact commits and canonical input lock.
        candidate_commit: Tested checkout identity.
        source_commit: Claimed newest canonical-input-changing ancestor.
        require_head: Whether the repository's current ``HEAD`` must equal the
            tested candidate, as required by producer and promotion callers.

    Returns:
        The candidate tree's validated canonical input path list.

    Raises:
        ManifestError: Either identity is malformed/missing; ``HEAD`` differs
            when required; source is not an ancestor or the exact newest
            canonical-input-changing ancestor; source/candidate lock authority
            differs; or any canonical input drifts after the source commit.

    Note:
        A zero diff is measured only over the strict self-including lock path
        authority. Parser, test, and documentation changes outside that set do
        not become image inputs merely to force commit equality.
    """
    for label, value in (
        ("candidate", candidate_commit),
        ("image source", source_commit),
    ):
        if re.fullmatch(r"[0-9a-f]{40}", value) is None:
            raise ManifestError(f"{label} commit must be a lowercase full SHA")
        try:
            resolved = _git_bytes(
                root, "rev-parse", "--verify", f"{value}^{{commit}}"
            ).decode("ascii").strip()
        except UnicodeError as error:
            raise ManifestError(f"{label} commit is not ASCII") from error
        if resolved != value:
            raise ManifestError(f"{label} commit does not resolve exactly")
    if require_head:
        try:
            head = _git_bytes(
                root, "rev-parse", "--verify", "HEAD^{commit}"
            ).decode("ascii").strip()
        except UnicodeError as error:
            raise ManifestError("repository HEAD is not ASCII") from error
        if head != candidate_commit:
            raise ManifestError("repository HEAD differs from the candidate commit")

    ancestry = subprocess.run(
        [
            "git",
            "-C",
            str(root),
            "merge-base",
            "--is-ancestor",
            source_commit,
            candidate_commit,
        ],
        check=False,
        capture_output=True,
    )
    if ancestry.returncode == 1:
        raise ManifestError("image source commit is not an ancestor of candidate")
    if ancestry.returncode != 0:
        detail = ancestry.stderr.decode(
            "utf-8", errors="backslashreplace"
        ).strip()
        raise ManifestError(
            "cannot verify image source ancestry"
            + (f": {detail}" if detail else "")
        )

    candidate_paths = _tree_image_input_paths(root, candidate_commit)
    source_paths = _tree_image_input_paths(root, source_commit)
    if candidate_paths is None or source_paths is None:
        raise ManifestError("candidate/source canonical input lock is absent")
    if candidate_paths != source_paths:
        raise ManifestError("candidate/source canonical input authority differs")
    resolved_source = _git_source_commit(root, candidate_commit)
    if resolved_source != source_commit:
        raise ManifestError(
            f"claimed image source {source_commit} is not newest canonical-input "
            f"ancestor {resolved_source}"
        )
    drift = _git_bytes(
        root,
        "diff",
        "--no-renames",
        "--name-only",
        "-z",
        f"{source_commit}..{candidate_commit}",
        "--",
        *candidate_paths,
    )
    if drift:
        if not drift.endswith(b"\0"):
            raise ManifestError("canonical input drift inventory is malformed")
        paths = [
            json.dumps(os.fsdecode(value), ensure_ascii=True)
            for value in drift[:-1].split(b"\0")
            if value
        ]
        raise ManifestError(
            "canonical image inputs drift after source commit: " + ", ".join(paths)
        )
    return candidate_paths


def _git_bytes(
    root: Path, *arguments: str, input_bytes: bytes | None = None
) -> bytes:
    """Run Git without a shell and return its exact stdout bytes.

    Args:
        root: Explicit repository whose objects and refs are authoritative.
        *arguments: Git arguments passed as separate non-shell argv entries.
        input_bytes: Optional exact stdin bytes for a batched Git operation.

    Returns:
        Exact stdout bytes from one successful Git process.

    Raises:
        ManifestError: Git cannot execute or returns nonzero.

    Note:
        Byte output preserves NUL path records and filenames not decodable as
        ordinary text; diagnostics alone use replacement-safe UTF-8 rendering.
        The optional input is passed directly without a shell or text decode.
    """
    try:
        completed = subprocess.run(
            ["git", "-C", str(root), *arguments],
            check=False,
            capture_output=True,
            input=input_bytes,
        )
    except OSError as error:
        raise ManifestError(f"cannot execute Git {' '.join(arguments)}: {error}") from error
    if completed.returncode != 0:
        detail = completed.stderr.decode("utf-8", errors="backslashreplace").strip()
        raise ManifestError(
            f"Git {' '.join(arguments)} failed: {detail or 'no diagnostic'}"
        )
    return completed.stdout


def _tree_image_input_paths(
    root: Path, revision: str, *, allow_absent: bool = False
) -> list[str] | None:
    """Load the strict canonical input lock from one immutable Git tree.

    Args:
        root: Repository containing the requested tree object.
        revision: Verified full commit identity used by ``git show``.
        allow_absent: Whether an exact missing tree path is returned as
            ``None`` for the narrowly bounded first-introduction comparison.

    Returns:
        The validated, self-including canonical input paths, or ``None`` only
        when ``allow_absent`` is true and the exact tree has no lock entry.

    Raises:
        ManifestError: The tree, lock entry type, blob, schema, or paths are
            invalid. A missing lock also fails unless ``allow_absent`` is true.

    Note:
        ``git ls-tree`` distinguishes one genuinely absent path from a corrupt
        tree, unsupported entry, or failed object lookup. The later ``show``
        therefore never treats a generic Git error as first introduction.
    """
    listing = _git_bytes(
        root,
        "ls-tree",
        "-z",
        "--full-tree",
        revision,
        "--",
        _IMAGE_LOCK_RELATIVE_PATH,
    )
    if not listing:
        if allow_absent:
            return None
        raise ManifestError(
            f"{revision}:{_IMAGE_LOCK_RELATIVE_PATH}: CI image lock is absent"
        )
    if not listing.endswith(b"\0"):
        raise ManifestError("CI image lock tree entry is not NUL terminated")
    records = listing[:-1].split(b"\0")
    if len(records) != 1 or b"\t" not in records[0]:
        raise ManifestError("CI image lock tree entry is missing or ambiguous")
    metadata, path_bytes = records[0].split(b"\t", 1)
    fields = metadata.split(b" ")
    if (
        path_bytes != _IMAGE_LOCK_RELATIVE_PATH.encode("utf-8")
        or len(fields) != 3
        or fields[0] not in {b"100644", b"100755"}
        or fields[1] != b"blob"
        or re.fullmatch(rb"[0-9a-f]{40,64}", fields[2]) is None
    ):
        raise ManifestError("CI image lock tree entry is not one regular blob")
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
        The base list is empty only for a proved first introduction whose head
        lock is strict, self-including, and added by this exact comparison.

    Raises:
        ManifestError: A ref, head lock, non-bootstrap base lock, diff, or path
            record is unavailable or malformed. No false route is emitted on
            failure.
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
        base_paths_or_absent = _tree_image_input_paths(
            root, authoritative_base, allow_absent=True
        )
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
        base_paths_or_absent = _tree_image_input_paths(
            root, authoritative_base, allow_absent=True
        )
        head_paths = _tree_image_input_paths(root, head_commit)
        if head_paths is None:
            raise ManifestError("comparison head CI image lock is absent")
        raw_paths = _git_bytes(
            root,
            "diff",
            "--no-renames",
            "--name-only",
            "-z",
            f"{authoritative_base}..{head_commit}",
        )
    if not raw_paths:
        if base_paths_or_absent is None:
            raise ManifestError(
                "first CI image lock introduction lacks an observable path change"
            )
        return [], base_paths_or_absent, head_paths
    if not raw_paths.endswith(b"\0"):
        raise ManifestError("Git changed-path inventory is not NUL terminated")
    changed = raw_paths[:-1].split(b"\0")
    if any(not path for path in changed):
        raise ManifestError("Git changed-path inventory contains an empty record")
    if base_paths_or_absent is None:
        lock_path_bytes = _IMAGE_LOCK_RELATIVE_PATH.encode("utf-8")
        if lock_path_bytes not in changed:
            raise ManifestError(
                "first CI image lock introduction is absent from the exact diff"
            )
        base_paths: list[str] = []
    else:
        base_paths = base_paths_or_absent
    return changed, base_paths, head_paths


def _command_detect_changed(arguments: argparse.Namespace) -> None:
    """Classify one comparison from strict revision lock path authority.

    Args:
        arguments: Parsed repository, base, head/worktree, and diagnostic output
            options from the protected command line.

    Returns:
        None after writing the JSON-quoted diagnostic path log and printing one
        exact ``true`` or ``false`` route value.

    Raises:
        ManifestError: Any revision, required lock, path inventory, or Git
            operation is unavailable or malformed. The sole missing-base
            exception is a strict self-including head lock added by the exact
            comparison; that boundary necessarily prints ``true`` rather than
            allowing a false route. No route value is printed on failure.
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
    workflow_commit = arguments.workflow_commit
    branch_name = arguments.branch
    repository = arguments.repository
    if re.fullmatch(r"[0-9a-f]{40}", candidate_commit) is None:
        raise ManifestError("promotion candidate commit must be a lowercase full SHA")
    if re.fullmatch(r"[0-9a-f]{40}", workflow_commit) is None:
        raise ManifestError("promotion workflow commit must be a lowercase full SHA")
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
    _validate_candidate_source_binding(
        arguments.repo_root,
        candidate_commit,
        candidate_source_commit,
        require_head=True,
    )
    candidate_manifest = create_manifest(
        arguments.repo_root,
        candidate_source_commit,
        repository,
        builder_runner,
    )
    measured_candidate_manifest_digest = _sha256_bytes(
        _canonical_bytes(candidate_manifest)
    )
    if measured_candidate_manifest_digest != candidate_manifest_digest:
        raise ManifestError(
            "candidate manifest differs from retained source/input identity"
        )

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
        "workflow_commit": workflow_commit,
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
    """Create a canonical manifest and optional digest from one retained runner.

    The runner record is decoded canonically here but is semantically rebound
    only inside ``create_manifest`` to that function's already-retained Linux
    lock measurement. This avoids reopening a canonical input pathname.
    """
    try:
        builder_runner = load_resolved_identity_record(
            arguments.builder_runner_identity
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


def _command_resolve_source_commit(arguments: argparse.Namespace) -> None:
    """Resolve and prove image source without collapsing commit identities.

    Args:
        arguments: Candidate/workflow identities and repository root parsed by
            the protected CLI.

    Returns:
        None after printing the candidate's exact image-source ancestor.

    Raises:
        ManifestError: The workflow identity is non-canonical or the candidate,
            source ancestry, last-change authority, checkout binding, or
            canonical-input zero-drift proof fails.

    Note:
        ``workflow_commit`` is carried and syntax-validated here but deliberately
        remains a distinct signer/control identity. The protected workflow
        mapping independently binds it to ``github.workflow_sha``.
    """
    if re.fullmatch(r"[0-9a-f]{40}", arguments.workflow_commit) is None:
        raise ManifestError("workflow commit must be a lowercase full SHA")
    source_commit = _git_source_commit(arguments.repo_root, arguments.candidate_commit)
    _validate_candidate_source_binding(
        arguments.repo_root,
        arguments.candidate_commit,
        source_commit,
        require_head=True,
    )
    print(source_commit)


def _command_verify_source_binding(arguments: argparse.Namespace) -> None:
    """Verify a supplied candidate/source pair using protected Git authority.

    Args:
        arguments: Candidate/source identities and repository root parsed by
            the protected CLI.

    Returns:
        None after printing the verified image-source commit.

    Raises:
        ManifestError: Candidate/source ancestry, last-change authority,
            canonical path identity, or zero-drift validation fails.
    """
    _validate_candidate_source_binding(
        arguments.repo_root,
        arguments.candidate_commit,
        arguments.source_commit,
        require_head=arguments.require_head,
    )
    print(arguments.source_commit)


def _command_verify(arguments: argparse.Namespace) -> None:
    """Recreate and compare a manifest against one retained repository state."""
    actual, actual_digest = _verify_manifest_file(arguments.manifest)
    try:
        builder_runner = load_resolved_identity_record(
            arguments.builder_runner_identity
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


def _validated_attestation_fetch_limit(value: Any, context: str) -> int:
    """Return the sole reviewed finite attestation discovery limit.

    Args:
        value: Decoded lock value or CLI integer.
        context: Stable source identity used in diagnostics.

    Returns:
        The exact protected fetch limit.

    Raises:
        ManifestError: The value is boolean, non-integer, or differs from the
            reviewed finite limit.

    Note:
        Limit rotation intentionally requires a code, lock, static-contract,
        documentation, and remote-evidence review. An implicit GitHub CLI
        default is never an authority.
    """
    if (
        isinstance(value, bool)
        or not isinstance(value, int)
        or value != _ATTESTATION_FETCH_LIMIT
    ):
        raise ManifestError(
            f"{context}: attestation fetch limit must be "
            f"{_ATTESTATION_FETCH_LIMIT}"
        )
    return value


def _published_image_lock(root: Path) -> dict[str, Any]:
    """Load the exact protected OCI discovery and attestation contract.

    Args:
        root: Repository containing the canonical CI-image lock.

    Returns:
        The strict published-image locator, labels, repository/workflow, and
        reviewed finite attestation fetch limit.

    Raises:
        ManifestError: The lock or published record has an unknown/missing
            field, malformed string identity, or non-reviewed fetch limit.

    Note:
        The limit is part of the self-including canonical image lock and cannot
        silently inherit or drift with GitHub CLI's default.
    """
    lock = _load_json(root / "ci/locks/ci-image-lock.json")
    published = lock.get("published_image") if isinstance(lock, dict) else None
    expected_fields = {
        "attestation_fetch_limit",
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
    ):
        raise ManifestError("published image lock is malformed")
    string_fields = expected_fields - {"attestation_fetch_limit"}
    if not all(
        isinstance(published[field], str) and published[field]
        for field in string_fields
    ):
        raise ManifestError("published image string identity is malformed")
    _validated_attestation_fetch_limit(
        published["attestation_fetch_limit"], "published image lock"
    )
    return published


def _command_snapshot_attestation_bundle(arguments: argparse.Namespace) -> None:
    """Retain one complete, unsaturated GitHub attestation download window.

    Args:
        arguments: Fresh download directory, exact subject digest, reviewed
            fetch limit, and process-private snapshot output.

    Returns:
        None after writing the exact retained JSONL bytes and printing their
        raw fetched-bundle count.

    Raises:
        ManifestError: The directory is linked/special, its exact digest-named
            bundle is absent or accompanied by another entry, the bundle is
            linked/special/malformed JSONL, the output is residual, or the raw
            fetched count reaches the finite limit and may be truncated.

    Note:
        ``gh attestation download`` documents one JSON object per line and
        ``--limit`` as the maximum number fetched. Counting the retained raw
        bundle therefore measures fetch saturation before offline verification;
        counting only ``gh attestation verify --format json`` output would be
        weaker because that array contains verified evidence, not fetch-count
        metadata. Saturation deliberately rejects even an exactly complete
        limit-sized set because the CLI exposes no continuation/total marker.
    """
    limit = _validated_attestation_fetch_limit(
        arguments.fetch_limit, "attestation bundle snapshot"
    )
    subject_digest = arguments.subject_digest
    if re.fullmatch(r"sha256:[0-9a-f]{64}", subject_digest) is None:
        raise ManifestError("attestation bundle subject digest is malformed")
    directory = arguments.download_directory
    try:
        directory_state = directory.lstat()
        with os.scandir(directory) as iterator:
            entries = list(iterator)
    except OSError as error:
        raise ManifestError(
            f"cannot inspect attestation download directory {directory}: {error}"
        ) from error
    if not stat.S_ISDIR(directory_state.st_mode) or directory.is_symlink():
        raise ManifestError("attestation download root must be one real directory")
    expected_name = f"{subject_digest}.jsonl"
    if len(entries) != 1 or entries[0].name != expected_name:
        raise ManifestError(
            "attestation download must contain only the exact digest bundle"
        )
    bundle_path = directory / expected_name
    measurement = _measure_regular_input(bundle_path)
    content = measurement.content
    if not content or not content.endswith(b"\n"):
        raise ManifestError("attestation bundle JSONL must be nonempty and newline terminated")
    records = content[:-1].split(b"\n")
    if not records or any(not record for record in records):
        raise ManifestError("attestation bundle JSONL contains an empty record")
    for index, record in enumerate(records):
        value = _load_json_bytes(record, f"{bundle_path}:record {index}")
        if not isinstance(value, dict):
            raise ManifestError(
                f"attestation bundle record {index} must be one JSON object"
            )
    if len(records) >= limit:
        raise ManifestError(
            "attestation bundle fetch reached the protected limit and may be truncated"
        )
    output = arguments.output
    if output == bundle_path or output.exists() or output.is_symlink():
        raise ManifestError("attestation bundle snapshot output must be fresh")
    _write_output(output, content)
    print(len(records))


def _command_attestation_identities(arguments: argparse.Namespace) -> None:
    """Extract one unique source/signer pair from verified GitHub evidence.

    Args:
        arguments: Verified JSON, protected fetch limit,
            image-source/current-candidate identities, and optional exact
            producer source/signer expectations.

    Returns:
        None after printing the certificate-bound source repository digest and
        build signer digest on two separate lines.

    Raises:
        ManifestError: Evidence is empty/malformed, lacks protected certificate
            fields, exceeds the finite fetch bound, contains non-canonical
            commit identities, disagrees with an explicit pair, or has no
            unique newest ancestry/zero-drift pair for published discovery.

    Note:
        Only ``verificationResult.signature.certificate`` is consumed. GitHub
        CLI documents that certificate data is populated from the Actions OIDC
        token; candidate-controllable predicate metadata is intentionally
        ignored. An explicit producer pair must be the sole verified pair. For
        a later published consumer, multiple same-digest rerun attestations are
        reduced to the unique newest source candidate that is an ancestor of
        the consumer and still binds the exact image-source inputs. Incomparable
        or same-candidate/different-signer evidence fails closed. Published
        verified evidence at the fetch limit also fails as possibly truncated;
        a known producer may reach the limit only because GitHub CLI already
        enforces both exact source and signer constraints, and every returned
        certificate is still required to equal that pair.
    """
    fetch_limit = _validated_attestation_fetch_limit(
        arguments.fetch_limit, "verified attestation evidence"
    )
    evidence = _load_json(arguments.attestation_json)
    if not isinstance(evidence, list) or not evidence:
        raise ManifestError("verified attestation evidence must be a non-empty array")
    if len(evidence) > fetch_limit:
        raise ManifestError("verified attestation evidence exceeds the fetch limit")
    expected_source = arguments.expected_source_commit
    expected_signer = arguments.expected_signer_commit
    if (expected_source is None) != (expected_signer is None):
        raise ManifestError("attestation source/signer expectations must be paired")
    if expected_source is None and len(evidence) >= fetch_limit:
        raise ManifestError(
            "published verified attestation evidence reached the fetch limit"
        )
    identities: set[tuple[str, str]] = set()
    for index, item in enumerate(evidence):
        if not isinstance(item, dict):
            raise ManifestError(f"verified attestation entry {index} is malformed")
        verification = item.get("verificationResult")
        signature = verification.get("signature") if isinstance(verification, dict) else None
        certificate = signature.get("certificate") if isinstance(signature, dict) else None
        if not isinstance(certificate, dict):
            raise ManifestError(
                f"verified attestation entry {index} lacks certificate identity"
            )
        source = certificate.get("sourceRepositoryDigest")
        signer = certificate.get("buildSignerDigest")
        if (
            not isinstance(source, str)
            or re.fullmatch(r"[0-9a-f]{40}", source) is None
            or not isinstance(signer, str)
            or re.fullmatch(r"[0-9a-f]{40}", signer) is None
        ):
            raise ManifestError(
                f"verified attestation entry {index} has non-canonical commit identity"
            )
        identities.add((source, signer))
    if expected_source is not None:
        expected = (expected_source, expected_signer)
        if (
            re.fullmatch(r"[0-9a-f]{40}", expected_source) is None
            or re.fullmatch(r"[0-9a-f]{40}", expected_signer) is None
            or identities != {expected}
        ):
            raise ManifestError(
                "verified attestation evidence differs from expected source/signer"
            )
        source, signer = expected
    else:
        image_source = arguments.image_source_commit
        consumer_candidate = arguments.consumer_candidate_commit
        for label, value in (
            ("image source", image_source),
            ("consumer candidate", consumer_candidate),
        ):
            if re.fullmatch(r"[0-9a-f]{40}", value) is None:
                raise ManifestError(f"{label} commit must be a lowercase full SHA")
        valid: set[tuple[str, str]] = set()
        for source, signer in identities:
            ancestry = subprocess.run(
                [
                    "git",
                    "-C",
                    str(arguments.repo_root),
                    "merge-base",
                    "--is-ancestor",
                    source,
                    consumer_candidate,
                ],
                check=False,
                capture_output=True,
            )
            if ancestry.returncode == 1:
                continue
            if ancestry.returncode != 0:
                raise ManifestError(
                    "cannot verify attestation source against consumer candidate"
                )
            try:
                _validate_candidate_source_binding(
                    arguments.repo_root,
                    source,
                    image_source,
                    require_head=False,
                )
            except ManifestError:
                continue
            valid.add((source, signer))
        maximal: set[tuple[str, str]] = set()
        for identity in valid:
            source, _ = identity
            superseded = False
            for other_source, _ in valid:
                if other_source == source:
                    continue
                relation = subprocess.run(
                    [
                        "git",
                        "-C",
                        str(arguments.repo_root),
                        "merge-base",
                        "--is-ancestor",
                        source,
                        other_source,
                    ],
                    check=False,
                    capture_output=True,
                )
                if relation.returncode == 0:
                    superseded = True
                    break
                if relation.returncode != 1:
                    raise ManifestError(
                        "cannot order verified attestation source candidates"
                    )
            if not superseded:
                maximal.add(identity)
        if len(maximal) != 1:
            raise ManifestError(
                "verified attestations lack one newest ancestry-bound source/signer pair"
            )
        source, signer = next(iter(maximal))
    print(source)
    print(signer)


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
    resolve_source = subparsers.add_parser(
        "resolve-source-commit",
        help="prove and print the candidate's last image-input-changing ancestor",
    )
    resolve_source.add_argument("--candidate-commit", required=True)
    resolve_source.add_argument("--workflow-commit", required=True)
    resolve_source.set_defaults(handler=_command_resolve_source_commit)
    source_binding = subparsers.add_parser(
        "verify-source-binding",
        help="verify candidate ancestry and zero canonical-input drift",
    )
    source_binding.add_argument("--candidate-commit", required=True)
    source_binding.add_argument("--source-commit", required=True)
    source_binding.add_argument("--require-head", action="store_true")
    source_binding.set_defaults(handler=_command_verify_source_binding)
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
    freshness.add_argument("--workflow-commit", required=True)
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
    attestation_identities = subparsers.add_parser(
        "attestation-identities",
        help="extract one certificate-bound source/signer identity pair",
    )
    attestation_identities.add_argument(
        "--attestation-json", type=Path, required=True
    )
    attestation_identities.add_argument("--fetch-limit", type=int, required=True)
    attestation_identities.add_argument("--image-source-commit", required=True)
    attestation_identities.add_argument("--consumer-candidate-commit", required=True)
    attestation_identities.add_argument("--expected-source-commit")
    attestation_identities.add_argument("--expected-signer-commit")
    attestation_identities.set_defaults(handler=_command_attestation_identities)
    bundle_snapshot = subparsers.add_parser(
        "snapshot-attestation-bundle",
        help="retain an unsaturated exact-subject GitHub bundle window",
    )
    bundle_snapshot.add_argument("--download-directory", type=Path, required=True)
    bundle_snapshot.add_argument("--subject-digest", required=True)
    bundle_snapshot.add_argument("--fetch-limit", type=int, required=True)
    bundle_snapshot.add_argument("--output", type=Path, required=True)
    bundle_snapshot.set_defaults(handler=_command_snapshot_attestation_bundle)
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
