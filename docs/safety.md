# Safety features

| Feature | Where | What it does |
|---|---|---|
| Kill switch | `30-safety`, `i915.enable_vm_bind=0` | No VM_BIND VMs can be created, VM_BIND params report 0; driver behaves like stock i915 |
| ABI handshake | `30-safety`, `I915_PARAM_VM_BIND_ABI`, modinfo `vm_bind_abi` | Userspace only uses VM_BIND for a matching major ABI |
| Private uapi ranges | `30-safety` | Future upstream uapi can't collide with ours |
| Version match | `vmbind.py build` | Target `kver` regex must match the headers' kernel release |
| Header guard | `vmbind.py build` | Every kernel header i915 included must match the target's pinned snapshot |
| BTF check | `vmbind.py build` | With `vmlinux` in the headers and `bpftool`: kernel structs duplicated in the module's BTF (= layout mismatch) are reported; `--strict-btf` fails |
| vermagic check | `vmbind.py build` | Built module's vermagic must start with the target kernel release |
| Safe install | `vmbind.py install` | Modules go to `updates/i915-vmbind/` (stock modules untouched), depmod, initramfs |
| Rollback | `vmbind.py uninstall` | Removes them, depmod, initramfs |
| Signing | `--sign KEY CERT`, `--sign-auto` | For Secure Boot / `module.sig_enforce=1` |

## Before first install

- Hold back kernel updates (`IgnorePkg = linux-cachyos linux-cachyos-headers`
  on Arch/CachyOS) or use DKMS; a kernel update otherwise brings back the
  stock i915 silently.
- Keep a fallback kernel entry in your boot loader.
- If the desktop fails: boot with `module_blacklist=i915`, log in on a TTY,
  `sudo ./vmbind.py uninstall`, reboot.

## Known limits

- Runtime behaviour is unverified on hardware. The forward port changed
  userptr invalidation, error capture ordering, GT wakeref handling and GGTT
  bind commit to follow upstream changes (see the commit messages of
  `10-vm_bind`).
- On GPUs supported by both i915 and xe, make sure i915 binds (`xe.force_probe`
  / `i915.force_probe` settings).
