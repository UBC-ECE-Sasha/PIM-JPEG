#include <mram.h>
#include <stdio.h>

#include "jpeg-host.h"
#include "dpu-jpeg.h"

__mram_noinit char file_buffer[MAX_INPUT_LENGTH];

#define PREFETCH_SIZE 1024
__dma_aligned char file_buffer_cache[NR_TASKLETS][PREFETCH_SIZE];

void init_file_reader_index(JpegDecompressor *d) {
  d->file_index = -PREFETCH_SIZE;
  d->cache_index = PREFETCH_SIZE;
}

/*
	Initialize decompressor state
	The decompressor state is private to each tasklet (on the stack)
*/
void init_jpeg_decompressor(JpegDecompressor *d) {
  int file_index = jpegInfo.image_data_start + jpegInfo.size_per_tasklet * d->tasklet_id;

  // Calculating offset so that mram_read is 8 byte aligned
  int offset = file_index % 8;
  d->file_index = file_index - offset - PREFETCH_SIZE;
	//dbg_printf("[%u] file_index: %i\n", d->tasklet_id, d->file_index);
  d->cache_index = offset + PREFETCH_SIZE;
	//dbg_printf("[%u] cache_index: %i\n", d->tasklet_id, d->cache_index);
  d->length = jpegInfo.image_data_start + jpegInfo.size_per_tasklet * (d->tasklet_id + 1);
  if (d->length > jpegInfo.length) {
    d->length = jpegInfo.length;
  }

  d->bit_buffer = 0;
  d->bits_left = 0;
}

uint8_t read_byte(JpegDecompressor *d) {

			//dbg_printf("[:%u] read_byte (cache_index=%u) (file_index=%i)\n", d->tasklet_id, d->cache_index, d->file_index);
	// if we are reading past the end of the cache, fetch some more
  if (d->cache_index >= PREFETCH_SIZE) {
		//dbg_printf("[%u] fetching\n", d->tasklet_id);
    d->file_index += PREFETCH_SIZE;
		if ((uint32_t)&file_buffer[d->file_index] >= (uint32_t)0x4000000)
		{
			dbg_printf("Invalid src addr: %p\n", &file_buffer[d->file_index]);
			while(1);
		}
    //mram_read(&file_buffer[d->file_index], file_buffer_cache[d->tasklet_id], PREFETCH_SIZE);
    d->cache_index -= PREFETCH_SIZE;
  }

  uint8_t byte = file_buffer_cache[d->tasklet_id][d->cache_index++];
  return byte;
}

uint16_t read_short(JpegDecompressor *d) {
  uint8_t byte1 = read_byte(d);
  uint8_t byte2 = read_byte(d);

  uint16_t two_bytes = (byte1 << 8) | byte2;
  return two_bytes;
}

//This is not actually EOF, just end of assigned range
int is_eof(JpegDecompressor *d) {
  return ((d->file_index + d->cache_index) >= d->length);
}

void skip_bytes(JpegDecompressor *d, int num_bytes) {
  int offset = d->cache_index + num_bytes;
  if (offset >= PREFETCH_SIZE) {
    while (offset >= PREFETCH_SIZE) {
      offset -= PREFETCH_SIZE;
      d->file_index += PREFETCH_SIZE;
    }
    //mram_read(&file_buffer[d->file_index], file_buffer_cache[d->tasklet_id], PREFETCH_SIZE);
  }
  d->cache_index = offset;

  if (is_eof(d)) {
    d->file_index = d->length;
    d->cache_index = 0;
  }
}
