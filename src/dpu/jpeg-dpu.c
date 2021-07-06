#include <barrier.h>
#include <defs.h>
#include <mram.h>
#include <stdio.h>

#include "dpu-jpeg.h"
#include "jpeg-host.h"

__host uint64_t file_length;
__host dpu_output_t output;
// About 32MB
__mram_noinit short MCU_buffer[NR_TASKLETS][16776960 / NR_TASKLETS];

JpegInfo jpegInfo;
JpegInfoDpu jpegInfoDpu;

BARRIER_INIT(init_barrier, NR_TASKLETS);
BARRIER_INIT(idct_barrier, NR_TASKLETS);

#define PREWRITE_SIZE 768
__dma_aligned short MCU_buffer_cache[NR_TASKLETS][PREWRITE_SIZE];
#define MCU_READ_WRITE_SIZE 128

#define INDEX_OFFSET 64
#define DC_COEFF_OFFSET 192

#define DEBUG 0

// /**
//  * Helper array for filling in quantization table in zigzag order
//  */
// const uint8_t ZIGZAG_ORDER[] = {0,  1,  8,  16, 9,  2,  3,  10, 17, 24, 32, 25, 18, 11, 4,  5,  12, 19, 26, 33, 40,
// 48,
//                                 41, 34, 27, 20, 13, 6,  7,  14, 21, 28, 35, 42, 49, 56, 57, 50, 43, 36, 29, 22, 15,
//                                 23, 30, 37, 44, 51, 58, 59, 52, 45, 38, 31, 39, 46, 53, 60, 61, 54, 47, 55, 62, 63};

static int get_num_bits(JpegDecompressor *d, int num_bits) {
  int bits = 0;
  if (num_bits == 0) {
    return bits;
  }

  uint8_t temp_byte;
  uint32_t actual_byte;
  while (d->bits_left < num_bits) {
    // Read a byte and decode it, if it is 0xFF
    temp_byte = read_byte(d);
    actual_byte = temp_byte;

    while (temp_byte == 0xFF) {
      // FF may be padded with FFs, read as many FFs as necessary
      temp_byte = read_byte(d);
      if (temp_byte == 0) {
        // Got FF which is not a marker, save it to buffer
        actual_byte = 0xFF;
      } else if (temp_byte >= M_RST_FIRST && temp_byte <= M_RST_LAST) {
        // Got restart markers, ignore and read new byte
        temp_byte = read_byte(d);
        actual_byte = temp_byte;
      } else {
        actual_byte = temp_byte;
      }
    }

    // Add the new bits to the buffer (MSB aligned)
    d->bit_buffer |= actual_byte << (32 - 8 - d->bits_left);
    d->bits_left += 8;
  }

  bits = d->bit_buffer >> (32 - num_bits);
  d->bit_buffer <<= num_bits;
  d->bits_left -= num_bits;
  return bits;
}

static uint8_t huff_decode(JpegDecompressor *d, HuffmanTable *h_table) {
  uint32_t code = 0;

  for (int i = 0; i < 16; i++) {
    int bit = get_num_bits(d, 1);
    code = (code << 1) | bit;
    for (int j = h_table->valoffset[i]; j < h_table->valoffset[i + 1]; j++) {
      if (code == h_table->codes[j]) {
        return h_table->huffval[j];
      }
    }
  }

  return -1;
}

static int decode_mcu(JpegDecompressor *d, int component_index, short *previous_dc) {
  QuantizationTable *q_table = &jpegInfo.quant_tables[jpegInfo.color_components[component_index].quant_table_id];
  HuffmanTable *dc_table = &jpegInfo.dc_huffman_tables[jpegInfo.color_components[component_index].dc_huffman_table_id];
  HuffmanTable *ac_table = &jpegInfo.ac_huffman_tables[jpegInfo.color_components[component_index].ac_huffman_table_id];

  // Get DC value for this MCU block
  uint8_t dc_length = huff_decode(d, dc_table);
  if (dc_length == (uint8_t) -1) {
    printf("Error: Invalid DC code\n");
    return -1;
  }
  if (dc_length > 11) {
    printf("Error: DC coefficient length greater than 11\n");
    return -1;
  }

  int coeff = get_num_bits(d, dc_length);
  if (dc_length != 0 && coeff < (1 << (dc_length - 1))) {
    // Convert to negative coefficient
    coeff -= (1 << dc_length) - 1;
  }
  MCU_buffer_cache[d->tasklet_id][0] = coeff + *previous_dc;
  *previous_dc = MCU_buffer_cache[d->tasklet_id][0];
  // Dequantization
  MCU_buffer_cache[d->tasklet_id][0] *= q_table->table[0];

  // Get the AC values for this MCU block
  int i = 1;
  while (i < 64) {
    uint8_t ac_length = huff_decode(d, ac_table);
    if (ac_length == (uint8_t) -1) {
      printf("Error: Invalid AC code\n");
      return -1;
    }

    // Got 0x00, fill remaining MCU block with 0s
    if (ac_length == 0x00) {
      while (i < 64) {
        MCU_buffer_cache[d->tasklet_id][ZIGZAG_ORDER[i++]] = 0;
      }
      break;
    }

    // MCU block not entirely filled yet, continue reading
    uint8_t num_zeroes = (ac_length >> 4) & 0x0F;
    uint8_t coeff_length = ac_length & 0x0F;
    coeff = 0;

    // Got 0xF0, skip 16 0s
    if (ac_length == 0xF0) {
      num_zeroes = 16;
    }

    if (i + num_zeroes >= 64) {
      printf("Error: Invalid AC code - zeros exceeded MCU length %d >= 64\n", i + num_zeroes);
      return -1;
    }
    for (int j = 0; j < num_zeroes; j++) {
      MCU_buffer_cache[d->tasklet_id][ZIGZAG_ORDER[i++]] = 0;
    }

    if (coeff_length > 10) {
      printf("Error: AC coefficient length greater than 10\n");
      return -1;
    }
    if (coeff_length != 0) {
      coeff = get_num_bits(d, coeff_length);
      if (coeff < (1 << (coeff_length - 1))) {
        // Convert to negative coefficient
        coeff -= (1 << coeff_length) - 1;
      }
      // Write coefficient to buffer as well as perform dequantization
      MCU_buffer_cache[d->tasklet_id][ZIGZAG_ORDER[i]] = coeff * q_table->table[ZIGZAG_ORDER[i]];
      i++;
    }
  }

  return 0;
}

static void decompress_scanline(JpegDecompressor *d) {
  short previous_dcs[3] = {0};
  int restart_interval = jpegInfo.restart_interval * jpegInfo.max_h_samp_factor * jpegInfo.max_v_samp_factor;
  int row, col;
  int synch_mcu_index = 0;

  for (row = 0; row < jpegInfo.mcu_height; row += jpegInfo.max_v_samp_factor) {
    for (col = 0; col < jpegInfo.mcu_width; col += jpegInfo.max_h_samp_factor) {
      if (is_eof(d)) {
        goto sync0;
      }

      for (int color_index = 0; color_index < jpegInfo.num_color_components; color_index++) {
        for (int y = 0; y < jpegInfo.color_components[color_index].v_samp_factor; y++) {
          for (int x = 0; x < jpegInfo.color_components[color_index].h_samp_factor; x++) {
            // Decode Huffman coded bitstream
            while (decode_mcu(d, color_index, &previous_dcs[color_index]) != 0) {
              // Keep decoding until valid MCU is decoded
            }

            if (synch_mcu_index < 128) {
              MCU_buffer_cache[d->tasklet_id][INDEX_OFFSET + synch_mcu_index] = d->file_index + d->cache_index;
              MCU_buffer_cache[d->tasklet_id][DC_COEFF_OFFSET + synch_mcu_index] = MCU_buffer_cache[d->tasklet_id][0];
              synch_mcu_index++;
            }

            int mcu_index = (((row + y) * jpegInfo.mcu_width_real + (col + x)) * 3 + color_index) << 6;
            mram_write(MCU_buffer_cache[d->tasklet_id], &MCU_buffer[d->tasklet_id][mcu_index], MCU_READ_WRITE_SIZE);
          }
        }
      }
    }
  }

sync0:;
  // Tasklet i has to overflow to MCUs decoded by Tasklet i + 1 for synchronisation
  // The last tasklet cannot overflow, so it returns first
  int current_mcu_index = (row * jpegInfo.mcu_width_real + col) * 192;
  if (current_mcu_index > 16776960 / NR_TASKLETS) {
    printf("Warning: Tasklet %d exceeded buffer size limit, output image is most likely malformed\n", d->tasklet_id);
  }

  if (d->tasklet_id == NR_TASKLETS - 1) {
    jpegInfoDpu.mcu_end_index[d->tasklet_id] = current_mcu_index;
    return;
  }

  // The synchronisation strategy is to have Tasklet i continually decode MCU blocks until several MCU blocks
  // are decoded that match the blocks decoded by Tasklet i + 1. Matching blocks are detected through comparing
  // the file offset between the 2 tasklets
  int next_tasklet_mcu_blocks_elapsed = 0;
  int num_synched_mcu_blocks = 0;
  int minimum_synched_mcu_blocks = jpegInfo.max_h_samp_factor * jpegInfo.max_v_samp_factor + 2;

  for (; row < jpegInfo.mcu_height; row += jpegInfo.max_v_samp_factor) {
    for (; col < jpegInfo.mcu_width; col += jpegInfo.max_h_samp_factor) {
      if (num_synched_mcu_blocks >= minimum_synched_mcu_blocks + 1) {
        jpegInfoDpu.mcu_end_index[d->tasklet_id] = (row * jpegInfo.mcu_width_real + col) * 192;
        int blocks_elapsed =
            (next_tasklet_mcu_blocks_elapsed / minimum_synched_mcu_blocks) * jpegInfo.max_h_samp_factor;
        jpegInfoDpu.mcu_start_index[d->tasklet_id + 1] = blocks_elapsed * 192;
        goto sync1;
      }

      for (int color_index = 0; color_index < jpegInfo.num_color_components; color_index++) {
        for (int y = 0; y < jpegInfo.color_components[color_index].v_samp_factor; y++) {
          for (int x = 0; x < jpegInfo.color_components[color_index].h_samp_factor; x++) {
            if (decode_mcu(d, color_index, &previous_dcs[color_index]) != 0) {
              jpegInfo.valid = 0;
              printf("Error: Invalid MCU\n");
              return;
            }

            short current_tasklet_file_index = d->file_index + d->cache_index;
            short next_tasklet_file_index =
                MCU_buffer_cache[d->tasklet_id + 1][INDEX_OFFSET + next_tasklet_mcu_blocks_elapsed];

            int mcu_index = (((row + y) * jpegInfo.mcu_width_real + (col + x)) * 3 + color_index) << 6;
            mram_write(MCU_buffer_cache[d->tasklet_id], &MCU_buffer[d->tasklet_id][mcu_index], MCU_READ_WRITE_SIZE);

            if (current_tasklet_file_index < next_tasklet_file_index) {
              // Tasklet i needs to decode more blocks
              num_synched_mcu_blocks = 0;
            } else if (current_tasklet_file_index > next_tasklet_file_index) {
              // More blocks needs to be elapsed for tasklet i + 1
              num_synched_mcu_blocks = 0;
              next_tasklet_mcu_blocks_elapsed++;

              while (current_tasklet_file_index > next_tasklet_file_index) {
                next_tasklet_file_index =
                    MCU_buffer_cache[d->tasklet_id + 1][INDEX_OFFSET + next_tasklet_mcu_blocks_elapsed];
                next_tasklet_mcu_blocks_elapsed++;
              }

              if (current_tasklet_file_index == next_tasklet_file_index) {
                jpegInfoDpu.dc_offset[d->tasklet_id][color_index] =
                    MCU_buffer_cache[d->tasklet_id][0] -
                    MCU_buffer_cache[d->tasklet_id + 1][DC_COEFF_OFFSET + next_tasklet_mcu_blocks_elapsed];
                num_synched_mcu_blocks++;
              }
            } else {
              jpegInfoDpu.dc_offset[d->tasklet_id][color_index] =
                  MCU_buffer_cache[d->tasklet_id][0] -
                  MCU_buffer_cache[d->tasklet_id + 1][DC_COEFF_OFFSET + next_tasklet_mcu_blocks_elapsed];

              num_synched_mcu_blocks++;
              next_tasklet_mcu_blocks_elapsed++;
            }
          }
        }
      }
    }
    col = 0;
  }

sync1:;
  // Tasklet 0 does a one pass through all MCUs to adjust DC coefficients
  if (d->tasklet_id != 0) {
    return;
  }

  int tasklet_index = 1;
  int start_index = jpegInfoDpu.mcu_start_index[tasklet_index] / 192;
  int tasklet_row = start_index / jpegInfo.mcu_width_real;
  int tasklet_col = start_index % jpegInfo.mcu_width_real;
  int dc_offset[3] = {jpegInfoDpu.dc_offset[0][0], jpegInfoDpu.dc_offset[0][1], jpegInfoDpu.dc_offset[0][2]};

  for (; row < jpegInfo.mcu_height; row += jpegInfo.max_v_samp_factor) {
    for (; col < jpegInfo.mcu_width; col += jpegInfo.max_h_samp_factor, tasklet_col += jpegInfo.max_h_samp_factor) {
      if (tasklet_col >= jpegInfo.mcu_width) {
        tasklet_col = 0;
        tasklet_row += jpegInfo.max_v_samp_factor;
      }
      if ((tasklet_row * jpegInfo.mcu_width_real + tasklet_col) * 192 >= jpegInfoDpu.mcu_end_index[tasklet_index]) {
        tasklet_index++;
        start_index = jpegInfoDpu.mcu_start_index[tasklet_index] / 192;
        tasklet_row = start_index / jpegInfo.mcu_width_real;
        tasklet_col = start_index % jpegInfo.mcu_width_real;
        dc_offset[0] += jpegInfoDpu.dc_offset[tasklet_index - 1][0];
        dc_offset[1] += jpegInfoDpu.dc_offset[tasklet_index - 1][1];
        dc_offset[2] += jpegInfoDpu.dc_offset[tasklet_index - 1][2];
      }

      for (int color_index = 0; color_index < jpegInfo.num_color_components; color_index++) {
        for (int y = 0; y < jpegInfo.color_components[color_index].v_samp_factor; y++) {
          for (int x = 0; x < jpegInfo.color_components[color_index].h_samp_factor; x++) {
            int mcu_index = (((tasklet_row + y) * jpegInfo.mcu_width_real + (tasklet_col + x)) * 3 + color_index) << 6;
            mram_read(&MCU_buffer[tasklet_index][mcu_index], MCU_buffer_cache[0], MCU_READ_WRITE_SIZE);

            MCU_buffer_cache[0][0] += dc_offset[color_index];

            mcu_index = (((row + y) * jpegInfo.mcu_width_real + (col + x)) * 3 + color_index) << 6;
            mram_write(MCU_buffer_cache[0], &MCU_buffer[0][mcu_index], MCU_READ_WRITE_SIZE);
          }
        }
      }
    }
    col = 0;
  }
}

static void inverse_dct_component(JpegDecompressor *d, int cache_index) {
  // ANN algorithm, intermediate values are bit shifted to the left to preserve precision
  // and then bit shifted to the right at the end
  for (int i = 0; i < 8; i++) {
    // Higher accuracy
    int g0 = (MCU_buffer_cache[d->tasklet_id][cache_index + (0 << 3) + i] * 181) >> 5;
    int g1 = (MCU_buffer_cache[d->tasklet_id][cache_index + (4 << 3) + i] * 181) >> 5;
    int g2 = (MCU_buffer_cache[d->tasklet_id][cache_index + (2 << 3) + i] * 59) >> 3;
    int g3 = (MCU_buffer_cache[d->tasklet_id][cache_index + (6 << 3) + i] * 49) >> 4;
    int g4 = (MCU_buffer_cache[d->tasklet_id][cache_index + (5 << 3) + i] * 71) >> 4;
    int g5 = (MCU_buffer_cache[d->tasklet_id][cache_index + (1 << 3) + i] * 251) >> 5;
    int g6 = (MCU_buffer_cache[d->tasklet_id][cache_index + (7 << 3) + i] * 25) >> 4;
    int g7 = (MCU_buffer_cache[d->tasklet_id][cache_index + (3 << 3) + i] * 213) >> 5;

    // Lower accuracy
    // int g0 = (MCU_buffer_cache[d->tasklet_id][cache_index + (0 << 3) + i] * 22) >> 2;
    // int g1 = (MCU_buffer_cache[d->tasklet_id][cache_index + (4 << 3) + i] * 22) >> 2;
    // int g2 = (MCU_buffer_cache[d->tasklet_id][cache_index + (2 << 3) + i] * 30) >> 2;
    // int g3 = (MCU_buffer_cache[d->tasklet_id][cache_index + (6 << 3) + i] * 12) >> 2;
    // int g4 = (MCU_buffer_cache[d->tasklet_id][cache_index + (5 << 3) + i] * 18) >> 2;
    // int g5 = (MCU_buffer_cache[d->tasklet_id][cache_index + (1 << 3) + i] * 31) >> 2;
    // int g6 = (MCU_buffer_cache[d->tasklet_id][cache_index + (7 << 3) + i] * 6) >> 2;
    // int g7 = (MCU_buffer_cache[d->tasklet_id][cache_index + (3 << 3) + i] * 27) >> 2;

    int f4 = g4 - g7;
    int f5 = g5 + g6;
    int f6 = g5 - g6;
    int f7 = g4 + g7;

    int e2 = g2 - g3;
    int e3 = g2 + g3;
    int e5 = f5 - f7;
    int e7 = f5 + f7;
    int e8 = f4 + f6;

    // Higher accuracy
    int d2 = (e2 * 181) >> 7;
    int d4 = (f4 * 277) >> 8;
    int d5 = (e5 * 181) >> 7;
    int d6 = (f6 * 669) >> 8;
    int d8 = (e8 * 49) >> 6;

    // Lower accuracy
    // int d2 = (e2 * 90) >> 6;
    // int d4 = (f4 * 69) >> 6;
    // int d5 = (e5 * 90) >> 6;
    // int d6 = (f6 * 167) >> 6;
    // int d8 = (e8 * 49) >> 6;

    int c0 = g0 + g1;
    int c1 = g0 - g1;
    int c2 = d2 - e3;
    int c4 = d4 + d8;
    int c5 = d5 + e7;
    int c6 = d6 - d8;
    int c8 = c5 - c6;

    int b0 = c0 + e3;
    int b1 = c1 + c2;
    int b2 = c1 - c2;
    int b3 = c0 - e3;
    int b4 = c4 - c8;
    int b6 = c6 - e7;

    MCU_buffer_cache[d->tasklet_id][cache_index + (0 << 3) + i] = (b0 + e7) >> 4;
    MCU_buffer_cache[d->tasklet_id][cache_index + (1 << 3) + i] = (b1 + b6) >> 4;
    MCU_buffer_cache[d->tasklet_id][cache_index + (2 << 3) + i] = (b2 + c8) >> 4;
    MCU_buffer_cache[d->tasklet_id][cache_index + (3 << 3) + i] = (b3 + b4) >> 4;
    MCU_buffer_cache[d->tasklet_id][cache_index + (4 << 3) + i] = (b3 - b4) >> 4;
    MCU_buffer_cache[d->tasklet_id][cache_index + (5 << 3) + i] = (b2 - c8) >> 4;
    MCU_buffer_cache[d->tasklet_id][cache_index + (6 << 3) + i] = (b1 - b6) >> 4;
    MCU_buffer_cache[d->tasklet_id][cache_index + (7 << 3) + i] = (b0 - e7) >> 4;
  }

  for (int i = 0; i < 8; i++) {
    // Higher accuracy
    int g0 = (MCU_buffer_cache[d->tasklet_id][cache_index + (i << 3) + 0] * 181) >> 5;
    int g1 = (MCU_buffer_cache[d->tasklet_id][cache_index + (i << 3) + 4] * 181) >> 5;
    int g2 = (MCU_buffer_cache[d->tasklet_id][cache_index + (i << 3) + 2] * 59) >> 3;
    int g3 = (MCU_buffer_cache[d->tasklet_id][cache_index + (i << 3) + 6] * 49) >> 4;
    int g4 = (MCU_buffer_cache[d->tasklet_id][cache_index + (i << 3) + 5] * 71) >> 4;
    int g5 = (MCU_buffer_cache[d->tasklet_id][cache_index + (i << 3) + 1] * 251) >> 5;
    int g6 = (MCU_buffer_cache[d->tasklet_id][cache_index + (i << 3) + 7] * 25) >> 4;
    int g7 = (MCU_buffer_cache[d->tasklet_id][cache_index + (i << 3) + 3] * 213) >> 5;

    // Lower accuracy
    // int g0 = (MCU_buffer_cache[d->tasklet_id][cache_index + (i << 3) + 0] * 22) >> 2;
    // int g1 = (MCU_buffer_cache[d->tasklet_id][cache_index + (i << 3) + 4] * 22) >> 2;
    // int g2 = (MCU_buffer_cache[d->tasklet_id][cache_index + (i << 3) + 2] * 30) >> 2;
    // int g3 = (MCU_buffer_cache[d->tasklet_id][cache_index + (i << 3) + 6] * 12) >> 2;
    // int g4 = (MCU_buffer_cache[d->tasklet_id][cache_index + (i << 3) + 5] * 18) >> 2;
    // int g5 = (MCU_buffer_cache[d->tasklet_id][cache_index + (i << 3) + 1] * 31) >> 2;
    // int g6 = (MCU_buffer_cache[d->tasklet_id][cache_index + (i << 3) + 7] * 6) >> 2;
    // int g7 = (MCU_buffer_cache[d->tasklet_id][cache_index + (i << 3) + 3] * 27) >> 2;

    int f4 = g4 - g7;
    int f5 = g5 + g6;
    int f6 = g5 - g6;
    int f7 = g4 + g7;

    int e2 = g2 - g3;
    int e3 = g2 + g3;
    int e5 = f5 - f7;
    int e7 = f5 + f7;
    int e8 = f4 + f6;

    // Higher accuracy
    int d2 = (e2 * 181) >> 7;
    int d4 = (f4 * 277) >> 8;
    int d5 = (e5 * 181) >> 7;
    int d6 = (f6 * 669) >> 8;
    int d8 = (e8 * 49) >> 6;

    // Lower accuracy
    // int d2 = (e2 * 90) >> 6;
    // int d4 = (f4 * 69) >> 6;
    // int d5 = (e5 * 90) >> 6;
    // int d6 = (f6 * 167) >> 6;
    // int d8 = (e8 * 49) >> 6;

    int c0 = g0 + g1;
    int c1 = g0 - g1;
    int c2 = d2 - e3;
    int c4 = d4 + d8;
    int c5 = d5 + e7;
    int c6 = d6 - d8;
    int c8 = c5 - c6;

    int b0 = c0 + e3;
    int b1 = c1 + c2;
    int b2 = c1 - c2;
    int b3 = c0 - e3;
    int b4 = c4 - c8;
    int b6 = c6 - e7;

    MCU_buffer_cache[d->tasklet_id][cache_index + (i << 3) + 0] = (b0 + e7) >> 4;
    MCU_buffer_cache[d->tasklet_id][cache_index + (i << 3) + 1] = (b1 + b6) >> 4;
    MCU_buffer_cache[d->tasklet_id][cache_index + (i << 3) + 2] = (b2 + c8) >> 4;
    MCU_buffer_cache[d->tasklet_id][cache_index + (i << 3) + 3] = (b3 + b4) >> 4;
    MCU_buffer_cache[d->tasklet_id][cache_index + (i << 3) + 4] = (b3 - b4) >> 4;
    MCU_buffer_cache[d->tasklet_id][cache_index + (i << 3) + 5] = (b2 - c8) >> 4;
    MCU_buffer_cache[d->tasklet_id][cache_index + (i << 3) + 6] = (b1 - b6) >> 4;
    MCU_buffer_cache[d->tasklet_id][cache_index + (i << 3) + 7] = (b0 - e7) >> 4;
  }
}

// https://en.wikipedia.org/wiki/YUV Y'UV444 to RGB888 conversion
static void ycbcr_to_rgb_pixel(JpegDecompressor *d, int cache_index, int v, int h) {
  int max_v = jpegInfo.max_v_samp_factor;
  int max_h = jpegInfo.max_h_samp_factor;

  // Iterating from bottom right to top left because otherwise the pixel data will get overwritten
  for (int y = 7; y >= 0; y--) {
    for (int x = 7; x >= 0; x--) {
      int pixel = cache_index + (y << 3) + x;
      int cbcr_pixel_row = y / max_v + 4 * v;
      int cbcr_pixel_col = x / max_h + 4 * h;
      int cbcr_pixel = (cbcr_pixel_row << 3) + cbcr_pixel_col + 64;

      short r =
          MCU_buffer_cache[d->tasklet_id][pixel] + ((45 * MCU_buffer_cache[d->tasklet_id][64 + cbcr_pixel]) >> 5) + 128;
      short g =
          MCU_buffer_cache[d->tasklet_id][pixel] -
          ((11 * MCU_buffer_cache[d->tasklet_id][cbcr_pixel] + 23 * MCU_buffer_cache[d->tasklet_id][64 + cbcr_pixel]) >>
           5) +
          128;
      short b =
          MCU_buffer_cache[d->tasklet_id][pixel] + ((113 * MCU_buffer_cache[d->tasklet_id][cbcr_pixel]) >> 6) + 128;

      if (r < 0)
        r = 0;
      if (r > 255)
        r = 255;
      if (g < 0)
        g = 0;
      if (g > 255)
        g = 255;
      if (b < 0)
        b = 0;
      if (b > 255)
        b = 255;

      MCU_buffer_cache[d->tasklet_id][pixel] = r;
      MCU_buffer_cache[d->tasklet_id][64 + pixel] = g;
      MCU_buffer_cache[d->tasklet_id][128 + pixel] = b;
    }
  }
}

static void inverse_dct_convert(JpegDecompressor *d) {
  int row = jpegInfoDpu.rows_per_mcu * d->tasklet_id;
  int end_row = jpegInfoDpu.rows_per_mcu * (d->tasklet_id + 1);
  if (row % 2 != 0) {
    row++;
  }
  if (end_row % 2 != 0) {
    end_row++;
  }
  if (d->tasklet_id == NR_TASKLETS - 1) {
    end_row = jpegInfo.mcu_height;
  }

  for (; row < end_row; row += jpegInfo.max_v_samp_factor) {
    for (int col = 0; col < jpegInfo.mcu_width; col += jpegInfo.max_h_samp_factor) {
      for (int color_index = 0; color_index < jpegInfo.num_color_components; color_index++) {
        for (int y = 0; y < jpegInfo.color_components[color_index].v_samp_factor; y++) {
          for (int x = 0; x < jpegInfo.color_components[color_index].h_samp_factor; x++) {
            int mcu_index = (((row + y) * jpegInfo.mcu_width_real + (col + x)) * 3 + color_index) << 6;
            int cache_index = ((y << 8) + (y << 7)) + ((x << 7) + (x << 6)) + (color_index << 6);
            mram_read(&MCU_buffer[0][mcu_index], &MCU_buffer_cache[d->tasklet_id][cache_index], MCU_READ_WRITE_SIZE);

            // Compute inverse DCT with ANN algorithm
            inverse_dct_component(d, cache_index);
          }
        }
      }

      // Convert from YCbCr to RGB
      for (int y = jpegInfo.max_v_samp_factor - 1; y >= 0; y--) {
        for (int x = jpegInfo.max_h_samp_factor - 1; x >= 0; x--) {
          // MCU to index is (current row + vertical sampling) * total number of MCUs in a row of the JPEG
          // + (current col + horizontal sampling)
          int mcu_index = (((row + y) * jpegInfo.mcu_width_real + (col + x)) * 3) << 6;
          int cache_index = ((y << 8) + (y << 7)) + ((x << 7) + (x << 6));

          ycbcr_to_rgb_pixel(d, cache_index, y, x);

          mram_write(&MCU_buffer_cache[d->tasklet_id][cache_index], &MCU_buffer[0][mcu_index], MCU_READ_WRITE_SIZE * 3);
        }
      }
    }
  }
}

#if DEBUG
static void print_jpeg_decompressor() {
  printf("\n********** DQT **********\n");
  for (int i = 0; i < 4; i++) {
    if (jpegInfo.quant_tables[i].exists) {
      printf("Table ID: %d", i);
      for (int j = 0; j < 64; j++) {
        if (j % 8 == 0) {
          printf("\n");
        }
        printf("%d ", jpegInfo.quant_tables[i].table[j]);
      }
      printf("\n\n");
    }
  }

  printf("********** DRI **********\n");
  printf("Restart Interval: %d\n", jpegInfo.restart_interval);

  printf("\n********** SOF **********\n");
  printf("Width: %d\n", jpegInfo.image_width);
  printf("Height: %d\n", jpegInfo.image_height);
  printf("Number of color components: %d\n\n", jpegInfo.num_color_components);
  for (int i = 0; i < jpegInfo.num_color_components; i++) {
    printf("Component ID: %d\n", jpegInfo.color_components[i].component_id);
    printf("H-samp factor: %d\n", jpegInfo.color_components[i].h_samp_factor);
    printf("V-samp factor: %d\n", jpegInfo.color_components[i].v_samp_factor);
    printf("Quantization table ID: %d\n\n", jpegInfo.color_components[i].quant_table_id);
  }

  printf("\n********** DHT **********\n");
  for (int i = 0; i < MAX_HUFFMAN_TABLES; i++) {
    if (jpegInfo.dc_huffman_tables[i].exists) {
      printf("DC Table ID: %d\n", i);
      for (int j = 0; j < 16; j++) {
        printf("%d: ", j + 1);
        for (int k = jpegInfo.dc_huffman_tables[i].valoffset[j]; k < jpegInfo.dc_huffman_tables[i].valoffset[j + 1];
             k++) {
          printf("%d ", jpegInfo.dc_huffman_tables[i].huffval[k]);
        }
        printf("\n");
      }
      printf("\n");
    }
  }
  for (int i = 0; i < MAX_HUFFMAN_TABLES; i++) {
    if (jpegInfo.ac_huffman_tables[i].exists) {
      printf("AC Table ID: %d\n", i);
      for (int j = 0; j < 16; j++) {
        printf("%d: ", j + 1);
        for (int k = jpegInfo.ac_huffman_tables[i].valoffset[j]; k < jpegInfo.ac_huffman_tables[i].valoffset[j + 1];
             k++) {
          printf("%d ", jpegInfo.ac_huffman_tables[i].huffval[k]);
        }
        printf("\n");
      }
      printf("\n");
    }
  }

  printf("\n********** SOS **********\n");
  for (int i = 0; i < jpegInfo.num_color_components; i++) {
    printf("Component ID: %d\n", jpegInfo.color_components[i].component_id);
    printf("DC table ID: %d\n", jpegInfo.color_components[i].dc_huffman_table_id);
    printf("AC table ID: %d\n\n", jpegInfo.color_components[i].ac_huffman_table_id);
  }
  printf("Start of selection: %d\n", jpegInfo.ss);
  printf("End of selection: %d\n", jpegInfo.se);
  printf("Successive approximation high: %d\n", jpegInfo.Ah);
  printf("Successive approximation low: %d\n\n", jpegInfo.Al);

  printf("\n********** BMP **********\n");
  printf("MCU width: %d\n", jpegInfo.mcu_width);
  printf("MCU height: %d\n", jpegInfo.mcu_height);
  printf("BMP padding: %d\n", jpegInfo.padding);
}
#endif

static void init_jpeg_info() {
  jpegInfo.valid = 1;

  for (int i = 0; i < 4; i++) {
    jpegInfo.quant_tables[i].exists = 0;

    if (i < 2) {
      jpegInfo.color_components[i].exists = 0;
      jpegInfo.dc_huffman_tables[i].exists = 0;
      jpegInfo.ac_huffman_tables[i].exists = 0;

    } else if (i < 3) {
      jpegInfo.color_components[i].exists = 0;
    }
  }

  jpegInfo.restart_interval = 0;
  jpegInfo.image_height = 0;
  jpegInfo.image_width = 0;
  jpegInfo.num_color_components = 0;
  jpegInfo.ss = 0;
  jpegInfo.se = 0;
  jpegInfo.Ah = 0;
  jpegInfo.Al = 0;

  jpegInfo.mcu_width = 0;
  jpegInfo.mcu_height = 0;
  jpegInfo.padding = 0;

  for (int i = 0; i < NR_TASKLETS; i++) {
    jpegInfoDpu.mcu_end_index[i] = 0;
    jpegInfoDpu.mcu_start_index[i] = 0;
    if (i < NR_TASKLETS - 1) {
      jpegInfoDpu.dc_offset[i][0] = 0;
      jpegInfoDpu.dc_offset[i][1] = 0;
      jpegInfoDpu.dc_offset[i][2] = 0;
    }
  }
}

int main() {
  JpegDecompressor decompressor;
  decompressor.length = file_length;
  decompressor.tasklet_id = me();
  jpegInfo.length = decompressor.length;

  if (decompressor.tasklet_id == 0) {
    int result = 1;

    init_file_reader_index(&decompressor);
    init_jpeg_info();

    int not_jpeg = check_start_of_image(&decompressor);
    if (not_jpeg) {
      return 1;
    }

    // Continuously read all markers until we reach Huffman coded bitstream
    while (jpegInfo.valid && result) {
      result = read_next_marker(&decompressor);
    }

    if (!jpegInfo.valid) {
      return 1;
    }

    jpegInfo.image_data_start = decompressor.file_index + decompressor.cache_index;
    jpegInfo.size_per_tasklet = (jpegInfo.length - jpegInfo.image_data_start + (NR_TASKLETS - 1)) / NR_TASKLETS;

    output.image_width = jpegInfo.image_width;
    output.image_height = jpegInfo.image_height;
    output.padding = jpegInfo.padding;
    output.mcu_width_real = jpegInfo.mcu_width_real;

#if DEBUG
    print_jpeg_decompressor();
#endif
  }

  // All tasklets should wait until tasklet 0 has finished reading all JPEG markers
  barrier_wait(&init_barrier);

  init_jpeg_decompressor(&decompressor);

  // Process Huffman coded bitstream, perform inverse DCT, and convert YCbCr to RGB
  decompress_scanline(&decompressor);
  if (!jpegInfo.valid) {
    return 1;
  }

  // All tasklets should wait until tasklet 0 has finished adjusting the DC coefficients
  barrier_wait(&idct_barrier);

  inverse_dct_convert(&decompressor);

  return 0;
}
