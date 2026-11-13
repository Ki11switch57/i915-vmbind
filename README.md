# i915-vmbind

Out-of-tree i915 kernel driver with the VM_BIND uapi (VM_BIND/VM_UNBIND,
EXECBUFFER3, VM-private BOs, userptr binds, out-fences), forward-ported from
Niranjana Vishwanathapura's never-merged 2022 series to current kernels.
Builds `i915.ko`/`kvmgt.ko` for one kernel without building the kernel.

> Status: compile-tested on every target below. **Not yet runtime-tested on
> hardware.** VM_BIND needs a Gen12+ GPU (TGL, ADL, RPL, DG1/DG2, MTL).
> Standard Mesa and all existing i915 userspace keep working unchanged.

## Quick start (build for the kernel you are running)

Requirements: Python 3.11+, git, make, your distro's kernel headers package
(`linux-headers`, `linux-cachyos-headers`, `linux-lts-headers`, ...), and the
compiler your kernel was built with (clang/lld for CachyOS, gcc for Arch).

```sh
git clone -b release/7.3.3-cachyos <your-repo-url> i915-vmbind   # or any release/* branch
cd i915-vmbind
./vmbind.py build                      # uses /lib/modules/$(uname -r)/build
sudo ./vmbind.py install               # -> /usr/lib/modules/<kver>/updates/i915-vmbind/, depmod, initramfs
reboot
```

From `main` the same `./vmbind.py build` works: it picks the target whose
`kver` regex matches `uname -r` and assembles the source from the
`upstream/<id>` tag plus `patches/`. `./vmbind.py list` shows the targets.

Check it after reboot:

```sh
./vmbind.py abi && make -C tests LINE=7.3 && ./tests/vmbind_check
```

Roll back: `sudo ./vmbind.py uninstall && reboot`. To keep the module but turn
VM_BIND off, boot with `i915.enable_vm_bind=0`. If the desktop doesn't come
up, boot with `module_blacklist=i915` and uninstall from a TTY.

## Targets

| Target | Line | For `uname -r` | Source |
|---|---|---|---|
| `7.3.4` | 7.3 | `7.3.4-*` (Arch `linux`, other vanilla builds) | kernel.org v7.3.4 |
| `7.3.3-cachyos` | 7.3 | `7.3.3-N-cachyos` | CachyOS `cachyos-7.3.3-1` |
| `7.3.1-cachyos` | 7.3 | `7.3.1-N-cachyos` | CachyOS `cachyos-7.3.1-1` |
| `6.18.63` | 6.18 | `6.18.63-*` (Arch `linux-lts`) | kernel.org v6.18.63 |
| `6.18.60-cachyos-lts` | 6.18 | `6.18.60-N-cachyos-lts` | CachyOS `cachyos-6.18.60-1` |
| `7.4-rc2` | 7.4 | `7.4.0-rc2*` (experimental) | mainline `ee83899302a0` |

Flavoured kernels (CachyOS etc.) patch i915 and DRM headers, so they get their
own target; building a vanilla target against them is refused by the header
guard.

## Useful build options

| Option | Purpose |
|---|---|
| `--kdir DIR` / `--headers-pkg FILE\|URL` | Build for a kernel other than the running one (`.pkg.tar.zst`, `.deb`, `.rpm`) |
| `--target ID` | Force a target instead of matching `uname -r` |
| `--toolchain auto\|gcc\|llvm` | `auto` follows the kernel's `CONFIG_CC_IS_CLANG` |
| `--container auto\|arch\|none` | Build in an Arch container (needed when the host can't run the headers' tools, e.g. Ubuntu building for CachyOS) |
| `--sign KEY CERT` / `--sign-auto` | Sign for Secure Boot / `module.sig_enforce` |
| `--strip`, `--compress` | Default: strip debug info, compress like the kernel's modules |
| `--force` | Build despite a version or header mismatch (not recommended) |

Every build writes `out/<kver>/BUILDINFO.json` (target, toolchain, header guard
and BTF results, module hashes).

## DKMS

```sh
./vmbind.py dkms --tarball                         # out/dkms/i915-vmbind-<ver>/ with all stable targets
sudo cp -r out/dkms/i915-vmbind-1.0.0 /usr/src/
sudo dkms install i915-vmbind/1.0.0
```

DKMS rebuilds on kernel updates. It fails cleanly for kernels no target
covers, and for kernels whose headers differ from the target's pinned snapshot
(generate with `--lenient` to only warn).

## Repository layout

See [docs/architecture.md](docs/architecture.md). In short: `main` holds the
tool, `vmbind.toml` and the patch stacks; `upstream/<id>` tags hold untouched
i915 sources (plus reference kernel headers) of each pinned kernel;
`release/<id>` branches and `<id>-vmbind-<version>` tags are complete,
buildable trees.

- [MAINTAINING.md](MAINTAINING.md): first-time setup, monthly updates, new features.
- [docs/safety.md](docs/safety.md): kill switch, header guard, BTF check, install safety.
- [docs/abi.md](docs/abi.md): the uapi contract with the Mesa sister repo.
- [docs/porting-notes.md](docs/porting-notes.md): every adaptation made while forward-porting.

## Credits and licensing

The VM_BIND series is by Niranjana Vishwanathapura (Intel) and keeps its
original authorship and sign-offs. Kernel code and patches are GPL-2.0 (MIT
for files that say so), like upstream i915. `vmbind.py` and the tests are MIT.
See [LICENSE](LICENSE).
