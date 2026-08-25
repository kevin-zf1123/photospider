#!/usr/bin/env python3
"""Validate the exact active default-branch GitHub ruleset contract.

By default this reader obtains the complete paginated repository ruleset list
and every detailed ruleset JSON through the authenticated GitHub CLI.
``--input`` accepts one fixture array for durable offline tests. It never
mutates repository settings and rejects ambiguous multiple default-branch
rulesets, spoofable check publishers, unresolved review threads,
deletion/force-push gaps, or merge methods incompatible with linear history.
"""

from __future__ import annotations

import argparse
import json
import re
import subprocess
import sys
from pathlib import Path
from typing import Any


class RulesetError(ValueError):
    """Report a missing, ambiguous, or incoherent ruleset requirement."""


def _unique_object(pairs: list[tuple[str, Any]]) -> dict[str, Any]:
    """Build a JSON object while rejecting duplicate members."""
    result: dict[str, Any] = {}
    for key, value in pairs:
        if key in result:
            raise RulesetError(f"duplicate JSON member: {key}")
        result[key] = value
    return result


def _decode_json(text: str, context: str) -> Any:
    """Decode strict JSON with stable diagnostics."""
    try:
        return json.loads(text, object_pairs_hook=_unique_object)
    except (json.JSONDecodeError, RulesetError) as error:
        raise RulesetError(f"{context}: invalid strict JSON: {error}") from error


def _load_json(path: Path) -> Any:
    """Read strict UTF-8 JSON from one protected or fixture path."""
    try:
        return _decode_json(path.read_text(encoding="utf-8"), str(path))
    except (OSError, UnicodeError) as error:
        raise RulesetError(f"cannot read {path}: {error}") from error


def _gh_json(endpoint: str) -> Any:
    """Read one authenticated GitHub API endpoint without mutating state."""
    completed = subprocess.run(
        ["gh", "api", "--method", "GET", endpoint],
        check=False,
        text=True,
        capture_output=True,
    )
    if completed.returncode != 0:
        raise RulesetError(f"GitHub ruleset readback failed for {endpoint}: {completed.stderr.strip()}")
    return _decode_json(completed.stdout, endpoint)


def _gh_paginated_array(endpoint: str) -> list[Any]:
    """Read and flatten every authenticated GitHub API array page.

    Args:
        endpoint: Exact read-only API endpoint, including explicit query policy.

    Returns:
        All records from every page in API order.

    Raises:
        RulesetError: ``gh`` fails, emits malformed JSON, or returns anything
            other than the ``--slurp`` outer array of per-page arrays.

    Note:
        ``--paginate --slurp`` is required together: pagination follows every
        Link page, while slurp preserves page boundaries for strict validation
        before this function flattens the records.
    """
    completed = subprocess.run(
        ["gh", "api", "--method", "GET", "--paginate", "--slurp", endpoint],
        check=False,
        text=True,
        capture_output=True,
    )
    if completed.returncode != 0:
        raise RulesetError(
            f"GitHub paginated ruleset readback failed for {endpoint}: "
            f"{completed.stderr.strip()}"
        )
    pages = _decode_json(completed.stdout, endpoint)
    if not isinstance(pages, list) or not pages or not all(isinstance(page, list) for page in pages):
        raise RulesetError("GitHub paginated ruleset list is not an outer array of page arrays")
    return [record for page in pages for record in page]


def _read_live(repository: str) -> list[dict[str, Any]]:
    """Read all non-parent summaries then fetch every detailed ruleset.

    Args:
        repository: Exact GitHub ``owner/name`` identity.

    Returns:
        Detailed rulesets corresponding to every unique paginated summary.

    Raises:
        RulesetError: The repository, list pages, summary IDs, or detail
            responses are malformed, incomplete, or ambiguous.
    """
    if not re.fullmatch(r"[A-Za-z0-9_.-]+/[A-Za-z0-9_.-]+", repository):
        raise RulesetError("repository must be an exact owner/name identity")
    summaries = _gh_paginated_array(f"repos/{repository}/rulesets?includes_parents=false")
    details: list[dict[str, Any]] = []
    seen_identifiers: set[int] = set()
    for summary in summaries:
        identifier = summary.get("id") if isinstance(summary, dict) else None
        if not isinstance(identifier, int) or identifier <= 0:
            raise RulesetError("GitHub ruleset summary has no canonical numeric ID")
        if identifier in seen_identifiers:
            raise RulesetError(f"GitHub ruleset summary repeats canonical ID {identifier}")
        seen_identifiers.add(identifier)
        detail = _gh_json(f"repos/{repository}/rulesets/{identifier}")
        if not isinstance(detail, dict):
            raise RulesetError(f"GitHub ruleset {identifier} detail is not an object")
        details.append(detail)
    return details


def _default_branch_ruleset(rulesets: list[dict[str, Any]]) -> dict[str, Any]:
    """Select exactly one active branch ruleset including ``~DEFAULT_BRANCH``."""
    matches: list[dict[str, Any]] = []
    for ruleset in rulesets:
        if ruleset.get("target") != "branch" or ruleset.get("enforcement") != "active":
            continue
        conditions = ruleset.get("conditions")
        references = conditions.get("ref_name") if isinstance(conditions, dict) else None
        includes = references.get("include") if isinstance(references, dict) else None
        excludes = references.get("exclude") if isinstance(references, dict) else None
        if isinstance(includes, list) and "~DEFAULT_BRANCH" in includes:
            if not isinstance(excludes, list) or "~DEFAULT_BRANCH" in excludes:
                raise RulesetError("default-branch ruleset has malformed or contradictory exclusions")
            matches.append(ruleset)
    if len(matches) != 1:
        raise RulesetError(f"expected exactly one active default-branch ruleset, found {len(matches)}")
    return matches[0]


def _rule_map(ruleset: dict[str, Any]) -> dict[str, dict[str, Any]]:
    """Index rules by type while rejecting duplicate or malformed rule records."""
    rules = ruleset.get("rules")
    if not isinstance(rules, list):
        raise RulesetError("default-branch ruleset has no detailed rules array")
    indexed: dict[str, dict[str, Any]] = {}
    for rule in rules:
        rule_type = rule.get("type") if isinstance(rule, dict) else None
        if not isinstance(rule_type, str) or not rule_type:
            raise RulesetError("ruleset contains a malformed rule")
        if rule_type in indexed:
            raise RulesetError(f"ruleset contains duplicate rule type {rule_type}")
        indexed[rule_type] = rule
    return indexed


def validate(rulesets: list[dict[str, Any]], policy: dict[str, Any]) -> dict[str, Any]:
    """Validate stable checks, publisher, review, and merge policy exactly."""
    if set(policy) != {
        "schema", "required_checks", "github_actions_integration_id", "allowed_linear_merge_methods"
    } or policy.get("schema") != "photospider-ruleset-policy-v1":
        raise RulesetError("protected ruleset policy has missing, unknown, or version-mismatched fields")
    required_checks = policy["required_checks"]
    allowed_methods = policy["allowed_linear_merge_methods"]
    integration_id = policy["github_actions_integration_id"]
    if (
        not isinstance(required_checks, list)
        or required_checks != sorted(required_checks)
        or len(required_checks) != len(set(required_checks))
        or not all(isinstance(item, str) and item for item in required_checks)
    ):
        raise RulesetError("protected required-check list is not canonical")
    if (
        not isinstance(allowed_methods, list)
        or allowed_methods != sorted(allowed_methods)
        or not allowed_methods
        or not set(allowed_methods).issubset({"rebase", "squash"})
    ):
        raise RulesetError("protected linear merge-method list is not canonical")
    if not isinstance(integration_id, int) or integration_id <= 0:
        raise RulesetError("protected GitHub Actions integration ID is invalid")

    ruleset = _default_branch_ruleset(rulesets)
    rules = _rule_map(ruleset)
    for required_rule in ("deletion", "non_fast_forward", "required_linear_history"):
        if required_rule not in rules:
            raise RulesetError(f"default branch is missing {required_rule} protection")
    pull_request = rules.get("pull_request", {}).get("parameters")
    if not isinstance(pull_request, dict):
        raise RulesetError("default branch has no detailed pull-request policy")
    if pull_request.get("required_review_thread_resolution") is not True:
        raise RulesetError("required conversation resolution is disabled")
    merge_methods = pull_request.get("allowed_merge_methods")
    if not isinstance(merge_methods, list) or sorted(merge_methods) != allowed_methods:
        raise RulesetError(
            f"linear-history merge methods are {merge_methods!r}, expected {allowed_methods!r}"
        )

    status_parameters = rules.get("required_status_checks", {}).get("parameters")
    if not isinstance(status_parameters, dict):
        raise RulesetError("default branch has no required status-check policy")
    checks = status_parameters.get("required_status_checks")
    if not isinstance(checks, list):
        raise RulesetError("required status checks are not a detailed array")
    actual: list[str] = []
    for check in checks:
        if not isinstance(check, dict) or set(check) != {"context", "integration_id"}:
            raise RulesetError("required check identity has missing or unknown fields")
        context = check["context"]
        if not isinstance(context, str) or not context:
            raise RulesetError("required check has an empty context")
        if check["integration_id"] != integration_id:
            raise RulesetError(f"required check {context!r} is not bound to GitHub Actions")
        actual.append(context)
    if sorted(actual) != required_checks or len(actual) != len(set(actual)):
        raise RulesetError(f"required checks are {sorted(actual)!r}, expected {required_checks!r}")
    if status_parameters.get("strict_required_status_checks_policy") is not True:
        raise RulesetError("required checks do not require the current base")
    return {
        "allowed_merge_methods": allowed_methods,
        "required_checks": required_checks,
        "ruleset_id": ruleset.get("id"),
        "ruleset_name": ruleset.get("name"),
        "schema": "photospider-ruleset-readback-v1",
    }


def main() -> int:
    """Read live or fixture rulesets and return zero only for exact policy."""
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--repo-root", type=Path, default=Path(__file__).resolve().parents[2])
    source = parser.add_mutually_exclusive_group(required=True)
    source.add_argument("--repository", help="read owner/name live through gh api")
    source.add_argument("--input", type=Path, help="read a detailed ruleset fixture array")
    parser.add_argument("--output", type=Path, help="write canonical successful readback JSON")
    arguments = parser.parse_args()
    try:
        policy = _load_json(arguments.repo_root.resolve() / "ci/locks/ruleset-policy.json")
        raw = _read_live(arguments.repository) if arguments.repository else _load_json(arguments.input)
        if not isinstance(raw, list) or not all(isinstance(item, dict) for item in raw):
            raise RulesetError("ruleset input must be an array of detailed objects")
        result = validate(raw, policy)
        encoded = json.dumps(result, sort_keys=True, separators=(",", ":"), ensure_ascii=True) + "\n"
        if arguments.output:
            if arguments.output.exists() and arguments.output.is_symlink():
                raise RulesetError("refusing symlink output")
            arguments.output.parent.mkdir(parents=True, exist_ok=True)
            arguments.output.write_text(encoded, encoding="utf-8")
        else:
            print(encoded, end="")
    except (RulesetError, OSError) as error:
        print(f"ruleset readback failed: {error}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
