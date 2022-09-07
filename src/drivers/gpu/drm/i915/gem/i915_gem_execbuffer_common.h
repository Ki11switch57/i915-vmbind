/* SPDX-License-Identifier: MIT */
/*
 * Copyright © 2022 Intel Corporation
 */

#ifndef __I915_GEM_EXECBUFFER_COMMON_H
#define __I915_GEM_EXECBUFFER_COMMON_H

#include <linux/types.h>

struct dma_fence;
struct dma_fence_chain;
struct drm_file;
struct drm_syncobj;

struct drm_i915_private;
struct intel_context;
struct intel_gt;
struct i915_gem_ww_ctx;
struct i915_request;
struct i915_sched_attr;

/**
 * struct eb_fence - Execbuffer fence
 *
 * Data structure for execbuffer timeline fence handling.
 */
struct eb_fence {
	/** @syncobj: Pointer to user specified syncobj */
	struct drm_syncobj *syncobj;

	/** @dma_fence: Fence associated with @syncobj */
	struct dma_fence *dma_fence;

	/** @value: User specified point in the timeline */
	u64 value;

	/** @chain_fence: Fence chain to add the timeline point */
	struct dma_fence_chain *chain_fence;
};

int i915_eb_pin_engine(struct intel_context *ce, struct i915_gem_ww_ctx *ww,
		       bool throttle, bool nonblock);
void i915_eb_unpin_engine(struct intel_context *ce);
int i915_eb_select_engine(struct intel_context *ce);
void i915_eb_put_engine(struct intel_context *ce);

struct intel_context *
i915_eb_find_context(struct intel_context *context,
		     unsigned int context_number);

int i915_eb_add_timeline_fence(struct drm_i915_private *i915,
			       struct drm_file *file, u32 handle, u64 point,
			       struct eb_fence *f, bool wait, bool signal);
void i915_eb_put_fence_array(struct eb_fence *fences, u64 num_fences);
int i915_eb_await_fence_array(struct eb_fence *fences, u64 num_fences,
			      struct i915_request *rq);
void i915_eb_signal_fence_array(struct eb_fence *fences, u64 num_fences,
				struct dma_fence * const fence);

int i915_eb_requests_add(struct i915_request **requests,
			 unsigned int num_requests,
			 struct intel_context *context,
			 struct i915_sched_attr sched,
			 int err);
void i915_eb_requests_get(struct i915_request **requests,
			  unsigned int num_requests);
void i915_eb_requests_put(struct i915_request **requests,
			  unsigned int num_requests);

struct dma_fence *
i915_eb_composite_fence_create(struct i915_request **requests,
			       unsigned int num_requests,
			       struct intel_context *context);

#endif /* __I915_GEM_EXECBUFFER_COMMON_H */
