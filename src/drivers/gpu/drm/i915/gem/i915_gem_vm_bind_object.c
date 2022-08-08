// SPDX-License-Identifier: MIT
/*
 * Copyright © 2022 Intel Corporation
 */

#include <uapi/drm/i915_drm.h>

#include <linux/interval_tree_generic.h>

#include <drm/drm_syncobj.h>

#include "gem/i915_gem_context.h"
#include "gem/i915_gem_vm_bind.h"

#include "gt/intel_gpu_commands.h"

#define START(node) ((node)->start)
#define LAST(node) ((node)->last)

/* Not all defined functions are used, hence use __maybe_unused */
INTERVAL_TREE_DEFINE(struct i915_vma, rb, u64, __subtree_last,
		     START, LAST, __maybe_unused static inline, i915_vm_bind_it)

#undef START
#undef LAST

/**
 * DOC: VM_BIND/UNBIND ioctls
 *
 * DRM_I915_GEM_VM_BIND/UNBIND ioctls allows UMD to bind/unbind GEM buffer
 * objects (BOs) or sections of a BOs at specified GPU virtual addresses on a
 * specified address space (VM). Multiple mappings can map to the same physical
 * pages of an object (aliasing). These mappings (also referred to as persistent
 * mappings) will be persistent across multiple GPU submissions (execbuf calls)
 * issued by the UMD, without user having to provide a list of all required
 * mappings during each submission (as required by older execbuf mode).
 *
 * The VM_BIND/UNBIND calls allow UMDs to request a timeline out fence for
 * signaling the completion of bind/unbind operation.
 *
 * VM_BIND feature is advertised to user via I915_PARAM_VM_BIND_VERSION.
 * User has to opt-in for VM_BIND mode of binding for an address space (VM)
 * during VM creation time via I915_VM_CREATE_FLAGS_USE_VM_BIND extension.
 *
 * VM_BIND/UNBIND ioctl calls executed on different CPU threads concurrently
 * are not ordered. Furthermore, parts of the VM_BIND/UNBIND operations can be
 * done asynchronously, when valid out fence is specified.
 *
 * VM_BIND locking order is as below.
 *
 * 1) vm_bind_lock mutex will protect vm_bind lists. This lock is taken in
 *    vm_bind/vm_unbind ioctl calls, in the execbuf path and while releasing the
 *    mapping.
 *
 *    In future, when GPU page faults are supported, we can potentially use a
 *    rwsem instead, so that multiple page fault handlers can take the read
 *    side lock to lookup the mapping and hence can run in parallel.
 *    The older execbuf mode of binding do not need this lock.
 *
 * 2) The object's dma-resv lock will protect i915_vma state and needs
 *    to be held while binding/unbinding a vma in the async worker and while
 *    updating dma-resv fence list of an object. Note that private BOs of a VM
 *    will all share a dma-resv object.
 *
 * 3) Spinlock/s to protect some of the VM's lists like the list of
 *    invalidated vmas (due to eviction and userptr invalidation) etc.
 */

/**
 * i915_gem_vm_bind_lookup_vma() - lookup for persistent vma mapped at a
 * specified address
 * @vm: virtual address space to look for persistent vma
 * @va: starting address where vma is mapped
 *
 * Retrieves the persistent vma mapped address @va from the @vm's vma tree.
 *
 * Returns vma pointer on success, NULL on failure.
 */
struct i915_vma *
i915_gem_vm_bind_lookup_vma(struct i915_address_space *vm, u64 va)
{
	lockdep_assert_held(&vm->vm_bind_lock);

	return i915_vm_bind_it_iter_first(&vm->va, va, va);
}

static void i915_gem_vm_bind_remove(struct i915_vma *vma, bool release_obj)
{
	lockdep_assert_held(&vma->vm->vm_bind_lock);

	spin_lock(&vma->vm->vm_rebind_lock);
	if (!list_empty(&vma->vm_rebind_link))
		list_del_init(&vma->vm_rebind_link);
	i915_vma_set_purged(vma);
	spin_unlock(&vma->vm->vm_rebind_lock);

	list_del_init(&vma->vm_bind_link);
	list_del_init(&vma->non_priv_vm_bind_link);
	i915_vm_bind_it_remove(vma, &vma->vm->va);

	/* Release object */
	if (release_obj)
		i915_gem_object_put(vma->obj);
}

static int i915_vm_bind_add_fence(struct drm_file *file, struct i915_vma *vma,
				  u32 handle, u64 point)
{
	struct drm_syncobj *syncobj;

	syncobj = drm_syncobj_find(file, handle);
	if (!syncobj) {
		drm_dbg(&vma->vm->i915->drm,
			"Invalid syncobj handle provided\n");
		return -ENOENT;
	}

	/*
	 * For timeline syncobjs we need to preallocate chains for
	 * later signaling.
	 */
	if (point) {
		vma->vm_bind_fence.chain_fence = dma_fence_chain_alloc();
		if (!vma->vm_bind_fence.chain_fence) {
			drm_syncobj_put(syncobj);
			return -ENOMEM;
		}
	} else {
		vma->vm_bind_fence.chain_fence = NULL;
	}
	vma->vm_bind_fence.syncobj = syncobj;
	vma->vm_bind_fence.value = point;

	return 0;
}

static void i915_vm_bind_put_fence(struct i915_vma *vma)
{
	if (!vma->vm_bind_fence.syncobj)
		return;

	drm_syncobj_put(vma->vm_bind_fence.syncobj);
	dma_fence_chain_free(vma->vm_bind_fence.chain_fence);
	vma->vm_bind_fence.syncobj = NULL;
}

/**
 * i915_vm_bind_signal_fence() - Add fence to vm_bind syncobj
 * @vma: vma mapping requiring signaling
 * @fence: fence to be added
 *
 * Associate specified @fence with the @vma's syncobj to be
 * signaled after the @fence work completes.
 */
void i915_vm_bind_signal_fence(struct i915_vma *vma,
			       struct dma_fence * const fence)
{
	struct drm_syncobj *syncobj = vma->vm_bind_fence.syncobj;

	if (!syncobj)
		return;

	if (vma->vm_bind_fence.chain_fence) {
		drm_syncobj_add_point(syncobj,
				      vma->vm_bind_fence.chain_fence,
				      fence, vma->vm_bind_fence.value);
		/*
		 * The chain's ownership is transferred to the
		 * timeline.
		 */
		vma->vm_bind_fence.chain_fence = NULL;
	} else {
		drm_syncobj_replace_fence(syncobj, fence);
	}
}

static int i915_gem_vm_unbind_vma(struct i915_address_space *vm,
				  struct drm_i915_gem_vm_unbind *va)
{
	struct drm_i915_gem_object *obj;
	struct i915_vma *vma;
	int ret;

	ret = mutex_lock_interruptible(&vm->vm_bind_lock);
	if (ret)
		return ret;

	va->start = gen8_noncanonical_addr(va->start);
	vma = i915_gem_vm_bind_lookup_vma(vm, va->start);

	if (!vma)
		ret = -ENOENT;
	else if (vma->size != va->length)
		ret = -EINVAL;

	if (ret) {
		mutex_unlock(&vm->vm_bind_lock);
		return ret;
	}

	i915_gem_vm_bind_remove(vma, false);

	mutex_unlock(&vm->vm_bind_lock);

	/*
	 * Destroy the vma and then release the object.
	 * As persistent vma holds object reference, it can only be destroyed
	 * either by vm_unbind ioctl or when VM is being released. As we are
	 * holding VM reference here, it is safe accessing the vma here.
	 */
	obj = vma->obj;
	i915_gem_object_lock(obj, NULL);
	i915_vma_destroy(vma);
	i915_gem_object_unlock(obj);

	i915_gem_object_put(obj);

	return 0;
}

/**
 * i915_gem_vm_unbind_all() - unbind all persistent mappings from an
 * address space
 * @vm: Address spece to remove persistent mappings from
 *
 * Unbind all userspace requested vm_bind mappings from @vm.
 */
void i915_gem_vm_unbind_all(struct i915_address_space *vm)
{
	struct i915_vma *vma, *t;

	mutex_lock(&vm->vm_bind_lock);
	list_for_each_entry_safe(vma, t, &vm->vm_bind_list, vm_bind_link)
		i915_gem_vm_bind_remove(vma, true);
	list_for_each_entry_safe(vma, t, &vm->vm_bound_list, vm_bind_link)
		i915_gem_vm_bind_remove(vma, true);
	mutex_unlock(&vm->vm_bind_lock);
}

static struct i915_vma *vm_bind_get_vma(struct i915_address_space *vm,
					struct drm_i915_gem_object *obj,
					struct drm_i915_gem_vm_bind *va)
{
	struct i915_gtt_view view;
	struct i915_vma *vma;

	va->start = gen8_noncanonical_addr(va->start);
	vma = i915_gem_vm_bind_lookup_vma(vm, va->start);
	if (vma)
		return ERR_PTR(-EEXIST);

	view.type = I915_GTT_VIEW_PARTIAL;
	view.partial.offset = va->offset >> PAGE_SHIFT;
	view.partial.size = va->length >> PAGE_SHIFT;
	vma = i915_vma_create_persistent(obj, vm, &view);
	if (IS_ERR(vma))
		return vma;

	vma->start = va->start;
	vma->last = va->start + va->length - 1;

	return vma;
}

static int i915_gem_vm_bind_obj(struct i915_address_space *vm,
				struct drm_i915_gem_vm_bind *va,
				struct drm_file *file)
{
	struct drm_i915_gem_object *obj;
	struct i915_vma *vma = NULL;
	struct i915_gem_ww_ctx ww;
	u64 pin_flags;
	int ret = 0;

	if (!i915_gem_vm_is_vm_bind_mode(vm))
		return -EOPNOTSUPP;

	/* Ensure start and length fields are valid */
	if (!va->length || !IS_ALIGNED(va->start, I915_GTT_PAGE_SIZE))
		ret = -EINVAL;

	/* In fences are not supported */
	if ((va->fence.flags & I915_TIMELINE_FENCE_WAIT) ||
	    (va->fence.flags & __I915_TIMELINE_FENCE_UNKNOWN_FLAGS))
		ret = -EINVAL;

	obj = i915_gem_object_lookup(file, va->handle);
	if (!obj)
		return -ENOENT;

	/* Ensure offset and length are aligned to object's max page size */
	if (!IS_ALIGNED(va->offset | va->length,
			i915_gem_object_max_page_size(obj->mm.placements,
						      obj->mm.n_placements)))
		ret = -EINVAL;

	/* Check for mapping range overflow */
	if (range_overflows_t(u64, va->offset, va->length, obj->base.size))
		ret = -EINVAL;

	if (ret)
		goto put_obj;

	if (obj->priv_root && obj->priv_root != vm->root_obj) {
		ret = -EINVAL;
		goto put_obj;
	}

	if (i915_gem_object_is_userptr(obj)) {
		ret = i915_gem_object_userptr_submit_init(obj);
		if (ret)
			goto put_obj;
	}

	ret = mutex_lock_interruptible(&vm->vm_bind_lock);
	if (ret)
		goto put_obj;

	vma = vm_bind_get_vma(vm, obj, va);
	if (IS_ERR(vma)) {
		ret = PTR_ERR(vma);
		goto unlock_vm;
	}

	if (va->fence.flags & I915_TIMELINE_FENCE_SIGNAL) {
		ret = i915_vm_bind_add_fence(file, vma, va->fence.handle,
					     va->fence.value);
		if (ret)
			goto put_vma;
	}

	pin_flags = va->start | PIN_OFFSET_FIXED | PIN_USER |
		    PIN_VALIDATE | PIN_NOEVICT;

	for_i915_gem_ww(&ww, ret, true) {
		ret = i915_gem_object_lock(vma->obj, &ww);
		if (ret)
			continue;

		ret = i915_vma_pin_ww(vma, &ww, 0, 0, pin_flags);
		if (ret)
			continue;

#ifdef CONFIG_MMU_NOTIFIER
		if (i915_gem_object_is_userptr(obj)) {
			ret = i915_gem_object_userptr_submit_done(obj);
			if (ret)
				continue;
		}
#endif
		/* If out fence is not requested, wait for bind to complete */
		if (!(va->fence.flags & I915_TIMELINE_FENCE_SIGNAL)) {
			ret = i915_vma_wait_for_bind(vma);
			if (ret)
				continue;
		}

		list_add_tail(&vma->vm_bind_link, &vm->vm_bound_list);
		i915_vm_bind_it_insert(vma, &vm->va);
		if (!obj->priv_root)
			list_add_tail(&vma->non_priv_vm_bind_link,
				      &vm->non_priv_vm_bind_list);

		/* Hold object reference until vm_unbind */
		i915_gem_object_get(vma->obj);
	}

	if (va->fence.flags & I915_TIMELINE_FENCE_SIGNAL)
		i915_vm_bind_put_fence(vma);
put_vma:
	if (ret)
		i915_vma_destroy(vma);
unlock_vm:
	mutex_unlock(&vm->vm_bind_lock);
put_obj:
	i915_gem_object_put(obj);

	return ret;
}

/**
 * i915_gem_vm_bind_ioctl() - ioctl function for binding a section of object
 * at a specified virtual address
 * @dev: drm_device pointer
 * @data: ioctl data structure
 * @file: drm_file pointer
 *
 * Adds the specified persistent mapping (virtual address to a section of an
 * object) and binds it in the device page table.
 *
 * Returns 0 on success, error code on failure.
 */
int i915_gem_vm_bind_ioctl(struct drm_device *dev, void *data,
			   struct drm_file *file)
{
	struct drm_i915_gem_vm_bind *args = data;
	struct i915_address_space *vm;
	int ret;

	/* Reserved fields must be 0 */
	if ((args->flags & __I915_GEM_VM_BIND_UNKNOWN_FLAGS) ||
	    args->rsvd[0] || args->rsvd[1] || args->extensions)
		return -EINVAL;

	vm = i915_gem_vm_lookup(file->driver_priv, args->vm_id);
	if (unlikely(!vm))
		return -ENOENT;

	ret = i915_gem_vm_bind_obj(vm, args, file);

	i915_vm_put(vm);
	return ret;
}

/**
 * i915_gem_vm_unbind_ioctl() - ioctl function for unbinding a mapping at a
 * specified virtual address
 * @dev: drm_device pointer
 * @data: ioctl data structure
 * @file: drm_file pointer
 *
 * Removes the persistent mapping at the specified address and unbinds it
 * from the device page table.
 *
 * Returns 0 on success, error code on failure. -ENOENT is returned if the
 * specified mapping is not found.
 */
int i915_gem_vm_unbind_ioctl(struct drm_device *dev, void *data,
			     struct drm_file *file)
{
	struct drm_i915_gem_vm_unbind *args = data;
	struct i915_address_space *vm;
	int ret;

	/* Reserved fields must be 0 */
	if ((args->flags & __I915_GEM_VM_UNBIND_UNKNOWN_FLAGS) ||
	    args->pad || args->rsvd[0] || args->rsvd[1] || args->extensions)
		return -EINVAL;

	vm = i915_gem_vm_lookup(file->driver_priv, args->vm_id);
	if (unlikely(!vm))
		return -ENOENT;

	ret = i915_gem_vm_unbind_vma(vm, args);

	i915_vm_put(vm);
	return ret;
}
