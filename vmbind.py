#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""i915-vmbind maintenance and build tool.

Keeps complete i915 source snapshots of pinned kernel releases (as git tags),
applies the VM_BIND patch stacks on top of them, and builds i915.ko/kvmgt.ko
out of tree against an installed kernel headers tree.

Run `vmbind.py <command> --help` for details. Standard library only.
"""
from __future__ import annotations

import argparse
import fnmatch
import hashlib
import json
import os
import platform
import re
import shutil
import subprocess
import sys
import tarfile
import tempfile
import time
import urllib.error
import urllib.request
import zipfile
from pathlib import Path

if sys.version_info < (3, 11):
    sys.exit("vmbind.py needs Python 3.11 or newer (for tomllib)")
import tomllib

ROOT = Path(__file__).resolve().parent
MANIFEST = ROOT / "vmbind.toml"
STATE = ROOT / ".vmbind"
CACHE = Path(os.environ.get("VMBIND_CACHE", Path.home() / ".cache" / "i915-vmbind"))
MODULES = ("i915", "kvmgt")
VERBOSE = False


# --------------------------------------------------------------------------- utils

class Fail(Exception):
    pass


def log(msg: str) -> None:
    print(f"==> {msg}", flush=True)


def warn(msg: str) -> None:
    print(f"warning: {msg}", file=sys.stderr, flush=True)


def run(cmd, cwd=None, env=None, check=True, capture=False, input=None):
    cmd = [str(c) for c in cmd]
    if VERBOSE:
        print("+ " + " ".join(cmd), file=sys.stderr)
    full_env = None
    if env:
        full_env = os.environ.copy()
        full_env.update(env)
    r = subprocess.run(cmd, cwd=cwd, env=full_env, text=True, input=input,
                       stdout=subprocess.PIPE if capture else None,
                       stderr=subprocess.PIPE if capture else None)
    if check and r.returncode != 0:
        detail = f"\n{r.stderr.strip()}" if capture and r.stderr else ""
        raise Fail(f"command failed ({r.returncode}): {' '.join(cmd)}{detail}")
    return r


def out(cmd, cwd=None, env=None) -> str:
    return run(cmd, cwd=cwd, env=env, capture=True).stdout.strip()


def git(*args, cwd=ROOT, env=None, check=True, capture=True):
    return run(["git", *args], cwd=cwd, env=env, check=check, capture=capture)


def gitout(*args, cwd=ROOT, env=None) -> str:
    return git(*args, cwd=cwd, env=env).stdout.strip()


def sha256(path: Path) -> str:
    h = hashlib.sha256()
    with open(path, "rb") as f:
        for chunk in iter(lambda: f.read(1 << 20), b""):
            h.update(chunk)
    return h.hexdigest()


def download(url: str, dest_dir: Path = CACHE / "downloads") -> Path:
    dest_dir.mkdir(parents=True, exist_ok=True)
    dest = dest_dir / url.rstrip("/").split("/")[-1]
    if dest.exists() and dest.stat().st_size > 0:
        return dest
    log(f"downloading {url}")
    tmp = dest.with_suffix(dest.suffix + ".part")
    req = urllib.request.Request(url, headers={"User-Agent": "i915-vmbind"})
    with urllib.request.urlopen(req) as r, open(tmp, "wb") as f:
        shutil.copyfileobj(r, f)
    tmp.rename(dest)
    return dest


def extract(archive: Path, dest: Path) -> None:
    """Extract tar.*, zip, .pkg.tar.zst, .deb, .rpm into dest."""
    dest.mkdir(parents=True, exist_ok=True)
    name = archive.name
    if name.endswith(".zip"):
        with zipfile.ZipFile(archive) as z:
            z.extractall(dest)
            for info in z.infolist():  # zipfile drops symlinks/modes
                mode = info.external_attr >> 16
                p = dest / info.filename
                if (mode & 0o170000) == 0o120000:
                    target = p.read_text()
                    p.unlink()
                    p.symlink_to(target)
                elif mode & 0o111 and p.is_file():
                    p.chmod(p.stat().st_mode | 0o111)
    elif name.endswith(".deb"):
        with tempfile.TemporaryDirectory() as t:
            run(["ar", "x", archive.resolve()], cwd=t)
            data = next(Path(t).glob("data.tar*"))
            run(["tar", "-xf", data, "-C", dest])
    elif name.endswith(".rpm"):
        run(["sh", "-c", f"rpm2cpio '{archive}' | (cd '{dest}' && cpio -idm --quiet)"])
    elif name.endswith((".zst", ".tzst")):
        run(["tar", "--zstd", "-xf", archive, "-C", dest])
    else:
        run(["tar", "-xf", archive, "-C", dest])


def manifest() -> dict:
    with open(MANIFEST, "rb") as f:
        m = tomllib.load(f)
    m.setdefault("targets", [])
    return m


def target_by_id(m: dict, tid: str) -> dict:
    for t in m["targets"]:
        if t["id"] == tid:
            return t
    raise Fail(f"unknown target '{tid}'. Known: {', '.join(t['id'] for t in m['targets'])}")


def select_target(m: dict, kver: str) -> dict | None:
    ordered = sorted(m["targets"], key=lambda t: t.get("flavour", "vanilla") == "vanilla")
    for t in ordered:
        if re.match(t["kver"], kver):
            return t
    return None


def stack_patches(m: dict, line: str, base: Path = ROOT) -> list[tuple[str, Path]]:
    res = []
    for stack in m["stacks"]["order"]:
        d = base / "patches" / line / stack
        if d.is_dir():
            res += [(stack, p) for p in sorted(d.glob("*.patch"))]
    return res


def is_local_stack(m: dict, stack: str) -> bool:
    return stack in m["stacks"].get("local", [])


def patch_message(patch: Path) -> str:
    """Commit message of a local (header-less) patch."""
    lines = patch.read_text().splitlines()
    msg = []
    for ln in lines:
        if ln == "---" or ln.startswith("diff --git "):
            break
        msg.append(ln)
    if msg and msg[0].startswith("Subject: "):
        msg[0] = msg[0][len("Subject: "):]
    text = "\n".join(msg).strip()
    if not text:
        raise Fail(f"{patch}: no commit message")
    return text + "\n"


def in_git_repo() -> bool:
    return (ROOT / ".git").exists() and shutil.which("git") is not None


def ident_env(args) -> dict:
    env = {}
    if getattr(args, "name", None):
        env.update(GIT_AUTHOR_NAME=args.name, GIT_COMMITTER_NAME=args.name)
    if getattr(args, "email", None):
        env.update(GIT_AUTHOR_EMAIL=args.email, GIT_COMMITTER_EMAIL=args.email)
    return env


# --------------------------------------------------------------- patch application

def apply_one(repo: Path, m: dict, stack: str, patch: Path, env: dict, three_way=True) -> None:
    """Apply one patch as one commit. Upstream-authored stacks keep their author
    via `git am`; local stacks are committed as the running maintainer."""
    if is_local_stack(m, stack):
        git("apply", "--index", "--directory=src", *(["-3"] if three_way else []),
            patch.resolve(), cwd=repo, env=env)
        msg = patch_message(patch)
        run(["git", "commit", "-q", "--no-verify", "-F", "-"], cwd=repo, env=env,
            input=msg, capture=True)
    else:
        git("am", "-q", "--directory=src", "--keep-cr", *(["-3"] if three_way else []),
            patch.resolve(), cwd=repo, env=env)


def materialize(m: dict, t: dict, dest: Path, line: str | None = None) -> Path:
    """Create dest as a scratch git repo holding upstream/<id> with all stacks
    applied. Returns dest."""
    if not in_git_repo():
        raise Fail("not a git checkout; run `vmbind.py init` first or use a release branch")
    tag = f"upstream/{t['id']}"
    if git("rev-parse", "-q", "--verify", f"refs/tags/{tag}", check=False).returncode:
        raise Fail(f"missing tag {tag}; import it first")
    if dest.exists():
        shutil.rmtree(dest)
    dest.mkdir(parents=True)
    run(["sh", "-c", f"git -C '{ROOT}' archive '{tag}' | tar -x -C '{dest}'"])
    env = {"GIT_AUTHOR_NAME": "vmbind", "GIT_AUTHOR_EMAIL": "vmbind@localhost",
           "GIT_COMMITTER_NAME": "vmbind", "GIT_COMMITTER_EMAIL": "vmbind@localhost",
           "GIT_CEILING_DIRECTORIES": str(dest.parent)}
    git("init", "-q", cwd=dest, env=env)
    git("add", "-A", cwd=dest, env=env)
    git("commit", "-q", "--no-verify", "-m", f"upstream {t['id']}", cwd=dest, env=env)
    for stack, p in stack_patches(m, line or t["line"]):
        try:
            # authors are irrelevant in scratch trees
            if is_local_stack(m, stack):
                apply_one(dest, m, stack, p, env)
            else:
                git("am", "-q", "--directory=src", "-3", p.resolve(), cwd=dest, env=env)
        except Fail as e:
            raise Fail(f"{stack}/{p.name} does not apply on {tag}:\n{e}\n"
                       f"Use `vmbind.py start {t['id']}` to rebase interactively.")
    return dest


# ---------------------------------------------------------------------------- import

def find_kernel_root(d: Path) -> Path:
    for cand in [d, *sorted(p for p in d.iterdir() if p.is_dir())]:
        if (cand / "Makefile").is_file() and (cand / "drivers/gpu/drm/i915").is_dir():
            return cand
    raise Fail(f"no kernel tree with drivers/gpu/drm/i915 found in {d}")


def kernel_version(root: Path) -> str:
    vals = {}
    for ln in (root / "Makefile").read_text().splitlines()[:12]:
        mm = re.match(r"^(VERSION|PATCHLEVEL|SUBLEVEL|EXTRAVERSION)\s*=\s*(.*)$", ln)
        if mm:
            vals[mm.group(1)] = mm.group(2).strip()
    v = f"{vals['VERSION']}.{vals['PATCHLEVEL']}"
    if vals.get("SUBLEVEL", "0") != "0":
        v += f".{vals['SUBLEVEL']}"
    return v + vals.get("EXTRAVERSION", "")


def copy_paths(src_root: Path, dest: Path, patterns: list[str]) -> int:
    n = 0
    for pat in patterns:
        s = src_root / pat
        if s.is_dir():
            shutil.copytree(s, dest / pat, symlinks=True, dirs_exist_ok=True)
            n += sum(1 for _ in (dest / pat).rglob("*"))
        elif s.exists():
            (dest / pat).parent.mkdir(parents=True, exist_ok=True)
            shutil.copy2(s, dest / pat, follow_symlinks=False)
            n += 1
        else:
            warn(f"{pat} not present in source tree")
    return n


def build_import_tree(m: dict, kroot: Path, dest: Path, meta: dict) -> None:
    imp = m["import"]
    copy_paths(kroot, dest / "src", imp["src"])
    copy_paths(kroot, dest / "reference", imp["reference"])
    for p in (dest / "reference").rglob("*"):
        if p.is_symlink():
            p.unlink()
    sums = []
    for p in sorted((dest / "reference").rglob("*")):
        if p.is_file():
            sums.append(f"{sha256(p)}  {p.relative_to(dest / 'reference').as_posix()}")
    (dest / "reference.sha256").write_text("\n".join(sums) + "\n")
    (dest / "UPSTREAM.json").write_text(json.dumps(meta, indent=2) + "\n")
    (dest / "README.md").write_text(
        f"# Upstream import: {meta['id']}\n\n"
        f"Unmodified files from Linux {meta['kernelversion']} ({meta['flavour']}).\n\n"
        "- `src/`: the i915 driver and the uapi headers it ships; the VM_BIND stacks apply here.\n"
        "- `reference/`: kernel headers the driver compiles against, kept for review and\n"
        "  for the build-time header guard (`reference.sha256`). Never used as include path.\n")


def resolve_kernel_source(spec: str, ref: str | None, work: Path) -> Path:
    p = Path(spec)
    if p.is_dir():
        return find_kernel_root(p)
    if p.is_file():
        extract(p, work)
        return find_kernel_root(work)
    if re.match(r"^\d+\.\d+(\.\d+)?(-rc\d+)?$", spec):
        major = spec.split(".")[0]
        url = (f"https://git.kernel.org/torvalds/t/linux-{spec}.tar.gz" if "-rc" in spec else
               f"https://cdn.kernel.org/pub/linux/kernel/v{major}.x/linux-{spec}.tar.xz")
        extract(download(url), work)
        return find_kernel_root(work)
    if re.match(r"^(https?|git|ssh)://|^git@", spec) or spec.endswith(".git"):
        if not ref:
            raise Fail("--ref is required when importing from a git URL")
        d = work / "linux"
        paths = m_cache["import"]["src"] + m_cache["import"]["reference"] + ["Makefile"]
        run(["git", "clone", "-q", "--depth=1", "--filter=blob:none", "--no-checkout",
             "--branch", ref, spec, d])
        run(["git", "-C", d, "sparse-checkout", "set", "--no-cone", *paths])
        run(["git", "-C", d, "checkout", "-q"])
        return d
    raise Fail(f"cannot interpret kernel source '{spec}'")


m_cache: dict = {}


def import_tree_to_tag(tree_dir: Path, tid: str, message: str, env: dict) -> str:
    idx = STATE / "import.index"
    idx.parent.mkdir(parents=True, exist_ok=True)
    if idx.exists():
        idx.unlink()
    e = dict(env, GIT_INDEX_FILE=str(idx), GIT_WORK_TREE=str(tree_dir))
    git("add", "-A", "-f", ".", cwd=tree_dir, env=dict(e, GIT_DIR=str(ROOT / ".git")))
    tree = gitout("write-tree", env=e)
    idx.unlink()
    commit = run(["git", "commit-tree", tree, "-F", "-"], cwd=ROOT, env=e,
                 input=message, capture=True).stdout.strip()
    git("tag", "-a", "-m", message.splitlines()[0], f"upstream/{tid}", commit, env=env)
    return commit


def append_target(t: dict) -> None:
    lines = ["", "[[targets]]"]
    for k in ("id", "line", "flavour", "kernelversion", "source", "kver"):
        if k in t:
            v = t[k]
            lines.append(f"{k} = '{v}'" if k == "kver" else f"{k} = {json.dumps(v)}")
    lines.append(f"headers = {json.dumps(t.get('headers', []))}")
    with open(MANIFEST, "a") as f:
        f.write("\n".join(lines) + "\n")


def cmd_import(args) -> None:
    m = manifest()
    m_cache.update(m)
    env = ident_env(args)
    with tempfile.TemporaryDirectory(prefix="vmbind-import-") as tmp:
        tmp = Path(tmp)
        kroot = resolve_kernel_source(args.kernel, args.ref, tmp / "k")
        kv = kernel_version(kroot)
        flavour = args.flavour or "vanilla"
        tid = args.id or (kv if flavour == "vanilla" else f"{kv}-{flavour}")
        line = args.line or ".".join(kv.split("-")[0].split(".")[:2])
        meta = {"id": tid, "kernelversion": kv, "flavour": flavour,
                "source": args.source_label or args.kernel + (f"@{args.ref}" if args.ref else ""),
                "imported": time.strftime("%Y-%m-%d")}
        tree = tmp / "tree"
        build_import_tree(m, kroot, tree, meta)
        if args.archive:
            Path(args.archive).parent.mkdir(parents=True, exist_ok=True)
            run(["tar", "--sort=name", "--owner=0", "--group=0", "--numeric-owner",
                 "--mtime=@0", "-cJf", Path(args.archive).resolve(), "-C", tree, "."],
                env={"XZ_OPT": "-9 -T0"})
            log(f"wrote {args.archive}")
            return
        if git("rev-parse", "-q", "--verify", f"refs/tags/upstream/{tid}", check=False).returncode == 0:
            raise Fail(f"upstream/{tid} already exists; imports are immutable, pick a new --id")
        import_tree_to_tag(tree, tid, f"Import {tid} i915 sources\n\nLinux {kv} ({flavour})\nSource: {meta['source']}\n", env)
        log(f"tagged upstream/{tid} (line {line})")
    if not any(t["id"] == tid for t in m["targets"]):
        if flavour == "vanilla":
            kver = "^" + re.escape(kv) + r"([-.+].*)?$"
        else:
            kver = "^" + re.escape(kv) + r"-.*" + re.escape(flavour) + r".*$"
        if args.kver:
            kver = args.kver
        if not (ROOT / "patches" / line).is_dir():
            warn(f"no patches/{line}; copy the closest line first, e.g. "
                 f"`cp -r patches/<old> patches/{line}`, then `vmbind.py start {tid}`")
        append_target({"id": tid, "line": line, "flavour": flavour, "kernelversion": kv,
                       "source": meta["source"], "kver": kver, "headers": []})
        log(f"added target {tid} to vmbind.toml; review its kver regex and headers")


# ---------------------------------------------------------------------- rebase flow

def state_file(tid: str) -> Path:
    return STATE / "state" / f"{tid}.json"


def worktree(tid: str) -> Path:
    return STATE / "wt" / tid


def drive(m: dict, tid: str, env: dict) -> None:
    st = json.loads(state_file(tid).read_text())
    wt = Path(st["worktree"])
    for item in st["patches"]:
        if item.get("commit"):
            continue
        stack, p = item["stack"], ROOT / item["file"]
        log(f"applying {stack}/{p.name}")
        try:
            apply_one(wt, m, stack, p, env)
        except Fail as e:
            print(e, file=sys.stderr)
            print(f"\nConflict in {stack}/{p.name}.\n"
                  f"  cd {wt}\n  # fix the files, `git add` them, then:\n"
                  f"  {sys.argv[0]} continue {tid}\n"
                  f"To drop this patch instead: {sys.argv[0]} continue {tid} --skip",
                  file=sys.stderr)
            raise SystemExit(1)
        item["commit"] = gitout("rev-parse", "HEAD", cwd=wt)
        state_file(tid).write_text(json.dumps(st, indent=2))
    log(f"all stacks applied on {st['branch']}. Next: build/test, then "
        f"`vmbind.py export {tid}` to write patches/{st['line']}/")


def cmd_start(args) -> None:
    m = manifest()
    t = target_by_id(m, args.target)
    line = args.line or t["line"]
    wt = worktree(t["id"])
    if wt.exists():
        raise Fail(f"{wt} exists; `vmbind.py abort {t['id']}` first")
    branch = f"work/{t['id']}"
    git("worktree", "add", "-q", "-B", branch, wt, f"upstream/{t['id']}^{{commit}}")
    git("config", "rerere.enabled", "true", cwd=wt)
    st = {"target": t["id"], "line": line, "worktree": str(wt), "branch": branch,
          "base": gitout("rev-parse", "HEAD", cwd=wt),
          "patches": [{"stack": s, "file": p.relative_to(ROOT).as_posix(), "commit": None}
                      for s, p in stack_patches(m, line)]}
    state_file(t["id"]).parent.mkdir(parents=True, exist_ok=True)
    state_file(t["id"]).write_text(json.dumps(st, indent=2))
    drive(m, t["id"], ident_env(args))


def cmd_continue(args) -> None:
    m = manifest()
    st = json.loads(state_file(args.target).read_text())
    wt = Path(st["worktree"])
    env = ident_env(args)
    item = next(i for i in st["patches"] if not i.get("commit"))
    am_dir = Path(gitout("rev-parse", "--git-path", "rebase-apply", cwd=wt))
    am_dir = am_dir if am_dir.is_absolute() else wt / am_dir
    if args.skip:
        if am_dir.exists():
            git("am", "--skip", cwd=wt, env=env)
        else:
            git("reset", "-q", "--hard", cwd=wt)
        item["commit"] = "skipped"
    else:
        if gitout("diff", "--name-only", "--diff-filter=U", cwd=wt):
            raise Fail("unresolved conflicts remain; `git add` the fixed files")
        if am_dir.exists():
            git("am", "--continue", cwd=wt, env=env, capture=False)
        else:
            run(["git", "commit", "-q", "--no-verify", "-F", "-"], cwd=wt, env=env,
                input=patch_message(ROOT / item["file"]), capture=True)
        item["commit"] = gitout("rev-parse", "HEAD", cwd=wt)
    state_file(args.target).write_text(json.dumps(st, indent=2))
    drive(m, args.target, env)


def cmd_abort(args) -> None:
    wt = worktree(args.target)
    if wt.exists():
        git("worktree", "remove", "--force", wt)
    state_file(args.target).unlink(missing_ok=True)
    log("aborted")


def write_patch(m: dict, repo: Path, commit: str, stack: str, dest: Path, num: int) -> None:
    text = gitout("format-patch", "-1", "--stdout", "--relative=src", "--no-signature",
                  "--zero-commit", "--stat=80", commit, cwd=repo)
    subj = gitout("log", "-1", "--format=%s", commit, cwd=repo)
    slug = re.sub(r"[^A-Za-z0-9]+", "-", subj).strip("-")[:52]
    if is_local_stack(m, stack):
        hdr, _, rest = text.partition("\n\n")
        subject = re.search(r"^Subject: (?:\[PATCH\] )?(.*(?:\n .*)*)", hdr, re.M).group(1)
        text = "Subject: " + subject.replace("\n ", " ") + "\n\n" + rest
    (dest / f"{num:04d}-{slug}.patch").write_text(text + "\n")


def cmd_export(args) -> None:
    m = manifest()
    st = json.loads(state_file(args.target).read_text())
    wt, line = Path(st["worktree"]), args.line or st["line"]
    if any(not i.get("commit") for i in st["patches"]):
        raise Fail("rebase not finished")
    known = {i["commit"]: i["stack"] for i in st["patches"] if i["commit"] != "skipped"}
    commits = gitout("rev-list", "--reverse", f"{st['base']}..HEAD", cwd=wt).split()
    plan: dict[str, list[str]] = {s: [] for s in m["stacks"]["order"]}
    extra = args.extra_stack or m["stacks"]["order"][-1]
    plan.setdefault(extra, [])
    for c in commits:
        plan[known.get(c, extra)].append(c)
    for c in commits:
        if c not in known:
            warn(f"new commit {c[:12]} goes to stack {extra}")
    dest_root = ROOT / "patches" / line
    for stack, cs in plan.items():
        d = dest_root / stack
        if d.exists():
            shutil.rmtree(d)
        if not cs:
            continue
        d.mkdir(parents=True)
        for n, c in enumerate(cs, 1):
            write_patch(m, wt, c, stack, d, n)
    log(f"wrote patches/{line}/ from {len(commits)} commits; review with `git diff --stat`")


# ---------------------------------------------------------------------------- release

TOOL_PATHS = ["vmbind.py", "vmbind.toml", "README.md", "MAINTAINING.md", "LICENSE",
              "patches", "tests", "docs", "ci", ".github", ".gitignore"]


def cmd_release(args) -> None:
    m = manifest()
    env = ident_env(args)
    ids = [t["id"] for t in m["targets"]] if args.all else args.targets
    if not ids:
        raise Fail("name targets or pass --all")
    version = m["project"]["version"]
    head = gitout("rev-parse", "HEAD")
    for tid in ids:
        t = target_by_id(m, tid)
        tag = f"{tid}-vmbind-{version}"
        branch = f"release/{tid}"
        if git("rev-parse", "-q", "--verify", f"refs/tags/{tag}", check=False).returncode == 0:
            if not args.force:
                log(f"{tag} exists, skipping (bump project.version or use --force)")
                continue
            git("tag", "-d", tag)
        wt = STATE / "wt" / f"release-{tid}"
        if wt.exists():
            git("worktree", "remove", "--force", wt)
        git("worktree", "add", "-q", "--detach", wt, f"upstream/{tid}^{{commit}}")
        try:
            for stack, p in stack_patches(m, t["line"]):
                try:
                    apply_one(wt, m, stack, p, env)
                except Fail as e:
                    raise Fail(f"{stack}/{p.name} does not apply to upstream/{tid}; run "
                               f"`vmbind.py start {tid}` and export first.\n{e}")
            tracked = set(gitout("ls-tree", "--name-only", head).split())
            present = [p for p in TOOL_PATHS if p in tracked]
            git("checkout", head, "--", *present, cwd=wt, env=env)
            rel = {"target": tid, "line": t["line"], "flavour": t.get("flavour", "vanilla"),
                   "kernelversion": t.get("kernelversion"), "version": version,
                   "abi": m["project"].get("abi"), "tooling": head,
                   "stacks": [f"{s}/{p.name}" for s, p in stack_patches(m, t["line"])]}
            (wt / "RELEASE.json").write_text(json.dumps(rel, indent=2) + "\n")
            git("add", "-A", "RELEASE.json", *present, cwd=wt, env=env)
            git("commit", "-q", "--no-verify", "-m", f"Release i915-vmbind {version} for {tid}",
                cwd=wt, env=env)
            commit = gitout("rev-parse", "HEAD", cwd=wt)
            git("branch", "-f", branch, commit)
            git("tag", "-a", "-m", f"i915-vmbind {version} for {tid}", tag, commit, env=env)
            log(f"{branch} -> {tag}")
        finally:
            git("worktree", "remove", "--force", wt)


# ------------------------------------------------------------------------------- init

def cmd_init(args) -> None:
    env = ident_env(args)
    if not (ROOT / ".git").exists():
        git("init", "-q", "-b", "main")
    try:
        name = env.get("GIT_AUTHOR_NAME") or gitout("config", "user.name")
        email = env.get("GIT_AUTHOR_EMAIL") or gitout("config", "user.email")
    except Fail:
        raise Fail("set git user.name/user.email, or pass --name/--email")
    log(f"history will be committed as {name} <{email}>")
    if git("rev-parse", "-q", "--verify", "HEAD", check=False).returncode:
        git("add", "-A", env=env)
        git("commit", "-q", "--no-verify", "-m", "Initial import of i915-vmbind tooling and patch stacks",
            env=env)
    archives = sorted((ROOT / "imports").glob("*.tar.xz"))
    for a in archives:
        tid = a.name[: -len(".tar.xz")]
        if git("rev-parse", "-q", "--verify", f"refs/tags/upstream/{tid}", check=False).returncode == 0:
            continue
        with tempfile.TemporaryDirectory(prefix="vmbind-init-") as tmp:
            run(["tar", "-xJf", a, "-C", tmp])
            meta = json.loads((Path(tmp) / "UPSTREAM.json").read_text())
            msg = (f"Import {tid} i915 sources\n\nLinux {meta['kernelversion']} ({meta['flavour']})\n"
                   f"Source: {meta['source']}\n")
            import_tree_to_tag(Path(tmp), tid, msg, env)
            log(f"tagged upstream/{tid}")
    if not args.no_release:
        args.all, args.targets, args.force = True, [], False
        cmd_release(args)
    log("done. Push with: git push -u origin main 'refs/heads/release/*' 'refs/tags/*'")


# ------------------------------------------------------------------------------ build

def kernel_release(kdir: Path) -> str:
    f = kdir / "include/config/kernel.release"
    if f.exists():
        return f.read_text().strip()
    m = re.search(r"/modules/([^/]+)/build/?$", str(kdir))
    if m:
        return m.group(1)
    raise Fail(f"cannot determine kernel release of {kdir}")


def kconfig(kdir: Path) -> dict:
    cfg = {}
    for ln in (kdir / ".config").read_text().splitlines():
        if ln.startswith("CONFIG_") and "=" in ln:
            k, v = ln.split("=", 1)
            cfg[k] = v.strip('"')
    return cfg


def resolve_kdir(args) -> Path:
    if args.headers_pkg:
        pkg = Path(args.headers_pkg)
        if not pkg.exists() and re.match(r"^https?://", args.headers_pkg):
            pkg = download(args.headers_pkg)
        dest = CACHE / "headers" / pkg.name
        if not dest.exists():
            log(f"extracting {pkg.name}")
            tmp = dest.with_name(dest.name + ".tmp")
            shutil.rmtree(tmp, ignore_errors=True)
            extract(pkg, tmp)
            tmp.rename(dest)
        cands = [p for p in dest.glob("usr/lib/modules/*/build") if (p / "Makefile").exists()]
        cands += [p for p in dest.glob("lib/modules/*/build") if (p / "Makefile").exists()]
        cands += [p for p in dest.glob("usr/src/*") if (p / ".config").exists()]
        if not cands:
            raise Fail(f"no kernel build tree inside {pkg.name}")
        return cands[0].resolve()
    if args.kdir:
        return Path(args.kdir).resolve()
    kver = args.kver or platform.release()
    for base in ("/usr/lib/modules", "/lib/modules"):
        p = Path(base) / kver / "build"
        if (p / "Makefile").exists():
            return p.resolve()
    raise Fail(f"no headers for {kver}: install your distro's kernel headers package "
               "(e.g. linux-headers, linux-cachyos-headers) or pass --kdir/--headers-pkg")


def find_release_src() -> tuple[Path, dict] | None:
    rel = ROOT / "RELEASE.json"
    if rel.exists() and (ROOT / "src").is_dir():
        return ROOT / "src", json.loads(rel.read_text())
    return None


def stage_tree(src: Path, kdir: Path, stage: Path) -> Path:
    if stage.exists():
        shutil.rmtree(stage)
    i915 = stage / "drivers/gpu/drm/i915"
    shutil.copytree(src / "drivers/gpu/drm/i915", i915, symlinks=True)
    ips = src / "drivers/platform/x86/intel_ips.h"
    if ips.exists():
        (stage / "drivers/platform/x86").mkdir(parents=True)
        shutil.copy2(ips, stage / "drivers/platform/x86/")
    (stage / "include/trace").mkdir(parents=True)  # anchors TRACE_INCLUDE_PATH ../../drivers/...
    ov = stage / "override"
    (ov / "uapi/drm").mkdir(parents=True)
    (ov / "drm").mkdir(parents=True)
    for h in sorted((src / "include/uapi/drm").glob("*.h")):
        shutil.copy2(h, ov / "uapi/drm")
        if not (kdir / "include/drm" / h.name).exists():
            shutil.copy2(h, ov / "drm")
    return i915


def needs_container(kdir: Path) -> bool:
    modpost = kdir / "scripts/mod/modpost"
    if not modpost.exists():
        return False
    r = run([modpost, "-h"], check=False, capture=True)
    return "GLIBC" in (r.stderr or "") or "not found" in (r.stderr or "")


def header_guard(stage: Path, kdir: Path, reference: Path, ignore: list[str]) -> tuple[list[str], int]:
    """Compare every kernel header the build actually used against the pinned
    reference snapshot. Returns the differing paths (relative to kdir)."""
    refsums = {}
    for ln in reference.read_text().splitlines():
        if ln:
            h, p = ln.split("  ", 1)
            refsums[p] = h
    used: set[str] = set()
    kd = str(kdir)
    for cmd in stage.rglob(".*.o.cmd"):
        for tok in re.split(r"[\s\\]+", cmd.read_text(errors="replace")):
            if tok.endswith(".h") and not tok.startswith("$("):
                p = os.path.normpath(tok if os.path.isabs(tok) else os.path.join(kd, tok))
                if p.startswith(kd + os.sep):
                    used.add(os.path.relpath(p, kd))
    diffs, checked = [], 0
    for rel in sorted(used):
        if rel in refsums and not any(fnmatch.fnmatch(rel, g) for g in ignore):
            f = kdir / rel
            if f.exists():
                checked += 1
                if sha256(f) != refsums[rel]:
                    diffs.append(rel)
    return diffs, checked


def btf_check(kdir: Path, ko: Path, src: Path) -> list[str] | None:
    """Kernel structs that pahole had to duplicate into the module's BTF.
    A duplicate means the module saw a different layout than vmlinux.
    Structs that i915 itself defines under the same name are ignored."""
    vmlinux = kdir / "vmlinux"
    bpftool = shutil.which("bpftool")
    if not (vmlinux.exists() and bpftool):
        return None
    r = run([bpftool, "btf", "dump", "-B", vmlinux, "file", ko, "format", "raw"],
            check=False, capture=True)
    if r.returncode:
        return None
    base = run([bpftool, "btf", "dump", "file", vmlinux, "format", "raw"], capture=True).stdout
    names = lambda txt: set(re.findall(r"^\[\d+\] STRUCT '([^'(][^']*)'", txt, re.M))
    dup = names(r.stdout) & names(base)
    if dup:
        own = set()
        for f in (src / "drivers/gpu/drm/i915").rglob("*.[ch]"):
            own |= set(re.findall(r"^struct (\w+) \{", f.read_text(errors="replace"), re.M))
        dup -= own
    return sorted(dup)


def container_engine() -> str:
    for e in ("podman", "docker"):
        if shutil.which(e):
            return e
    raise Fail("--container arch needs podman or docker")


def container_image(rebuild=False) -> str:
    img = "i915-vmbind-build:latest"
    if rebuild or run([container_engine(), "image", "inspect", img], check=False, capture=True).returncode:
        log(f"building container image {img}")
        run([container_engine(), "build", "-t", img, "-f", ROOT / "ci" / "Containerfile", ROOT / "ci"])
    return img


def cmd_build(args) -> None:
    m = manifest() if MANIFEST.exists() else None
    kdir = resolve_kdir(args)
    kver = kernel_release(kdir)
    if args.kver and args.kver != kver:
        raise Fail(f"--kver {args.kver} but headers in {kdir} are for {kver}")
    cfg = kconfig(kdir)
    if cfg.get("CONFIG_DRM_I915") not in ("m", None) and not args.force:
        raise Fail(f"{kver} has CONFIG_DRM_I915={cfg.get('CONFIG_DRM_I915')}; i915 must be a module")
    log(f"kernel {kver} ({kdir})")

    rel_src = find_release_src()
    if args.source:
        src = Path(args.source).resolve()
        src = src / "src" if (src / "src/drivers/gpu/drm/i915").is_dir() else src
        tinfo = {"id": args.target or "custom", "line": "?"}
        refsum = Path(args.source) / "reference.sha256"
    elif rel_src:
        src, rel = rel_src
        tinfo = {"id": rel["target"], "line": rel["line"], "kver": None}
        if m:
            tinfo.update(target_by_id(m, rel["target"]))
        refsum = ROOT / "reference.sha256"
    else:
        t = target_by_id(m, args.target) if args.target else select_target(m, kver)
        if not t:
            raise Fail(f"no target matches kernel {kver}. Targets: "
                       f"{', '.join(x['id'] for x in m['targets'])}. Pick one with --target "
                       "(add --force if versions differ), or import this kernel.")
        tinfo = t
        work = STATE / "src" / t["id"]
        log(f"preparing {t['id']} sources (line {t['line']})")
        materialize(m, t, work)
        src, refsum = work / "src", work / "reference.sha256"
    if tinfo.get("kver") and not re.match(tinfo["kver"], kver):
        msg = f"target {tinfo['id']} is for kernels matching {tinfo['kver']}, not {kver}"
        if not args.force:
            raise Fail(msg + " (use --force to build anyway)")
        warn(msg)
    log(f"source: target {tinfo['id']}")

    outdir = Path(args.out_dir or (ROOT / "out" / kver)).resolve()
    stage = Path(args.stage_dir or (STATE / "build" / kver)).resolve()
    i915 = stage_tree(src, kdir, stage)

    llvm = {"auto": cfg.get("CONFIG_CC_IS_CLANG") == "y", "llvm": True, "gcc": False}[args.toolchain]
    container = args.container
    if container == "auto":
        container = "arch" if needs_container(kdir) else "none"
        if container == "arch":
            log("host cannot run this kernel's build tools; building in a container")
    version = m["project"]["version"] if m else "unknown"
    make = ["make", "-C", kdir, f"M={i915}", f"-j{args.jobs}",
            f"NOSTDINC_FLAGS=-nostdinc -I{stage}/override -I{stage}/include/trace",
            f"KCFLAGS=-DI915_VMBIND_RELEASE={version}"]
    if llvm:
        make.append("LLVM=1")
    if args.extra_make:
        make += args.extra_make
    make.append("modules")
    if container == "none":
        want = cfg.get("CONFIG_CC_VERSION_TEXT", "")
        cc = "clang" if llvm else "gcc"
        if not shutil.which(cc):
            raise Fail(f"{kver} was built with '{want}' but {cc} is not installed "
                       "(or use --container arch)")
        have = out([cc, "--version"]).splitlines()[0]
        wm, hm = re.search(r"(\d+)\.\d+", want), re.search(r"(\d+)\.\d+", have)
        if wm and hm and wm.group(1) != hm.group(1):
            warn(f"kernel built with '{want}', host has '{have}'")
        log(f"building with {cc}")
        run(make, capture=False)
    else:
        img = container_image()
        mounts = sorted({str(kdir), str(Path(kdir).resolve()), str(stage), str(CACHE)})
        eng = container_engine()
        dock = [eng, "run", "--rm"]
        dock += ["--userns=keep-id"] if eng == "podman" else ["-u", f"{os.getuid()}:{os.getgid()}"]
        for mnt in mounts:
            if Path(mnt).exists():
                dock += ["-v", f"{mnt}:{mnt}"]
        log(f"building in {img}")
        run(dock + [img] + make, capture=False)

    built = [i915 / f"{mod}.ko" for mod in MODULES if (i915 / f"{mod}.ko").exists()]
    if not any(b.name == "i915.ko" for b in built):
        raise Fail("build produced no i915.ko")

    report = {"target": tinfo["id"], "line": tinfo.get("line"), "kernel": kver,
              "version": version, "toolchain": "llvm" if llvm else "gcc",
              "built": time.strftime("%Y-%m-%dT%H:%M:%SZ", time.gmtime())}
    if refsum.exists() and not args.no_header_guard:
        diffs, checked = header_guard(stage, kdir, refsum,
                                      (m or {}).get("guard", {}).get("ignore", []))
        report["headers_checked"] = checked
        report["header_guard"] = diffs
        if diffs:
            print("\nThese kernel headers used by i915 differ from the pinned "
                  f"{tinfo['id']} snapshot:", file=sys.stderr)
            for d in diffs[:40]:
                print(f"  {d}", file=sys.stderr)
            if len(diffs) > 40:
                print(f"  ... and {len(diffs) - 40} more", file=sys.stderr)
            hint = ("Your kernel is patched differently from this target (wrong flavour or "
                    "version?). The module may load and then crash.")
            if not (args.force or getattr(args, "allow_header_mismatch", False)):
                raise Fail(hint + " Use --allow-header-mismatch to keep the build anyway.")
            warn(hint)
        else:
            log(f"header guard: all {checked} kernel headers i915 used match the pinned snapshot")
    if not args.no_btf_check:
        dup = btf_check(kdir, i915 / "i915.ko", src)
        report["btf_duplicates"] = dup
        if dup is None:
            log("BTF check skipped (needs vmlinux in headers and bpftool)")
        elif dup:
            warn(f"BTF: {len(dup)} kernel structs differ between vmlinux and i915.ko: "
                 + ", ".join(dup[:15]))
            if args.strict_btf:
                raise Fail("BTF layout check failed")
        else:
            log("BTF check: no kernel struct layout differences")

    outdir.mkdir(parents=True, exist_ok=True)
    for old in outdir.glob("*.ko*"):
        old.unlink()
    strip = args.strip if args.strip != "auto" else "yes"
    comp = args.compress
    if comp == "auto":
        comp = ("zst" if cfg.get("CONFIG_MODULE_COMPRESS_ZSTD") == "y" else
                "xz" if cfg.get("CONFIG_MODULE_COMPRESS_XZ") == "y" else
                "gz" if cfg.get("CONFIG_MODULE_COMPRESS_GZIP") == "y" else "none")
    sign = None
    if args.sign:
        sign = args.sign
    elif args.sign_auto and (kdir / "certs/signing_key.pem").exists():
        sign = [kdir / "certs/signing_key.pem", kdir / "certs/signing_key.x509"]
    files = []
    for ko in built:
        dst = outdir / ko.name
        shutil.copy2(ko, dst)
        if strip == "yes":
            tool = shutil.which("llvm-strip" if llvm else "strip") or shutil.which("strip")
            if tool:
                run([tool, "--strip-debug", dst])
            else:
                warn("no strip tool found; leaving debug info")
        if sign:
            run([kdir / "scripts/sign-file", "sha512", sign[0], sign[1], dst])
        if comp == "zst":
            run(["zstd", "-q", "-19", "--rm", "-f", dst]); dst = dst.with_suffix(".ko.zst")
        elif comp == "xz":
            run(["xz", "-f", "--check=crc32", "--lzma2=dict=1MiB", dst]); dst = dst.with_suffix(".ko.xz")
        elif comp == "gz":
            run(["gzip", "-9f", dst]); dst = dst.with_suffix(".ko.gz")
        files.append({"file": dst.name, "sha256": sha256(dst)})
    report["modules"] = files
    report["signed"] = bool(sign)
    vm = run(["modinfo", "-F", "vermagic", outdir / files[0]["file"]], check=False, capture=True)
    if vm.returncode == 0:
        report["vermagic"] = vm.stdout.strip()
        if not vm.stdout.startswith(kver + " "):
            raise Fail(f"vermagic '{vm.stdout.strip()}' does not match {kver}")
    (outdir / "BUILDINFO.json").write_text(json.dumps(report, indent=2) + "\n")
    log(f"built {', '.join(f['file'] for f in files)} in {outdir}")
    log(f"install with: sudo {sys.argv[0]} install --kver {kver}")


# ---------------------------------------------------------------------------- install

INSTALL_SUBDIR = "updates/i915-vmbind"


def regen_initramfs(kver: str) -> None:
    if shutil.which("mkinitcpio"):
        run(["mkinitcpio", "-P"], capture=False)
    elif shutil.which("dracut"):
        run(["dracut", "-f", "--kver", kver], capture=False)
    elif shutil.which("update-initramfs"):
        run(["update-initramfs", "-u", "-k", kver], capture=False)
    elif shutil.which("booster"):
        run(["booster", "build", "--force", "--kernel-version", kver,
             f"/boot/booster-{kver}.img"], capture=False)
    else:
        warn("no initramfs tool found; regenerate your initramfs manually if it contains i915")


def cmd_install(args) -> None:
    kver = args.kver or platform.release()
    src = Path(args.dir or ROOT / "out" / kver)
    info = json.loads((src / "BUILDINFO.json").read_text())
    if info["kernel"] != kver:
        raise Fail(f"{src} was built for {info['kernel']}, not {kver}")
    root = Path(args.root)
    dest = root / "usr/lib/modules" / kver / INSTALL_SUBDIR
    if not (root / "usr/lib/modules" / kver).exists() and (root / "lib/modules" / kver).exists():
        dest = root / "lib/modules" / kver / INSTALL_SUBDIR
    dest.mkdir(parents=True, exist_ok=True)
    for old in dest.glob("*.ko*"):
        old.unlink()
    for f in info["modules"]:
        shutil.copy2(src / f["file"], dest / f["file"])
        (dest / f["file"]).chmod(0o644)
    shutil.copy2(src / "BUILDINFO.json", dest / "BUILDINFO.json")
    log(f"installed to {dest}")
    if shutil.which("depmod"):
        run(["depmod", "-b", root, "-a", kver] if str(root) != "/" else ["depmod", "-a", kver])
    if str(root) == "/" and not args.no_initramfs:
        regen_initramfs(kver)
    log("reboot to load the new i915. Roll back with `vmbind.py uninstall`; "
        "or boot once with i915.enable_vm_bind=0 to disable VM_BIND only")


def cmd_uninstall(args) -> None:
    kver = args.kver or platform.release()
    root = Path(args.root)
    removed = False
    for base in ("usr/lib/modules", "lib/modules"):
        d = root / base / kver / INSTALL_SUBDIR
        if d.exists():
            shutil.rmtree(d)
            removed = True
            log(f"removed {d}")
    if not removed:
        log("nothing installed for " + kver)
        return
    if shutil.which("depmod"):
        run(["depmod", "-b", root, "-a", kver] if str(root) != "/" else ["depmod", "-a", kver])
    if str(root) == "/" and not args.no_initramfs:
        regen_initramfs(kver)


# ------------------------------------------------------------------------------- dkms

DKMS_CONF = """PACKAGE_NAME="i915-vmbind"
PACKAGE_VERSION="{version}"
AUTOINSTALL="yes"
MAKE[0]="python3 ./vmbind.py build --kdir ${{kernel_source_dir}} --kver ${{kernelver}} --out-dir ./dkms-out --stage-dir ./dkms-build/${{kernelver}} --container none --strip no --compress none --no-btf-check {guard}"
CLEAN="rm -rf ./dkms-out ./dkms-build"
BUILT_MODULE_NAME[0]="i915"
BUILT_MODULE_LOCATION[0]="dkms-out"
DEST_MODULE_LOCATION[0]="/updates/dkms"
if grep -qs '^CONFIG_DRM_I915_GVT_KVMGT=m' "/lib/modules/${{kernelver}}/build/.config"; then
    BUILT_MODULE_NAME[1]="kvmgt"
    BUILT_MODULE_LOCATION[1]="dkms-out"
    DEST_MODULE_LOCATION[1]="/updates/dkms"
fi
"""


def cmd_dkms(args) -> None:
    m = manifest()
    version = m["project"]["version"]
    ids = args.targets or [t["id"] for t in m["targets"] if t.get("status") != "experimental"]
    dest = Path(args.out_dir).resolve() / f"i915-vmbind-{version}"
    if dest.exists():
        shutil.rmtree(dest)
    (dest / "targets").mkdir(parents=True)
    shutil.copy2(ROOT / "vmbind.py", dest)
    targets = []
    for tid in ids:
        t = target_by_id(m, tid)
        work = STATE / "src" / tid
        materialize(m, t, work)
        td = dest / "targets" / tid
        shutil.copytree(work / "src", td / "src", symlinks=True)
        shutil.copy2(work / "reference.sha256", td)
        targets.append(t)
        log(f"added {tid}")
    with open(dest / "vmbind.toml", "w") as f:
        f.write(MANIFEST.read_text().split("[[targets]]")[0])
        for t in targets:
            f.write("\n[[targets]]\n")
            for k in ("id", "line", "flavour", "kernelversion"):
                if k in t:
                    f.write(f"{k} = {json.dumps(t[k])}\n")
            f.write(f"kver = '{t['kver']}'\n")
    (dest / "dkms.conf").write_text(DKMS_CONF.format(
        version=version, guard="--allow-header-mismatch" if args.lenient else ""))
    log(f"DKMS tree: {dest}")
    log(f"install with: sudo cp -r {dest} /usr/src/ && sudo dkms install i915-vmbind/{version}")
    if args.tarball:
        tb = dest.parent / f"i915-vmbind-{version}-dkms.tar.gz"
        run(["tar", "-czf", tb, "-C", dest.parent, dest.name])
        log(f"wrote {tb}")


def dkms_tree_src(kver: str) -> tuple[Path, dict] | None:
    """Inside a DKMS tree: pick targets/<id>/ for this kernel."""
    if not (ROOT / "targets").is_dir() or not MANIFEST.exists():
        return None
    m = manifest()
    t = select_target(m, kver)
    if not t:
        raise Fail(f"i915-vmbind has no source for kernel {kver}; "
                   f"supported: {', '.join(x['id'] for x in m['targets'])}")
    return ROOT / "targets" / t["id"], t


# ------------------------------------------------------------------------------ check

AI_RE = re.compile(r"co-authored-by|devin|claude|chatgpt|openai|copilot|gemini|cursor\.sh"
                   r"|generated (by|with) ai", re.I)


def cmd_check(args) -> None:
    m = manifest()
    problems = []
    for line_dir in sorted((ROOT / "patches").iterdir()):
        for stack, p in stack_patches(m, line_dir.name):
            txt = p.read_text()
            head = txt.split("\ndiff --git", 1)[0]
            if AI_RE.search(head):
                problems.append(f"{p.relative_to(ROOT)}: AI name or co-author trailer in metadata")
            if not is_local_stack(m, stack) and not re.search(r"^From: .+<.+@.+>", txt, re.M):
                problems.append(f"{p.relative_to(ROOT)}: upstream-authored stack needs a From: header")
            if is_local_stack(m, stack) and re.search(r"^From: ", head, re.M):
                problems.append(f"{p.relative_to(ROOT)}: local stack patches carry no author")
    for pr in problems:
        print(f"FAIL {pr}")
    ids = args.targets or [t["id"] for t in m["targets"]]
    for tid in ids:
        t = target_by_id(m, tid)
        try:
            materialize(m, t, STATE / "src" / tid)
            print(f"ok   {tid}: {len(stack_patches(m, t['line']))} patches apply")
        except Fail as e:
            problems.append(str(e))
            print(f"FAIL {tid}: {str(e).splitlines()[0]}")
            continue
        if args.build:
            hdrs = t.get("headers", [])
            if not hdrs:
                print(f"skip {tid}: no headers package in vmbind.toml")
                continue
            pkg = first_reachable(hdrs)
            if not pkg:
                problems.append(f"{tid}: none of its headers URLs is reachable")
                print(f"FAIL {tid}: no reachable headers package")
                continue
            ns = argparse.Namespace(**{**vars(build_defaults()), "headers_pkg": pkg,
                                       "target": tid, "container": args.container,
                                       "out_dir": str(ROOT / "out" / f"check-{tid}")})
            try:
                cmd_build(ns)
                print(f"ok   {tid}: built")
            except Fail as e:
                problems.append(f"{tid}: {e}")
                print(f"FAIL {tid}: build: {e}")
    if problems:
        raise Fail(f"{len(problems)} problem(s)")
    log("all checks passed")


def first_reachable(urls: list[str]) -> str | None:
    """First local file or URL answering a HEAD request (mirrors drop old packages,
    archives get them later, so targets list both)."""
    for u in urls:
        if not re.match(r"^https?://", u):
            if Path(u).exists():
                return u
            continue
        try:
            req = urllib.request.Request(u, method="HEAD", headers={"User-Agent": "vmbind.py"})
            with urllib.request.urlopen(req, timeout=30):
                return u
        except (urllib.error.URLError, OSError):
            continue
    return None


def build_defaults():
    p = argparse.ArgumentParser()
    add_build_args(p)
    return p.parse_args([])


# ---------------------------------------------------------------- overlap/abi/list

def hunks(diff: str) -> dict[str, list[tuple[int, int]]]:
    res: dict[str, list[tuple[int, int]]] = {}
    cur = None
    for ln in diff.splitlines():
        if ln.startswith("--- a/"):
            cur = ln[6:]
        elif ln.startswith("--- /dev/null"):
            cur = None
        elif ln.startswith("@@") and cur:
            mm = re.match(r"@@ -(\d+)(?:,(\d+))?", ln)
            s, n = int(mm.group(1)), int(mm.group(2) or 1)
            res.setdefault(cur, []).append((s, s + max(n, 1)))
    return res


def cmd_overlap(args) -> None:
    m = manifest()
    new = target_by_id(m, args.new)
    old = target_by_id(m, args.old)
    up = gitout("diff", "-U0", f"upstream/{old['id']}", f"upstream/{new['id']}", "--", "src")
    up_h = {k[4:]: v for k, v in hunks(up).items() if k.startswith("src/")}
    mine: dict[str, list[tuple[int, int]]] = {}
    for _, p in stack_patches(m, args.line or old["line"]):
        for f, hs in hunks(p.read_text()).items():
            mine.setdefault(f, []).extend(hs)
    print(f"upstream changed {len(up_h)} files between {old['id']} and {new['id']}")
    for f in sorted(set(up_h) & set(mine)):
        clash = sum(1 for a in up_h[f] for b in mine[f] if a[0] < b[1] + 3 and b[0] < a[1] + 3)
        print(f"  {'CLASH' if clash else 'touch'} {f}: {len(up_h[f])} upstream hunks"
              + (f", {clash} near our hunks" if clash else ""))


def cmd_abi(args) -> None:
    m = manifest()
    lines = sorted({t["line"] for t in m["targets"]})
    dest = Path(args.out_dir)
    for line in lines:
        t = next(x for x in m["targets"] if x["line"] == line)
        work = materialize(m, t, STATE / "src" / t["id"])
        d = dest / line / "drm"
        d.mkdir(parents=True, exist_ok=True)
        for name in ("drm.h", "drm_mode.h", "i915_drm.h"):
            txt = (work / "src/include/uapi/drm" / name).read_text()
            (d / name).write_text(re.sub(r"\b__user\b\s*", "", txt))
        hdr = (d / "i915_drm.h").read_text()
        d = dest / line
        ma = re.search(r"I915_VM_BIND_ABI_MAJOR\s+(\d+)", hdr)
        mi = re.search(r"I915_VM_BIND_ABI_MINOR\s+(\d+)", hdr)
        abi = f"{ma.group(1)}.{mi.group(1)}" if ma and mi else "none"
        (d / "ABI").write_text(abi + "\n")
        log(f"abi/{line}: VM_BIND ABI {abi}")


def cmd_list(args) -> None:
    m = manifest()
    tags = set(gitout("tag", "-l").split()) if in_git_repo() else set()
    print(f"{'target':24} {'line':6} {'flavour':12} {'imported':9} kernels")
    for t in m["targets"]:
        imp = "yes" if f"upstream/{t['id']}" in tags else "no"
        print(f"{t['id']:24} {t['line']:6} {t.get('flavour', 'vanilla'):12} {imp:9} {t['kver']}"
              + ("  [experimental]" if t.get("status") == "experimental" else ""))


# ------------------------------------------------------------------------------- main

def add_build_args(p) -> None:
    p.add_argument("--kdir", help="kernel build dir (default /lib/modules/<kver>/build)")
    p.add_argument("--headers-pkg", help="headers package file or URL (.pkg.tar.zst/.deb/.rpm) to build against")
    p.add_argument("--kver", help="kernel release; default: running kernel / headers")
    p.add_argument("--target", help="target id from vmbind.toml (default: match kver)")
    p.add_argument("--source", help="prepared i915 tree to build instead of a target")
    p.add_argument("--toolchain", choices=["auto", "gcc", "llvm"], default="auto")
    p.add_argument("--container", choices=["auto", "arch", "none"], default="auto")
    p.add_argument("--sign", nargs=2, metavar=("KEY", "CERT"), help="sign modules (MOK/Secure Boot)")
    p.add_argument("--sign-auto", action="store_true", help="sign with the headers' certs/signing_key.pem if present")
    p.add_argument("--strip", choices=["auto", "yes", "no"], default="auto")
    p.add_argument("--compress", choices=["auto", "zst", "xz", "gz", "none"], default="auto")
    p.add_argument("-j", "--jobs", type=int, default=os.cpu_count() or 1)
    p.add_argument("--out-dir", help="output dir (default out/<kver>)")
    p.add_argument("--stage-dir", help=argparse.SUPPRESS)
    p.add_argument("--no-header-guard", action="store_true")
    p.add_argument("--no-btf-check", action="store_true")
    p.add_argument("--strict-btf", action="store_true", help="fail on BTF layout differences")
    p.add_argument("--allow-header-mismatch", action="store_true",
                   help="keep the build when kernel headers differ from the pinned snapshot")
    p.add_argument("--force", action="store_true", help="build despite version/header mismatches")
    p.add_argument("--extra-make", nargs=argparse.REMAINDER, help="extra make arguments")


def main() -> None:
    global VERBOSE
    ap = argparse.ArgumentParser(prog="vmbind.py", description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("-v", "--verbose", action="store_true")
    sub = ap.add_subparsers(dest="cmd", required=True)

    def ident(p):
        p.add_argument("--name", help="git author/committer name (default: git config)")
        p.add_argument("--email", help="git author/committer email")

    p = sub.add_parser("init", help="create git history: main, upstream/* tags, release/* branches")
    ident(p); p.add_argument("--no-release", action="store_true"); p.set_defaults(fn=cmd_init)
    p = sub.add_parser("import", help="import i915 sources from a kernel dir/archive/URL/version")
    p.add_argument("kernel", help="directory, .tar.*/.zip, kernel.org version (7.3.5), or git URL")
    p.add_argument("--ref", help="branch/tag when importing from git")
    p.add_argument("--id", help="target id (default: <version>[-<flavour>])")
    p.add_argument("--flavour", help="e.g. cachyos, cachyos-lts (default vanilla)")
    p.add_argument("--line", help="patch line (default: major.minor)")
    p.add_argument("--kver", help="regex of `uname -r` values this target serves")
    p.add_argument("--source-label", help="provenance recorded in UPSTREAM.json")
    p.add_argument("--archive", help=argparse.SUPPRESS)
    ident(p); p.set_defaults(fn=cmd_import)
    p = sub.add_parser("start", help="apply the stacks onto a target in a worktree, stopping on conflicts")
    p.add_argument("target"); p.add_argument("--line", help="take patches from another line")
    ident(p); p.set_defaults(fn=cmd_start)
    p = sub.add_parser("continue", help="continue after resolving a conflict")
    p.add_argument("target"); p.add_argument("--skip", action="store_true"); ident(p)
    p.set_defaults(fn=cmd_continue)
    p = sub.add_parser("abort", help="drop a rebase worktree"); p.add_argument("target")
    p.set_defaults(fn=cmd_abort)
    p = sub.add_parser("export", help="write the worktree's commits back to patches/<line>/")
    p.add_argument("target"); p.add_argument("--line")
    p.add_argument("--extra-stack", help="stack for commits you added (default: last stack)")
    p.set_defaults(fn=cmd_export)
    p = sub.add_parser("release", help="create release/<id> branches and tags")
    p.add_argument("targets", nargs="*"); p.add_argument("--all", action="store_true")
    p.add_argument("--force", action="store_true"); ident(p); p.set_defaults(fn=cmd_release)
    p = sub.add_parser("build", help="build i915.ko/kvmgt.ko for a kernel")
    add_build_args(p); p.set_defaults(fn=cmd_build)
    p = sub.add_parser("install", help="install built modules into updates/")
    p.add_argument("--kver"); p.add_argument("--dir", help="build output dir (default out/<kver>)")
    p.add_argument("--root", default="/"); p.add_argument("--no-initramfs", action="store_true")
    p.set_defaults(fn=cmd_install)
    p = sub.add_parser("uninstall", help="remove installed modules (back to stock i915)")
    p.add_argument("--kver"); p.add_argument("--root", default="/")
    p.add_argument("--no-initramfs", action="store_true"); p.set_defaults(fn=cmd_uninstall)
    p = sub.add_parser("dkms", help="generate a DKMS source tree")
    p.add_argument("targets", nargs="*"); p.add_argument("--out-dir", default="out/dkms")
    p.add_argument("--tarball", action="store_true")
    p.add_argument("--lenient", action="store_true",
                   help="let DKMS keep builds whose kernel headers differ from the pinned snapshot")
    p.set_defaults(fn=cmd_dkms)
    p = sub.add_parser("check", help="lint patch metadata, apply stacks, optionally build")
    p.add_argument("targets", nargs="*"); p.add_argument("--build", action="store_true")
    p.add_argument("--container", choices=["auto", "arch", "none"], default="auto")
    p.set_defaults(fn=cmd_check)
    p = sub.add_parser("overlap", help="show where upstream changes meet our patches")
    p.add_argument("old"); p.add_argument("new"); p.add_argument("--line")
    p.set_defaults(fn=cmd_overlap)
    p = sub.add_parser("abi", help="export the VM_BIND uapi header per line (for Mesa)")
    p.add_argument("--out-dir", default="abi"); p.set_defaults(fn=cmd_abi)
    p = sub.add_parser("list", help="list targets"); p.set_defaults(fn=cmd_list)

    args = ap.parse_args()
    VERBOSE = args.verbose
    # Inside a DKMS tree, `build` picks the bundled source for the kernel.
    if args.cmd == "build" and not args.source:
        kv = args.kver or platform.release()
        d = dkms_tree_src(kv)
        if d:
            args.source, args.target = str(d[0]), d[1]["id"]
    try:
        args.fn(args)
    except Fail as e:
        print(f"error: {e}", file=sys.stderr)
        sys.exit(1)
    except KeyboardInterrupt:
        sys.exit(130)


if __name__ == "__main__":
    main()
