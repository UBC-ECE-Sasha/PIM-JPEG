#include <barrier.h>
#include <defs.h>
#include <mram.h>
#include <stdio.h>

#include "jpeg-common.h"
#include "jpeg-host.h"

__host uint64_t file_length;
__host dpu_output_t output;
__mram_noinit char file_buffer[16 << 20];
// About 32MB
__mram_noinit short MCU_buffer[NR_TASKLETS][16776960 / NR_TASKLETS];

JpegInfo jpegInfo;

BARRIER_INIT(init_barrier, NR_TASKLETS);
BARRIER_INIT(idct_barrier, NR_TASKLETS);

#define PREFETCH_SIZE 1024
#define PREWRITE_SIZE 768
__dma_aligned char file_buffer_cache[NR_TASKLETS][PREFETCH_SIZE];
__dma_aligned short MCU_buffer_cache[NR_TASKLETS][PREWRITE_SIZE];
#define MCU_READ_WRITE_SIZE 128

#define INDEX_OFFSET 64
#define DC_COEFF_OFFSET 192

#define DEBUG 0

/**
 * Helper array for filling in quantization table in zigzag order
 */
const uint8_t ZIGZAG_ORDER[] = {0,  1,  8,  16, 9,  2,  3,  10, 17, 24, 32, 25, 18, 11, 4,  5,  12, 19, 26, 33, 40, 48,
                                41, 34, 27, 20, 13, 6,  7,  14, 21, 28, 35, 42, 49, 56, 57, 50, 43, 36, 29, 22, 15, 23,
                                30, 37, 44, 51, 58, 59, 52, 45, 38, 31, 39, 46, 53, 60, 61, 54, 47, 55, 62, 63};

/**
 * Check whether EOF is reached by comparing current file index is >= total length of file
 *
 * @param d JpegDecompressor struct that holds all information about the JPEG currently being decoded
 */
static int eof(JpegDecompressor *d) {
  return ((d->file_index + d->cache_index) >= d->length);
}

/**
 * Helper function to read a byte from the file
 *
 * @param d JpegDecompressor struct that holds all information about the JPEG currently being decoded
 */
static uint8_t read_byte(JpegDecompressor *d) {
  if (d->cache_index >= PREFETCH_SIZE) {
    d->file_index += PREFETCH_SIZE;
    mram_read(&file_buffer[d->file_index], file_buffer_cache[d->tasklet_id], PREFETCH_SIZE);
    d->cache_index -= PREFETCH_SIZE;
  }

  uint8_t byte = file_buffer_cache[d->tasklet_id][d->cache_index++];
  return byte;
}

/**
 * Helper function to read 2 bytes from the file, MSB order
 *
 * @param d JpegDecompressor struct that holds all information about the JPEG currently being decoded
 */
static uint16_t read_short(JpegDecompressor *d) {
  uint8_t byte1 = read_byte(d);
  uint8_t byte2 = read_byte(d);

  uint16_t two_bytes = (byte1 << 8) | byte2;
  return two_bytes;
}

/**
 * Skip num_bytes bytes
 *
 * @param d JpegDecompressor struct that holds all information about the JPEG currently being decoded
 * @param num_bytes The number of bytes to skip
 */
static void skip_bytes(JpegDecompressor *d, int num_bytes) {
  // If after skipping num_bytes bytes we go beyond EOF, then only skip till EOF
  if (d->file_index + d->cache_index + num_bytes >= d->length) {
    d->file_index = d->length;
    d->cache_index = 0;
  } else {
    int offset = d->cache_index + num_bytes;
    if (offset >= PREFETCH_SIZE) {
      while (offset >= PREFETCH_SIZE) {
        offset -= PREFETCH_SIZE;
        d->file_index += PREFETCH_SIZE;
      }
      mram_read(&file_buffer[d->file_index], file_buffer_cache[d->tasklet_id], PREFETCH_SIZE);
    }
    d->cache_index = offset;
  }
}

/**
 * Skip over an unknown or uninteresting variable-length marker like APPN or COM
 *
 * @param d JpegDecompressor struct that holds all information about the JPEG currently being decoded
 */
static int skip_marker(JpegDecompressor *d) {
  int length = read_short(d);
  length -= 2;

  // Length includes itself, so must be at least 2
  if (length < 0) {
    jpegInfo.valid = 0;
    printf("ERROR: Invalid length encountered in skip_marker");
    return -1;
  }

  // Skip over the remaining bytes
  skip_bytes(d, length);

  return 0;
}

/**
 * Find the next JPEG marker (byte with value FF). Swallow consecutive duplicate FF bytes
 *
 * @param d JpegDecompressor struct that holds all information about the JPEG currently being decoded
 */
static int next_marker(JpegDecompressor *d) {
  int discarded_bytes = 0;

  uint8_t byte = read_byte(d);
  while (byte != 0xFF) {
    if (eof(d)) {
      return -1;
    }
    discarded_bytes++;
    byte = read_byte(d);
  }

  // Get marker code byte, swallowing any duplicate FF bytes.
  // Extra FFs are legal as pad bytes, so don't count them in discarded_bytes.
  int marker;
  do {
    if (eof(d)) {
      return -1;
    }
    marker = read_byte(d);
  } while (marker == 0xFF);

  if (discarded_bytes) {
    printf("WARNING: Discarded %u bytes\n", discarded_bytes);
  }

  return marker;
}

/**
 * Read whether we are at valid START OF IMAGE
 *
 * @param d JpegDecompressor struct that holds all information about the JPEG currently being decoded
 */
static void check_start_of_image(JpegDecompressor *d) {
  uint8_t c1 = 0, c2 = 0;

  if (!eof(d)) {
    c1 = read_byte(d);
    c2 = read_byte(d);
  }
  if (c1 != 0xFF || c2 != M_SOI) {
    jpegInfo.valid = 0;
    printf("Error: Not JPEG: %X %X\n", c1, c2);
  }
}

/**
 * Read Quantization Table
 * Page 39: Section B.2.4.1
 *
 * @param d JpegDecompressor struct that holds all information about the JPEG currently being decoded
 */
static void process_DQT(JpegDecompressor *d) {
  int length = read_short(d); // Lq
  length -= 2;

  while (length > 0) {
    uint8_t qt_info = read_byte(d);
    length -= 1;

    uint8_t table_id = qt_info & 0x0F; // Tq
    if (table_id > 3) {
      jpegInfo.valid = 0;
      printf("Error: Invalid DQT - got quantization table ID: %d, ID should be between 0 and 3\n", table_id);
      return;
    }
    jpegInfo.quant_tables[table_id].exists = 1;

    uint8_t precision = (qt_info >> 4) & 0x0F; // Pq
    if (precision == 0) {
      // 8 bit precision
      for (int i = 0; i < 64; i++) {
        jpegInfo.quant_tables[table_id].table[ZIGZAG_ORDER[i]] = read_byte(d); // Qk
      }
      length -= 64;
    } else {
      // 16 bit precision
      for (int i = 0; i < 64; i++) {
        jpegInfo.quant_tables[table_id].table[ZIGZAG_ORDER[i]] = read_short(d); // Qk
      }
      length -= 128;
    }
  }

  if (length != 0) {
    jpegInfo.valid = 0;
    printf("Error: Invalid DQT - length incorrect\n");
  }
}

/**
 * Read Restart Interval
 * Page 43: Section B.2.4.4
 *
 * @param d JpegDecompressor struct that holds all information about the JPEG currently being decoded
 */
static void process_DRI(JpegDecompressor *d) {
  int length = read_short(d); // Lr
  if (length != 4) {
    jpegInfo.valid = 0;
    printf("Error: Invalid DRI - length is not 4\n");
    return;
  }

  jpegInfo.restart_interval = read_short(d); // Ri
}

/**
 * Read Start Of Frame
 * Page 35: Section B.2.2
 *
 * @param d JpegDecompressor struct that holds all information about the JPEG currently being decoded
 */
static void process_SOFn(JpegDecompressor *d) {
  if (jpegInfo.num_color_components != 0) {
    jpegInfo.valid = 0;
    printf("Error: Invalid SOF - multiple SOFs encountered\n");
    return;
  }

  int length = read_short(d); // Lf

  uint8_t precision = read_byte(d); // P
  if (precision != 8) {
    jpegInfo.valid = 0;
    printf("Error: Invalid SOF - precision is %d, should be 8\n", precision);
    return;
  }

  jpegInfo.image_height = read_short(d); // Y
  jpegInfo.image_width = read_short(d);  // X
  if (jpegInfo.image_height == 0 || jpegInfo.image_width == 0) {
    jpegInfo.valid = 0;
    printf("Error: Invalid SOF - dimensions: %d x %d\n", jpegInfo.image_width, jpegInfo.image_height);
    return;
  }

  jpegInfo.mcu_height = (jpegInfo.image_height + 7) / 8;
  jpegInfo.mcu_width = (jpegInfo.image_width + 7) / 8;
  jpegInfo.padding = jpegInfo.image_width % 4;
  jpegInfo.mcu_height_real = jpegInfo.mcu_height;
  jpegInfo.mcu_width_real = jpegInfo.mcu_width;

  // Should be 3
  jpegInfo.num_color_components = read_byte(d); // Nf
  if (jpegInfo.num_color_components == 0 || jpegInfo.num_color_components > 3) {
    jpegInfo.valid = 0;
    printf("Error: Invalid SOF - number of color components: %d\n", jpegInfo.num_color_components);
    return;
  }

  for (int i = 0; i < jpegInfo.num_color_components; i++) {
    // The component ID is expected to be 1, 2, or 3
    uint8_t component_id = read_byte(d); // Ci
    if (component_id == 0 || component_id > 3) {
      jpegInfo.valid = 0;
      printf("Error: Invalid SOF - component ID: %d\n", component_id);
      return;
    }

    ColorComponentInfo *component = &jpegInfo.color_components[component_id - 1];
    component->exists = 1;
    component->component_id = component_id;

    uint8_t factor = read_byte(d);
    component->h_samp_factor = (factor >> 4) & 0x0F; // Hi
    component->v_samp_factor = factor & 0x0F;        // Vi
    if (component_id == 1) {
      // Only luminance channel can have horizontal or vertical sampling factor greater than 1
      if ((component->h_samp_factor != 1 && component->h_samp_factor != 2) ||
          (component->v_samp_factor != 1 && component->v_samp_factor != 2)) {
        jpegInfo.valid = 0;
        printf("Error: Invalid SOF - horizontal or vertical sampling factor for luminance out of range %d %d\n",
               component->h_samp_factor, component->v_samp_factor);
        return;
      }

      if (component->h_samp_factor == 2 && jpegInfo.mcu_width % 2 == 1) {
        // Add padding to real MCU width if horizontal sampling factor is 2
        jpegInfo.mcu_width_real++;
      }
      if (component->v_samp_factor == 2 && jpegInfo.mcu_height % 2 == 1) {
        // Add padding to real MCU height if vertical sampling factor is 2
        jpegInfo.mcu_height_real++;
      }

      jpegInfo.max_h_samp_factor = component->h_samp_factor;
      jpegInfo.max_v_samp_factor = component->v_samp_factor;
    } else if (component->h_samp_factor != 1 || component->v_samp_factor != 1) {
      jpegInfo.valid = 0;
      printf("Error: Invalid SOF - horizontal and vertical sampling factor for Cr and Cb not 1");
      return;
    }

    component->quant_table_id = read_byte(d); // Tqi
  }
  jpegInfo.rows_per_mcu = jpegInfo.mcu_height_real / NR_TASKLETS;

  if (length - 8 - (3 * jpegInfo.num_color_components) != 0) {
    jpegInfo.valid = 0;
    printf("Error: Invalid SOF - length incorrect\n");
  }
}

/**
 * Read Huffman tables
 * Page 40: Section B.2.4.2
 *
 * @param d JpegDecompressor struct that holds all information about the JPEG currently being decoded
 */
static void process_DHT(JpegDecompressor *d) {
  int length = read_short(d); // Lf
  length -= 2;

  // Keep reading Huffman tables until we run out of data
  while (length > 0) {
    uint8_t ht_info = read_byte(d);
    length -= 1;

    uint8_t table_id = ht_info & 0x0F;        // Th
    uint8_t ac_table = (ht_info >> 4) & 0x0F; // Tc
    if (table_id > 3) {
      jpegInfo.valid = 0;
      printf("Error: Invalid DHT - Huffman Table ID: %d\n", table_id);
      return;
    }

    HuffmanTable *h_table = ac_table ? &jpegInfo.ac_huffman_tables[table_id] : &jpegInfo.dc_huffman_tables[table_id];
    h_table->exists = 1;

    h_table->valoffset[0] = 0;
    int total = 0;
    for (int i = 1; i <= 16; i++) {
      total += read_byte(d); // Li
      h_table->valoffset[i] = total;
    }
    length -= 16;

    for (int i = 0; i < total; i++) {
      h_table->huffval[i] = read_byte(d); // Vij
    }
    length -= total;
  }

  if (length != 0) {
    jpegInfo.valid = 0;
    printf("Error: Invalid DHT - length incorrect\n");
  }
}

/**
 * Helper function for actually generating the Huffman codes
 *
 * @param h_table HuffmanTable struct that holds all the Huffman values and offsets
 */
static void generate_codes(HuffmanTable *h_table) {
  uint32_t code = 0;
  for (int i = 0; i < 16; i++) {
    for (int j = h_table->valoffset[i]; j < h_table->valoffset[i + 1]; j++) {
      h_table->codes[j] = code;
      code++;
    }
    code <<= 1;
  }
}

/**
 * Generate the Huffman codes for Huffman tables
 *
 * @param d JpegDecompressor struct that holds all information about the JPEG currently being decoded
 */
static void build_huffman_tables(JpegDecompressor *d) {
  for (int i = 0; i < MAX_HUFFMAN_TABLES; i++) {
    if (jpegInfo.dc_huffman_tables[i].exists) {
      generate_codes(&jpegInfo.dc_huffman_tables[i]);
    }
    if (jpegInfo.ac_huffman_tables[i].exists) {
      generate_codes(&jpegInfo.ac_huffman_tables[i]);
    }
  }
}

/**
 * Read Start Of Scan
 * Page 37: Section B.2.3
 *
 * @param d JpegDecompressor struct that holds all information about the JPEG currently being decoded
 */
static void process_SOS(JpegDecompressor *d) {
  int length = read_short(d); // Ls
  length -= 2;

  uint8_t num_components = read_byte(d); // Ns
  length -= 1;
  if (num_components == 0 || num_components != jpegInfo.num_color_components) {
    jpegInfo.valid = 0;
    printf("Error: Invalid SOS - number of color components does not match SOF: %d vs %d\n", num_components,
           jpegInfo.num_color_components);
    return;
  }

  for (int i = 0; i < num_components; i++) {
    uint8_t component_id = read_byte(d); // Csj
    if (component_id == 0 || component_id > 3) {
      jpegInfo.valid = 0;
      printf("Error: Invalid SOS - component ID: %d\n", component_id);
      return;
    }

    ColorComponentInfo *component = &jpegInfo.color_components[component_id - 1];
    uint8_t tdta = read_byte(d);
    component->dc_huffman_table_id = (tdta >> 4) & 0x0F; // Tdj
    component->ac_huffman_table_id = tdta & 0x0F;        // Taj
  }

  jpegInfo.ss = read_byte(d); // Ss
  jpegInfo.se = read_byte(d); // Se
  uint8_t A = read_byte(d);
  jpegInfo.Ah = (A >> 4) & 0xF; // Ah
  jpegInfo.Al = A & 0xF;        // Al
  length -= 3;

  if (jpegInfo.ss != 0 || jpegInfo.se != 63) {
    jpegInfo.valid = 0;
    printf("Error: Invalid SOS - invalid spectral selection\n");
    return;
  }
  if (jpegInfo.Ah != 0 || jpegInfo.Al != 0) {
    jpegInfo.valid = 0;
    printf("Error: Invalid SOS - invalid successive approximation\n");
    return;
  }

  if (length - (2 * num_components) != 0) {
    jpegInfo.valid = 0;
    printf("Error: Invalid SOS - length incorrect\n");
  }

  build_huffman_tables(d);
}

/**
 * Get the number of specified bits from Huffman coded bitstream
 *
 * @param d JpegDecompressor struct that holds all information about the JPEG currently being decoded
 * @param num_bits The number of bits to read from the bitstream
 */
static int get_bits(JpegDecompressor *d, int num_bits) {
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

/**
 * Read Huffman coded bitstream bit by bit to form a Huffman code, then return the value which matches
 * this code. Value is read from the Huffman table formed when reading in the JPEG header
 *
 * @param d JpegDecompressor struct that holds all information about the JPEG currently being decoded
 * @param h_table HuffmanTable struct that holds all the Huffman codes and values
 */
static uint8_t huff_decode(JpegDecompressor *d, HuffmanTable *h_table) {
  uint32_t code = 0;

  for (int i = 0; i < 16; i++) {
    int bit = get_bits(d, 1);
    code = (code << 1) | bit;
    for (int j = h_table->valoffset[i]; j < h_table->valoffset[i + 1]; j++) {
      if (code == h_table->codes[j]) {
        return h_table->huffval[j];
      }
    }
  }

  return -1;
}

/**
 * F.2.1.2 Decode 8x8 block data unit
 *
 * @param d JpegDecompressor struct that holds all information about the JPEG currently being decoded
 * @param component_index The component index specifies which color channel to use (Y, Cb, Cr),
 *                        used to determine which quantization table and Huffman tables to use
 * @param previous_dc The value of the previous DC coefficient, needed to calculate value of current DC coefficient
 */
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

  int coeff = get_bits(d, dc_length);
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
      coeff = get_bits(d, coeff_length);
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

/**
 * Decode the Huffman coded bitstream
 *
 * @param d JpegDecompressor struct that holds all information about the JPEG currently being decoded
 */
static void decompress_scanline(JpegDecompressor *d) {
  short previous_dcs[3] = {0};
  int restart_interval = jpegInfo.restart_interval * jpegInfo.max_h_samp_factor * jpegInfo.max_v_samp_factor;
  int row, col;
  int synch_mcu_index = 0;

  for (row = 0; row < jpegInfo.mcu_height; row += jpegInfo.max_v_samp_factor) {
    for (col = 0; col < jpegInfo.mcu_width; col += jpegInfo.max_h_samp_factor) {
      if (eof(d)) {
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
    jpegInfo.mcu_end_index[d->tasklet_id] = current_mcu_index;
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
        jpegInfo.mcu_end_index[d->tasklet_id] = (row * jpegInfo.mcu_width_real + col) * 192;
        int blocks_elapsed =
            (next_tasklet_mcu_blocks_elapsed / minimum_synched_mcu_blocks) * jpegInfo.max_h_samp_factor;
        jpegInfo.mcu_start_index[d->tasklet_id + 1] = blocks_elapsed * 192;
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
                jpegInfo.dc_offset[d->tasklet_id][color_index] =
                    MCU_buffer_cache[d->tasklet_id][0] -
                    MCU_buffer_cache[d->tasklet_id + 1][DC_COEFF_OFFSET + next_tasklet_mcu_blocks_elapsed];
                num_synched_mcu_blocks++;
              }
            } else {
              jpegInfo.dc_offset[d->tasklet_id][color_index] =
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
  int start_index = jpegInfo.mcu_start_index[tasklet_index] / 192;
  int tasklet_row = start_index / jpegInfo.mcu_width_real;
  int tasklet_col = start_index % jpegInfo.mcu_width_real;
  int dc_offset[3] = {jpegInfo.dc_offset[0][0], jpegInfo.dc_offset[0][1], jpegInfo.dc_offset[0][2]};

  for (; row < jpegInfo.mcu_height; row += jpegInfo.max_v_samp_factor) {
    for (; col < jpegInfo.mcu_width; col += jpegInfo.max_h_samp_factor, tasklet_col += jpegInfo.max_h_samp_factor) {
      if (tasklet_col >= jpegInfo.mcu_width) {
        tasklet_col = 0;
        tasklet_row += jpegInfo.max_v_samp_factor;
      }
      if ((tasklet_row * jpegInfo.mcu_width_real + tasklet_col) * 192 >= jpegInfo.mcu_end_index[tasklet_index]) {
        tasklet_index++;
        start_index = jpegInfo.mcu_start_index[tasklet_index] / 192;
        tasklet_row = start_index / jpegInfo.mcu_width_real;
        tasklet_col = start_index % jpegInfo.mcu_width_real;
        dc_offset[0] += jpegInfo.dc_offset[tasklet_index - 1][0];
        dc_offset[1] += jpegInfo.dc_offset[tasklet_index - 1][1];
        dc_offset[2] += jpegInfo.dc_offset[tasklet_index - 1][2];
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

/**
 * Function to perform inverse DCT for one of the color components of an MCU using integers only
 *
 * @param d JpegDecompressor struct that holds all information about the JPEG currently being decoded
 * @param cache_index The cache index specifies which cache block to index into in the MCU_buffer_cache
 */
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

/**
 * Function to perform conversion from YCbCr to RGB for the 64 pixels within an MCU
 * https://en.wikipedia.org/wiki/YUV Y'UV444 to RGB888 conversion
 *
 * @param d JpegDecompressor struct that holds all information about the JPEG currently being decoded
 * @param cache_index The cache index specifies which cache block to index into in the MCU_buffer_cache
 * @param v Determines whether to read from second vertical half of CbCr buffer
 * @param h Determines whether to read from second horizontal half of CbCr buffer
 */
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

/**
 * Compute inverse DCT, and convert from YCbCr to RGB
 *
 * @param d JpegDecompressor struct that holds all information about the JPEG currently being decoded
 */
static void inverse_dct_convert(JpegDecompressor *d) {
  int row = jpegInfo.rows_per_mcu * d->tasklet_id;
  int end_row = jpegInfo.rows_per_mcu * (d->tasklet_id + 1);
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

/**
 * Read JPEG markers
 * Return 0 when the SOS marker is found
 * Otherwise return 1 for failure
 *
 * @param d JpegDecompressor struct that holds all information about the JPEG currently being decoded
 */
static int read_marker(JpegDecompressor *d) {
  int marker;

  marker = next_marker(d);
  switch (marker) {
    case -1:
      jpegInfo.valid = 0;
      printf("Error: Read past EOF\n");
      break;

    case M_APP_FIRST ... M_APP_LAST:
      skip_marker(d);
      break;

    case M_DQT:
      process_DQT(d);
      break;

    case M_DRI:
      process_DRI(d);
      break;

    case M_SOF0:
      // case M_SOF5 ... M_SOF7:
      // case M_SOF9 ... M_SOF11:
      // case M_SOF13 ... M_SOF15:
      process_SOFn(d);
      break;

    case M_SOF2:
      // TODO: handle progressive JPEG
      jpegInfo.valid = 0;
      break;

    case M_DHT:
      process_DHT(d);
      break;

    case M_SOS:
      // Return 0 when we find the SOS marker
      process_SOS(d);
      return 0;

    case M_COM:
    case M_EXT_FIRST ... M_EXT_LAST:
    case M_DNL:
    case M_DHP:
    case M_EXP:
      skip_marker(d);
      break;

    default:
      jpegInfo.valid = 0;
      printf("Error: Unhandled marker: FF %X\n", marker);
      break;
  }

  return 1;
}

#if DEBUG
/**
 * Helper function to print out filled in values of the JPEG decompressor
 */
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
  for (int i = 0; i < 2; i++) {
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
  for (int i = 0; i < 2; i++) {
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

/**
 * Initialize global JPEG info with default values
 */
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

  // These fields will be filled when reading the JPEG header information
  jpegInfo.restart_interval = 0;
  jpegInfo.image_height = 0;
  jpegInfo.image_width = 0;
  jpegInfo.num_color_components = 0;
  jpegInfo.ss = 0;
  jpegInfo.se = 0;
  jpegInfo.Ah = 0;
  jpegInfo.Al = 0;

  // These fields will be used when writing to BMP
  jpegInfo.mcu_width = 0;
  jpegInfo.mcu_height = 0;
  jpegInfo.padding = 0;

  for (int i = 0; i < NR_TASKLETS; i++) {
    jpegInfo.mcu_end_index[i] = 0;
    jpegInfo.mcu_start_index[i] = 0;
    if (i < NR_TASKLETS - 1) {
      jpegInfo.dc_offset[i][0] = 0;
      jpegInfo.dc_offset[i][1] = 0;
      jpegInfo.dc_offset[i][2] = 0;
    }
  }
}

/**
 * Initialize JPEG decompressor with default values
 *
 * @param d JpegDecompressor struct that holds all information about the JPEG currently being decoded
 */
static void init_jpeg_decompressor(JpegDecompressor *d) {
  int file_index = jpegInfo.image_data_start + jpegInfo.size_per_tasklet * d->tasklet_id;
  // Calculating offset so that mram_read is 8 byte aligned
  int offset = file_index % 8;
  d->file_index = file_index - offset - PREFETCH_SIZE;
  d->cache_index = offset + PREFETCH_SIZE;
  d->length = jpegInfo.image_data_start + jpegInfo.size_per_tasklet * (d->tasklet_id + 1);
  if (d->length > jpegInfo.length) {
    d->length = jpegInfo.length;
  }

  // These fields will be used when decoding Huffman coded bitstream
  d->bit_buffer = 0;
  d->bits_left = 0;
}

int main() {
  JpegDecompressor decompressor;
  decompressor.length = file_length;
  decompressor.tasklet_id = me();
  jpegInfo.length = decompressor.length;

  if (decompressor.tasklet_id == 0) {
    int result = 1;

    decompressor.file_index = -PREFETCH_SIZE;
    decompressor.cache_index = PREFETCH_SIZE;

    init_jpeg_info();

    check_start_of_image(&decompressor);

    // Continuously read all markers until we reach Huffman coded bitstream
    while (jpegInfo.valid && result) {
      result = read_marker(&decompressor);
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
