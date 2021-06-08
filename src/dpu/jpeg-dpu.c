#include <defs.h>
#include <mram.h>
#include <stdio.h>

#include "jpeg-common.h"
#include "jpeg-host.h"

__host dpu_input_t input;
// TODO: implicit MRAM access currently, change to explicit once logic is finished
__mram_noinit char buffer[MAX_INPUT_LENGTH];

/**
 * Helper array for filling in quantization table in zigzag order
 */
const uint8_t ZIGZAG_ORDER[] = {0,  1,  8,  16, 9,  2,  3,  10, 17, 24, 32, 25, 18, 11, 4,  5,  12, 19, 26, 33, 40, 48,
                                41, 34, 27, 20, 13, 6,  7,  14, 21, 28, 35, 42, 49, 56, 57, 50, 43, 36, 29, 22, 15, 23,
                                30, 37, 44, 51, 58, 59, 52, 45, 38, 31, 39, 46, 53, 60, 61, 54, 47, 55, 62, 63};

/**
 * Check whether EOF is reached by comparing current ptr location to start
 * location + entire length of file
 *
 * @param d JpegDecompressor struct that holds all information about the JPEG currently being decoded
 */
static int eof(JpegDecompressor *d) { return (d->index >= d->length); }

/**
 * Helper function to read a byte from the file
 *
 * @param d JpegDecompressor struct that holds all information about the JPEG currently being decoded
 */
static uint8_t read_byte(JpegDecompressor *d) {
  uint8_t temp;

  temp = buffer[d->index++];
  return temp;
}

/**
 * Helper function to read 2 bytes from the file, MSB order
 *
 * @param d JpegDecompressor struct that holds all information about the JPEG currently being decoded
 */
static uint16_t read_short(JpegDecompressor *d) {
  uint16_t temp3;
  uint8_t temp1, temp2;

  temp1 = buffer[d->index++];
  temp2 = buffer[d->index++];

  temp3 = (temp1 << 8) | temp2;
  return temp3;
}

/**
 * Read whether we are at valid START OF IMAGE
 *
 * @param d JpegDecompressor struct that holds all information about the JPEG currently being decoded
 */
static void process_header(JpegDecompressor *d) {
  uint8_t c1 = 0, c2 = 0;

  if (!eof(d)) {
    c1 = read_byte(d);
    c2 = read_byte(d);
  }
  if (c1 != 0xFF || c2 != M_SOI) {
    d->valid = 0;
    printf("Error: Not JPEG: %X %X\n", c1, c2);
  } else {
    printf("Got SOI marker: %X %X\n", c1, c2);
  }
}

/**
 * Initialize the JPEG decompressor with default values
 *
 * @param d JpegDecompressor struct that holds all information about the JPEG currently being decoded
 */
static void init_jpeg_decompressor(JpegDecompressor *d) {
  d->valid = 1;
  d->index = 0;

  for (int i = 0; i < 4; i++) {
    d->quant_tables[i].exists = 0;

    if (i < 2) {
      d->color_components[i].exists = 0;
      d->dc_huffman_tables[i].exists = 0;
      d->ac_huffman_tables[i].exists = 0;

    } else if (i < 3) {
      d->color_components[i].exists = 0;
    }
  }

  // These fields will be filled when reading the JPEG header information
  d->restart_interval = 0;
  d->image_height = 0;
  d->image_width = 0;
  d->num_color_components = 0;
  d->ss = 0;
  d->se = 0;
  d->Ah = 0;
  d->Al = 0;

  // These fields will be used when decoded Huffman coded bitstream
  d->get_buffer = 0;
  d->bits_left = 0;

  // These fields will be used when writing to BMP
  d->mcu_width = 0;
  d->mcu_height = 0;
  d->padding = 0;
}

int main() {
  JpegDecompressor decompressor;
  int res = 1;

  decompressor.length = input.file_length;
  init_jpeg_decompressor(&decompressor);

  // Check whether file starts with SOI
  process_header(&decompressor);

  printf("\n");

  return 0;
}