# VM_BIND uapi contract (kernel <-> Mesa)

The kernel repo owns the contract. `./vmbind.py abi` exports the patched
uapi headers per line to `abi/<line>/drm/` plus `abi/<line>/ABI`; the Mesa
sister repo copies `i915_drm.h` into its `include/drm-uapi/` and its CI must
fail if the copy differs from the pinned kernel-repo tag.

## Current ABI: 1.0

| Item | Value |
|---|---|
| `DRM_I915_GEM_VM_BIND` / `VM_UNBIND` / `EXECBUFFER3` | ioctl nr `0x5d` / `0x5e` / `0x5f` |
| `I915_PARAM_VM_BIND_VERSION` | `0x56420000`; 1 when VM_BIND is usable, 0 otherwise |
| `I915_PARAM_VM_BIND_ABI` | `0x56420001`; `(major << 16) \| minor`, 0 when unusable |
| `I915_VM_CREATE_FLAGS_USE_VM_BIND` | bit 31 of `drm_i915_gem_vm_control.flags` |
| `I915_GEM_CREATE_EXT_VM_PRIVATE` | create extension `0x20` |
| modinfo | `vm_bind_abi=1.0`, `vm_bind_release=<repo version>` |

All numbers sit in ranges upstream i915 will not reach (upstream already
took the original series' getparam 58/59 and create-extension 2). Stock
kernels return `-EINVAL` for every one of them, so probing is safe.

## Rules for userspace

1. Query `I915_PARAM_VM_BIND_ABI`. On error or 0, use execbuf2 as today.
2. Use VM_BIND only if the major equals the one you were built for and the
   minor is at least the one you need. Otherwise fall back to execbuf2.
3. VM_BIND can be disabled at runtime (`i915.enable_vm_bind=0`); never assume
   it from the module version alone.

## Changing the ABI

- Compatible addition (new flag, extension, param): bump
  `I915_VM_BIND_ABI_MINOR` in the `30-safety` handshake patch of every line.
- Incompatible change: bump `_MAJOR`, reset minor. Mesa built for the old
  major falls back to execbuf2 automatically.
- Update `project.abi` in `vmbind.toml` and this table, then cut a release of
  both repos together.

## Packaging the bundle

Packages built from a kernel-repo release (`i915-vmbind-dkms`) and the Mesa
repo release (`mesa-anv-vmbind`, ANV only, installed as a separate ICD next to
distro Mesa) depend on each other's exact version. The runtime handshake
covers anything packaging misses.
