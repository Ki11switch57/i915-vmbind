/* SPDX-License-Identifier: MIT */
/*
 * Copyright © 2022 Intel Corporation
 */

#ifndef __I915_GEM_VM_BIND_H
#define __I915_GEM_VM_BIND_H

#include <linux/types.h>

struct drm_device;
struct drm_file;
struct i915_address_space;
struct i915_vma;

struct i915_vma *
i915_gem_vm_bind_lookup_vma(struct i915_address_space *vm, u64 va);

int i915_gem_vm_bind_ioctl(struct drm_device *dev, void *data,
			   struct drm_file *file);
int i915_gem_vm_unbind_ioctl(struct drm_device *dev, void *data,
			     struct drm_file *file);

void i915_gem_vm_unbind_all(struct i915_address_space *vm);

#endif /* __I915_GEM_VM_BIND_H */
