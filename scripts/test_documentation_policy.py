"""Publication attack fixtures. All writes and commits are in disposable repos."""

import gzip
import importlib.util
import io
import json
import os
import subprocess
import sys
import tarfile
import tempfile
import unittest
import zipfile
from pathlib import Path
from unittest.mock import patch

spec = importlib.util.spec_from_file_location(
    "publication_guard", Path(__file__).with_name("check_documentation.py")
)
guard = importlib.util.module_from_spec(spec)
spec.loader.exec_module(guard)


def zip_bytes(files):
    stream = io.BytesIO()
    with zipfile.ZipFile(stream, "w") as container:
        for name, data in files.items():
            container.writestr(name, data)
    return stream.getvalue()


class PublicationTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory(prefix="documentation-fixture-")
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)
        self.env = {k: v for k, v in os.environ.items() if not k.startswith("GIT_")}
        clean_environment = patch.dict(os.environ, self.env, clear=True)
        clean_environment.start()
        self.addCleanup(clean_environment.stop)
        self.git("init", "-q")
        self.git("config", "user.name", "Fixture")
        self.git("config", "user.email", "fixture@example.invalid")
        self.policy = {
            "version": 1,
            "repository": "golba98/fixture",
            "private_paths": [".private/", "docs/tmp/", ".agents/teamwork/"],
            "protected_paths": [guard.POLICY, guard.SCRIPT, ".gitignore"],
            "private_hashes": [],
            "private_git_blobs": [],
            "public_markdown": {},
        }
        self.write(".gitignore", "/.private/\n/docs/tmp/\n")
        self.write("README.md", "# Public project\n\nInstallation guide.\n")
        self.write(guard.POLICY, json.dumps(self.policy))
        self.git("add", ".")
        self.git("commit", "-qm", "Public baseline")
        self.baseline = guard.scan(guard.Snapshot(self.root, ref="HEAD"), self.policy)[0]
        path = guard.state_path(self.root)
        path.parent.mkdir()
        path.write_text(
            json.dumps({"policy": self.policy, "catalog": self.baseline, "approved_bundles": []})
        )

    def git(self, *args):
        return subprocess.check_output(
            ["git", "--no-optional-locks", "-C", str(self.root), *args],
            env=self.env,
            stderr=subprocess.DEVNULL,
        )

    def write(self, name, data):
        path = self.root / name
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_bytes(data if isinstance(data, bytes) else data.encode())

    def result(self, staged=False, ref=None):
        return guard.local_check(
            self.root, guard.Snapshot(self.root, ref=ref, staged=staged), False
        )

    def test_repeat_proposal_preserves_project_policy_formatting(self):
        guard.propose(self.root, guard.Snapshot(self.root), ".private/review.json")
        path = self.root / guard.POLICY
        formatted = json.dumps(json.loads(path.read_text()), indent=4) + "\n"
        path.write_text(formatted)
        guard.propose(self.root, guard.Snapshot(self.root), ".private/review.json")
        self.assertEqual(path.read_text(), formatted)
        proposal = json.loads((self.root / ".private/review.json").read_text())
        self.assertEqual(proposal["catalog"][guard.POLICY], guard.sha(formatted.encode()))

    def test_ordinary_project_types_do_not_require_document_review(self):
        for name, text in [
            ("src/main.rs", "fn main() {}"),
            ("app.py", "print(1)"),
            ("main.ts", "export const n = 1;"),
            ("main.js", "const n = 1;"),
            ("main.cpp", "int main() { return 0; }"),
        ]:
            self.write(name, text)
        self.git("add", ".")
        self.assertEqual(self.result(True)[2], [])

    def test_dependency_updates_pass_but_publication_command_changes_require_review(self):
        self.write("package.json", json.dumps({"dependencies": {"example": "1"}}))
        self.git("add", ".")
        self.assertFalse(self.result(True)[2])
        self.write("package.json", json.dumps({"dependencies": {"example": "2"}}))
        self.git("add", ".")
        self.assertFalse(self.result(True)[2])
        self.write("package.json", json.dumps({"scripts": {"check:docs": "echo bypass"}}))
        self.git("add", ".")
        self.assertTrue(any("package.json#publication" in e for e in self.result(True)[2]))

    def test_readme_exact_bytes_and_new_public_documents_need_review(self):
        self.write("README.md", "# Public project\n\nNew installation command.\n")
        self.write("docs/research.md", "# Reproducible research\n\nMethods and measured results.\n")
        self.git("add", ".")
        candidate = guard.Snapshot(self.root, staged=True)
        _catalog, digest, errors, _ = guard.validate(candidate, self.policy, self.baseline)
        self.assertEqual(len(errors), 2)
        self.assertEqual(guard.validate(candidate, self.policy, self.baseline, [digest])[2], [])
        self.write("README.md", "# Public project\n\nDifferent bytes after review.\n")
        self.git("add", "README.md")
        self.assertTrue(
            guard.validate(
                guard.Snapshot(self.root, staged=True), self.policy, self.baseline, [digest]
            )[2]
        )

    def test_unstaged_bytes_and_approval_config_cannot_approve_staged_notes(self):
        self.write("README.md", "# Session handoff\nInternal progress\n")
        self.git("add", "README.md")
        self.write("README.md", "# Public project\n\nInstallation guide.\n")
        self.assertTrue(self.result(True)[2])
        self.policy["public_markdown"]["README.md"] = guard.sha(
            b"# Session handoff\nInternal progress\n"
        )
        self.write(guard.POLICY, json.dumps(self.policy))
        self.git("add", guard.POLICY)
        self.assertTrue(self.result(True)[2])

    def test_force_added_private_files_are_always_rejected(self):
        for name in [
            ".private/notes.md",
            "docs/tmp/notes.txt",
            ".agents/teamwork/agent/DISPATCH.md",
            "nested/.PRIVATE/notes.md",
            "nested/%2eprivate/notes.md",
            "nested/．private/notes.md",
        ]:
            self.write(name, "private prose")
            self.git("add", "-f", "--", name)
        result = self.result(True)
        self.assertGreaterEqual(sum("Private path" in e for e in result[2]), 6)
        self.assertTrue(
            guard.validate(
                guard.Snapshot(self.root, staged=True), self.policy, self.baseline, [result[1]]
            )[2]
        )

    def test_agent_entrypoints_are_private_in_git_and_archives(self):
        for name in ("AGENTS.md", "CLAUDE.md", "GEMINI.md", "nested/agents.md"):
            self.write(name, "# Local agent instructions\n")
            self.git("add", "-f", "--", name)
        errors = self.result(True)[2]
        self.assertEqual(sum("Private path" in error for error in errors), 4)
        archive_errors = []
        guard.inspect_archive(
            "package.zip",
            zip_bytes({"package/AGENTS.md": b"local"}),
            self.policy,
            archive_errors,
            [],
        )
        self.assertTrue(archive_errors)

    def test_fresh_ci_package_inputs_must_match_committed_documentation(self):
        self.policy["public_markdown"] = {
            "README.md": guard.sha((self.root / "README.md").read_bytes())
        }
        self.write(guard.POLICY, json.dumps(self.policy))
        self.git("add", guard.POLICY)
        self.git("commit", "-qm", "Reviewed source manifest")
        guard.state_path(self.root).unlink()
        self.assertFalse(guard.package_source_check(self.root)[2])
        self.write("README.md", "# New, uncommitted public content\n")
        self.assertTrue(guard.package_source_check(self.root)[2])

    def test_renamed_markdown_and_known_private_blobs(self):
        self.git("mv", "README.md", "guide.txt")
        self.assertTrue(self.result(True)[2])
        data = b"private historical task notes"
        self.write("innocent.dat", data)
        self.git("add", "innocent.dat")
        oid = self.git("rev-parse", ":innocent.dat").decode().strip()
        self.policy["private_git_blobs"] = [oid]
        self.assertTrue(
            any(
                "Renamed private content" in e
                for e in guard.scan(guard.Snapshot(self.root, staged=True), self.policy)[1]
            )
        )

    def test_private_archive_members_nested_archives_and_extension_tricks(self):
        self.write(
            "payload.bin",
            zip_bytes({"safe/inner.zip": zip_bytes({".private/handoff.md": b"private"})}),
        )
        self.git("add", ".")
        self.assertTrue(any("archive member" in e for e in self.result(True)[2]))
        for name in ["../escape.md", "/absolute.md", "C:\\private.md", "docs/%2e%2e/escape.md"]:
            errors, warnings = [], []
            guard.inspect_archive(
                "fixture.zip", zip_bytes({name: b"notes"}), self.policy, errors, warnings
            )
            self.assertTrue(errors, name)

    def test_tar_links_and_expansion_budget(self):
        stream = io.BytesIO()
        with tarfile.open(fileobj=stream, mode="w") as container:
            entry = tarfile.TarInfo("guide.md")
            entry.type = tarfile.SYMTYPE
            entry.linkname = ".private/notes.md"
            container.addfile(entry)
        errors, warnings = [], []
        guard.inspect_archive("fixture.tar", stream.getvalue(), self.policy, errors, warnings)
        self.assertTrue(errors)
        old = guard.MAX_EXPANDED
        try:
            guard.MAX_EXPANDED = 4
            errors = []
            guard.inspect_archive(
                "large.zip", zip_bytes({"guide.md": b"12345"}), self.policy, errors, []
            )
            self.assertTrue(errors)
        finally:
            guard.MAX_EXPANDED = old

    def test_standalone_compressed_markdown_is_inspected(self):
        errors, warnings = [], []
        members = guard.inspect_archive(
            "guide.md.gz", gzip.compress(b"# Public guide\n"), self.policy, errors, warnings
        )
        self.assertFalse(errors)
        self.assertIn("guide.md.gz!guide.md", members)

    def test_hook_composition_preserves_arguments_and_push_input(self):
        hooks = self.root / ".git/hooks"
        previous = hooks / "pre-push"
        previous.write_text(
            '#!/bin/sh\nprintf "%s\\n" "$@" > .git/legacy-args\ncat > .git/legacy-input\n'
        )
        previous.chmod(0o755)
        guard.install_hooks(self.root)
        remote = self.root / ".private/remote.git"
        subprocess.run(["git", "init", "--bare", "-q", str(remote)], check=True, env=self.env)
        head = self.git("rev-parse", "HEAD").decode().strip()
        self.git(
            "-c", "core.hooksPath=/dev/null", "push", "-q", str(remote), "HEAD:refs/heads/main"
        )
        data = ("refs/heads/main " + head + " refs/heads/main " + head + "\n").encode()
        process = subprocess.run(
            [str(previous), "origin", str(remote)],
            input=data,
            check=False,
            cwd=self.root,
            env=self.env,
            capture_output=True,
        )
        self.assertEqual(process.returncode, 0, process.stderr)
        self.assertEqual((self.root / ".git/legacy-input").read_bytes(), data)
        self.assertEqual(
            (self.root / ".git/legacy-args").read_text().splitlines(), ["origin", str(remote)]
        )

    def test_linked_worktree_uses_common_review_state(self):
        worktree = self.root / ".private/linked"
        self.git("worktree", "add", "-qb", "linked", str(worktree), "HEAD")
        self.assertEqual(guard.state_path(worktree), guard.state_path(self.root))
        self.assertFalse(
            guard.local_check(worktree, guard.Snapshot(worktree, staged=True), False)[2]
        )

    def test_internal_heuristic_warns_without_claiming_semantic_certainty(self):
        self.write("docs/notes.md", "# Agent handoff\n\nCurrent session notes.\n")
        self.git("add", ".")
        _catalog, digest, _errors, warnings = self.result(True)
        self.assertTrue(warnings)
        self.assertEqual(
            guard.validate(
                guard.Snapshot(self.root, staged=True), self.policy, self.baseline, [digest]
            )[2],
            [],
        )

    def test_protected_code_and_missing_policy_require_review(self):
        self.write(guard.SCRIPT, "# replacement checker")
        self.git("add", ".")
        self.assertTrue(self.result(True)[2])
        self.git("rm", guard.POLICY)
        with self.assertRaises(ValueError):
            self.result(True)

    def test_symlink_and_normalization_collision(self):
        (self.root / "public.md").symlink_to(".private/notes.md")
        self.write("readme.MD", "# Duplicate\n")
        self.git("add", ".")
        errors = self.result(True)[2]
        self.assertTrue(any("symlink" in e for e in errors))
        self.assertTrue(any("collision" in e for e in errors))

    def test_noninteractive_owner_review_is_refused(self):
        process = subprocess.run(
            [
                sys.executable,
                str(Path(guard.__file__)),
                "--root",
                str(self.root),
                "--review",
                ".private/proposal.json",
            ],
            input=b"",
            check=False,
            capture_output=True,
        )
        self.assertNotEqual(process.returncode, 0)
        self.assertIn(b"interactive host terminal", process.stderr)

    def test_owner_review_records_exact_snapshot_and_later_edits_fail(self):
        self.write("README.md", "# Public project\n\nReviewed installation improvement.\n")
        guard.propose(self.root, guard.Snapshot(self.root), ".private/review.json")
        proposal = json.loads((self.root / ".private/review.json").read_text())
        with (
            patch.object(sys.stdin, "isatty", return_value=True),
            patch.object(sys.stdout, "isatty", return_value=True),
            patch("builtins.input", return_value="APPROVE " + proposal["digest"]),
        ):
            guard.review(self.root, self.root / ".private/review.json")
        self.assertFalse(self.result()[2])
        self.write("README.md", "# Public project\n\nUnreviewed later addition.\n")
        self.assertTrue(self.result()[2])

    def test_owner_comments_are_bound_to_author_head_and_repository_digest(self):
        digest = guard.bundle(self.policy, self.baseline)
        comments = [{"user": {"id": 42, "type": "User"}, "body": "/docs-approve head " + digest}]
        self.assertTrue(guard.owner_comment(comments, 42, "head", digest))
        self.assertFalse(guard.owner_comment(comments, 7, "head", digest))
        self.assertFalse(guard.owner_comment(comments, 42, "other-head", digest))
        self.assertFalse(
            guard.owner_comment(
                comments,
                42,
                "head",
                guard.bundle(dict(self.policy, repository="golba98/other"), self.baseline),
            )
        )

    def test_ci_uses_trusted_code_and_owner_approval_cannot_override_private_paths(self):
        base = self.git("rev-parse", "HEAD").decode().strip()
        self.write("README.md", "# Public guide\n\nReviewed install instructions.\n")
        self.write(guard.SCRIPT, "raise RuntimeError('PR checker must never execute')\n")
        self.git("add", ".")
        self.git("commit", "-qm", "Proposed documentation and checker")
        head = self.git("rev-parse", "HEAD").decode().strip()
        self.git("checkout", "--detach", "-q", base)
        digest = guard.bundle(
            self.policy, guard.scan(guard.Snapshot(self.root, ref=head), self.policy)[0]
        )
        statuses = []
        real_git = guard.git

        def mocked_git(root, *args, **kwargs):
            if "fetch" in args:
                return b""
            if args == ("rev-parse", "FETCH_HEAD"):
                return (head + "\n").encode()
            return real_git(root, *args, **kwargs)

        def mocked_api(repository, endpoint, token, method="GET", payload=None):
            if endpoint == "":
                return {"owner": {"id": 42}}
            if endpoint.startswith("/pulls/"):
                return {"head": {"sha": head}, "base": {"sha": base}}
            if endpoint.startswith("/issues/"):
                return [
                    {
                        "user": {"id": 42, "type": "User"},
                        "body": "/docs-approve " + head + " " + digest,
                    }
                ]
            if endpoint.startswith("/statuses/"):
                statuses.append(payload["state"])
                return {}
            raise AssertionError(endpoint)

        with (
            patch.object(guard, "git", mocked_git),
            patch.object(guard, "api", mocked_api),
            patch.dict(
                os.environ, {"GITHUB_REPOSITORY": "golba98/fixture", "GITHUB_TOKEN": "fixture-only"}
            ),
        ):
            self.assertFalse(guard.ci(self.root, 1))
        self.assertEqual(statuses, ["pending", "success"])

        self.git("checkout", "--detach", "-q", head)
        self.write(".private/notes.md", "# Agent handoff\nPrivate\n")
        self.git("add", "-f", ".private/notes.md")
        self.git("commit", "-qm", "Unsafe private addition")
        head = self.git("rev-parse", "HEAD").decode().strip()
        self.git("checkout", "--detach", "-q", base)
        digest = guard.bundle(
            self.policy, guard.scan(guard.Snapshot(self.root, ref=head), self.policy)[0]
        )
        statuses.clear()
        with (
            patch.object(guard, "git", mocked_git),
            patch.object(guard, "api", mocked_api),
            patch.dict(
                os.environ, {"GITHUB_REPOSITORY": "golba98/fixture", "GITHUB_TOKEN": "fixture-only"}
            ),
        ):
            self.assertTrue(guard.ci(self.root, 1))
        self.assertEqual(statuses, ["pending", "failure"])

    def test_push_range_includes_reverted_intermediate_violation_and_tags(self):
        remote = self.root / ".private/remote.git"
        subprocess.run(["git", "init", "--bare", "-q", str(remote)], check=True, env=self.env)
        baseline = self.git("rev-parse", "HEAD").decode().strip()
        self.git("push", "-q", str(remote), "HEAD:refs/heads/main")
        self.write("docs/secret.md", "# Agent handoff\nPrivate progress\n")
        self.git("add", "docs/secret.md")
        self.git("commit", "-qm", "Intermediate leak")
        leak = self.git("rev-parse", "HEAD").decode().strip()
        self.git("rm", "docs/secret.md")
        self.git("commit", "-qm", "Revert leak")
        head = self.git("rev-parse", "HEAD").decode().strip()
        rows = ["refs/heads/main " + head + " refs/heads/main " + baseline]
        commits = guard.outgoing(self.root, rows, str(remote))
        self.assertIn(leak, commits)
        self.assertTrue(self.result(ref=leak)[2])
        rows = ["refs/tags/v1 " + head + " refs/tags/v1 " + "0" * 40]
        self.assertIn(leak, guard.outgoing(self.root, rows, str(remote)))


if __name__ == "__main__":
    unittest.main()
