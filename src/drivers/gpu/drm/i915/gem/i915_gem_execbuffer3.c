// SPDX-License-Identifier: MIT
/*
 * Copyright © 2022 Intel Corporation
 */

#include <linux/dma-resv.h>
#include <linux/uaccess.h>

#include <drm/drm_syncobj.h>

#include "gt/intel_context.h"
#include "gt/intel_gpu_commands.h"
#include "gt/intel_gt.h"

#include "i915_drv.h"
#include "i915_gem_context.h"
#include "i915_gem_execbuffer_common.h"
#include "i915_gem_ioctls.h"
#include "i915_gem_vm_bind.h"
#include "i915_trace.h"

#define __EXEC3_ENGINE_PINNED		BIT_ULL(32)
#define __EXEC3_INTERNAL_FLAGS		(~0ull << 32)

/* Catch emission of unexpected errors for CI! */
#if IS_ENABLED(CONFIG_DRM_I915_DEBUG_GEM)
#undef EINVAL
#define EINVAL ({ \
	DRM_DEBUG_DRIVER("EINVAL at %s:%d\n", __func__, __LINE__); \
	22; \
})
#endif

/**
 * DOC: User command execution in vm_bind mode
 *
 * A VM in VM_BIND mode will not support older execbuf mode of binding.
 * The execbuf ioctl handling in VM_BIND mode differs significantly from the
 * older execbuf2 ioctl (See struct drm_i915_gem_execbuffer2).
 * Hence, a new execbuf3 ioctl has been added to support VM_BIND mode. (See
 * struct drm_i915_gem_execbuffer3). The execbuf3 ioctl will not accept any
 * execlist. Hence, no support for implicit sync.
 *
 * The new execbuf3 ioctl only works in VM_BIND mode and the VM_BIND mode only
 * works with execbuf3 ioctl for submission.
 *
 * The execbuf3 ioctl directly specifies the batch addresses instead of as
 * object handles as in execbuf2 ioctl. The execbuf3 ioctl will also not
 * support many of the older features like in/out/submit fences, fence array,
 * default gem context etc. (See struct drm_i915_gem_execbuffer3).
 *
 * In VM_BIND mode, VA allocation is completely managed by the user instead of
 * the i915 driver. Hence all VA assignment, eviction are not applicable in
 * VM_BIND mode. Also, for determining object activeness, VM_BIND mode will not
 * be using the i915_vma active reference tracking. It will instead check the
 * dma-resv object's fence list for that.
 *
 * So, a lot of code supporting execbuf2 ioctl, like relocations, VA evictions,
 * vma lookup table, implicit sync, vma active reference tracking etc., are not
 * applicable for execbuf3 ioctl.
 */

/**
 * struct i915_execbuffer - execbuf struct for execbuf3
 * @i915: reference to the i915 instance we run on
 * @file: drm file reference
 * @args: execbuf3 ioctl structure
 * @gt: reference to the gt instance ioctl submitted for
 * @context: logical state for the request
 * @gem_context: callers context
 * @wakeref: wakeref of @gt
 * @wakeref_gt0: wakeref of GT0 when @gt is not GT0
 * @requests: requests to be build
 * @composite_fence: used for excl fence in dma_resv objects when > 1 BB submitted
 * @ww: i915_gem_ww_ctx instance
 * @num_batches: number of batches submitted
 * @batch_addresses: addresses corresponds to the submitted batches
 * @batches: references to the i915_vmas corresponding to the batches
 * @fences: array of execbuf fences (See struct eb_fence)
 * @num_fences: number of fences in @fences array
 */
struct i915_execbuffer {
	struct drm_i915_private *i915;
	struct drm_file *file;
	struct drm_i915_gem_execbuffer3 *args;

	struct intel_gt *gt;
	struct intel_context *context;
	struct i915_gem_context *gem_context;
	intel_wakeref_t wakeref;
	intel_wakeref_t wakeref_gt0;

	struct i915_request *requests[MAX_ENGINE_INSTANCE + 1];
	struct dma_fence *composite_fence;

	struct i915_gem_ww_ctx ww;

	unsigned int num_batches;
	u64 batch_addresses[MAX_ENGINE_INSTANCE + 1];
	struct i915_vma *batches[MAX_ENGINE_INSTANCE + 1];

	struct eb_fence *fences;
	u64 num_fences;
};

static void eb_unpin_engine(struct i915_execbuffer *eb);

static int eb_select_context(struct i915_execbuffer *eb)
{
	struct i915_gem_context *ctx;

	ctx = i915_gem_context_lookup(eb->file->driver_priv, eb->args->ctx_id);
	if (IS_ERR(ctx))
		return PTR_ERR(ctx);

	if (!i915_gem_vm_is_vm_bind_mode(ctx->vm)) {
		i915_gem_context_put(ctx);
		return -EOPNOTSUPP;
	}

	eb->gem_context = ctx;
	return 0;
}

static struct i915_vma *
eb_find_vma(struct i915_address_space *vm, u64 addr)
{
	u64 va;

	lockdep_assert_held(&vm->vm_bind_lock);

	va = gen8_noncanonical_addr(addr & PIN_OFFSET_MASK);
	return i915_gem_vm_bind_lookup_vma(vm, va);
}

static int eb_lookup_vma_all(struct i915_execbuffer *eb)
{
	struct i915_vma *vma;
	unsigned int i;

	for (i = 0; i < eb->num_batches; i++) {
		vma = eb_find_vma(eb->context->vm, eb->batch_addresses[i]);
		if (!vma)
			return -EINVAL;

		eb->batches[i] = vma;
	}

	return 0;
}

static void eb_release_vma_all(struct i915_execbuffer *eb)
{
	eb_unpin_engine(eb);
}

/*
 * Using two helper loops for the order of which requests / batches are created
 * and added the to backend. Requests are created in order from the parent to
 * the last child. Requests are added in the reverse order, from the last child
 * to parent. This is done for locking reasons as the timeline lock is acquired
 * during request creation and released when the request is added to the
 * backend. To make lockdep happy (see intel_context_timeline_lock) this must be
 * the ordering.
 */
#define for_each_batch_create_order(_eb) \
	for (unsigned int i = 0; i < (_eb)->num_batches; ++i)

static int eb_move_to_gpu(struct i915_execbuffer *eb)
{
	/* Unconditionally flush any chipset caches (for streaming writes). */
	intel_gt_chipset_flush(eb->gt);

	return 0;
}

static int eb_request_submit(struct i915_execbuffer *eb,
			     struct i915_request *rq,
			     struct i915_vma *batch,
			     u64 batch_len)
{
	struct intel_engine_cs *engine = rq->context->engine;
	int err;

	if (intel_context_nopreempt(rq->context))
		__set_bit(I915_FENCE_FLAG_NOPREEMPT, &rq->fence.flags);

	/*
	 * After we completed waiting for other engines (using HW semaphores)
	 * then we can signal that this request/batch is ready to run. This
	 * allows us to determine if the batch is still waiting on the GPU
	 * or actually running by checking the breadcrumb.
	 */
	if (engine->emit_init_breadcrumb) {
		err = engine->emit_init_breadcrumb(rq);
		if (err)
			return err;
	}

	return engine->emit_bb_start(rq, i915_vma_offset(batch), batch_len, 0);
}

static int eb_submit(struct i915_execbuffer *eb)
{
	int err;

	err = eb_move_to_gpu(eb);

	for_each_batch_create_order(eb) {
		if (!eb->requests[i])
			break;

		trace_i915_request_queue(eb->requests[i], 0);
		if (!err)
			err = eb_request_submit(eb, eb->requests[i],
						eb->batches[i],
						eb->batches[i]->size);
	}

	return err;
}

static int eb_pin_engine(struct i915_execbuffer *eb, bool throttle)
{
	int err;

	GEM_BUG_ON(eb->args->flags & __EXEC3_ENGINE_PINNED);

	err = i915_eb_pin_engine(eb->context, &eb->ww, throttle,
				 eb->file->filp->f_flags & O_NONBLOCK);
	if (err)
		return err;

	eb->args->flags |= __EXEC3_ENGINE_PINNED;
	return 0;
}

static void eb_unpin_engine(struct i915_execbuffer *eb)
{
	if (!(eb->args->flags & __EXEC3_ENGINE_PINNED))
		return;

	eb->args->flags &= ~__EXEC3_ENGINE_PINNED;

	i915_eb_unpin_engine(eb->context);
}

static int eb_select_engine(struct i915_execbuffer *eb)
{
	struct intel_context *ce;
	unsigned int idx;
	int err;

	if (!i915_gem_context_user_engines(eb->gem_context))
		return -EINVAL;

	idx = eb->args->engine_idx;
	ce = i915_gem_context_get_engine(eb->gem_context, idx);
	if (IS_ERR(ce))
		return PTR_ERR(ce);

	eb->num_batches = ce->parallel.number_children + 1;

	err = i915_eb_select_engine(ce, &eb->wakeref, &eb->wakeref_gt0);
	if (err)
		goto err;

	eb->context = ce;
	eb->gt = ce->engine->gt;

	/*
	 * Make sure engine pool stays alive even if we call intel_context_put
	 * during ww handling. The pool is destroyed when last pm reference
	 * is dropped, which breaks our -EDEADLK handling.
	 */
	return 0;

err:
	intel_context_put(ce);
	return err;
}

static void eb_put_engine(struct i915_execbuffer *eb)
{
	i915_eb_put_engine(eb->context, eb->wakeref, eb->wakeref_gt0);
}

static int add_timeline_fence_array(struct i915_execbuffer *eb)
{
	struct drm_i915_gem_timeline_fence __user *user_fences;
	struct eb_fence *f;
	u64 nfences;

	nfences = eb->args->fence_count;
	if (!nfences)
		return 0;

	/* Check multiplication overflow for access_ok() and kvmalloc_array() */
	BUILD_BUG_ON(sizeof(size_t) > sizeof(unsigned long));
	if (nfences > min_t(unsigned long,
			    ULONG_MAX / sizeof(*user_fences),
			    SIZE_MAX / sizeof(*f)))
		return -EINVAL;

	user_fences = u64_to_user_ptr(eb->args->timeline_fences);
	if (!access_ok(user_fences, nfences * sizeof(*user_fences)))
		return -EFAULT;

	eb->fences = kcalloc(nfences, sizeof(*f), __GFP_NOWARN | GFP_KERNEL);
	if (!eb->fences)
		return -ENOMEM;

	f = eb->fences;

	BUILD_BUG_ON(~(ARCH_KMALLOC_MINALIGN - 1) &
		     ~__I915_TIMELINE_FENCE_UNKNOWN_FLAGS);

	while (nfences--) {
		struct drm_i915_gem_timeline_fence user_fence;
		bool wait, signal;
		int ret;

		if (__copy_from_user(&user_fence,
				     user_fences++,
				     sizeof(user_fence)))
			return -EFAULT;

		if (user_fence.flags & __I915_TIMELINE_FENCE_UNKNOWN_FLAGS)
			return -EINVAL;

		wait = user_fence.flags & I915_TIMELINE_FENCE_WAIT;
		signal = user_fence.flags & I915_TIMELINE_FENCE_SIGNAL;
		ret = i915_eb_add_timeline_fence(eb->i915, eb->file,
						 user_fence.handle,
						 user_fence.value, f, wait,
						 signal);
		if (ret < 0)
			return ret;
		else if (!ret)
			continue;

		f++;
		eb->num_fences++;
	}

	return 0;
}

static int parse_timeline_fences(struct i915_execbuffer *eb)
{
	return add_timeline_fence_array(eb);
}

static int parse_batch_addresses(struct i915_execbuffer *eb)
{
	struct drm_i915_gem_execbuffer3 *args = eb->args;

	if (eb->num_batches == 1) {
		eb->batch_addresses[0] = args->batch_address;
	} else {
		u64 __user *batch_addr = u64_to_user_ptr(args->batch_address);

		if (copy_from_user(eb->batch_addresses, batch_addr,
				   sizeof(batch_addr[0]) * eb->num_batches))
			return -EFAULT;
	}

	return 0;
}

static int eb_composite_fence_create(struct i915_execbuffer *eb)
{
	struct dma_fence *fence;

	fence = i915_eb_composite_fence_create(eb->requests, eb->num_batches,
					       eb->context);
	if (IS_ERR(fence))
		return PTR_ERR(fence);

	eb->composite_fence = fence;

	return 0;
}

static int eb_fences_add(struct i915_execbuffer *eb, struct i915_request *rq)
{
	int err;

	if (unlikely(eb->gem_context->syncobj)) {
		struct dma_fence *fence;

		fence = drm_syncobj_fence_get(eb->gem_context->syncobj);
		err = i915_request_await_dma_fence(rq, fence);
		dma_fence_put(fence);
		if (err)
			return err;
	}

	if (eb->fences) {
		err = i915_eb_await_fence_array(eb->fences, eb->num_fences, rq);
		if (err)
			return err;
	}

	if (intel_context_is_parallel(eb->context)) {
		err = eb_composite_fence_create(eb);
		if (err)
			return err;
	}

	return 0;
}

static int eb_requests_create(struct i915_execbuffer *eb)
{
	int err;

	for_each_batch_create_order(eb) {
		/* Allocate a request for this batch buffer nice and early. */
		eb->requests[i] =
			i915_request_create(i915_eb_find_context(eb->context,
								 i));
		if (IS_ERR(eb->requests[i])) {
			err = PTR_ERR(eb->requests[i]);
			eb->requests[i] = NULL;
			return err;
		}

		/*
		 * Only the first request added (committed to backend) has to
		 * take the in fences into account as all subsequent requests
		 * will have fences inserted inbetween them.
		 */
		if (i + 1 == eb->num_batches) {
			err = eb_fences_add(eb, eb->requests[i]);
			if (err)
				return err;
		}

		if (eb->batches[i])
			eb->requests[i]->batch_res =
				i915_vma_resource_get(eb->batches[i]->resource);
	}

	return 0;
}

static int
i915_gem_do_execbuffer(struct drm_device *dev,
		       struct drm_file *file,
		       struct drm_i915_gem_execbuffer3 *args)
{
	struct drm_i915_private *i915 = to_i915(dev);
	struct i915_execbuffer eb;
	bool throttle = true;
	int err;

	BUILD_BUG_ON(__EXEC3_INTERNAL_FLAGS & ~__I915_EXEC3_UNKNOWN_FLAGS);

	eb.i915 = i915;
	eb.file = file;
	eb.args = args;

	eb.fences = NULL;
	eb.num_fences = 0;

	memset(eb.requests, 0, sizeof(struct i915_request *) *
	       ARRAY_SIZE(eb.requests));
	eb.composite_fence = NULL;

	err = parse_timeline_fences(&eb);
	if (err)
		return err;

	err = eb_select_context(&eb);
	if (unlikely(err))
		goto err_fences;

	err = eb_select_engine(&eb);
	if (unlikely(err))
		goto err_context;

	err = parse_batch_addresses(&eb);
	if (unlikely(err))
		goto err_engine;

	mutex_lock(&eb.context->vm->vm_bind_lock);

	err = eb_lookup_vma_all(&eb);
	if (err) {
		eb_release_vma_all(&eb);
		goto err_vm_bind_lock;
	}

	i915_gem_ww_ctx_init(&eb.ww, true);

retry_validate:
	err = eb_pin_engine(&eb, throttle);
	if (err)
		goto err_validate;

	/* only throttle once, even if we didn't need to throttle */
	throttle = false;

err_validate:
	if (err == -EDEADLK) {
		eb_release_vma_all(&eb);
		err = i915_gem_ww_ctx_backoff(&eb.ww);
		if (!err)
			goto retry_validate;
	}
	if (err)
		goto err_vma;

	ww_acquire_done(&eb.ww.ctx);

	err = eb_requests_create(&eb);
	if (err) {
		if (eb.requests[0])
			goto err_request;
		else
			goto err_vma;
	}

	err = eb_submit(&eb);

err_request:
	i915_eb_requests_get(eb.requests, eb.num_batches);
	err = i915_eb_requests_add(eb.requests, eb.num_batches, eb.context,
				   eb.gem_context->sched, err);

	if (eb.fences)
		i915_eb_signal_fence_array(eb.fences, eb.num_fences,
					   eb.composite_fence ?
					   eb.composite_fence :
					   &eb.requests[0]->fence);

	if (unlikely(eb.gem_context->syncobj)) {
		drm_syncobj_replace_fence(eb.gem_context->syncobj,
					  eb.composite_fence ?
					  eb.composite_fence :
					  &eb.requests[0]->fence);
	}

	if (eb.composite_fence)
		dma_fence_put(eb.composite_fence);

	i915_eb_requests_put(eb.requests, eb.num_batches);

err_vma:
	eb_release_vma_all(&eb);
	WARN_ON(err == -EDEADLK);
	i915_gem_ww_ctx_fini(&eb.ww);
err_vm_bind_lock:
	mutex_unlock(&eb.context->vm->vm_bind_lock);
err_engine:
	eb_put_engine(&eb);
err_context:
	i915_gem_context_put(eb.gem_context);
err_fences:
	i915_eb_put_fence_array(eb.fences, eb.num_fences);
	return err;
}

int
i915_gem_execbuffer3_ioctl(struct drm_device *dev, void *data,
			   struct drm_file *file)
{
	struct drm_i915_gem_execbuffer3 *args = data;
	int err;

	/* Reserved fields must be 0 */
	if (args->rsvd || args->extensions)
		return -EINVAL;

	if (args->flags & __I915_EXEC3_UNKNOWN_FLAGS)
		return -EINVAL;

	err = i915_gem_do_execbuffer(dev, file, args);

	args->flags &= ~__I915_EXEC3_UNKNOWN_FLAGS;
	return err;
}
