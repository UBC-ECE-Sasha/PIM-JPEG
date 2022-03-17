#define _POSIX_C_SOURCE 199309L

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <time.h>

#include "bmp.h"
#include "jpeg-common.h"

#define TIME 0      // If set to 1, times how long it takes to do specific parts of the JPEG decoding process
#define USE_FLOAT 0 // If set to 1, uses the most accurate method of computing inverse DCT by using floats

#if TIME
#define TIME_NOW(_t) (clock_gettime(CLOCK_MONOTONIC, (_t)))
#define TIME_DIFFERENCE(_start, _end) ((_end.tv_sec + _end.tv_nsec / 1.0e9) - (_start.tv_sec + _start.tv_nsec / 1.0e9))
#endif

#define M0 1.84775906502257351225 // 118 >> 6 or 473 >> 8
#define M1 1.41421356237309504880 // 90 >> 6  or 181 >> 7
#define M2 1.08239220029239396879 // 69 >> 6  or 277 >> 8
#define M3 1.41421356237309504880 // 90 >> 6  or 181 >> 7
#define M4 2.61312592975275305571 // 167 >> 6 or 669 >> 8
#define M5 0.76536686473017954345 // 49 >> 6

#define S0 0.35355339059327376220 // 22 >> 6 or 181 >> 9
#define S1 0.49039264020161522456 // 31 >> 6 or 251 >> 9
#define S2 0.46193976625564337806 // 30 >> 6 or 59 >> 7
#define S3 0.41573480615127261853 // 27 >> 6 or 213 >> 9
#define S4 0.35355339059327376220 // 22 >> 6 or 181 >> 9
#define S5 0.27778511650980111237 // 18 >> 6 or 71 >> 8
#define S6 0.19134171618254488586 // 12 >> 6 or 49 >> 8
#define S7 0.09754516100806413392 // 6 >> 6  or 25 >> 8

JpegInfo jpegInfo;

/* We want to emulate the behaviour of 'tjbench <jpg> -scale 1/8'
        That calls 'process_data_simple_main' and 'decompress_onepass' in
turbojpeg On my laptop, I see:

./tjbench ../DSCF5148.JPG -scale 1/8

>>>>>  JPEG 4:2:2 --> BGR (Top-down)  <<<<<

Image size: 1920 x 1080 --> 240 x 135
Decompress    --> Frame rate:         92.177865 fps
                  Throughput:         191.140021 Megapixels/sec
*/

// /**
//  * Helper array for filling in quantization table in zigzag order
//  */
// const uint8_t ZIGZAG_ORDER[] = {0,  1,  8,  16, 9,  2,  3,  10, 17, 24, 32, 25, 18, 11, 4,  5,  12, 19, 26, 33, 40,
// 48,
//                                 41, 34, 27, 20, 13, 6,  7,  14, 21, 28, 35, 42, 49, 56, 57, 50, 43, 36, 29, 22, 15,
//                                 23, 30, 37, 44, 51, 58, 59, 52, 45, 38, 31, 39, 46, 53, 60, 61, 54, 47, 55, 62, 63};

static int is_eof(JpegDecompressor *d) {
  return (d->ptr >= (d->data + d->length));
}

static uint8_t read_byte(JpegDecompressor *d) {
  uint8_t byte = (*d->ptr);
  d->ptr++;
  return byte;
}

static uint16_t read_short(JpegDecompressor *d) {
  uint8_t byte1 = read_byte(d);
  uint8_t byte2 = read_byte(d);

  uint16_t two_bytes = (byte1 << 8) | byte2;
  return two_bytes;
}

static void skip_bytes(JpegDecompressor *d, int num_bytes) {
  d->ptr += num_bytes;
  if (is_eof(d)) {
    d->ptr = d->data + d->length;
  }
}

static int skip_marker(JpegDecompressor *d) {
  int length = read_short(d);
  length -= 2;

  if (length < 0) {
    jpegInfo.valid = 0;
    fprintf(stderr, "ERROR: Invalid length encountered in skip_marker\n");
    return -1;
  }

  skip_bytes(d, length);
  return 0;
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

static int skip_to_next_marker(JpegDecompressor *d) {
  int num_skipped_bytes = count_and_skip_non_marker_bytes(d);
  int marker = get_marker_and_ignore_ff_bytes(d);

  if (num_skipped_bytes) {
    printf("WARNING: Discarded %u bytes\n", num_skipped_bytes);
  }

  return marker;
}

static void check_start_of_image(JpegDecompressor *d) {
  uint8_t c1 = 0, c2 = 0;

  if (!is_eof(d)) {
    c1 = read_byte(d);
    c2 = read_byte(d);
  }
  if (c1 != 0xFF || c2 != M_SOI) {
    jpegInfo.valid = 0;
    fprintf(stderr, "Error: Not JPEG: %X %X\n", c1, c2);
  }
}

static void form_low_precision_DQT(JpegDecompressor *d, int *length, uint8_t table_id) {
  for (int i = 0; i < 64; i++) {
    jpegInfo.quant_tables[table_id].table[ZIGZAG_ORDER[i]] = read_byte(d); // Qk
  }
  *length -= 64;
}

static void form_high_precision_DQT(JpegDecompressor *d, int *length, uint8_t table_id) {
  for (int i = 0; i < 64; i++) {
    jpegInfo.quant_tables[table_id].table[ZIGZAG_ORDER[i]] = read_short(d); // Qk
  }
  *length -= 128;
}

static int read_and_form_DQT(JpegDecompressor *d, int *length) {
  uint8_t qt_info = read_byte(d);
  *length -= 1;

  uint8_t table_id = qt_info & 0x0F; // Tq
  if (table_id > 3) {
    jpegInfo.valid = 0;
    fprintf(stderr, "Error: Invalid DQT - got quantization table ID: %d, ID should be between 0 and 3\n", table_id);
    return 1;
  }
  jpegInfo.quant_tables[table_id].exists = 1;

  uint8_t precision = (qt_info >> 4) & 0x0F; // Pq
  if (precision == 0) {
    form_low_precision_DQT(d, length, table_id);
  } else {
    form_high_precision_DQT(d, length, table_id);
  }

  return 0;
}

// Page 39: Section B.2.4.1
static void process_DQT(JpegDecompressor *d) {
  int length = read_short(d); // Lq
  length -= 2;

  while (length > 0) {
    int error = read_and_form_DQT(d, &length);
    if (error) {
      return;
    }
  }

  if (length != 0) {
    jpegInfo.valid = 0;
    fprintf(stderr, "Error: Invalid DQT - length incorrect\n");
  }
}

static void process_DRI(JpegDecompressor *d) {
  int length = read_short(d);
  if (length != 4) {
    jpegInfo.valid = 0;
    fprintf(stderr, "Error: Invalid DRI - length is not 4\n");
    return;
  }

  jpegInfo.restart_interval = read_short(d);
}

static int read_SOF_metadata(JpegDecompressor *d) {
  uint8_t precision = read_byte(d); // P
  if (precision != 8) {
    jpegInfo.valid = 0;
    fprintf(stderr, "Error: Invalid SOF - precision is %d, should be 8\n", precision);
    return 1;
  }

  jpegInfo.image_height = read_short(d); // Y
  jpegInfo.image_width = read_short(d);  // X
  if (jpegInfo.image_height == 0 || jpegInfo.image_width == 0) {
    jpegInfo.valid = 0;
    fprintf(stderr, "Error: Invalid SOF - dimensions: %d x %d\n", jpegInfo.image_width, jpegInfo.image_height);
    return 1;
  }

  jpegInfo.num_color_components = read_byte(d); // Nf
  if (jpegInfo.num_color_components == 0 || jpegInfo.num_color_components > 3) {
    jpegInfo.valid = 0;
    fprintf(stderr, "Error: Invalid SOF - number of color components: %d\n", jpegInfo.num_color_components);
    return 1;
  }

  return 0;
}

static int read_SOF_color_component_info(JpegDecompressor *d) {
  uint8_t component_id = read_byte(d); // Ci
  ColorComponentInfo *component;
  if (component_id == 0 || jpegInfo.greyScale == 1) 
  {
    // Black and white picture with gray scale
    jpegInfo.greyScale = 1;
    component = &jpegInfo.color_components[component_id];
  }
  else if (component_id <= 3)
  {  
    component = &jpegInfo.color_components[component_id - 1];
  }
  else 
  {
    jpegInfo.valid = 0;
    fprintf(stderr, "Error: Invalid SOF - component ID: %d\n", component_id);
    return 1;
  }

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
      fprintf(stderr, "Error: Invalid SOF - horizontal or vertical sampling factor for luminance out of range %d %d\n",
              component->h_samp_factor, component->v_samp_factor);
      return 1;
    }

    jpegInfo.max_h_samp_factor = component->h_samp_factor;
    jpegInfo.max_v_samp_factor = component->v_samp_factor;
  } else if (component->h_samp_factor != 1 || component->v_samp_factor != 1) {
    jpegInfo.valid = 0;
    fprintf(stderr, "Error: Invalid SOF - horizontal and vertical sampling factor for Cr and Cb not 1");
    return 1;
  }

  component->quant_table_id = read_byte(d); // Tqi

  return 0;
}

static void initialize_MCU_height_width() {
  jpegInfo.mcu_height = (jpegInfo.image_height + 7) / 8;
  jpegInfo.mcu_width = (jpegInfo.image_width + 7) / 8;
  jpegInfo.padding = jpegInfo.image_width % 4;
  jpegInfo.mcu_height_real = jpegInfo.mcu_height;
  jpegInfo.mcu_width_real = jpegInfo.mcu_width;
  if (jpegInfo.max_v_samp_factor == 2 && jpegInfo.mcu_height_real % 2 == 1) {
    jpegInfo.mcu_height_real++;
  }
  if (jpegInfo.max_h_samp_factor == 2 && jpegInfo.mcu_width_real % 2 == 1) {
    jpegInfo.mcu_width_real++;
  }
}

// Page 35: Section B.2.2
static void process_SOFn(JpegDecompressor *d) {
  if (jpegInfo.num_color_components != 0) {
    jpegInfo.valid = 0;
    fprintf(stderr, "Error: Invalid SOF - multiple SOFs encountered\n");
    return;
  }

  int length = read_short(d); // Lf

  int error = read_SOF_metadata(d);
  if (error) {
    return;
  }


  for (int i = 0; i < jpegInfo.num_color_components; i++) {
    error = read_SOF_color_component_info(d);
    if (error) {
      return;
    }
  }

  initialize_MCU_height_width();

  if (length - 8 - (3 * jpegInfo.num_color_components) != 0) {
    jpegInfo.valid = 0;
    fprintf(stderr, "Error: Invalid SOF - length incorrect\n");
  }
}

static int read_DHT(JpegDecompressor *d, int *length) {
  uint8_t ht_info = read_byte(d);
  *length -= 1;

  uint8_t table_id = ht_info & 0x0F;        // Th
  uint8_t ac_table = (ht_info >> 4) & 0x0F; // Tc
  if (table_id > 3) {
    jpegInfo.valid = 0;
    fprintf(stderr, "Error: Invalid DHT - Huffman Table ID: %d\n", table_id);
    return 1;
  }

  HuffmanTable *h_table = ac_table ? &jpegInfo.ac_huffman_tables[table_id] : &jpegInfo.dc_huffman_tables[table_id];
  h_table->exists = 1;

  h_table->valoffset[0] = 0;
  int total = 0;
  for (int i = 1; i <= 16; i++) {
    total += read_byte(d); // Li
    h_table->valoffset[i] = total;
  }
  *length -= 16;

  for (int i = 0; i < total; i++) {
    h_table->huffval[i] = read_byte(d); // Vij
  }
  *length -= total;

  return 0;
}

// Page 40: Section B.2.4.2
static void process_DHT(JpegDecompressor *d) {
  int length = read_short(d); // Lf
  length -= 2;

  // Keep reading Huffman tables until we run out of data
  while (length > 0) {
    int error = read_DHT(d, &length);
    if (error) {
      return;
    }
  }

  if (length != 0) {
    jpegInfo.valid = 0;
    fprintf(stderr, "Error: Invalid DHT - length incorrect\n");
  }
}

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

static void build_huffman_tables() {
  for (int i = 0; i < MAX_HUFFMAN_TABLES; i++) {
    if (jpegInfo.dc_huffman_tables[i].exists) {
      generate_codes(&jpegInfo.dc_huffman_tables[i]);
    }
    if (jpegInfo.ac_huffman_tables[i].exists) {
      generate_codes(&jpegInfo.ac_huffman_tables[i]);
    }
  }
}

static int read_SOS_color_component_info(JpegDecompressor *d) {
  uint8_t component_id = read_byte(d); // Csj
  ColorComponentInfo *component;
  if (component_id == 0 || jpegInfo.greyScale == 1)
  {
    // Black and white picture with gray scale
    jpegInfo.greyScale = 1;
    component = &jpegInfo.color_components[component_id];
  }
  else if (component_id > 3) {
    jpegInfo.valid = 0;
    fprintf(stderr, "Error: Invalid SOS - component ID: %d\n", component_id);
    return 1;
  } 
  else {
    component = &jpegInfo.color_components[component_id - 1];
  } 

  uint8_t tdta = read_byte(d);
  component->dc_huffman_table_id = (tdta >> 4) & 0x0F; // Tdj
  component->ac_huffman_table_id = tdta & 0x0F;        // Taj

  return 0;
}

static int read_SOS_metadata(JpegDecompressor *d) {
  jpegInfo.ss = read_byte(d); // Ss
  jpegInfo.se = read_byte(d); // Se
  uint8_t A = read_byte(d);
  jpegInfo.Ah = (A >> 4) & 0xF; // Ah
  jpegInfo.Al = A & 0xF;        // Al

  if (jpegInfo.ss != 0 || jpegInfo.se != 63) {
    jpegInfo.valid = 0;
    fprintf(stderr, "Error: Invalid SOS - invalid spectral selection\n");
    return 1;
  }
  if (jpegInfo.Ah != 0 || jpegInfo.Al != 0) {
    jpegInfo.valid = 0;
    fprintf(stderr, "Error: Invalid SOS - invalid successive approximation\n");
    return 1;
  }

  return 0;
}

// Page 37: Section B.2.3
static void process_SOS(JpegDecompressor *d) {
  int length = read_short(d); // Ls

  uint8_t num_components = read_byte(d); // Ns
  if (num_components == 0 || num_components != jpegInfo.num_color_components) {
    jpegInfo.valid = 0;
    fprintf(stderr, "Error: Invalid SOS - number of color components does not match SOF: %d vs %d\n", num_components,
            jpegInfo.num_color_components);
    return;
  }

  for (int i = 0; i < num_components; i++) {
    int error = read_SOS_color_component_info(d);
    if (error) {
      return;
    }
  }

  int error = read_SOS_metadata(d);
  if (error) {
    return;
  }

  if (length - 6 - (2 * num_components) != 0) {
    jpegInfo.valid = 0;
    fprintf(stderr, "Error: Invalid SOS - length incorrect\n");
  }

  build_huffman_tables();
}

static int get_num_bits(JpegDecompressor *d, uint32_t num_bits) {
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

static int decode_mcu(JpegDecompressor *d, int component_index, short *buffer, short *previous_dc) {
  QuantizationTable *q_table = &jpegInfo.quant_tables[jpegInfo.color_components[component_index].quant_table_id];
  HuffmanTable *dc_table = &jpegInfo.dc_huffman_tables[jpegInfo.color_components[component_index].dc_huffman_table_id];
  HuffmanTable *ac_table = &jpegInfo.ac_huffman_tables[jpegInfo.color_components[component_index].ac_huffman_table_id];

  // Get DC value for this MCU block
  uint8_t dc_length = huff_decode(d, dc_table);
  if (dc_length == (uint8_t) -1) {
    fprintf(stderr, "Error: Invalid DC code\n");
    return -1;
  }
  if (dc_length > 11) {
    fprintf(stderr, "Error: DC coefficient length greater than 11\n");
    return -1;
  }

  int coeff = get_num_bits(d, dc_length);
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
    if (ac_length == (uint8_t) -1) {
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
      fprintf(stderr, "Error: Invalid AC code - zeros exceeded MCU length %d >= 64\n", i + num_zeroes);
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
      coeff = get_num_bits(d, coeff_length);
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

#if USE_FLOAT
static void inverse_dct_component_float(short *buffer) {
  // ANN algorithm
  for (int i = 0; i < 8; i++) {
    float g0 = buffer[0 * 8 + i] * S0;
    float g1 = buffer[4 * 8 + i] * S4;
    float g2 = buffer[2 * 8 + i] * S2;
    float g3 = buffer[6 * 8 + i] * S6;
    float g4 = buffer[5 * 8 + i] * S5;
    float g5 = buffer[1 * 8 + i] * S1;
    float g6 = buffer[7 * 8 + i] * S7;
    float g7 = buffer[3 * 8 + i] * S3;

    float f4 = g4 - g7;
    float f5 = g5 + g6;
    float f6 = g5 - g6;
    float f7 = g4 + g7;

    float e2 = g2 - g3;
    float e3 = g2 + g3;
    float e5 = f5 - f7;
    float e7 = f5 + f7;
    float e8 = f4 + f6;

    float d2 = e2 * M1;
    float d4 = f4 * M2;
    float d5 = e5 * M3;
    float d6 = f6 * M4;
    float d8 = e8 * M5;

    float c0 = g0 + g1;
    float c1 = g0 - g1;
    float c2 = d2 - e3;
    float c4 = d4 + d8;
    float c5 = d5 + e7;
    float c6 = d6 - d8;
    float c8 = c5 - c6;

    float b0 = c0 + e3;
    float b1 = c1 + c2;
    float b2 = c1 - c2;
    float b3 = c0 - e3;
    float b4 = c4 - c8;
    float b6 = c6 - e7;

    buffer[0 * 8 + i] = b0 + e7;
    buffer[1 * 8 + i] = b1 + b6;
    buffer[2 * 8 + i] = b2 + c8;
    buffer[3 * 8 + i] = b3 + b4;
    buffer[4 * 8 + i] = b3 - b4;
    buffer[5 * 8 + i] = b2 - c8;
    buffer[6 * 8 + i] = b1 - b6;
    buffer[7 * 8 + i] = b0 - e7;
  }

  for (int i = 0; i < 8; i++) {
    float g0 = buffer[i * 8 + 0] * S0;
    float g1 = buffer[i * 8 + 4] * S4;
    float g2 = buffer[i * 8 + 2] * S2;
    float g3 = buffer[i * 8 + 6] * S6;
    float g4 = buffer[i * 8 + 5] * S5;
    float g5 = buffer[i * 8 + 1] * S1;
    float g6 = buffer[i * 8 + 7] * S7;
    float g7 = buffer[i * 8 + 3] * S3;

    float f4 = g4 - g7;
    float f5 = g5 + g6;
    float f6 = g5 - g6;
    float f7 = g4 + g7;

    float e2 = g2 - g3;
    float e3 = g2 + g3;
    float e5 = f5 - f7;
    float e7 = f5 + f7;
    float e8 = f4 + f6;

    float d2 = e2 * M1;
    float d4 = f4 * M2;
    float d5 = e5 * M3;
    float d6 = f6 * M4;
    float d8 = e8 * M5;

    float c0 = g0 + g1;
    float c1 = g0 - g1;
    float c2 = d2 - e3;
    float c4 = d4 + d8;
    float c5 = d5 + e7;
    float c6 = d6 - d8;
    float c8 = c5 - c6;

    float b0 = c0 + e3;
    float b1 = c1 + c2;
    float b2 = c1 - c2;
    float b3 = c0 - e3;
    float b4 = c4 - c8;
    float b6 = c6 - e7;

    buffer[i * 8 + 0] = b0 + e7;
    buffer[i * 8 + 1] = b1 + b6;
    buffer[i * 8 + 2] = b2 + c8;
    buffer[i * 8 + 3] = b3 + b4;
    buffer[i * 8 + 4] = b3 - b4;
    buffer[i * 8 + 5] = b2 - c8;
    buffer[i * 8 + 6] = b1 - b6;
    buffer[i * 8 + 7] = b0 - e7;
  }
}
#else
static void inverse_dct_component(short *buffer) {
  // ANN algorithm, intermediate values are bit shifted to the left to preserve precision
  // and then bit shifted to the right at the end
  for (int i = 0; i < 8; i++) {
    // Higher accuracy
    int g0 = (buffer[0 * 8 + i] * 181) >> 5;
    int g1 = (buffer[4 * 8 + i] * 181) >> 5;
    int g2 = (buffer[2 * 8 + i] * 59) >> 3;
    int g3 = (buffer[6 * 8 + i] * 49) >> 4;
    int g4 = (buffer[5 * 8 + i] * 71) >> 4;
    int g5 = (buffer[1 * 8 + i] * 251) >> 5;
    int g6 = (buffer[7 * 8 + i] * 25) >> 4;
    int g7 = (buffer[3 * 8 + i] * 213) >> 5;

    // Lower accuracy
    // int g0 = (buffer[0 * 8 + i] * 22) >> 2;
    // int g1 = (buffer[4 * 8 + i] * 22) >> 2;
    // int g2 = (buffer[2 * 8 + i] * 30) >> 2;
    // int g3 = (buffer[6 * 8 + i] * 12) >> 2;
    // int g4 = (buffer[5 * 8 + i] * 18) >> 2;
    // int g5 = (buffer[1 * 8 + i] * 31) >> 2;
    // int g6 = (buffer[7 * 8 + i] * 6) >> 2;
    // int g7 = (buffer[3 * 8 + i] * 27) >> 2;

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

    buffer[0 * 8 + i] = (b0 + e7) >> 4;
    buffer[1 * 8 + i] = (b1 + b6) >> 4;
    buffer[2 * 8 + i] = (b2 + c8) >> 4;
    buffer[3 * 8 + i] = (b3 + b4) >> 4;
    buffer[4 * 8 + i] = (b3 - b4) >> 4;
    buffer[5 * 8 + i] = (b2 - c8) >> 4;
    buffer[6 * 8 + i] = (b1 - b6) >> 4;
    buffer[7 * 8 + i] = (b0 - e7) >> 4;
  }

  for (int i = 0; i < 8; i++) {
    // Higher accuracy
    int g0 = (buffer[i * 8 + 0] * 181) >> 5;
    int g1 = (buffer[i * 8 + 4] * 181) >> 5;
    int g2 = (buffer[i * 8 + 2] * 59) >> 3;
    int g3 = (buffer[i * 8 + 6] * 49) >> 4;
    int g4 = (buffer[i * 8 + 5] * 71) >> 4;
    int g5 = (buffer[i * 8 + 1] * 251) >> 5;
    int g6 = (buffer[i * 8 + 7] * 25) >> 4;
    int g7 = (buffer[i * 8 + 3] * 213) >> 5;

    // Lower accuracy
    // int g0 = (buffer[i * 8 + 0] * 22) >> 2;
    // int g1 = (buffer[i * 8 + 4] * 22) >> 2;
    // int g2 = (buffer[i * 8 + 2] * 30) >> 2;
    // int g3 = (buffer[i * 8 + 6] * 12) >> 2;
    // int g4 = (buffer[i * 8 + 5] * 18) >> 2;
    // int g5 = (buffer[i * 8 + 1] * 31) >> 2;
    // int g6 = (buffer[i * 8 + 7] * 6) >> 2;
    // int g7 = (buffer[i * 8 + 3] * 27) >> 2;

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

    buffer[i * 8 + 0] = (b0 + e7) >> 4;
    buffer[i * 8 + 1] = (b1 + b6) >> 4;
    buffer[i * 8 + 2] = (b2 + c8) >> 4;
    buffer[i * 8 + 3] = (b3 + b4) >> 4;
    buffer[i * 8 + 4] = (b3 - b4) >> 4;
    buffer[i * 8 + 5] = (b2 - c8) >> 4;
    buffer[i * 8 + 6] = (b1 - b6) >> 4;
    buffer[i * 8 + 7] = (b0 - e7) >> 4;
  }
}
#endif

// https://en.wikipedia.org/wiki/YUV Y'UV444 to RGB888 conversion
static void ycbcr_to_rgb_pixel(short *buffer, short *cbcr, int v, int h) {
  int max_v = jpegInfo.max_v_samp_factor;
  int max_h = jpegInfo.max_h_samp_factor;

  // Iterating from bottom right to top leftbecause otherwise the pixel data will get overwritten
  for (int y = 7; y >= 0; y--) {
    for (int x = 7; x >= 0; x--) {
      uint32_t pixel = (y << 3) + x;
      uint32_t cbcr_pixel_row = y / max_v + 4 * v;
      uint32_t cbcr_pixel_col = x / max_h + 4 * h;
      uint32_t cbcr_pixel = (cbcr_pixel_row << 3) + cbcr_pixel_col + 64;

#if USE_FLOAT
      // Floating point version, most accurate, but floating point calculations in DPUs are emulated, so very slow
      short r = buffer[0][pixel] + 1.402 * cbcr[2][cbcr_pixel] + 128;
      short g = buffer[0][pixel] - 0.344 * cbcr[1][cbcr_pixel] - 0.714 * cbcr[2][cbcr_pixel] + 128;
      short b = buffer[0][pixel] + 1.772 * cbcr[1][cbcr_pixel] + 128;
#else
      // TODO: if multiplication is too slow, use bit shifting. However, bit shifting is less accurate from what I can
      // see int r = buffer[0][i] + buffer[2][i] + (buffer[2][i] >> 2) + (buffer[2][i] >> 3) + (buffer[2][i] >> 5) +
      // 128; int g = buffer[0][i] - ((buffer[1][i] >> 2) + (buffer[1][i] >> 4) + (buffer[1][i] >> 5)) -
      //         ((buffer[2][i] >> 1) + (buffer[2][i] >> 3) + (buffer[2][i] >> 4) + (buffer[2][i] >> 5)) + 128;
      // int b = buffer[0][i] + buffer[1][i] + (buffer[1][i] >> 1) + (buffer[1][i] >> 2) + (buffer[1][i] >> 6) + 128;

      // Integer only, quite accurate but may be less performant than only using bit shifting
      short r = buffer[pixel] + ((45 * cbcr[64 + cbcr_pixel]) >> 5) + 128;
      short g = buffer[pixel] - ((11 * cbcr[cbcr_pixel] + 23 * cbcr[64 + cbcr_pixel]) >> 5) + 128;
      short b = buffer[pixel] + ((113 * cbcr[cbcr_pixel]) >> 6) + 128;
#endif

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

      buffer[pixel] = r;
      buffer[64 + pixel] = g;
      buffer[128 + pixel] = b;
    }
  }
}

static short *decompress_scanline(JpegDecompressor *d) {
  short *mcus = (short *) malloc((jpegInfo.mcu_height_real * jpegInfo.mcu_width_real) * (3 * 64) * sizeof(short));
  short previous_dcs[3] = {0};
  uint32_t restart_interval = jpegInfo.restart_interval * jpegInfo.max_h_samp_factor * jpegInfo.max_v_samp_factor;

  for (uint32_t row = 0; row < jpegInfo.mcu_height; row += jpegInfo.max_v_samp_factor) {
    for (uint32_t col = 0; col < jpegInfo.mcu_width; col += jpegInfo.max_h_samp_factor) {
      if (restart_interval != 0 && (row * jpegInfo.mcu_width_real + col) % restart_interval == 0) {
        previous_dcs[0] = 0;
        previous_dcs[1] = 0;
        previous_dcs[2] = 0;

        // Align get buffer to next byte
        uint32_t offset = d->bits_left % 8;
        if (offset != 0) {
          d->bit_buffer <<= offset;
          d->bits_left -= offset;
        }
      }

      for (uint32_t color_index = 0; color_index < jpegInfo.num_color_components; color_index++) {
        for (uint32_t y = 0; y < jpegInfo.color_components[color_index].v_samp_factor; y++) {
          for (uint32_t x = 0; x < jpegInfo.color_components[color_index].h_samp_factor; x++) {
            // MCU to index is (current row + vertical sampling) * total number of MCUs in a row of the JPEG
            // + (current col + horizontal sampling)
            short *buffer = &mcus[(((row + y) * jpegInfo.mcu_width_real + (col + x)) * 3 + color_index) << 6];

            // Decode Huffman coded bitstream
            if (decode_mcu(d, color_index, buffer, &previous_dcs[color_index]) != 0) {
              jpegInfo.valid = 0;
              fprintf(stderr, "Error: Invalid MCU\n");
              free(mcus);
              return NULL;
            }

            // Compute inverse DCT with ANN algorithm
#if USE_FLOAT
            inverse_dct_component_float(buffer);
#else
            inverse_dct_component(buffer);
#endif
          }
        }
      }

      // Convert from YCbCr to RGB
      short *cbcr = &mcus[((row * jpegInfo.mcu_width_real + col) * 3) << 6];
      for (int y = jpegInfo.max_v_samp_factor - 1; y >= 0; y--) {
        for (int x = jpegInfo.max_h_samp_factor - 1; x >= 0; x--) {
          short *buffer = &mcus[(((row + y) * jpegInfo.mcu_width_real + (col + x)) * 3) << 6];
          ycbcr_to_rgb_pixel(buffer, cbcr, y, x);
        }
      }
    }
  }

  return mcus;
}

/**
 * Read JPEG markers
 * Return 0 when the SOS marker is found
 * Otherwise return 1
 *
 * @param d JpegDecompressor struct that holds all information about the JPEG currently being decoded
 */
static int read_next_marker(JpegDecompressor *d) {
  int marker;

  marker = skip_to_next_marker(d);
  switch (marker) {
    case -1:
      jpegInfo.valid = 0;
      fprintf(stderr, "Error: Read past EOF\n");
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
      fprintf(stderr, "Error: Unhandled marker: FF %X\n", marker);
      break;
  }

  return 1;
}

#if DEBUG
static void print_jpeg_decompressor(JpegDecompressor *d) {
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
}

static void init_jpeg_decompressor(JpegDecompressor *d) {
  d->bit_buffer = 0;
  d->bits_left = 0;
}

/**
 * Entry point for decoding JPEG using CPU
 *
 * @param file_length The total length of a file in bytes
 * @param filename The filename of the input file
 * @param buffer The buffer containing all file data
 */
void jpeg_cpu_scale(uint64_t file_length, char *filename, char *buffer) {
  JpegDecompressor decompressor;
  decompressor.length = file_length;
  jpegInfo.length = decompressor.length;

  int result = 1;

  decompressor.data = buffer;
  decompressor.ptr = decompressor.data;

  init_jpeg_info();
  init_jpeg_decompressor(&decompressor);

  // Check whether file starts with SOI
  check_start_of_image(&decompressor);

  // Continuously read all markers until we reach Huffman coded bitstream
  while (jpegInfo.valid && result) {
    result = read_next_marker(&decompressor);
  }

  if (!jpegInfo.valid) {
    return;
  }

#if DEBUG
  print_jpeg_decompressor(&decompressor);
#endif

  // Process Huffman coded bitstream, perform inverse DCT, and convert YCbCr to RGB
  short *mcus = decompress_scanline(&decompressor);
  if (mcus == NULL || !jpegInfo.valid) {
    fprintf(stderr, "Error: Invalid JPEG\n");
    return;
  }

  // Now write the decoded data out as BMP
  write_bmp_cpu(filename, jpegInfo.image_width, jpegInfo.image_height, jpegInfo.padding, jpegInfo.mcu_width_real, mcus);
  free(mcus);

  return;
}
