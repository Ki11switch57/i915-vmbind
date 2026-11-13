# Upstream import: 6.18.60-cachyos-lts

Unmodified files from Linux 6.18.60 (cachyos-lts).

- `src/`: the i915 driver and the uapi headers it ships; the VM_BIND stacks apply here.
- `reference/`: kernel headers the driver compiles against, kept for review and
  for the build-time header guard (`reference.sha256`). Never used as include path.
