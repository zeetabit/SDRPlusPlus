MAKEFILES:=$(shell find cmake-build-debug -mindepth 3 -name Makefile -type f)
DIRS:=$(foreach m,$(MAKEFILES),$(realpath $(dir $(m))))

.PHONY: all
all: $(DIRS)

.PHONY: $(DIRS)
$(DIRS):
	echo "MAKEFILE ITERATION: handle $@"
	$(MAKE) -f $@/Makefile -C $@