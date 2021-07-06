#include <mram.h>
#include <stdio.h>

#include "dpu-jpeg.h"

__mram_noinit char file_buffer[16 << 20];

#define PREFETCH_SIZE 1024
__dma_aligned char file_buffer_cache[NR_TASKLETS][PREFETCH_SIZE];

static void skip_bytes(JpegDecompressor *d, int num_bytes);
static int count_and_skip_non_marker_bytes(JpegDecompressor *d);
static int get_marker_and_ignore_ff_bytes(JpegDecompressor *d);

void init_file_reader_index(JpegDecompressor *d) {
  d->file_index = -PREFETCH_SIZE;
  d->cache_index = PREFETCH_SIZE;
}

void init_jpeg_decompressor(JpegDecompressor *d) {
  int file_index = jpegInfo.image_data_start + jpegInfo.size_per_tasklet * d->tasklet_id;
  // Calculating offset so that mram_read is 8 byte aligned
  int offset = file_index % 8;
  d->file_index = file_index - offset - PREFETCH_SIZE;
  d->cache_index = offset + PREFETCH_SIZE;
  d->length = jpegInfo.image_data_start + jpegInfo.size_per_tasklet * (d->tasklet_id + 1);
  if (d->length > jpegInfo.length) {
    d->length = jpegInfo.length;
  }

  d->bit_buffer = 0;
  d->bits_left = 0;
}

int is_eof(JpegDecompressor *d) {
  return ((d->file_index + d->cache_index) >= d->length);
}

uint8_t read_byte(JpegDecompressor *d) {
  if (d->cache_index >= PREFETCH_SIZE) {
    d->file_index += PREFETCH_SIZE;
    mram_read(&file_buffer[d->file_index], file_buffer_cache[d->tasklet_id], PREFETCH_SIZE);
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

int skip_marker(JpegDecompressor *d) {
  int length = read_short(d);
  length -= 2;

  if (length < 0) {
    printf("ERROR: Invalid length encountered in skip_marker");
    return JPEG_INVALID_ERROR_CODE;
  }

  skip_bytes(d, length);
  return JPEG_VALID;
}

int skip_to_next_marker(JpegDecompressor *d) {
  int num_skipped_bytes = count_and_skip_non_marker_bytes(d);
  int marker = get_marker_and_ignore_ff_bytes(d);

  if (num_skipped_bytes) {
    printf("WARNING: Discarded %u bytes\n", num_skipped_bytes);
  }

  return marker;
}

static void skip_bytes(JpegDecompressor *d, int num_bytes) {
  int offset = d->cache_index + num_bytes;
  if (offset >= PREFETCH_SIZE) {
    while (offset >= PREFETCH_SIZE) {
      offset -= PREFETCH_SIZE;
      d->file_index += PREFETCH_SIZE;
    }
    mram_read(&file_buffer[d->file_index], file_buffer_cache[d->tasklet_id], PREFETCH_SIZE);
  }
  d->cache_index = offset;

  if (is_eof(d)) {
    d->file_index = d->length;
    d->cache_index = 0;
  }
}

static int count_and_skip_non_marker_bytes(JpegDecompressor *d) {
  int num_skipped_bytes = 0;
  uint8_t byte = read_byte(d);
  while (byte != 0xFF) {
    if (is_eof(d)) {
      return -1;
    }
    num_skipped_bytes++;
    byte = read_byte(d);
  }
  return num_skipped_bytes;
}

static int get_marker_and_ignore_ff_bytes(JpegDecompressor *d) {
  int marker;
  do {
    if (is_eof(d)) {
      return -1;
    }
    marker = read_byte(d);
  } while (marker == 0xFF);
  return marker;
}
