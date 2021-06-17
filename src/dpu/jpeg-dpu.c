#include <barrier.h>
#include <defs.h>
#include <mram.h>
#include <stdio.h>

#include "jpeg-common.h"
#include "jpeg-host.h"

__host dpu_input_t input;
__host dpu_output_t output;
__mram_noinit char file_buffer[16 << 20];
// About 32MB
__mram_noinit short MCU_buffer[NR_TASKLETS][(87380 * 3 * 64) / NR_TASKLETS];

JpegInfo jpegInfo;

BARRIER_INIT(init_barrier, NR_TASKLETS);

// TODO: maybe seqread is faster?
#define PREFETCH_SIZE 1024
#define PREWRITE_SIZE 768
__dma_aligned char file_buffer_cache[NR_TASKLETS][PREFETCH_SIZE];
__dma_aligned short MCU_buffer_cache[NR_TASKLETS][PREWRITE_SIZE];

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
static int eof(JpegDecompressor *d) { return ((d->file_index + d->cache_index) >= d->length); }

/**
 * Helper function to read a byte from the file
 *
 * @param d JpegDecompressor struct that holds all information about the JPEG currently being decoded
 */
static uint8_t read_byte(JpegDecompressor *d) {
  uint8_t temp;

  if (d->cache_index >= PREFETCH_SIZE) {
    d->file_index += PREFETCH_SIZE;
    mram_read(&file_buffer[d->file_index], file_buffer_cache[d->tasklet_id], PREFETCH_SIZE);
    d->cache_index -= PREFETCH_SIZE;
  }

  temp = file_buffer_cache[d->tasklet_id][d->cache_index++];
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

  temp1 = read_byte(d);
  temp2 = read_byte(d);

  temp3 = (temp1 << 8) | temp2;
  return temp3;
}

/**
 * Skip count bytes
 *
 * @param d JpegDecompressor struct that holds all information about the JPEG currently being decoded
 * @param count The number of bytes to skip
 */
static void skip_bytes(JpegDecompressor *d, int count) {
  // If after skipping count bytes we go beyond EOF, then only skip till EOF
  if (d->file_index + d->cache_index + count >= d->length) {
    d->file_index = d->length;
    d->cache_index = 0;
  } else {
    int offset = d->cache_index + count;
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
  uint16_t length = read_short(d);
  // Length includes itself, so must be at least 2
  if (length < 2) {
    jpegInfo.valid = 0;
    printf("ERROR: Invalid length encountered in skip_marker\n");
    return -1;
  }

  length -= 2;
  // Skip over the remaining bytes
  skip_bytes(d, length);

  return 0;
}

/**
 * Find the next marker (byte with value FF). Swallow consecutive duplicate FF bytes
 *
 * @param d JpegDecompressor struct that holds all information about the JPEG currently being decoded
 */
static int next_marker(JpegDecompressor *d) {
  uint8_t byte;
  int marker;
  int discarded_bytes = 0;

  // Find 0xFF byte; count and skip any non-FFs
  byte = read_byte(d);
  while (byte != 0xFF) {
    if (eof(d))
      return -1;

    discarded_bytes++;
    byte = read_byte(d);
  }

  // Get marker code byte, swallowing any duplicate FF bytes.  Extra FFs
  // are legal as pad bytes, so don't count them in discarded_bytes.
  do {
    if (eof(d))
      return -1;

    marker = read_byte(d);
  } while (marker == 0xFF);

  if (discarded_bytes) {
    printf("WARNING: discarded %u bytes\n", discarded_bytes);
  }

  return marker;
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
    jpegInfo.valid = 0;
    printf("Error: Not JPEG: %X %X\n", c1, c2);
  } else {
    printf("Got SOI marker: %X %X\n", c1, c2);
  }
}

/**
 * Read Quantization Table
 * Page 40: Table B.4
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
 *
 * @param d JpegDecompressor struct that holds all information about the JPEG currently being decoded
 */
static void process_DRI(JpegDecompressor *d) {
  int length = read_short(d);
  if (length != 4) {
    jpegInfo.valid = 0;
    printf("Error: Invalid DRI - length is not 4\n");
    return;
  }

  jpegInfo.restart_interval = read_short(d);
}

/**
 * Read Start Of Frame
 * Page 36: Table B.2
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

  if (length - 8 - (3 * jpegInfo.num_color_components) != 0) {
    jpegInfo.valid = 0;
    printf("Error: Invalid SOF - length incorrect\n");
  }
}

/**
 * Read Huffman tables
 * Page 41: Table B.5
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
  for (int i = 0; i < 2; i++) {
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
 * Page 38: Table B.3
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
static int get_bits(JpegDecompressor *d, uint32_t num_bits) {
  uint8_t b;
  uint32_t c;
  int temp = 0;
  if (num_bits == 0) {
    return temp;
  }

  while (d->bits_left < num_bits) {
    // Read a byte and decode it, if it is 0xFF
    b = read_byte(d);
    c = b;

    while (b == 0xFF) {
      // FF may be padded with FFs, read as many FFs as necessary
      b = read_byte(d);
      if (b == 0) {
        // Got FF which is not a marker, save it to buffer
        c = 0xFF;
      } else if (b >= M_RST_FIRST && b <= M_RST_LAST) {
        // Got restart markers, ignore and read new byte
        b = read_byte(d);
        c = b;
      } else {
        c = b;
      }
    }

    // Add the new bits to the buffer (MSB aligned)
    d->get_buffer |= c << (32 - 8 - d->bits_left);
    d->bits_left += 8;
  }

  temp = d->get_buffer >> (32 - num_bits);
  d->get_buffer <<= num_bits;
  d->bits_left -= num_bits;
  return temp;
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
 * @param cache_index The cache index specifies which cache block to index into in the MCU_buffer_cache
 * @param component_index The component index specifies which color channel to use (Y, Cb, Cr),
 *                        used to determine which quantization table and Huffman tables to use
 * @param previous_dc The value of the previous DC coefficient, needed to calculate value of current DC coefficient
 */
static int decode_mcu(JpegDecompressor *d, uint32_t cache_index, uint32_t component_index, short *previous_dc) {
  QuantizationTable *q_table = &jpegInfo.quant_tables[jpegInfo.color_components[component_index].quant_table_id];
  HuffmanTable *dc_table = &jpegInfo.dc_huffman_tables[jpegInfo.color_components[component_index].dc_huffman_table_id];
  HuffmanTable *ac_table = &jpegInfo.ac_huffman_tables[jpegInfo.color_components[component_index].ac_huffman_table_id];

  // Get DC value for this MCU block
  uint8_t dc_length = huff_decode(d, dc_table);
  if (dc_length == (uint8_t)-1) {
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
  MCU_buffer_cache[d->tasklet_id][cache_index] = coeff + *previous_dc;
  *previous_dc = MCU_buffer_cache[d->tasklet_id][cache_index];
  // Dequantization
  MCU_buffer_cache[d->tasklet_id][cache_index] *= q_table->table[0];

  // Get the AC values for this MCU block
  int i = 1;
  while (i < 64) {
    uint8_t ac_length = huff_decode(d, ac_table);
    if (ac_length == (uint8_t)-1) {
      printf("Error: Invalid AC code\n");
      return -1;
    }

    // Got 0x00, fill remaining MCU block with 0s
    if (ac_length == 0x00) {
      while (i < 64) {
        MCU_buffer_cache[d->tasklet_id][cache_index + ZIGZAG_ORDER[i++]] = 0;
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
      MCU_buffer_cache[d->tasklet_id][cache_index + ZIGZAG_ORDER[i++]] = 0;
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
      MCU_buffer_cache[d->tasklet_id][cache_index + ZIGZAG_ORDER[i]] = coeff * q_table->table[ZIGZAG_ORDER[i]];
      i++;
    }
  }

  return 0;
}

/**
 * Function to perform inverse DCT for one of the color components of an MCU using integers only
 *
 * @param d JpegDecompressor struct that holds all information about the JPEG currently being decoded
 * @param cache_index The cache index specifies which cache block to index into in the MCU_buffer_cache
 */
static void inverse_dct_component(JpegDecompressor *d, uint32_t cache_index) {
  // ANN algorithm, intermediate values are bit shifted to the left to preserve precision
  // and then bit shifted to the right at the end
  for (int i = 0; i < 8; i++) {
    // Higher accuracy
    int g0 = (MCU_buffer_cache[d->tasklet_id][cache_index + 0 * 8 + i] * 181) >> 5;
    int g1 = (MCU_buffer_cache[d->tasklet_id][cache_index + 4 * 8 + i] * 181) >> 5;
    int g2 = (MCU_buffer_cache[d->tasklet_id][cache_index + 2 * 8 + i] * 59) >> 3;
    int g3 = (MCU_buffer_cache[d->tasklet_id][cache_index + 6 * 8 + i] * 49) >> 4;
    int g4 = (MCU_buffer_cache[d->tasklet_id][cache_index + 5 * 8 + i] * 71) >> 4;
    int g5 = (MCU_buffer_cache[d->tasklet_id][cache_index + 1 * 8 + i] * 251) >> 5;
    int g6 = (MCU_buffer_cache[d->tasklet_id][cache_index + 7 * 8 + i] * 25) >> 4;
    int g7 = (MCU_buffer_cache[d->tasklet_id][cache_index + 3 * 8 + i] * 213) >> 5;

    // Lower accuracy
    // int g0 = (MCU_buffer_cache[d->tasklet_id][cache_index + 0 * 8 + i] * 22) >> 2;
    // int g1 = (MCU_buffer_cache[d->tasklet_id][cache_index + 4 * 8 + i] * 22) >> 2;
    // int g2 = (MCU_buffer_cache[d->tasklet_id][cache_index + 2 * 8 + i] * 30) >> 2;
    // int g3 = (MCU_buffer_cache[d->tasklet_id][cache_index + 6 * 8 + i] * 12) >> 2;
    // int g4 = (MCU_buffer_cache[d->tasklet_id][cache_index + 5 * 8 + i] * 18) >> 2;
    // int g5 = (MCU_buffer_cache[d->tasklet_id][cache_index + 1 * 8 + i] * 31) >> 2;
    // int g6 = (MCU_buffer_cache[d->tasklet_id][cache_index + 7 * 8 + i] * 6) >> 2;
    // int g7 = (MCU_buffer_cache[d->tasklet_id][cache_index + 3 * 8 + i] * 27) >> 2;

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

    MCU_buffer_cache[d->tasklet_id][cache_index + 0 * 8 + i] = (b0 + e7) >> 4;
    MCU_buffer_cache[d->tasklet_id][cache_index + 1 * 8 + i] = (b1 + b6) >> 4;
    MCU_buffer_cache[d->tasklet_id][cache_index + 2 * 8 + i] = (b2 + c8) >> 4;
    MCU_buffer_cache[d->tasklet_id][cache_index + 3 * 8 + i] = (b3 + b4) >> 4;
    MCU_buffer_cache[d->tasklet_id][cache_index + 4 * 8 + i] = (b3 - b4) >> 4;
    MCU_buffer_cache[d->tasklet_id][cache_index + 5 * 8 + i] = (b2 - c8) >> 4;
    MCU_buffer_cache[d->tasklet_id][cache_index + 6 * 8 + i] = (b1 - b6) >> 4;
    MCU_buffer_cache[d->tasklet_id][cache_index + 7 * 8 + i] = (b0 - e7) >> 4;
  }

  for (int i = 0; i < 8; i++) {
    // Higher accuracy
    int g0 = (MCU_buffer_cache[d->tasklet_id][cache_index + i * 8 + 0] * 181) >> 5;
    int g1 = (MCU_buffer_cache[d->tasklet_id][cache_index + i * 8 + 4] * 181) >> 5;
    int g2 = (MCU_buffer_cache[d->tasklet_id][cache_index + i * 8 + 2] * 59) >> 3;
    int g3 = (MCU_buffer_cache[d->tasklet_id][cache_index + i * 8 + 6] * 49) >> 4;
    int g4 = (MCU_buffer_cache[d->tasklet_id][cache_index + i * 8 + 5] * 71) >> 4;
    int g5 = (MCU_buffer_cache[d->tasklet_id][cache_index + i * 8 + 1] * 251) >> 5;
    int g6 = (MCU_buffer_cache[d->tasklet_id][cache_index + i * 8 + 7] * 25) >> 4;
    int g7 = (MCU_buffer_cache[d->tasklet_id][cache_index + i * 8 + 3] * 213) >> 5;

    // Lower accuracy
    // int g0 = (MCU_buffer_cache[d->tasklet_id][cache_index + i * 8 + 0] * 22) >> 2;
    // int g1 = (MCU_buffer_cache[d->tasklet_id][cache_index + i * 8 + 4] * 22) >> 2;
    // int g2 = (MCU_buffer_cache[d->tasklet_id][cache_index + i * 8 + 2] * 30) >> 2;
    // int g3 = (MCU_buffer_cache[d->tasklet_id][cache_index + i * 8 + 6] * 12) >> 2;
    // int g4 = (MCU_buffer_cache[d->tasklet_id][cache_index + i * 8 + 5] * 18) >> 2;
    // int g5 = (MCU_buffer_cache[d->tasklet_id][cache_index + i * 8 + 1] * 31) >> 2;
    // int g6 = (MCU_buffer_cache[d->tasklet_id][cache_index + i * 8 + 7] * 6) >> 2;
    // int g7 = (MCU_buffer_cache[d->tasklet_id][cache_index + i * 8 + 3] * 27) >> 2;

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

    MCU_buffer_cache[d->tasklet_id][cache_index + i * 8 + 0] = (b0 + e7) >> 4;
    MCU_buffer_cache[d->tasklet_id][cache_index + i * 8 + 1] = (b1 + b6) >> 4;
    MCU_buffer_cache[d->tasklet_id][cache_index + i * 8 + 2] = (b2 + c8) >> 4;
    MCU_buffer_cache[d->tasklet_id][cache_index + i * 8 + 3] = (b3 + b4) >> 4;
    MCU_buffer_cache[d->tasklet_id][cache_index + i * 8 + 4] = (b3 - b4) >> 4;
    MCU_buffer_cache[d->tasklet_id][cache_index + i * 8 + 5] = (b2 - c8) >> 4;
    MCU_buffer_cache[d->tasklet_id][cache_index + i * 8 + 6] = (b1 - b6) >> 4;
    MCU_buffer_cache[d->tasklet_id][cache_index + i * 8 + 7] = (b0 - e7) >> 4;
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
static void ycbcr_to_rgb_pixel(JpegDecompressor *d, uint32_t cache_index, int v, int h) {
  int max_v = jpegInfo.max_v_samp_factor;
  int max_h = jpegInfo.max_h_samp_factor;

  // Iterating from bottom right to top left because otherwise the pixel data will get overwritten
  for (int y = 7; y >= 0; y--) {
    for (int x = 7; x >= 0; x--) {
      uint32_t pixel = cache_index + y * 8 + x;
      uint32_t cbcr_pixel_row = y / max_v + 4 * v;
      uint32_t cbcr_pixel_col = x / max_h + 4 * h;
      uint32_t cbcr_pixel = cbcr_pixel_row * 8 + cbcr_pixel_col + 64;

      // TODO: if multiplication is too slow, use bit shifting. However, bit shifting is less accurate from what I can
      // see int r = buffer[0][i] + buffer[2][i] + (buffer[2][i] >> 2) + (buffer[2][i] >> 3) + (buffer[2][i] >> 5) +
      // 128; int g = buffer[0][i] - ((buffer[1][i] >> 2) + (buffer[1][i] >> 4) + (buffer[1][i] >> 5)) -
      //         ((buffer[2][i] >> 1) + (buffer[2][i] >> 3) + (buffer[2][i] >> 4) + (buffer[2][i] >> 5)) + 128;
      // int b = buffer[0][i] + buffer[1][i] + (buffer[1][i] >> 1) + (buffer[1][i] >> 2) + (buffer[1][i] >> 6) + 128;

      // Integer only, quite accurate but may be less performant than only using bit shifting
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
 * Decode the Huffman coded bitstream, compute inverse DCT, and convert from YCbCr to RGB
 *
 * @param d JpegDecompressor struct that holds all information about the JPEG currently being decoded
 */
static void decompress_scanline(JpegDecompressor *d) {
  short previous_dcs[3] = {0};
  uint32_t restart_interval = jpegInfo.restart_interval * jpegInfo.max_h_samp_factor * jpegInfo.max_v_samp_factor;
  uint32_t row, col;
  int i = 0;

  for (row = 0; row < jpegInfo.mcu_height; row += jpegInfo.max_v_samp_factor) {
    for (col = 0; col < jpegInfo.mcu_width; col += jpegInfo.max_h_samp_factor) {
      if (eof(d)) {
        goto sync0;
      }

      for (uint32_t index = 0; index < 3; index++) {
        for (uint32_t y = 0; y < jpegInfo.color_components[index].v_samp_factor; y++) {
          for (uint32_t x = 0; x < jpegInfo.color_components[index].h_samp_factor; x++) {
            // uint32_t cache_index = (y * 384) + (x * 192) + (index * 64);
            uint32_t cache_index = 0;

            // if (decode_mcu(d, cache_index, index, &previous_dcs[index]) != 0) {
            //   jpegInfo.valid = 0;
            //   printf("Error: Invalid MCU\n");
            //   return;
            // }

            // Decode Huffman coded bitstream
            while (decode_mcu(d, cache_index, index, &previous_dcs[index]) != 0) {
              printf("Tasklet: %d, row: %d, col: %d, index: %d, y: %d, x: %d\n", d->tasklet_id, row, col, index, y, x);
            }

            if (i < 64) {
              MCU_buffer_cache[d->tasklet_id][64 + i] = d->file_index + d->cache_index;
              MCU_buffer_cache[d->tasklet_id][128 + i] = MCU_buffer_cache[d->tasklet_id][0];
              i++;
            }

            uint32_t mcu_index = (((row + y) * jpegInfo.mcu_width_real + (col + x)) * 3 + index) * 64;
            mram_write(&MCU_buffer_cache[d->tasklet_id][cache_index], &MCU_buffer[d->tasklet_id][mcu_index], 128);
          }
        }
      }

      // Convert from YCbCr to RGB
      // for (int y = jpegInfo.max_v_samp_factor - 1; y >= 0; y--) {
      //   for (int x = jpegInfo.max_h_samp_factor - 1; x >= 0; x--) {
      //     // MCU to index is (current row + vertical sampling) * total number of MCUs in a row of the JPEG
      //     // + (current col + horizontal sampling)
      //     uint32_t mcu_index = ((row + y) * jpegInfo.mcu_width_real + (col + x)) * 3 * 64;
      //     uint32_t cache_index = (y * 384) + (x * 192);

      //     ycbcr_to_rgb_pixel(d, cache_index, y, x);

      //     mram_write(&MCU_buffer_cache[d->tasklet_id][cache_index], &MCU_buffer[d->tasklet_id][mcu_index], 384);
      //   }
      // }
    }
  }

sync0:;
  // Tasklet i has to overflow to MCUs decoded by tasklet i + 1 for synchronisation
  printf("Tasklet %d got here\n", d->tasklet_id);

  // The last tasklet cannot overflow, so it returns first
  if (d->tasklet_id == NR_TASKLETS - 1) {
    jpegInfo.mcu_end_index[d->tasklet_id] = (row * jpegInfo.mcu_width_real + col) * 192;
    return;
  }

  i = 0;
  int j = 0;
  int counter = 0;
  int temp = jpegInfo.max_h_samp_factor * jpegInfo.max_v_samp_factor + 2;

  for (; row < jpegInfo.mcu_height; row += jpegInfo.max_v_samp_factor) {
    for (; col < jpegInfo.mcu_width; col += jpegInfo.max_h_samp_factor) {
      if (counter >= temp + 1) {
        jpegInfo.mcu_end_index[d->tasklet_id] = (row * jpegInfo.mcu_width_real + col) * 192;
        jpegInfo.mcu_start_index[d->tasklet_id + 1] = i * 64;
        goto sync1;
      }

      for (uint32_t index = 0; index < 3; index++) {
        for (uint32_t y = 0; y < jpegInfo.color_components[index].v_samp_factor; y++) {
          for (uint32_t x = 0; x < jpegInfo.color_components[index].h_samp_factor; x++) {
            uint32_t cache_index = 0;
            while (decode_mcu(d, cache_index, index, &previous_dcs[index]) != 0) {
              printf("This shouldn't ever print\n");
            }

            short index0 = d->file_index + d->cache_index;
            short index1 = MCU_buffer_cache[d->tasklet_id + 1][64 + i];

            uint32_t mcu_index = (((row + y) * jpegInfo.mcu_width_real + (col + x)) * 3 + index) * 64;
            mram_write(&MCU_buffer_cache[d->tasklet_id][cache_index], &MCU_buffer[d->tasklet_id][mcu_index], 128);

            if (index0 < index1) {
              counter = 0;
            } else if (index0 > index1) {
              counter = 0;
              i++;

              while (index0 > index1) {
                index1 = MCU_buffer_cache[d->tasklet_id + 1][64 + i];
                i++;
              }

              if (index0 == index1) {
                jpegInfo.dc_offset[d->tasklet_id][index] =
                    MCU_buffer_cache[d->tasklet_id][0] - MCU_buffer_cache[d->tasklet_id + 1][128 + i];
                counter++;
              }
            } else {
              jpegInfo.dc_offset[d->tasklet_id][index] =
                  MCU_buffer_cache[d->tasklet_id][0] - MCU_buffer_cache[d->tasklet_id + 1][128 + i];

              counter++;
              i++;
            }
          }
        }
      }
    }
  }

sync1:;
  // Tasklet 0 does a one pass through all MCUs to adjust DC coefficients
  if (d->tasklet_id != 0) {
    return;
  }

  uint32_t index = jpegInfo.mcu_end_index[0];
  int dc_offset[3] = {0};
  for (i = 1; i < NR_TASKLETS; i++) {
    int k = 0;
    dc_offset[0] += jpegInfo.dc_offset[i - 1][0];
    dc_offset[1] += jpegInfo.dc_offset[i - 1][1];
    dc_offset[2] += jpegInfo.dc_offset[i - 1][2];

    for (j = jpegInfo.mcu_start_index[i]; j < jpegInfo.mcu_end_index[i]; j += 64, index += 64) {
      mram_read(&MCU_buffer[i][j], &MCU_buffer_cache[0][0], 128);
      MCU_buffer_cache[0][0] += dc_offset[k] - 50;
      mram_write(&MCU_buffer_cache[0][0], &MCU_buffer[0][index], 128);
      k++;
      if (k == 3) {
        k = 0;
      }
    }
  }

  // TODO: remove later
  for (row = 0; row < jpegInfo.mcu_height; row += jpegInfo.max_v_samp_factor) {
    for (col = 0; col < jpegInfo.mcu_width; col += jpegInfo.max_h_samp_factor) {
      for (uint32_t index = 0; index < 3; index++) {
        for (uint32_t y = 0; y < jpegInfo.color_components[index].v_samp_factor; y++) {
          for (uint32_t x = 0; x < jpegInfo.color_components[index].h_samp_factor; x++) {
            uint32_t mcu_index = (((row + y) * jpegInfo.mcu_width_real + (col + x)) * 3 + index) * 64;
            uint32_t cache_index = 0;
            mram_read(&MCU_buffer[d->tasklet_id][mcu_index], &MCU_buffer_cache[d->tasklet_id][cache_index], 128);

            // Compute inverse DCT with ANN algorithm
            inverse_dct_component(d, cache_index);

            mram_write(&MCU_buffer_cache[d->tasklet_id][cache_index], &MCU_buffer[d->tasklet_id][mcu_index], 128);
          }
        }
      }

      // Convert from YCbCr to RGB
      for (int y = jpegInfo.max_v_samp_factor - 1; y >= 0; y--) {
        for (int x = jpegInfo.max_h_samp_factor - 1; x >= 0; x--) {
          // MCU to index is (current row + vertical sampling) * total number of MCUs in a row of the JPEG
          // + (current col + horizontal sampling)
          uint32_t mcu_index = ((row + y) * jpegInfo.mcu_width_real + (col + x)) * 3 * 64;
          uint32_t cache_index = 0;
          mram_read(&MCU_buffer[d->tasklet_id][mcu_index], &MCU_buffer_cache[d->tasklet_id][cache_index], 384);

          ycbcr_to_rgb_pixel(d, cache_index, y, x);

          mram_write(&MCU_buffer_cache[d->tasklet_id][cache_index], &MCU_buffer[d->tasklet_id][mcu_index], 384);
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
      printf("Got APPN marker: FF %X\n", marker);
      skip_marker(d);
      break;

    case M_DQT:
      printf("Got DQT marker: FF %X\n", marker);
      process_DQT(d);
      break;

    case M_DRI:
      printf("Got DRI marker: FF %X\n", marker);
      process_DRI(d);
      break;

    case M_SOF0:
      // case M_SOF5 ... M_SOF7:
      // case M_SOF9 ... M_SOF11:
      // case M_SOF13 ... M_SOF15:
      printf("Got SOF marker: FF %X\n", marker);
      process_SOFn(d);
      break;

    case M_SOF2:
      // TODO: handle progressive JPEG
      jpegInfo.valid = 0;
      printf("Got progressive JPEG: FF %X, not supported yet\n", marker);
      break;

    case M_DHT:
      printf("Got DHT marker: FF %X\n", marker);
      process_DHT(d);
      break;

    case M_SOS:
      // Return 0 when we find the SOS marker
      printf("Got SOS marker: FF %X\n", marker);
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

  // These fields will be used when decoding Huffman coded bitstream
  d->get_buffer = 0;
  d->bits_left = 0;
}

int main() {
  JpegDecompressor decompressor;
  decompressor.length = input.file_length;
  decompressor.tasklet_id = me();
  jpegInfo.length = decompressor.length;

  if (decompressor.tasklet_id == 0) {
    int res = 1;

    decompressor.tasklet_id = me();
    decompressor.file_index = -PREFETCH_SIZE;
    decompressor.cache_index = PREFETCH_SIZE;

    init_jpeg_info();

    // Check whether file starts with SOI
    process_header(&decompressor);

    // Continuously read all markers until we reach Huffman coded bitstream
    while (jpegInfo.valid && res) {
      res = read_marker(&decompressor);
    }

    if (!jpegInfo.valid) {
      printf("Error: Invalid JPEG\n");
      return 1;
    }

    jpegInfo.image_data_start = decompressor.file_index + decompressor.cache_index;
    jpegInfo.size_per_tasklet = (jpegInfo.length - jpegInfo.image_data_start) / NR_TASKLETS;

    // print_jpeg_decompressor();

    output.image_width = jpegInfo.image_width;
    output.image_height = jpegInfo.image_height;
    output.padding = jpegInfo.padding;
    output.mcu_width_real = jpegInfo.mcu_width_real;
  }

  // All tasklets should wait until tasklet 0 has finished reading all JPEG markers
  barrier_wait(&init_barrier);

  init_jpeg_decompressor(&decompressor);
  // printf("Tasklet: %d, start: %d %d, end: %d\n", decompressor.tasklet_id, decompressor.file_index,
  //        decompressor.cache_index, decompressor.length);

  // Process Huffman coded bitstream, perform inverse DCT, and convert YCbCr to RGB
  decompress_scanline(&decompressor);
  if (!jpegInfo.valid) {
    // printf("Error: Invalid JPEG\n");
    return 1;
  }

  // printf("Tasklet %d end index: %d\n", decompressor.tasklet_id, jpegInfo.mcu_end_index[decompressor.tasklet_id]);

  // printf("Tasklet: %d\n", decompressor.tasklet_id);
  // for (int i = 0; i < 5; i++) {
  //   for (int j = 0; j < 1; j++) {
  //     for (int k = 0; k < 64; k++) {
  //       if (k % 8 == 0) {
  //         printf("\n");
  //       }
  //       printf("%d ", MCU_buffer[decompressor.tasklet_id][i * 192 + j * 64 + k]);
  //     }
  //     printf("\n");
  //   }
  // }

  // if (decompressor.tasklet_id == 0) {
  //   decompress_scanline(&decompressor);
  //   if (!jpegInfo.valid) {
  //     printf("Error: Invalid JPEG\n");
  //     return 1;
  //   }
  // } else {
  //   printf("%d\n", decompressor.tasklet_id);
  //   int sum = 0;
  //   while (!eof(&decompressor)) {
  //     uint8_t readbyte = read_byte(&decompressor);
  //     sum += readbyte;
  //   }
  //   printf("Sum: %d\n", sum);
  // }

  return 0;
}
