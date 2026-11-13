"""Portable publication review. Git objects are data; no project code is executed.

Only a host owner review may add local approval receipts. Same-user writable
receipts/hooks are a convention, not an adversarial security boundary.
"""

import argparse
import base64
import bz2
import difflib
import gzip
import hashlib
import io
import json
import lzma
import os
import re
import stat
import subprocess
import sys
import tarfile
import unicodedata
import urllib.parse
import urllib.request
import zipfile
from contextlib import ExitStack
from pathlib import Path

POLICY = ".github/documentation-policy.json"
SCRIPT = "scripts/check_documentation.py"
MARKDOWN = (".md", ".markdown", ".mdown", ".mdx", ".mkd")
ARCHIVES = (
    ".zip",
    ".whl",
    ".jar",
    ".tar",
    ".tgz",
    ".tar.gz",
    ".tar.bz2",
    ".tar.xz",
    ".gz",
    ".bz2",
    ".xz",
    ".7z",
    ".rar",
)
MAX_FILE = 32 * 1024 * 1024
MAX_EXPANDED = 128 * 1024 * 1024
MAX_MEMBERS = 10000
MAX_DEPTH = 3
INTERNAL = re.compile(
    r"(?im)^(?:#{1,6}\s*)?(?:agent handoff|overhaul handoff|session (?:summary|decisions|decision log|handoff)|task (?:completion report|plan)|implementation (?:completion )?report|completion report|progress report|repository overhaul plan|internal (?:progress|investigation)|raw debugging transcript)\b"
)


def sha(data):
    return hashlib.sha256(data).hexdigest()


def encoded(value):
    return (
        json.dumps(value, sort_keys=True, ensure_ascii=True, separators=(",", ":")) + "\n"
    ).encode()


def git(root, *args, data=None):
    result = subprocess.run(
        ["git", "--no-optional-locks", "-C", str(root), *args],
        input=data,
        capture_output=True,
        check=False,
    )
    if result.returncode:
        raise ValueError("Git operation failed: " + " ".join(args[:2]))
    return result.stdout


def canonical(name):
    name = unicodedata.normalize("NFKC", name).casefold().replace("\\", "/")
    for _ in range(3):
        decoded = urllib.parse.unquote(name)
        if decoded == name:
            break
        name = decoded
    return "/".join(part.rstrip(" .") for part in name.split("/"))


def safe_name(name):
    normalized = canonical(name)
    parts = normalized.split("/")
    return (
        bool(name)
        and not normalized.startswith("/")
        and not re.match(r"^[a-z]:", normalized)
        and all(p not in ("", ".", "..") for p in parts)
    )


def private_name(name, policy):
    name = canonical(name)
    if name.rsplit("/", 1)[-1] in {"agents.md", "agents.override.md", "claude.md", "gemini.md"}:
        return True
    if ".private" in name.split("/"):
        return True
    return any(
        "/" + canonical(prefix).strip("/") + "/" in "/" + name + "/"
        for prefix in policy.get("private_paths", [])
    )


def markdown(name, data=None):
    if canonical(name).endswith(MARKDOWN):
        return True
    # Detect prose renamed to .txt or extensionless names without classifying
    # ordinary source comments as public documentation.
    suffix = Path(name).suffix.lower()
    return (
        data is not None and suffix in ("", ".txt") and bool(re.match(rb"\s*#{1,6} [^\n]+\n", data))
    )


def archive_name(name):
    return canonical(name).endswith(ARCHIVES)


def protected(name, policy):
    return name in policy.get("protected_paths", []) or name.startswith(
        (".github/workflows/", ".githooks/")
    )


def publication_fragment(data):
    """Protect publication-related npm fields without gating dependency edits."""
    config = json.loads(data)
    scripts = config.get("scripts", {})
    selected = {
        name: value
        for name, value in scripts.items()
        if name
        in (
            "check:docs",
            "test:docs",
            "precommit",
            "prepush",
            "pre-commit",
            "pre-push",
            "prepare",
            "prepublishOnly",
            "prepack",
            "postpack",
        )
        or "check_documentation.py" in str(value)
    }
    result = {"scripts": selected} if selected else {}
    if "files" in config:
        result["files"] = config["files"]
    return result


class Snapshot:
    def __init__(self, root, ref=None, staged=False):
        self.root = Path(root)
        self.ref = ref
        self.staged = staged
        self.entries = {}
        if ref:
            rows = git(root, "ls-tree", "-rz", ref).split(b"\0")
            for row in filter(None, rows):
                meta, name = row.split(b"\t", 1)
                mode, _kind, oid = meta.decode().split()
                self.entries[os.fsdecode(name)] = (mode, oid)
        else:
            rows = git(root, "ls-files", "--stage", "-z").split(b"\0")
            for row in filter(None, rows):
                meta, name = row.split(b"\t", 1)
                mode, oid, stage = meta.decode().split()
                if stage != "0":
                    raise ValueError("Unmerged index: resolve conflicts before publication")
                self.entries[os.fsdecode(name)] = (mode, oid)
            if not staged:
                for name in git(root, "ls-files", "--others", "--exclude-standard", "-z").split(
                    b"\0"
                ):
                    if name:
                        self.entries[os.fsdecode(name)] = ("worktree", None)
                self.entries = {
                    n: e for n, e in self.entries.items() if os.path.lexists(self.root / n)
                }

    def read(self, name):
        _mode, oid = self.entries[name]
        if self.ref or self.staged:
            size = int(git(self.root, "cat-file", "-s", oid))
            if size > MAX_FILE:
                raise ValueError("File exceeds inspection limit: " + ascii(name))
            return git(self.root, "cat-file", "blob", oid)
        path = self.root / name
        # Do not follow a parent symlink to private or outside content.
        for parent in path.parents:
            if parent == self.root:
                break
            if parent.is_symlink():
                raise ValueError("Symlink parent: " + ascii(name))
        if path.is_symlink():
            return os.fsencode(os.readlink(path))
        if not stat.S_ISREG(path.stat().st_mode) or path.stat().st_size > MAX_FILE:
            raise ValueError("Unsupported file or inspection limit: " + ascii(name))
        return path.read_bytes()

    def link(self, name):
        return self.entries[name][0] == "120000" or (
            not self.ref and not self.staged and (self.root / name).is_symlink()
        )

    def prefix(self, name):
        if not self.ref and not self.staged:
            if self.link(name):
                return b""
            with (self.root / name).open("rb") as stream:
                return stream.read(512)
        if self.entries[name][0] == "160000":
            return b""  # Gitlink, not a file owned by this tree.
        process = subprocess.Popen(
            [
                "git",
                "--no-optional-locks",
                "-C",
                str(self.root),
                "cat-file",
                "blob",
                self.entries[name][1],
            ],
            stdout=subprocess.PIPE,
            stderr=subprocess.DEVNULL,
        )
        try:
            return process.stdout.read(512)
        finally:
            process.stdout.close()
            if process.poll() is None:
                process.terminate()
            process.wait()


def archive_magic(data):
    if (
        data.startswith(
            (
                b"PK\x03\x04",
                b"PK\x05\x06",
                b"\x1f\x8b",
                b"BZh",
                b"\xfd7zXZ\x00",
                b"7z\xbc\xaf\x27\x1c",
                b"Rar!",
            )
        )
        or data[257:262] == b"ustar"
    ):
        return True
    if len(data) >= 512 and any(data[:512]):
        try:
            tarfile.TarInfo.frombuf(data[:512], "utf-8", "surrogateescape")
            return True
        except tarfile.HeaderError:
            pass
    return False


def inspect_archive(name, data, policy, errors, warnings, members=None, depth=0, budget=None):
    """Inspect in memory, never extract links or filesystem members."""
    members = {} if members is None else members
    budget = [0, 0] if budget is None else budget
    if depth >= MAX_DEPTH:
        errors.append("Archive nesting limit: " + ascii(name))
        return members

    def member(member_name, size, reader, link=False):
        budget[0] += 1
        budget[1] += size
        locator = name + "!" + member_name
        if budget[0] > MAX_MEMBERS or size > MAX_FILE or budget[1] > MAX_EXPANDED:
            raise ValueError("Archive inspection budget exceeded")
        if not safe_name(member_name) or private_name(member_name, policy) or link:
            errors.append("Private, unsafe or linked archive member: " + ascii(locator))
            return
        payload = reader(MAX_FILE + 1)
        if len(payload) != size:
            raise ValueError("Archive member size mismatch")
        if sha(payload) in policy.get("private_hashes", []):
            errors.append("Preserved private content in archive: " + ascii(locator))
        if markdown(member_name, payload):
            members[locator] = sha(payload)
        if INTERNAL.search(payload.decode("utf-8", errors="replace")):
            warnings.append("Review internal-report language: " + ascii(locator))
        if archive_name(member_name) or archive_magic(payload):
            inspect_archive(locator, payload, policy, errors, warnings, members, depth + 1, budget)

    try:
        stream = io.BytesIO(data)
        if zipfile.is_zipfile(stream):
            with zipfile.ZipFile(stream) as container:
                for entry in container.infolist():
                    if entry.is_dir():
                        if not safe_name(entry.filename.rstrip("/")) or private_name(
                            entry.filename, policy
                        ):
                            errors.append(
                                "Unsafe archive directory: " + ascii(name + "!" + entry.filename)
                            )
                        continue
                    if entry.flag_bits & 1:
                        raise ValueError("Encrypted archive requires manual inspection")
                    with container.open(entry) as source:
                        member(
                            entry.filename,
                            entry.file_size,
                            source.read,
                            stat.S_ISLNK(entry.external_attr >> 16),
                        )
        else:
            with ExitStack() as stack:
                stream.seek(0)
                try:
                    container = stack.enter_context(tarfile.open(fileobj=stream, mode="r:*"))
                except tarfile.ReadError:
                    opener = (
                        gzip.GzipFile
                        if data[:2] == b"\x1f\x8b"
                        else bz2.BZ2File
                        if data.startswith(b"BZh")
                        else lzma.LZMAFile
                        if data.startswith(b"\xfd7zXZ\x00")
                        else None
                    )
                    if opener:
                        source_stream = (
                            opener(fileobj=io.BytesIO(data))
                            if opener is gzip.GzipFile
                            else opener(io.BytesIO(data))
                        )
                        with source_stream as source:
                            payload = source.read(MAX_FILE + 1)
                        if len(payload) > MAX_FILE:
                            raise ValueError("Compressed data exceeds inspection limit")
                        member(
                            Path(name).name.removesuffix(".gz"),
                            len(payload),
                            io.BytesIO(payload).read,
                        )
                        return members
                    raise ValueError("Unsupported archive requires separate inspection")
                for entry in container:
                    if entry.isdir():
                        if not safe_name(entry.name.rstrip("/")) or private_name(
                            entry.name, policy
                        ):
                            errors.append(
                                "Unsafe archive directory: " + ascii(name + "!" + entry.name)
                            )
                        continue
                    if not entry.isfile():
                        member(entry.name, 0, lambda n: b"", True)
                    else:
                        with container.extractfile(entry) as source:
                            member(entry.name, entry.size, source.read)
    except (
        ValueError,
        OSError,
        EOFError,
        tarfile.TarError,
        zipfile.BadZipFile,
        RuntimeError,
    ) as exc:
        errors.append(str(exc) + ": " + ascii(name))
    return members


def scan(snapshot, policy):
    catalog, errors, warnings = {}, [], []
    names = {}
    for name, (_, oid) in sorted(snapshot.entries.items()):
        if snapshot.entries[name][0] == "160000":
            warnings.append("Submodule needs its own publication policy: " + ascii(name))
            continue
        normalized = canonical(name)
        if not safe_name(name):
            errors.append("Unsafe publication path: " + ascii(name))
            continue
        if normalized in names and names[normalized] != name:
            errors.append("Case/normalization collision: " + ascii(name))
        names[normalized] = name
        if private_name(name, policy):
            errors.append("Private path in publication snapshot: " + ascii(name))
            continue
        if oid in policy.get("private_git_blobs", []):
            errors.append("Renamed private content: " + ascii(name))
        try:
            prefix = snapshot.prefix(name)
        except OSError:
            errors.append("Unreadable publication file: " + ascii(name))
            continue
        disguised_doc = Path(name).suffix.lower() not in (
            ".py",
            ".rs",
            ".js",
            ".ts",
            ".jsx",
            ".tsx",
            ".sh",
            ".c",
            ".h",
            ".cpp",
            ".hpp",
        ) and bool(re.match(rb"\s*#{1,6} [^\n]+\n", prefix))
        inspect = (
            markdown(name)
            or archive_name(name)
            or protected(name, policy)
            or snapshot.link(name)
            or archive_magic(prefix)
            or disguised_doc
            or Path(name).suffix.lower() in ("", ".txt")
            or Path(name).name == "package.json"
        )
        if not inspect:
            continue
        try:
            data = snapshot.read(name)
        except (ValueError, OSError) as exc:
            errors.append(str(exc))
            continue
        if snapshot.link(name):
            target = data.decode("utf-8", errors="replace")
            # Git symlinks cannot be used to import private notes or escape.
            if not safe_name(target) or private_name(target, policy):
                errors.append("Unsafe publication symlink: " + ascii(name))
            if markdown(name) or protected(name, policy):
                errors.append("Documentation/policy must be regular files: " + ascii(name))
            continue
        if sha(data) in policy.get("private_hashes", []):
            errors.append("Private content renamed into publication: " + ascii(name))
        if Path(name).name == "package.json":
            try:
                fragment = publication_fragment(data)
                if fragment:
                    catalog[name + "#publication"] = sha(encoded(fragment))
            except (ValueError, AttributeError):
                errors.append("Invalid publication package metadata: " + ascii(name))
        is_doc = markdown(name, data) or disguised_doc
        if is_doc or protected(name, policy) or archive_name(name) or archive_magic(prefix):
            catalog[name] = sha(data)
        if is_doc and INTERNAL.search(data.decode("utf-8", errors="replace")):
            warnings.append("Review internal-report language: " + ascii(name))
        if archive_name(name) or archive_magic(prefix):
            catalog[name] = sha(data)
            catalog.update(inspect_archive(name, data, policy, errors, warnings))
    return catalog, errors, warnings


def bundle(policy, catalog):
    return sha(encoded({"repository": policy["repository"], "catalog": catalog}))


def state_path(root):
    common = Path(
        os.fsdecode(git(root, "rev-parse", "--path-format=absolute", "--git-common-dir")).strip()
    )
    return common / "documentation-review" / "state.json"


def read_policy(snapshot):
    if POLICY not in snapshot.entries or snapshot.link(POLICY):
        raise ValueError("Publication policy must be staged/tracked as a regular file: " + POLICY)
    policy = json.loads(snapshot.read(POLICY))
    if policy.get("version") != 1 or not re.fullmatch(
        r"golba98/[A-Za-z0-9_.-]+", policy.get("repository", "")
    ):
        raise ValueError("Invalid documentation policy")
    for field in ("private_paths", "protected_paths", "private_hashes", "private_git_blobs"):
        if not isinstance(policy.get(field, []), list):
            raise TypeError("Invalid policy field: " + field)
    return policy


def effective_policy(policy, trusted):
    result = dict(policy)
    for field in ("private_paths", "protected_paths", "private_hashes", "private_git_blobs"):
        result[field] = sorted(set(policy.get(field, [])) | set(trusted.get(field, [])))
    return result


def validate(snapshot, trusted_policy, accepted, approved_digests=()):
    catalog, errors, warnings = scan(snapshot, trusted_policy)
    digest = bundle(trusted_policy, catalog)
    if digest not in approved_digests:
        for name in sorted(set(catalog) | set(accepted)):
            if catalog.get(name) != accepted.get(name):
                errors.append("Owner exact-content review required: " + ascii(name))
    return catalog, digest, errors, warnings


def local_check(root, snapshot, print_result=True):
    path = state_path(root)
    if not path.is_file():
        raise ValueError("Owner review state unavailable; prepare a proposal, do not self-approve")
    state = json.loads(path.read_text())
    proposed = read_policy(snapshot)
    trusted = state["policy"]
    if proposed["repository"] != trusted["repository"]:
        raise ValueError("Repository identity differs from trusted review state")
    # A candidate cannot remove a private prefix to authorize its own content.
    trusted = dict(trusted)
    trusted["private_paths"] = sorted(
        set(trusted.get("private_paths", [])) | set(proposed.get("private_paths", []))
    )
    trusted["protected_paths"] = sorted(
        set(trusted.get("protected_paths", [])) | set(proposed.get("protected_paths", []))
    )
    for field in ("private_hashes", "private_git_blobs"):
        trusted[field] = sorted(set(trusted.get(field, [])) | set(proposed.get(field, [])))
    result = validate(snapshot, trusted, state["catalog"], state.get("approved_bundles", []))
    if print_result:
        show(result)
    return result


def package_source_check(root):
    """Check package inputs without requiring CI to retain local owner receipts.

    Existing local receipts remain authoritative. In a fresh CI checkout, only
    unchanged committed documentation/policy bytes matching its public manifest
    are accepted; PR owner review and release provenance remain separate gates.
    """
    candidate = Snapshot(root)
    if state_path(root).is_file():
        return local_check(root, candidate)
    committed = Snapshot(root, ref="HEAD")
    policy = read_policy(committed)
    accepted, errors, warnings = scan(committed, policy)
    for name, digest in policy.get("public_markdown", {}).items():
        if accepted.get(name) != digest:
            errors.append("Committed public manifest mismatch: " + ascii(name))
    result = validate(candidate, policy, accepted)
    result = (result[0], result[1], errors + result[2], warnings + result[3])
    show(result)
    return result


def show(result):
    _, digest, errors, warnings = result
    for warning in warnings:
        print("WARNING:", warning)
    for error in errors[:100]:
        print("ERROR:", error)
    if len(errors) > 100:
        print("ERROR:", len(errors) - 100, "additional review items")
    print("Documentation digest:", digest)
    print("Publication check " + ("FAILED" if errors else "passed"))


def propose(root, snapshot, output, revision=None):
    policy = read_policy(snapshot)
    previous = (
        json.loads(state_path(root).read_text()) if state_path(root).exists() else {"catalog": {}}
    )
    inspection_policy = effective_policy(policy, previous.get("policy", {}))
    catalog, errors, warnings = scan(snapshot, inspection_policy)
    if errors:
        show((catalog, bundle(policy, catalog), errors, warnings))
        raise ValueError("Resolve structural/private-content violations before owner review")
    # Preparing hashes is not approval: the separate trusted state still gates
    # commits and the owner must review the full candidate bundle.
    if not revision:
        public = {n: h for n, h in catalog.items() if markdown(n) or "!" in n}
        if policy.get("public_markdown") != public:
            policy["public_markdown"] = public
            policy_file = Path(root) / POLICY
            policy_file.write_text(json.dumps(policy, indent=2, sort_keys=True) + "\n")
        snapshot = Snapshot(root)
        catalog, errors, warnings = scan(snapshot, inspection_policy)
        if errors:
            raise ValueError("Candidate changed during proposal preparation")
    changes = []
    for name in sorted(set(previous["catalog"]) | set(catalog)):
        if previous["catalog"].get(name) == catalog.get(name):
            continue
        record = {"path": name, "before": previous["catalog"].get(name), "after": catalog.get(name)}
        if "!" not in name and name in snapshot.entries:
            record["data"] = base64.b64encode(snapshot.read(name)).decode()
        elif name.endswith("#publication"):
            record["data"] = base64.b64encode(
                encoded(publication_fragment(snapshot.read(name.removesuffix("#publication"))))
            ).decode()
        changes.append(record)
    proposal = {
        "repository": policy["repository"],
        "policy": policy,
        "catalog": catalog,
        "digest": bundle(policy, catalog),
        "changes": changes,
        "warnings": warnings,
        "revision": revision,
    }
    target = Path(root, output).resolve()
    if not private_name(str(target.relative_to(Path(root).resolve())), policy):
        raise ValueError("Proposal output must be in the configured private directory")
    target.parent.mkdir(parents=True, exist_ok=True)
    target.write_bytes(encoded(proposal))
    target.chmod(0o600)
    print("Private proposal:", target)
    print("Owner review digest:", proposal["digest"])


def review(root, proposal_path):
    if os.environ.get("AGENT_BOUNDARY") or not sys.stdin.isatty() or not sys.stdout.isatty():
        raise ValueError("Owner review requires a separate interactive host terminal")
    raw = Path(proposal_path).read_bytes()
    proposal = json.loads(raw)
    revision = proposal.get("revision")
    if revision and not re.fullmatch(r"[0-9a-f]{40,64}", revision):
        raise ValueError("Proposal revision must be an immutable commit hash")
    snapshot = Snapshot(root, ref=revision)
    current_policy = read_policy(snapshot)
    state_file = state_path(root)
    state = json.loads(state_file.read_text()) if state_file.exists() else {"catalog": {}}
    inspection_policy = effective_policy(current_policy, state.get("policy", {}))
    catalog, errors, warnings = scan(snapshot, inspection_policy)
    digest = bundle(current_policy, catalog)
    if (
        errors
        or catalog != proposal["catalog"]
        or digest != proposal["digest"]
        or current_policy != proposal["policy"]
    ):
        raise ValueError("Stale or structurally unsafe proposal; prepare it again")
    if state.get("policy", current_policy)["repository"] != proposal["repository"]:
        raise ValueError("Wrong repository")
    for item in proposal["changes"]:
        print("Review", ascii(item["path"]), item["before"], "->", item["after"])
        if item.get("data") and not archive_name(item["path"]):
            new = base64.b64decode(item["data"]).decode("utf-8", errors="replace")
            try:
                old = git(root, "show", "HEAD:" + item["path"]).decode("utf-8", errors="replace")
            except ValueError:
                old = ""
            for line in difflib.unified_diff(old.splitlines(), new.splitlines(), lineterm=""):
                print(ascii(line))
    for warning in warnings:
        print("WARNING:", warning)
    print(
        "Archive members are approved by digest; inspect originals and public evidence before proceeding."
    )
    if (
        input("After reviewing all listed bytes, type APPROVE " + digest + "\n")
        != "APPROVE " + digest
    ):
        raise ValueError("Not approved")
    final = scan(Snapshot(root, ref=revision), inspection_policy)
    if Path(proposal_path).read_bytes() != raw or final[0] != catalog or final[1]:
        raise ValueError("Files changed during review")
    state_file.parent.mkdir(mode=0o700, exist_ok=True)
    state["approved_bundles"] = sorted(set(state.get("approved_bundles", [])) | {digest})
    if not revision:
        state.update(policy=current_policy, catalog=catalog)
    state_file.write_bytes(encoded(state))
    receipt = state_file.parent / (digest + ".json")
    receipt.write_bytes(
        encoded(
            {
                "repository": proposal["repository"],
                "digest": digest,
                "catalog": catalog,
                "review": "interactive-owner-host-review",
            }
        )
    )
    receipt.chmod(0o600)
    print("Owner receipt recorded locally. Nothing staged, committed or published.")


def outgoing(root, rows, remote):
    # Advertised refs establish the boundary, including new branches and tags.
    refs = git(root, "ls-remote", remote).splitlines()
    known = []
    for row in refs:
        oid = row.split()[0].decode()
        try:
            git(root, "cat-file", "-e", oid + "^{commit}")
            known.append(oid)
        except ValueError:
            continue
    if refs and not known:
        raise ValueError("Remote history boundary unavailable locally; fetch it before pushing")
    commits = set()
    for row in rows:
        fields = row.split()
        if len(fields) != 4:
            raise ValueError("Malformed pre-push input")
        _, local, _, old = fields
        if set(local) == {"0"}:
            continue
        excluded = list(known)
        if set(old) != {"0"}:
            git(root, "cat-file", "-e", old + "^{commit}")
            excluded.append(old)
        args = ["rev-list", local] + (["--not", *excluded] if excluded else [])
        commits.update(git(root, *args).decode().splitlines())
    return sorted(commits)


def api(repository, endpoint, token, method="GET", payload=None):
    request = urllib.request.Request(
        "https://api.github.com/repos/" + repository + endpoint,
        data=encoded(payload) if payload is not None else None,
        method=method,
        headers={
            "Accept": "application/vnd.github+json",
            "Authorization": "Bearer " + token,
            "X-GitHub-Api-Version": "2022-11-28",
            "User-Agent": "documentation-publication-check",
        },
    )
    with urllib.request.urlopen(request, timeout=30) as response:
        return json.load(response)


def owner_comment(comments, owner_id, head, digest):
    expected = "/docs-approve " + head + " " + digest
    return any(
        c.get("user", {}).get("id") == owner_id
        and c.get("body", "").strip() == expected
        and c.get("user", {}).get("type") == "User"
        for c in comments
    )


def install_hooks(root):
    """Install local guards without replacing any existing hook behavior."""
    state_file = state_path(root)
    if not state_file.exists():
        snapshot = Snapshot(root, ref="HEAD")
        policy = read_policy(snapshot)
        head = git(root, "rev-parse", "HEAD").decode().strip()
        # A fresh clone may freeze already-public HEAD bytes. Unpublished
        # candidates instead require the interactive owner proposal workflow.
        request = urllib.request.Request(
            "https://api.github.com/repos/" + policy["repository"],
            headers={"User-Agent": "documentation-publication-check"},
        )
        with urllib.request.urlopen(request, timeout=30) as response:
            metadata = json.load(response)
        if (
            metadata.get("private")
            or metadata.get("visibility") != "public"
            or metadata["full_name"].lower() != policy["repository"].lower()
        ):
            raise ValueError("Public repository identity is unverified")
        request = urllib.request.Request(
            "https://api.github.com/repos/" + policy["repository"] + "/commits/" + head,
            headers={"User-Agent": "documentation-publication-check"},
        )
        with urllib.request.urlopen(request, timeout=30) as response:
            if json.load(response).get("sha") != head:
                raise ValueError("HEAD is not verified as already public")
        catalog, errors, _ = scan(snapshot, policy)
        if errors:
            raise ValueError(
                "Public HEAD contains publication violations; remediate before installation"
            )
        state_file.parent.mkdir(parents=True, mode=0o700, exist_ok=True)
        state_file.write_bytes(
            encoded(
                {
                    "policy": policy,
                    "catalog": catalog,
                    "approved_bundles": [],
                    "baseline_kind": "verified already-public HEAD",
                    "baseline_head": head,
                }
            )
        )
    state = json.loads(state_file.read_text())
    folder = state_file.parent
    source = Path(__file__).read_bytes()
    target = folder / "check_documentation.py"
    if (
        target.exists()
        and target.read_bytes() != source
        and state.get("approved_bundles")
        and state["catalog"].get(SCRIPT) != sha(source)
    ):
        raise ValueError("Owner must review changed checker bytes before updating trusted hooks")
    target.write_bytes(source)
    driver = """#!/usr/bin/env python3
from pathlib import Path
import subprocess
import sys
here = Path(__file__).resolve().parent
kind = sys.argv[1]
data = sys.stdin.buffer.read() if kind == 'pre-push' else None
root = subprocess.check_output(['git', 'rev-parse', '--show-toplevel']).decode().strip()
args = ['--pre-push', *sys.argv[2:]] if kind == 'pre-push' else ['--staged']
result = subprocess.run([sys.executable, str(here / 'check_documentation.py'), '--root', root, *args], input=data).returncode
legacy = here / 'previous-hooks' / kind
if legacy.exists():
    result = subprocess.run([str(legacy), *sys.argv[2:]], input=data).returncode or result
raise SystemExit(result)
"""
    (folder / "hook.py").write_text(driver)
    hooks = Path(
        git(root, "rev-parse", "--path-format=absolute", "--git-path", "hooks").decode().strip()
    )
    common = folder.parent
    if not hooks.is_relative_to(common) and not hooks.is_relative_to(Path(root)):
        raise ValueError("Shared external hooks directory preserved; integrate this guard manually")
    hooks.mkdir(parents=True, exist_ok=True)
    marker = "# documentation-publication-hook-v1"
    bodies = {}
    for kind in ("pre-commit", "pre-push"):
        path = hooks / kind
        if path.is_symlink():
            raise ValueError("Existing hook symlink preserved; integrate manually")
        if path.exists() and marker not in path.read_text(errors="replace"):
            previous = folder / "previous-hooks" / kind
            if previous.exists() and previous.read_bytes() != path.read_bytes():
                raise ValueError("Conflicting legacy hook preserved")
            previous.parent.mkdir(exist_ok=True)
            previous.write_bytes(path.read_bytes())
            previous.chmod(path.stat().st_mode & 0o777)
        bodies[path] = (
            "#!/bin/sh\n"
            + marker
            + '\nexec python3 "$(git rev-parse --git-common-dir)/documentation-review/hook.py" '
            + kind
            + ' "$@"\n'
        )
    for path, body in bodies.items():
        path.write_text(body)
        path.chmod(0o755)
    print("Local documentation hooks installed; existing behavior preserved.")


def ci(root, number):
    repository = os.environ["GITHUB_REPOSITORY"]
    if not re.fullmatch(r"golba98/[A-Za-z0-9_.-]+", repository):
        raise ValueError("Unexpected repository owner")
    token = os.environ["GITHUB_TOKEN"]
    metadata = api(repository, "", token)
    pr = api(repository, "/pulls/" + str(number), token)
    head, base = pr["head"]["sha"], pr["base"]["sha"]
    api(
        repository,
        "/statuses/" + head,
        token,
        "POST",
        {
            "state": "pending",
            "context": "documentation/owner-review",
            "description": "Inspecting exact documentation and publication policy",
        },
    )
    # No checkout and no head-controlled subprocesses, hooks or filters.
    git(
        root,
        "-c",
        "core.hooksPath=/dev/null",
        "fetch",
        "--no-tags",
        "--no-recurse-submodules",
        "https://github.com/" + repository + ".git",
        "refs/pull/" + str(number) + "/head",
    )
    if git(root, "rev-parse", "FETCH_HEAD").decode().strip() != head:
        raise ValueError("PR changed during inspection; rerun")
    git(
        root,
        "-c",
        "core.hooksPath=/dev/null",
        "fetch",
        "--no-tags",
        "--no-recurse-submodules",
        "https://github.com/" + repository + ".git",
        base,
    )
    trusted_policy = read_policy(Snapshot(root, ref="HEAD"))
    if trusted_policy["repository"].lower() != repository.lower():
        raise ValueError("Policy identity mismatch")
    accepted, base_errors, _ = scan(Snapshot(root, ref="HEAD"), trusted_policy)
    if base_errors:
        raise ValueError("Trusted base needs documentation policy remediation")
    candidate = Snapshot(root, ref=head)
    catalog, digest, errors, warnings = validate(candidate, trusted_policy, accepted)
    comments = []
    for page in range(1, 101):
        chunk = api(
            repository, "/issues/" + str(number) + "/comments?per_page=100&page=" + str(page), token
        )
        comments.extend(chunk)
        if len(chunk) < 100:
            break
    else:
        raise ValueError("Comment pagination limit; review manually")
    if owner_comment(comments, metadata["owner"]["id"], head, digest):
        catalog, digest, errors, warnings = validate(candidate, trusted_policy, accepted, [digest])
    for revision in git(root, "rev-list", head, "^" + base).decode().splitlines():
        if revision == head:
            continue
        result = validate(Snapshot(root, ref=revision), trusted_policy, accepted)
        if owner_comment(comments, metadata["owner"]["id"], revision, result[1]):
            result = validate(Snapshot(root, ref=revision), trusted_policy, accepted, [result[1]])
        if result[2]:
            errors.append("Unreviewed or unsafe intermediate PR commit: " + revision)
            print("Intermediate approval command: /docs-approve", revision, result[1])
    latest = api(repository, "/pulls/" + str(number), token)
    if latest["head"]["sha"] != head or latest["base"]["sha"] != base:
        raise ValueError("PR base/head changed during inspection; rerun")
    show((catalog, digest, errors, warnings))
    print("Owner approval command: /docs-approve", head, digest)
    api(
        repository,
        "/statuses/" + head,
        token,
        "POST",
        {
            "state": "failure" if errors else "success",
            "context": "documentation/owner-review",
            "description": "Exact content requires owner review"
            if errors
            else "Documentation publication validated",
        },
    )
    return bool(errors)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--root", default=".")
    parser.add_argument(
        "--proposal-revision",
        help="With --propose, review a committed outgoing snapshot without switching branches",
    )
    mode = parser.add_mutually_exclusive_group()
    mode.add_argument("--staged", action="store_true")
    mode.add_argument("--revision")
    mode.add_argument("--pre-push", nargs=2, metavar=("REMOTE_NAME", "REMOTE_URL"))
    mode.add_argument("--artifact")
    mode.add_argument("--propose", metavar="PRIVATE_JSON")
    mode.add_argument("--review", metavar="PRIVATE_JSON")
    mode.add_argument("--ci", type=int, metavar="PR_NUMBER")
    mode.add_argument("--install-hooks", action="store_true")
    mode.add_argument("--package-source", action="store_true")
    args = parser.parse_args()
    root = Path(os.fsdecode(git(args.root, "rev-parse", "--show-toplevel")).strip())
    if args.install_hooks:
        install_hooks(root)
        return 0
    if args.package_source:
        return bool(package_source_check(root)[2])
    if args.review:
        review(root, args.review)
        return 0
    if args.ci:
        return ci(root, args.ci)
    if args.pre_push:
        rows = sys.stdin.read().splitlines()
        failed = False
        for ref in outgoing(root, rows, args.pre_push[1]):
            failed = bool(local_check(root, Snapshot(root, ref=ref))[2]) or failed
        return int(failed)
    snapshot = Snapshot(root, ref=args.revision, staged=args.staged)
    if args.propose:
        revision = (
            git(root, "rev-parse", args.proposal_revision + "^{commit}").decode().strip()
            if args.proposal_revision
            else None
        )
        propose(
            root, Snapshot(root, ref=revision) if revision else snapshot, args.propose, revision
        )
        return 0
    if args.artifact:
        state = json.loads(state_path(root).read_text())
        path = Path(args.artifact)
        errors, warnings = [], []
        if path.stat().st_size > MAX_FILE:
            raise ValueError("Artifact exceeds inspection limit")
        members = inspect_archive(path.name, path.read_bytes(), state["policy"], errors, warnings)
        approved = set(state["catalog"].values())
        for name, digest in members.items():
            if digest not in approved:
                errors.append("Unreviewed Markdown in package: " + ascii(name))
        show((members, sha(path.read_bytes()), errors, warnings))
        return bool(errors)
    return bool(local_check(root, snapshot)[2])


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except (ValueError, TypeError, OSError, KeyError, json.JSONDecodeError) as error:
        print("Publication guard refused:", str(error), file=sys.stderr)
        raise SystemExit(1)
