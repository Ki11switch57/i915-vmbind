# Architecture

## Why full source per kernel, not one driver with compat shims

i915 depends on DRM core, dma-buf, TTM and mm internals that change every
release. Intel's backports project solved that with a compat layer that grew
with every kernel. Here, each pinned kernel brings its own i915, and only the
VM_BIND stacks are carried forward. A target therefore builds exactly the
i915 that kernel shipped, plus our patches.

## What a target pins

`upstream/<id>` contains, from that kernel's tree:

- `src/drivers/gpu/drm/i915/`, `src/include/uapi/drm/`,
  `src/drivers/platform/x86/intel_ips.h` and the i915 docs: everything the
  stacks modify or the build needs beyond the kernel headers.
- `reference/include/`, `reference/arch/x86/include/`: the kernel headers the
  driver compiles against, and `reference.sha256`.

`reference/` is never used as include path. A module must be built against
the headers of the exact kernel binary it loads into (its `.config`,
generated headers, `Module.symvers`), which only the distro's headers package
has. Instead, the build compares every header i915 actually included (from
the compiler's dependency files) against `reference.sha256`. A difference
means the running kernel isn't the kernel the target pins (wrong version or
flavour), and the build stops.

## Build

`vmbind.py build` stages `src/` into `.vmbind/build/<kver>/`:

- `drivers/gpu/drm/i915/`: the driver, built with `make -C <kdir> M=...`.
- `override/uapi/drm/`, `override/drm/`: our uapi headers, placed in front of
  the kernel's include path so the patched `i915_drm.h` wins.
- `include/trace/`: anchors i915's relative `TRACE_INCLUDE_PATH`.

The toolchain follows the kernel's config (`LLVM=1` for clang/LTO kernels).
`--container arch` runs the same make in an Arch image (`ci/Containerfile`)
when the host can't run the headers' build tools (e.g. glibc too old).

## Patch stacks

`patches/<line>/<stack>/NNNN-*.patch`, applied in `[stacks].order`:

| Stack | Contents |
|---|---|
| `00-compat` | glue needed only for out-of-tree builds (currently empty) |
| `10-vm_bind` | the 23-patch VM_BIND series, original authorship |
| `20-fixes` | fixes to the series (VM reference leak in gem_create_ext) |
| `30-safety` | private uapi ranges, `enable_vm_bind` kill switch, ABI handshake |
| `40+` | future features, one stack each |

## Mesa

Mesa lives in a sister repository with the same model (untouched Mesa
release imports + an ANV VM_BIND stack). The two are joined by the uapi
contract in [abi.md](abi.md), exact-version package dependencies, and a
runtime ABI check. See abi.md.
