IDIR = include
CC = gcc
CFLAGS = --std=c99 -O3 -g -Wall -Wextra -I $(IDIR) -I ./PIM-common/common/include -I ./PIM-common/host/include
DPU_OPTS = `dpu-pkg-config --cflags --libs dpu`

# define DEBUG in the source if we are debugging
ifeq ($(DEBUG_CPU), 1)
	CFLAGS+=-DDEBUG
endif

ifeq ($(DEBUG_DPU), 1)
	CFLAGS+=-DDEBUG_DPU
endif

# Collect statistics about various operations
STATS ?= 0

# How many files can be assigned to a single DPU
MAX_FILES_PER_DPU ?= 1

# How many tasklets should each DPU use for decompression
NR_TASKLETS ?= 1

ifeq ($(STATS), 1)
	CFLAGS+=-DSTATISTICS
endif

SOURCE = src/jpeg-host.c src/bmp.c src/jpeg-cpu.c

.PHONY: default all dpu host clean tags

default: all

all: dpu host

clean:
	$(RM) host-*
	$(MAKE) -C src/dpu clean

dpu:
	$(MAKE) DEBUG=$(DEBUG_DPU) NR_TASKLETS=$(NR_TASKLETS) -C src/dpu

host: $(SOURCE)
	$(CC) $(CFLAGS) -DNR_TASKLETS=$(NR_TASKLETS) -DMAX_FILES_PER_DPU=$(MAX_FILES_PER_DPU) $^ -o $@-$(NR_TASKLETS) $(DPU_OPTS)

tags:
	ctags -R -f tags . ~/projects/upmem/upmem-sdk
