#include <byteswap.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "bmp.h"
#include "jpeg-cpu.h"

#define M_PI 3.14159265358979323846
#define rounded_division(_a, _b) (((_a) + (_b)-1) / (_b))

#define M0 1.84775906502257351225
#define M1 1.41421356237309504880
#define M3 1.41421356237309504880
#define M5 0.76536686473017954345
#define M2 1.08239220029239396879
#define M4 2.61312592975275305571

#define S0 0.35355339059327376220
#define S1 0.49039264020161522456
#define S2 0.46193976625564337806
#define S3 0.41573480615127261853
#define S4 0.35355339059327376220
#define S5 0.27778511650980111237
#define S6 0.19134171618254488586
#define S7 0.09754516100806413392

/* We want to emulate the behaviour of 'tjbench <jpg> -scale 1/8'
        That calls 'process_data_simple_main' and 'decompress_onepass' in
turbojpeg On my laptop, I see:

./tjbench ../DSCF5148.JPG -scale 1/8

>>>>>  JPEG 4:2:2 --> BGR (Top-down)  <<<<<

Image size: 1920 x 1080 --> 240 x 135
Decompress    --> Frame rate:         92.177865 fps
                  Throughput:         191.140021 Megapixels/sec
*/

/**
 * Helper array for filling in quantization table in zigzag order
 */
const uint8_t ZIGZAG_ORDER[] = {0,  1,  8,  16, 9,  2,  3,  10, 17, 24, 32, 25, 18, 11, 4,  5,  12, 19, 26, 33, 40, 48,
                                41, 34, 27, 20, 13, 6,  7,  14, 21, 28, 35, 42, 49, 56, 57, 50, 43, 36, 29, 22, 15, 23,
                                30, 37, 44, 51, 58, 59, 52, 45, 38, 31, 39, 46, 53, 60, 61, 54, 47, 55, 62, 63};

/**
 * Check whether EOF is reached by comparing current ptr location to start
 * location + entire length of file
 */
static int eof(JpegDecompressor *d) { return (d->ptr >= d->data + d->length); }

/**
 * Helper function to read a byte from the file
 */
static uint8_t read_byte(JpegDecompressor *d) {
  uint8_t temp;

  temp = (*d->ptr);
  d->ptr++;
  return temp;
}

/**
 * Helper function to read 2 bytes from the file, MSB order
 */
static uint16_t read_short(JpegDecompressor *d) {
  uint16_t temp3;
  uint8_t temp1, temp2;

  temp1 = *d->ptr;
  d->ptr++;
  temp2 = *d->ptr;
  d->ptr++;

  temp3 = (temp1 << 8) | temp2;
  return temp3;
}

/**
 * Skip count bytes
 */
static void skip_bytes(JpegDecompressor *d, int count) {
  // If after skipping count bytes we go beyond EOF, then only skip till EOF
  if (d->ptr + count > d->data + d->length)
    d->ptr = d->data + d->length;
  else
    d->ptr += count;
}

/**
 * Skip over an unknown or uninteresting variable-length marker like APPN or COM
 */
static int skip_marker(JpegDecompressor *d) {
  uint16_t length = read_short(d);
  // Length includes itself, so must be at least 2
  if (length < 2) {
    d->valid = 0;
    fprintf(stderr, "ERROR: Invalid length encountered in skip_marker\n");
    return -1;
  }

  length -= 2;
  // Skip over the remaining bytes
  skip_bytes(d, length);

  return 0;
}

/**
 * Find the next marker (byte with value FF). Swallow consecutive duplicate FF bytes
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
 */
static void process_header(JpegDecompressor *d) {
  uint8_t c1 = 0, c2 = 0;

  if (!eof(d)) {
    c1 = read_byte(d);
    c2 = read_byte(d);
  }
  if (c1 != 0xFF || c2 != M_SOI) {
    d->valid = 0;
    fprintf(stderr, "Error: Not JPEG: %X %X\n", c1, c2);
  } else {
    printf("Got SOI marker: %X %X\n", c1, c2);
  }
}

/**
 * Read Quantization Table
 * Page 40: Table B.4
 */
static void process_DQT(JpegDecompressor *d) {
  int length = read_short(d); // Lq
  length -= 2;

  while (length > 0) {
    uint8_t qt_info = read_byte(d);
    length -= 1;

    uint8_t table_id = qt_info & 0x0F; // Tq
    if (table_id > 3) {
      d->valid = 0;
      fprintf(stderr, "Error: Invalid DQT - got quantization table ID: %d, ID should be between 0 and 3\n", table_id);
      return;
    }
    d->quant_tables[table_id].exists = 1;

    uint8_t precision = (qt_info >> 4) & 0x0F; // Pq
    if (precision == 0) {
      // 8 bit precision
      for (int i = 0; i < 64; i++) {
        d->quant_tables[table_id].table[ZIGZAG_ORDER[i]] = read_byte(d); // Qk
      }
      length -= 64;
    } else {
      // 16 bit precision
      for (int i = 0; i < 64; i++) {
        d->quant_tables[table_id].table[ZIGZAG_ORDER[i]] = read_short(d); // Qk
      }
      length -= 128;
    }
  }

  if (length != 0) {
    d->valid = 0;
    fprintf(stderr, "Error: Invalid DQT - length incorrect\n");
  }
}

/**
 * Read Restart Interval
 */
static void process_DRI(JpegDecompressor *d) {
  int length = read_short(d);
  if (length != 4) {
    d->valid = 0;
    fprintf(stderr, "Error: Invalid DRI - length is not 4\n");
    return;
  }

  d->restart_interval = read_short(d);
}

/**
 * Read Start Of Frame
 * Page 36: Table B.2
 */
static void process_SOFn(JpegDecompressor *d) {
  if (d->num_color_components != 0) {
    d->valid = 0;
    fprintf(stderr, "Error: Invalid SOF - multiple SOFs encountered\n");
    return;
  }

  int length = read_short(d); // Lf

  uint8_t precision = read_byte(d); // P
  if (precision != 8) {
    d->valid = 0;
    fprintf(stderr, "Error: Invalid SOF - precision is %d, should be 8\n", precision);
    return;
  }

  d->image_height = read_short(d); // Y
  d->image_width = read_short(d);  // X
  if (d->image_height == 0 || d->image_width == 0) {
    d->valid = 0;
    fprintf(stderr, "Error: Invalid SOF - dimensions: %d x %d\n", d->image_width, d->image_height);
    return;
  }
  d->mcu_height = (d->image_height + 7) / 8;
  d->mcu_width = (d->image_width + 7) / 8;
  d->padding = d->image_width % 4;
  d->total_mcus = d->mcu_height * d->mcu_width;

  // Should be 3
  d->num_color_components = read_byte(d); // Nf
  if (d->num_color_components == 0 || d->num_color_components > 3) {
    d->valid = 0;
    fprintf(stderr, "Error: Invalid SOF - number of color components: %d\n", d->num_color_components);
    return;
  }

  for (int i = 0; i < d->num_color_components; i++) {
    // The component ID is expected to be 1, 2, or 3
    uint8_t component_id = read_byte(d); // Ci
    if (component_id == 0 || component_id > 3) {
      d->valid = 0;
      fprintf(stderr, "Error: Invalid SOF - component ID: %d\n", component_id);
      return;
    }

    ColorComponentInfo *component = &d->color_components[component_id - 1];
    component->exists = 1;
    component->component_id = component_id;

    uint8_t factor = read_byte(d);
    component->h_samp_factor = (factor >> 4) & 0x0F; // Hi
    component->v_samp_factor = factor & 0x0F;        // Vi
    if (component->h_samp_factor != 1 || component->v_samp_factor != 1) {
      // TODO: add support for other sampling factors
      d->valid = 0;
      fprintf(stderr, "Error: Invalid SOF - horizontal and vertical sampling factor other than 1 not support yet\n");
      return;
    }
    component->quant_table_id = read_byte(d); // Tqi
  }

  if (length - 8 - (3 * d->num_color_components) != 0) {
    d->valid = 0;
    fprintf(stderr, "Error: Invalid SOF - length incorrect\n");
  }
}

/**
 * Read Huffman tables
 * Page 41: Table B.5
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
      d->valid = 0;
      fprintf(stderr, "Error: Invalid DHT - Huffman Table ID: %d\n", table_id);
      return;
    }

    HuffmanTable *h_table = ac_table ? &d->ac_huffman_tables[table_id] : &d->dc_huffman_tables[table_id];
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
    d->valid = 0;
    fprintf(stderr, "Error: Invalid DHT - length incorrect\n");
  }
}

/**
 * Helper function for actually generating the Huffman codes
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
 */
static void build_huffman_tables(JpegDecompressor *d) {
  for (int i = 0; i < 4; i++) {
    if (d->dc_huffman_tables[i].exists) {
      generate_codes(&d->dc_huffman_tables[i]);
    }
    if (d->ac_huffman_tables[i].exists) {
      generate_codes(&d->ac_huffman_tables[i]);
    }
  }
}

/**
 * Read Start Of Scan
 * Page 38: Table B.3
 */
static void process_SOS(JpegDecompressor *d) {
  int length = read_short(d); // Ls
  length -= 2;

  uint8_t num_components = read_byte(d); // Ns
  length -= 1;
  if (num_components == 0 || num_components != d->num_color_components) {
    d->valid = 0;
    fprintf(stderr, "Error: Invalid SOS - number of color components does not match SOF: %d vs %d", num_components,
            d->num_color_components);
    return;
  }

  for (int i = 0; i < num_components; i++) {
    uint8_t component_id = read_byte(d); // Csj
    if (component_id == 0 || component_id > 3) {
      d->valid = 0;
      fprintf(stderr, "Error: Invalid SOS - component ID: %d\n", component_id);
      return;
    }

    ColorComponentInfo *component = &d->color_components[component_id - 1];
    uint8_t tdta = read_byte(d);
    component->dc_huffman_table_id = (tdta >> 4) & 0x0F; // Tdj
    component->ac_huffman_table_id = tdta & 0x0F;        // Taj
  }

  d->ss = read_byte(d); // Ss
  d->se = read_byte(d); // Se
  uint8_t A = read_byte(d);
  d->Ah = (A >> 4) & 0xF; // Ah
  d->Al = A & 0xF;        // Al
  length -= 3;

  if (d->ss != 0 || d->se != 63) {
    d->valid = 0;
    fprintf(stderr, "Error: Invalid SOS - invalid spectral selection\n");
    return;
  }
  if (d->Ah != 0 || d->Al != 0) {
    d->valid = 0;
    fprintf(stderr, "Error: Invalid SOS - invalid successive approximation\n");
    return;
  }

  if (length - (2 * num_components) != 0) {
    d->valid = 0;
    fprintf(stderr, "Error: Invalid SOS - length incorrect\n");
  }

  build_huffman_tables(d);
}

// static void reset_decoder(JpegDecompressor *d) {
//   d->restarts_left = d->restart_interval;
//   d->get_buffer = 0;
//   d->bits_left = 0;
// }

// int process_restart(JpegDecompressor *d) {
//   int marker = next_marker(d);
//   switch (marker) {
//     case -1:
//       printf("Error: EOF while looking for RST\n");
//       return -1;

//     case M_EOI:
//       printf("EOI after %u restarts\n", d->num_restart_intervals);
//       return -1;

//     case M_RST_FIRST ... M_RST_LAST:
//       d->num_restart_intervals++;
//       reset_decoder(d);
//       printf("Found restart %u (%u)\n", marker - M_RST_FIRST, d->num_restart_intervals);
//       return 0;

//     default:
//       printf("Found unexpected marker 0x%x\n", marker);
//   }

//   return -1;
// }

/**
 * Get the number of specified bits from the buffer
 */
static int get_bits(JpegDecompressor *d, uint32_t num_bits) {
  uint8_t b;
  uint32_t c;
  int temp = 0;
  if (num_bits == 0) {
    return temp;
  }

  // TODO: handle restart intervals
  while (d->bits_left < num_bits) {
    // read a byte and decode it, if it is 0xFF
    b = read_byte(d);
    c = b;
    while (b == 0xFF) {
      b = read_byte(d);
      if (b == 0)
        c = 0xFF;
      else
        c = b;
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

/* F.2.1.2 Decode 8x8 block data unit */
static int decode_mcu(JpegDecompressor *d, int component_index, int *buffer, int *previous_dc) {
  QuantizationTable *q_table = &d->quant_tables[d->color_components[component_index].quant_table_id];
  HuffmanTable *dc_table = &d->dc_huffman_tables[d->color_components[component_index].dc_huffman_table_id];
  HuffmanTable *ac_table = &d->ac_huffman_tables[d->color_components[component_index].ac_huffman_table_id];

  // Get DC value for this MCU block
  uint8_t dc_length = huff_decode(d, dc_table);
  if (dc_length == (uint8_t)-1) {
    fprintf(stderr, "Error: Invalid DC code\n");
    return -1;
  }
  if (dc_length > 11) {
    fprintf(stderr, "Error: DC coefficient length greater than 11\n");
    return -1;
  }

  int coeff = get_bits(d, dc_length);
  if (dc_length != 0 && coeff < (1 << (dc_length - 1))) {
    // Convert to negative coefficient
    coeff -= (1 << dc_length) - 1;
  }
  buffer[0] = coeff + *previous_dc;
  *previous_dc = buffer[0];
  // Dequantization
  buffer[0] *= q_table->table[0];

  // Get the AC values for this MCU block
  int i = 1;
  while (i < 64) {
    uint8_t ac_length = huff_decode(d, ac_table);
    if (ac_length == (uint8_t)-1) {
      fprintf(stderr, "Error: Invalid AC code\n");
      return -1;
    }

    // Got 0x00, fill remaining MCU block with 0s
    if (ac_length == 0x00) {
      while (i < 64) {
        buffer[ZIGZAG_ORDER[i++]] = 0;
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
      fprintf(stderr, "Error: Invalid AC code - zeros exceeded MCU length");
      return -1;
    }
    for (int j = 0; j < num_zeroes; j++) {
      buffer[ZIGZAG_ORDER[i++]] = 0;
    }

    if (coeff_length > 10) {
      fprintf(stderr, "Error: AC coefficient length greater than 10\n");
      return -1;
    }
    if (coeff_length != 0) {
      coeff = get_bits(d, coeff_length);
      if (coeff < (1 << (coeff_length - 1))) {
        // Convert to negative coefficient
        coeff -= (1 << coeff_length) - 1;
      }
      // Write coefficient to buffer as well as perform dequantization
      buffer[ZIGZAG_ORDER[i]] = coeff * q_table->table[ZIGZAG_ORDER[i]];
      i++;
    }
  }

  return 0;
}

/**
 * Decode the Huffman coded bitstream
 */
static MCU *decompress_scanline(JpegDecompressor *d) {
  MCU *mcus = (MCU *)malloc(d->total_mcus * sizeof(MCU));
  int previous_dcs[3] = {0};

  // TODO: iterate through scan lines instead of total_mcus
  for (int i = 0; i < d->total_mcus; i++) {
    // TODO: Take into account Restart Intervals
    for (int j = 0; j < d->num_color_components; j++) {
      if (decode_mcu(d, j, mcus[i].buffer[j], &previous_dcs[j]) != 0) {
        fprintf(stderr, "Error: Invalid MCU\n");
        free(mcus);
        return NULL;
      }
    }
  }

  // // Loops here for verification
  // for (int k = d->total_mcus - 5; k < d->total_mcus; k++) {
  //   for (int j = 0; j < 3; j++) {
  //     for (int i = 0; i < 64; i++) {
  //       if (i % 8 == 0)
  //         printf("\n");
  //       printf("%d ", mcus[k].buffer[j][ZIGZAG_ORDER[i]]);
  //     }
  //     printf("\n");
  //   }
  // }

  return mcus;
}

/**
 * Function to perform inverse DCT for one of the color components of an MCU
 */
static void inverse_dct_component(int *component, int index) {
  for (int i = 0; i < 8; i++) {
    float g0 = component[0 * 8 + i] * S0;
    float g1 = component[4 * 8 + i] * S4;
    float g2 = component[2 * 8 + i] * S2;
    float g3 = component[6 * 8 + i] * S6;
    float g4 = component[5 * 8 + i] * S5;
    float g5 = component[1 * 8 + i] * S1;
    float g6 = component[7 * 8 + i] * S7;
    float g7 = component[3 * 8 + i] * S3;

    float f0 = g0;
    float f1 = g1;
    float f2 = g2;
    float f3 = g3;
    float f4 = g4 - g7;
    float f5 = g5 + g6;
    float f6 = g5 - g6;
    float f7 = g4 + g7;

    float e0 = f0;
    float e1 = f1;
    float e2 = f2 - f3;
    float e3 = f2 + f3;
    float e4 = f4;
    float e5 = f5 - f7;
    float e6 = f6;
    float e7 = f5 + f7;
    float e8 = f4 + f6;

    float d0 = e0;
    float d1 = e1;
    float d2 = e2 * M1;
    float d3 = e3;
    float d4 = e4 * M2;
    float d5 = e5 * M3;
    float d6 = e6 * M4;
    float d7 = e7;
    float d8 = e8 * M5;

    float c0 = d0 + d1;
    float c1 = d0 - d1;
    float c2 = d2 - d3;
    float c3 = d3;
    float c4 = d4 + d8;
    float c5 = d5 + d7;
    float c6 = d6 - d8;
    float c7 = d7;
    float c8 = c5 - c6;

    float b0 = c0 + c3;
    float b1 = c1 + c2;
    float b2 = c1 - c2;
    float b3 = c0 - c3;
    float b4 = c4 - c8;
    float b5 = c8;
    float b6 = c6 - c7;
    float b7 = c7;

    component[0 * 8 + i] = b0 + b7;
    component[1 * 8 + i] = b1 + b6;
    component[2 * 8 + i] = b2 + b5;
    component[3 * 8 + i] = b3 + b4;
    component[4 * 8 + i] = b3 - b4;
    component[5 * 8 + i] = b2 - b5;
    component[6 * 8 + i] = b1 - b6;
    component[7 * 8 + i] = b0 - b7;
  }

  for (int i = 0; i < 8; i++) {
    float g0 = component[i * 8 + 0] * S0;
    float g1 = component[i * 8 + 4] * S4;
    float g2 = component[i * 8 + 2] * S2;
    float g3 = component[i * 8 + 6] * S6;
    float g4 = component[i * 8 + 5] * S5;
    float g5 = component[i * 8 + 1] * S1;
    float g6 = component[i * 8 + 7] * S7;
    float g7 = component[i * 8 + 3] * S3;

    float f0 = g0;
    float f1 = g1;
    float f2 = g2;
    float f3 = g3;
    float f4 = g4 - g7;
    float f5 = g5 + g6;
    float f6 = g5 - g6;
    float f7 = g4 + g7;

    float e0 = f0;
    float e1 = f1;
    float e2 = f2 - f3;
    float e3 = f2 + f3;
    float e4 = f4;
    float e5 = f5 - f7;
    float e6 = f6;
    float e7 = f5 + f7;
    float e8 = f4 + f6;

    float d0 = e0;
    float d1 = e1;
    float d2 = e2 * M1;
    float d3 = e3;
    float d4 = e4 * M2;
    float d5 = e5 * M3;
    float d6 = e6 * M4;
    float d7 = e7;
    float d8 = e8 * M5;

    float c0 = d0 + d1;
    float c1 = d0 - d1;
    float c2 = d2 - d3;
    float c3 = d3;
    float c4 = d4 + d8;
    float c5 = d5 + d7;
    float c6 = d6 - d8;
    float c7 = d7;
    float c8 = c5 - c6;

    float b0 = c0 + c3;
    float b1 = c1 + c2;
    float b2 = c1 - c2;
    float b3 = c0 - c3;
    float b4 = c4 - c8;
    float b5 = c8;
    float b6 = c6 - c7;
    float b7 = c7;

    component[i * 8 + 0] = b0 + b7;
    component[i * 8 + 1] = b1 + b6;
    component[i * 8 + 2] = b2 + b5;
    component[i * 8 + 3] = b3 + b4;
    component[i * 8 + 4] = b3 - b4;
    component[i * 8 + 5] = b2 - b5;
    component[i * 8 + 6] = b1 - b6;
    component[i * 8 + 7] = b0 - b7;
  }
}

/**
 * Function to perform conversion from YCbCr to RGB for the 64 pixels within an MCU
 */
static void ycbcr_to_rgb_pixel(int buffer[3][64]) {
  // https://en.wikipedia.org/wiki/YUV Y'UV444 to RGB888 conversion
  for (int i = 0; i < 64; i++) {
    // TODO: if multiplication is too slow, use bit shifting. However, bit shifting is less accurate from what I can see
    // int r = buffer[0][i] + buffer[2][i] + (buffer[2][i] >> 2) + (buffer[2][i] >> 3) + (buffer[2][i] >> 5) + 128;
    // int g = buffer[0][i] - ((buffer[1][i] >> 2) + (buffer[1][i] >> 4) + (buffer[1][i] >> 5)) -
    //         ((buffer[2][i] >> 1) + (buffer[2][i] >> 3) + (buffer[2][i] >> 4) + (buffer[2][i] >> 5)) + 128;
    // int b = buffer[0][i] + buffer[1][i] + (buffer[1][i] >> 1) + (buffer[1][i] >> 2) + (buffer[1][i] >> 6) + 128;

    // Floating point version, most accurate, but floating point calculations in DPUs are emulated, so very slow
    // int r = buffer[0][i] + 1.402 * buffer[2][i] + 128;
    // int g = buffer[0][i] - 0.344 * buffer[1][i] - 0.714 * buffer[2][i] + 128;
    // int b = buffer[0][i] + 1.772 * buffer[1][i] + 128;

    // Integer only, quite accurate but may be less performant than only using bit shifting
    int r = buffer[0][i] + ((45 * buffer[2][i]) >> 5) + 128;
    int g = buffer[0][i] - ((11 * buffer[1][i] + 23 * buffer[2][i]) >> 5) + 128;
    int b = buffer[0][i] + ((113 * buffer[1][i]) >> 6) + 128;

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

    buffer[0][i] = r;
    buffer[1][i] = g;
    buffer[2][i] = b;
  }
}

/**
 * Inverse Discrete Cosine Transform
 */
static void inverse_dct(JpegDecompressor *d, MCU *mcus) {
  for (int i = 0; i < d->total_mcus; i++) {
    for (int j = 0; j < d->num_color_components; j++) {
      // TODO: use integer only for idct instead of floating point
      inverse_dct_component(mcus[i].buffer[j], i);
    }

    ycbcr_to_rgb_pixel(mcus[i].buffer);
  }

  // Loops here for verification
  // for (int k = 0; k < 5; k++) {
  //   for (int j = 0; j < 3; j++) {
  //     for (int i = 0; i < 64; i++) {
  //       if (i % 8 == 0)
  //         printf("\n");
  //       printf("%d ", mcus[k].buffer[j][ZIGZAG_ORDER[i]]);
  //     }
  //     printf("\n");
  //   }
  // }
}

/**
 * Read JPEG markers
 * Return 0 when the SOS marker is found
 * Otherwise return 1 for failure
 */
static int read_marker(JpegDecompressor *d) {
  int marker;

  marker = next_marker(d);
  switch (marker) {
    case -1:
      d->valid = 0;
      fprintf(stderr, "Error: Read past EOF\n");
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

    case M_SOF0 ... M_SOF3:
    case M_SOF5 ... M_SOF7:
    case M_SOF9 ... M_SOF11:
    case M_SOF13 ... M_SOF15:
      // TODO: handle progressive JPEG
      printf("Got SOF marker: FF %X\n", marker);
      process_SOFn(d);
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
      d->valid = 0;
      fprintf(stderr, "Error: Unhandled marker: FF %X\n", marker);
      break;
  }

  return 1;
}

/**
 * Helper function to print out filled in values of the JPEG decompressor
 */
static void print_jpeg_decompressor(JpegDecompressor *d) {
  printf("\n********** DQT **********\n");
  for (int i = 0; i < 4; i++) {
    if (d->quant_tables[i].exists) {
      printf("Table ID: %d", i);
      for (int j = 0; j < 64; j++) {
        if (j % 8 == 0) {
          printf("\n");
        }
        printf("%d ", d->quant_tables[i].table[j]);
      }
      printf("\n\n");
    }
  }

  printf("********** DRI **********\n");
  printf("Restart Interval: %d\n", d->restart_interval);

  printf("\n********** SOF **********\n");
  printf("Width: %d\n", d->image_width);
  printf("Height: %d\n", d->image_height);
  printf("Number of color components: %d\n\n", d->num_color_components);
  for (int i = 0; i < d->num_color_components; i++) {
    printf("Component ID: %d\n", d->color_components[i].component_id);
    printf("H-samp factor: %d\n", d->color_components[i].h_samp_factor);
    printf("V-samp factor: %d\n", d->color_components[i].v_samp_factor);
    printf("Quantization table ID: %d\n\n", d->color_components[i].quant_table_id);
  }

  printf("\n********** DHT **********\n");
  for (int i = 0; i < 4; i++) {
    if (d->dc_huffman_tables[i].exists) {
      printf("DC Table ID: %d\n", i);
      for (int j = 0; j < 16; j++) {
        printf("%d: ", j + 1);
        for (int k = d->dc_huffman_tables[i].valoffset[j]; k < d->dc_huffman_tables[i].valoffset[j + 1]; k++) {
          printf("%d ", d->dc_huffman_tables[i].huffval[k]);
        }
        printf("\n");
      }
      printf("\n");
    }
  }
  for (int i = 0; i < 4; i++) {
    if (d->ac_huffman_tables[i].exists) {
      printf("AC Table ID: %d\n", i);
      for (int j = 0; j < 16; j++) {
        printf("%d: ", j + 1);
        for (int k = d->ac_huffman_tables[i].valoffset[j]; k < d->ac_huffman_tables[i].valoffset[j + 1]; k++) {
          printf("%d ", d->ac_huffman_tables[i].huffval[k]);
        }
        printf("\n");
      }
      printf("\n");
    }
  }

  printf("\n********** SOS **********\n");
  for (int i = 0; i < d->num_color_components; i++) {
    printf("Component ID: %d\n", d->color_components[i].component_id);
    printf("DC table ID: %d\n", d->color_components[i].dc_huffman_table_id);
    printf("AC table ID: %d\n\n", d->color_components[i].ac_huffman_table_id);
  }
  printf("Start of selection: %d\n", d->ss);
  printf("End of selection: %d\n", d->se);
  printf("Successive approximation high: %d\n", d->Ah);
  printf("Successive approximation low: %d\n\n", d->Al);

  printf("\n********** BMP **********\n");
  printf("MCU width: %d\n", d->mcu_width);
  printf("MCU height: %d\n", d->mcu_height);
  printf("BMP padding: %d\n", d->padding);
}

/**
 * Initialize the JPEG decompressor with default values
 */
static void init_jpeg_decompressor(JpegDecompressor *d) {
  d->valid = 1;

  for (int i = 0; i < 4; i++) {
    d->quant_tables[i].exists = 0;
    d->dc_huffman_tables[i].exists = 0;
    d->ac_huffman_tables[i].exists = 0;
  }
  for (int i = 0; i < 3; i++) {
    d->color_components[i].exists = 0;
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

void jpeg_cpu_scale(uint64_t file_length, char *filename, char *buffer) {
  JpegDecompressor decompressor;
  int res = 1;

  printf("Decoding file: %s\n", filename);

  decompressor.data = buffer;
  decompressor.ptr = decompressor.data;
  decompressor.length = file_length;

  init_jpeg_decompressor(&decompressor);

  // Check whether file starts with SOI
  process_header(&decompressor);

  // Continuously read all markers until we reach Huffman coded bitstream
  while (decompressor.valid && res) {
    res = read_marker(&decompressor);
  }

  if (!decompressor.valid) {
    fprintf(stderr, "Error: Invalid JPEG\n");
    return;
  }

  // print_jpeg_decompressor(&decompressor);

  // Process Huffman coded bitstream
  MCU *mcus = decompress_scanline(&decompressor);
  if (mcus == NULL) {
    return;
  }

  // Compute inverse DCT and convert YCbCr to RGB
  inverse_dct(&decompressor, mcus);

  // Now write the decoded data out as BMP
  BmpObject image;
  uint8_t *ptr;

  image.win_header.width = decompressor.image_width;
  image.win_header.height = decompressor.image_height;
  ptr = (uint8_t *)malloc(decompressor.image_height * (decompressor.image_width * 3 + decompressor.padding));
  image.data = ptr;

  for (int y = decompressor.image_height - 1; y >= 0; y--) {
    uint32_t mcu_row = y / 8;
    uint32_t pixel_row = y % 8;

    for (int x = 0; x < decompressor.image_width; x++) {
      uint32_t mcu_column = x / 8;
      uint32_t pixel_column = x % 8;
      uint32_t mcu_index = mcu_row * decompressor.mcu_width + mcu_column;
      uint32_t pixel_index = pixel_row * 8 + pixel_column;
      ptr[0] = mcus[mcu_index].buffer[2][pixel_index];
      ptr[1] = mcus[mcu_index].buffer[1][pixel_index];
      ptr[2] = mcus[mcu_index].buffer[0][pixel_index];
      ptr += 3;
    }

    for (uint32_t i = 0; i < decompressor.padding; i++) {
      ptr[0] = 0;
      ptr++;
    }
  }

  // Form BMP file name
  char *period_ptr = strrchr(filename, '.');
  if (period_ptr == NULL) {
    strcpy(filename + strlen(filename), ".bmp");
  } else {
    strcpy(period_ptr, ".bmp");
  }

  write_bmp(filename, &image);
  printf("Decoded to: %s\n\n", filename);

  free(image.data);
  free(mcus);
  return;
}
