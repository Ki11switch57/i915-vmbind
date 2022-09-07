// SPDX-License-Identifier: MIT
/*
 * Copyright © 2022 Intel Corporation
 */

#include <linux/dma-fence-array.h>

#include <drm/drm_print.h>
#include <drm/drm_syncobj.h>

#include "gt/intel_context.h"
#include "gt/intel_gt.h"
#include "gt/intel_gt_pm.h"
#include "gt/intel_ring.h"

#include "i915_drv.h"
#include "i915_gem_execbuffer_common.h"

#define __EXEC_COMMON_FENCE_WAIT	BIT(0)
#define __EXEC_COMMON_FENCE_SIGNAL	BIT(1)

static struct i915_request *eb_throttle(struct intel_context *ce)
{
	struct intel_ring *ring = ce->ring;
	struct intel_timeline *tl = ce->timeline;
	struct i915_request *rq;

	/*
	 * Completely unscientific finger-in-the-air estimates for suitable
	 * maximum user request size (to avoid blocking) and then backoff.
	 */
	if (intel_ring_update_space(ring) >= PAGE_SIZE)
		return NULL;

	/*
	 * Find a request that after waiting upon, there will be at least half
	 * the ring available. The hysteresis allows us to compete for the
	 * shared ring and should mean that we sleep less often prior to
	 * claiming our resources, but not so long that the ring completely
	 * drains before we can submit our next request.
	 */
	list_for_each_entry(rq, &tl->requests, link) {
		if (rq->ring != ring)
			continue;

		if (__intel_ring_space(rq->postfix,
				       ring->emit, ring->size) > ring->size / 2)
			break;
	}
	if (&rq->link == &tl->requests)
		return NULL; /* weird, we will check again later for real */

	return i915_request_get(rq);
}

static int eb_pin_timeline(struct intel_context *ce, bool throttle,
			   bool nonblock)
{
	struct intel_timeline *tl;
	struct i915_request *rq = NULL;

	/*
	 * Take a local wakeref for preparing to dispatch the execbuf as
	 * we expect to access the hardware fairly frequently in the
	 * process, and require the engine to be kept awake between accesses.
	 * Upon dispatch, we acquire another prolonged wakeref that we hold
	 * until the timeline is idle, which in turn releases the wakeref
	 * taken on the engine, and the parent device.
	 */
	tl = intel_context_timeline_lock(ce);
	if (IS_ERR(tl))
		return PTR_ERR(tl);

	intel_context_enter(ce);
	if (throttle)
		rq = eb_throttle(ce);
	intel_context_timeline_unlock(tl);

	if (rq) {
		long timeout = nonblock ? 0 : MAX_SCHEDULE_TIMEOUT;

		if (i915_request_wait(rq, I915_WAIT_INTERRUPTIBLE,
				      timeout) < 0) {
			i915_request_put(rq);

			/*
			 * Error path, cannot use intel_context_timeline_lock as
			 * that is user interruptable and this clean up step
			 * must be done.
			 */
			mutex_lock(&ce->timeline->mutex);
			intel_context_exit(ce);
			mutex_unlock(&ce->timeline->mutex);

			if (nonblock)
				return -EWOULDBLOCK;
			else
				return -EINTR;
		}
		i915_request_put(rq);
	}

	return 0;
}

/**
 * i915_eb_pin_engine() - Pin the engine
 * @ce: the context
 * @ww: optional locking context or NULL
 * @throttle: throttle to ensure enough ring space
 * @nonblock: do not block during throttle
 *
 * Pin the @ce timeline. If @throttle is set, enable throttling to ensure
 * enough ring space is available either by waiting for requests to complete
 * (if @nonblock is not set) or by returning error -EWOULDBLOCK (if @nonblock
 * is set).
 *
 * Returns 0 upon success, -ve error code upon error.
 */
int i915_eb_pin_engine(struct intel_context *ce, struct i915_gem_ww_ctx *ww,
		       bool throttle, bool nonblock)
{
	struct intel_context *child;
	int err;
	int i = 0, j = 0;

	if (unlikely(intel_context_is_banned(ce)))
		return -EIO;

	/*
	 * Pinning the contexts may generate requests in order to acquire
	 * GGTT space, so do this first before we reserve a seqno for
	 * ourselves.
	 */
	err = intel_context_pin_ww(ce, ww);
	if (err)
		return err;

	for_each_child(ce, child) {
		err = intel_context_pin_ww(child, ww);
		GEM_BUG_ON(err);	/* perma-pinned should incr a counter */
	}

	for_each_child(ce, child) {
		err = eb_pin_timeline(child, throttle, nonblock);
		if (err)
			goto unwind;
		++i;
	}
	err = eb_pin_timeline(ce, throttle, nonblock);
	if (err)
		goto unwind;

	return 0;

unwind:
	for_each_child(ce, child) {
		if (j++ < i) {
			mutex_lock(&child->timeline->mutex);
			intel_context_exit(child);
			mutex_unlock(&child->timeline->mutex);
		}
	}
	for_each_child(ce, child)
		intel_context_unpin(child);
	intel_context_unpin(ce);
	return err;
}

/**
 * i915_eb_unpin_engine() - Unpin the engine
 * @ce: the context
 *
 * Unpin the @ce timeline.
 */
void i915_eb_unpin_engine(struct intel_context *ce)
{
	struct intel_context *child;

	for_each_child(ce, child) {
		mutex_lock(&child->timeline->mutex);
		intel_context_exit(child);
		mutex_unlock(&child->timeline->mutex);

		intel_context_unpin(child);
	}

	mutex_lock(&ce->timeline->mutex);
	intel_context_exit(ce);
	mutex_unlock(&ce->timeline->mutex);

	intel_context_unpin(ce);
}

/**
 * i915_eb_find_context() - Find the context
 * @context: the context
 * @context_number: required context index
 *
 * Returns the @context_number'th child of specified @context,
 * or NULL if the child context is not found.
 * If @context_number is 0, return the specified @context.
 */
struct intel_context *
i915_eb_find_context(struct intel_context *context, unsigned int context_number)
{
	struct intel_context *child;

	if (likely(context_number == 0))
		return context;

	for_each_child(context, child)
		if (!--context_number)
			return child;

	GEM_BUG_ON("Context not found");

	return NULL;
}

static void __free_fence_array(struct eb_fence *fences, u64 n)
{
	while (n--) {
		drm_syncobj_put(ptr_mask_bits(fences[n].syncobj, 2));
		dma_fence_put(fences[n].dma_fence);
		dma_fence_chain_free(fences[n].chain_fence);
	}
	kvfree(fences);
}

/**
 * i915_eb_put_fence_array() - Free Execbuffer fence array
 * @fences: Pointer to array of Execbuffer fences (See struct eb_fences)
 * @num_fences: Number of fences in @fences array
 *
 * Free the Execbuffer fences in @fences array.
 */
void i915_eb_put_fence_array(struct eb_fence *fences, u64 num_fences)
{
	if (fences)
		__free_fence_array(fences, num_fences);
}

/**
 * i915_eb_add_timeline_fence() - Add a fence to the specified Execbuffer fence
 * array.
 * @i915: i915 private
 * @file: drm file pointer
 * @handle: drm_syncobj handle
 * @point: point in the timeline
 * @f: Execbuffer fence
 * @wait: wait for the specified fence
 * @signal: signal the specified fence
 *
 * Add the fence specified by drm_syncobj @handle at specified @point in the
 * timeline to the Execbuffer fence array @f. If @wait is specified, it is an
 * input fence and if @signal is specified it is an output fence.
 *
 * Returns 0 if the timeline fence to be added is already signaled (@f is not
 * updated). Returns 1 upon successfully adding the timeline fence (@f is
 * updated) and -ve error code upon failure.
 */
int i915_eb_add_timeline_fence(struct drm_i915_private *i915,
			       struct drm_file *file, u32 handle, u64 point,
			       struct eb_fence *f, bool wait, bool signal)
{
	struct drm_syncobj *syncobj;
	struct dma_fence *fence = NULL;
	u32 flags = 0;
	int err = 0;

	syncobj = drm_syncobj_find(file, handle);
	if (!syncobj) {
		drm_dbg(&i915->drm, "Invalid syncobj handle provided\n");
		return -ENOENT;
	}

	fence = drm_syncobj_fence_get(syncobj);

	if (!fence && wait && !signal) {
		drm_dbg(&i915->drm, "Syncobj handle has no fence\n");
		drm_syncobj_put(syncobj);
		return -EINVAL;
	}

	if (fence)
		err = dma_fence_chain_find_seqno(&fence, point);

	if (err && !signal) {
		drm_dbg(&i915->drm,
			"Syncobj handle missing requested point %llu\n", point);
		dma_fence_put(fence);
		drm_syncobj_put(syncobj);
		return err;
	}

	/*
	 * A point might have been signaled already and
	 * garbage collected from the timeline. In this case
	 * just ignore the point and carry on.
	 */
	if (!fence && !signal) {
		drm_syncobj_put(syncobj);
		return 0;
	}

	/*
	 * For timeline syncobjs we need to preallocate chains for
	 * later signaling.
	 */
	if (point != 0 && signal) {
		/*
		 * Waiting and signaling the same point (when point !=
		 * 0) would break the timeline.
		 */
		if (wait) {
			drm_dbg(&i915->drm,
				"Trying to wait & signal the same timeline point.\n");
			dma_fence_put(fence);
			drm_syncobj_put(syncobj);
			return -EINVAL;
		}

		f->chain_fence = dma_fence_chain_alloc();
		if (!f->chain_fence) {
			drm_syncobj_put(syncobj);
			dma_fence_put(fence);
			return -ENOMEM;
		}
	} else {
		f->chain_fence = NULL;
	}

	flags |= wait ? __EXEC_COMMON_FENCE_WAIT : 0;
	flags |= signal ? __EXEC_COMMON_FENCE_SIGNAL : 0;

	f->syncobj = ptr_pack_bits(syncobj, flags, 2);
	f->dma_fence = fence;
	f->value = point;
	return 1;
}

/**
 * i915_eb_await_fence_array() - Setup a request to asynchronously
 * wait for fences in the specified Execbuffer fence array.
 * @fences: pointer to Execbuffer fence array
 * @num_fences: number of fences in @fences array
 * @rq: the i915_request that should wait for fences in @fences array
 *
 * Setup the request @rq to asynchronously wait for fences specified in
 * @fences array to signal before starting execution.
 *
 * Returns 0 upon success, -ve error upon failure.
 */
int i915_eb_await_fence_array(struct eb_fence *fences, u64 num_fences,
			      struct i915_request *rq)
{
	unsigned int n;

	for (n = 0; n < num_fences; n++) {
		int err;

		if (!fences[n].dma_fence)
			continue;

		err = i915_request_await_dma_fence(rq, fences[n].dma_fence);
		if (err < 0)
			return err;
	}

	return 0;
}

/**
 * i915_eb_signal_fence_array() - Attach a dma-fence to all out fences of
 * Execbuffer fence array.
 * @fences: pointer to Execbuffer fence array
 * @num_fences: number of fences in @fences array
 * @fence: the dma-fence to attach to all out fences in @fences array
 *
 * Attach the specified @fence to all out fences of Execbuffer fence array
 * @fences, at the specified timeline point. Thus, the out fences gets
 * signaled when the specified @fence gets signaled.
 */
void i915_eb_signal_fence_array(struct eb_fence *fences, u64 num_fences,
				struct dma_fence * const fence)
{
	unsigned int n;

	for (n = 0; n < num_fences; n++) {
		struct drm_syncobj *syncobj;
		unsigned int flags;

		syncobj = ptr_unpack_bits(fences[n].syncobj, &flags, 2);
		if (!(flags & __EXEC_COMMON_FENCE_SIGNAL))
			continue;

		if (fences[n].chain_fence) {
			drm_syncobj_add_point(syncobj,
					      fences[n].chain_fence,
					      fence,
					      fences[n].value);
			/*
			 * The chain's ownership is transferred to the
			 * timeline.
			 */
			fences[n].chain_fence = NULL;
		} else {
			drm_syncobj_replace_fence(syncobj, fence);
		}
	}
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
#define for_each_batch_create_order(_num_batches) \
	for (unsigned int i = 0; i < (_num_batches); ++i)
#define for_each_batch_add_order(_num_batches) \
	for (int i = (_num_batches) - 1; i >= 0; --i)

static void retire_requests(struct intel_timeline *tl, struct i915_request *end)
{
	struct i915_request *rq, *rn;

	list_for_each_entry_safe(rq, rn, &tl->requests, link)
		if (rq == end || !i915_request_retire(rq))
			break;
}

static int eb_request_add(struct intel_context *context,
			  struct i915_request *rq,
			  struct i915_sched_attr sched,
			  int err, bool last_parallel)
{
	struct intel_timeline * const tl = i915_request_timeline(rq);
	struct i915_sched_attr attr = {};
	struct i915_request *prev;

	lockdep_assert_held(&tl->mutex);
	lockdep_unpin_lock(&tl->mutex, rq->cookie);

	trace_i915_request_add(rq);

	prev = __i915_request_commit(rq);

	/* Check that the context wasn't destroyed before submission */
	if (likely(!intel_context_is_closed(context))) {
		attr = sched;
	} else {
		/* Serialise with context_close via the add_to_timeline */
		i915_request_set_error_once(rq, -ENOENT);
		__i915_request_skip(rq);
		err = -ENOENT; /* override any transient errors */
	}

	if (intel_context_is_parallel(context)) {
		if (err) {
			__i915_request_skip(rq);
			set_bit(I915_FENCE_FLAG_SKIP_PARALLEL,
				&rq->fence.flags);
		}
		if (last_parallel)
			set_bit(I915_FENCE_FLAG_SUBMIT_PARALLEL,
				&rq->fence.flags);
	}

	__i915_request_queue(rq, &attr);

	/* Try to clean up the client's timeline after submitting the request */
	if (prev)
		retire_requests(tl, prev);

	mutex_unlock(&tl->mutex);

	return err;
}

/**
 * i915_eb_requests_add() - Handle request queuing
 * @requests: pointer to an array of request pointers
 * @num_requests: size of @requests array
 * @context: the context
 * @sched: schedule attribute
 * @err: error path if true
 *
 * Add requests to timeline queue.
 *
 * Return 0 upon success, error code upon failure.
 */
int i915_eb_requests_add(struct i915_request **requests,
			 unsigned int num_requests,
			 struct intel_context *context,
			 struct i915_sched_attr sched,
			 int err)
{
	/*
	 * We iterate in reverse order of creation to release timeline mutexes
	 * in same order.
	 */
	for_each_batch_add_order(num_requests) {
		struct i915_request *rq = requests[i];

		if (!rq)
			continue;

		err |= eb_request_add(context, rq, sched, err, i == 0);
	}

	return err;
}

/**
 * i915_eb_requests_get() - Get reference of requests
 * @requests: pointer to an array of request pointers
 * @num_requests: size of @requests array
 *
 * Get reference for each requests in @requests array.
 */
void i915_eb_requests_get(struct i915_request **requests,
			  unsigned int num_requests)
{
	for_each_batch_create_order(num_requests) {
		if (!requests[i])
			break;

		i915_request_get(requests[i]);
	}
}

/**
 * i915_eb_requests_put() - Release reference of requests
 * @requests: pointer to an array of request pointers
 * @num_requests: size of @requests array
 *
 * Release reference for each requests in @requests array.
 */
void i915_eb_requests_put(struct i915_request **requests,
			  unsigned int num_requests)
{
	for_each_batch_create_order(num_requests) {
		if (!requests[i])
			break;

		i915_request_put(requests[i]);
	}
}

/**
 * i915_eb_composite_fence_create() - Create a composite fence for an array of
 * requests on a specified context.
 * @requests: pointer to an array of request pointers
 * @num_requests: size of @requests array
 * @context: the context
 *
 * Create and return the base class of a dma_fence_array with fences of all
 * requests in @requests array with fence context specified by @context.
 *
 * Returns fence array base upon success, an error pointer upon failure.
 */
struct dma_fence *i915_eb_composite_fence_create(struct i915_request **requests,
						 unsigned int num_requests,
						 struct intel_context *context)
{
	struct dma_fence_array *fence_array;
	struct dma_fence **fences;

	GEM_BUG_ON(!intel_context_is_parent(context));

	fences = kmalloc_array(num_requests, sizeof(*fences), GFP_KERNEL);
	if (!fences)
		return ERR_PTR(-ENOMEM);

	for_each_batch_create_order(num_requests) {
		fences[i] = &requests[i]->fence;
		__set_bit(I915_FENCE_FLAG_COMPOSITE,
			  &requests[i]->fence.flags);
	}

	fence_array = dma_fence_array_create(num_requests,
					     fences,
					     context->parallel.fence_context,
					     context->parallel.seqno++);
	if (!fence_array) {
		kfree(fences);
		return ERR_PTR(-ENOMEM);
	}

	/* Move ownership to the dma_fence_array created above */
	for_each_batch_create_order(num_requests)
		dma_fence_get(fences[i]);

	return &fence_array->base;
}

/**
 * i915_eb_select_engine() - Get engine references
 * @ce: the context
 *
 * Get reference of context @ce and children, reference of associated VM
 * and wakeref of associated tile. Also allocate @ce resources.
 *
 * Returns 0 upon success, -ve error upon failure.
 * Returns -EIO if the associated tile is wedged.
 */
int i915_eb_select_engine(struct intel_context *ce)
{
	struct intel_context *child;
	int err;

	for_each_child(ce, child)
		intel_context_get(child);
	intel_gt_pm_get(ce->engine->gt);

	if (!test_bit(CONTEXT_ALLOC_BIT, &ce->flags)) {
		err = intel_context_alloc_state(ce);
		if (err)
			goto err;
	}
	for_each_child(ce, child) {
		if (!test_bit(CONTEXT_ALLOC_BIT, &child->flags)) {
			err = intel_context_alloc_state(child);
			if (err)
				goto err;
		}
	}

	/*
	 * ABI: Before userspace accesses the GPU (e.g. execbuffer), report
	 * EIO if the GPU is already wedged.
	 */
	err = intel_gt_terminally_wedged(ce->engine->gt);
	if (err)
		goto err;

	if (!i915_vm_tryget(ce->vm)) {
		err = -ENOENT;
		goto err;
	}

	return 0;
err:
	intel_gt_pm_put(ce->engine->gt);
	for_each_child(ce, child)
		intel_context_put(child);
	return err;
}

/**
 * i915_eb_put_engine() - Release engine references
 * @ce: the context
 *
 * Release reference of context @ce and children, reference of associated VM
 * and wakeref of associated tile.
 */
void i915_eb_put_engine(struct intel_context *ce)
{
	struct intel_context *child;

	i915_vm_put(ce->vm);
	intel_gt_pm_put(ce->engine->gt);
	for_each_child(ce, child)
		intel_context_put(child);
	intel_context_put(ce);
}
