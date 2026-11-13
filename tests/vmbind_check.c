// SPDX-License-Identifier: MIT
// Smoke test for the i915 VM_BIND module: version and ABI query, VM_BIND VM
// create, BO bind/unbind, and one EXECBUFFER3 submission of an empty batch.
// Usage: vmbind_check [/dev/dri/renderD128]
#include <errno.h>
#include <stdint.h>
#include <sys/mman.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <sys/ioctl.h>
#include <unistd.h>
#include "drm/i915_drm.h"

static int xioctl(int fd, unsigned long req, void *arg)
{
	int r;
	do r = ioctl(fd, req, arg); while (r == -1 && (errno == EINTR || errno == EAGAIN));
	return r;
}

/* Bind a batch holding MI_BATCH_BUFFER_END and run it on rcs0 via execbuf3. */
static int submit_test(int fd, __u32 vm_id)
{
	const __u64 va = 0x200000000ull;
	struct drm_i915_gem_create bo = { .size = 4096 };
	struct drm_i915_gem_mmap_offset mo = { 0 };
	uint32_t *map;
	int ok = 0;

	if (xioctl(fd, DRM_IOCTL_I915_GEM_CREATE, &bo)) {
		printf("FAIL  batch create: %s\n", strerror(errno));
		return 0;
	}
	mo.handle = bo.handle;
	mo.flags = I915_MMAP_OFFSET_FIXED;
	if (xioctl(fd, DRM_IOCTL_I915_GEM_MMAP_OFFSET, &mo)) {
		mo.flags = I915_MMAP_OFFSET_WB;
		if (xioctl(fd, DRM_IOCTL_I915_GEM_MMAP_OFFSET, &mo)) {
			printf("FAIL  batch mmap: %s\n", strerror(errno));
			goto close_bo;
		}
	}
	map = mmap(NULL, 4096, PROT_WRITE, MAP_SHARED, fd, mo.offset);
	if (map == MAP_FAILED) {
		printf("FAIL  batch mmap: %s\n", strerror(errno));
		goto close_bo;
	}
	map[0] = 0x0A << 23; /* MI_BATCH_BUFFER_END */
	map[1] = 0;
	munmap(map, 4096);

	struct drm_i915_gem_vm_bind b = { .vm_id = vm_id, .handle = bo.handle,
					  .start = va, .length = bo.size };
	if (xioctl(fd, DRM_IOCTL_I915_GEM_VM_BIND, &b)) {
		printf("FAIL  VM_BIND batch: %s\n", strerror(errno));
		goto close_bo;
	}

	I915_DEFINE_CONTEXT_PARAM_ENGINES(engines, 1) = {
		.engines = { { .engine_class = I915_ENGINE_CLASS_RENDER, .engine_instance = 0 } },
	};
	struct drm_i915_gem_context_create_ext_setparam p_eng = {
		.base = { .name = I915_CONTEXT_CREATE_EXT_SETPARAM },
		.param = { .param = I915_CONTEXT_PARAM_ENGINES,
			   .value = (uintptr_t)&engines, .size = sizeof(engines) },
	};
	struct drm_i915_gem_context_create_ext_setparam p_vm = {
		.base = { .name = I915_CONTEXT_CREATE_EXT_SETPARAM,
			  .next_extension = (uintptr_t)&p_eng },
		.param = { .param = I915_CONTEXT_PARAM_VM, .value = vm_id },
	};
	struct drm_i915_gem_context_create_ext cc = {
		.flags = I915_CONTEXT_CREATE_FLAGS_USE_EXTENSIONS,
		.extensions = (uintptr_t)&p_vm,
	};
	if (xioctl(fd, DRM_IOCTL_I915_GEM_CONTEXT_CREATE_EXT, &cc)) {
		printf("FAIL  context create (VM_BIND VM, rcs0): %s\n", strerror(errno));
		goto unbind;
	}

	struct drm_syncobj_create sc = { 0 };
	if (xioctl(fd, DRM_IOCTL_SYNCOBJ_CREATE, &sc)) {
		printf("FAIL  syncobj create: %s\n", strerror(errno));
		goto ctx;
	}
	struct drm_i915_gem_timeline_fence tf = { .handle = sc.handle,
						  .flags = I915_TIMELINE_FENCE_SIGNAL };
	struct drm_i915_gem_execbuffer3 eb = {
		.ctx_id = cc.ctx_id, .engine_idx = 0, .batch_address = va,
		.fence_count = 1, .timeline_fences = (uintptr_t)&tf,
	};
	if (xioctl(fd, DRM_IOCTL_I915_GEM_EXECBUFFER3, &eb)) {
		printf("FAIL  EXECBUFFER3: %s\n", strerror(errno));
		goto syncobj;
	}
	struct drm_syncobj_wait w = { .handles = (uintptr_t)&sc.handle, .count_handles = 1,
				      .timeout_nsec = INT64_MAX };
	if (xioctl(fd, DRM_IOCTL_SYNCOBJ_WAIT, &w)) {
		printf("FAIL  EXECBUFFER3 completion wait: %s\n", strerror(errno));
		goto syncobj;
	}
	printf("OK    EXECBUFFER3 batch on rcs0 completed\n");
	ok = 1;
syncobj:
	{
		struct drm_syncobj_destroy sd = { .handle = sc.handle };
		xioctl(fd, DRM_IOCTL_SYNCOBJ_DESTROY, &sd);
	}
ctx:
	{
		struct drm_i915_gem_context_destroy cd = { .ctx_id = cc.ctx_id };
		xioctl(fd, DRM_IOCTL_I915_GEM_CONTEXT_DESTROY, &cd);
	}
unbind:
	{
		struct drm_i915_gem_vm_unbind u = { .vm_id = vm_id, .start = va, .length = bo.size };
		xioctl(fd, DRM_IOCTL_I915_GEM_VM_UNBIND, &u);
	}
close_bo:
	{
		struct drm_gem_close c = { .handle = bo.handle };
		xioctl(fd, DRM_IOCTL_GEM_CLOSE, &c);
	}
	return ok;
}

int main(int argc, char **argv)
{
	const char *path = argc > 1 ? argv[1] : "/dev/dri/renderD128";
	int fd = open(path, O_RDWR | O_CLOEXEC), ver = 0, ok = 1;
	if (fd < 0) { perror(path); return 2; }

	drm_version_t v = {0}; char name[32] = {0};
	v.name = name; v.name_len = sizeof(name) - 1;
	if (xioctl(fd, DRM_IOCTL_VERSION, &v) || strcmp(name, "i915")) {
		fprintf(stderr, "%s is not an i915 device (driver: %s)\n", path, name);
		return 2;
	}

	struct drm_i915_getparam gp = { .param = I915_PARAM_VM_BIND_VERSION, .value = &ver };
	if (xioctl(fd, DRM_IOCTL_I915_GETPARAM, &gp)) {
		printf("FAIL  VM_BIND_VERSION query: %s (stock i915 loaded?)\n", strerror(errno));
		return 1;
	}
	printf("%-5s VM_BIND_VERSION = %d\n", ver ? "OK" : "FAIL", ver);
	if (!ver) return 1;

#ifdef I915_PARAM_VM_BIND_ABI
	int abi = 0;
	gp.param = I915_PARAM_VM_BIND_ABI;
	gp.value = &abi;
	if (xioctl(fd, DRM_IOCTL_I915_GETPARAM, &gp)) {
		printf("FAIL  VM_BIND_ABI query: %s\n", strerror(errno));
		return 1;
	}
	printf("%-5s VM_BIND_ABI = %d.%d (built for %d.%d)\n",
	       (abi >> 16) == I915_VM_BIND_ABI_MAJOR ? "OK" : "FAIL",
	       abi >> 16, abi & 0xffff, I915_VM_BIND_ABI_MAJOR, I915_VM_BIND_ABI_MINOR);
	if ((abi >> 16) != I915_VM_BIND_ABI_MAJOR) return 1;
#endif

	struct drm_i915_gem_vm_control vm = { .flags = I915_VM_CREATE_FLAGS_USE_VM_BIND };
	if (xioctl(fd, DRM_IOCTL_I915_GEM_VM_CREATE, &vm)) {
		printf("FAIL  VM create (USE_VM_BIND): %s\n", strerror(errno));
		return 1;
	}
	printf("OK    VM create (USE_VM_BIND) id=%u\n", vm.vm_id);

	struct drm_i915_gem_create bo = { .size = 2 << 20 };
	if (xioctl(fd, DRM_IOCTL_I915_GEM_CREATE, &bo)) {
		printf("FAIL  GEM create: %s\n", strerror(errno));
		return 1;
	}

	const __u64 va = 0x100000000ull;
	struct drm_i915_gem_vm_bind b = { .vm_id = vm.vm_id, .handle = bo.handle,
					  .start = va, .length = bo.size };
	if (xioctl(fd, DRM_IOCTL_I915_GEM_VM_BIND, &b)) {
		printf("FAIL  VM_BIND 2MiB @0x%llx: %s\n", (unsigned long long)va, strerror(errno));
		ok = 0;
	} else {
		printf("OK    VM_BIND 2MiB @0x%llx\n", (unsigned long long)va);
		struct drm_i915_gem_vm_unbind u = { .vm_id = vm.vm_id, .start = va, .length = bo.size };
		if (xioctl(fd, DRM_IOCTL_I915_GEM_VM_UNBIND, &u)) {
			printf("FAIL  VM_UNBIND: %s\n", strerror(errno));
			ok = 0;
		} else {
			printf("OK    VM_UNBIND\n");
		}
	}

	if (ok)
		ok = submit_test(fd, vm.vm_id);

	struct drm_gem_close c = { .handle = bo.handle };
	xioctl(fd, DRM_IOCTL_GEM_CLOSE, &c);
	xioctl(fd, DRM_IOCTL_I915_GEM_VM_DESTROY, &vm);
	close(fd);
	printf(ok ? "PASS\n" : "FAIL\n");
	return !ok;
}
