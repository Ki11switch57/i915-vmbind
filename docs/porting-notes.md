# i915 VM_BIND forward-port notes
Source: https://gitlab.freedesktop.org/nvishwa1/nvishwa1-drm-tip branch drm-tip (23 patches on drm-tip 2022-12-13)
Method: rebase series release-by-release onto mainline tags; build drivers/gpu/drm/i915 at each step.

- v6.2: clean.
- v6.3: i915_gpu_error.c: upstream e8a3319c31a1 moved ring/ctx capture into engine_coredump_add_context(), which runs before capture_user(). Split out capture_context_vmas() so add_request does capture_user (takes vm->mutex) first, then batch, then ring/ctx, preserving the patch's lock-ordering intent.
- v6.3: i915_vma.c needs i915_config.h for i915_fence_timeout() (ff1e93e924f2 header split).
- v6.5: **uAPI renumber**: upstream took I915_GEM_CREATE_EXT 2 for SET_PAT (MTL PAT index). Moved I915_GEM_CREATE_EXT_VM_PRIVATE 2 -> 3. Breaks ABI with any userspace built against the old series.
- (pre-existing in original series, not fixed) i915_gem_create_ext_ioctl leaks the vm ref on the NEEDS_CPU_ACCESS -EINVAL early returns.
- v6.5: **uAPI renumber**: I915_PARAM 58 taken by PXP_STATUS; I915_PARAM_VM_BIND_VERSION 58 -> 59.
- v6.6: execbuf common split: ported f56fe3e91787 (MTL GT0 pm ref vs i915_vma_parked UAF) into i915_eb_select_engine()/i915_eb_put_engine() in i915_gem_execbuffer_common.c.
- v6.8: 5e4e06e4087e (track gt pm wakerefs): changed i915_eb_select_engine()/i915_eb_put_engine() to return/take intel_wakeref_t for the tile and GT0; execbuf and execbuf3 store them in their eb structs.
- v6.9: 86ceaaaec597 removed i915->mm.notifier_lock and made the userptr mmu-notifier non-blocking (no dma_resv wait; pages stay pinned until next use). Kept the series' persistent-vma queueing onto vm->userptr_invalidated_list in the notifier (spinlocks only, non-blocking), dropped the wait, and dropped notifier_lock read-locking around userptr_submit_done() in execbuf3 and vm_bind (matches upstream execbuf2).
- v6.10: **uAPI renumber**: I915_PARAM 59 taken by HAS_CONTEXT_FREQ_HINT; I915_PARAM_VM_BIND_VERSION 59 -> 60.
- v6.18: i915_vma.c err_fence: combined vm_bind out-fence signalling with 84bbe327a5cb (async commit for GGTT on CHV/BXT+VTD).
- v6.19: header diet: i915_gem_dmabuf.c, i915_gem_execbuffer_common.c, i915_gem_vm_bind_object.c need <drm/drm_print.h> for drm_dbg().
- v7.0: i915_fence_timeout() lost its i915 arg upstream; adapted persistent-vma evict await. Makefile: gemfs removed upstream. kmalloc_array->kmalloc_objs in execbuf.c conflict (took series' common helper).
- v7.2: dma_fence_array_create() dropped signal_on_any arg; adapted i915_eb_composite_fence_create().
- v7.3: d2da6696e0c4 moved dma_resv_fini to i915_gem_object_free (RCU). Kept priv_root put in __i915_gem_object_fini; root's resv fini still happens after an RCU grace period queued after the put, so RCU readers of a private BO's (shared) resv stay safe.
- master (v7.4-rc2+): 468a755f203e i915_gtt_view->intel_gtt_view (+I915_GTT_VIEW_*->INTEL_GTT_VIEW_*) sed applied to series; __i915_vma_get_pages switch->if/else helpers, kept persistent partial pages branch. 31643dc89818 (reset err per fence) already inherent in i915_eb_add_timeline_fence().
- audit: execbuf3 batch->node.start -> i915_vma_offset(batch) per 8e4ee5e87ce6 (no behavior change, persistent vmas have no guard). Other execbuf2 fixes since base were either legacy-only (relocs, exec array, BSD select, vma/ctx VM check) or already merged via conflicts.

## Result of the mainline port
- Base: mainline ee83899302a0 (v7.4-rc2-205). 23 patches, same 32 files as the original series.
- Every commit builds (drivers/gpu/drm/i915, CONFIG_DRM_I915=m, DEBUG_GEM, USERPTR, CAPTURE_ERROR); tip also builds with DRM_I915_SELFTEST. No unresolved series symbols in i915.o.
- Not done: runtime/hardware testing, IGT, checkpatch.

## Out-of-tree lines (i915-vmbind 1.0.0)
- Lines: 6.18 (v6.18.63, CachyOS LTS 6.18.60), 7.3 (v7.3.4, CachyOS 7.3.1/7.3.3), 7.4 (mainline ee83899302a0). One patch set per line; all 27 patches apply to every target of a line unchanged.
- 6.18: i915_gem_object.c cleanup path conflict in "Support for VM private BOs" (keep both dma_resv_fini and the priv_root put).
- The renumbers above (create-ext 3, param 60) are superseded by the private ranges in 30-safety (create-ext 0x20, params 0x5642xxxx, ioctls 0x5d-0x5f, VM-create flag bit 31); see docs/abi.md.
- 20-fixes: the gem_create_ext VM reference leak noted above is fixed.
