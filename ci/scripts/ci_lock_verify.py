#!/usr/bin/env python3
"""Validate every repository-protected CI lock and active consumer.

This durable verifier is intentionally independent of candidate product code.
It rejects floating workflow actions/runners/images, persisted checkout tokens,
unsafe pull-request-target trust routing, reusable-workflow permission
inheritance drift, malformed protected locks, and Docker installation paths
that bypass the immutable snapshot or hash locks.
Run it from any directory; ``--repo-root`` is primarily for fixture tests.
"""

from __future__ import annotations

import argparse
import json
import re
import stat
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
        "security-darwin": {"contents": "read"},
        "suite-gate": {},
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
    workflow_text = workflow_path.read_text(encoding="utf-8")
    if "  workflow_call:" not in workflow_text:
        raise ContractError(f"{workflow_path}: image producer is not reusable")
    if re.search(
        r"(?m)^  (?:push|pull_request|pull_request_target|workflow_dispatch):",
        workflow_text,
    ):
        raise ContractError(
            f"{workflow_path}: image producer must be callable only by the protected integration route"
        )
    source_contract = (
        "publish-source-commit",
        '--workflow-commit "${{ inputs.candidate_commit }}"',
        '--source-commit "${{ steps.source.outputs.commit }}"',
        "org.opencontainers.image.revision=${{ steps.source.outputs.commit }}",
        "CI_IMAGE_SOURCE_COMMIT=${{ steps.source.outputs.commit }}",
        "name: ci-image-input-${{ steps.source.outputs.commit }}-${{ github.run_id }}-${{ github.run_attempt }}",
        "CI_IMAGE_EXPECTED_DIGEST: ${{ steps.push.outputs.digest }}",
        "CI_IMAGE_LOCATOR: ${{ steps.caller.outputs.temporary_image }}",
        "tags: ${{ steps.caller.outputs.temporary_image }}",
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
    if workflow_text.count("uses: docker/build-push-action@") != 1:
        raise ContractError(f"{workflow_path}: candidate image must be built exactly once")
    forbidden_publisher_fragments = (
        "docker/metadata-action@",
        "type=raw,value=latest",
        "type=ref,event=branch",
        "workflow_dispatch:",
    )
    for fragment in forbidden_publisher_fragments:
        if fragment in workflow_text:
            raise ContractError(
                f"{workflow_path}: build step may not publish canonical routing {fragment!r}"
            )
    attest_index = workflow_text.find("subject-digest: ${{ steps.push.outputs.digest }}")
    verify_index = workflow_text.find("Verify candidate digest, attestation, and labels")
    if attest_index < 0 or verify_index < attest_index:
        raise ContractError(
            f"{workflow_path}: exact candidate attestation must precede verified output"
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
        "architecture": "arm64",
        "image_os": "macos15",
        "runner_label": "macos-15",
        "triplet": "arm64-osx",
    }
    for field, expected_value in exact_values.items():
        if lock[field] != expected_value:
            raise ContractError(f"{path}: unexpected {field} identity")
    if not re.fullmatch(r"20[0-9]{6}\.[0-9]{4}\.[0-9]+", str(lock["image_version"])):
        raise ContractError(f"{path}: malformed GitHub runner image version")
    if not re.fullmatch(r"[0-9a-f]{40}", str(lock["vcpkg_commit"])):
        raise ContractError(f"{path}: vcpkg identity is not a full commit SHA")
    integration = (root / ".github/workflows/ci-integration-suite.yml").read_text(
        encoding="utf-8"
    )
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
        "suite-gate:",
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
    """Run the complete protected lock and active-consumer validation."""
    actions = _read_actions(root / "ci/locks/actions.lock")
    _verify_packages(root / "ci/locks/ubuntu-24.04-packages.lock")
    _verify_requirements(root / "ci/locks/requirements-ci.txt")
    _verify_image_lock(root, actions)
    _verify_dockerfile(root)
    _verify_darwin_lock(root)
    _verify_linux_runner_lock(root)
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
