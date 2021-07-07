IDIR0 = ../../include
IDIR1 = ../../PIM-common/common/include
IDIR2 = ../../PIM-common/host/include
CC = dpu-upmem-dpurte-clang
CFLAGS = -DNR_DPUS=$(NR_DPUS) -DNR_TASKLETS=$(NR_TASKLETS) -I$(IDIR0) -I$(IDIR1) -I$(IDIR2) -O2

SOURCE = jpeg-dpu.c dpu-jpeg-reader.c dpu-jpeg-marker.c dpu-jpeg-decode.c $(wildcard markers/*.c)

.PHONY: clean

jpeg-dpu: $(SOURCE)
	$(CC) $(CFLAGS) $^ -o $@-$(NR_TASKLETS)

clean:
	rm -f jpeg-dpu-*