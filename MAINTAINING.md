# Maintaining i915-vmbind

All commands run from a `main` checkout. Python 3.11+ and git are required;
use `--container arch` for builds when your host can't run a target's tools.

## First-time setup (from the release zip)

```sh
unzip i915-vmbind.zip && cd i915-vmbind
git config --global user.name  "Killswitch"            # or pass --name/--email to init
git config --global user.email "abdullah2157@proton.me"
./vmbind.py init          # main commit, upstream/<id> tags from imports/, release/<id> branches + tags
git remote add origin <url>
git push -u origin main 'refs/heads/release/*' 'refs/tags/*'
rm imports/*.tar.xz       # now stored in the upstream/* tags
```

`init` commits as you. Patches in `10-vm_bind` keep their original author
(Niranjana Vishwanathapura); patches in local stacks (`00-compat`,
`20-fixes`, `30-safety`, ...) carry no author header and are committed as
whoever runs `init`/`release`, i.e. you. Never add co-author trailers or AI
names; `./vmbind.py check` rejects them.

## Model

| Ref | Contents | Mutable? |
|---|---|---|
| `main` | `vmbind.py`, `vmbind.toml`, `patches/<line>/<stack>/`, docs, CI | yes |
| `upstream/<id>` (tag) | untouched `src/` (i915 + uapi) and `reference/` (kernel headers) of one kernel | never |
| `release/<id>` (branch) | upstream + all stacks + tooling: a complete buildable tree | regenerated per release |
| `<id>-vmbind-<ver>` (tag) | a published release | never |

A *line* (`6.18`, `7.3`, `7.4`) is one patch set shared by every target of
that kernel series (vanilla and flavours).

## Monthly stable update

Example: 7.3.4 -> 7.3.5 and CachyOS 7.3.3 -> 7.3.5.

```sh
./vmbind.py import 7.3.5                                   # downloads from kernel.org
./vmbind.py import ~/linux-cachyos.zip --flavour cachyos \
    --id 7.3.5-cachyos                                     # dir, .zip, .tar.* or git URL --ref
./vmbind.py overlap 7.3.4 7.3.5                            # where upstream touched our code
./vmbind.py start 7.3.5                                    # applies the 7.3 stacks in .vmbind/wt/7.3.5
#   on conflict: fix files in .vmbind/wt/7.3.5, git add, ./vmbind.py continue 7.3.5
./vmbind.py build --target 7.3.5 --headers-pkg <linux-headers-7.3.5...pkg.tar.zst>
./vmbind.py export 7.3.5                                   # only if the patches had to change
./vmbind.py check
```

Then edit `vmbind.toml`: check the new targets' `kver` regex and `headers`,
and drop or mark old targets (their tags stay). Bump `project.version`,
commit, and run `./vmbind.py release --all` (existing release tags are kept,
new ones made). Push `main`, `release/*` and tags.

A new kernel series (e.g. 7.4) starts as a copy of the previous line:
`cp -r patches/7.3 patches/7.4`, then `import`, `start`, `export`.

Imports are immutable. To redo one, delete the tag locally first
(`git tag -d upstream/<id>`) and only if it was never pushed.

## Adding features or fixes

1. `./vmbind.py start <target>` for one target of the line you work on.
2. Commit your change in `.vmbind/wt/<target>` (normal git). Keep new code in
   new files and hooks into upstream files small: that keeps rebases cheap.
3. Build and test it.
4. `./vmbind.py export <target> --extra-stack 40-myfeature`. Add the stack to
   `[stacks].order` (and `local` if you wrote it) in `vmbind.toml`.
5. Port it to the other lines (`start <target-of-other-line>`, `git cherry-pick`,
   `export`).
6. If the uapi changes, bump `I915_VM_BIND_ABI_MINOR` (compatible) or
   `_MAJOR` (incompatible), update `project.abi`, `docs/abi.md`, and the Mesa
   repo.

Bug fixes to the original series go in `20-fixes`, never into `10-vm_bind`,
so the original series stays recognisable. Exception: pure forward-port
adaptations (renamed APIs etc.) needed to make a `10-vm_bind` patch apply or
build are folded into that patch, as was done for every release from 6.2 to
7.4.

## Checks

- `./vmbind.py check` – patch metadata, and every target's stacks apply.
- `./vmbind.py check --build` – also builds every target with a `headers` URL.
- CI (`.github/workflows/ci.yml`) runs `check --build` on each push and PR.
- Hardware: `tests/vmbind_check` on a Gen12+ machine after install.
