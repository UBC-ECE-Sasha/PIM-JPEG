IDIR = include
CC = gcc
CFLAGS = --std=c99 -O3 -g -Wall -Wextra -I $(IDIR) -I ./PIM-common/common/include -I ./PIM-common/host/include -DSEQREAD_CACHE_SIZE=$(SEQREAD_CACHE_SIZE)
DPU_OPTS = `dpu-pkg-config --cflags --libs dpu`

# define DEBUG in the source if we are debugging
ifeq ($(DEBUG_CPU), 1)
	CFLAGS+=-DDEBUG
endif

ifeq ($(DEBUG_DPU), 1)
	CFLAGS+=-DDEBUG_DPU
endif

# Default NR_DPUS and NR_TASKLETS
NR_DPUS = 1
NR_TASKLETS = 1

# Bulk (dpu_prepare_xfer) is default
BULK = 0

# Statistics are on by default
STATS = 1
SEQREAD_CACHE_SIZE=256
MAX_FILES_PER_DPU=64

ifeq ($(BULK), 1)
	CFLAGS+=-DBULK_TRANSFER
endif

ifeq ($(STATS), 1)
	CFLAGS+=-DSTATISTICS
endif

SOURCE = src/jpeg-host.c src/bmp.c src/jpeg-cpu.c

.PHONY: default all dpu host clean tags

default: all

all: host

clean:
	$(RM) host-*
	$(MAKE) -C src/dpu clean

dpu:
	DEBUG=$(DEBUG_DPU) NR_TASKLETS=$(NR_TASKLETS) SEQREAD_CACHE_SIZE=$(SEQREAD_CACHE_SIZE) MAX_FILES_PER_DPU=$(MAX_FILES_PER_DPU)  $(MAKE) -C dpu-grep

host: $(SOURCE)
	$(CC) $(CFLAGS) -DNR_TASKLETS=$(NR_TASKLETS) -DMAX_FILES_PER_DPU=$(MAX_FILES_PER_DPU) $^ -o $@-$(NR_TASKLETS) $(DPU_OPTS)
	NR_DPUS=$(NR_DPUS) NR_TASKLETS=$(NR_TASKLETS) \
	$(MAKE) -C src/dpu

tags:
	ctags -R -f tags . ~/projects/upmem/upmem-sdk
