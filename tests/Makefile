# Build the VM_BIND smoke test against the exported uapi headers.
#   ../vmbind.py abi && make LINE=7.3
LINE ?= 7.3
UAPI ?= ../abi/$(LINE)
CFLAGS ?= -O2 -Wall -Wextra -Wno-missing-field-initializers

vmbind_check: vmbind_check.c $(UAPI)/drm/i915_drm.h
	$(CC) $(CFLAGS) -I$(UAPI) -o $@ $< $(LDFLAGS)

clean:
	rm -f vmbind_check
.PHONY: clean
