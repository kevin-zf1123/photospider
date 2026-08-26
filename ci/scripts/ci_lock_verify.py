#!/usr/bin/env python3
"""Validate every repository-protected CI lock and active consumer.

This durable verifier is intentionally independent of candidate product code.
It rejects floating workflow actions/runners/images, persisted checkout tokens,
unsafe pull-request-target trust routing, reusable-workflow permission
inheritance drift, malformed protected locks, and Docker installation paths
that select an external frontend or bypass the exact helper identity, immutable
snapshot, network allowlist, hash locks, or complete Darwin/suite-gate mapping.
Run it from any directory; ``--repo-root`` is primarily for fixture tests.
"""

from __future__ import annotations

import argparse
import hashlib
import json
import os
import re
import shlex
import stat
import sys
from pathlib import Path
from typing import Any, Callable, Iterable, NamedTuple

_SCRIPT_DIRECTORY = Path(__file__).resolve().parent
if str(_SCRIPT_DIRECTORY) not in sys.path:
    sys.path.insert(0, str(_SCRIPT_DIRECTORY))
from ci_runner_verify import RunnerError, load_runner_lock


_SUITE_GATE_HELPER_SOURCE_SHA256 = (
    "e7790a57ace6ef052f252d9c084821eec5acb82271449645e3502d097dc9bcf8"
)
"""Verifier-owned exact source-byte identity for the protected suite gate."""


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


def _sha256_regular_file(
    path: Path,
    context: str,
    *,
    _test_hook: Callable[[str, int], None] | None = None,
) -> str:
    """Hash one stable retained regular-file snapshot without following links.

    Args:
        path: Exact protected helper or input file.
        context: Stable diagnostic identity for failure messages.

    Returns:
        Lowercase SHA-256 of the retained file bytes.

    Raises:
        ContractError: Required descriptor flags are unavailable, or the path
            is missing, aliased, non-regular, unreadable, changes identity, or
            yields different bytes while the retained descriptor is measured.

    Note:
        The private hook exists only for deterministic adversarial tests. It is
        called after the retained descriptor is established and after its first
        complete read. Production callers never supply it. The descriptor is
        opened with ``O_NOFOLLOW`` and ``O_CLOEXEC``; ``O_NONBLOCK`` prevents a
        hostile FIFO from blocking before its type can be rejected. Two reads
        from the same descriptor plus pathname/descriptor metadata checks catch
        in-place mutation and final-component replacement at the measured
        boundaries without reopening a potentially different object.
    """
    required_flags = ("O_NOFOLLOW", "O_CLOEXEC", "O_NONBLOCK")
    missing_flags = [name for name in required_flags if not hasattr(os, name)]
    if missing_flags:
        raise ContractError(
            f"{context}: required safe-open flags are unavailable: "
            f"{', '.join(missing_flags)}"
        )

    def stable_identity(value: os.stat_result) -> tuple[int, ...]:
        """Return metadata whose drift invalidates retained measurement.

        Args:
            value: One pathname or descriptor stat result.

        Returns:
            Device, inode, mode, link count, size, and nanosecond write/change
            timestamps used for exact phase comparisons.
        """
        return (
            value.st_dev,
            value.st_ino,
            value.st_mode,
            value.st_nlink,
            value.st_size,
            value.st_mtime_ns,
            value.st_ctime_ns,
        )

    def require_path_matches(
        descriptor_stat: os.stat_result, phase: str
    ) -> None:
        """Require the current pathname to name the retained regular object.

        Args:
            descriptor_stat: Current retained-descriptor metadata.
            phase: Stable measurement phase included in diagnostics.

        Returns:
            None when pathname type and identity exactly match the descriptor.

        Raises:
            ContractError: The path is missing, nonregular, or differs from the
                retained descriptor at this measurement phase.
        """
        try:
            path_stat = path.lstat()
        except OSError as error:
            raise ContractError(
                f"{context}: protected helper pathname is unavailable {phase}: {error}"
            ) from error
        if not stat.S_ISREG(path_stat.st_mode):
            raise ContractError(
                f"{context}: protected helper pathname is not regular {phase}"
            )
        if stable_identity(path_stat) != stable_identity(descriptor_stat):
            raise ContractError(
                f"{context}: protected helper pathname identity changed {phase}"
            )

    flags = (
        os.O_RDONLY
        | getattr(os, "O_NOFOLLOW")
        | getattr(os, "O_CLOEXEC")
        | getattr(os, "O_NONBLOCK")
    )
    descriptor = -1
    try:
        descriptor = os.open(path, flags)
        initial_stat = os.fstat(descriptor)
        if not stat.S_ISREG(initial_stat.st_mode):
            raise ContractError(f"{context}: protected helper must be a regular file")
        require_path_matches(initial_stat, "after open")
        if _test_hook is not None:
            _test_hook("after_open", descriptor)

        digests: list[str] = []
        for pass_number in (1, 2):
            digest = hashlib.sha256()
            byte_count = 0
            while True:
                chunk = os.read(descriptor, 1024 * 1024)
                if not chunk:
                    break
                byte_count += len(chunk)
                digest.update(chunk)
            pass_stat = os.fstat(descriptor)
            if (
                stable_identity(pass_stat) != stable_identity(initial_stat)
                or byte_count != initial_stat.st_size
            ):
                raise ContractError(
                    f"{context}: protected helper changed during retained read"
                )
            require_path_matches(pass_stat, f"after read {pass_number}")
            digests.append(digest.hexdigest())
            if pass_number == 1:
                if _test_hook is not None:
                    _test_hook("after_first_read", descriptor)
                os.lseek(descriptor, 0, os.SEEK_SET)
        if digests[0] != digests[1]:
            raise ContractError(
                f"{context}: protected helper bytes changed during retained read"
            )
        return digests[0]
    except ContractError:
        raise
    except OSError as error:
        raise ContractError(f"{context}: cannot hash protected helper: {error}") from error
    finally:
        if descriptor >= 0:
            os.close(descriptor)


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


def _workflow_files(root: Path) -> list[Path]:
    """Return the complete canonical workflow inventory without following links.

    Args:
        root: Repository root containing ``.github/workflows``.

    Returns:
        Every regular ``*.yml`` and ``*.yaml`` workflow in bytewise name order.

    Raises:
        ContractError: The workflow root is absent/aliased, an entry is a link
            or special file, an unexpected filename is present, or the inventory
            is empty/unreadable.
    """
    workflow_root = root / ".github" / "workflows"
    try:
        root_mode = workflow_root.lstat().st_mode
    except OSError as error:
        raise ContractError(f"cannot inspect workflow directory {workflow_root}: {error}") from error
    if stat.S_ISLNK(root_mode) or not stat.S_ISDIR(root_mode):
        raise ContractError(f"{workflow_root}: workflow root is not a real directory")
    try:
        entries = sorted(workflow_root.iterdir(), key=lambda candidate: candidate.name)
    except OSError as error:
        raise ContractError(f"cannot enumerate workflow directory {workflow_root}: {error}") from error

    workflows: list[Path] = []
    for path in entries:
        try:
            mode = path.lstat().st_mode
        except OSError as error:
            raise ContractError(f"cannot inspect workflow entry {path}: {error}") from error
        if stat.S_ISLNK(mode):
            raise ContractError(f"{path}: workflow entry must not be a link")
        if not stat.S_ISREG(mode):
            raise ContractError(f"{path}: workflow entry must be a regular file")
        if not re.fullmatch(r"[A-Za-z0-9][A-Za-z0-9_.-]*\.(?:yml|yaml)", path.name):
            raise ContractError(f"{path}: unexpected workflow directory entry")
        workflows.append(path)
    if not workflows:
        raise ContractError(f"{workflow_root}: workflow inventory is empty")
    return workflows


def _workflow_job_blocks(path: Path) -> dict[str, list[str]]:
    """Parse the protected workflow's top-level ``jobs`` mapping.

    Args:
        path: Regular workflow file whose two-space job layout is protected.

    Returns:
        A mapping from each exact job identifier to its indented body lines.

    Raises:
        ContractError: The workflow is unreadable, has no unique ``jobs``
            mapping, repeats a job, or uses an unsupported top-level job shape.

    Note:
        This is intentionally a strict parser for the maintained workflow
        subset, not a general YAML implementation. It interprets indentation
        and mapping ownership so a matching string in a step cannot satisfy a
        job-level permission contract.
    """
    try:
        lines = path.read_text(encoding="utf-8").splitlines()
    except (OSError, UnicodeError) as error:
        raise ContractError(f"cannot read workflow {path}: {error}") from error
    jobs_indices = [index for index, line in enumerate(lines) if line == "jobs:"]
    if len(jobs_indices) != 1:
        raise ContractError(f"{path}: expected exactly one top-level jobs mapping")

    jobs: dict[str, list[str]] = {}
    current_name: str | None = None
    for line_number, line in enumerate(lines[jobs_indices[0] + 1 :], jobs_indices[0] + 2):
        stripped = line.lstrip()
        if not stripped or stripped.startswith("#"):
            if current_name is not None:
                jobs[current_name].append(line)
            continue
        indent = len(line) - len(stripped)
        if indent == 0:
            break
        if indent == 2:
            match = re.fullmatch(r"  ([a-z][a-z0-9_-]*):", line)
            if match is None:
                raise ContractError(
                    f"{path}:{line_number}: unsupported protected job key"
                )
            current_name = match.group(1)
            if current_name in jobs:
                raise ContractError(f"{path}:{line_number}: duplicate job {current_name}")
            jobs[current_name] = []
            continue
        if current_name is None:
            raise ContractError(
                f"{path}:{line_number}: content precedes the first protected job"
            )
        jobs[current_name].append(line)
    if not jobs:
        raise ContractError(f"{path}: protected jobs mapping is empty")
    return jobs


def _strip_yaml_comment(value: str) -> str:
    """Remove one YAML plain-scalar comment without touching quoted ``#``.

    Args:
        value: Scalar tail from one protected workflow mapping entry.

    Returns:
        The scalar bytes before an unquoted whitespace-delimited comment.

    Raises:
        ContractError: A quoted scalar is unterminated.

    Note:
        GitHub expressions used by the maintained workflow contain no YAML
        quoting escapes. This helper nevertheless tracks both quote styles so
        action release hints cannot become part of the parsed action identity.
    """
    quote: str | None = None
    escaped = False
    for index, character in enumerate(value):
        if quote == '"' and escaped:
            escaped = False
            continue
        if quote == '"' and character == "\\":
            escaped = True
            continue
        if character in {"'", '"'}:
            if quote is None:
                quote = character
            elif quote == character:
                quote = None
            continue
        if character == "#" and quote is None and (
            index == 0 or value[index - 1].isspace()
        ):
            return value[:index].rstrip()
    if quote is not None:
        raise ContractError("protected workflow contains an unterminated quoted scalar")
    return value.rstrip()


def _yaml_mapping_entry(path: Path, line_number: int, text: str) -> tuple[str, str]:
    """Parse one canonical key/value entry from the protected YAML subset.

    Args:
        path: Workflow path used in diagnostics.
        line_number: One-based physical line number.
        text: Entry text after its owned indentation has been removed.

    Returns:
        The exact mapping key and comment-free scalar tail.

    Raises:
        ContractError: The key, merge/anchor syntax, or entry shape is outside
            the explicit protected subset.
    """
    match = re.fullmatch(r"([A-Za-z_][A-Za-z0-9_-]*):(?:[ ](.*))?", text)
    if match is None:
        raise ContractError(f"{path}:{line_number}: unsupported YAML mapping entry")
    key, tail = match.groups()
    if key == "<<" or (tail or "").lstrip().startswith(("&", "*")):
        raise ContractError(f"{path}:{line_number}: YAML aliases and merges are forbidden")
    return key, _strip_yaml_comment(tail or "")


def _yaml_flow_sequence(path: Path, line_number: int, value: str) -> list[str]:
    """Parse the maintained plain-scalar ``[a, b]`` YAML sequence form.

    Args:
        path: Workflow path used in diagnostics.
        line_number: One-based physical line number.
        value: Complete bracketed scalar text.

    Returns:
        Ordered nonempty plain scalar entries.

    Raises:
        ContractError: The flow value is empty, quoted, nested, or duplicated.
    """
    inner = value[1:-1].strip()
    if not inner:
        return []
    entries = [entry.strip() for entry in inner.split(",")]
    if any(
        not entry
        or any(marker in entry for marker in "[]{}'\"")
        or entry.startswith(("&", "*"))
        for entry in entries
    ):
        raise ContractError(f"{path}:{line_number}: unsupported YAML flow sequence")
    if len(entries) != len(set(entries)):
        raise ContractError(f"{path}:{line_number}: duplicate YAML sequence entry")
    return entries


def _yaml_scalar(path: Path, line_number: int, value: str) -> Any:
    """Decode one scalar from the deliberately small protected YAML grammar.

    Args:
        path: Workflow path used in diagnostics.
        line_number: One-based physical line number.
        value: Comment-free nonempty scalar text.

    Returns:
        A string, an explicit empty mapping, or a plain flow sequence.

    Raises:
        ContractError: Complex flow mappings/sequences or alias syntax appears.

    Note:
        Boolean-looking values intentionally remain strings. GitHub Actions
        owns expression evaluation; this parser owns only reviewed structure.
    """
    if value == "{}":
        return {}
    if value.startswith("["):
        if not value.endswith("]"):
            raise ContractError(f"{path}:{line_number}: unterminated YAML flow sequence")
        return _yaml_flow_sequence(path, line_number, value)
    if value.startswith("{") or value.endswith("}") and not value.endswith("}}"):
        raise ContractError(f"{path}:{line_number}: unsupported YAML flow mapping")
    if value.startswith(("&", "*")):
        raise ContractError(f"{path}:{line_number}: YAML aliases and anchors are forbidden")
    return value


def _yaml_next_content(lines: list[str], index: int) -> int:
    """Return the next nonblank, non-comment physical-line index."""
    while index < len(lines):
        stripped = lines[index].lstrip(" ")
        if stripped and not stripped.startswith("#"):
            break
        index += 1
    return index


def _yaml_block_scalar(
    path: Path,
    lines: list[str],
    index: int,
    parent_indent: int,
    style: str,
) -> tuple[str, int]:
    """Decode one literal or folded block owned by a workflow mapping field.

    Args:
        path: Workflow path used in diagnostics.
        lines: Physical workflow lines for one exact top-level job.
        index: First physical line after the block-scalar marker.
        parent_indent: Indentation of the owning mapping key.
        style: One of ``|``, ``|-``, ``>``, or ``>-``.

    Returns:
        Decoded text and the first unconsumed physical-line index.

    Raises:
        ContractError: The block is empty or escapes its owned indentation.
    """
    end = index
    while end < len(lines):
        stripped = lines[end].lstrip(" ")
        if stripped and len(lines[end]) - len(stripped) <= parent_indent:
            break
        end += 1
    owned = lines[index:end]
    content_indents = [
        len(line) - len(line.lstrip(" ")) for line in owned if line.strip()
    ]
    if not content_indents:
        raise ContractError(f"{path}: protected YAML block scalar is empty")
    content_indent = min(content_indents)
    if content_indent <= parent_indent:
        raise ContractError(f"{path}: protected YAML block scalar escapes its owner")
    fragments = [
        "" if not line.strip() else line[content_indent:] for line in owned
    ]
    if style.startswith("|"):
        value = "\n".join(fragments)
    else:
        paragraphs: list[str] = []
        current: list[str] = []
        for fragment in fragments:
            if fragment:
                current.append(fragment)
            elif current:
                paragraphs.append(" ".join(current))
                current = []
        if current:
            paragraphs.append(" ".join(current))
        value = "\n".join(paragraphs)
    if not style.endswith("-"):
        value += "\n"
    return value, end


def _parse_yaml_mapping(
    path: Path,
    lines: list[str],
    index: int,
    indent: int,
) -> tuple[dict[str, Any], int]:
    """Parse one indentation-owned mapping from the protected YAML subset."""
    result: dict[str, Any] = {}
    while True:
        index = _yaml_next_content(lines, index)
        if index >= len(lines):
            break
        line = lines[index]
        if "\t" in line[: len(line) - len(line.lstrip())]:
            raise ContractError(f"{path}: tabs cannot own protected YAML indentation")
        stripped = line.lstrip(" ")
        actual_indent = len(line) - len(stripped)
        if actual_indent < indent:
            break
        if actual_indent != indent or stripped.startswith("- "):
            raise ContractError(
                f"{path}:{index + 1}: unsupported protected YAML mapping indentation"
            )
        key, tail = _yaml_mapping_entry(path, index + 1, stripped)
        if key in result:
            raise ContractError(f"{path}:{index + 1}: duplicate YAML mapping key {key}")
        index += 1
        if tail in {"|", "|-", ">", ">-"}:
            result[key], index = _yaml_block_scalar(
                path, lines, index, actual_indent, tail
            )
            continue
        if tail:
            result[key] = _yaml_scalar(path, index, tail)
            continue
        child_index = _yaml_next_content(lines, index)
        if child_index >= len(lines):
            raise ContractError(f"{path}: YAML mapping {key} has no value")
        child_line = lines[child_index]
        child_stripped = child_line.lstrip(" ")
        child_indent = len(child_line) - len(child_stripped)
        if child_indent != actual_indent + 2:
            raise ContractError(
                f"{path}:{child_index + 1}: YAML child indentation differs for {key}"
            )
        if child_stripped.startswith("- "):
            result[key], index = _parse_yaml_sequence(
                path, lines, child_index, child_indent
            )
        else:
            result[key], index = _parse_yaml_mapping(
                path, lines, child_index, child_indent
            )
    return result, index


def _parse_yaml_sequence(
    path: Path,
    lines: list[str],
    index: int,
    indent: int,
) -> tuple[list[Any], int]:
    """Parse one scalar or mapping sequence from the protected YAML subset."""
    result: list[Any] = []
    while True:
        index = _yaml_next_content(lines, index)
        if index >= len(lines):
            break
        line = lines[index]
        stripped = line.lstrip(" ")
        actual_indent = len(line) - len(stripped)
        if actual_indent < indent:
            break
        if actual_indent != indent or not stripped.startswith("- "):
            raise ContractError(
                f"{path}:{index + 1}: unsupported protected YAML sequence indentation"
            )
        item_text = stripped[2:]
        index += 1
        if re.match(r"[A-Za-z_][A-Za-z0-9_-]*:", item_text):
            key, tail = _yaml_mapping_entry(path, index, item_text)
            if not tail or tail in {"|", "|-", ">", ">-"}:
                raise ContractError(
                    f"{path}:{index}: sequence mapping must start with a scalar field"
                )
            item: dict[str, Any] = {key: _yaml_scalar(path, index, tail)}
            next_index = _yaml_next_content(lines, index)
            if next_index < len(lines):
                next_line = lines[next_index]
                next_stripped = next_line.lstrip(" ")
                next_indent = len(next_line) - len(next_stripped)
                if next_indent == indent + 2:
                    additional, index = _parse_yaml_mapping(
                        path, lines, next_index, indent + 2
                    )
                    overlap = set(item) & set(additional)
                    if overlap:
                        raise ContractError(
                            f"{path}: sequence mapping repeats {sorted(overlap)}"
                        )
                    item.update(additional)
            result.append(item)
        else:
            scalar = _strip_yaml_comment(item_text)
            if not scalar:
                raise ContractError(f"{path}:{index}: empty YAML sequence entry")
            result.append(_yaml_scalar(path, index, scalar))
    if not result:
        raise ContractError(f"{path}: protected YAML sequence is empty")
    return result, index


def _workflow_job_mappings(
    path: Path, job_names: Iterable[str]
) -> dict[str, dict[str, Any]]:
    """Return exact top-level jobs decoded into protected YAML mappings.

    Args:
        path: Maintained workflow path.
        job_names: Exact jobs whose structure is security-authoritative.

    Returns:
        Each requested job as a nested mapping/list/scalar tree.

    Raises:
        ContractError: A job is absent, repeats a field, or uses unsupported
            YAML syntax/indentation in the protected subset.

    Note:
        Raw blocks are used only to establish top-level ownership. Every field,
        step, environment, dependency, and run command consumed by a verifier
        comes from this decoded mapping, never from substring membership.
    """
    blocks = _workflow_job_blocks(path)
    requested = list(job_names)
    if len(requested) != len(set(requested)):
        raise ContractError(f"{path}: duplicate protected job request")
    result: dict[str, dict[str, Any]] = {}
    for job_name in requested:
        if job_name not in blocks:
            raise ContractError(f"{path}: missing protected job {job_name}")
        mapping, index = _parse_yaml_mapping(path, blocks[job_name], 0, 4)
        if _yaml_next_content(blocks[job_name], index) != len(blocks[job_name]):
            raise ContractError(f"{path}: unconsumed YAML content in job {job_name}")
        result[job_name] = mapping
    return result


def _workflow_mapping(path: Path) -> dict[str, Any]:
    """Decode one complete protected workflow into the maintained YAML subset.

    Args:
        path: Regular workflow whose top-level event, permission, and job
            mappings are security-authoritative.

    Returns:
        The complete nested mapping with ordered step sequences and exact
        scalar/block values. YAML boolean-looking values remain strings.

    Raises:
        ContractError: The workflow is unreadable, empty, only partly consumed,
            or uses syntax outside the explicit protected YAML grammar.

    Note:
        Unlike ``_workflow_job_mappings``, this parser owns every top-level key.
        It is used where an extra trigger, permission, or job is itself unsafe.
    """
    try:
        lines = path.read_text(encoding="utf-8").splitlines()
    except (OSError, UnicodeError) as error:
        raise ContractError(f"cannot read workflow {path}: {error}") from error
    mapping, index = _parse_yaml_mapping(path, lines, 0, 0)
    if _yaml_next_content(lines, index) != len(lines):
        raise ContractError(f"{path}: unconsumed top-level YAML content")
    if not mapping:
        raise ContractError(f"{path}: protected workflow mapping is empty")
    return mapping


def _job_field_occurrences(lines: list[str], field: str) -> list[tuple[int, str]]:
    """Return exact job-level occurrences of one YAML mapping field.

    Args:
        lines: Body lines owned by one top-level job.
        field: Literal job-level field name to locate.

    Returns:
        Zero or more ``(line_index, scalar_tail)`` occurrences at four-space
        indentation. The scalar tail is empty for a nested mapping.

    Raises:
        ContractError: ``field`` is not a canonical protected key name.

    Note:
        Step-level and expression text are deliberately excluded by the exact
        indentation boundary.
    """
    if re.fullmatch(r"[a-z][a-z0-9_-]*", field) is None:
        raise ContractError(f"invalid protected workflow field {field!r}")
    pattern = re.compile(rf"^    {re.escape(field)}:\s*(.*?)\s*$")
    occurrences: list[tuple[int, str]] = []
    for index, line in enumerate(lines):
        match = pattern.fullmatch(line)
        if match is not None:
            occurrences.append((index, match.group(1)))
    return occurrences


def _job_nested_mapping(
    path: Path,
    job_name: str,
    lines: list[str],
    field: str,
) -> dict[str, str] | None:
    """Parse one strict job-level scalar mapping or literal empty mapping.

    Args:
        path: Workflow path used in fail-closed diagnostics.
        job_name: Exact owner job identifier.
        lines: Body lines owned by ``job_name``.
        field: Job-level mapping field, such as ``permissions`` or ``with``.

    Returns:
        ``None`` when the field is absent, otherwise its exact scalar entries.
        A literal ``{}`` returns an empty mapping.

    Raises:
        ContractError: The field is duplicated, uses aliases/non-scalar values,
            repeats a key, or escapes the maintained six-space mapping shape.

    Note:
        The parser intentionally rejects YAML merge keys and anchors. Protected
        permission ownership must remain explicit in the reviewed job itself.
    """
    occurrences = _job_field_occurrences(lines, field)
    if not occurrences:
        return None
    if len(occurrences) != 1:
        raise ContractError(f"{path}: job {job_name} repeats {field}")
    field_index, scalar_tail = occurrences[0]
    if scalar_tail == "{}":
        return {}
    if scalar_tail:
        raise ContractError(
            f"{path}: job {job_name} {field} must be an explicit mapping"
        )

    result: dict[str, str] = {}
    entry_pattern = re.compile(r"^      ([a-z][a-z0-9_-]*):\s*(.*?)\s*$")
    for line_number, line in enumerate(lines[field_index + 1 :], field_index + 2):
        stripped = line.lstrip()
        if not stripped or stripped.startswith("#"):
            continue
        indent = len(line) - len(stripped)
        if indent <= 4:
            break
        match = entry_pattern.fullmatch(line)
        if match is None:
            raise ContractError(
                f"{path}: job {job_name} has unsupported {field} entry at "
                f"body line {line_number}"
            )
        key, value = match.groups()
        if not value:
            raise ContractError(
                f"{path}: job {job_name} has an empty {field}.{key} value"
            )
        if key in result:
            raise ContractError(f"{path}: job {job_name} repeats {field}.{key}")
        result[key] = value
    if not result:
        raise ContractError(
            f"{path}: job {job_name} must use literal {{}} for empty {field}"
        )
    return result


def _job_scalar(path: Path, job_name: str, lines: list[str], field: str) -> str | None:
    """Return one exact job-level scalar without interpreting YAML aliases.

    Args:
        path: Workflow path used in diagnostics.
        job_name: Exact owner job identifier.
        lines: Body lines owned by ``job_name``.
        field: Literal scalar field to read.

    Returns:
        The scalar text, or ``None`` when the field is absent.

    Raises:
        ContractError: The field is duplicated or encoded as a nested/block
            value rather than one explicit scalar.
    """
    occurrences = _job_field_occurrences(lines, field)
    if not occurrences:
        return None
    if len(occurrences) != 1:
        raise ContractError(f"{path}: job {job_name} repeats {field}")
    _, value = occurrences[0]
    if not value or value in {">", ">-", "|", "|-"}:
        raise ContractError(f"{path}: job {job_name} {field} is not a scalar")
    return value


def _job_sequence(path: Path, job_name: str, lines: list[str], field: str) -> list[str] | None:
    """Return one strict job-level sequence of plain scalar values.

    Args:
        path: Workflow path used in fail-closed diagnostics.
        job_name: Exact owner job identifier.
        lines: Body lines owned by ``job_name``.
        field: Literal job-level sequence field to read.

    Returns:
        ``None`` when the field is absent, otherwise its ordered scalar values.

    Raises:
        ContractError: The field is duplicated, uses an inline/block value, is
            empty, repeats an entry, or escapes the maintained indentation shape.

    Note:
        This deliberately supports only the protected workflow subset. Anchors,
        aliases, mappings, and expressions cannot conceal a DAG dependency.
    """
    occurrences = _job_field_occurrences(lines, field)
    if not occurrences:
        return None
    if len(occurrences) != 1:
        raise ContractError(f"{path}: job {job_name} repeats {field}")
    field_index, scalar_tail = occurrences[0]
    if scalar_tail:
        raise ContractError(
            f"{path}: job {job_name} {field} must be an explicit sequence"
        )
    values: list[str] = []
    pattern = re.compile(r"^      - ([A-Za-z0-9][A-Za-z0-9_-]*)$")
    for line_number, line in enumerate(lines[field_index + 1 :], field_index + 2):
        stripped = line.lstrip()
        if not stripped or stripped.startswith("#"):
            continue
        indent = len(line) - len(stripped)
        if indent <= 4:
            break
        match = pattern.fullmatch(line)
        if match is None:
            raise ContractError(
                f"{path}: job {job_name} has unsupported {field} entry at "
                f"body line {line_number}"
            )
        values.append(match.group(1))
    if not values:
        raise ContractError(f"{path}: job {job_name} has an empty {field} sequence")
    if len(values) != len(set(values)):
        raise ContractError(f"{path}: job {job_name} repeats a {field} dependency")
    return values


def _job_condition(path: Path, job_name: str, lines: list[str]) -> str | None:
    """Return one whitespace-normalized job condition expression.

    Args:
        path: Workflow path used in diagnostics.
        job_name: Exact owner job identifier.
        lines: Body lines owned by ``job_name``.

    Returns:
        The scalar or folded-block condition with all whitespace removed, or
        ``None`` when the job has no condition.

    Raises:
        ContractError: ``if`` is duplicated, empty, or uses an unsupported
            block-scalar shape.

    Note:
        Normalization is limited to whitespace because the protected condition
        contains no string literal in which whitespace is semantically owned.
    """
    occurrences = _job_field_occurrences(lines, "if")
    if not occurrences:
        return None
    if len(occurrences) != 1:
        raise ContractError(f"{path}: job {job_name} repeats if")
    field_index, scalar_tail = occurrences[0]
    fragments: list[str]
    if scalar_tail in {">", ">-", "|", "|-"}:
        fragments = []
        for line in lines[field_index + 1 :]:
            stripped = line.lstrip()
            if not stripped:
                continue
            indent = len(line) - len(stripped)
            if indent <= 4:
                break
            fragments.append(stripped)
    elif scalar_tail:
        fragments = [scalar_tail]
    else:
        raise ContractError(f"{path}: job {job_name} has an empty if condition")
    if not fragments:
        raise ContractError(f"{path}: job {job_name} has an empty if condition")
    return re.sub(r"\s+", "", " ".join(fragments))


def _verify_reusable_workflow_permissions(root: Path) -> None:
    """Validate caller ceilings and per-job isolation in the shared CI DAG.

    Args:
        root: Repository root containing the protected caller and reusable DAG.

    Raises:
        ContractError: A caller can publish with insufficient grants, a
            read-only caller receives write authority, the reusable workflow
            declares a workflow-wide ceiling, any execution job can inherit
            caller writes, or the sole attestation job requests an elevation.

    Note:
        GitHub validates reusable-workflow permission compatibility before a
        job-level ``if`` can skip execution. The attestation job therefore has
        no local ``permissions`` declaration and may inherit write grants only
        from one of the two trusted callers; every other job declares an exact
        read-only or empty permission mapping and cannot inherit those writes.
    """
    caller_path = root / ".github/workflows/ci-integration.yml"
    shared_path = root / ".github/workflows/ci-integration-suite.yml"
    shared_text = shared_path.read_text(encoding="utf-8")
    if any(line.startswith("permissions:") for line in shared_text.splitlines()):
        raise ContractError(
            f"{shared_path}: reusable workflow must not set workflow-level permissions"
        )

    caller_jobs = _workflow_job_blocks(caller_path)
    shared_jobs = _workflow_job_blocks(shared_path)
    reusable_callers = {
        name: lines
        for name, lines in caller_jobs.items()
        if _job_scalar(caller_path, name, lines, "uses")
        == "./.github/workflows/ci-integration-suite.yml"
    }
    expected_callers = {
        "published-image-integration-readonly": (
            {
                "attestations": "read",
                "contents": "read",
                "packages": "read",
            },
            "false",
        ),
        "published-image-integration-trusted": (
            {
                "artifact-metadata": "write",
                "attestations": "write",
                "contents": "read",
                "id-token": "write",
                "packages": "read",
            },
            "true",
        ),
        "candidate-image-integration": (
            {
                "artifact-metadata": "write",
                "attestations": "write",
                "contents": "read",
                "id-token": "write",
                "packages": "read",
            },
            "true",
        ),
    }
    if set(reusable_callers) != set(expected_callers):
        raise ContractError(
            f"{caller_path}: shared-suite callers differ: "
            f"expected={sorted(expected_callers)}, actual={sorted(reusable_callers)}"
        )
    for job_name, (expected_permissions, expected_publish) in expected_callers.items():
        lines = reusable_callers[job_name]
        permissions = _job_nested_mapping(caller_path, job_name, lines, "permissions")
        publish_inputs = _job_nested_mapping(caller_path, job_name, lines, "with")
        if permissions != expected_permissions:
            raise ContractError(
                f"{caller_path}: caller {job_name} permissions differ: {permissions!r}"
            )
        if publish_inputs is None or publish_inputs.get(
            "publish_reusable_attestations"
        ) != expected_publish:
            raise ContractError(
                f"{caller_path}: caller {job_name} publication mode differs"
            )

    expected_job_permissions: dict[str, dict[str, str]] = {
        "identity-preflight": {
            "attestations": "read",
            "contents": "read",
            "packages": "read",
        },
        "integration-plan": {"contents": "read", "packages": "read"},
        "build-integrity-default": {"contents": "read", "packages": "read"},
        "verify-targeted-artifacts": {"contents": "read"},
        "targeted-artifacts-ready": {},
        "full-ctest": {
            "attestations": "read",
            "contents": "read",
            "packages": "read",
        },
        "build-smoke": {
            "attestations": "read",
            "contents": "read",
            "packages": "read",
        },
        "openexr-smoke": {
            "attestations": "read",
            "contents": "read",
            "packages": "read",
        },
        "scripted-cli": {
            "attestations": "read",
            "contents": "read",
            "packages": "read",
        },
        "propagation-script": {
            "attestations": "read",
            "contents": "read",
            "packages": "read",
        },
        "plugin-load": {
            "attestations": "read",
            "contents": "read",
            "packages": "read",
        },
        "execution-repeat": {
            "attestations": "read",
            "contents": "read",
            "packages": "read",
        },
        "installed-package-consumer": {
            "attestations": "read",
            "contents": "read",
            "packages": "read",
        },
        "sanitizer-asan": {"contents": "read", "packages": "read"},
        "sanitizer-tsan": {"contents": "read", "packages": "read"},
        "fuzz-codecs": {"contents": "read", "packages": "read"},
        "sanitizer-asan-darwin": {"contents": "read"},
        "sanitizer-tsan-darwin": {"contents": "read"},
        "fuzz-codecs-darwin": {"contents": "read"},
        "suite-gate": {"contents": "read"},
    }
    attestation_job = "attest-targeted-artifacts"
    expected_jobs = set(expected_job_permissions) | {attestation_job}
    if set(shared_jobs) != expected_jobs:
        raise ContractError(
            f"{shared_path}: shared jobs differ: expected={sorted(expected_jobs)}, "
            f"actual={sorted(shared_jobs)}"
        )
    for job_name, expected_permissions in expected_job_permissions.items():
        permissions = _job_nested_mapping(
            shared_path, job_name, shared_jobs[job_name], "permissions"
        )
        if permissions != expected_permissions:
            raise ContractError(
                f"{shared_path}: job {job_name} read-only permissions differ: "
                f"{permissions!r}"
            )
        if any(value != "read" for value in permissions.values()):
            raise ContractError(
                f"{shared_path}: job {job_name} contains a non-read permission"
            )

    inherited = _job_nested_mapping(
        shared_path,
        attestation_job,
        shared_jobs[attestation_job],
        "permissions",
    )
    if inherited is not None:
        raise ContractError(
            f"{shared_path}: {attestation_job} must inherit the trusted caller ceiling"
        )
    condition = _job_condition(
        shared_path, attestation_job, shared_jobs[attestation_job]
    )
    expected_condition = re.sub(
        r"\s+",
        "",
        "${{ inputs.publish_reusable_attestations && "
        "needs.verify-targeted-artifacts.result == 'success' }}",
    )
    if condition != expected_condition:
        raise ContractError(
            f"{shared_path}: {attestation_job} trust condition differs"
        )


def _step_lines(lines: list[str], action_index: int) -> list[str]:
    """Return the YAML lines owned by one ``- uses`` workflow step.

    Args:
        lines: Complete workflow text split into physical lines.
        action_index: Zero-based index of the step's ``uses`` line.

    Returns:
        Lines after ``uses`` and before the next sibling step or enclosing
        mapping boundary.

    Raises:
        ContractError: The indexed line is not a sequence action step.

    Note:
        This deliberately parses only the exact protected step shape needed by
        the lock verifier; general YAML semantics remain GitHub's concern.
    """
    action_line = lines[action_index]
    action_indent = len(action_line) - len(action_line.lstrip())
    if not action_line.lstrip().startswith("- uses:"):
        raise ContractError("checkout step parser received a non-action line")
    result: list[str] = []
    for line in lines[action_index + 1 :]:
        stripped = line.lstrip()
        if not stripped:
            result.append(line)
            continue
        indent = len(line) - len(stripped)
        if indent < action_indent or (
            indent == action_indent and stripped.startswith("- ")
        ):
            break
        result.append(line)
    return result


def _verify_checkout_step(path: Path, lines: list[str], action_index: int) -> None:
    """Require one checkout action to refuse credential persistence explicitly.

    Args:
        path: Workflow path used in diagnostics.
        lines: Complete workflow physical lines.
        action_index: Zero-based checkout ``uses`` line index.

    Raises:
        ContractError: The checkout lacks exactly one literal
            ``persist-credentials: false`` input.
    """
    action_line = lines[action_index]
    action_indent = len(action_line) - len(action_line.lstrip())
    step_lines = _step_lines(lines, action_index)
    with_indent = action_indent + 2
    with_indices = [
        index
        for index, line in enumerate(step_lines)
        if line == " " * with_indent + "with:"
    ]
    if len(with_indices) != 1:
        raise ContractError(
            f"{path}:{action_index + 1}: checkout must have one explicit with "
            "mapping containing persist-credentials: false"
        )
    input_indent = with_indent + 2
    values: list[str] = []
    pattern = re.compile(r"^\s*persist-credentials:\s*([^#\s]+)\s*(?:#.*)?$")
    for line in step_lines[with_indices[0] + 1 :]:
        stripped = line.lstrip()
        if not stripped:
            continue
        indent = len(line) - len(stripped)
        if indent <= with_indent:
            break
        match = pattern.match(line)
        if match and indent == input_indent:
            values.append(match.group(1))
    if values != ["false"]:
        raise ContractError(
            f"{path}:{action_index + 1}: checkout must set exactly one "
            "literal persist-credentials: false input"
        )


def _verify_pull_request_target_trust(path: Path, lines: list[str]) -> None:
    """Require fail-closed fork routing and a base-code ruleset reader.

    Args:
        path: Workflow path used in diagnostics.
        lines: Complete workflow physical lines.

    Raises:
        ContractError: Fork rejection can occur after checkout, is limited to
            ``CI/**``, or the token-bearing ruleset job consumes head code.

    Note:
        Same-repository heads remain eligible for the maintained sentinels and
        push gates. This check owns only the fork/base-token trust boundary.
    """
    if not any(line.strip() == "pull_request_target:" for line in lines):
        return
    reject_name = "- name: Reject fork pull request before checkout"
    reject_indices = [
        index for index, line in enumerate(lines) if line.strip() == reject_name
    ]
    checkout_indices = [
        index
        for index, line in enumerate(lines)
        if re.match(r"^\s*- uses:\s*actions/checkout@", line)
    ]
    if len(reject_indices) != 1 or not checkout_indices:
        raise ContractError(
            f"{path}: pull_request_target fork guard is missing or ambiguous"
        )
    reject_index = reject_indices[0]
    if reject_index >= checkout_indices[0]:
        raise ContractError(f"{path}: fork guard must precede the first checkout")
    reject_block = "\n".join(lines[reject_index : checkout_indices[0]])
    required_reject = (
        "github.event_name == 'pull_request_target'",
        "github.event.pull_request.head.repo.full_name != github.repository",
        "Fork pull requests are rejected before checkout.",
    )
    if any(fragment not in reject_block for fragment in required_reject):
        raise ContractError(f"{path}: fork guard does not reject every fork identity")
    if "startsWith(github.head_ref, 'CI/')" in reject_block:
        raise ContractError(f"{path}: fork guard is incorrectly limited to CI/** heads")
    try:
        reject_env_index = next(
            index
            for index in range(reject_index + 1, checkout_indices[0])
            if lines[index] == "        env:"
        )
    except StopIteration as error:
        raise ContractError(f"{path}: fork guard condition is not closed by env") from error
    normalized_condition = re.sub(
        r"\s+", "", "".join(lines[reject_index + 1 : reject_env_index])
    )
    expected_condition = (
        "if:>-${{github.event_name=='pull_request_target'&&"
        "github.event.pull_request.head.repo.full_name!=github.repository}}"
    )
    if normalized_condition != expected_condition:
        raise ContractError(f"{path}: fork guard condition does not reject every fork")

    try:
        job_index = lines.index("  ruleset-readback:")
    except ValueError as error:
        raise ContractError(
            f"{path}: pull_request_target lacks trusted ruleset readback"
        ) from error
    job_end = len(lines)
    for index in range(job_index + 1, len(lines)):
        if re.match(r"^  [A-Za-z0-9_.-]+:\s*$", lines[index]):
            job_end = index
            break
    ruleset_block = "\n".join(lines[job_index:job_end])
    required_ruleset = (
        "persist-credentials: false",
        "repository: ${{ github.repository }}",
        "ref: ${{ github.event_name == 'pull_request_target' && github.event.pull_request.base.sha || github.sha }}",
        "GH_TOKEN: ${{ secrets.GITHUB_TOKEN }}",
        "python3 ci/scripts/ruleset_readback.py",
    )
    if any(fragment not in ruleset_block for fragment in required_ruleset):
        raise ContractError(f"{path}: ruleset readback is not bound to trusted base code")
    if (
        "pull_request.head.repo" in ruleset_block
        or "pull_request.head.sha" in ruleset_block
    ):
        raise ContractError(f"{path}: ruleset readback consumes untrusted head identity")


def _verify_ci_image_producer(
    root: Path, actions: dict[str, tuple[str, str]]
) -> None:
    """Require the complete callable CI-image producer workflow mapping.

    Args:
        root: Repository root containing the protected producer workflow.
        actions: Canonical action name to release/full-SHA lock mapping.

    Returns:
        None when the complete trigger, permission, build job, ordered steps,
        action inputs, commands, environments, outputs, and publication
        identities equal the reviewed producer contract.

    Raises:
        ContractError: A required action lock is absent or any top-level/job/
            step field differs, including an extra build, command, condition,
            environment, build argument, tag, or publication path.

    Note:
        Exact parsed-tree equality makes checkout then lock verification a hard
        pre-build order and admits one Buildx build/push action only. Comments
        remain review hints; they cannot satisfy or widen an active mapping.
    """
    path = root / ".github/workflows/build-ci-image.yml"
    required_actions = (
        "actions/attest",
        "actions/checkout",
        "actions/upload-artifact",
        "docker/build-push-action",
        "docker/login-action",
    )
    missing_actions = [name for name in required_actions if name not in actions]
    if missing_actions:
        raise ContractError(
            f"{path}: producer action locks are absent: {missing_actions}"
        )
    action_references = {
        name: f"{name}@{actions[name][1]}" for name in required_actions
    }

    caller_run = (
        "set -Eeuo pipefail\n"
        'if [[ "$CI_EVENT_NAME" != push ||\n'
        '  ("$CI_EVENT_REF" != refs/heads/main && '
        '"$CI_EVENT_REF" != refs/heads/CI/*) ]]; then\n'
        '  echo "CI image candidates may be built only by trusted main or '
        'CI/** pushes." >&2\n'
        "  exit 1\n"
        "fi\n"
        'if [[ ! "$CI_CANDIDATE_COMMIT" =~ ^[0-9a-f]{40}$ ||\n'
        '  "$CI_CANDIDATE_COMMIT" != "$CI_EVENT_COMMIT" ]]; then\n'
        '  echo "Candidate image commit differs from the trusted push '
        'commit." >&2\n'
        "  exit 1\n"
        "fi\n"
        'temporary_tag="candidate-$CI_CANDIDATE_COMMIT-$CI_RUN_ID-'
        '$CI_RUN_ATTEMPT"\n'
        "printf 'temporary_tag=%s\\n' \"$temporary_tag\" >> \"$GITHUB_OUTPUT\"\n"
        "printf 'temporary_image=%s:%s\\n' \\\n"
        '  "$CI_IMAGE_REPOSITORY" "$temporary_tag" >> "$GITHUB_OUTPUT"\n\n'
    )
    source_run = (
        "set -Eeuo pipefail\n"
        "source_commit=$(python3 ci/scripts/ci_image_manifest.py \\\n"
        "  publish-source-commit \\\n"
        '  --workflow-commit "${{ inputs.candidate_commit }}")\n'
        "printf 'commit=%s\\n' \"$source_commit\" >> \"$GITHUB_OUTPUT\"\n\n"
    )
    builder_run = (
        "set -Eeuo pipefail\n"
        "python3 ci/scripts/ci_runner_verify.py \\\n"
        "  --platform Linux \\\n"
        "  --runner-label ubuntu-24.04 \\\n"
        '  --output "$CI_RUNNER_IDENTITY_FILE"\n'
        "image_version=$(python3 ci/scripts/ci_image_manifest.py builder-label \\\n"
        '  --builder-runner-identity "$CI_RUNNER_IDENTITY_FILE")\n'
        "printf 'image_version=%s\\n' \"$image_version\" >> \"$GITHUB_OUTPUT\"\n\n"
    )
    manifest_run = (
        "set -Eeuo pipefail\n"
        'mkdir -p "$CI_IMAGE_MANIFEST_DIR"\n'
        "digest=$(python3 ci/scripts/ci_image_manifest.py create \\\n"
        '  --source-commit "${{ steps.source.outputs.commit }}" \\\n'
        '  --repository "${{ github.repository }}" \\\n'
        '  --builder-runner-identity "${{ runner.temp }}/photospider-builder-'
        'runner-${{ github.run_id }}-${{ github.run_attempt }}.json" \\\n'
        '  --output "$CI_IMAGE_MANIFEST_DIR/ci-image-input-v1.json" \\\n'
        '  --digest-output "$CI_IMAGE_MANIFEST_DIR/'
        'ci-image-input-v1.sha256")\n'
        "printf 'digest=%s\\n' \"$digest\" >> \"$GITHUB_OUTPUT\"\n\n"
    )
    cross_check_run = (
        "set -Eeuo pipefail\n"
        '[[ "$CI_VERIFIED_DIGEST" == "$CI_BUILT_DIGEST" ]]\n'
        '[[ "$CI_VERIFIED_MANIFEST_DIGEST" == '
        '"$CI_CREATED_MANIFEST_DIGEST" ]]\n'
        '[[ "$CI_VERIFIED_SOURCE_COMMIT" == "$CI_CREATED_SOURCE_COMMIT" ]]\n'
        '[[ "$CI_VERIFIED_BUILDER_IMAGE_VERSION" == '
        '"$CI_CREATED_BUILDER_IMAGE_VERSION" ]]\n\n'
    )
    expected = {
        "name": "Build CI Image Candidate",
        "on": {
            "workflow_call": {
                "inputs": {
                    "candidate_commit": {
                        "description": (
                            "Exact trusted push commit that changed canonical "
                            "image inputs."
                        ),
                        "required": "true",
                        "type": "string",
                    }
                },
                "outputs": {
                    "digest": {
                        "description": (
                            "Exact attested OCI digest produced by the single build."
                        ),
                        "value": "${{ jobs.build.outputs.digest }}",
                    },
                    "image_ref": {
                        "description": "Digest-qualified candidate image reference.",
                        "value": "${{ jobs.build.outputs.image_ref }}",
                    },
                    "manifest_digest": {
                        "description": "Canonical CI-image input manifest digest.",
                        "value": "${{ jobs.build.outputs.manifest_digest }}",
                    },
                    "builder_image_version": {
                        "description": (
                            "Exact approved Linux image version that built the OCI subject."
                        ),
                        "value": "${{ jobs.build.outputs.builder_image_version }}",
                    },
                    "source_commit": {
                        "description": (
                            "Canonical image-input-changing source commit."
                        ),
                        "value": "${{ jobs.build.outputs.source_commit }}",
                    },
                    "temporary_tag": {
                        "description": (
                            "Event-scoped temporary SHA tag used by the producer."
                        ),
                        "value": "${{ jobs.build.outputs.temporary_tag }}",
                    },
                },
            }
        },
        "permissions": {
            "artifact-metadata": "write",
            "attestations": "write",
            "contents": "read",
            "id-token": "write",
            "packages": "write",
        },
        "jobs": {
            "build": {
                "runs-on": "ubuntu-24.04",
                "outputs": {
                    "digest": "${{ steps.identity.outputs.digest }}",
                    "image_ref": "${{ steps.identity.outputs.image }}",
                    "manifest_digest": (
                        "${{ steps.identity.outputs.manifest_digest }}"
                    ),
                    "builder_image_version": (
                        "${{ steps.builder.outputs.image_version }}"
                    ),
                    "source_commit": "${{ steps.identity.outputs.source_commit }}",
                    "temporary_tag": "${{ steps.caller.outputs.temporary_tag }}",
                },
                "steps": [
                    {
                        "name": "Require a trusted image-input push",
                        "id": "caller",
                        "env": {
                            "CI_CANDIDATE_COMMIT": "${{ inputs.candidate_commit }}",
                            "CI_EVENT_NAME": "${{ github.event_name }}",
                            "CI_EVENT_COMMIT": "${{ github.sha }}",
                            "CI_EVENT_REF": "${{ github.ref }}",
                            "CI_IMAGE_REPOSITORY": (
                                "ghcr.io/${{ github.repository }}/photospider-ci"
                            ),
                            "CI_RUN_ATTEMPT": "${{ github.run_attempt }}",
                            "CI_RUN_ID": "${{ github.run_id }}",
                        },
                        "run": caller_run,
                    },
                    {
                        "uses": action_references["actions/checkout"],
                        "with": {
                            "persist-credentials": "false",
                            "fetch-depth": "0",
                            "ref": "${{ inputs.candidate_commit }}",
                        },
                    },
                    {
                        "name": "Verify protected locks",
                        "run": "python3 ci/scripts/ci_lock_verify.py",
                    },
                    {
                        "name": "Verify exact Linux builder image",
                        "id": "builder",
                        "env": {
                            "CI_RUNNER_IDENTITY_FILE": (
                                "${{ runner.temp }}/photospider-builder-runner-"
                                "${{ github.run_id }}-${{ github.run_attempt }}.json"
                            )
                        },
                        "run": builder_run,
                    },
                    {
                        "name": "Resolve publishable image source identity",
                        "id": "source",
                        "run": source_run,
                    },
                    {
                        "name": "Create canonical image input manifest",
                        "id": "manifest",
                        "env": {
                            "CI_IMAGE_MANIFEST_DIR": (
                                "${{ github.workspace }}/CI-results/ci-image-manifest"
                            )
                        },
                        "run": manifest_run,
                    },
                    {
                        "name": "Log in to GHCR",
                        "uses": action_references["docker/login-action"],
                        "with": {
                            "registry": "ghcr.io",
                            "username": "${{ github.actor }}",
                            "password": "${{ secrets.GITHUB_TOKEN }}",
                        },
                    },
                    {
                        "name": "Build and push candidate exactly once",
                        "id": "push",
                        "uses": action_references["docker/build-push-action"],
                        "with": {
                            "context": ".",
                            "file": "Dockerfile.ci",
                            "push": "true",
                            "tags": "${{ steps.caller.outputs.temporary_image }}",
                            "labels": (
                                "org.opencontainers.image.revision="
                                "${{ steps.source.outputs.commit }}\n"
                                "org.photospider.ci.builder-image-version="
                                "${{ steps.builder.outputs.image_version }}\n"
                                "org.photospider.ci.input-manifest-sha256="
                                "${{ steps.manifest.outputs.digest }}\n"
                            ),
                            "build-args": (
                                "CI_IMAGE_INPUT_MANIFEST_SHA256="
                                "${{ steps.manifest.outputs.digest }}\n"
                                "CI_IMAGE_SOURCE_COMMIT="
                                "${{ steps.source.outputs.commit }}\n\n"
                            ),
                        },
                    },
                    {
                        "name": "Attest exact temporary OCI subject",
                        "uses": action_references["actions/attest"],
                        "with": {
                            "subject-name": (
                                "ghcr.io/${{ github.repository }}/photospider-ci"
                            ),
                            "subject-digest": "${{ steps.push.outputs.digest }}",
                            "push-to-registry": "true",
                        },
                    },
                    {
                        "name": "Verify candidate digest, attestation, and labels",
                        "id": "identity",
                        "env": {
                            "CI_ARTIFACT_DIR": (
                                "${{ github.workspace }}/CI-results/ci-image-identity"
                            ),
                            "CI_IMAGE_EXPECTED_DIGEST": (
                                "${{ steps.push.outputs.digest }}"
                            ),
                            "CI_IMAGE_LOCATOR": (
                                "${{ steps.caller.outputs.temporary_image }}"
                            ),
                            "CI_IMAGE_REPOSITORY": "${{ github.repository }}",
                            "GH_TOKEN": "${{ secrets.GITHUB_TOKEN }}",
                        },
                        "run": "bash ci/scripts/ci_image_verify.sh",
                    },
                    {
                        "name": "Cross-check build and verified identities",
                        "env": {
                            "CI_BUILT_DIGEST": "${{ steps.push.outputs.digest }}",
                            "CI_CREATED_MANIFEST_DIGEST": (
                                "${{ steps.manifest.outputs.digest }}"
                            ),
                            "CI_CREATED_SOURCE_COMMIT": (
                                "${{ steps.source.outputs.commit }}"
                            ),
                            "CI_CREATED_BUILDER_IMAGE_VERSION": (
                                "${{ steps.builder.outputs.image_version }}"
                            ),
                            "CI_VERIFIED_DIGEST": (
                                "${{ steps.identity.outputs.digest }}"
                            ),
                            "CI_VERIFIED_MANIFEST_DIGEST": (
                                "${{ steps.identity.outputs.manifest_digest }}"
                            ),
                            "CI_VERIFIED_SOURCE_COMMIT": (
                                "${{ steps.identity.outputs.source_commit }}"
                            ),
                            "CI_VERIFIED_BUILDER_IMAGE_VERSION": (
                                "${{ steps.identity.outputs.builder_image_version }}"
                            ),
                        },
                        "run": cross_check_run,
                    },
                    {
                        "name": "Upload canonical image input manifest",
                        "uses": action_references["actions/upload-artifact"],
                        "with": {
                            "name": (
                                "ci-image-input-${{ steps.source.outputs.commit }}-"
                                "${{ github.run_id }}-${{ github.run_attempt }}"
                            ),
                            "path": "CI-results/ci-image-manifest",
                            "if-no-files-found": "error",
                            "retention-days": "7",
                        },
                    },
                ],
            }
        },
    }
    if _workflow_mapping(path) != expected:
        raise ContractError(f"{path}: complete CI-image producer mapping differs")


def _verify_workflows(root: Path, actions: dict[str, tuple[str, str]]) -> None:
    """Verify every workflow action, runner, and container identity.

    Both supported YAML suffixes traverse the same parser and lock rules. The
    inventory helper fails closed before parsing if the directory contains an
    alias, special file, subdirectory, or unexpected regular entry.

    Args:
        root: Repository root containing protected workflows.
        actions: Canonical action name to release/full-SHA lock mapping.

    Raises:
        ContractError: Any workflow surface or consumed identity is unsafe,
            floating, unknown, or inconsistent with the lock table.
    """
    uses_pattern = re.compile(r"^\s*(?:-\s*)?uses:\s*([^#\s]+)(?:\s+#\s*(\S+))?\s*$")
    image_pattern = re.compile(r"^\s*image:\s*(.*?)\s*$")
    runner_pattern = re.compile(r"^\s*runs-on:\s*([^#\s]+)")
    used_actions: set[str] = set()
    for path in _workflow_files(root):
        try:
            lines = path.read_text(encoding="utf-8").splitlines()
        except (OSError, UnicodeError) as error:
            raise ContractError(f"cannot read workflow {path}: {error}") from error
        _verify_pull_request_target_trust(path, lines)
        for line_number, line in enumerate(lines, 1):
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
                if action == "actions/checkout":
                    _verify_checkout_step(path, lines, line_number - 1)
            image_match = image_pattern.match(line)
            if image_match:
                image = image_match.group(1)
                if "${{" in image:
                    trusted_outputs = (
                        "inputs.image_ref",
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


def _verify_packages(path: Path) -> dict[str, str]:
    """Validate and return the exact sorted top-level Ubuntu package lock.

    Args:
        path: Protected ``name=version`` package lock.

    Returns:
        A mapping from each unique package name to its exact locked version.

    Raises:
        ContractError: A row has a non-Debian package name or malformed exact
            version, ordering/uniqueness differs, or the two-package offline
            TLS bootstrap is absent.

    Note:
        The same mapping drives the ordinary snapshot transaction and validates
        the special pre-APT bootstrap identities; it is not a second package list.
    """
    packages: list[str] = []
    versions: dict[str, str] = {}
    for line_number, line in enumerate(path.read_text(encoding="utf-8").splitlines(), 1):
        if not line or line.startswith("#"):
            continue
        if not re.fullmatch(r"[a-z0-9][a-z0-9+.-]+=[^\s=]+", line):
            raise ContractError(f"{path}:{line_number}: invalid exact package lock")
        package, version = line.split("=", 1)
        packages.append(package)
        versions[package] = version
    _require_sorted_unique(packages, str(path))
    for package in ("ca-certificates", "openssl"):
        if package not in versions:
            raise ContractError(f"{path}: offline TLS bootstrap package {package} must be locked")
    return versions


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


def _verify_apt_bootstrap_lock(root: Path, lock: dict[str, Any]) -> tuple[str, ...]:
    """Validate the hash-locked offline TLS bootstrap and return Docker clauses.

    Args:
        root: Repository root containing the package and image locks.
        lock: Strictly decoded protected CI-image lock.

    Returns:
        The three canonical normalized Docker ``ADD --checksum`` clauses.

    Raises:
        ContractError: Snapshot, schema, package-version, URL, architecture, or
            SHA-256 identity differs from the protected two-package closure.

    Note:
        Ubuntu's minimal base lacks CA certificates. Only ``openssl`` plus
        ``ca-certificates`` are bootstrapped before APT, and their versions also
        occur in the ordinary package lock consumed by the snapshot transaction.
    """
    snapshot = lock.get("apt_snapshot")
    if not isinstance(snapshot, str) or not re.fullmatch(r"[0-9]{8}T[0-9]{6}Z", snapshot):
        raise ContractError("CI image lock has no canonical APT snapshot identity")
    bootstrap = lock.get("apt_bootstrap")
    if not isinstance(bootstrap, dict):
        raise ContractError("CI image lock has no offline APT bootstrap")
    _exact_keys(bootstrap, {"ca_certificates", "openssl"}, "apt_bootstrap")
    ca_certificates = bootstrap["ca_certificates"]
    openssl = bootstrap["openssl"]
    if not isinstance(ca_certificates, dict) or not isinstance(openssl, dict):
        raise ContractError("APT bootstrap package identities must be objects")
    _exact_keys(
        ca_certificates,
        {"sha256", "url", "version"},
        "apt_bootstrap.ca_certificates",
    )
    _exact_keys(
        openssl,
        {"amd64_sha256", "amd64_url", "arm64_sha256", "arm64_url", "version"},
        "apt_bootstrap.openssl",
    )
    package_versions = _verify_packages(root / "ci/locks/ubuntu-24.04-packages.lock")
    if ca_certificates["version"] != package_versions["ca-certificates"]:
        raise ContractError("APT CA bootstrap version differs from the package lock")
    if openssl["version"] != package_versions["openssl"]:
        raise ContractError("APT OpenSSL bootstrap version differs from the package lock")

    ca_url = (
        f"https://snapshot.ubuntu.com/ubuntu/{snapshot}/pool/main/c/ca-certificates/"
        f"ca-certificates_{ca_certificates['version']}_all.deb"
    )
    if ca_certificates["url"] != ca_url:
        raise ContractError("APT CA bootstrap URL is not the locked snapshot package")
    clauses = [
        f"ADD --checksum=sha256:{ca_certificates['sha256']} {ca_url} "
        "/tmp/ci-bootstrap/ca-certificates.deb"
    ]
    for architecture in ("amd64", "arm64"):
        sha256 = openssl[f"{architecture}_sha256"]
        url = openssl[f"{architecture}_url"]
        expected_url = (
            f"https://snapshot.ubuntu.com/ubuntu/{snapshot}/pool/main/o/openssl/"
            f"openssl_{openssl['version']}_{architecture}.deb"
        )
        if url != expected_url:
            raise ContractError(
                f"APT OpenSSL {architecture} bootstrap URL is not the locked snapshot package"
            )
        if not re.fullmatch(r"[0-9a-f]{64}", str(sha256)):
            raise ContractError(f"APT OpenSSL {architecture} bootstrap hash is malformed")
        clauses.append(
            f"ADD --checksum=sha256:{sha256} {url} "
            f"/tmp/ci-bootstrap/openssl-{architecture}.deb"
        )
    if not re.fullmatch(r"[0-9a-f]{64}", str(ca_certificates["sha256"])):
        raise ContractError("APT CA bootstrap hash is malformed")
    return tuple(clauses)


def _active_helper_source(path: Path) -> tuple[str, str]:
    """Return complete text and active-line identity for one protected helper.

    Args:
        path: Protected helper whose executable text is inspected.

    Returns:
        Complete UTF-8 source and a normalized stream containing every
        nonblank, non-full-line-comment source line with outer whitespace
        removed and one canonical trailing newline.

    Raises:
        ContractError: The helper cannot be decoded as UTF-8.

    Note:
        Full-file SHA-256 owns exact bytes. This stream independently owns
        executable statements so comments cannot forge the semantic allowlist.
    """
    try:
        source = path.read_text(encoding="utf-8")
    except (OSError, UnicodeError) as error:
        raise ContractError(f"{path}: cannot read protected helper source: {error}") from error
    active_lines = [
        line.strip()
        for line in source.splitlines()
        if line.strip() and not line.lstrip().startswith("#")
    ]
    return source, "\n".join(active_lines) + "\n"


def _verify_installer_semantics(path: Path) -> None:
    """Require the complete canonical CI-image installer active surface.

    Args:
        path: Exact protected installer source.

    Returns:
        None after executable identity and network/install allowlist match.

    Raises:
        ContractError: Active source, entrypoint/control flow, network command
            set, hash consumption, or package/install sequence differs.

    Note:
        The active digest is verifier-owned, not copied from the JSON lock.
        Explicit checks make network and control boundaries reviewable.
    """
    source, active = _active_helper_source(path)
    active_digest = hashlib.sha256(active.encode("utf-8")).hexdigest()
    expected_active_digest = (
        "9fcfbcef038034b146468c4f9577b578e08a880cb92b7137f77aeb67208bb373"
    )
    if active_digest != expected_active_digest:
        raise ContractError(f"{path}: installer active statement identity differs")
    active_lines = active.splitlines()
    if active_lines[:2] != ["set -Eeuo pipefail", "umask 022"]:
        raise ContractError(f"{path}: installer strict-mode preamble differs")
    if active_lines.count("ci_image_install_main() {") != 1 or active_lines[-1] != (
        'ci_image_install_main "$@"'
    ):
        raise ContractError(f"{path}: installer entrypoint invocation differs")
    if re.search(r"(?:^|[;&|\s])exit(?:[;&|\s]|$)", active):
        raise ContractError(f"{path}: installer contains an early process exit")
    apt_commands = re.findall(
        r"(?:^|[\s|;&])([^\s|;&]*apt-get)(?=[\s|;&]|$)", active
    )
    if apt_commands != ["apt-get", "apt-get"]:
        raise ContractError(f"{path}: installer APT command allowlist differs")
    if len(re.findall(r"(?:^|[\s|;&])curl(?=[\s|;&]|$)", active)) != 1:
        raise ContractError(f"{path}: installer curl network allowlist differs")
    if re.search(r"(?:^|[\s|;&])(?:wget|sh|bash)(?=[\s|;&]|$)", active):
        raise ContractError(f"{path}: installer invokes an unapproved shell/network command")
    required_fragments = (
        'curl --fail --location --proto \'=https\' --tlsv1.2',
        '"https://github.com/cli/cli/releases/download/v${GH_CLI_VERSION}/$gh_archive"',
        "| sha256sum --check --strict -",
        'tar -C /tmp -xzf "/tmp/$gh_archive"',
        'install -m 0755 "/tmp/gh_${GH_CLI_VERSION}_linux_${architecture}/bin/gh"',
        "apt-get update",
        "| xargs apt-get install -y --no-install-recommends --",
        '"$VENV/bin/pip" install',
        "--require-hashes",
    )
    for fragment in required_fragments:
        if active.count(fragment) != 1:
            raise ContractError(
                f"{path}: installer required network/install fragment differs at {fragment!r}"
            )
    forbidden_fragments = (
        "curl |",
        "sha256sum()",
        "/usr/bin/apt-get",
        "/bin/apt-get",
        "--allow-unauthenticated",
        "Acquire::https::Verify-Peer=false",
        "trusted=yes",
    )
    for fragment in forbidden_fragments:
        if fragment in active:
            raise ContractError(f"{path}: installer contains forbidden fragment {fragment!r}")
    if re.search(r"\|[ \t]*(?:sh|bash)(?:[ \t;]|$)", active):
        raise ContractError(f"{path}: installer contains a pipe-to-shell command")
    if re.search(r"(?:^|[;&])[ \t]*(?:alias|eval|source|\.)[ \t]+", active):
        raise ContractError(f"{path}: installer contains dynamic shell indirection")


def _verify_suite_gate_helper_identity(
    path: Path, declared_hash: str, actual_hash: str
) -> None:
    """Require three-way exact source identity for the suite-gate helper.

    Args:
        path: Exact protected Python gate helper.
        declared_hash: Full-file SHA-256 declared by the protected image lock.
        actual_hash: SHA-256 measured from the retained regular-file object.

    Returns:
        None when verifier constant, lock declaration, and actual bytes match.

    Raises:
        ContractError: Any of the three independently owned identities differs.

    Note:
        Exact source bytes are stable across supported Python minor versions;
        ``ast.dump`` and ``ast.unparse`` are intentionally not identity
        authorities. Behavior tests separately execute every required result and
        attestation branch, while mutation tests prove that changing the helper
        and recomputing only the ordinary JSON lock hash remains insufficient.
    """
    if not (
        declared_hash
        == actual_hash
        == _SUITE_GATE_HELPER_SOURCE_SHA256
    ):
        raise ContractError(
            f"{path}: suite-gate verifier-owned source identity differs"
        )


def _verify_protected_helpers(
    root: Path, lock: dict[str, Any], input_paths: list[str]
) -> dict[str, dict[str, str]]:
    """Validate helper roles, versions, paths, hashes, and execution boundaries.

    Args:
        root: Repository root containing protected helper sources.
        lock: Strict CI-image lock object.
        input_paths: Canonical image-input inventory.

    Returns:
        Canonical helper records keyed by stable role.

    Raises:
        ContractError: Helper inventory, version, path, hash, retained-file or
            verifier-owned source identity, or installer allowlist differs.

    Note:
        Both helpers are canonical image/control inputs even though only the
        installer is copied into the image.
    """
    helpers = lock.get("protected_helpers")
    expected = {
        "ci-image-installer": "ci/scripts/ci_image_install.sh",
        "integration-suite-gate": "ci/scripts/integration_suite_gate.py",
    }
    if not isinstance(helpers, dict) or set(helpers) != set(expected):
        raise ContractError("protected helper identity set differs")
    result: dict[str, dict[str, str]] = {}
    for name, expected_path in expected.items():
        record = helpers[name]
        if not isinstance(record, dict):
            raise ContractError(f"protected helper {name!r} record is not an object")
        _exact_keys(record, {"path", "sha256", "version"}, f"protected helper {name}")
        if record["path"] != expected_path or record["version"] != "v1":
            raise ContractError(f"protected helper {name!r} path/version differs")
        if expected_path not in input_paths:
            raise ContractError(f"protected helper {name!r} is absent from image inputs")
        declared_hash = record["sha256"]
        if not isinstance(declared_hash, str) or re.fullmatch(
            r"[0-9a-f]{64}", declared_hash
        ) is None:
            raise ContractError(f"protected helper {name!r} hash is malformed")
        actual_hash = _sha256_regular_file(root / expected_path, f"protected helper {name}")
        if actual_hash != declared_hash:
            raise ContractError(f"protected helper {name!r} bytes differ from the lock")
        if name == "integration-suite-gate":
            _verify_suite_gate_helper_identity(
                root / expected_path, declared_hash, actual_hash
            )
        result[name] = {
            "path": expected_path,
            "sha256": declared_hash,
            "version": "v1",
        }
    _verify_installer_semantics(root / expected["ci-image-installer"])
    return result


def _verify_ci_image_resolver_order(root: Path) -> None:
    """Require attestation before image-layer pull and identity reconstruction.

    Args:
        root: Repository root containing the protected image resolver.

    Raises:
        ContractError: A required active command is missing, duplicated, or
            ordered so that Docker can pull/inspect layers, builder labels can
            be retained, a manifest can be reconstructed, or final output can
            be emitted before exact-subject attestation succeeds.

    Note:
        Only exact non-comment command lines are authoritative. The durable
        shell mock separately executes success and attestation-failure paths;
        this static check makes the reviewed order part of the protected lock
        verifier without treating comments as executable evidence.
    """
    path = root / "ci/scripts/ci_image_verify.sh"
    try:
        lines = path.read_text(encoding="utf-8").splitlines()
    except (OSError, UnicodeError) as error:
        raise ContractError(
            f"cannot read protected CI image resolver {path}: {error}"
        ) from error
    active = [
        line.strip()
        for line in lines
        if line.strip() and not line.lstrip().startswith("#")
    ]
    ordered_commands = (
        'inspect_output=$(docker buildx imagetools inspect "$locator")',
        'source_commit=$(python3 "$SCRIPT_DIR/ci_image_manifest.py" \\',
        'gh attestation verify "oci://$exact_image" \\',
        'docker pull "$exact_image" >/dev/null',
        (
            "docker image inspect --format '{{json .Config.Labels}}' "
            '"$exact_image" > "$labels_path"'
        ),
        'builder_image_version=$(python3 "$SCRIPT_DIR/ci_image_manifest.py" \\',
        'manifest_digest=$(python3 "$SCRIPT_DIR/ci_image_manifest.py" \\',
        'verified_manifest_digest=$(python3 "$SCRIPT_DIR/ci_image_manifest.py" \\',
        '} | tee "$ARTIFACT_DIR/ci-image-identity.env"',
    )
    positions: list[int] = []
    for command in ordered_commands:
        matches = [index for index, line in enumerate(active) if line == command]
        if len(matches) != 1:
            raise ContractError(
                f"{path}: protected resolver command is missing or ambiguous: {command}"
            )
        positions.append(matches[0])
    if positions != sorted(positions) or len(positions) != len(set(positions)):
        raise ContractError(
            f"{path}: exact-subject attestation must precede layer pull and identity output"
        )


def _verify_image_lock(root: Path, actions: dict[str, tuple[str, str]]) -> None:
    """Validate image inputs, offline bootstrap, digests, and builder identity.

    Args:
        root: Repository root containing protected image inputs and workflows.
        actions: Canonical action lock keyed by action name.

    Raises:
        ContractError: The lock schema, bootstrap closure, input inventory,
            publisher workflow, base digest, or builder identity differs.

    Note:
        Bootstrap URLs/hashes are cross-checked against the one package lock and
        Dockerfile; the complete lock file remains part of the image manifest.
    """
    path = root / "ci/locks/ci-image-lock.json"
    lock = _load_json(path)
    if not isinstance(lock, dict):
        raise ContractError(f"{path}: root must be an object")
    _exact_keys(
        lock,
        {
            "schema", "apt_bootstrap", "apt_snapshot", "base_image", "builder",
            "github_cli", "input_paths", "protected_helpers", "published_image",
        },
        str(path),
    )
    if lock["schema"] != "photospider-ci-image-lock-v1":
        raise ContractError(f"{path}: unknown schema")
    if not re.fullmatch(r"[0-9]{8}T[0-9]{6}Z", str(lock["apt_snapshot"])):
        raise ContractError(f"{path}: invalid immutable snapshot ID")
    _verify_apt_bootstrap_lock(root, lock)
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
    required = {
        "Dockerfile.ci",
        ".dockerignore",
        "ci/locks/ubuntu-24.04-snapshot.sources.in",
        str(path.relative_to(root)),
    }
    if "ci/locks/ubuntu-24.04-snapshot.sources.in" not in input_paths:
        raise ContractError(f"{path}: canonical APT source is absent from image inputs")
    if not required.issubset(input_paths):
        raise ContractError(f"{path}: real image inputs are missing")
    for relative in input_paths:
        candidate = root / relative
        if relative.startswith("/") or ".." in Path(relative).parts:
            raise ContractError(f"{path}: unsafe image input {relative!r}")
        if not candidate.is_file() or candidate.is_symlink():
            raise ContractError(f"{path}: image input is not a regular repository file: {relative}")
    _verify_protected_helpers(root, lock, input_paths)

    _verify_ci_image_producer(root, actions)
    _verify_ci_image_resolver_order(root)
    published = lock["published_image"]
    _exact_keys(
        published,
        {
            "builder_image_version_label",
            "input_manifest_label",
            "locator",
            "source_commit_label",
            "source_repository",
            "source_workflow",
        },
        f"{path}:published_image",
    )
    if not str(published["locator"]).endswith(":latest"):
        raise ContractError(f"{path}: published locator must be an explicit locator tag")

    # Active Dockerfile consumption is parsed and verified independently by
    # ``_verify_dockerfile``. Keeping raw-text checks here would let review hints
    # or comments masquerade as executable identities.


class _DockerInstruction(NamedTuple):
    """One active logical Dockerfile instruction and its source location."""

    keyword: str
    arguments: str
    line_number: int


_GO_UNICODE_WHITE_SPACE_SINGLETONS = frozenset(
    {
        0x0009,
        0x000A,
        0x000B,
        0x000C,
        0x000D,
        0x0020,
        0x0085,
        0x00A0,
        0x1680,
        0x2028,
        0x2029,
        0x202F,
        0x205F,
        0x3000,
    }
)


def _go_unicode_is_space(character: str) -> bool:
    """Return whether one character satisfies Go ``unicode.IsSpace``.

    Args:
        character: Exactly one decoded Unicode code point from a strict UTF-8
            Dockerfile parser-directive line.

    Returns:
        ``True`` only for the Unicode White_Space code points recognized by Go:
        the explicit singleton set above or U+2000 through U+200A inclusive.

    Raises:
        ContractError: The caller supplies zero or multiple code points rather
            than the one-character unit required by this predicate.

    Note:
        The explicit set intentionally does not use Python ``str.isspace``.
        Python additionally accepts U+001C through U+001F, while BuildKit calls
        Go ``unicode.IsSpace`` after removing the comment marker at byte zero.
    """
    if len(character) != 1:
        raise ContractError("Go unicode.IsSpace requires exactly one character")
    code_point = ord(character)
    return (
        code_point in _GO_UNICODE_WHITE_SPACE_SINGLETONS
        or 0x2000 <= code_point <= 0x200A
    )


def _go_scan_lines(source: str) -> list[str]:
    """Split decoded text exactly like Go ``bufio.ScanLines``.

    Args:
        source: Strictly decoded Dockerfile text. The caller owns byte decoding
            and any BOM/shebang preprocessing before this physical-line step.

    Returns:
        Physical line tokens split only at LF. One terminal CR is removed from
        each token, and a final LF does not create an additional empty token.
        Empty input produces no tokens.

    Note:
        CR-only, VT, FF, FS/GS/RS/US, NEL, and Unicode line/paragraph separators
        remain inside one token. Python ``str.splitlines`` recognizes those as
        boundaries and therefore cannot model BuildKit's active instruction
        surface safely.
    """
    if not source:
        return []
    lines = source.split("\n")
    if source.endswith("\n"):
        lines.pop()
    return [line[:-1] if line.endswith("\r") else line for line in lines]


def _docker_parser_directive(
    raw_line: str, comment_prefix: str = "#"
) -> tuple[str, str] | None:
    """Decode one Docker-recognized parser-directive comment shape.

    Args:
        raw_line: Physical line observed while Docker is still accepting parser
            directives before any blank line, ordinary comment, or instruction.
        comment_prefix: BuildKit directive comment marker. The maintained
            Dockerfile parser uses ``#``; frontend detection additionally
            probes BuildKit's ``//`` fallback without executing that frontend.

    Returns:
        The ASCII-case-folded directive key and case-preserved nonempty value,
        or ``None`` when the line is an ordinary comment rather than a valid
        ``<comment-prefix> key=value`` parser directive.

    Raises:
        ContractError: ``comment_prefix`` is outside BuildKit's two maintained
            frontend-detection comment forms.

    Note:
        BuildKit requires the comment marker at byte zero, then removes exactly
        the Go ``unicode.IsSpace`` set before applying its ASCII directive
        expression. Marker-leading whitespace is therefore an ordinary comment,
        not an active frontend. The value may carry a frontend command line, so
        detection rejects the whole nonempty value rather than only tag-shaped
        tokens. Callers own the initial directive-phase boundary.
    """
    if comment_prefix not in {"#", "//"}:
        raise ContractError("unsupported Docker parser-directive comment prefix")
    if not raw_line.startswith(comment_prefix):
        return None
    payload = raw_line[len(comment_prefix) :]
    first_non_space = 0
    while first_non_space < len(payload) and _go_unicode_is_space(
        payload[first_non_space]
    ):
        first_non_space += 1
    payload = payload[first_non_space:]
    match = re.fullmatch(
        r"([A-Za-z][A-Za-z0-9]*)[ \t\f\r\n]*="
        r"[ \t\f\r\n]*(.+?)[ \t\f\r\n]*",
        payload,
    )
    if match is None:
        return None
    key, value = match.groups()
    return key.lower(), value


def _buildkit_syntax_directive(source: str) -> tuple[str, int, str] | None:
    """Detect an external frontend using BuildKit's pre-parse precedence.

    Args:
        source: Strict UTF-8 Dockerfile text. A decoded BOM may still be present
            so the detector can model BuildKit before policy rejects the bytes.

    Returns:
        ``(frontend_value, one_based_line, form)`` for the first syntax identity
        BuildKit would select after discarding one UTF-8 BOM and one first-line
        shebang, or ``None`` when no supported frontend form is active.

    Raises:
        ContractError: Internal directive parsing receives an unsupported
            comment prefix. Malformed user text simply is not a syntax identity
            and remains subject to the ordinary restricted Dockerfile parser.

    Note:
        This is the deliberately narrow security-relevant subset of BuildKit's
        ``parser.DetectSyntax``: traditional ``#`` directives, its ``//``
        fallback, and an entire JSON definition with a string ``syntax`` field.
        The verifier never executes or resolves the returned frontend.
    """
    normalized = source.removeprefix("\ufeff")
    line_offset = 0
    first_line, separator, remainder = normalized.partition("\n")
    if first_line.startswith("#!"):
        normalized = remainder if separator else ""
        line_offset = 1
    physical = _go_scan_lines(normalized)
    valid_keys = {"syntax", "escape", "check"}
    for comment_prefix, form in (("#", "hash"), ("//", "c-style")):
        for relative_line, raw_line in enumerate(physical, 1):
            directive = _docker_parser_directive(raw_line, comment_prefix)
            if directive is None:
                break
            key, value = directive
            if key not in valid_keys:
                break
            if key == "syntax":
                return value, line_offset + relative_line, form
    try:
        json_directive = json.loads(normalized)
    except (json.JSONDecodeError, UnicodeError):
        return None
    if isinstance(json_directive, dict) and isinstance(
        json_directive.get("syntax"), str
    ):
        return json_directive["syntax"], line_offset + 1, "json"
    return None


def _dockerfile_instructions(path: Path) -> list[_DockerInstruction]:
    """Parse the maintained Dockerfile into active logical instructions.

    Args:
        path: Dockerfile whose executable structure is protected.

    Returns:
        Ordered active instructions with continuations joined and ordinary
        full-line comments excluded.

    Raises:
        ContractError: Input is unreadable or non-UTF-8, starts with a UTF-8 BOM
            or shebang, selects an external syntax frontend after BuildKit's
            BOM/shebang preprocessing, uses an unsupported/duplicate parser
            directive or non-backslash escape mode, contains an unterminated
            continuation, unsupported instruction, or JSON/heredoc form on a
            security-authoritative instruction.

    Note:
        This is deliberately a restricted Dockerfile parser, not a BuildKit
        frontend. Frontend selection is rejected before active instructions so
        a BOM or shebang cannot hide a directive from this policy. It shares the
        exact Go ``bufio.ScanLines`` physical-line authority with frontend
        detection, so a non-LF separator cannot reveal an instruction that
        BuildKit still treats as part of the preceding token. The production file
        is kept canonical so unsupported syntax fails closed.
    """
    try:
        source_bytes = path.read_bytes()
    except OSError as error:
        raise ContractError(f"cannot read Dockerfile {path}: {error}") from error
    try:
        source = source_bytes.decode("utf-8")
    except UnicodeDecodeError as error:
        raise ContractError(
            f"{path}: Dockerfile is not strict UTF-8: {error}"
        ) from error
    has_bom = source_bytes.startswith(b"\xef\xbb\xbf")
    normalized = source.removeprefix("\ufeff")
    first_line = normalized.partition("\n")[0]
    has_shebang = first_line.startswith("#!")
    syntax_directive = _buildkit_syntax_directive(source)
    if has_bom:
        raise ContractError(
            f"{path}: UTF-8 BOM is forbidden before Dockerfile frontend detection"
        )
    if syntax_directive is not None:
        frontend, line_number, form = syntax_directive
        raise ContractError(
            f"{path}:{line_number}: Docker syntax parser directive is forbidden "
            f"(BuildKit {form} frontend {frontend!r})"
        )
    if has_shebang:
        raise ContractError(f"{path}: first-line Dockerfile shebang is forbidden")
    physical = _go_scan_lines(source)
    instructions: list[_DockerInstruction] = []
    fragments: list[str] = []
    start_line = 0
    directive_phase = True
    seen_directives: set[str] = set()
    allowed = {"ADD", "ARG", "COPY", "ENV", "FROM", "LABEL", "RUN", "WORKDIR"}
    for line_number, raw_line in enumerate(physical, 1):
        if "\x00" in raw_line:
            raise ContractError(f"{path}:{line_number}: unsafe Dockerfile NUL byte")
        stripped = raw_line.strip()
        if not fragments and (not stripped or stripped.startswith("#")):
            if directive_phase:
                directive = _docker_parser_directive(raw_line)
                if directive is None:
                    directive_phase = False
                else:
                    key, value = directive
                    if key in seen_directives:
                        raise ContractError(
                            f"{path}:{line_number}: duplicate Docker parser directive {key}"
                        )
                    seen_directives.add(key)
                    if key == "syntax":
                        raise ContractError(
                            f"{path}:{line_number}: Docker syntax parser directive is forbidden"
                        )
                    if key == "escape" and value != "\\":
                        raise ContractError(
                            f"{path}:{line_number}: unsupported Docker escape directive"
                        )
                    if key == "check":
                        raise ContractError(
                            f"{path}:{line_number}: unsupported Docker check directive"
                        )
                    if key not in {"escape", "check", "syntax"}:
                        # Docker treats an unknown directive-shaped line as an
                        # ordinary comment and stops looking for directives.
                        directive_phase = False
            continue
        if fragments and (not stripped or stripped.startswith("#")):
            # Docker ignores full-line comments and blank lines inside a
            # continued instruction; they cannot satisfy the active contract.
            continue
        directive_phase = False
        if "\t" in raw_line[: len(raw_line) - len(raw_line.lstrip())]:
            raise ContractError(f"{path}:{line_number}: unsafe Dockerfile whitespace")
        if not fragments:
            start_line = line_number
        continued = raw_line.rstrip().endswith("\\")
        fragment = raw_line.rstrip()
        if continued:
            fragment = fragment[:-1]
        fragments.append(fragment.strip())
        if continued:
            continue
        logical = " ".join(fragment for fragment in fragments if fragment)
        fragments = []
        match = re.fullmatch(r"([A-Za-z]+)[ \t]+(.+)", logical)
        if match is None:
            raise ContractError(f"{path}:{start_line}: malformed Docker instruction")
        keyword, arguments = match.groups()
        keyword = keyword.upper()
        if keyword not in allowed:
            raise ContractError(f"{path}:{start_line}: unsupported Docker instruction {keyword}")
        if keyword in {"ADD", "ARG", "COPY", "FROM", "RUN"}:
            if arguments.lstrip().startswith("[") or "<<" in arguments:
                raise ContractError(
                    f"{path}:{start_line}: unsupported {keyword} JSON/heredoc form"
                )
        instructions.append(_DockerInstruction(keyword, arguments, start_line))
    if fragments:
        raise ContractError(f"{path}:{start_line}: unterminated Docker continuation")
    if not instructions:
        raise ContractError(f"{path}: Dockerfile has no active instructions")
    return instructions


def _docker_words(path: Path, instruction: _DockerInstruction) -> list[str]:
    """Split one canonical Docker instruction argument vector safely."""
    try:
        return shlex.split(instruction.arguments, comments=False, posix=True)
    except ValueError as error:
        raise ContractError(
            f"{path}:{instruction.line_number}: malformed {instruction.keyword} arguments"
        ) from error


def _verify_snapshot_source_template(root: Path, snapshot: str) -> None:
    """Require one canonical signed snapshot source for both Linux architectures.

    Args:
        root: Repository root containing the protected Deb822 template.
        snapshot: Exact lock timestamp substituted during the image build.

    Raises:
        ContractError: The template is aliased, malformed, duplicated, contains
            another source, or does not bind the Canonical snapshot authority.

    Note:
        Docker overwrites the locked base image's ``ubuntu.sources`` with this
        template, so archive, security, and ports source families cannot retain
        architecture-specific live behavior.
    """
    path = root / "ci/locks/ubuntu-24.04-snapshot.sources.in"
    try:
        mode = path.lstat().st_mode
        lines = path.read_text(encoding="utf-8").splitlines()
    except (OSError, UnicodeError) as error:
        raise ContractError(f"cannot read protected APT source {path}: {error}") from error
    if stat.S_ISLNK(mode) or not stat.S_ISREG(mode):
        raise ContractError(f"{path}: protected APT source must be a regular file")
    fields: dict[str, str] = {}
    for line_number, line in enumerate(lines, 1):
        if not line or line.startswith("#"):
            continue
        match = re.fullmatch(r"([A-Za-z][A-Za-z-]*):[ ]+([^\s].*)", line)
        if match is None:
            raise ContractError(f"{path}:{line_number}: malformed Deb822 source field")
        key, value = match.groups()
        if key in fields:
            raise ContractError(f"{path}:{line_number}: duplicate Deb822 source field {key}")
        fields[key] = value
    expected = {
        "Types": "deb",
        "URIs": "https://snapshot.ubuntu.com/ubuntu/@APT_SNAPSHOT@/",
        "Suites": "noble noble-updates noble-security",
        "Components": "main restricted universe multiverse",
        "Signed-By": "/usr/share/keyrings/ubuntu-archive-keyring.gpg",
    }
    if fields != expected:
        raise ContractError(f"{path}: canonical signed snapshot source differs")
    if not re.fullmatch(r"[0-9]{8}T[0-9]{6}Z", snapshot):
        raise ContractError(f"{path}: APT snapshot substitution is malformed")


def _verify_dockerfile(root: Path) -> None:
    """Check semantic Docker consumption of immutable image/dependency locks.

    Args:
        root: Repository root containing Dockerfile and protected locks.

    Raises:
        ContractError: Active FROM/ARG/COPY/ADD/RUN structure drifts, a remote
            input lacks its exact checksum, or APT can execute before/beyond the
            single signed snapshot source and package transaction.

    Note:
        Full-line Docker comments and shell comments never participate. A valid
        initial ``syntax`` parser directive is rejected before instruction
        parsing; unsupported syntax fails closed instead of falling back to
        raw-text count/index logic.
    """
    path = root / "Dockerfile.ci"
    instructions = _dockerfile_instructions(path)
    image_lock = _load_json(root / "ci/locks/ci-image-lock.json")
    if not isinstance(image_lock, dict):
        raise ContractError("CI image lock root must be an object")
    bootstrap_clauses = _verify_apt_bootstrap_lock(root, image_lock)
    snapshot = image_lock.get("apt_snapshot")
    if not isinstance(snapshot, str):
        raise ContractError("CI image lock APT snapshot is absent")
    _verify_snapshot_source_template(root, snapshot)
    input_paths = image_lock.get("input_paths")
    if not isinstance(input_paths, list) or not all(
        isinstance(item, str) for item in input_paths
    ):
        raise ContractError("CI image lock input paths are malformed")
    _verify_protected_helpers(root, image_lock, input_paths)

    expected_from = (
        f"{image_lock['base_image']['name'].removeprefix('docker.io/library/')}:"
        f"{image_lock['base_image']['tag']}@{image_lock['base_image']['digest']}"
    )
    expected_arguments = (
        f"APT_SNAPSHOT={snapshot}",
        f"GH_CLI_VERSION={image_lock['github_cli']['version']}",
        f"GH_CLI_AMD64_SHA256={image_lock['github_cli']['amd64_sha256']}",
        f"GH_CLI_ARM64_SHA256={image_lock['github_cli']['arm64_sha256']}",
        "CI_IMAGE_INPUT_MANIFEST_SHA256",
        "CI_IMAGE_SOURCE_COMMIT",
    )
    expected_copies = (
        "ci/locks/ubuntu-24.04-packages.lock /tmp/ci-locks/ubuntu-24.04-packages.lock",
        "ci/locks/ubuntu-24.04-snapshot.sources.in /tmp/ci-locks/ubuntu-24.04-snapshot.sources.in",
        "ci/locks/requirements-ci.txt /tmp/ci-locks/requirements-ci.txt",
        "ci/scripts/ci_image_install.sh /tmp/ci-image-install.sh",
    )
    expected_adds = tuple(clause.removeprefix("ADD ") for clause in bootstrap_clauses)
    expected_instructions = [
        ("FROM", expected_from),
        *(("ARG", argument) for argument in expected_arguments),
        (
            "LABEL",
            'org.opencontainers.image.revision="$CI_IMAGE_SOURCE_COMMIT" '
            'org.photospider.ci.input-manifest-sha256="$CI_IMAGE_INPUT_MANIFEST_SHA256"',
        ),
        ("ENV", "DEBIAN_FRONTEND=noninteractive"),
        ("ENV", "VENV=/opt/venv"),
        ("ENV", 'PATH="$VENV/bin:$PATH"'),
        *(("COPY", copy) for copy in expected_copies),
        *(("ADD", add) for add in expected_adds),
        ("RUN", "bash /tmp/ci-image-install.sh"),
        ("WORKDIR", "/workspace"),
    ]
    actual_instructions = [
        (instruction.keyword, instruction.arguments) for instruction in instructions
    ]
    if actual_instructions != expected_instructions:
        mismatch = next(
            (
                index
                for index, (actual, expected) in enumerate(
                    zip(actual_instructions, expected_instructions)
                )
                if actual != expected
            ),
            min(len(actual_instructions), len(expected_instructions)),
        )
        raise ContractError(
            f"{path}: canonical active instruction stream differs at index {mismatch}"
        )

    remote_adds: list[tuple[str, str, str]] = []
    for instruction in instructions:
        if instruction.keyword != "ADD":
            continue
        words = _docker_words(path, instruction)
        if len(words) != 3:
            raise ContractError(f"{path}: remote ADD shape differs")
        checksum, url, destination = words
        if re.fullmatch(r"--checksum=sha256:[0-9a-f]{64}", checksum) is None:
            raise ContractError(f"{path}: remote ADD lacks exact SHA-256")
        if re.fullmatch(r"https://snapshot\.ubuntu\.com/ubuntu/[A-Za-z0-9_./~+-]+", url) is None:
            raise ContractError(f"{path}: remote ADD authority differs")
        remote_adds.append((checksum, url, destination))
    expected_remote_adds = [
        tuple(shlex.split(clause, comments=False, posix=True)[1:])
        for clause in bootstrap_clauses
    ]
    if remote_adds != expected_remote_adds:
        raise ContractError(f"{path}: remote ADD set/order differs from the protected lock")


def _verify_darwin_lock(root: Path) -> None:
    """Validate the exact finite Darwin rollout set and vcpkg mappings.

    Args:
        root: Repository root containing the protected Darwin runner lock.

    Raises:
        ContractError: The schema, shared arm64 identity, triplet, ordered
            version set, or one-to-one vcpkg mapping differs from review.

    Note:
        Workflow consumption is validated separately by
        ``_verify_darwin_security_dag`` so lock and scheduling failures remain
        independently diagnosable.
    """
    path = root / "ci/locks/darwin-runner-lock.json"
    try:
        lock = load_runner_lock(root, "Darwin")
    except RunnerError as error:
        raise ContractError(f"{path}: {error}") from error
    exact_values: dict[str, Any] = {
        "approved_images": [
            {
                "image_version": "20260727.0256.1",
                "vcpkg_commit": "6d9d7df564a1ccdaa994e4ad39ccd4a32360867b",
            },
            {
                "image_version": "20260824.0311.1",
                "vcpkg_commit": "127402f1c75bb3d5ff6bce04b285faa4930a5aca",
            },
        ],
        "schema": "photospider-darwin-runner-lock-v2",
        "architecture": "arm64",
        "image_os": "macos15",
        "runner_label": "macos-15",
        "triplet": "arm64-osx",
    }
    for field, expected_value in exact_values.items():
        if lock[field] != expected_value:
            raise ContractError(f"{path}: unexpected {field} identity")


def _verify_darwin_security_dag(root: Path) -> None:
    """Require three independently scheduled Darwin security profile jobs.

    Args:
        root: Repository root containing the protected shared integration DAG.

    Raises:
        ContractError: A profile is missing, serially loops, depends on a sibling,
            changes its runner/profile/artifact identity, or escapes suite-gate
            aggregation.

    Note:
        Each job depends only on ``integration-plan``. Therefore one profile's
        failure cannot prevent either sibling from being scheduled, while the
        stable suite gate still requires all three conclusions to be successful.
    """
    path = root / ".github/workflows/ci-integration-suite.yml"
    profiles = {
        "sanitizer-asan-darwin": {
            "timeout": "90",
            "verify_step": "Verify exact Darwin ASan runner",
            "run_step": "Run isolated Darwin ASan profile",
            "run": "bash ci/scripts/sanitizer_test.sh",
            "env": {
                "CI_ARTIFACT_DIR": "${{ github.workspace }}/CI-results/sanitizer-asan-darwin",
                "CI_INVENTORY_DIR": "${{ github.workspace }}/CI-results/profile-inventory",
                "CI_RUNNER_IDENTITY_FILE": "${{ runner.temp }}/photospider-darwin-asan-runner-${{ github.run_id }}-${{ github.run_attempt }}.json",
                "CI_RUNNER_TEMP": "${{ runner.temp }}",
                "SANITIZER": "asan",
            },
            "upload_step": "Upload Darwin ASan diagnostics",
            "artifact": "sanitizer-asan-darwin-results",
            "artifact_path": "CI-results/sanitizer-asan-darwin",
            "result": "CI_DARWIN_ASAN_RESULT",
        },
        "sanitizer-tsan-darwin": {
            "timeout": "90",
            "verify_step": "Verify exact Darwin TSan runner",
            "run_step": "Run isolated Darwin TSan profile",
            "run": "bash ci/scripts/sanitizer_test.sh",
            "env": {
                "CI_ARTIFACT_DIR": "${{ github.workspace }}/CI-results/sanitizer-tsan-darwin",
                "CI_INVENTORY_DIR": "${{ github.workspace }}/CI-results/profile-inventory",
                "CI_RUNNER_IDENTITY_FILE": "${{ runner.temp }}/photospider-darwin-tsan-runner-${{ github.run_id }}-${{ github.run_attempt }}.json",
                "CI_RUNNER_TEMP": "${{ runner.temp }}",
                "SANITIZER": "tsan",
            },
            "upload_step": "Upload Darwin TSan diagnostics",
            "artifact": "sanitizer-tsan-darwin-results",
            "artifact_path": "CI-results/sanitizer-tsan-darwin",
            "result": "CI_DARWIN_TSAN_RESULT",
        },
        "fuzz-codecs-darwin": {
            "timeout": "30",
            "verify_step": "Verify exact Darwin fuzz runner",
            "run_step": "Run bounded Darwin codec fuzz smoke",
            "run": "bash ci/scripts/fuzz_smoke.sh",
            "env": {
                "CI_ARTIFACT_DIR": "${{ github.workspace }}/CI-results/fuzz-codecs-darwin",
                "CI_INVENTORY_DIR": "${{ github.workspace }}/CI-results/profile-inventory",
                "CI_RUNNER_IDENTITY_FILE": "${{ runner.temp }}/photospider-darwin-fuzz-runner-${{ github.run_id }}-${{ github.run_attempt }}.json",
                "CI_RUNNER_TEMP": "${{ runner.temp }}",
            },
            "upload_step": "Upload transient Darwin fuzz diagnostics",
            "artifact": "fuzz-codecs-darwin-results",
            "artifact_path": "CI-results/fuzz-codecs-darwin",
            "result": "CI_DARWIN_FUZZ_RESULT",
        },
    }
    requested_jobs = [*profiles, "suite-gate"]
    jobs = _workflow_job_mappings(path, requested_jobs)
    raw_job_inventory = _workflow_job_blocks(path)
    if "security-darwin" in raw_job_inventory:
        raise ContractError(f"{path}: serial Darwin security aggregate remains")

    actions = _read_actions(root / "ci/locks/actions.lock")

    def action_reference(name: str) -> str:
        """Return one exact locked action reference for expected step mappings."""
        if name not in actions:
            raise ContractError(f"{path}: protected Darwin action {name!r} is absent")
        return f"{name}@{actions[name][1]}"

    checkout_action = action_reference("actions/checkout")
    download_action = action_reference("actions/download-artifact")
    upload_action = action_reference("actions/upload-artifact")

    for job_name, profile in profiles.items():
        job = jobs[job_name]
        expected_verify = (
            "python3 ci/scripts/ci_runner_verify.py --platform Darwin "
            "--runner-label macos-15 --output \"$CI_RUNNER_IDENTITY_FILE\""
        )
        identity_path = profile["env"]["CI_RUNNER_IDENTITY_FILE"]
        expected_job = {
            "permissions": {"contents": "read"},
            "needs": "integration-plan",
            "runs-on": "macos-15",
            "timeout-minutes": profile["timeout"],
            "steps": [
                {
                    "uses": checkout_action,
                    "with": {
                        "persist-credentials": "false",
                        "repository": "${{ inputs.checkout_repository }}",
                        "fetch-depth": "0",
                        "submodules": "recursive",
                        "ref": "${{ inputs.checkout_ref }}",
                    },
                },
                {
                    "name": profile["verify_step"],
                    "env": {"CI_RUNNER_IDENTITY_FILE": identity_path},
                    "run": expected_verify,
                },
                {
                    "name": "Download security profile inventory",
                    "uses": download_action,
                    "with": {
                        "name": "ci-security-profile-inventory",
                        "path": "CI-results/profile-inventory",
                    },
                },
                {
                    "name": profile["run_step"],
                    "env": profile["env"],
                    "run": profile["run"],
                },
                {
                    "name": profile["upload_step"],
                    "if": "always()",
                    "uses": upload_action,
                    "with": {
                        "name": profile["artifact"],
                        "path": profile["artifact_path"],
                        "if-no-files-found": "warn",
                        "retention-days": "3",
                    },
                },
            ],
        }
        if job != expected_job:
            raise ContractError(f"{path}: {job_name} complete job mapping differs")

    suite_name = "suite-gate"
    suite = jobs[suite_name]
    suite_needs = [
        "identity-preflight",
        "integration-plan",
        "build-integrity-default",
        "verify-targeted-artifacts",
        "attest-targeted-artifacts",
        "targeted-artifacts-ready",
        "full-ctest",
        "build-smoke",
        "openexr-smoke",
        "scripted-cli",
        "propagation-script",
        "plugin-load",
        "execution-repeat",
        "installed-package-consumer",
        "sanitizer-asan",
        "sanitizer-tsan",
        "fuzz-codecs",
        "sanitizer-asan-darwin",
        "sanitizer-tsan-darwin",
        "fuzz-codecs-darwin",
    ]
    result_variables = {
        "identity-preflight": "CI_IDENTITY_RESULT",
        "integration-plan": "CI_PLAN_RESULT",
        "build-integrity-default": "CI_BUILD_RESULT",
        "verify-targeted-artifacts": "CI_VERIFY_RESULT",
        "targeted-artifacts-ready": "CI_ARTIFACT_READY_RESULT",
        "full-ctest": "CI_CTEST_RESULT",
        "build-smoke": "CI_BUILD_SMOKE_RESULT",
        "openexr-smoke": "CI_OPENEXR_RESULT",
        "scripted-cli": "CI_SCRIPTED_CLI_RESULT",
        "propagation-script": "CI_PROPAGATION_RESULT",
        "plugin-load": "CI_PLUGIN_RESULT",
        "execution-repeat": "CI_EXECUTION_RESULT",
        "installed-package-consumer": "CI_INSTALLED_PACKAGE_RESULT",
        "sanitizer-asan": "CI_ASAN_RESULT",
        "sanitizer-tsan": "CI_TSAN_RESULT",
        "fuzz-codecs": "CI_FUZZ_RESULT",
        "sanitizer-asan-darwin": "CI_DARWIN_ASAN_RESULT",
        "sanitizer-tsan-darwin": "CI_DARWIN_TSAN_RESULT",
        "fuzz-codecs-darwin": "CI_DARWIN_FUZZ_RESULT",
    }
    suite_environment = {
        variable: f"${{{{ needs.{job_name}.result }}}}"
        for job_name, variable in result_variables.items()
    }
    suite_environment.update(
        {
            "CI_ATTESTATION_RESULT": "${{ needs.attest-targeted-artifacts.result }}",
            "CI_IMAGE_DIGEST": "${{ needs.identity-preflight.outputs.image_digest }}",
            "CI_PUBLISH_REUSABLE_ATTESTATIONS": "${{ inputs.publish_reusable_attestations }}",
        }
    )
    expected_suite = {
        "permissions": {"contents": "read"},
        "needs": suite_needs,
        "if": "always()",
        "runs-on": "ubuntu-24.04",
        "outputs": {
            "validated_image_digest": "${{ steps.aggregate.outputs.validated_image_digest }}"
        },
        "steps": [
            {
                "name": "Checkout protected suite gate control",
                "uses": checkout_action,
                "with": {
                    "persist-credentials": "false",
                    "repository": "${{ github.repository }}",
                    "fetch-depth": "1",
                    "ref": "${{ github.workflow_sha }}",
                    "sparse-checkout": "ci/scripts/integration_suite_gate.py",
                    "sparse-checkout-cone-mode": "false",
                    "path": ".ci-suite-gate-control",
                },
            },
            {
                "name": "Aggregate the exact shared integration DAG",
                "id": "aggregate",
                "env": suite_environment,
                "run": (
                    "python3 .ci-suite-gate-control/ci/scripts/integration_suite_gate.py "
                    '--output "$GITHUB_OUTPUT"'
                ),
            },
        ],
    }
    if suite != expected_suite:
        raise ContractError(f"{path}: suite-gate complete job mapping differs")


def _verify_linux_runner_lock(root: Path) -> None:
    """Validate the finite Linux rollout set and image-manifest coverage."""
    path = root / "ci/locks/linux-runner-lock.json"
    try:
        lock = load_runner_lock(root, "Linux")
    except RunnerError as error:
        raise ContractError(f"{path}: {error}") from error
    exact_values: dict[str, Any] = {
        "approved_image_versions": [
            "20260816.277.1",
            "20260823.283.1",
        ],
        "schema": "photospider-linux-runner-lock-v2",
        "architecture": "x86_64",
        "image_os": "ubuntu24",
        "runner_label": "ubuntu-24.04",
    }
    for field, expected_value in exact_values.items():
        if lock[field] != expected_value:
            raise ContractError(f"{path}: unexpected {field} identity")
    image_lock = _load_json(root / "ci/locks/ci-image-lock.json")
    if "ci/locks/linux-runner-lock.json" not in image_lock.get("input_paths", []):
        raise ContractError("Linux builder identity is absent from canonical image inputs")


def _verify_runner_identity_handoffs(root: Path) -> None:
    """Require one retained runner record to cross each security job boundary.

    Args:
        root: Repository root containing maintained workflows and platform reader.

    Raises:
        ContractError: A Linux security job does not create its resolved record
            before profile data, changes paths between producer and consumer,
            accepts candidate inputs, or the platform reader reinterprets the
            mutable runner environment.

    Note:
        Darwin jobs are additionally covered by complete mapping equality. This
        focused check covers the three containerized Linux jobs, the manual
        sanitizer, and the local-image manifest producer without duplicating
        their unrelated workflow fields.
    """
    suite_path = root / ".github/workflows/ci-integration-suite.yml"
    suite_jobs = _workflow_job_mappings(
        suite_path, ("sanitizer-asan", "sanitizer-tsan", "fuzz-codecs")
    )
    suite_contract = {
        "sanitizer-asan": (
            "Verify exact Linux ASan runner",
            "Run isolated ASan profile",
            "${{ runner.temp }}/photospider-linux-asan-runner-${{ github.run_id }}-${{ github.run_attempt }}.json",
        ),
        "sanitizer-tsan": (
            "Verify exact Linux TSan runner",
            "Run isolated TSan profile",
            "${{ runner.temp }}/photospider-linux-tsan-runner-${{ github.run_id }}-${{ github.run_attempt }}.json",
        ),
        "fuzz-codecs": (
            "Verify exact Linux fuzz runner",
            "Run bounded codec fuzz smoke",
            "${{ runner.temp }}/photospider-linux-fuzz-runner-${{ github.run_id }}-${{ github.run_attempt }}.json",
        ),
    }

    def require_handoff(
        path: Path,
        job_name: str,
        job: dict[str, Any],
        verify_name: str,
        execute_name: str,
        identity_path: str,
    ) -> None:
        """Validate one ordered producer/consumer pair inside a parsed job."""
        steps = job.get("steps")
        if not isinstance(steps, list) or not all(isinstance(step, dict) for step in steps):
            raise ContractError(f"{path}: {job_name} steps are malformed")
        verify_matches = [
            (index, step) for index, step in enumerate(steps) if step.get("name") == verify_name
        ]
        execute_matches = [
            (index, step) for index, step in enumerate(steps) if step.get("name") == execute_name
        ]
        if len(verify_matches) != 1 or len(execute_matches) != 1:
            raise ContractError(f"{path}: {job_name} runner handoff steps are ambiguous")
        verify_index, verify_step = verify_matches[0]
        execute_index, execute_step = execute_matches[0]
        expected_run = (
            "python3 ci/scripts/ci_runner_verify.py --platform Linux "
            "--runner-label ubuntu-24.04 --output \"$CI_RUNNER_IDENTITY_FILE\""
        )
        if verify_step.get("env") != {"CI_RUNNER_IDENTITY_FILE": identity_path}:
            raise ContractError(f"{path}: {job_name} resolved runner output differs")
        if verify_step.get("run") != expected_run:
            raise ContractError(f"{path}: {job_name} runner verifier command differs")
        execute_environment = execute_step.get("env")
        if (
            not isinstance(execute_environment, dict)
            or execute_environment.get("CI_RUNNER_IDENTITY_FILE") != identity_path
        ):
            raise ContractError(f"{path}: {job_name} retained runner consumer differs")
        if "${{ inputs." in identity_path or verify_index >= execute_index:
            raise ContractError(f"{path}: {job_name} runner identity order/trust differs")

    for job_name, (verify_name, execute_name, identity_path) in suite_contract.items():
        require_handoff(
            suite_path,
            job_name,
            suite_jobs[job_name],
            verify_name,
            execute_name,
            identity_path,
        )

    manual_path = root / ".github/workflows/ci-sanitizer.yml"
    manual = _workflow_job_mappings(manual_path, ("sanitizer",))["sanitizer"]
    require_handoff(
        manual_path,
        "sanitizer",
        manual,
        "Verify exact Linux sanitizer runner",
        "Run sanitizer tests",
        "${{ runner.temp }}/photospider-manual-sanitizer-runner-${{ github.run_id }}-${{ github.run_attempt }}.json",
    )

    health_path = root / ".github/workflows/ci-healthcheck.yml"
    health = _workflow_job_mappings(health_path, ("healthcheck-local-image",))[
        "healthcheck-local-image"
    ]
    manifest_steps = [
        step
        for step in health.get("steps", [])
        if isinstance(step, dict)
        and step.get("name") == "Verify image-input contract without rebuilding"
    ]
    if len(manifest_steps) != 1:
        raise ContractError(f"{health_path}: local image identity producer is ambiguous")
    manifest_step = manifest_steps[0]
    expected_health_path = (
        "${{ runner.temp }}/photospider-healthcheck-runner-"
        "${{ github.run_id }}-${{ github.run_attempt }}.json"
    )
    environment = manifest_step.get("env")
    command = manifest_step.get("run")
    if (
        not isinstance(environment, dict)
        or environment.get("CI_RUNNER_IDENTITY_FILE") != expected_health_path
        or not isinstance(command, str)
        or command.count("ci_runner_verify.py") != 1
        or '--output "$CI_RUNNER_IDENTITY_FILE"' not in command
        or '--builder-runner-identity "$CI_RUNNER_IDENTITY_FILE"' not in command
        or command.index("ci_runner_verify.py")
        >= command.index('--builder-runner-identity "$CI_RUNNER_IDENTITY_FILE"')
    ):
        raise ContractError(f"{health_path}: local manifest runner provenance differs")

    platform_reader = (root / "ci/scripts/security_platform_prepare.sh").read_text(
        encoding="utf-8"
    )
    if (
        "load_resolved_identity" not in platform_reader
        or "CI_RUNNER_IDENTITY_FILE" not in platform_reader
        or "ImageOS" in platform_reader
        or "ImageVersion" in platform_reader
    ):
        raise ContractError("security platform reader reinterprets mutable runner state")


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


def _verify_shared_integration_dag(root: Path) -> None:
    """Require build-once routing and role-minimal shared integration artifacts.

    Args:
        root: Repository root containing protected workflows, scripts, and lock.

    Raises:
        ContractError: Ordinary/candidate callers diverge, image production is
            duplicated, promotion can rebuild or bypass suite success, the
            serial aggregate remains, or artifact/parallelism boundaries drift.
    """
    integration_path = root / ".github/workflows/ci-integration.yml"
    shared_path = root / ".github/workflows/ci-integration-suite.yml"
    healthcheck_path = root / ".github/workflows/ci-healthcheck.yml"
    integration = integration_path.read_text(encoding="utf-8")
    shared = shared_path.read_text(encoding="utf-8")
    healthcheck = healthcheck_path.read_text(encoding="utf-8")
    if integration.count("uses: ./.github/workflows/build-ci-image.yml") != 1:
        raise ContractError(f"{integration_path}: candidate builder call is not unique")
    if integration.count("uses: ./.github/workflows/ci-integration-suite.yml") != 3:
        raise ContractError(
            f"{integration_path}: published/read-only, published/trusted, and candidate routes must share one workflow"
        )
    required_integration = (
        "needs: [candidate-image-build, candidate-image-integration]",
        "needs.candidate-image-integration.outputs.validated_image_digest",
        "bash ci/scripts/ci_image_promote.sh",
        "packages: write",
        "github.event_name == 'push'",
    )
    if any(fragment not in integration for fragment in required_integration):
        raise ContractError(f"{integration_path}: same-digest promotion contract differs")
    if "integration_suite.sh" in integration or "docker build" in healthcheck:
        raise ContractError("serial integration or duplicate healthcheck image build remains")
    forbidden_shared = (
        "ci-build-default",
        "reusable_build_consume.sh",
        "integration_suite.sh",
    )
    if any(fragment in shared for fragment in forbidden_shared):
        raise ContractError(f"{shared_path}: coarse reusable artifact path remains")
    required_shared = (
        "workflow_call:",
        "image_ref:",
        "image_digest:",
        "candidate_commit:",
        "ci-control-default",
        "ci-runtime-default",
        "ci-installed-package-default",
        "ci-openexr-metadata-default",
        "targeted_artifact_consume.sh",
        "static_product_consumer_test.sh",
        "installed-package-consumer:",
        "openexr-smoke:",
        "sanitizer-asan-darwin:",
        "sanitizer-tsan-darwin:",
        "fuzz-codecs-darwin:",
        "suite-gate:",
        "integration_suite_gate.py",
    )
    if any(fragment not in shared for fragment in required_shared):
        raise ContractError(f"{shared_path}: targeted shared DAG contract differs")

    identity_block = shared.split("  identity-preflight:\n", 1)[1].split(
        "\n  integration-plan:\n", 1
    )[0]
    identity_contract = (
        "repository: ${{ github.repository }}",
        "ref: ${{ inputs.workflow_commit }}",
        "path: .ci-protected-control",
        "CI_EXPECTED_WORKFLOW_COMMIT: ${{ github.workflow_sha }}",
        "CI_IMAGE_EXPECTED_DIGEST: ${{ inputs.image_digest }}",
        "CI_IMAGE_EXPECTED_MANIFEST_DIGEST: ${{ inputs.image_manifest_digest }}",
        "CI_IMAGE_EXPECTED_SOURCE_COMMIT: ${{ inputs.image_source_commit }}",
        "CI_IMAGE_EXPECTED_WORKFLOW_COMMIT: ${{ inputs.workflow_commit }}",
        "bash .ci-protected-control/ci/scripts/ci_image_verify.sh",
    )
    if any(fragment not in identity_block for fragment in identity_contract):
        raise ContractError(
            f"{shared_path}: called-workflow image identity preflight differs"
        )
    if "repository: ${{ inputs.checkout_repository }}" in identity_block:
        raise ContractError(
            f"{shared_path}: identity preflight checks out candidate repository code"
        )
    checkout_index = identity_block.find("Checkout protected reusable workflow control")
    login_index = identity_block.find("Authenticate exact GHCR identity reader")
    verify_index = identity_block.find(
        "bash .ci-protected-control/ci/scripts/ci_image_verify.sh"
    )
    if checkout_index < 0 or login_index < checkout_index or verify_index < login_index:
        raise ContractError(
            f"{shared_path}: protected image verification order is incomplete"
        )
    if (
        "path: |\n            CI-results/installed-package-consumer"
        in shared
    ):
        raise ContractError(
            f"{shared_path}: installed evidence re-uploads the restored package tree"
        )

    targeted_consumer = (root / "ci/scripts/targeted_artifact_consume.sh").read_text(
        encoding="utf-8"
    )
    attestation_contract = (
        '--source-digest "$CI_CANDIDATE_COMMIT"',
        '--signer-digest "$CI_WORKFLOW_COMMIT"',
        "$GITHUB_REPOSITORY/.github/workflows/ci-integration-suite.yml",
        "snapshot-targeted-manifest",
        '--expected-manifest-sha256 "$manifest_snapshot_digest"',
        "CI_STATIC_PACKAGE_MANIFEST_SHA256",
    )
    if any(fragment not in targeted_consumer for fragment in attestation_contract):
        raise ContractError(
            "targeted artifact source/signer attestation identity differs"
        )
    if '--source-digest "$CI_WORKFLOW_COMMIT"' in targeted_consumer:
        raise ContractError("workflow commit is incorrectly used as artifact source")

    common = (root / "ci/scripts/common.sh").read_text(encoding="utf-8")
    ctest = (root / "ci/scripts/ctest_full.sh").read_text(encoding="utf-8")
    build = (root / "ci/scripts/build_integrity.sh").read_text(encoding="utf-8")
    reusable = (root / "ci/scripts/reusable_build.py").read_text(encoding="utf-8")
    if "export CMAKE_BUILD_PARALLEL_LEVEL=$CI_JOBS" not in common:
        raise ContractError("common CI boundary does not export the nested CMake job limit")
    for fragment in ('--parallel "$CI_JOBS"', '--output-junit "$CI_ARTIFACT_DIR/ctest-full.junit.xml"'):
        if fragment not in ctest:
            raise ContractError(f"ordinary CTest contract differs at {fragment!r}")
    required_index = build.find("ensure_ci_targets build_required_targets")
    all_index = build.find("ensure_ci_all build_all")
    route_index = build.find("build_smoke_route.py")
    if required_index < 0 or all_index < required_index or route_index < all_index:
        raise ContractError("producer phases no longer reuse one sequential build tree")
    closure_contract = (
        "ordinary_ctest_closure_v1.json",
        "ctest_runtime_closure.py",
        "dedicated_build_smoke_matrix",
        "openexr_build_smoke_matrix",
    )
    if any(fragment not in build for fragment in closure_contract):
        raise ContractError("producer does not emit complete role/CTest closure routing")
    package_contract = (
        "producer/CMakeCache.txt",
        "producer/generated/ci_inventory/",
        "installable_public_headers.txt",
    )
    if any(fragment not in reusable for fragment in package_contract):
        raise ContractError("installed-package producer metadata contract differs")
    openexr_contract = (
        '"openexr-metadata"',
        'PurePosixPath("producer/CMakeCache.txt")',
        "verify-targeted-tree",
    )
    if any(fragment not in reusable for fragment in openexr_contract):
        raise ContractError("OpenEXR/package targeted role contract differs")
    static_runner = (root / "ci/scripts/static_product_consumer_test.sh").read_text(
        encoding="utf-8"
    )
    for fragment in (
        "verify_package_content before",
        "verify_package_content after",
        "--content-root \"$PACKAGE_ROOT\"",
        "ATTESTED_MANIFEST_SHA256",
        '--expected-manifest-sha256 "$ATTESTED_MANIFEST_SHA256"',
    ):
        if fragment not in static_runner:
            raise ContractError(
                "installed-package before/after content verification differs"
            )
    if "mapfile" in static_runner:
        raise ContractError(
            "installed-package runner requires unavailable Darwin Bash mapfile"
        )
    openexr_runner = (root / "ci/scripts/openexr_smoke_test.sh").read_text(
        encoding="utf-8"
    )
    for fragment in (
        "CMAKE_BUILD_TYPE",
        "CMAKE_CONFIGURATION_TYPES",
        'build_configuration=$(require_cached_build_configuration)',
        '--config "$build_configuration"',
    ):
        if fragment not in openexr_runner:
            raise ContractError(
                "OpenEXR metadata runner does not bind producer configuration"
            )
    if '${CMAKE_BUILD_TYPE:-' in openexr_runner:
        raise ContractError(
            "OpenEXR metadata runner permits environment configuration override"
        )

    routing_path = root / "ci/locks/build-smoke-routing.json"
    routing = _load_json(routing_path)
    if not isinstance(routing, dict) or set(routing) != {
        "dedicated_consumer_roles",
        "default_consumer_role",
        "producer_tests",
        "schema",
    }:
        raise ContractError(f"{routing_path}: routing fields differ")
    if routing["schema"] != "photospider-build-smoke-routing-v3":
        raise ContractError(f"{routing_path}: routing schema is unknown")
    if routing["default_consumer_role"] != "ctest-control":
        raise ContractError(f"{routing_path}: default artifact role differs")
    _require_sorted_unique(routing["producer_tests"], f"{routing_path}:producer_tests")
    if routing["producer_tests"] != ["PublicHeaderSelfContainment"]:
        raise ContractError(f"{routing_path}: producer-local role differs")
    dedicated = routing["dedicated_consumer_roles"]
    if dedicated != {
        "OpenExrDeepProviderOptionOffSmoke": "openexr-metadata",
        "StaticProductConsumerSmoke": "installed-package",
    }:
        raise ContractError(f"{routing_path}: dedicated consumer roles differ")


def verify(root: Path) -> None:
    """Run the complete protected lock and active-consumer validation.

    Args:
        root: Exact repository root whose protected surface is authoritative.

    Raises:
        ContractError: Any lock, workflow, trust, permission, snapshot, runner,
            profile-DAG, or active-consumer contract fails closed.

    Note:
        Validation is read-only and performs no network resolution; the real
        Docker build remains the same-snapshot transitive dependency solver.
    """
    actions = _read_actions(root / "ci/locks/actions.lock")
    _verify_packages(root / "ci/locks/ubuntu-24.04-packages.lock")
    _verify_requirements(root / "ci/locks/requirements-ci.txt")
    _verify_image_lock(root, actions)
    _verify_dockerfile(root)
    _verify_darwin_lock(root)
    _verify_darwin_security_dag(root)
    _verify_linux_runner_lock(root)
    _verify_runner_identity_handoffs(root)
    _verify_fuzz_job_timeout(root)
    _verify_reusable_workflow_permissions(root)
    _verify_shared_integration_dag(root)
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
