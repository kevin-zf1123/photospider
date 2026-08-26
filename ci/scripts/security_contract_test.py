#!/usr/bin/env python3
"""Durable regressions for protected CI identity and fail-closed readers."""

from __future__ import annotations

import gzip
import hashlib
import importlib.util
import io
import json
import os
import re
import shlex
import shutil
import subprocess
import sys
import tarfile
import tempfile
import time
import unittest
from pathlib import Path
from types import SimpleNamespace
from unittest import mock


REPO_ROOT = Path(__file__).resolve().parents[2]
SCRIPTS = REPO_ROOT / "ci/scripts"
COMMIT_A = subprocess.run(
    ["git", "-C", str(REPO_ROOT), "rev-parse", "--verify", "HEAD^{commit}"],
    check=True,
    text=True,
    capture_output=True,
).stdout.strip()
COMMIT_B = "2" * 40
IMAGE_DIGEST = "sha256:" + "3" * 64


def run_command(
    *arguments: object,
    expect_success: bool = True,
    environment: dict[str, str] | None = None,
) -> subprocess.CompletedProcess[str]:
    """Run one command at repository root and assert its expected outcome."""
    completed = subprocess.run(
        [str(argument) for argument in arguments],
        cwd=REPO_ROOT,
        env={**os.environ, **(environment or {})},
        text=True,
        capture_output=True,
        check=False,
    )
    if expect_success and completed.returncode != 0:
        raise AssertionError(
            f"command failed ({completed.returncode}): {' '.join(map(str, arguments))}\n"
            f"stdout:\n{completed.stdout}\nstderr:\n{completed.stderr}"
        )
    if not expect_success and completed.returncode == 0:
        raise AssertionError(f"command unexpectedly passed: {' '.join(map(str, arguments))}")
    return completed


class ImageManifestContractTest(unittest.TestCase):
    """Exercise canonical image-input creation and exact OCI-label binding."""

    def test_manifest_and_labels_bind_exact_inputs(self) -> None:
        """Accept exact manifest/labels and reject a mismatched manifest label."""
        with tempfile.TemporaryDirectory() as temporary_text:
            temporary = Path(temporary_text)
            manifest = temporary / "manifest.json"
            digest_sidecar = temporary / "manifest.sha256"
            created = run_command(
                "python3", SCRIPTS / "ci_image_manifest.py", "create",
                "--source-commit", COMMIT_A,
                "--repository", "kevin-zf1123/photospider",
                "--output", manifest,
                "--digest-output", digest_sidecar,
            )
            digest = created.stdout.strip()
            self.assertRegex(digest, r"^[0-9a-f]{64}$")
            self.assertEqual(digest_sidecar.read_text(encoding="utf-8"), digest + "\n")
            run_command(
                "python3", SCRIPTS / "ci_image_manifest.py", "verify",
                "--source-commit", COMMIT_A,
                "--repository", "kevin-zf1123/photospider",
                "--manifest", manifest,
                "--expected-digest", digest,
            )
            labels = temporary / "labels.json"
            labels.write_text(
                json.dumps(
                    {
                        "org.opencontainers.image.revision": COMMIT_A,
                        "org.photospider.ci.input-manifest-sha256": digest,
                    },
                    sort_keys=True,
                ) + "\n",
                encoding="utf-8",
            )
            run_command(
                "python3", SCRIPTS / "ci_image_manifest.py", "verify-labels",
                "--manifest", manifest,
                "--labels-json", labels,
                "--source-commit", COMMIT_A,
            )
            labels.write_text(
                json.dumps(
                    {
                        "org.opencontainers.image.revision": COMMIT_A,
                        "org.photospider.ci.input-manifest-sha256": "0" * 64,
                    },
                    sort_keys=True,
                ) + "\n",
                encoding="utf-8",
            )
            failed = run_command(
                "python3", SCRIPTS / "ci_image_manifest.py", "verify-labels",
                "--manifest", manifest,
                "--labels-json", labels,
                "--source-commit", COMMIT_A,
                expect_success=False,
            )
            self.assertIn("expected", failed.stderr)

    def test_publisher_rejects_head_after_last_image_input_commit(self) -> None:
        """Reject a later manual/push HEAD before registry publication can begin."""
        with tempfile.TemporaryDirectory() as temporary_text:
            root = Path(temporary_text) / "repository"
            lock = root / "ci/locks/ci-image-lock.json"
            lock.parent.mkdir(parents=True)
            lock.write_text(
                json.dumps({"input_paths": ["Dockerfile.ci"]}, sort_keys=True) + "\n",
                encoding="utf-8",
            )
            (root / "Dockerfile.ci").write_text("FROM scratch\n", encoding="utf-8")
            run_command("git", "-C", root, "init", "-q")
            run_command("git", "-C", root, "config", "user.name", "CI Contract")
            run_command("git", "-C", root, "config", "user.email", "ci@example.invalid")
            run_command("git", "-C", root, "config", "commit.gpgsign", "false")
            run_command("git", "-C", root, "add", "--", "Dockerfile.ci", "ci/locks/ci-image-lock.json")
            run_command("git", "-C", root, "commit", "-q", "-m", "image input")
            image_source = run_command("git", "-C", root, "rev-parse", "HEAD").stdout.strip()
            accepted = run_command(
                "python3", SCRIPTS / "ci_image_manifest.py",
                "--repo-root", root,
                "publish-source-commit",
                "--workflow-commit", image_source,
            )
            self.assertEqual(accepted.stdout.strip(), image_source)

            (root / "README.md").write_text("later unrelated commit\n", encoding="utf-8")
            run_command("git", "-C", root, "add", "--", "README.md")
            run_command("git", "-C", root, "commit", "-q", "-m", "unrelated")
            later_head = run_command("git", "-C", root, "rev-parse", "HEAD").stdout.strip()
            self.assertNotEqual(later_head, image_source)
            failed = run_command(
                "python3", SCRIPTS / "ci_image_manifest.py",
                "--repo-root", root,
                "publish-source-commit",
                "--workflow-commit", later_head,
                expect_success=False,
            )
            self.assertIn("differs from workflow commit", failed.stderr)
            self.assertIn("refusing an unattestable publication", failed.stderr)

        workflow = (REPO_ROOT / ".github/workflows/build-ci-image.yml").read_text(
            encoding="utf-8"
        )
        guard = workflow.index("publish-source-commit")
        self.assertLess(guard, workflow.index("uses: docker/login-action@"))
        self.assertLess(guard, workflow.index("uses: docker/build-push-action@"))
        self.assertNotIn('--source-commit "${{ github.sha }}"', workflow)

    def test_published_identity_requires_attestation_before_output(self) -> None:
        """Exercise the shell resolver with exact mocked registry and gh boundaries."""
        with tempfile.TemporaryDirectory() as temporary_text:
            temporary = Path(temporary_text)
            source = run_command(
                "python3", SCRIPTS / "ci_image_manifest.py", "source-commit"
            ).stdout.strip()
            manifest = temporary / "precomputed.json"
            digest = run_command(
                "python3", SCRIPTS / "ci_image_manifest.py", "create",
                "--source-commit", source,
                "--repository", "kevin-zf1123/photospider",
                "--output", manifest,
            ).stdout.strip()
            binary_dir = temporary / "bin"
            binary_dir.mkdir()
            docker_log = temporary / "docker.log"
            gh_log = temporary / "gh.log"
            docker = binary_dir / "docker"
            docker.write_text(
                "#!/usr/bin/env bash\n"
                "set -Eeuo pipefail\n"
                "printf '%q ' \"$@\" >> \"$CI_TEST_DOCKER_LOG\"; printf '\\n' >> \"$CI_TEST_DOCKER_LOG\"\n"
                "case \"${1:-} ${2:-} ${3:-}\" in\n"
                "  'buildx imagetools inspect') printf 'Name: test\\nDigest: %s\\n' \"$CI_TEST_IMAGE_DIGEST\" ;;\n"
                "  'image inspect --format') printf '{\"org.opencontainers.image.revision\":\"%s\",\"org.photospider.ci.input-manifest-sha256\":\"%s\"}\\n' \"$CI_TEST_SOURCE_COMMIT\" \"$CI_TEST_MANIFEST_DIGEST\" ;;\n"
                "  *) exit 0 ;;\n"
                "esac\n",
                encoding="utf-8",
            )
            docker.chmod(0o755)
            gh = binary_dir / "gh"
            gh.write_text(
                "#!/usr/bin/env bash\n"
                "set -Eeuo pipefail\n"
                "printf '%q ' \"$@\" >> \"$CI_TEST_GH_LOG\"; printf '\\n' >> \"$CI_TEST_GH_LOG\"\n"
                "printf '[]\\n'\n",
                encoding="utf-8",
            )
            gh.chmod(0o755)
            python = binary_dir / "python3"
            python.write_text(
                "#!/usr/bin/env bash\n"
                "set -Eeuo pipefail\n"
                "if [[ ${1:-} == */ci_runner_verify.py ]]; then printf '{\"platform\":\"Linux\"}\\n'; exit 0; fi\n"
                f"exec {sys.executable!s} \"$@\"\n",
                encoding="utf-8",
            )
            python.chmod(0o755)
            output = temporary / "identity.env"
            image_digest = "sha256:" + "4" * 64
            environment = {
                "PATH": f"{binary_dir}:{os.environ['PATH']}",
                "CI_ARTIFACT_DIR": str(temporary / "artifacts"),
                "CI_IMAGE_EXPECTED_MANIFEST_DIGEST": digest,
                "CI_IMAGE_EXPECTED_SOURCE_COMMIT": source,
                "CI_IMAGE_EXPECTED_WORKFLOW_COMMIT": COMMIT_A,
                "CI_IMAGE_OUTPUT_FILE": str(output),
                "CI_IMAGE_REPOSITORY": "kevin-zf1123/photospider",
                "CI_TEST_DOCKER_LOG": str(docker_log),
                "CI_TEST_GH_LOG": str(gh_log),
                "CI_TEST_IMAGE_DIGEST": image_digest,
                "CI_TEST_MANIFEST_DIGEST": digest,
                "CI_TEST_SOURCE_COMMIT": source,
            }
            run_command("bash", SCRIPTS / "ci_image_verify.sh", environment=environment)
            outputs = output.read_text(encoding="utf-8")
            self.assertIn(f"digest={image_digest}", outputs)
            self.assertIn(
                f"image=ghcr.io/kevin-zf1123/photospider/photospider-ci@{image_digest}",
                outputs,
            )
            gh_arguments = gh_log.read_text(encoding="utf-8")
            self.assertIn("attestation verify", gh_arguments)
            self.assertIn("--deny-self-hosted-runners", gh_arguments)
            self.assertIn(f"--source-digest {source}", gh_arguments)
            self.assertIn(f"--signer-digest {source}", gh_arguments)

            candidate_output = temporary / "candidate-identity.env"
            candidate_locator = (
                "ghcr.io/kevin-zf1123/photospider/photospider-ci:"
                f"candidate-{COMMIT_A}-1-1"
            )
            docker_log.write_text("", encoding="utf-8")
            candidate_environment = {
                **environment,
                "CI_ARTIFACT_DIR": str(temporary / "candidate-artifacts"),
                "CI_IMAGE_EXPECTED_DIGEST": image_digest,
                "CI_IMAGE_LOCATOR": candidate_locator,
                "CI_IMAGE_OUTPUT_FILE": str(candidate_output),
            }
            run_command(
                "bash", SCRIPTS / "ci_image_verify.sh",
                environment=candidate_environment,
            )
            self.assertIn(candidate_locator, docker_log.read_text(encoding="utf-8"))
            self.assertIn(f"digest={image_digest}", candidate_output.read_text(encoding="utf-8"))

            exact_output = temporary / "exact-identity.env"
            exact_locator = (
                "ghcr.io/kevin-zf1123/photospider/photospider-ci@"
                f"{image_digest}"
            )
            run_command(
                "bash", SCRIPTS / "ci_image_verify.sh",
                environment={
                    **candidate_environment,
                    "CI_ARTIFACT_DIR": str(temporary / "exact-artifacts"),
                    "CI_IMAGE_LOCATOR": exact_locator,
                    "CI_IMAGE_OUTPUT_FILE": str(exact_output),
                },
            )
            self.assertIn(f"image={exact_locator}", exact_output.read_text(encoding="utf-8"))

            forged_cases = (
                (
                    "manifest",
                    {"CI_IMAGE_EXPECTED_MANIFEST_DIGEST": "5" * 64},
                    "manifest digest differs",
                ),
                (
                    "source",
                    {"CI_IMAGE_EXPECTED_SOURCE_COMMIT": COMMIT_B},
                    "source commit differs",
                ),
                (
                    "workflow",
                    {"CI_IMAGE_EXPECTED_WORKFLOW_COMMIT": COMMIT_B},
                    "Protected checkout differs",
                ),
                (
                    "digest",
                    {"CI_IMAGE_EXPECTED_DIGEST": "sha256:" + "5" * 64},
                    "differs from expected",
                ),
                (
                    "digest-ref",
                    {
                        "CI_IMAGE_LOCATOR": (
                            "ghcr.io/kevin-zf1123/photospider/photospider-ci@"
                            "sha256:" + "6" * 64
                        )
                    },
                    "resolved to a different OCI digest",
                ),
                (
                    "repository-ref",
                    {
                        "CI_IMAGE_LOCATOR": (
                            "ghcr.io/kevin-zf1123/forged/photospider-ci@"
                            f"{image_digest}"
                        )
                    },
                    "repository differs",
                ),
            )
            for label, override, diagnostic in forged_cases:
                with self.subTest(forged_identity=label):
                    mismatch = run_command(
                        "bash",
                        SCRIPTS / "ci_image_verify.sh",
                        environment={
                            **candidate_environment,
                            **override,
                            "CI_ARTIFACT_DIR": str(
                                temporary / f"mismatch-{label}-artifacts"
                            ),
                            "CI_IMAGE_OUTPUT_FILE": str(
                                temporary / f"mismatch-{label}.env"
                            ),
                        },
                        expect_success=False,
                    )
                    self.assertIn(diagnostic, mismatch.stderr)


class ImagePromotionContractTest(unittest.TestCase):
    """Exercise trusted same-digest tag promotion and negative event routing."""

    def test_workflow_quotes_untrusted_branch_from_environment(self) -> None:
        """Pass Git-valid hostile branch bytes as one inert promotion argument.

        Returns:
            None after the maintained folded run scalar preserves every branch
            value exactly and none of its embedded shell syntax executes.

        Raises:
            AssertionError: The workflow interpolates ``github.ref_name`` in
                shell source, omits the quoted environment boundary, changes a
                branch argument, or creates an injection marker.

        Note:
            GitHub resolves the expression into the step environment. The test
            executes the production step command with a logging promotion shim;
            hostile values are supplied only through ``subprocess`` environment
            data and are never interpolated into the test's shell source.
        """
        workflow = REPO_ROOT / ".github/workflows/ci-integration.yml"
        lines = workflow.read_text(encoding="utf-8").splitlines()
        step_marker = "      - name: Promote the tested digest without rebuilding"
        try:
            step_start = lines.index(step_marker)
        except ValueError as error:
            raise AssertionError("promotion step is missing") from error

        step_end = len(lines)
        for index in range(step_start + 1, len(lines)):
            if lines[index].startswith("      - ") or re.match(
                r"^  [A-Za-z0-9_-]+:$", lines[index]
            ):
                step_end = index
                break
        step_lines = lines[step_start:step_end]
        step_text = "\n".join(step_lines)
        self.assertIn(
            "CI_PROMOTION_BRANCH: ${{ github.ref_name }}", step_text
        )

        try:
            run_header = step_lines.index("        run: >-")
        except ValueError as error:
            raise AssertionError(
                "promotion step lacks its folded run scalar"
            ) from error
        run_lines = step_lines[run_header + 1 :]
        self.assertTrue(run_lines)
        self.assertTrue(all(line.startswith("          ") for line in run_lines))
        run_source = " ".join(line[10:] for line in run_lines)
        self.assertNotIn("github.ref_name", run_source)
        self.assertIn('--branch "$CI_PROMOTION_BRANCH"', run_source)

        trusted_replacements = {
            "${{ github.repository }}": "kevin-zf1123/photospider",
            "${{ needs.candidate-image-build.outputs.image_ref }}": (
                "ghcr.io/kevin-zf1123/photospider/photospider-ci@"
                f"{IMAGE_DIGEST}"
            ),
            "${{ needs.candidate-image-build.outputs.digest }}": IMAGE_DIGEST,
            "${{ github.sha }}": COMMIT_A,
            "${{ needs.candidate-image-build.outputs.source_commit }}": COMMIT_A,
            "${{ needs.candidate-image-build.outputs.manifest_digest }}": "4" * 64,
            "${{ github.event_name }}": "push",
        }
        executable_source = run_source
        for expression, fixed_value in trusted_replacements.items():
            executable_source = executable_source.replace(expression, fixed_value)
        self.assertNotIn("${{", executable_source)

        with tempfile.TemporaryDirectory() as temporary_text:
            root = Path(temporary_text)
            script_dir = root / "ci/scripts"
            script_dir.mkdir(parents=True)
            shim = script_dir / "ci_image_promote.sh"
            shim.write_text(
                "#!/usr/bin/env bash\n"
                "set -Eeuo pipefail\n"
                "printf '%s\\0' \"$@\" > \"$CI_TEST_PROMOTION_ARGV_LOG\"\n",
                encoding="utf-8",
            )
            shim.chmod(0o755)

            injection_cases = (
                (
                    "command-substitution",
                    "CI/$(touch${IFS}promotion-dollar-marker)",
                    root / "promotion-dollar-marker",
                ),
                (
                    "backtick-substitution",
                    "CI/`touch${IFS}promotion-backtick-marker`",
                    root / "promotion-backtick-marker",
                ),
                (
                    "quote-semicolon",
                    'CI/a";touch${IFS}promotion-quote-marker;echo"b',
                    root / "promotion-quote-marker",
                ),
            )
            for label, branch, marker in injection_cases:
                with self.subTest(branch_syntax=label):
                    run_command(
                        "git", "check-ref-format", f"refs/heads/{branch}"
                    )
                    argv_log = root / f"{label}.argv"
                    completed = subprocess.run(
                        ["bash", "-c", executable_source],
                        cwd=root,
                        env={
                            **os.environ,
                            "CI_IMAGE_PROMOTION_ARTIFACT_DIR": str(
                                root / f"{label}-evidence"
                            ),
                            "CI_PROMOTION_BRANCH": branch,
                            "CI_TEST_PROMOTION_ARGV_LOG": str(argv_log),
                        },
                        text=True,
                        capture_output=True,
                        check=False,
                    )
                    self.assertEqual(
                        completed.returncode,
                        0,
                        msg=(
                            f"promotion command failed for {label}:\n"
                            f"stdout:\n{completed.stdout}\n"
                            f"stderr:\n{completed.stderr}"
                        ),
                    )
                    arguments = argv_log.read_bytes().split(b"\0")
                    self.assertEqual(arguments[-1], b"")
                    arguments = arguments[:-1]
                    branch_index = arguments.index(b"--branch") + 1
                    self.assertEqual(arguments[branch_index], os.fsencode(branch))
                    self.assertFalse(marker.exists())

    def test_promotion_is_monotonic_across_live_ref_updates(self) -> None:
        """Serialize immutable/mutable writers and reject stale identities.

        Returns:
            None after real Git histories and a stateful registry shim prove
            SHA creation/reuse/conflict, registry-failure, normal, inverse,
            documentation-only, superseded, force-push, manifest-failure, and
            ref-drift outcomes.

        Raises:
            AssertionError: Production promotion changes a newer mutable tag,
                moves an existing SHA tag, mistakes registry failure for
                absence, accepts an unknown live identity, or weakens existing
                digest/tag/event validation.

        Note:
            The fixed production HTTPS remote is redirected through an isolated
            Git HOME to a temporary bare repository. Candidate scripts still
            execute unchanged and no repository-owned ref or registry is used.
        """
        with tempfile.TemporaryDirectory() as temporary_text:
            root = Path(temporary_text)
            repository = root / "candidate-repository"
            origin = root / "origin.git"
            test_home = root / "home"
            scratch_root = root / "scratch"
            binary_dir = root / "bin"
            for directory in (test_home, scratch_root, binary_dir):
                directory.mkdir()

            def run_local(
                *arguments: object,
                cwd: Path = root,
                environment: dict[str, str] | None = None,
                expect_success: bool = True,
                input_text: str | None = None,
            ) -> subprocess.CompletedProcess[str]:
                """Run one isolated fixture command with an asserted outcome.

                Args:
                    *arguments: Exact argv entries.
                    cwd: Fixture-owned working directory.
                    environment: Optional environment additions.
                    expect_success: Whether zero exit status is required.
                    input_text: Optional standard-input text.

                Returns:
                    Completed process with captured text output.

                Raises:
                    AssertionError: Exit status differs from expectation.

                Note:
                    No argument is interpolated into a shell command.
                """
                completed = subprocess.run(
                    [str(argument) for argument in arguments],
                    cwd=cwd,
                    env={**os.environ, **(environment or {})},
                    input=input_text,
                    text=True,
                    capture_output=True,
                    check=False,
                )
                if expect_success and completed.returncode != 0:
                    raise AssertionError(
                        f"fixture command failed: {arguments!r}\n"
                        f"stdout:\n{completed.stdout}\n"
                        f"stderr:\n{completed.stderr}"
                    )
                if not expect_success and completed.returncode == 0:
                    raise AssertionError(
                        f"fixture command unexpectedly passed: {arguments!r}"
                    )
                return completed

            run_local("git", "init", "-b", "main", repository)
            run_local("git", "config", "user.name", "CI Promotion Test", cwd=repository)
            run_local(
                "git",
                "config",
                "user.email",
                "ci-promotion@example.invalid",
                cwd=repository,
            )
            image_lock = json.loads(
                (REPO_ROOT / "ci/locks/ci-image-lock.json").read_text(
                    encoding="utf-8"
                )
            )
            input_paths = image_lock["input_paths"]
            self.assertIsInstance(input_paths, list)
            for relative in input_paths:
                source = REPO_ROOT / relative
                destination = repository / relative
                destination.parent.mkdir(parents=True, exist_ok=True)
                shutil.copy2(source, destination)
            run_local("git", "add", "--all", cwd=repository)
            run_local(
                "git", "commit", "-m", "fixture: image identity A", cwd=repository
            )
            commit_a = run_local(
                "git", "rev-parse", "HEAD^{commit}", cwd=repository
            ).stdout.strip()
            run_local("git", "clone", "--bare", repository, origin)
            run_local("git", "remote", "add", "origin", origin, cwd=repository)

            real_git = shutil.which("git")
            self.assertIsNotNone(real_git)
            git_environment = {
                "GIT_CONFIG_NOSYSTEM": "1",
                "HOME": str(test_home),
            }
            run_local(
                real_git,
                "config",
                "--global",
                "protocol.file.allow",
                "always",
                environment=git_environment,
            )
            run_local(
                real_git,
                "config",
                "--global",
                f"url.{origin.resolve().as_uri()}.insteadOf",
                "https://github.com/kevin-zf1123/photospider.git",
                environment=git_environment,
            )

            def update_remote(branch: str, commit: str) -> None:
                """Move one fixture bare-repository branch to an exact commit.

                Args:
                    branch: Git-valid branch name without ``refs/heads/``.
                    commit: Fixture commit to upload and install as the ref tip.

                Returns:
                    None after a forced local-file push succeeds.

                Raises:
                    AssertionError: The object upload or ref update fails.

                Note:
                    Only the temporary bare origin is mutated.
                """
                run_local(
                    real_git,
                    "push",
                    "--force",
                    origin,
                    f"{commit}:refs/heads/{branch}",
                    cwd=repository,
                )

            def manifest_for(commit: str, label: str) -> str:
                """Measure one fixture commit with the production manifest helper.

                Args:
                    commit: Exact fixture checkout/source commit.
                    label: Unique output basename for the canonical manifest.

                Returns:
                    Lowercase SHA-256 digest of the canonical manifest bytes.

                Raises:
                    AssertionError: Checkout, manifest creation, or digest syntax
                        validation fails.

                Note:
                    The manifest path is outside the fixture Git worktree.
                """
                run_local("git", "checkout", "--detach", commit, cwd=repository)
                manifest = root / f"{label}.manifest.json"
                digest = run_command(
                    sys.executable,
                    SCRIPTS / "ci_image_manifest.py",
                    "--repo-root",
                    repository,
                    "create",
                    "--source-commit",
                    commit,
                    "--repository",
                    "kevin-zf1123/photospider",
                    "--output",
                    manifest,
                ).stdout.strip()
                self.assertRegex(digest, r"\A[0-9a-f]{64}\Z")
                return digest

            def create_image_input_identity(label: str) -> tuple[str, str]:
                """Create one fresh exact image-input commit and its manifest.

                Args:
                    label: Unique diagnostic suffix written into the canonical
                        ``.dockerignore`` image input and commit message.

                Returns:
                    Full commit SHA and canonical manifest digest for the new
                    fixture identity.

                Raises:
                    AssertionError: Checkout, commit, or manifest generation
                        fails through the isolated fixture helpers.

                Note:
                    Every identity branches directly from ``commit_a`` so each
                    immutable SHA destination starts absent independently. The
                    modified input is production-authoritative and therefore
                    keeps source commit, manifest source, and checkout equal.
                """
                run_local("git", "checkout", "--detach", commit_a, cwd=repository)
                dockerignore = repository / ".dockerignore"
                dockerignore.write_text(
                    dockerignore.read_text(encoding="utf-8")
                    + f"\n# immutable SHA post-create fixture: {label}\n",
                    encoding="utf-8",
                )
                run_local("git", "add", ".dockerignore", cwd=repository)
                run_local(
                    "git",
                    "commit",
                    "-m",
                    f"fixture: immutable SHA {label}",
                    cwd=repository,
                )
                commit = run_local(
                    "git", "rev-parse", "HEAD^{commit}", cwd=repository
                ).stdout.strip()
                return commit, manifest_for(commit, f"immutable-sha-{label}")

            manifest_a = manifest_for(commit_a, "identity-a")
            image_repository = "ghcr.io/kevin-zf1123/photospider/photospider-ci"
            registry_state_path = root / "registry-state.json"
            docker_log = root / "docker.log"
            inspect_log = root / "inspect.log"
            docker = binary_dir / "docker"
            docker.write_text(
                "#!/usr/bin/env python3\n"
                "import json\n"
                "import os\n"
                "from pathlib import Path\n"
                "import shlex\n"
                "import sys\n"
                "arguments = sys.argv[1:]\n"
                "log = Path(os.environ['CI_TEST_DOCKER_LOG'])\n"
                "with log.open('a', encoding='utf-8') as handle:\n"
                "    handle.write(' '.join(shlex.quote(value) for value in arguments) + '\\n')\n"
                "state_path = Path(os.environ['CI_TEST_REGISTRY_STATE'])\n"
                "state = (\n"
                "    json.loads(state_path.read_text(encoding='utf-8'))\n"
                "    if state_path.exists() else {}\n"
                ")\n"
                "if arguments[:3] == ['buildx', 'imagetools', 'create']:\n"
                "    tags = []\n"
                "    metadata = None\n"
                "    index = 3\n"
                "    while index < len(arguments) - 1:\n"
                "        if arguments[index] == '--tag':\n"
                "            tags.append(arguments[index + 1]); index += 2\n"
                "        elif arguments[index] == '--metadata-file':\n"
                "            metadata = Path(arguments[index + 1]); index += 2\n"
                "        else:\n"
                "            index += 1\n"
                "    source_digest = arguments[-1].rsplit('@', 1)[-1]\n"
                "    for tag in tags:\n"
                "        state[tag] = source_digest\n"
                "    state_path.write_text(\n"
                "        json.dumps(state, sort_keys=True) + '\\n', encoding='utf-8'\n"
                "    )\n"
                "    if metadata is None:\n"
                "        raise SystemExit(95)\n"
                "    metadata.parent.mkdir(parents=True, exist_ok=True)\n"
                "    metadata_digest = os.environ.get('CI_TEST_METADATA_DIGEST', source_digest)\n"
                "    metadata.write_text(\n"
                "        json.dumps(\n"
                "            {'containerimage.descriptor': {'digest': metadata_digest}}\n"
                "        ) + '\\n',\n"
                "        encoding='utf-8',\n"
                "    )\n"
                "elif arguments[:3] == ['buildx', 'imagetools', 'inspect']:\n"
                "    target = arguments[-1]\n"
                "    with Path(os.environ['CI_TEST_INSPECT_LOG']).open(\n"
                "        'a', encoding='utf-8'\n"
                "    ) as handle:\n"
                "        handle.write(target + '\\n')\n"
                "    mode = 'expected'\n"
                "    selected = os.environ.get('CI_TEST_INSPECT_TARGET')\n"
                "    sequence_text = os.environ.get('CI_TEST_INSPECT_SEQUENCE', '')\n"
                "    if sequence_text and not selected:\n"
                "        raise SystemExit(92)\n"
                "    if sequence_text and selected == target:\n"
                "        sequence = json.loads(sequence_text)\n"
                "        if (not isinstance(sequence, list) or not sequence or\n"
                "                not all(isinstance(item, str) and item for item in sequence)):\n"
                "            raise SystemExit(93)\n"
                "        sequence_state_path = Path(\n"
                "            os.environ['CI_TEST_INSPECT_SEQUENCE_STATE']\n"
                "        )\n"
                "        sequence_state = (\n"
                "            json.loads(sequence_state_path.read_text(encoding='utf-8'))\n"
                "            if sequence_state_path.exists() else {}\n"
                "        )\n"
                "        sequence_index = sequence_state.get(target, 0)\n"
                "        if (not isinstance(sequence_index, int) or\n"
                "                isinstance(sequence_index, bool) or\n"
                "                sequence_index < 0 or sequence_index >= len(sequence)):\n"
                "            raise SystemExit(94)\n"
                "        mode = sequence[sequence_index]\n"
                "        sequence_state[target] = sequence_index + 1\n"
                "        sequence_state_path.write_text(\n"
                "            json.dumps(sequence_state, sort_keys=True) + '\\n',\n"
                "            encoding='utf-8',\n"
                "        )\n"
                "    elif not selected or selected == target:\n"
                "        mode = os.environ.get('CI_TEST_INSPECT_MODE', 'expected')\n"
                "    expected = state.get(target)\n"
                "    wrong = os.environ['CI_TEST_WRONG_DIGEST']\n"
                "    if mode == 'expected' and expected:\n"
                "        print(json.dumps({'digest': expected}, separators=(',', ':')))\n"
                "    elif mode == 'wrong':\n"
                "        print(json.dumps({'digest': wrong}, separators=(',', ':')))\n"
                "    elif mode == 'no-digest':\n"
                "        print('{}')\n"
                "    elif mode == 'multiple':\n"
                "        first = expected or wrong\n"
                "        print(f'{{\"digest\":\"{first}\",\"digest\":\"{wrong}\"}}')\n"
                "    elif mode in {'expected', 'missing'}:\n"
                "        print(f'ERROR: {target}: not found', file=sys.stderr)\n"
                "        raise SystemExit(1)\n"
                "    elif mode == 'auth':\n"
                "        print('ERROR: denied: requested access to the resource is denied', file=sys.stderr)\n"
                "        raise SystemExit(1)\n"
                "    elif mode == 'network':\n"
                "        print('ERROR: failed to do request: TLS handshake timeout', file=sys.stderr)\n"
                "        raise SystemExit(1)\n"
                "    elif mode == 'not-found-drift':\n"
                "        print(f'ERROR: {target}: not found (registry detail)', file=sys.stderr)\n"
                "        raise SystemExit(1)\n"
                "    elif mode == 'unknown':\n"
                "        print('ERROR: unclassified registry failure', file=sys.stderr)\n"
                "        raise SystemExit(42)\n"
                "    else:\n"
                "        raise SystemExit(96)\n"
                "else:\n"
                "    raise SystemExit(97)\n",
                encoding="utf-8",
            )
            docker.chmod(0o755)

            git_wrapper = binary_dir / "git"
            git_wrapper.write_text(
                "#!/usr/bin/env bash\n"
                "set -Eeuo pipefail\n"
                "is_fetch=false\n"
                "for argument in \"$@\"; do\n"
                "  if [[ \"$argument\" == fetch ]]; then is_fetch=true; fi\n"
                "done\n"
                "\"$CI_TEST_REAL_GIT\" \"$@\"\n"
                "if [[ \"$is_fetch\" == true && -n ${CI_TEST_GIT_DRIFT_REF:-} ]]; then\n"
                "  count=0\n"
                "  if [[ -f \"$CI_TEST_GIT_DRIFT_COUNTER\" ]]; then\n"
                "    read -r count < \"$CI_TEST_GIT_DRIFT_COUNTER\"\n"
                "  fi\n"
                "  count=$((count + 1))\n"
                "  printf '%s\\n' \"$count\" > \"$CI_TEST_GIT_DRIFT_COUNTER\"\n"
                "  if ((count == 1)); then\n"
                "    \"$CI_TEST_REAL_GIT\" \\\n"
                "      --git-dir=\"$CI_TEST_GIT_DRIFT_REPOSITORY\" \\\n"
                "      update-ref \"$CI_TEST_GIT_DRIFT_REF\" \\\n"
                "      \"$CI_TEST_GIT_DRIFT_NEXT\"\n"
                "  fi\n"
                "fi\n",
                encoding="utf-8",
            )
            git_wrapper.chmod(0o755)

            base_environment = {
                **git_environment,
                "PATH": f"{binary_dir}:{os.environ['PATH']}",
                "CI_IMAGE_PROMOTION_REPO_ROOT": str(repository),
                "CI_IMAGE_PROMOTION_SCRATCH_ROOT": str(scratch_root),
                "CI_TEST_DOCKER_LOG": str(docker_log),
                "CI_TEST_INSPECT_LOG": str(inspect_log),
                "CI_TEST_REAL_GIT": str(real_git),
                "CI_TEST_REGISTRY_STATE": str(registry_state_path),
                "CI_TEST_WRONG_DIGEST": "sha256:" + "9" * 64,
            }

            def invoke(
                branch: str,
                label: str,
                *,
                candidate_commit: str = commit_a,
                source_commit: str | None = None,
                manifest_digest: str = manifest_a,
                image_digest: str = IMAGE_DIGEST,
                event_name: str = "push",
                expect_success: bool = True,
                environment_overrides: dict[str, str] | None = None,
            ) -> tuple[
                subprocess.CompletedProcess[str],
                dict[str, str] | None,
                str,
                list[str],
            ]:
                """Run production promotion against live fixture Git/OCI state.

                Args:
                    branch: Exact live branch identity.
                    label: Unique filesystem-safe evidence label.
                    candidate_commit: Tested product and checkout commit.
                    source_commit: Image source commit, defaulting to candidate.
                    manifest_digest: Candidate canonical input-manifest digest.
                    image_digest: Exact tested OCI digest.
                    event_name: Protected trigger identity.
                    expect_success: Whether production must return zero.
                    environment_overrides: Optional registry/ref fault controls.

                Returns:
                    Process, success-only outputs, Docker call log, and ordered
                    inspect destination list.

                Raises:
                    AssertionError: Checkout or process outcome is incoherent,
                        a failed run reports a promoted/superseded success, or a
                        successful output record is malformed.

                Note:
                    Each call owns fresh evidence while registry state persists,
                    allowing deterministic monotonic-tag assertions. Its unique
                    sequence-state file lets separate Docker shim processes
                    return distinct responses for repeated inspection of the
                    same destination within one promotion invocation.
                """
                run_local(
                    "git", "checkout", "--detach", candidate_commit, cwd=repository
                )
                docker_log.write_text("", encoding="utf-8")
                inspect_log.write_text("", encoding="utf-8")
                output = root / f"{label}.outputs"
                sequence_state = root / f"{label}.inspect-sequence.json"
                actual_source = source_commit or candidate_commit
                image = f"{image_repository}@{image_digest}"
                completed = run_command(
                    "bash",
                    SCRIPTS / "ci_image_promote.sh",
                    "--repository",
                    "kevin-zf1123/photospider",
                    "--image-ref",
                    image,
                    "--expected-digest",
                    image_digest,
                    "--candidate-commit",
                    candidate_commit,
                    "--source-commit",
                    actual_source,
                    "--manifest-digest",
                    manifest_digest,
                    "--event-name",
                    event_name,
                    "--branch",
                    branch,
                    environment={
                        **base_environment,
                        "CI_TEST_METADATA_DIGEST": image_digest,
                        **(environment_overrides or {}),
                        "CI_TEST_INSPECT_SEQUENCE_STATE": str(sequence_state),
                        "CI_IMAGE_PROMOTION_ARTIFACT_DIR": str(
                            root / f"{label}-evidence"
                        ),
                        "CI_IMAGE_PROMOTION_OUTPUT_FILE": str(output),
                    },
                    expect_success=expect_success,
                )
                inspected = inspect_log.read_text(encoding="utf-8").splitlines()
                docker_calls = docker_log.read_text(encoding="utf-8")
                if not expect_success:
                    output_text = (
                        output.read_text(encoding="utf-8")
                        if output.exists()
                        else ""
                    )
                    self.assertNotRegex(
                        output_text, r"(?m)^status=(?:promoted|superseded)$"
                    )
                    self.assertNotRegex(
                        completed.stdout,
                        r"(?m)^(?:Promoted |Candidate .* was superseded;)",
                    )
                    return completed, None, docker_calls, inspected
                values: dict[str, str] = {}
                for line in output.read_text(encoding="utf-8").splitlines():
                    key, separator, value = line.partition("=")
                    if not separator or not key or not value or key in values:
                        raise AssertionError(
                            f"malformed promotion output for {label}: {line!r}"
                        )
                    values[key] = value
                self.assertEqual(
                    set(values),
                    {"branch_tag", "digest", "sha_action", "sha_tag", "status"},
                )
                return completed, values, docker_calls, inspected

            def registry_state() -> dict[str, str]:
                """Return the stateful shim's exact current tag-to-digest map.

                Returns:
                    Empty state before first mutation, otherwise the decoded
                    destination-to-digest string mapping.

                Raises:
                    AssertionError: Persisted state is not a JSON object.

                Note:
                    The caller performs exact per-tag digest assertions.
                """
                if not registry_state_path.exists():
                    return {}
                value = json.loads(registry_state_path.read_text(encoding="utf-8"))
                self.assertIsInstance(value, dict)
                return value

            def created_tag_groups(docker_calls: str) -> list[list[str]]:
                """Decode every stateful Buildx create call's exact tag group.

                Args:
                    docker_calls: Newline-delimited, shell-quoted argv log from
                        one production promotion invocation.

                Returns:
                    Ordered destination groups, one list per create operation.

                Raises:
                    AssertionError: A create call has a missing tag value or
                        contains no destination.

                Note:
                    Parsing the shim's argv log makes the regression sensitive
                    to removal or reordering of the immutable SHA preflight; a
                    same-SHA rerun must contain no SHA destination in any group.
                """
                groups: list[list[str]] = []
                for line in docker_calls.splitlines():
                    arguments = shlex.split(line)
                    if arguments[:3] != ["buildx", "imagetools", "create"]:
                        continue
                    tags: list[str] = []
                    for index, argument in enumerate(arguments[:-1]):
                        if argument == "--tag":
                            self.assertLess(index + 1, len(arguments))
                            tags.append(arguments[index + 1])
                    self.assertTrue(tags)
                    groups.append(tags)
                return groups

            branch_destination = f"{image_repository}:branch-main"
            latest_destination = f"{image_repository}:latest"
            mutable_sentinels = {
                branch_destination: "sha256:" + "b" * 64,
                latest_destination: "sha256:" + "c" * 64,
            }
            registry_state_path.write_text(
                json.dumps(mutable_sentinels, sort_keys=True) + "\n",
                encoding="utf-8",
            )
            immutable_creation_failures = (
                (
                    "metadata-mismatch",
                    None,
                    {
                        "CI_TEST_METADATA_DIGEST": base_environment[
                            "CI_TEST_WRONG_DIGEST"
                        ]
                    },
                    "Immutable SHA creation metadata differs",
                    1,
                ),
                (
                    "post-create-wrong-digest",
                    ("missing", "wrong"),
                    {},
                    "New immutable SHA tag differs from the tested digest",
                    2,
                ),
                (
                    "post-create-missing-digest",
                    ("missing", "no-digest"),
                    {},
                    "inspected manifest lacks one canonical top-level digest",
                    2,
                ),
                (
                    "post-create-multiple-digests",
                    ("missing", "multiple"),
                    {},
                    "duplicate JSON member: digest",
                    2,
                ),
            )
            for (
                label,
                inspect_sequence,
                environment_overrides,
                diagnostic,
                expected_inspect_count,
            ) in immutable_creation_failures:
                with self.subTest(immutable_sha_creation=label):
                    candidate_commit, candidate_manifest = (
                        create_image_input_identity(label)
                    )
                    update_remote("main", candidate_commit)
                    sha_destination = (
                        f"{image_repository}:sha-{candidate_commit}"
                    )
                    invocation_environment = dict(environment_overrides)
                    if inspect_sequence is not None:
                        invocation_environment.update(
                            {
                                "CI_TEST_INSPECT_SEQUENCE": json.dumps(
                                    inspect_sequence
                                ),
                                "CI_TEST_INSPECT_TARGET": sha_destination,
                            }
                        )
                    state_before = registry_state()
                    failed, _, calls, inspected = invoke(
                        "main",
                        label,
                        candidate_commit=candidate_commit,
                        manifest_digest=candidate_manifest,
                        environment_overrides=invocation_environment,
                        expect_success=False,
                    )
                    self.assertIn(diagnostic, failed.stderr)
                    self.assertEqual(
                        inspected, [sha_destination] * expected_inspect_count
                    )
                    self.assertEqual(
                        created_tag_groups(calls), [[sha_destination]]
                    )
                    state_after = registry_state()
                    self.assertEqual(state_after[sha_destination], IMAGE_DIGEST)
                    self.assertEqual(
                        set(state_after).difference(state_before),
                        {sha_destination},
                    )
                    for mutable_destination in (
                        branch_destination,
                        latest_destination,
                    ):
                        self.assertEqual(
                            state_after[mutable_destination],
                            state_before[mutable_destination],
                        )

            update_remote("main", commit_a)

            long_branch = "CI/" + "/".join(
                f"segment{index}-" + "x" * 52 for index in range(4)
            )
            valid_cases = (
                ("main", "main"),
                ("CI/a-b", "ci-flat"),
                ("CI/a/b", "ci-nested"),
                ("CI/a+b", "ci-plus"),
                ("CI/a@b", "ci-at"),
                ("CI/a=b", "ci-equals"),
                ("CI/a,b", "ci-comma"),
                ("CI/a;z", "ci-semicolon"),
                ("CI/中文", "ci-unicode"),
                (long_branch, "ci-long"),
            )
            for branch, _ in valid_cases:
                update_remote(branch, commit_a)

            successful: dict[
                str, tuple[dict[str, str], str, list[str]]
            ] = {}
            for branch, label in valid_cases:
                if branch != "main":
                    run_command("git", "check-ref-format", f"refs/heads/{branch}")
                _, outputs, calls, inspected = invoke(branch, label)
                if outputs is None:
                    raise AssertionError(f"successful promotion lacks output: {label}")
                self.assertEqual(outputs["status"], "promoted")
                successful[label] = (outputs, calls, inspected)

            _, repeated_outputs, repeated_calls, repeated_inspected = invoke(
                "CI/a-b", "ci-flat-repeat"
            )
            if repeated_outputs is None:
                raise AssertionError("repeated promotion lacks output")
            main_outputs, main_calls, _ = successful["main"]
            flat_outputs, _, _ = successful["ci-flat"]
            nested_outputs, _, _ = successful["ci-nested"]
            long_outputs, _, _ = successful["ci-long"]
            self.assertEqual(main_outputs["branch_tag"], "branch-main")
            self.assertEqual(main_outputs["sha_tag"], f"sha-{commit_a}")
            self.assertEqual(main_outputs["sha_action"], "created")
            self.assertEqual(repeated_outputs["sha_action"], "reused")
            self.assertIn("photospider-ci:latest", main_calls)
            self.assertNotEqual(
                flat_outputs["branch_tag"], nested_outputs["branch_tag"]
            )
            self.assertEqual(
                flat_outputs["branch_tag"], repeated_outputs["branch_tag"]
            )
            for branch, label in valid_cases:
                outputs, calls, inspected = successful[label]
                branch_tag = outputs["branch_tag"]
                self.assertRegex(
                    branch_tag, r"\A[A-Za-z0-9_][A-Za-z0-9_.-]{0,127}\Z"
                )
                self.assertLessEqual(len(branch_tag), 128)
                self.assertEqual(outputs["digest"], IMAGE_DIGEST)
                self.assertEqual(outputs["sha_tag"], f"sha-{commit_a}")
                sha_destination = f"{image_repository}:sha-{commit_a}"
                branch_destination = f"{image_repository}:{branch_tag}"
                if branch == "main":
                    expected_inspections = [
                        sha_destination,
                        sha_destination,
                        branch_destination,
                        f"{image_repository}:latest",
                    ]
                    expected_create_groups = [
                        [sha_destination],
                        [branch_destination, f"{image_repository}:latest"],
                    ]
                    self.assertEqual(outputs["sha_action"], "created")
                else:
                    expected_inspections = [sha_destination, branch_destination]
                    expected_create_groups = [[branch_destination]]
                    self.assertEqual(outputs["sha_action"], "reused")
                self.assertEqual(inspected, expected_inspections)
                self.assertEqual(
                    calls.count("buildx imagetools inspect"),
                    len(expected_inspections),
                )
                self.assertEqual(created_tag_groups(calls), expected_create_groups)
                self.assertIn("--prefer-index=false", calls)
                self.assertIn(f"{image_repository}@{IMAGE_DIGEST}", calls)
                self.assertNotIn(" build ", calls)
                if branch == "main":
                    self.assertIn("photospider-ci:latest", calls)
                else:
                    expected_hash = hashlib.sha256(os.fsencode(branch)).hexdigest()
                    self.assertTrue(branch_tag.endswith(f"-{expected_hash}"))
                    self.assertNotIn("photospider-ci:latest", calls)
            self.assertTrue(flat_outputs["branch_tag"].startswith("branch-CI-a-b-"))
            self.assertTrue(nested_outputs["branch_tag"].startswith("branch-CI-a-b-"))
            self.assertLessEqual(len(long_outputs["branch_tag"]), 128)
            self.assertEqual(repeated_inspected, successful["ci-flat"][2])
            self.assertEqual(
                created_tag_groups(repeated_calls),
                [[f"{image_repository}:{flat_outputs['branch_tag']}"]],
            )
            self.assertIn("--prefer-index=false", repeated_calls)

            promotion_source = (SCRIPTS / "ci_image_promote.sh").read_text(
                encoding="utf-8"
            )
            self.assertNotIn("gh attestation", promotion_source)
            self.assertNotIn("docker build ", promotion_source)

            invalid_branches = ("", "CI/.hidden", "CI/foo.", "CI/foo.lock")
            for index, branch in enumerate(invalid_branches):
                run_command(
                    "git",
                    "check-ref-format",
                    f"refs/heads/{branch}",
                    expect_success=False,
                )
                rejected, _, calls, inspected = invoke(
                    branch, f"invalid-branch-{index}", expect_success=False
                )
                self.assertIn("canonical CI/** branch", rejected.stderr)
                self.assertEqual(calls, "")
                self.assertEqual(inspected, [])

            rejected_event, _, calls, inspected = invoke(
                "main",
                "pull-request-target",
                event_name="pull_request_target",
                expect_success=False,
            )
            self.assertIn("Only a trusted push", rejected_event.stderr)
            self.assertEqual(calls, "")
            self.assertEqual(inspected, [])

            mismatched_source, _, calls, inspected = invoke(
                "main",
                "source-mismatch",
                source_commit=COMMIT_B,
                expect_success=False,
            )
            self.assertIn("candidate/source commits", mismatched_source.stderr)
            self.assertEqual(calls, "")
            self.assertEqual(inspected, [])

            wrong_metadata, _, calls, inspected = invoke(
                "main",
                "wrong-metadata",
                expect_success=False,
                environment_overrides={
                    "CI_TEST_METADATA_DIGEST": base_environment[
                        "CI_TEST_WRONG_DIGEST"
                    ]
                },
            )
            self.assertIn("Mutable promotion metadata differs", wrong_metadata.stderr)
            self.assertIn("buildx imagetools create", calls)
            self.assertEqual(
                inspected, [f"{image_repository}:sha-{commit_a}"]
            )

            branch_destination = f"{image_repository}:branch-main"
            sha_destination_a = f"{image_repository}:sha-{commit_a}"

            immutable_state = registry_state()
            digest_conflict, _, calls, inspected = invoke(
                "CI/a-b",
                "same-commit-cross-ref-different-digest",
                image_digest=base_environment["CI_TEST_WRONG_DIGEST"],
                expect_success=False,
            )
            self.assertIn(
                "Immutable SHA tag already resolves to a different digest",
                digest_conflict.stderr,
            )
            self.assertEqual(inspected, [sha_destination_a])
            self.assertNotIn("buildx imagetools create", calls)
            self.assertEqual(registry_state(), immutable_state)

            immutable_preflight_failures = (
                ("no-digest", "inspected manifest lacks"),
                ("multiple", "duplicate JSON member: digest"),
                ("auth", "without one exact not-found result"),
                ("network", "without one exact not-found result"),
                ("not-found-drift", "without one exact not-found result"),
                ("unknown", "without one exact not-found result"),
            )
            for mode, diagnostic in immutable_preflight_failures:
                with self.subTest(immutable_sha_inspect=mode):
                    state_before = registry_state()
                    failed, _, calls, inspected = invoke(
                        "main",
                        f"immutable-preflight-{mode}",
                        expect_success=False,
                        environment_overrides={
                            "CI_TEST_INSPECT_MODE": mode,
                            "CI_TEST_INSPECT_TARGET": sha_destination_a,
                        },
                    )
                    self.assertIn(diagnostic, failed.stderr)
                    self.assertEqual(inspected, [sha_destination_a])
                    self.assertNotIn("buildx imagetools create", calls)
                    self.assertEqual(registry_state(), state_before)

            mutable_inspect_failures = (
                (
                    "wrong",
                    branch_destination,
                    [sha_destination_a, branch_destination],
                    "does not resolve to the tested digest",
                ),
                (
                    "missing",
                    latest_destination,
                    [sha_destination_a, branch_destination, latest_destination],
                    "could not be inspected",
                ),
                (
                    "multiple",
                    branch_destination,
                    [sha_destination_a, branch_destination],
                    "duplicate JSON member: digest",
                ),
            )
            for mode, target, expected_inspected, diagnostic in (
                mutable_inspect_failures
            ):
                failed, _, calls, inspected = invoke(
                    "main",
                    f"mutable-inspect-{mode}",
                    expect_success=False,
                    environment_overrides={
                        "CI_TEST_INSPECT_MODE": mode,
                        "CI_TEST_INSPECT_TARGET": target,
                    },
                )
                self.assertIn(diagnostic, failed.stderr)
                self.assertEqual(inspected, expected_inspected)
                self.assertEqual(inspected.count(target), 1)
                self.assertEqual(
                    calls.count("buildx imagetools inspect"), len(inspected)
                )

            run_local("git", "checkout", "--detach", commit_a, cwd=repository)
            dockerignore = repository / ".dockerignore"
            dockerignore.write_text(
                dockerignore.read_text(encoding="utf-8")
                + "\n# promotion freshness identity B\n",
                encoding="utf-8",
            )
            run_local("git", "add", ".dockerignore", cwd=repository)
            run_local(
                "git", "commit", "-m", "fixture: image identity B", cwd=repository
            )
            commit_b = run_local(
                "git", "rev-parse", "HEAD^{commit}", cwd=repository
            ).stdout.strip()
            manifest_b = manifest_for(commit_b, "identity-b")
            image_digest_b = "sha256:" + "8" * 64
            update_remote("main", commit_b)

            _, outputs_b, _, _ = invoke(
                "main",
                "main-b-after-a",
                candidate_commit=commit_b,
                manifest_digest=manifest_b,
                image_digest=image_digest_b,
            )
            self.assertIsNotNone(outputs_b)
            self.assertEqual(outputs_b["status"], "promoted")
            state = registry_state()
            self.assertEqual(state[branch_destination], image_digest_b)
            self.assertEqual(state[latest_destination], image_digest_b)
            self.assertEqual(
                state[f"{image_repository}:sha-{commit_b}"], image_digest_b
            )

            _, stale_a, _, stale_inspected = invoke(
                "main", "main-a-after-b"
            )
            self.assertIsNotNone(stale_a)
            self.assertEqual(stale_a["status"], "superseded")
            self.assertEqual(stale_inspected, [sha_destination_a])
            state = registry_state()
            self.assertEqual(state[branch_destination], image_digest_b)
            self.assertEqual(state[latest_destination], image_digest_b)
            self.assertEqual(state[sha_destination_a], IMAGE_DIGEST)

            manifest_mismatch, _, calls, inspected = invoke(
                "CI/a-b",
                "candidate-manifest-mismatch",
                manifest_digest="0" * 64,
                expect_success=False,
            )
            self.assertIn(
                "manifest differs for the candidate source commit",
                manifest_mismatch.stderr,
            )
            self.assertEqual(calls, "")
            self.assertEqual(inspected, [])

            update_remote("CI/pending-new-image", commit_b)
            _, pending_a, _, pending_inspected = invoke(
                "CI/pending-new-image", "pending-new-image-a"
            )
            self.assertIsNotNone(pending_a)
            self.assertEqual(pending_a["status"], "superseded")
            self.assertEqual(pending_inspected, [sha_destination_a])
            pending_destination = (
                f"{image_repository}:{pending_a['branch_tag']}"
            )
            self.assertNotIn(pending_destination, registry_state())

            run_local("git", "checkout", "--detach", commit_a, cwd=repository)
            (repository / "docs-only-note.md").write_text(
                "documentation-only branch advance\n", encoding="utf-8"
            )
            run_local("git", "add", "docs-only-note.md", cwd=repository)
            run_local(
                "git", "commit", "-m", "docs: advance without image input", cwd=repository
            )
            docs_commit = run_local(
                "git", "rev-parse", "HEAD^{commit}", cwd=repository
            ).stdout.strip()
            update_remote("CI/docs-only", docs_commit)
            _, docs_outputs, _, _ = invoke("CI/docs-only", "docs-only-a")
            self.assertIsNotNone(docs_outputs)
            self.assertEqual(docs_outputs["status"], "promoted")
            self.assertEqual(
                registry_state()[
                    f"{image_repository}:{docs_outputs['branch_tag']}"
                ],
                IMAGE_DIGEST,
            )

            empty_tree = run_local(
                "git", "mktree", cwd=repository, input_text=""
            ).stdout.strip()
            unrelated_commit = run_local(
                "git",
                "commit-tree",
                empty_tree,
                "-m",
                "unrelated force-push tip",
                cwd=repository,
            ).stdout.strip()
            update_remote("CI/force-push", unrelated_commit)
            force_failed, _, calls, inspected = invoke(
                "CI/force-push", "force-push-a", expect_success=False
            )
            self.assertIn("not an ancestor", force_failed.stderr)
            self.assertEqual(calls, "")
            self.assertEqual(inspected, [])

            run_local("git", "checkout", "--detach", commit_a, cwd=repository)
            (repository / ".dockerignore").unlink()
            run_local("git", "add", "--all", cwd=repository)
            run_local(
                "git", "commit", "-m", "fixture: missing image input", cwd=repository
            )
            missing_input_commit = run_local(
                "git", "rev-parse", "HEAD^{commit}", cwd=repository
            ).stdout.strip()
            update_remote("CI/missing-input", missing_input_commit)
            manifest_failed, _, calls, inspected = invoke(
                "CI/missing-input", "missing-input-a", expect_success=False
            )
            self.assertIn("image input is not a regular file", manifest_failed.stderr)
            self.assertEqual(calls, "")
            self.assertEqual(inspected, [])

            update_remote("CI/ref-drift", commit_a)
            drift_failed, _, calls, inspected = invoke(
                "CI/ref-drift",
                "ref-drift-a",
                expect_success=False,
                environment_overrides={
                    "CI_TEST_GIT_DRIFT_COUNTER": str(root / "drift-fetch-count"),
                    "CI_TEST_GIT_DRIFT_NEXT": commit_b,
                    "CI_TEST_GIT_DRIFT_REF": "refs/heads/CI/ref-drift",
                    "CI_TEST_GIT_DRIFT_REPOSITORY": str(origin),
                },
            )
            self.assertIn("changed during promotion freshness", drift_failed.stderr)
            self.assertEqual(calls, "")
            self.assertEqual(inspected, [])


class CommandTimeoutContractTest(unittest.TestCase):
    """Exercise the portable total-deadline primitive used by fuzz profiles."""

    def test_deadline_terminates_a_real_process_group(self) -> None:
        """Return 124 promptly when a live child exceeds its declared bound."""
        started = time.monotonic()
        failed = run_command(
            "python3", SCRIPTS / "ci_command_timeout.py",
            "--timeout-seconds", "1",
            "--label", "fuzz-contract-test",
            "--", sys.executable, "-c", "import time; time.sleep(30)",
            expect_success=False,
        )
        elapsed = time.monotonic() - started
        self.assertEqual(failed.returncode, 124)
        self.assertLess(elapsed, 8)
        self.assertIn("exceeded its declared 1-second job timeout", failed.stderr)

    def test_fuzz_runner_wraps_configure_build_and_keeps_input_timeout(self) -> None:
        """Bind the matrix job bound before work while retaining libFuzzer timeout."""
        runner = (SCRIPTS / "fuzz_smoke.sh").read_text(encoding="utf-8")
        wrapper = runner.index('exec python3 "$SCRIPT_DIR/ci_command_timeout.py"')
        self.assertLess(wrapper, runner.index("run_logged configure_fuzz_codecs"))
        self.assertIn('--timeout-minutes "$job_timeout_minutes"', runner)
        self.assertIn('-- bash "$SCRIPT_DIR/fuzz_smoke.sh" --bounded-worker', runner)
        self.assertIn('"-timeout=$timeout"', runner)


class RunnerIdentityContractTest(unittest.TestCase):
    """Exercise exact hosted-runner image and architecture readback."""

    def test_linux_runner_exact_identity_passes_and_rotation_fails(self) -> None:
        """Accept the protected image version and reject one rotated value."""
        module_path = SCRIPTS / "ci_runner_verify.py"
        specification = importlib.util.spec_from_file_location("ci_runner_verify_test", module_path)
        self.assertIsNotNone(specification)
        self.assertIsNotNone(specification.loader)
        module = importlib.util.module_from_spec(specification)
        specification.loader.exec_module(module)
        exact_environment = {"ImageOS": "ubuntu24", "ImageVersion": "20260816.277.1"}
        with (
            mock.patch.object(module.platform, "system", return_value="Linux"),
            mock.patch.object(module.platform, "machine", return_value="x86_64"),
            mock.patch.dict(module.os.environ, exact_environment, clear=False),
        ):
            verified = module.verify(REPO_ROOT, "Linux", "ubuntu-24.04")
        self.assertEqual(verified["runner_label"], "ubuntu-24.04")
        with (
            mock.patch.object(module.platform, "system", return_value="Linux"),
            mock.patch.object(module.platform, "machine", return_value="x86_64"),
            mock.patch.dict(
                module.os.environ,
                {"ImageOS": "ubuntu24", "ImageVersion": "rotated"},
                clear=False,
            ),
            self.assertRaises(module.RunnerError),
        ):
            module.verify(REPO_ROOT, "Linux", "ubuntu-24.04")

        with (
            mock.patch.object(module.platform, "system", return_value="Linux"),
            mock.patch.object(module.platform, "machine", return_value="x86_64"),
            mock.patch.dict(module.os.environ, exact_environment, clear=False),
            self.assertRaises(module.RunnerError),
        ):
            module.verify(REPO_ROOT, "Linux", "ubuntu-latest")

    def test_darwin_runner_uses_one_arm64_label_and_triplet_identity(self) -> None:
        """Accept macos-15 arm64 and reject the former x86_64 interpretation."""
        module_path = SCRIPTS / "ci_runner_verify.py"
        specification = importlib.util.spec_from_file_location("ci_runner_verify_darwin_test", module_path)
        self.assertIsNotNone(specification)
        self.assertIsNotNone(specification.loader)
        module = importlib.util.module_from_spec(specification)
        specification.loader.exec_module(module)
        exact_environment = {"ImageOS": "macos15", "ImageVersion": "20260727.0256.1"}
        with (
            mock.patch.object(module.platform, "system", return_value="Darwin"),
            mock.patch.object(module.platform, "machine", return_value="arm64"),
            mock.patch.dict(module.os.environ, exact_environment, clear=False),
        ):
            verified = module.verify(REPO_ROOT, "Darwin", "macos-15")
        self.assertEqual(verified["architecture"], "arm64")
        with (
            mock.patch.object(module.platform, "system", return_value="Darwin"),
            mock.patch.object(module.platform, "machine", return_value="x86_64"),
            mock.patch.dict(module.os.environ, exact_environment, clear=False),
            self.assertRaises(module.RunnerError),
        ):
            module.verify(REPO_ROOT, "Darwin", "macos-15")


class ProfileReaderContractTest(unittest.TestCase):
    """Exercise exact fallback, complete versioned input, and partial rejection."""

    @staticmethod
    def _write_complete_versioned_identity(inventory: Path) -> str:
        """Write one canonical complete versioned security identity fixture.

        Args:
            inventory: Existing directory that will receive all three identity
                files.

        Returns:
            The SHA-256 digest binding the matrix and security-role records.
        """
        matrix_records = {
            ("capability", "address-sanitizer"): {
                "default": False,
                "dependencies": [],
                "option": "USE_ASAN",
                "platforms": ["Darwin", "Linux"],
            },
            ("capability", "build-fuzzers"): {
                "default": False,
                "dependencies": [],
                "option": "PHOTOSPIDER_BUILD_FUZZERS",
                "platforms": ["Darwin", "Linux"],
            },
            ("capability", "build-testing"): {
                "default": True,
                "dependencies": [],
                "option": "BUILD_TESTING",
                "platforms": ["Darwin", "Linux"],
            },
            ("capability", "thread-sanitizer"): {
                "default": False,
                "dependencies": [],
                "option": "USE_TSAN",
                "platforms": ["Darwin", "Linux"],
            },
            ("component", "security-validation"): {
                "dependencies": ["openssl", "utf8proc"],
                "targets": ["fuzz_codec", "test_contract"],
            },
            ("dependency", "openssl"): {
                "cmake_package": "OpenSSL",
                "lock": "ci/locks/requirements-ci.txt",
                "mode": "required",
                "vcpkg_port": "openssl",
            },
            ("dependency", "utf8proc"): {
                "cmake_package": "utf8proc",
                "lock": "ci/locks/requirements-ci.txt",
                "mode": "required",
                "vcpkg_port": "utf8proc",
            },
            ("profile", "fuzz-codecs"): {
                "cmake_args": [
                    "-DBUILD_TESTING=OFF",
                    "-DPHOTOSPIDER_BUILD_FUZZERS=ON",
                    "-DUSE_ASAN=OFF",
                    "-DUSE_TSAN=OFF",
                ],
                "components": ["security-validation"],
                "dependencies": ["openssl", "utf8proc"],
                "platforms": ["Darwin", "Linux"],
                "roles": ["fuzz-artifact", "fuzz-codec"],
            },
            ("profile", "sanitizer-asan"): {
                "cmake_args": [
                    "-DBUILD_TESTING=ON",
                    "-DPHOTOSPIDER_BUILD_FUZZERS=OFF",
                    "-DUSE_ASAN=ON",
                    "-DUSE_TSAN=OFF",
                ],
                "components": ["security-validation"],
                "dependencies": ["openssl", "utf8proc"],
                "platforms": ["Darwin", "Linux"],
                "roles": ["sanitizer-artifact", "sanitizer-target", "security-label"],
            },
            ("profile", "sanitizer-tsan"): {
                "cmake_args": [
                    "-DBUILD_TESTING=ON",
                    "-DPHOTOSPIDER_BUILD_FUZZERS=OFF",
                    "-DUSE_ASAN=OFF",
                    "-DUSE_TSAN=ON",
                ],
                "components": ["security-validation"],
                "dependencies": ["openssl", "utf8proc"],
                "platforms": ["Darwin", "Linux"],
                "roles": ["sanitizer-artifact", "sanitizer-target", "security-label"],
            },
            ("role", "fuzz-artifact"): {"kind": "artifact", "selector": "fuzz-smoke"},
            ("role", "fuzz-codec"): {
                "corpus_sha256": None,
                "job_timeout_minutes": 20,
                "kind": "fuzz-target",
                "max_len": 65536,
                "production_limit": 65536,
                "production_limit_symbol": "ps::codec::kLimit",
                "runs": 1000,
                "seed": 1,
                "selector": "fuzz_codec",
                "timeout_seconds": 2,
            },
            ("role", "sanitizer-artifact"): {
                "kind": "artifact",
                "selector": "sanitizer-smoke",
            },
            ("role", "sanitizer-target"): {"kind": "target", "selector": "test_contract"},
            ("role", "security-label"): {"kind": "ctest-label", "selector": "security"},
            ("target", "fuzz_codec"): {
                "capabilities": ["build-fuzzers"],
                "component": "security-validation",
            },
            ("target", "test_contract"): {
                "capabilities": ["build-testing"],
                "component": "security-validation",
            },
        }
        encoded_records = [
            f"{kind}\t{identifier}\t"
            + json.dumps(value, sort_keys=True, separators=(",", ":"), ensure_ascii=True)
            for (kind, identifier), value in matrix_records.items()
        ]
        matrix = inventory / "build_profile_matrix_v1.tsv"
        matrix.write_text(
            "schema\tphotospider-build-profile-matrix-v1\n"
            + "\n".join(sorted(encoded_records))
            + "\n",
            encoding="utf-8",
        )
        digest = hashlib.sha256(matrix.read_bytes()).hexdigest()
        (inventory / "build_profile_matrix_v1.tsv.sha256").write_text(
            f"{digest}  build_profile_matrix_v1.tsv\n",
            encoding="utf-8",
        )
        records = [
            "fuzz\tfuzz-codecs\tfuzz-codec",
            "sanitizer\tsanitizer-asan\t[\"sanitizer-target\"]\t[\"security-label\"]",
            "sanitizer\tsanitizer-tsan\t[\"sanitizer-target\"]\t[\"security-label\"]",
        ]
        (inventory / "ci_security_roles_v1.tsv").write_text(
            "schema\tphotospider-ci-security-roles-v1\n"
            f"matrix_sha256\t{digest}\n"
            + "\n".join(sorted(records)) + "\n",
            encoding="utf-8",
        )
        return digest

    @staticmethod
    def _refresh_versioned_digest(inventory: Path) -> str:
        """Rebind sidecar and roles header after one deliberate matrix mutation."""
        matrix = inventory / "build_profile_matrix_v1.tsv"
        digest = hashlib.sha256(matrix.read_bytes()).hexdigest()
        (inventory / "build_profile_matrix_v1.tsv.sha256").write_text(
            f"{digest}  build_profile_matrix_v1.tsv\n", encoding="utf-8"
        )
        roles = inventory / "ci_security_roles_v1.tsv"
        lines = roles.read_text(encoding="utf-8").splitlines()
        lines[1] = f"matrix_sha256\t{digest}"
        roles.write_text("\n".join(lines) + "\n", encoding="utf-8")
        return digest

    def test_current_main_fallback_is_hash_bound(self) -> None:
        """Resolve current main only through the explicitly identified fallback."""
        completed = run_command(
            "python3", SCRIPTS / "ci_profile_manifest.py",
            "--profile", "fuzz-codecs",
        )
        resolved = json.loads(completed.stdout)
        self.assertTrue(resolved["fallback"])
        self.assertEqual(resolved["profile"]["seed"], 1)
        self.assertEqual(resolved["profile"]["runs"], 1000)
        self.assertEqual(
            [target["max_len"] for target in resolved["profile"]["targets"]],
            [65536, 131072],
        )

    def test_partial_versioned_identity_fails_closed(self) -> None:
        """Reject any subset of the three required versioned identity files."""
        with tempfile.TemporaryDirectory() as temporary_text:
            inventory = Path(temporary_text)
            (inventory / "build_profile_matrix_v1.tsv").write_text(
                "schema\tphotospider-build-profile-matrix-v1\n",
                encoding="utf-8",
            )
            failed = run_command(
                "python3", SCRIPTS / "ci_profile_manifest.py",
                "--inventory-dir", inventory,
                expect_success=False,
            )
            self.assertIn("partial versioned profile identity", failed.stderr)

    def test_staging_clears_complete_stale_identity_before_fallback(self) -> None:
        """Never reuse a valid old output set when the current source is empty."""
        with tempfile.TemporaryDirectory() as temporary_text:
            root = Path(temporary_text)
            source = root / "empty-source"
            output = root / "artifact"
            source.mkdir()
            output.mkdir()
            self._write_complete_versioned_identity(output)
            resolved_path = output / "resolved-security-profiles.json"
            before = run_command(
                "python3", SCRIPTS / "ci_profile_manifest.py",
                "--inventory-dir", output,
                "--output", resolved_path,
            )
            self.assertEqual(before.returncode, 0)
            self.assertFalse(json.loads(resolved_path.read_text(encoding="utf-8"))["fallback"])

            run_command(
                "bash", SCRIPTS / "ci_profile_inventory_stage.sh", source, output,
            )
            for identity_name in (
                "build_profile_matrix_v1.tsv",
                "build_profile_matrix_v1.tsv.sha256",
                "ci_security_roles_v1.tsv",
            ):
                self.assertFalse((output / identity_name).exists())
            resolved = json.loads(resolved_path.read_text(encoding="utf-8"))
            self.assertTrue(resolved["fallback"])
            self.assertEqual(resolved["schema"], "photospider-resolved-ci-security-v1")

    def test_staging_rejects_symlink_and_special_output_targets(self) -> None:
        """Fail before clearing when an exact output name is not a regular file."""
        for unsafe_kind in ("symlink", "fifo"):
            with self.subTest(unsafe_kind=unsafe_kind), tempfile.TemporaryDirectory() as temporary_text:
                root = Path(temporary_text)
                source = root / "source"
                output = root / "output"
                source.mkdir()
                output.mkdir()
                unsafe = output / "resolved-security-profiles.json"
                if unsafe_kind == "symlink":
                    outside = root / "outside.json"
                    outside.write_text("preserve\n", encoding="utf-8")
                    unsafe.symlink_to(outside)
                else:
                    os.mkfifo(unsafe)
                failed = run_command(
                    "bash", SCRIPTS / "ci_profile_inventory_stage.sh", source, output,
                    expect_success=False,
                )
                self.assertIn("Unsafe existing staged profile artifact", failed.stderr)

    def test_fallback_rejects_changed_candidate_authority(self) -> None:
        """Reject the temporary lists as soon as one hash-bound source changes."""
        with tempfile.TemporaryDirectory() as temporary_text:
            root = Path(temporary_text)
            lock_source = REPO_ROOT / "ci/locks/current-main-profiles-v1.json"
            lock_target = root / "ci/locks/current-main-profiles-v1.json"
            lock_target.parent.mkdir(parents=True)
            shutil.copyfile(lock_source, lock_target)
            lock = json.loads(lock_source.read_text(encoding="utf-8"))
            for relative in lock["source_hashes"]:
                destination = root / relative
                destination.parent.mkdir(parents=True, exist_ok=True)
                shutil.copyfile(REPO_ROOT / relative, destination)
            with (root / "CMakeLists.txt").open("a", encoding="utf-8") as handle:
                handle.write("\n# drift\n")
            failed = run_command(
                "python3", SCRIPTS / "ci_profile_manifest.py",
                "--repo-root", root,
                "--inventory-dir", root / "empty-inventory",
                expect_success=False,
            )
            self.assertIn("fallback is stale for CMakeLists.txt", failed.stderr)

    def test_fallback_rejects_each_production_packet_limit_drift(self) -> None:
        """Bind both production size-limit headers to fallback fuzz bounds."""
        relative_headers = (
            "src/lib/execution/isolation/isolated_cpu_invocation_protocol.hpp",
            "src/lib/server/worker/worker_protocol.hpp",
        )
        for relative_header in relative_headers:
            with self.subTest(relative_header=relative_header), tempfile.TemporaryDirectory() as temporary_text:
                root = Path(temporary_text)
                lock_source = REPO_ROOT / "ci/locks/current-main-profiles-v1.json"
                lock_target = root / "ci/locks/current-main-profiles-v1.json"
                lock_target.parent.mkdir(parents=True)
                shutil.copyfile(lock_source, lock_target)
                lock = json.loads(lock_source.read_text(encoding="utf-8"))
                for relative in lock["source_hashes"]:
                    destination = root / relative
                    destination.parent.mkdir(parents=True, exist_ok=True)
                    shutil.copyfile(REPO_ROOT / relative, destination)
                with (root / relative_header).open("a", encoding="utf-8") as handle:
                    handle.write("\n// packet limit drift\n")
                failed = run_command(
                    "python3", SCRIPTS / "ci_profile_manifest.py",
                    "--repo-root", root,
                    "--inventory-dir", root / "empty-inventory",
                    expect_success=False,
                )
                self.assertIn(f"fallback is stale for {relative_header}", failed.stderr)

    def test_complete_versioned_identity_is_cross_digest_bound(self) -> None:
        """Accept canonical roles only when their matrix sidecar and digest agree."""
        with tempfile.TemporaryDirectory() as temporary_text:
            inventory = Path(temporary_text)
            digest = self._write_complete_versioned_identity(inventory)
            roles = inventory / "ci_security_roles_v1.tsv"
            completed = run_command(
                "python3", SCRIPTS / "ci_profile_manifest.py",
                "--inventory-dir", inventory,
            )
            resolved = json.loads(completed.stdout)
            self.assertFalse(resolved["fallback"])
            self.assertEqual(resolved["matrix_sha256"], digest)
            self.assertEqual(len(resolved["profiles"]), 3)
            fuzz = next(profile for profile in resolved["profiles"] if profile["profile"] == "fuzz-codecs")
            self.assertEqual(
                fuzz["cmake_args"],
                [
                    "-DBUILD_TESTING=OFF",
                    "-DPHOTOSPIDER_BUILD_FUZZERS=ON",
                    "-DUSE_ASAN=OFF",
                    "-DUSE_TSAN=OFF",
                ],
            )

            roles.write_text(roles.read_text(encoding="utf-8").replace(digest, "0" * 64), encoding="utf-8")
            failed = run_command(
                "python3", SCRIPTS / "ci_profile_manifest.py",
                "--inventory-dir", inventory,
                expect_success=False,
            )
            self.assertIn("matrix digest does not match", failed.stderr)

    def test_versioned_matrix_rejects_malformed_records_and_isolation(self) -> None:
        """Reject bad fields, IDs, platforms, bounds, references, and isolation."""
        mutations = (
            ("\"roles\":[\"fuzz-artifact\",\"fuzz-codec\"]", "\"roles\":[\"fuzz-artifact\",\"unknown-role\"]", "unknown role"),
            ("-DUSE_ASAN=ON", "-DUSE_ASAN=OFF", "CMake isolation"),
            ("role\tfuzz-codec\t", "role\tfuzz-codec_\t", "invalid identifier"),
            ("\"seed\":1", "\"seed\":2", "requires seed 1"),
            ("\"timeout_seconds\":2}", "\"timeout_seconds\":2,\"unknown\":true}", "fields differ"),
            ("\"platforms\":[\"Darwin\",\"Linux\"]", "\"platforms\":[\"Linux\",\"Windows\"]", "unsupported capability platform"),
        )
        for old, new, diagnostic in mutations:
            with self.subTest(diagnostic=diagnostic), tempfile.TemporaryDirectory() as temporary_text:
                inventory = Path(temporary_text)
                self._write_complete_versioned_identity(inventory)
                matrix = inventory / "build_profile_matrix_v1.tsv"
                original = matrix.read_text(encoding="utf-8")
                self.assertIn(old, original)
                matrix.write_text(original.replace(old, new, 1), encoding="utf-8")
                self._refresh_versioned_digest(inventory)
                failed = run_command(
                    "python3", SCRIPTS / "ci_profile_manifest.py",
                    "--inventory-dir", inventory,
                    expect_success=False,
                )
                self.assertIn(diagnostic, failed.stderr)

    def test_versioned_matrix_rejects_noncanonical_record_order(self) -> None:
        """Reject a byte-valid matrix whose semantic records are reordered."""
        with tempfile.TemporaryDirectory() as temporary_text:
            inventory = Path(temporary_text)
            self._write_complete_versioned_identity(inventory)
            matrix = inventory / "build_profile_matrix_v1.tsv"
            lines = matrix.read_text(encoding="utf-8").splitlines()
            lines[1], lines[2] = lines[2], lines[1]
            matrix.write_text("\n".join(lines) + "\n", encoding="utf-8")
            self._refresh_versioned_digest(inventory)
            failed = run_command(
                "python3", SCRIPTS / "ci_profile_manifest.py",
                "--inventory-dir", inventory,
                expect_success=False,
            )
            self.assertIn("not bytewise sorted", failed.stderr)

    def test_versioned_fuzz_matrix_arguments_reach_real_runner_boundary(self) -> None:
        """Carry matrix arguments through the real CMake fuzz-output boundary."""
        with tempfile.TemporaryDirectory() as temporary_text:
            root = Path(temporary_text)
            inventory = root / "inventory"
            inventory.mkdir()
            self._write_complete_versioned_identity(inventory)
            binary_dir = root / "bin"
            binary_dir.mkdir()
            configure_log = root / "configure.log"
            fuzz_log = root / "fuzz.log"
            uname = binary_dir / "uname"
            uname.write_text(
                "#!/usr/bin/env bash\n"
                "[[ ${1:-} == -s ]] && { echo Linux; exit 0; }\n"
                "[[ ${1:-} == -m ]] && { echo x86_64; exit 0; }\n"
                "exit 1\n",
                encoding="utf-8",
            )
            uname.chmod(0o755)
            cmake = binary_dir / "cmake"
            cmake.write_text(
                "#!/usr/bin/env bash\n"
                "set -Eeuo pipefail\n"
                "if [[ ${1:-} == --build && ${4:-} == help ]]; then\n"
                "  echo '... fuzz_codec'; exit 0\n"
                "fi\n"
                "if [[ ${1:-} == --build ]]; then exit 0; fi\n"
                "printf '%q ' \"$@\" > \"$CI_TEST_CONFIGURE_LOG\"; printf '\\n' >> \"$CI_TEST_CONFIGURE_LOG\"\n"
                "build_dir=\n"
                "while (($#)); do [[ $1 == -B ]] && { build_dir=$2; break; }; shift; done\n"
                "mkdir -p \"$build_dir/fuzzers\"\n"
                "printf '%s\\n' '#!/usr/bin/env bash' 'printf '\"'\"'%q '\"'\"' \"$@\" >> \"$CI_TEST_FUZZ_LOG\"; printf '\"'\"'\\n'\"'\"' >> \"$CI_TEST_FUZZ_LOG\"' > \"$build_dir/fuzzers/fuzz_codec\"\n"
                "chmod +x \"$build_dir/fuzzers/fuzz_codec\"\n",
                encoding="utf-8",
            )
            cmake.chmod(0o755)
            artifacts = root / "artifacts"
            run_command(
                "bash", SCRIPTS / "fuzz_smoke.sh",
                environment={
                    "BUILD_DIR": str(root / "build"),
                    "CI_ARTIFACT_DIR": str(artifacts),
                    "CI_INVENTORY_DIR": str(inventory),
                    "CI_JOBS": "1",
                    "CI_TEST_CONFIGURE_LOG": str(configure_log),
                    "CI_TEST_FUZZ_LOG": str(fuzz_log),
                    "PATH": f"{binary_dir}:{os.environ['PATH']}",
                },
            )
            configured = configure_log.read_text(encoding="utf-8")
            for argument in (
                "-DBUILD_TESTING=OFF",
                "-DPHOTOSPIDER_BUILD_FUZZERS=ON",
                "-DUSE_ASAN=OFF",
                "-DUSE_TSAN=OFF",
            ):
                self.assertIn(argument, configured)
            fuzzed = fuzz_log.read_text(encoding="utf-8")
            for argument in ("-seed=1", "-runs=1000", "-timeout=2", "-max_len=65536"):
                self.assertIn(argument, fuzzed)
            runner = (SCRIPTS / "fuzz_smoke.sh").read_text(encoding="utf-8")
            self.assertIn("binary=$BUILD_DIR/fuzzers/$target", runner)
            self.assertNotIn("binary=$BUILD_DIR/tests/$target", runner)


class SecurityPlatformContractTest(unittest.TestCase):
    """Exercise Darwin runner locks and fresh vcpkg materialization."""

    @staticmethod
    def _prepare_darwin_fixture(
        root: Path,
    ) -> tuple[Path, dict[str, str], Path, Path, Path, Path]:
        """Create one exact Darwin runner, Git-object, and binary fixture.

        Args:
            root: Empty temporary directory that owns all fixture state.

        Returns:
            The resolved profile, base environment, Git log, vcpkg log,
            runner-temp directory, and deliberately dirty preinstalled source.

        Note:
            The Git shim implements only the read/fetch/checkout/status boundary
            used by ``security_platform_prepare.sh``. It reports the source as
            dirty if a caller inspects worktree diffs, while the exact locked
            commit object remains available for a clean fresh checkout.
        """
        profile = root / "profile.json"
        run_command(
            "python3", SCRIPTS / "ci_profile_manifest.py",
            "--profile", "fuzz-codecs",
            "--output", profile,
        )
        binary_dir = root / "bin"
        binary_dir.mkdir()
        uname = binary_dir / "uname"
        uname.write_text(
            "#!/usr/bin/env bash\n"
            "[[ ${1:-} == -s ]] && { echo Darwin; exit 0; }\n"
            "[[ ${1:-} == -m ]] && { echo arm64; exit 0; }\n"
            "exit 1\n",
            encoding="utf-8",
        )
        uname.chmod(0o755)

        git_log = root / "git.log"
        git = binary_dir / "git"
        git.write_text(
            f"#!{sys.executable}\n"
            "import json\n"
            "import os\n"
            "import sys\n"
            "from pathlib import Path\n"
            "args = sys.argv[1:]\n"
            "with open(os.environ['CI_TEST_GIT_LOG'], 'a', encoding='utf-8') as handle:\n"
            "    handle.write(json.dumps(args, separators=(',', ':')) + '\\n')\n"
            "worktree = None\n"
            "index = 0\n"
            "while index < len(args):\n"
            "    if args[index] == '-C':\n"
            "        worktree = Path(args[index + 1])\n"
            "        index += 2\n"
            "    elif args[index] == '-c':\n"
            "        index += 2\n"
            "    elif args[index].startswith('-'):\n"
            "        index += 1\n"
            "    else:\n"
            "        break\n"
            "command = args[index] if index < len(args) else ''\n"
            "command_args = args[index + 1:]\n"
            "source = Path(os.environ['CI_TEST_VCPKG_SOURCE'])\n"
            "locked = os.environ['CI_TEST_LOCKED_COMMIT']\n"
            "if command == 'init':\n"
            "    target = Path(command_args[-1])\n"
            "    (target / '.git').mkdir(parents=True)\n"
            "elif command == 'cat-file':\n"
            "    raise SystemExit(0 if os.environ.get('CI_TEST_SOURCE_HAS_COMMIT', '1') == '1' else 1)\n"
            "elif command == 'checkout':\n"
            "    toolchain = worktree / 'scripts/buildsystems/vcpkg.cmake'\n"
            "    toolchain.parent.mkdir(parents=True)\n"
            "    toolchain.write_text('# locked fixture\\n', encoding='utf-8')\n"
            "    unsafe = os.environ.get('CI_TEST_UNSAFE_FRESH', '')\n"
            "    if unsafe == 'symlink':\n"
            "        (worktree / 'unsafe-link').symlink_to(source / 'tracked-dirty')\n"
            "    elif unsafe == 'fifo':\n"
            "        os.mkfifo(worktree / 'unsafe-fifo')\n"
            "    elif unsafe == 'residual':\n"
            "        (worktree / 'residual-state').write_text('unexpected\\n', encoding='utf-8')\n"
            "elif command == 'rev-parse':\n"
            "    print(os.environ.get('CI_TEST_FRESH_COMMIT', locked) if worktree != source else locked)\n"
            "elif command == 'status':\n"
            "    if worktree == source:\n"
            "        print(' M tracked-dirty')\n"
            "    elif os.environ.get('CI_TEST_UNSAFE_FRESH') == 'residual':\n"
            "        print('?? residual-state')\n"
            "elif command == 'diff' and worktree == source:\n"
            "    raise SystemExit(1)\n",
            encoding="utf-8",
        )
        git.chmod(0o755)

        vcpkg_source = root / "preinstalled-vcpkg"
        (vcpkg_source / ".git").mkdir(parents=True)
        (vcpkg_source / "tracked-dirty").write_text("dirty runner source\n", encoding="utf-8")
        vcpkg_log = root / "vcpkg.log"
        vcpkg = vcpkg_source / "vcpkg"
        vcpkg.write_text(
            "#!/usr/bin/env bash\n"
            "set -Eeuo pipefail\n"
            "{\n"
            "  printf 'executable=%s\\n' \"$0\"\n"
            "  printf 'root=%s\\n' \"${VCPKG_ROOT:-}\"\n"
            "  printf 'binary=%s\\n' \"${VCPKG_BINARY_SOURCES:-}\"\n"
            "  printf 'asset=%s\\n' \"${X_VCPKG_ASSET_SOURCES:-}\"\n"
            "  for argument in \"$@\"; do printf 'arg=%s\\n' \"$argument\"; done\n"
            "} > \"$CI_TEST_VCPKG_LOG\"\n",
            encoding="utf-8",
        )
        vcpkg.chmod(0o755)
        runner_temp = root / "runner-temp"
        runner_temp.mkdir()
        output = root / "cmake-args.txt"
        locked_commit = "6d9d7df564a1ccdaa994e4ad39ccd4a32360867b"
        environment = {
            "PATH": f"{binary_dir}:{os.environ['PATH']}",
            "CI_PLATFORM_CMAKE_ARGS_FILE": str(output),
            "CI_RUNNER_TEMP": str(runner_temp),
            "CI_TEST_GIT_LOG": str(git_log),
            "CI_TEST_LOCKED_COMMIT": locked_commit,
            "CI_TEST_VCPKG_LOG": str(vcpkg_log),
            "CI_TEST_VCPKG_SOURCE": str(vcpkg_source),
            "ImageOS": "macos15",
            "ImageVersion": "20260727.0256.1",
            "RUNNER_IMAGE_NAME": "macos-15",
            "VCPKG_INSTALLATION_ROOT": str(vcpkg_source),
        }
        return profile, environment, git_log, vcpkg_log, runner_temp, vcpkg_source

    def test_dirty_source_produces_fresh_clean_checkout_and_exact_install(self) -> None:
        """Use a dirty image source only as locked objects, then install fresh."""
        with tempfile.TemporaryDirectory() as temporary_text:
            root = Path(temporary_text)
            profile, environment, git_log, vcpkg_log, runner_temp, source = (
                self._prepare_darwin_fixture(root)
            )
            run_command(
                "bash", SCRIPTS / "security_platform_prepare.sh", profile,
                environment=environment,
            )
            arguments = (root / "cmake-args.txt").read_text(encoding="utf-8")
            self.assertIn("-DVCPKG_TARGET_TRIPLET=arm64-osx", arguments)
            self.assertIn(str(runner_temp), arguments)
            self.assertNotIn(str(source / "scripts/buildsystems/vcpkg.cmake"), arguments)

            install_lines = vcpkg_log.read_text(encoding="utf-8").splitlines()
            executable = next(line.removeprefix("executable=") for line in install_lines if line.startswith("executable="))
            fresh_root = next(line.removeprefix("root=") for line in install_lines if line.startswith("root="))
            self.assertEqual(executable, f"{fresh_root}/vcpkg")
            self.assertTrue(Path(fresh_root).is_relative_to(runner_temp.resolve()))
            self.assertNotEqual(executable, str(source / "vcpkg"))
            self.assertIn("binary=clear", install_lines)
            self.assertIn("asset=clear", install_lines)
            install_arguments = [line.removeprefix("arg=") for line in install_lines if line.startswith("arg=")]
            self.assertEqual(
                install_arguments,
                [
                    "install",
                    "--triplet",
                    "arm64-osx",
                    "--x-install-root",
                    f"{fresh_root}/installed",
                    "--clean-after-build",
                    "openssl",
                    "utf8proc",
                ],
            )
            git_calls = [json.loads(line) for line in git_log.read_text(encoding="utf-8").splitlines()]
            source_identity = str(source.resolve())
            self.assertTrue(any("cat-file" in call and source_identity in call for call in git_calls))
            self.assertFalse(any("diff" in call and source_identity in call for call in git_calls))
            self.assertFalse(any("status" in call and source_identity in call for call in git_calls))

            failed = run_command(
                "bash", SCRIPTS / "security_platform_prepare.sh", profile,
                environment={
                    **environment,
                    "ImageVersion": "stale",
                    "CI_PLATFORM_CMAKE_ARGS_FILE": str(root / "stale.txt"),
                },
                expect_success=False,
            )
            self.assertIn("differs from protected", failed.stderr)

    def test_fresh_checkout_rejects_commit_link_fifo_and_residual_state(self) -> None:
        """Fail closed for every fresh-checkout identity and filesystem drift."""
        with tempfile.TemporaryDirectory() as temporary_text:
            root = Path(temporary_text)
            profile, environment, _, _, _, _ = self._prepare_darwin_fixture(root)
            cases = (
                ({"CI_TEST_FRESH_COMMIT": "2" * 40}, "differs from protected"),
                ({"CI_TEST_UNSAFE_FRESH": "symlink"}, "contains a link"),
                ({"CI_TEST_UNSAFE_FRESH": "fifo"}, "contains a special entry"),
                ({"CI_TEST_UNSAFE_FRESH": "residual"}, "contains residual or modified state"),
            )
            for index, (override, diagnostic) in enumerate(cases):
                with self.subTest(diagnostic=diagnostic):
                    failed = run_command(
                        "bash", SCRIPTS / "security_platform_prepare.sh", profile,
                        environment={
                            **environment,
                            **override,
                            "CI_PLATFORM_CMAKE_ARGS_FILE": str(root / f"unsafe-{index}.txt"),
                        },
                        expect_success=False,
                    )
                    self.assertIn(diagnostic, failed.stderr)

    def test_missing_preinstalled_object_uses_only_explicit_official_source(self) -> None:
        """Fetch the exact commit from microsoft/vcpkg when local objects lack it."""
        with tempfile.TemporaryDirectory() as temporary_text:
            root = Path(temporary_text)
            profile, environment, git_log, _, _, _ = self._prepare_darwin_fixture(root)
            run_command(
                "bash", SCRIPTS / "security_platform_prepare.sh", profile,
                environment={**environment, "CI_TEST_SOURCE_HAS_COMMIT": "0"},
            )
            calls = [json.loads(line) for line in git_log.read_text(encoding="utf-8").splitlines()]
            fetch = next(call for call in calls if "fetch" in call)
            self.assertIn("https://github.com/microsoft/vcpkg.git", fetch)
            self.assertIn(environment["CI_TEST_LOCKED_COMMIT"], fetch)

        workflow = (REPO_ROOT / ".github/workflows/ci-integration.yml").read_text(
            encoding="utf-8"
        )
        self.assertIn("CI_RUNNER_TEMP: ${{ runner.temp }}", workflow)
        self.assertNotIn("CI_DARWIN_VCPKG_INSTALLED:", workflow)

    def test_gtest_inventory_uses_canonicalized_actual_ctest_includes(self) -> None:
        """Accept canonical aliases and reject foreign CTest include records."""
        module = (REPO_ROOT / "cmake/PhotospiderCiInventory.cmake").as_posix()
        for foreign_include in (False, True):
            with self.subTest(foreign_include=foreign_include), tempfile.TemporaryDirectory() as temporary_text:
                root = Path(temporary_text)
                source = root / "source"
                build = root / "build"
                source.mkdir()
                (source / "main.cpp").write_text(
                    "int main() { return 0; }\n", encoding="utf-8"
                )
                include_expression = (
                    "${CMAKE_SOURCE_DIR}/alpha[1]_include.cmake"
                    if foreign_include
                    else "${CMAKE_BINARY_DIR}/alias/../alpha[1]_include.cmake"
                )
                (source / "CMakeLists.txt").write_text(
                    "cmake_minimum_required(VERSION 3.16)\n"
                    "project(ci_inventory_contract LANGUAGES CXX)\n"
                    f'include("{module}")\n'
                    "add_executable(alpha main.cpp)\n"
                    "add_executable(beta main.cpp)\n"
                    "set_property(TARGET alpha PROPERTY CTEST_DISCOVERED_TEST_COUNTER 1)\n"
                    "set_property(TARGET beta PROPERTY CTEST_DISCOVERED_TEST_COUNTER 1)\n"
                    "file(MAKE_DIRECTORY \"${CMAKE_BINARY_DIR}/alias\")\n"
                    "file(WRITE \"${CMAKE_BINARY_DIR}/alpha[1]_include.cmake\" \"\")\n"
                    "file(WRITE \"${CMAKE_BINARY_DIR}/beta[1]_include.cmake\" \"\")\n"
                    f'file(WRITE "{include_expression}" "")\n'
                    "set_property(DIRECTORY PROPERTY TEST_INCLUDE_FILES\n"
                    f'  "${{CMAKE_BINARY_DIR}}/beta[1]_include.cmake" "{include_expression}")\n'
                    "get_property(root_targets DIRECTORY PROPERTY BUILDSYSTEM_TARGETS)\n"
                    "get_property(ctest_includes DIRECTORY PROPERTY TEST_INCLUDE_FILES)\n"
                    "photospider_collect_registered_gtest_targets(\n"
                    "  registered ctest_includes root_targets \"${CMAKE_BINARY_DIR}\")\n"
                    "if(NOT registered STREQUAL \"alpha;beta\")\n"
                    "  message(FATAL_ERROR \"unexpected registered target inventory: ${registered}\")\n"
                    "endif()\n",
                    encoding="utf-8",
                )
                completed = run_command(
                    "cmake", "-S", source, "-B", build,
                    expect_success=not foreign_include,
                )
                if foreign_include:
                    self.assertIn(
                        "CTest include is outside the root binary directory",
                        completed.stderr,
                    )


class BuildSmokeRoutingContractTest(unittest.TestCase):
    """Validate disjoint producer, control, and dedicated smoke routing."""

    def test_current_main_routing_is_complete_and_role_explicit(self) -> None:
        """Route all four partitions once and reject a missing locked entry."""
        with tempfile.TemporaryDirectory() as temporary_text:
            root = Path(temporary_text)
            matrix = root / "matrix.json"
            consumers = root / "consumers.json"
            dedicated = root / "dedicated.json"
            openexr = root / "openexr.json"
            producers = root / "producers.z"
            entries = [
                {"artifact": "dependency-disabled", "test": "DependencyDisabledInstallSmoke"},
                {
                    "artifact": "openexr-option-off",
                    "test": "OpenExrDeepProviderOptionOffSmoke",
                },
                {"artifact": "public-header", "test": "PublicHeaderSelfContainment"},
                {"artifact": "static-product", "test": "StaticProductConsumerSmoke"},
            ]
            matrix.write_text(
                json.dumps({"include": entries}, sort_keys=True, separators=(",", ":")) + "\n",
                encoding="utf-8",
            )
            run_command(
                "python3", SCRIPTS / "build_smoke_route.py",
                "--matrix", matrix,
                "--lock", REPO_ROOT / "ci/locks/build-smoke-routing.json",
                "--consumer-matrix", consumers,
                "--dedicated-matrix", dedicated,
                "--openexr-matrix", openexr,
                "--producer-names", producers,
            )
            routed = json.loads(consumers.read_text(encoding="utf-8"))
            self.assertEqual(
                routed,
                {
                    "include": [
                        {
                            "artifact": "dependency-disabled",
                            "artifact_role": "ctest-control",
                            "test": "DependencyDisabledInstallSmoke",
                        }
                    ]
                },
            )
            self.assertEqual(
                producers.read_bytes(),
                b"PublicHeaderSelfContainment\0",
            )
            self.assertEqual(
                json.loads(dedicated.read_text(encoding="utf-8")),
                {
                    "include": [
                        {
                            "artifact": "static-product",
                            "artifact_role": "installed-package",
                            "test": "StaticProductConsumerSmoke",
                        }
                    ]
                },
            )
            self.assertEqual(
                json.loads(openexr.read_text(encoding="utf-8")),
                {
                    "include": [
                        {
                            "artifact": "openexr-option-off",
                            "artifact_role": "openexr-metadata",
                            "test": "OpenExrDeepProviderOptionOffSmoke",
                        }
                    ]
                },
            )

            matrix.write_text(
                json.dumps({"include": entries[:-1]}, sort_keys=True, separators=(",", ":")) + "\n",
                encoding="utf-8",
            )
            failed = run_command(
                "python3", SCRIPTS / "build_smoke_route.py",
                "--matrix", matrix,
                "--lock", REPO_ROOT / "ci/locks/build-smoke-routing.json",
                "--consumer-matrix", consumers,
                "--dedicated-matrix", dedicated,
                "--openexr-matrix", openexr,
                "--producer-names", producers,
                expect_success=False,
            )
            self.assertIn("installed-package tests differ", failed.stderr)

    def test_openexr_metadata_runner_replays_the_exact_source_driver_boundary(self) -> None:
        """Pass only cache metadata and the locked option-off identity to Python."""
        with tempfile.TemporaryDirectory() as temporary_text:
            root = Path(temporary_text)
            binary_dir = root / "bin"
            binary_dir.mkdir()
            invocation_log = root / "openexr-invocation.json"
            for executable_name in ("cmake", "ctest", "nm"):
                executable = binary_dir / executable_name
                executable.write_text("#!/bin/sh\nexit 0\n", encoding="utf-8")
                executable.chmod(0o755)
            registered_python = binary_dir / "registered-python"
            registered_python.write_text(
                f"#!{sys.executable}\n"
                "import json\n"
                "import os\n"
                "import sys\n"
                "with open(os.environ['CI_TEST_OPENEXR_LOG'], 'w', encoding='utf-8') as handle:\n"
                "    json.dump(sys.argv[1:], handle, separators=(',', ':'))\n",
                encoding="utf-8",
            )
            registered_python.chmod(0o755)
            path_python = binary_dir / "python3"
            path_python.write_text(
                "#!/bin/sh\n"
                "echo 'PATH python must not execute OpenEXR smoke' >&2\n"
                "exit 97\n",
                encoding="utf-8",
            )
            path_python.chmod(0o755)
            metadata_root = root / "metadata/ci"
            producer = metadata_root / "producer"
            producer.mkdir(parents=True)
            (producer / "CMakeCache.txt").write_text(
                "CMAKE_BUILD_TYPE:STRING=Release\n"
                f"CMAKE_COMMAND:INTERNAL={binary_dir / 'cmake'}\n"
                f"CMAKE_CTEST_COMMAND:INTERNAL={binary_dir / 'ctest'}\n"
                f"CMAKE_NM:FILEPATH={binary_dir / 'nm'}\n"
                f"_Python3_EXECUTABLE:INTERNAL={registered_python}\n",
                encoding="utf-8",
            )
            environment = {
                "CI_ARTIFACT_DIR": str(root / "artifacts"),
                "CI_ARTIFACT_ROLE": "openexr-metadata",
                "CI_OPENEXR_METADATA_ROOT": str(metadata_root),
                "CI_TEST_OPENEXR_LOG": str(invocation_log),
                "CMAKE_BUILD_TYPE": "Debug",
                "PATH": f"{binary_dir}:{os.environ['PATH']}",
                "SMOKE_TEST_NAME": "OpenExrDeepProviderOptionOffSmoke",
            }
            run_command(
                "bash", SCRIPTS / "openexr_smoke_test.sh", environment=environment
            )
            arguments = json.loads(invocation_log.read_text(encoding="utf-8"))
            self.assertEqual(arguments[:2], ["-B", str(REPO_ROOT / "tests/integration/openexr_deep_provider_option_off_smoke.py")])
            expected_pairs = {
                "--repo": str(REPO_ROOT),
                "--producer-build": str(producer),
                "--cmake-executable": str(binary_dir / "cmake"),
                "--ctest-executable": str(binary_dir / "ctest"),
                "--symbol-tool": str(binary_dir / "nm"),
                "--config": "Release",
                "--mode": "off",
            }
            for option, expected in expected_pairs.items():
                index = arguments.index(option)
                self.assertEqual(arguments[index + 1], expected)

            invocation_log.unlink()
            failed = run_command(
                "bash",
                SCRIPTS / "openexr_smoke_test.sh",
                environment={**environment, "SMOKE_TEST_NAME": "ForgedOpenExrSmoke"},
                expect_success=False,
            )
            self.assertIn("Unsupported OpenEXR metadata smoke identity", failed.stderr)
            self.assertFalse(invocation_log.exists())

            valid_cache = (producer / "CMakeCache.txt").read_text(encoding="utf-8")
            invalid_caches = {
                "missing": valid_cache.replace(
                    "CMAKE_BUILD_TYPE:STRING=Release\n", ""
                ),
                "empty": valid_cache.replace(
                    "CMAKE_BUILD_TYPE:STRING=Release",
                    "CMAKE_BUILD_TYPE:STRING=",
                ),
                "control": valid_cache.replace(
                    "CMAKE_BUILD_TYPE:STRING=Release",
                    "CMAKE_BUILD_TYPE:STRING=Release\tDebug",
                ),
                "multi-config": valid_cache
                + "CMAKE_CONFIGURATION_TYPES:STRING=Debug;Release\n",
            }
            expected_diagnostics = {
                "missing": "exactly one CMAKE_BUILD_TYPE",
                "empty": "invalid CMAKE_BUILD_TYPE",
                "control": "invalid CMAKE_BUILD_TYPE",
                "multi-config": "does not accept a multi-config",
            }
            for label, cache_text in invalid_caches.items():
                with self.subTest(cache=label):
                    (producer / "CMakeCache.txt").write_text(
                        cache_text, encoding="utf-8"
                    )
                    invocation_log.unlink(missing_ok=True)
                    rejected = run_command(
                        "bash",
                        SCRIPTS / "openexr_smoke_test.sh",
                        environment=environment,
                        expect_success=False,
                    )
                    self.assertIn(expected_diagnostics[label], rejected.stderr)
                    self.assertFalse(invocation_log.exists())
            (producer / "CMakeCache.txt").write_text(
                valid_cache, encoding="utf-8"
            )


class ReusableBuildContractTest(unittest.TestCase):
    """Exercise deterministic packing, exact identity checks, and safe extraction."""

    @staticmethod
    def _load_reusable_module() -> object:
        """Load the protected producer module for deterministic fixture emission."""
        module_path = SCRIPTS / "reusable_build.py"
        specification = importlib.util.spec_from_file_location("reusable_build_contract", module_path)
        if specification is None or specification.loader is None:
            raise AssertionError("cannot load reusable build module")
        module = importlib.util.module_from_spec(specification)
        specification.loader.exec_module(module)
        return module

    @staticmethod
    def _load_ctest_closure_module() -> object:
        """Load the protected ordinary CTest closure implementation."""
        module_path = SCRIPTS / "ctest_runtime_closure.py"
        specification = importlib.util.spec_from_file_location(
            "ctest_runtime_closure_contract", module_path
        )
        if specification is None or specification.loader is None:
            raise AssertionError("cannot load ordinary CTest closure module")
        module = importlib.util.module_from_spec(specification)
        specification.loader.exec_module(module)
        return module

    @staticmethod
    def _prepare_build(source: Path, *, runtime_alias: bool = False) -> Path:
        """Materialize one complete fresh CMake/package/generated build fixture.

        Args:
            source: Fixture build root.
            runtime_alias: Whether to add a safe versioned DSO alias for the
                targeted-role materialization contract.

        Returns:
            Generated CI inventory directory.
        """
        source.mkdir(parents=True, exist_ok=True)
        executable = source / "bin/tool"
        executable.parent.mkdir(parents=True, exist_ok=True)
        executable.write_text("#!/bin/sh\nexit 0\n", encoding="utf-8")
        executable.chmod(0o755)
        test_executable = source / "tests/test_contract"
        test_executable.parent.mkdir(parents=True, exist_ok=True)
        test_executable.write_text("#!/bin/sh\nexit 0\n", encoding="utf-8")
        test_executable.chmod(0o755)
        command_data = source / "tests/relative-command-data.bin"
        command_data.write_bytes(b"ordinary-relative-command-data")
        required_data = source / "tests/relative-required-data.bin"
        required_data.write_bytes(b"ordinary-relative-required-data")
        environment_data = source / "tests/relative-environment-data.bin"
        environment_data.write_bytes(b"ordinary-relative-environment-data")
        environment_path = source / "tests/relative-environment-path"
        environment_path.mkdir(exist_ok=True)
        (environment_path / "path-entry.bin").write_bytes(
            b"ordinary-relative-environment-path"
        )
        (source / "tests/unreferenced-working-directory-data.bin").write_bytes(
            b"must-not-be-selected-by-working-directory"
        )
        plugin = source / "plugins/runtime_contract_plugin.so"
        plugin.parent.mkdir(parents=True, exist_ok=True)
        plugin.write_bytes(b"runtime-plugin")
        trust_manifest = source / "generated/plugin_trust/manifest.txt"
        trust_manifest.parent.mkdir(parents=True, exist_ok=True)
        trust_manifest.write_text("runtime trust manifest\n", encoding="utf-8")
        trust_signature = source / "generated/plugin_trust/signature.hex"
        trust_signature.write_text("00\n", encoding="utf-8")
        nonstandard_include = source / "contract-discovery.cmake"
        nonstandard_include.write_text(
            'add_test(test_contract "./test_contract" '
            '"relative-command-data.bin")\n'
            "set_tests_properties(test_contract PROPERTIES\n"
            '  REQUIRED_FILES "relative-required-data.bin"\n'
            '  ENVIRONMENT "PHOTOSPIDER_RELATIVE_ENV_FILE='
            'relative-environment-data.bin;'
            f'PHOTOSPIDER_PLUGIN_TRUST_MANIFEST={trust_manifest};'
            f'PHOTOSPIDER_PLUGIN_TRUST_SIGNATURE={trust_signature};'
            "PHOTOSPIDER_PLUGIN_TRUST_PUBLIC_KEY="
            f'{REPO_ROOT / "tests/fixtures/trust/test_ed25519_public_key.pem"}"\n'
            '  ENVIRONMENT_MODIFICATION '
            '"PHOTOSPIDER_RELATIVE_PATH=path_list_prepend:'
            'relative-environment-path"\n'
            '  WORKING_DIRECTORY "tests")\n',
            encoding="utf-8",
        )
        (source / "CTestTestfile.cmake").write_text(
            f'include("{nonstandard_include}")\n', encoding="utf-8"
        )
        runtime_library = source / "lib/libphotospider_runtime.so"
        runtime_library.parent.mkdir(parents=True, exist_ok=True)
        runtime_library.write_bytes(b"runtime-shared-object")
        runtime_alias_path = source / "lib/libphotospider_runtime.so.0"
        if runtime_alias_path.exists() or runtime_alias_path.is_symlink():
            runtime_alias_path.unlink()
        if runtime_alias:
            runtime_alias_path.symlink_to(runtime_library.name)
        object_file = source / "CMakeFiles/photospider.dir/product.cpp.o"
        object_file.parent.mkdir(parents=True, exist_ok=True)
        object_file.write_bytes(b"producer-object")
        (source / "lib/libphotospider.a").write_bytes(b"producer-static-library")
        dependency_file = source / "CMakeFiles/photospider.dir/product.cpp.o.d"
        dependency_file.write_text("product.cpp.o: product.cpp\n", encoding="utf-8")
        (source / "CMakeCache.txt").write_text(
            "BUILD_TESTING:BOOL=ON\n"
            "CMAKE_BUILD_TYPE:STRING=RelWithDebInfo\n"
            "CMAKE_CXX_COMPILER:FILEPATH=/usr/bin/clang++\n"
            "CMAKE_GENERATOR:INTERNAL=Ninja\n"
            "CMAKE_PROJECT_NAME:STATIC=photospider\n"
            "PHOTOSPIDER_BUILD_IPC:BOOL=ON\n"
            "USE_ASAN:BOOL=OFF\n"
            "USE_TSAN:BOOL=OFF\n",
            encoding="utf-8",
        )
        module_dir = source / "CMakeFiles/3.31.0"
        module_dir.mkdir(parents=True, exist_ok=True)
        (module_dir / "CMakeCXXCompiler.cmake").write_text(
            'set(CMAKE_CXX_COMPILER "/usr/bin/clang++")\n'
            'set(CMAKE_CXX_COMPILER_ID "Clang")\n'
            'set(CMAKE_CXX_COMPILER_VERSION "18.1.8")\n',
            encoding="utf-8",
        )
        (module_dir / "CMakeSystem.cmake").write_text(
            'set(CMAKE_SYSTEM_NAME "Linux")\n'
            'set(CMAKE_SYSTEM_PROCESSOR "x86_64")\n',
            encoding="utf-8",
        )
        (source / "PhotospiderConfig.cmake").write_text(
            "set(Photospider_embedded_FOUND TRUE)\n"
            "set(Photospider_policy_sdk_FOUND ON)\n",
            encoding="utf-8",
        )
        export = source / "CMakeFiles/Export/fixture/PhotospiderTargets.cmake"
        export.parent.mkdir(parents=True, exist_ok=True)
        export.write_text(
            "add_library(Photospider::photospider_kernel STATIC IMPORTED)\n"
            "add_library(Photospider::policy_sdk INTERFACE IMPORTED)\n",
            encoding="utf-8",
        )
        inventory = source / "generated/ci_inventory"
        inventory.mkdir(parents=True, exist_ok=True)
        (inventory / "installable_public_headers.txt").write_text(
            "include/photospider/core/graph.hpp\n", encoding="utf-8"
        )
        (inventory / "registered_gtest_targets-RelWithDebInfo.tsv").write_text(
            "# target\tconfigured executable\n"
            "test_contract\t/tmp/test_contract\n",
            encoding="utf-8",
        )
        (source / ".photospider-ci-build-complete").write_text(
            f"build_dir={source.resolve()}\n"
            f"source_dir={REPO_ROOT.resolve()}\n"
            "profile=default\n"
            "build_testing=ON\n"
            "photospider_build_ipc=ON\n"
            f"candidate_commit={COMMIT_A}\n"
            "created_at=2026-08-25T12:00:00Z\n",
            encoding="utf-8",
        )
        inventory_path = source.parent / "post-build-ctest-inventory.json"
        discovered = subprocess.run(
            [
                "ctest",
                "--test-dir",
                str(source),
                "--show-only=json-v1",
                "-C",
                "RelWithDebInfo",
            ],
            check=True,
            capture_output=True,
        )
        inventory_path.write_bytes(discovered.stdout)
        closure = ReusableBuildContractTest._load_ctest_closure_module()
        closure.write_closure(
            REPO_ROOT,
            source,
            inventory_path,
            inventory / "ordinary_ctest_closure_v1.json",
            "RelWithDebInfo",
        )
        return inventory

    def _create(self, root: Path, suffix: str) -> tuple[Path, Path, dict[str, object]]:
        """Create one test archive and return its paths and parsed manifest."""
        source = root / "source"
        inventory = self._prepare_build(source)
        archive = root / f"ci-build-{suffix}.tar.gz"
        manifest = root / f"ci-build-{suffix}.manifest.json"
        run_command(
            "python3", SCRIPTS / "reusable_build.py",
            "--inventory-dir", inventory,
            "create",
            "--source", source,
            "--archive", archive,
            "--manifest", manifest,
            "--candidate-commit", COMMIT_A,
            "--profile", "default",
            "--image-digest", IMAGE_DIGEST,
            "--workflow-commit", COMMIT_B,
        )
        return archive, manifest, json.loads(manifest.read_text(encoding="utf-8"))

    def _create_targeted(
        self, root: Path, role: str, payload: Path | None = None
    ) -> tuple[Path, Path, dict[str, object]]:
        """Create one role-minimal artifact from the complete fixture build."""
        source = root / "targeted-root/ci"
        inventory = self._prepare_build(source, runtime_alias=True)
        archive = root / f"{role}.tar.gz"
        manifest = root / f"{role}.manifest.json"
        arguments: list[object] = [
            "python3", SCRIPTS / "reusable_build.py",
            "--inventory-dir", inventory,
            "create-targeted",
            "--source", source,
            "--role", role,
            "--archive", archive,
            "--manifest", manifest,
            "--candidate-commit", COMMIT_A,
            "--profile", "default",
            "--image-digest", IMAGE_DIGEST,
            "--workflow-commit", COMMIT_B,
        ]
        if payload is not None:
            arguments.extend(("--payload-source", payload))
        run_command(*arguments)
        return archive, manifest, json.loads(manifest.read_text(encoding="utf-8"))

    @staticmethod
    def _prepare_installed_prefix(root: Path) -> Path:
        """Create one minimal installed-package role fixture.

        Args:
            root: Existing test-owned root.

        Returns:
            Fresh installed prefix containing a public header, package config,
            static product library, and one versioned runtime DSO alias.
        """
        installed = root / "installed-prefix"
        (installed / "include/photospider").mkdir(parents=True)
        (installed / "include/photospider/product.hpp").write_text(
            "#pragma once\n", encoding="utf-8"
        )
        (installed / "lib/cmake/Photospider").mkdir(parents=True)
        (installed / "lib/cmake/Photospider/PhotospiderConfig.cmake").write_text(
            "set(Photospider_FOUND TRUE)\n", encoding="utf-8"
        )
        (installed / "lib/libphotospider.a").write_bytes(b"installed-product")
        installed_runtime = installed / "lib/libphotospider_runtime.so.1.0"
        installed_runtime.write_bytes(b"installed-runtime")
        (installed / "lib/libphotospider_runtime.so.1").symlink_to(
            installed_runtime.name
        )
        return installed

    def test_deterministic_archive_and_exact_safe_extraction(self) -> None:
        """Produce identical archives and reject a wrong expected candidate."""
        with tempfile.TemporaryDirectory() as temporary_text:
            root = Path(temporary_text)
            archive_a, manifest_a, identity = self._create(root, "a")
            archive_b, _, _ = self._create(root, "b")
            self.assertEqual(hashlib.sha256(archive_a.read_bytes()).digest(), hashlib.sha256(archive_b.read_bytes()).digest())
            destination = root / "restored"
            common = (
                "--archive", archive_a,
                "--manifest", manifest_a,
                "--profile", "default",
                "--matrix-sha256", identity["matrix_sha256"],
                "--image-digest", IMAGE_DIGEST,
                "--workflow-commit", COMMIT_B,
            )
            run_command(
                "python3", SCRIPTS / "reusable_build.py", "verify-extract",
                *common,
                "--candidate-commit", COMMIT_A,
                "--destination", destination,
            )
            self.assertEqual((destination / "ci/bin/tool").read_text(encoding="utf-8"), "#!/bin/sh\nexit 0\n")
            failed = run_command(
                "python3", SCRIPTS / "reusable_build.py", "verify-only",
                *common,
                "--candidate-commit", "4" * 40,
                expect_success=False,
            )
            self.assertIn("candidate_commit mismatch", failed.stderr)

    def test_targeted_roles_bind_content_size_and_forbid_build_residue(self) -> None:
        """Keep role members exact and reject object/dependency fan-out drift."""
        with tempfile.TemporaryDirectory() as temporary_text:
            root = Path(temporary_text)
            installed = root / "installed-prefix"
            (installed / "include/photospider").mkdir(parents=True)
            (installed / "include/photospider/product.hpp").write_text(
                "#pragma once\n", encoding="utf-8"
            )
            (installed / "lib/cmake/Photospider").mkdir(parents=True)
            (installed / "lib/cmake/Photospider/PhotospiderConfig.cmake").write_text(
                "set(Photospider_FOUND TRUE)\n", encoding="utf-8"
            )
            (installed / "lib/libphotospider.a").write_bytes(b"installed-product")
            installed_runtime = installed / "lib/libphotospider_runtime.so.1.0"
            installed_runtime.write_bytes(b"installed-runtime")
            (installed / "lib/libphotospider_runtime.so.1").symlink_to(
                installed_runtime.name
            )

            role_values: dict[str, tuple[Path, Path, dict[str, object]]] = {}
            for role in (
                "ctest-control",
                "ctest-runtime",
                "installed-package",
                "openexr-metadata",
            ):
                role_values[role] = self._create_targeted(
                    root, role, installed if role == "installed-package" else None
                )

            control = role_values["ctest-control"][2]
            runtime = role_values["ctest-runtime"][2]
            package = role_values["installed-package"][2]
            openexr = role_values["openexr-metadata"][2]
            control_paths = [member["path"] for member in control["content"]["members"]]
            runtime_paths = [member["path"] for member in runtime["content"]["members"]]
            package_paths = [member["path"] for member in package["content"]["members"]]
            openexr_paths = [
                member["path"] for member in openexr["content"]["members"]
            ]
            self.assertIn("CMakeCache.txt", control_paths)
            self.assertIn("CTestTestfile.cmake", control_paths)
            self.assertNotIn("bin/tool", control_paths)
            self.assertNotIn("bin/tool", runtime_paths)
            self.assertIn("tests/test_contract", runtime_paths)
            self.assertIn("tests/relative-command-data.bin", runtime_paths)
            self.assertIn("tests/relative-required-data.bin", runtime_paths)
            self.assertIn("tests/relative-environment-data.bin", runtime_paths)
            self.assertIn(
                "tests/relative-environment-path/path-entry.bin", runtime_paths
            )
            self.assertNotIn(
                "tests/unreferenced-working-directory-data.bin", runtime_paths
            )
            self.assertIn("lib/libphotospider_runtime.so", runtime_paths)
            self.assertIn("lib/libphotospider_runtime.so.0", runtime_paths)
            self.assertIn("plugins/runtime_contract_plugin.so", runtime_paths)
            self.assertIn("generated/plugin_trust/manifest.txt", runtime_paths)
            self.assertFalse(any(path.endswith((".o", ".o.d", ".a")) for path in runtime_paths))
            self.assertTrue(
                all(
                    path.startswith("installed/")
                    or path
                    in {
                        "producer/CMakeCache.txt",
                        "producer/generated/ci_inventory/installable_public_headers.txt",
                    }
                    for path in package_paths
                )
            )
            self.assertIn("installed/lib/libphotospider.a", package_paths)
            self.assertIn("installed/lib/libphotospider_runtime.so.1", package_paths)
            self.assertIn("producer/CMakeCache.txt", package_paths)
            self.assertIn(
                "producer/generated/ci_inventory/installable_public_headers.txt",
                package_paths,
            )
            self.assertEqual(openexr_paths, ["producer/CMakeCache.txt"])
            for identity in (control, runtime, package, openexr):
                members = identity["content"]["members"]
                self.assertEqual(
                    identity["content"]["uncompressed_size"],
                    sum(member["size"] for member in members),
                )

            archive, manifest, identity = role_values["ctest-runtime"]
            destination = root / "targeted-root"
            shutil.rmtree(destination / "ci")
            run_command(
                "python3", SCRIPTS / "reusable_build.py", "verify-targeted-extract",
                "--archive", archive,
                "--manifest", manifest,
                "--role", "ctest-runtime",
                "--candidate-commit", COMMIT_A,
                "--profile", "default",
                "--matrix-sha256", identity["matrix_sha256"],
                "--image-digest", IMAGE_DIGEST,
                "--workflow-commit", COMMIT_B,
                "--destination", destination,
            )
            self.assertTrue((destination / "ci/tests/test_contract").is_file())
            self.assertTrue(
                (destination / "ci/lib/libphotospider_runtime.so.0").is_file()
            )
            self.assertFalse(
                (destination / "ci/lib/libphotospider_runtime.so.0").is_symlink()
            )

            identity["content"]["members"][0]["size"] += 1
            identity["content"]["uncompressed_size"] += 1
            manifest.write_text(
                json.dumps(identity, sort_keys=True, separators=(",", ":")) + "\n",
                encoding="utf-8",
            )
            failed = run_command(
                "python3", SCRIPTS / "reusable_build.py", "verify-targeted-only",
                "--archive", archive,
                "--manifest", manifest,
                "--role", "ctest-runtime",
                "--candidate-commit", COMMIT_A,
                "--profile", "default",
                "--matrix-sha256", identity["matrix_sha256"],
                "--image-digest", IMAGE_DIGEST,
                "--workflow-commit", COMMIT_B,
                expect_success=False,
            )
            self.assertIn("archived content differs", failed.stderr)

    def test_targeted_content_sizes_reject_bool_string_negative_and_total_drift(self) -> None:
        """Reject non-integer and inconsistent member or aggregate byte counts."""
        with tempfile.TemporaryDirectory() as temporary_text:
            root = Path(temporary_text)
            installed = self._prepare_installed_prefix(root)
            archive, manifest, identity = self._create_targeted(
                root, "installed-package", installed
            )
            cases = (
                ("member-bool", ("member", True), "member size is invalid"),
                ("member-string", ("member", "1"), "member size is invalid"),
                ("member-negative", ("member", -1), "member size is invalid"),
                (
                    "aggregate-bool",
                    ("aggregate", True),
                    "uncompressed size is invalid",
                ),
                (
                    "aggregate-string",
                    ("aggregate", "1"),
                    "uncompressed size is invalid",
                ),
                (
                    "aggregate-negative",
                    ("aggregate", -1),
                    "uncompressed size is invalid",
                ),
                (
                    "aggregate-total-drift",
                    (
                        "aggregate",
                        identity["content"]["uncompressed_size"] + 1,
                    ),
                    "uncompressed size mismatch",
                ),
            )
            for label, (field, replacement), diagnostic in cases:
                with self.subTest(case=label):
                    mutated = json.loads(json.dumps(identity))
                    if field == "member":
                        mutated["content"]["members"][0]["size"] = replacement
                    else:
                        mutated["content"]["uncompressed_size"] = replacement
                    manifest.write_text(
                        json.dumps(
                            mutated,
                            sort_keys=True,
                            separators=(",", ":"),
                            ensure_ascii=True,
                        )
                        + "\n",
                        encoding="utf-8",
                    )
                    failed = run_command(
                        "python3",
                        SCRIPTS / "reusable_build.py",
                        "verify-targeted-only",
                        "--archive",
                        archive,
                        "--manifest",
                        manifest,
                        "--role",
                        "installed-package",
                        "--candidate-commit",
                        COMMIT_A,
                        "--profile",
                        "default",
                        "--matrix-sha256",
                        identity["matrix_sha256"],
                        "--image-digest",
                        IMAGE_DIGEST,
                        "--workflow-commit",
                        COMMIT_B,
                        expect_success=False,
                    )
                    self.assertIn(diagnostic, failed.stderr)

    def test_restored_package_tree_rejects_add_delete_rewrite_and_mode_drift(self) -> None:
        """Remeasure every package member and executable bit before and after use."""
        with tempfile.TemporaryDirectory() as temporary_text:
            root = Path(temporary_text)
            installed = self._prepare_installed_prefix(root)
            archive, manifest, identity = self._create_targeted(
                root, "installed-package", installed
            )
            destination = root / "restored-package"
            extract_arguments: tuple[object, ...] = (
                "python3",
                SCRIPTS / "reusable_build.py",
                "verify-targeted-extract",
                "--archive",
                archive,
                "--manifest",
                manifest,
                "--role",
                "installed-package",
                "--candidate-commit",
                COMMIT_A,
                "--profile",
                "default",
                "--matrix-sha256",
                identity["matrix_sha256"],
                "--image-digest",
                IMAGE_DIGEST,
                "--workflow-commit",
                COMMIT_B,
                "--destination",
                destination,
            )
            verify_arguments: tuple[object, ...] = (
                "python3",
                SCRIPTS / "reusable_build.py",
                "verify-targeted-tree",
                "--manifest",
                manifest,
                "--role",
                "installed-package",
                "--content-root",
                destination / "ci",
            )
            run_command(*extract_arguments)
            evidence = root / "tree-verification.json"
            run_command(*verify_arguments, "--evidence-output", evidence)
            self.assertTrue(evidence.is_file())
            verified = json.loads(evidence.read_text(encoding="utf-8"))
            retained_arguments: tuple[object, ...] = (
                "--expected-content-sha256",
                verified["content_sha256"],
                "--expected-manifest-sha256",
                verified["manifest_sha256"],
            )

            original_manifest = manifest.read_bytes()
            forged_manifest = json.loads(original_manifest)
            forged_manifest["producer_workflow_commit"] = COMMIT_A
            manifest.write_text(
                json.dumps(
                    forged_manifest,
                    sort_keys=True,
                    separators=(",", ":"),
                    ensure_ascii=True,
                )
                + "\n",
                encoding="utf-8",
            )
            forged = run_command(
                *verify_arguments,
                *retained_arguments,
                expect_success=False,
            )
            self.assertIn("retained manifest identity", forged.stderr)
            manifest.write_bytes(original_manifest)

            mutations = ("addition", "deletion", "rewrite", "executable-bit")
            for mutation in mutations:
                with self.subTest(mutation=mutation):
                    run_command(*extract_arguments)
                    content_root = destination / "ci"
                    header = content_root / "installed/include/photospider/product.hpp"
                    if mutation == "addition":
                        (content_root / "installed/unexpected-member.txt").write_text(
                            "unexpected\n", encoding="utf-8"
                        )
                    elif mutation == "deletion":
                        header.unlink()
                    elif mutation == "rewrite":
                        original_size = header.stat().st_size
                        header.write_bytes(b"x" * original_size)
                    else:
                        header.chmod(header.stat().st_mode | 0o111)
                    failed = run_command(
                        *verify_arguments,
                        *retained_arguments,
                        expect_success=False,
                    )
                    self.assertIn(
                        "content tree differs from its verified manifest",
                        failed.stderr,
                    )

    def test_atomic_manifest_replacement_fails_one_retained_tree_snapshot(self) -> None:
        """Reject pathname replacement after hashing retained manifest bytes."""
        module = self._load_reusable_module()
        with tempfile.TemporaryDirectory() as temporary_text:
            root = Path(temporary_text)
            installed = self._prepare_installed_prefix(root)
            archive, manifest, identity = self._create_targeted(
                root, "installed-package", installed
            )
            destination = root / "restored-package"
            run_command(
                "python3",
                SCRIPTS / "reusable_build.py",
                "verify-targeted-extract",
                "--archive",
                archive,
                "--manifest",
                manifest,
                "--role",
                "installed-package",
                "--candidate-commit",
                COMMIT_A,
                "--profile",
                "default",
                "--matrix-sha256",
                identity["matrix_sha256"],
                "--image-digest",
                IMAGE_DIGEST,
                "--workflow-commit",
                COMMIT_B,
                "--destination",
                destination,
            )
            forged_identity = json.loads(json.dumps(identity))
            forged_identity["producer_workflow_commit"] = COMMIT_A
            self.assertEqual(forged_identity["content"], identity["content"])
            replacement = root / "replacement.manifest.json"
            replacement.write_text(
                json.dumps(
                    forged_identity,
                    sort_keys=True,
                    separators=(",", ":"),
                    ensure_ascii=True,
                )
                + "\n",
                encoding="utf-8",
            )
            arguments = SimpleNamespace(
                content_root=destination / "ci",
                evidence_output=root / "must-not-exist.json",
                expected_content_sha256=None,
                expected_manifest_sha256=None,
                manifest=manifest,
                role="installed-package",
            )
            original_sha256 = module._ManifestSnapshot.sha256
            replaced = False

            def replace_after_digest(snapshot: object) -> str:
                """Replace only the pathname after hashing the retained bytes."""
                nonlocal replaced
                digest = original_sha256(snapshot)
                replacement.replace(manifest)
                replaced = True
                return digest

            with mock.patch.object(
                module._ManifestSnapshot, "sha256", replace_after_digest
            ):
                with self.assertRaisesRegex(
                    module.ReusableBuildError,
                    "targeted manifest pathname changed during verification",
                ):
                    module._verify_targeted_tree(arguments)
            self.assertTrue(replaced)
            self.assertFalse(arguments.evidence_output.exists())

    def test_atomic_manifest_replacement_blocks_targeted_archive_verification(self) -> None:
        """Reject a replaced manifest pathname in verify-only/extract shared code."""
        module = self._load_reusable_module()
        with tempfile.TemporaryDirectory() as temporary_text:
            root = Path(temporary_text)
            installed = self._prepare_installed_prefix(root)
            archive, manifest, identity = self._create_targeted(
                root, "installed-package", installed
            )
            forged_identity = json.loads(json.dumps(identity))
            forged_identity["profile"] = "forged-profile"
            self.assertEqual(forged_identity["content"], identity["content"])
            replacement = root / "replacement.manifest.json"
            replacement.write_text(
                json.dumps(
                    forged_identity,
                    sort_keys=True,
                    separators=(",", ":"),
                    ensure_ascii=True,
                )
                + "\n",
                encoding="utf-8",
            )
            arguments = SimpleNamespace(
                archive=archive,
                candidate_commit=COMMIT_A,
                expected_manifest_sha256=None,
                image_digest=IMAGE_DIGEST,
                manifest=manifest,
                matrix_sha256=identity["matrix_sha256"],
                profile="default",
                repo_root=REPO_ROOT,
                role="installed-package",
                workflow_commit=COMMIT_B,
            )
            original_sha256 = module._ManifestSnapshot.sha256

            def replace_after_digest(snapshot: object) -> str:
                """Replace the pathname after the retained object is hashed."""
                digest = original_sha256(snapshot)
                replacement.replace(manifest)
                return digest

            with mock.patch.object(
                module._ManifestSnapshot, "sha256", replace_after_digest
            ):
                with self.assertRaisesRegex(
                    module.ReusableBuildError,
                    "targeted manifest pathname changed during verification",
                ):
                    module._verify_targeted_only(arguments)

    def test_runtime_closure_rejects_missing_relative_inputs_library_and_trust(self) -> None:
        """Reject each relative CTest input plus absolute runtime/trust classes."""
        closure = self._load_ctest_closure_module()
        removals = {
            "nonstandard include": "contract-discovery.cmake",
            "relative executable": "tests/test_contract",
            "relative command data": "tests/relative-command-data.bin",
            "relative required file": "tests/relative-required-data.bin",
            "relative environment file": "tests/relative-environment-data.bin",
            "relative environment path file": (
                "tests/relative-environment-path/path-entry.bin"
            ),
            "dynamic library": "lib/libphotospider_runtime.so",
            "trust material": "generated/plugin_trust/manifest.txt",
        }
        with tempfile.TemporaryDirectory() as temporary_text:
            root = Path(temporary_text)
            for label, relative in removals.items():
                with self.subTest(dependency=label):
                    build = root / re.sub(r"[^a-z]+", "-", label) / "ci"
                    self._prepare_build(build)
                    (build / relative).unlink()
                    with self.assertRaises(closure.CTestClosureError):
                        closure.verify_restored_runtime(REPO_ROOT, build, "ctest")

    def test_runtime_closure_accepts_real_root_locator_command_shapes(self) -> None:
        """Ignore exact source/build roots emitted by real CTest JSON discovery."""
        closure = self._load_ctest_closure_module()
        with tempfile.TemporaryDirectory() as temporary_text:
            root = Path(temporary_text)
            source = root / "source"
            build = root / "build"
            source.mkdir()
            driver = source / "driver.py"
            driver.write_text("raise SystemExit(0)\n", encoding="utf-8")
            (source / "unreferenced-source.txt").write_text(
                "must not enter the closure\n", encoding="utf-8"
            )
            python = Path(sys.executable).as_posix()
            (source / "CMakeLists.txt").write_text(
                "cmake_minimum_required(VERSION 3.16)\n"
                "project(ctest_root_locator_contract NONE)\n"
                "enable_testing()\n"
                "add_test(NAME InstallConsumerArchitecturePropagationSafety\n"
                f'  COMMAND "{python}" -B "${{CMAKE_SOURCE_DIR}}/driver.py"\n'
                "    --build-dir \"${CMAKE_BINARY_DIR}\"\n"
                "    --cmake-executable \"${CMAKE_COMMAND}\"\n"
                "    --ctest-executable \"${CMAKE_CTEST_COMMAND}\"\n"
                "    --config Release\n"
                f'    --python-executable "{python}")\n'
                "add_test(NAME GraphCliOptionBadAlloc\n"
                f'  COMMAND "{python}" "${{CMAKE_SOURCE_DIR}}/driver.py"\n'
                "    --repo \"${CMAKE_SOURCE_DIR}\"\n"
                "    --binary \"${CMAKE_BINARY_DIR}/bin/graph_cli\"\n"
                "    --out \"${CMAKE_BINARY_DIR}/tests/graph_cli_option_bad_alloc\")\n",
                encoding="utf-8",
            )
            run_command("cmake", "-S", source, "-B", build)
            executable = build / "bin/graph_cli"
            executable.parent.mkdir(parents=True, exist_ok=True)
            executable.write_text("#!/bin/sh\nexit 0\n", encoding="utf-8")
            executable.chmod(0o755)
            (build / "objects").mkdir()
            (build / "objects/unrelated.o").write_bytes(b"producer object")
            (build / "unreferenced-root.txt").write_text(
                "must not enter the closure\n", encoding="utf-8"
            )
            inventory = root / "ctest-inventory.json"
            discovered = run_command(
                "ctest",
                "--test-dir",
                build,
                "--show-only=json-v1",
                "-C",
                "Release",
            )
            inventory.write_text(discovered.stdout, encoding="utf-8")
            payload = json.loads(discovered.stdout)
            commands = {test["name"]: test["command"] for test in payload["tests"]}
            graph_command = commands["GraphCliOptionBadAlloc"]
            install_command = commands["InstallConsumerArchitecturePropagationSafety"]
            self.assertEqual(
                Path(graph_command[graph_command.index("--repo") + 1]).resolve(),
                source.resolve(),
            )
            self.assertEqual(
                Path(
                    install_command[install_command.index("--build-dir") + 1]
                ).resolve(),
                build.resolve(),
            )

            value = closure.create_closure(
                source, build, inventory, "Release", require_runtime=True
            )
            self.assertIn("bin/graph_cli", value["runtime_paths"])
            self.assertNotIn("objects/unrelated.o", value["runtime_paths"])
            self.assertNotIn("unreferenced-root.txt", value["runtime_paths"])
            source_paths = [item["path"] for item in value["source_inputs"]]
            self.assertEqual(source_paths, ["driver.py"])
            self.assertNotIn("unreferenced-source.txt", source_paths)

    def test_ctest_control_graph_rejects_a_two_file_cycle(self) -> None:
        """Reject a generated CTest include back edge with a live DFS stack."""
        closure = self._load_ctest_closure_module()
        with tempfile.TemporaryDirectory() as temporary_text:
            build = Path(temporary_text)
            child = build / "child/CTestTestfile.cmake"
            child.parent.mkdir()
            (build / "CTestTestfile.cmake").write_text(
                'include("child/CTestTestfile.cmake")\n', encoding="utf-8"
            )
            child.write_text(
                'include("../CTestTestfile.cmake")\n', encoding="utf-8"
            )
            with self.assertRaisesRegex(
                closure.CTestClosureError, "generated CTest include cycle"
            ):
                closure._control_paths(build)

    def test_runtime_closure_rejects_an_escaping_library_alias(self) -> None:
        """Materialize only in-root DSO aliases and reject an escaping target."""
        closure = self._load_ctest_closure_module()
        with tempfile.TemporaryDirectory() as temporary_text:
            root = Path(temporary_text)
            build = root / "ci"
            self._prepare_build(build, runtime_alias=True)
            alias = build / "lib/libphotospider_runtime.so.0"
            alias.unlink()
            outside = root / "outside-runtime.so.0"
            outside.write_bytes(b"outside")
            alias.symlink_to(outside)
            with self.assertRaises(closure.CTestClosureError):
                closure.verify_restored_runtime(REPO_ROOT, build, "ctest")

    def test_installed_package_rejects_a_nonlibrary_link(self) -> None:
        """Materialize only DSO aliases and reject linked package headers."""
        with tempfile.TemporaryDirectory() as temporary_text:
            root = Path(temporary_text)
            build = root / "targeted-root/ci"
            inventory = self._prepare_build(build, runtime_alias=True)
            installed = root / "installed-prefix"
            header_root = installed / "include/photospider"
            header_root.mkdir(parents=True)
            real_header = header_root / "product.hpp"
            real_header.write_text("#pragma once\n", encoding="utf-8")
            (header_root / "alias.hpp").symlink_to(real_header.name)
            (installed / "lib/cmake/Photospider").mkdir(parents=True)
            (installed / "lib/cmake/Photospider/PhotospiderConfig.cmake").write_text(
                "set(Photospider_FOUND TRUE)\n", encoding="utf-8"
            )
            (installed / "lib/libphotospider.a").write_bytes(b"product")
            failed = run_command(
                "python3",
                SCRIPTS / "reusable_build.py",
                "--inventory-dir",
                inventory,
                "create-targeted",
                "--source",
                build,
                "--payload-source",
                installed,
                "--role",
                "installed-package",
                "--archive",
                root / "installed-package.tar.gz",
                "--manifest",
                root / "installed-package.manifest.json",
                "--candidate-commit",
                COMMIT_A,
                "--profile",
                "default",
                "--image-digest",
                IMAGE_DIGEST,
                "--workflow-commit",
                COMMIT_B,
                expect_success=False,
            )
            self.assertIn("non-library link", failed.stderr)

    def test_traversal_member_is_rejected_after_digest_match(self) -> None:
        """Reject a traversal entry even when a caller recomputes the archive hash."""
        with tempfile.TemporaryDirectory() as temporary_text:
            root = Path(temporary_text)
            archive, manifest, identity = self._create(root, "unsafe")
            with archive.open("wb") as raw:
                with gzip.GzipFile(filename="", mode="wb", fileobj=raw, mtime=0) as compressed:
                    with tarfile.open(fileobj=compressed, mode="w") as tar:
                        information = tarfile.TarInfo("ci/../escape")
                        information.size = 1
                        tar.addfile(information, io.BytesIO(b"x"))
            identity["artifact"]["sha256"] = hashlib.sha256(archive.read_bytes()).hexdigest()
            identity["artifact"]["size"] = archive.stat().st_size
            manifest.write_text(
                json.dumps(identity, sort_keys=True, separators=(",", ":"), ensure_ascii=True) + "\n",
                encoding="utf-8",
            )
            failed = run_command(
                "python3", SCRIPTS / "reusable_build.py", "verify-only",
                "--archive", archive,
                "--manifest", manifest,
                "--candidate-commit", COMMIT_A,
                "--profile", "default",
                "--matrix-sha256", identity["matrix_sha256"],
                "--image-digest", IMAGE_DIGEST,
                "--workflow-commit", COMMIT_B,
                expect_success=False,
            )
            self.assertIn("unsafe archive member path", failed.stderr)

    def test_atomic_archive_replacement_cannot_change_verified_extraction(self) -> None:
        """Keep measurement and extraction on the object opened before replacement."""
        with tempfile.TemporaryDirectory() as temporary_text:
            root = Path(temporary_text)
            archive, manifest, identity = self._create(root, "accepted")
            (root / "source/replacement-marker.txt").write_text(
                "replacement archive\n", encoding="utf-8"
            )
            replacement, _, _ = self._create(root, "replacement")
            destination = root / "restored"
            arguments = SimpleNamespace(
                archive=archive,
                candidate_commit=COMMIT_A,
                destination=destination,
                image_digest=IMAGE_DIGEST,
                manifest=manifest,
                matrix_sha256=identity["matrix_sha256"],
                profile="default",
                repo_root=REPO_ROOT,
                workflow_commit=COMMIT_B,
            )
            module = self._load_reusable_module()
            original_sha256 = module._ArchiveSnapshot.sha256
            replaced = False

            def replace_after_digest(snapshot: object) -> str:
                """Atomically replace the pathname after hashing the open object."""
                nonlocal replaced
                digest = original_sha256(snapshot)
                replacement.replace(archive)
                replaced = True
                return digest

            with mock.patch.object(
                module._ArchiveSnapshot, "sha256", replace_after_digest
            ):
                module._verify_extract(arguments)

            self.assertTrue(replaced)
            self.assertFalse((destination / "ci/replacement-marker.txt").exists())
            with tarfile.open(archive, mode="r:gz") as replacement_archive:
                self.assertIn(
                    "ci/replacement-marker.txt",
                    [member.name for member in replacement_archive.getmembers()],
                )

    def test_archive_snapshot_rejects_links_and_special_files(self) -> None:
        """Reject alias and FIFO archive objects without blocking on the FIFO."""
        module = self._load_reusable_module()
        with tempfile.TemporaryDirectory() as temporary_text:
            root = Path(temporary_text)
            regular = root / "regular.tar.gz"
            regular.write_bytes(b"not needed for open-boundary validation")
            link = root / "archive-link.tar.gz"
            link.symlink_to(regular)
            with self.assertRaises((OSError, module.ReusableBuildError)):
                module._ArchiveSnapshot(link)

            fifo = root / "archive-fifo.tar.gz"
            os.mkfifo(fifo)
            with self.assertRaises(module.ReusableBuildError):
                module._ArchiveSnapshot(fifo)

    def test_manifest_snapshot_rejects_links_and_special_files(self) -> None:
        """Reject final-component aliases and FIFOs before manifest decoding."""
        module = self._load_reusable_module()
        with tempfile.TemporaryDirectory() as temporary_text:
            root = Path(temporary_text)
            regular = root / "regular.manifest.json"
            regular.write_text("{}\n", encoding="utf-8")
            link = root / "manifest-link.json"
            link.symlink_to(regular)
            with self.assertRaises((OSError, module.ReusableBuildError)):
                module._ManifestSnapshot(link)

            fifo = root / "manifest-fifo.json"
            os.mkfifo(fifo)
            with self.assertRaises(module.ReusableBuildError):
                module._ManifestSnapshot(fifo)

    def test_consumer_verifies_two_attestations_before_extraction(self) -> None:
        """Require archive and manifest attestation calls on the generic consumer."""
        with tempfile.TemporaryDirectory() as temporary_text:
            root = Path(temporary_text)
            source = root / "source"
            self._prepare_build(source)
            (source / "result.txt").write_text("verified\n", encoding="utf-8")
            reusable = root / "reusable"
            reusable.mkdir()
            archive = reusable / "ci-build.tar.gz"
            manifest = reusable / "ci-build.manifest.json"
            run_command(
                "python3", SCRIPTS / "reusable_build.py",
                "--inventory-dir", source / "generated/ci_inventory",
                "create",
                "--source", source,
                "--archive", archive,
                "--manifest", manifest,
                "--candidate-commit", COMMIT_A,
                "--profile", "default",
                "--image-digest", IMAGE_DIGEST,
                "--workflow-commit", COMMIT_B,
            )
            identity = json.loads(manifest.read_text(encoding="utf-8"))
            binary_dir = root / "bin"
            binary_dir.mkdir()
            gh_log = root / "gh.log"
            gh = binary_dir / "gh"
            gh.write_text(
                "#!/usr/bin/env bash\n"
                "set -Eeuo pipefail\n"
                "printf '%q ' \"$@\" >> \"$CI_TEST_GH_LOG\"; printf '\\n' >> \"$CI_TEST_GH_LOG\"\n"
                "printf '[]\\n'\n",
                encoding="utf-8",
            )
            gh.chmod(0o755)
            destination = root / "destination"
            run_command(
                "bash", SCRIPTS / "reusable_build_consume.sh",
                environment={
                    "PATH": f"{binary_dir}:{os.environ['PATH']}",
                    "CI_BUILD_PROFILE": "default",
                    "CI_CANDIDATE_COMMIT": COMMIT_A,
                    "CI_IMAGE_DIGEST": IMAGE_DIGEST,
                    "CI_MATRIX_SHA256": str(identity["matrix_sha256"]),
                    "CI_REUSABLE_DESTINATION": str(destination),
                    "CI_REUSABLE_DIR": str(reusable),
                    "CI_TEST_GH_LOG": str(gh_log),
                    "CI_WORKFLOW_COMMIT": COMMIT_B,
                    "GITHUB_REPOSITORY": "kevin-zf1123/photospider",
                },
            )
            self.assertEqual((destination / "ci/result.txt").read_text(encoding="utf-8"), "verified\n")
            attestations = gh_log.read_text(encoding="utf-8").splitlines()
            self.assertEqual(len(attestations), 2)
            self.assertTrue(all("--deny-self-hosted-runners" in line for line in attestations))

    def test_targeted_attestation_separates_source_and_signer_digests(self) -> None:
        """Bind candidate source and reusable-workflow signer independently."""
        with tempfile.TemporaryDirectory() as temporary_text:
            root = Path(temporary_text)
            installed = root / "installed-prefix"
            (installed / "include/photospider").mkdir(parents=True)
            (installed / "include/photospider/product.hpp").write_text(
                "#pragma once\n", encoding="utf-8"
            )
            (installed / "lib/cmake/Photospider").mkdir(parents=True)
            (installed / "lib/cmake/Photospider/PhotospiderConfig.cmake").write_text(
                "set(Photospider_FOUND TRUE)\n", encoding="utf-8"
            )
            (installed / "lib/libphotospider.a").write_bytes(b"installed-product")
            archive, manifest, identity = self._create_targeted(
                root, "installed-package", installed
            )
            manifest_sha256 = hashlib.sha256(manifest.read_bytes()).hexdigest()
            binary_dir = root / "bin"
            binary_dir.mkdir()
            gh_log = root / "gh.log"
            github_environment = root / "github-environment"
            github_environment.write_text("", encoding="utf-8")
            gh = binary_dir / "gh"
            gh.write_text(
                "#!/usr/bin/env bash\n"
                "set -Eeuo pipefail\n"
                "printf '%q ' \"$@\" >> \"$CI_TEST_GH_LOG\"; "
                "printf '\\n' >> \"$CI_TEST_GH_LOG\"\n"
                "printf '[]\\n'\n",
                encoding="utf-8",
            )
            gh.chmod(0o755)
            run_command(
                "bash",
                SCRIPTS / "targeted_artifact_consume.sh",
                environment={
                    "PATH": f"{binary_dir}:{os.environ['PATH']}",
                    "CI_ARTIFACT_ROLE": "installed-package",
                    "CI_BUILD_PROFILE": "default",
                    "CI_CANDIDATE_COMMIT": COMMIT_A,
                    "CI_IMAGE_DIGEST": IMAGE_DIGEST,
                    "CI_MATRIX_SHA256": str(identity["matrix_sha256"]),
                    "CI_TARGETED_ARCHIVE": str(archive),
                    "CI_TARGETED_MANIFEST": str(manifest),
                    "CI_TARGETED_DESTINATION": str(root / "restored-package"),
                    "CI_TEST_GH_LOG": str(gh_log),
                    "CI_WORKFLOW_COMMIT": COMMIT_B,
                    "GITHUB_ENV": str(github_environment),
                    "GITHUB_REPOSITORY": "kevin-zf1123/photospider",
                },
            )
            attestations = gh_log.read_text(encoding="utf-8").splitlines()
            self.assertEqual(len(attestations), 2)
            for line in attestations:
                self.assertIn(f"--source-digest {COMMIT_A}", line)
                self.assertIn(f"--signer-digest {COMMIT_B}", line)
                self.assertIn(
                    "--signer-workflow "
                    "kevin-zf1123/photospider/.github/workflows/"
                    "ci-integration-suite.yml",
                    line,
                )
            manifest_attestation = next(
                line for line in attestations if "manifest.json" in line
            )
            self.assertIn("photospider-targeted-manifest", manifest_attestation)
            self.assertNotIn(str(manifest), manifest_attestation)
            self.assertEqual(
                github_environment.read_text(encoding="utf-8"),
                "CI_STATIC_PACKAGE_MANIFEST_SHA256="
                + manifest_sha256
                + "\n",
            )

    def test_producer_rejects_cached_empty_and_caller_forged_identity(self) -> None:
        """Reject stale origins, missing measured surfaces, and forged compiler data."""
        with tempfile.TemporaryDirectory() as temporary_text:
            root = Path(temporary_text)
            source = root / "source"
            inventory = self._prepare_build(source)
            common = (
                "python3", SCRIPTS / "reusable_build.py",
                "--inventory-dir", inventory,
                "create",
                "--source", source,
                "--archive", root / "candidate.tar.gz",
                "--manifest", root / "candidate.manifest.json",
                "--candidate-commit", COMMIT_A,
                "--profile", "default",
                "--image-digest", IMAGE_DIGEST,
                "--workflow-commit", COMMIT_B,
            )

            stamp = source / ".photospider-ci-build-complete"
            valid_stamp = stamp.read_text(encoding="utf-8")
            stamp.write_text(valid_stamp.replace(f"build_dir={source.resolve()}", "build_dir=/cached/tree"), encoding="utf-8")
            cached = run_command(*common, expect_success=False)
            self.assertIn("completion stamp build_dir mismatch", cached.stderr)
            stamp.write_text(valid_stamp, encoding="utf-8")

            package_config = source / "PhotospiderConfig.cmake"
            valid_config = package_config.read_text(encoding="utf-8")
            package_config.write_text("# empty package surface\n", encoding="utf-8")
            empty = run_command(*common, expect_success=False)
            self.assertIn("component/target inventory is empty", empty.stderr)
            package_config.write_text(valid_config, encoding="utf-8")

            digest = ProfileReaderContractTest._write_complete_versioned_identity(inventory)
            module = self._load_reusable_module()
            measured = module._measured_identity_value(
                REPO_ROOT,
                source,
                inventory,
                COMMIT_A,
                "default",
                digest,
                False,
            )
            measured["compiler"]["cxx_compiler_id"] = "caller-forged"
            (inventory / "reusable_build_identity_v1.json").write_text(
                json.dumps(measured, sort_keys=True, separators=(",", ":"), ensure_ascii=True) + "\n",
                encoding="utf-8",
            )
            forged = run_command(*common, expect_success=False)
            self.assertIn("differs from protected measurement", forged.stderr)

    def test_protected_build_entry_rejects_a_preexisting_output_tree(self) -> None:
        """Execute the maintained freshness helper against cached and empty paths."""
        source = (SCRIPTS / "build_integrity.sh").read_text(encoding="utf-8")
        match = re.search(
            r"(?ms)^prepare_fresh_producer_build\(\) \{.*?^\}\n\nprepare_fresh_producer_build$",
            source,
        )
        self.assertIsNotNone(match)
        with tempfile.TemporaryDirectory() as temporary_text:
            repository = Path(temporary_text) / "repository"
            build = repository / "build/ci"
            build.mkdir(parents=True)
            harness = Path(temporary_text) / "fresh-build-contract.sh"
            harness.write_text(
                "#!/usr/bin/env bash\nset -Eeuo pipefail\n" + match.group(0) + "\n",
                encoding="utf-8",
            )
            harness.chmod(0o755)
            cached = run_command(
                "bash", harness,
                environment={"REPO_ROOT": str(repository), "BUILD_DIR": str(build)},
                expect_success=False,
            )
            self.assertIn("refuses a cached or residual build directory", cached.stderr)
            build.rmdir()
            run_command(
                "bash", harness,
                environment={"REPO_ROOT": str(repository), "BUILD_DIR": str(build)},
            )
            self.assertTrue(build.is_dir())
            self.assertFalse(build.is_symlink())


class StaticProductConsumerInputContractTest(unittest.TestCase):
    """Exercise the package-input CLI boundary without running nested builds."""

    @staticmethod
    def _load_consumer_module() -> object:
        """Load the maintained package consumer with dataclass registration."""
        module_path = REPO_ROOT / "tests/integration/static_product_consumer_smoke.py"
        name = "static_product_consumer_input_contract"
        specification = importlib.util.spec_from_file_location(name, module_path)
        if specification is None or specification.loader is None:
            raise AssertionError("cannot load static product consumer module")
        module = importlib.util.module_from_spec(specification)
        sys.modules[name] = module
        sys.path.insert(0, str(module_path.parent))
        try:
            specification.loader.exec_module(module)
        finally:
            sys.path.pop(0)
            sys.modules.pop(name, None)
        return module

    def test_package_inputs_are_paired_disjoint_and_not_cleanup_owned(self) -> None:
        """Accept exact package inputs and reject mixed, linked, or overlapping paths."""
        module = self._load_consumer_module()
        with tempfile.TemporaryDirectory() as temporary_text:
            root = Path(temporary_text)
            prefix = root / "artifact/ci/installed"
            metadata = root / "artifact/ci/producer"
            prefix.mkdir(parents=True)
            metadata.mkdir(parents=True)
            (prefix / "prefix-sentinel").write_text("installed\n", encoding="utf-8")
            (metadata / "metadata-sentinel").write_text("metadata\n", encoding="utf-8")
            work = root / "consumer-work"

            def arguments(**overrides: object) -> SimpleNamespace:
                """Return one package-input namespace with selected overrides."""
                values: dict[str, object] = {
                    "build": None,
                    "configure_fresh_producer": False,
                    "installed_prefix": str(prefix),
                    "producer_metadata": str(metadata),
                    "work": str(work),
                }
                values.update(overrides)
                return SimpleNamespace(**values)

            resolved = module.resolve_execution_inputs(arguments(), REPO_ROOT)
            self.assertTrue(resolved.package_input)
            self.assertEqual(resolved.installed_prefix, prefix.resolve())
            self.assertEqual(resolved.build_metadata, metadata.resolve())
            work.mkdir()
            (work / "job-owned").write_text("temporary\n", encoding="utf-8")
            module.strict_remove_tree(work)
            self.assertTrue((prefix / "prefix-sentinel").is_file())
            self.assertTrue((metadata / "metadata-sentinel").is_file())

            invalid = (
                arguments(producer_metadata=None),
                arguments(build=str(root / "build")),
                arguments(configure_fresh_producer=True),
                arguments(work=str(prefix / "nested-work")),
            )
            for value in invalid:
                with self.assertRaises(ValueError):
                    module.resolve_execution_inputs(value, REPO_ROOT)

            link = root / "metadata-link"
            link.symlink_to(metadata, target_is_directory=True)
            with self.assertRaises(ValueError):
                module.resolve_execution_inputs(
                    arguments(producer_metadata=str(link)), REPO_ROOT
                )


class RulesetContractTest(unittest.TestCase):
    """Exercise exact stable checks and coherent default-branch policy."""

    def test_valid_fixture_passes_and_incoherent_fixture_fails(self) -> None:
        """Accept only the exact GitHub Actions-bound stable check pair."""
        base = {
            "conditions": {"ref_name": {"exclude": [], "include": ["~DEFAULT_BRANCH"]}},
            "enforcement": "active",
            "id": 1,
            "name": "main protect",
            "rules": [
                {"type": "deletion"},
                {"type": "non_fast_forward"},
                {"type": "required_linear_history"},
                {
                    "parameters": {
                        "allowed_merge_methods": ["rebase", "squash"],
                        "required_review_thread_resolution": True,
                    },
                    "type": "pull_request",
                },
                {
                    "parameters": {
                        "required_status_checks": [
                            {"context": "healthcheck", "integration_id": 15368},
                            {"context": "integration", "integration_id": 15368},
                        ],
                        "strict_required_status_checks_policy": True,
                    },
                    "type": "required_status_checks",
                },
            ],
            "target": "branch",
        }
        with tempfile.TemporaryDirectory() as temporary_text:
            valid = Path(temporary_text) / "valid.json"
            invalid = Path(temporary_text) / "invalid.json"
            valid.write_text(json.dumps([base]), encoding="utf-8")
            broken = json.loads(json.dumps(base))
            broken["rules"][3]["parameters"]["required_review_thread_resolution"] = False
            broken["rules"][3]["parameters"]["allowed_merge_methods"] = ["merge"]
            invalid.write_text(json.dumps([broken]), encoding="utf-8")
            run_command(
                "python3", SCRIPTS / "ruleset_readback.py",
                "--input", valid,
            )
            failed = run_command(
                "python3", SCRIPTS / "ruleset_readback.py",
                "--input", invalid,
                expect_success=False,
            )
            self.assertIn("required conversation resolution is disabled", failed.stderr)

    def test_live_readback_aggregates_later_ruleset_pages(self) -> None:
        """Find a second default-branch ruleset after the first 30 summaries."""
        with tempfile.TemporaryDirectory() as temporary_text:
            root = Path(temporary_text)
            binary_dir = root / "bin"
            binary_dir.mkdir()
            gh_log = root / "gh.log"
            gh = binary_dir / "gh"
            gh.write_text(
                f"#!{sys.executable}\n"
                "import json\n"
                "import os\n"
                "import sys\n"
                "args = sys.argv[1:]\n"
                "with open(os.environ['CI_TEST_GH_LOG'], 'a', encoding='utf-8') as handle:\n"
                "    handle.write(json.dumps(args, separators=(',', ':')) + '\\n')\n"
                "endpoint = args[-1]\n"
                "if endpoint.endswith('rulesets?includes_parents=false'):\n"
                "    first = [{'id': identifier} for identifier in range(1, 31)]\n"
                "    second = [{'id': 31}]\n"
                "    print(json.dumps([first, second] if '--paginate' in args and '--slurp' in args else first))\n"
                "else:\n"
                "    identifier = int(endpoint.rsplit('/', 1)[1])\n"
                "    active = identifier in (1, 31)\n"
                "    print(json.dumps({\n"
                "        'conditions': {'ref_name': {'exclude': [], 'include': ['~DEFAULT_BRANCH'] if active else []}},\n"
                "        'enforcement': 'active' if active else 'disabled',\n"
                "        'id': identifier,\n"
                "        'name': f'ruleset-{identifier}',\n"
                "        'rules': [],\n"
                "        'target': 'branch',\n"
                "    }))\n",
                encoding="utf-8",
            )
            gh.chmod(0o755)
            failed = run_command(
                "python3", SCRIPTS / "ruleset_readback.py",
                "--repository", "kevin-zf1123/photospider",
                environment={
                    "CI_TEST_GH_LOG": str(gh_log),
                    "PATH": f"{binary_dir}:{os.environ['PATH']}",
                },
                expect_success=False,
            )
            self.assertIn("expected exactly one active default-branch ruleset, found 2", failed.stderr)
            calls = [json.loads(line) for line in gh_log.read_text(encoding="utf-8").splitlines()]
            list_call = calls[0]
            self.assertIn("--paginate", list_call)
            self.assertIn("--slurp", list_call)
            self.assertIn(
                "repos/kevin-zf1123/photospider/rulesets?includes_parents=false",
                list_call,
            )
            self.assertTrue(any(call[-1].endswith("/rulesets/31") for call in calls))


class LockSurfaceContractTest(unittest.TestCase):
    """Exercise the complete active protected lock/workflow verifier."""

    @staticmethod
    def _load_lock_module() -> object:
        """Load the protected lock verifier for isolated workflow fixtures."""
        module_path = SCRIPTS / "ci_lock_verify.py"
        specification = importlib.util.spec_from_file_location(
            "ci_lock_verify_contract", module_path
        )
        if specification is None or specification.loader is None:
            raise AssertionError("cannot load CI lock verifier")
        module = importlib.util.module_from_spec(specification)
        specification.loader.exec_module(module)
        return module

    @staticmethod
    def _replace_workflow_job_fragment(
        workflow: str,
        job_name: str,
        original: str,
        replacement: str,
    ) -> str:
        """Replace one exact fragment only inside a named top-level job.

        Args:
            workflow: Complete maintained workflow text.
            job_name: Exact two-space-indented job identifier.
            original: Fragment required exactly once in the selected job.
            replacement: Text that replaces ``original`` in that job.

        Returns:
            The complete workflow with the selected job changed.

        Raises:
            AssertionError: The job is absent or the fragment is not unique in
                that job.

        Note:
            Scoping mutations prevents an unrelated caller or sibling job from
            accidentally satisfying a negative permission fixture.
        """
        start_marker = f"  {job_name}:\n"
        start = workflow.find(start_marker)
        if start < 0:
            raise AssertionError(f"workflow job is absent: {job_name}")
        following = re.search(r"(?m)^  [a-z][a-z0-9_-]*:\n", workflow[start + len(start_marker) :])
        end = (
            len(workflow)
            if following is None
            else start + len(start_marker) + following.start()
        )
        block = workflow[start:end]
        if block.count(original) != 1:
            raise AssertionError(
                f"job {job_name} does not contain one exact fragment: {original!r}"
            )
        return workflow[:start] + block.replace(original, replacement, 1) + workflow[end:]

    def test_reusable_permission_ceiling_is_structurally_isolated(self) -> None:
        """Accept three exact callers and reject every inheritance/elevation drift."""
        module = self._load_lock_module()
        module._verify_reusable_workflow_permissions(REPO_ROOT)
        caller_path = REPO_ROOT / ".github/workflows/ci-integration.yml"
        shared_path = REPO_ROOT / ".github/workflows/ci-integration-suite.yml"
        caller = caller_path.read_text(encoding="utf-8")
        shared = shared_path.read_text(encoding="utf-8")
        cases = (
            (
                "readonly-write",
                "caller",
                "published-image-integration-readonly",
                "      attestations: read\n",
                "      attestations: write\n",
                "caller published-image-integration-readonly permissions differ",
            ),
            (
                "readonly-publication",
                "caller",
                "published-image-integration-readonly",
                "      publish_reusable_attestations: false\n",
                "      publish_reusable_attestations: true\n",
                "caller published-image-integration-readonly publication mode differs",
            ),
            (
                "trusted-downgrade",
                "caller",
                "published-image-integration-trusted",
                "      artifact-metadata: write\n",
                "      artifact-metadata: read\n",
                "caller published-image-integration-trusted permissions differ",
            ),
            (
                "execution-inherits",
                "shared",
                "identity-preflight",
                (
                    "    permissions:\n"
                    "      attestations: read\n"
                    "      contents: read\n"
                    "      packages: read\n"
                ),
                "",
                "job identity-preflight read-only permissions differ",
            ),
            (
                "execution-write",
                "shared",
                "security-darwin",
                "      contents: read\n",
                "      contents: write\n",
                "job security-darwin read-only permissions differ",
            ),
            (
                "attestation-local-permissions",
                "shared",
                "attest-targeted-artifacts",
                "    runs-on: ubuntu-24.04\n",
                "    runs-on: ubuntu-24.04\n    permissions:\n      contents: read\n",
                "attest-targeted-artifacts must inherit the trusted caller ceiling",
            ),
            (
                "attestation-condition",
                "shared",
                "attest-targeted-artifacts",
                "        inputs.publish_reusable_attestations &&\n",
                "        true &&\n",
                "attest-targeted-artifacts trust condition differs",
            ),
        )
        for label, owner, job_name, original, replacement, diagnostic in cases:
            with self.subTest(
                permission_drift=label
            ), tempfile.TemporaryDirectory() as temporary_text:
                root = Path(temporary_text)
                workflow_root = root / ".github/workflows"
                workflow_root.mkdir(parents=True)
                mutated_caller = caller
                mutated_shared = shared
                if owner == "caller":
                    mutated_caller = self._replace_workflow_job_fragment(
                        caller, job_name, original, replacement
                    )
                else:
                    mutated_shared = self._replace_workflow_job_fragment(
                        shared, job_name, original, replacement
                    )
                (workflow_root / "ci-integration.yml").write_text(
                    mutated_caller, encoding="utf-8"
                )
                (workflow_root / "ci-integration-suite.yml").write_text(
                    mutated_shared, encoding="utf-8"
                )
                with self.assertRaises(module.ContractError) as raised:
                    module._verify_reusable_workflow_permissions(root)
                self.assertIn(diagnostic, str(raised.exception))

    def test_unpinned_yaml_workflow_fails_the_same_lock_parser(self) -> None:
        """Reject an unpinned action in the formerly omitted YAML suffix."""
        module = self._load_lock_module()
        with tempfile.TemporaryDirectory() as temporary_text:
            root = Path(temporary_text)
            workflow_root = root / ".github/workflows"
            workflow_root.mkdir(parents=True)
            (workflow_root / "unpinned.yaml").write_text(
                "jobs:\n"
                "  test:\n"
                "    runs-on: ubuntu-24.04\n"
                "    steps:\n"
                "      - uses: actions/checkout@v4 # v4\n",
                encoding="utf-8",
            )
            locked_commit = "1" * 40
            with self.assertRaises(module.ContractError) as raised:
                module._verify_workflows(
                    root, {"actions/checkout": ("v4", locked_commit)}
                )
            self.assertIn(f"uses v4, expected {locked_commit}", str(raised.exception))

    def test_checkout_credentials_must_be_explicitly_nonpersistent(self) -> None:
        """Reject pinned checkout steps that omit or enable persisted tokens."""
        module = self._load_lock_module()
        locked_commit = "1" * 40
        for credential_input in (
            "",
            "          persist-credentials: true\n",
            "        env:\n          persist-credentials: false\n",
        ):
            with self.subTest(credential_input=credential_input), tempfile.TemporaryDirectory() as temporary_text:
                root = Path(temporary_text)
                workflow_root = root / ".github/workflows"
                workflow_root.mkdir(parents=True)
                (workflow_root / "checkout.yml").write_text(
                    "jobs:\n"
                    "  test:\n"
                    "    runs-on: ubuntu-24.04\n"
                    "    steps:\n"
                    f"      - uses: actions/checkout@{locked_commit} # v4\n"
                    "        with:\n"
                    + credential_input,
                    encoding="utf-8",
                )
                with self.assertRaises(module.ContractError) as raised:
                    module._verify_workflows(
                        root, {"actions/checkout": ("v4", locked_commit)}
                    )
                self.assertIn("persist-credentials: false", str(raised.exception))

    def test_pull_request_target_requires_universal_precheckout_fork_guard(self) -> None:
        """Reject a pull-request-target guard limited to protected branch names."""
        module = self._load_lock_module()
        lines = (
            "on:\n"
            "  pull_request_target:\n"
            "jobs:\n"
            "  protected-ci-paths:\n"
            "    steps:\n"
            "      - name: Reject fork pull request before checkout\n"
            "        if: github.event_name == 'pull_request_target' && "
            "startsWith(github.head_ref, 'CI/') && "
            "github.event.pull_request.head.repo.full_name != github.repository\n"
            "        run: echo 'Fork pull requests are rejected before checkout.'\n"
            "      - uses: actions/checkout@1111111111111111111111111111111111111111 # v4\n"
        ).splitlines()
        with self.assertRaises(module.ContractError) as raised:
            module._verify_pull_request_target_trust(Path("fork.yml"), lines)
        self.assertIn("incorrectly limited to CI/**", str(raised.exception))

    def test_workflow_inventory_rejects_links_specials_and_unknown_entries(self) -> None:
        """Reject every noncanonical directory entry before YAML parsing."""
        module = self._load_lock_module()
        cases = ("symlink", "fifo", "directory", "unknown-extension")
        for unsafe_kind in cases:
            with self.subTest(unsafe_kind=unsafe_kind), tempfile.TemporaryDirectory() as temporary_text:
                root = Path(temporary_text)
                workflow_root = root / ".github/workflows"
                workflow_root.mkdir(parents=True)
                if unsafe_kind == "symlink":
                    outside = root / "outside.yml"
                    outside.write_text("jobs: {}\n", encoding="utf-8")
                    (workflow_root / "unsafe.yml").symlink_to(outside)
                elif unsafe_kind == "fifo":
                    os.mkfifo(workflow_root / "unsafe.yaml")
                elif unsafe_kind == "directory":
                    (workflow_root / "nested.yml").mkdir()
                else:
                    (workflow_root / "README.md").write_text("unexpected\n", encoding="utf-8")
                with self.assertRaises(module.ContractError):
                    module._workflow_files(root)

    def test_repository_lock_surface_is_exact(self) -> None:
        """Reject no active floating actions, runners, images, or install inputs."""
        run_command("python3", SCRIPTS / "ci_lock_verify.py")


if __name__ == "__main__":
    unittest.main(verbosity=2)
