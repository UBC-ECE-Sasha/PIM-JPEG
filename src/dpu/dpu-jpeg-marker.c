#include <stdio.h>

#include "dpu-jpeg.h"

// /**
//  * Helper array for filling in quantization table in zigzag order
//  */
// const uint8_t ZIGZAG_ORDER[] = {0,  1,  8,  16, 9,  2,  3,  10, 17, 24, 32, 25, 18, 11, 4,  5,  12, 19, 26, 33, 40,
// 48,
//                                 41, 34, 27, 20, 13, 6,  7,  14, 21, 28, 35, 42, 49, 56, 57, 50, 43, 36, 29, 22, 15,
//                                 23, 30, 37, 44, 51, 58, 59, 52, 45, 38, 31, 39, 46, 53, 60, 61, 54, 47, 55, 62, 63};

int check_start_of_image(JpegDecompressor *d) {
  uint8_t c1 = 0, c2 = 0;

  if (!is_eof(d)) {
    c1 = read_byte(d);
    c2 = read_byte(d);
  }
  if (c1 != 0xFF || c2 != M_SOI) {
    printf("Error: Not JPEG: %X %X\n", c1, c2);
    return JPEG_INVALID_ERROR_CODE;
  }
  return JPEG_VALID;
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
    printf("Error: Invalid DQT - got quantization table ID: %d, ID should be between 0 and 3\n", table_id);
    return JPEG_INVALID_ERROR_CODE;
  }
  jpegInfo.quant_tables[table_id].exists = 1;

  uint8_t precision = (qt_info >> 4) & 0x0F; // Pq
  if (precision == 0) {
    form_low_precision_DQT(d, length, table_id);
  } else {
    form_high_precision_DQT(d, length, table_id);
  }

  return JPEG_VALID;
}

// Page 39: Section B.2.4.1
static int process_DQT(JpegDecompressor *d) {
  int length = read_short(d); // Lq
  length -= 2;

  while (length > 0) {
    int error = read_and_form_DQT(d, &length);
    if (error) {
      return error;
    }
  }

  if (length != 0) {
    printf("Error: Invalid DQT - length incorrect\n");
    return JPEG_INVALID_ERROR_CODE;
  }
  return JPEG_VALID;
}

static int process_DRI(JpegDecompressor *d) {
  int length = read_short(d); // Lr
  if (length != 4) {
    printf("Error: Invalid DRI - length is not 4\n");
    return JPEG_INVALID_ERROR_CODE;
  }

  jpegInfo.restart_interval = read_short(d); // Ri
  return JPEG_VALID;
}

static int read_SOF_metadata(JpegDecompressor *d) {
  uint8_t precision = read_byte(d); // P
  if (precision != 8) {
    printf("Error: Invalid SOF - precision is %d, should be 8\n", precision);
    return JPEG_INVALID_ERROR_CODE;
  }

  jpegInfo.image_height = read_short(d); // Y
  jpegInfo.image_width = read_short(d);  // X
  if (jpegInfo.image_height == 0 || jpegInfo.image_width == 0) {
    printf("Error: Invalid SOF - dimensions: %d x %d\n", jpegInfo.image_width, jpegInfo.image_height);
    return JPEG_INVALID_ERROR_CODE;
  }

  jpegInfo.num_color_components = read_byte(d); // Nf
  if (jpegInfo.num_color_components == 0 || jpegInfo.num_color_components > 3) {
    printf("Error: Invalid SOF - number of color components: %d\n", jpegInfo.num_color_components);
    return JPEG_INVALID_ERROR_CODE;
  }

  return JPEG_VALID;
}

static int read_SOF_color_component_info(JpegDecompressor *d) {
  uint8_t component_id = read_byte(d); // Ci
  if (component_id == 0 || component_id > 3) {
    printf("Error: Invalid SOF - component ID: %d\n", component_id);
    return JPEG_INVALID_ERROR_CODE;
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
      printf("Error: Invalid SOF - horizontal or vertical sampling factor for luminance out of range %d %d\n",
             component->h_samp_factor, component->v_samp_factor);
      return JPEG_INVALID_ERROR_CODE;
    }

    jpegInfo.max_h_samp_factor = component->h_samp_factor;
    jpegInfo.max_v_samp_factor = component->v_samp_factor;
  } else if (component->h_samp_factor != 1 || component->v_samp_factor != 1) {
    printf("Error: Invalid SOF - horizontal and vertical sampling factor for Cr and Cb not 1");
    return JPEG_INVALID_ERROR_CODE;
  }

  component->quant_table_id = read_byte(d); // Tqi

  return JPEG_VALID;
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
  jpegInfoDpu.rows_per_mcu = jpegInfo.mcu_height_real / NR_TASKLETS;
}

// Page 35: Section B.2.2
static int process_SOFn(JpegDecompressor *d) {
  if (jpegInfo.num_color_components != 0) {
    printf("Error: Invalid SOF - multiple SOFs encountered\n");
    return JPEG_INVALID_ERROR_CODE;
  }

  int length = read_short(d); // Lf

  int error = read_SOF_metadata(d);
  if (error) {
    return error;
  }

  for (int i = 0; i < jpegInfo.num_color_components; i++) {
    error = read_SOF_color_component_info(d);
    if (error) {
      return error;
    }
  }

  initialize_MCU_height_width();

  if (length - 8 - (3 * jpegInfo.num_color_components) != 0) {
    printf("Error: Invalid SOF - length incorrect\n");
    return JPEG_INVALID_ERROR_CODE;
  }

  return JPEG_VALID;
}

static int read_DHT(JpegDecompressor *d, int *length) {
  uint8_t ht_info = read_byte(d);
  *length -= 1;

  uint8_t table_id = ht_info & 0x0F;        // Th
  uint8_t ac_table = (ht_info >> 4) & 0x0F; // Tc
  if (table_id > 3) {
    printf("Error: Invalid DHT - Huffman Table ID: %d\n", table_id);
    return JPEG_INVALID_ERROR_CODE;
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

  return JPEG_VALID;
}

// Page 40: Section B.2.4.2
static int process_DHT(JpegDecompressor *d) {
  int length = read_short(d); // Lf
  length -= 2;

  while (length > 0) {
    int error = read_DHT(d, &length);
    if (error) {
      return error;
    }
  }

  if (length != 0) {
    printf("Error: Invalid DHT - length incorrect\n");
    return JPEG_INVALID_ERROR_CODE;
  }

  return JPEG_VALID;
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
  if (component_id == 0 || component_id > 3) {
    printf("Error: Invalid SOS - component ID: %d\n", component_id);
    return JPEG_INVALID_ERROR_CODE;
  }

  ColorComponentInfo *component = &jpegInfo.color_components[component_id - 1];
  uint8_t tdta = read_byte(d);
  component->dc_huffman_table_id = (tdta >> 4) & 0x0F; // Tdj
  component->ac_huffman_table_id = tdta & 0x0F;        // Taj

  return JPEG_VALID;
}

static int read_SOS_metadata(JpegDecompressor *d) {
  jpegInfo.ss = read_byte(d); // Ss
  jpegInfo.se = read_byte(d); // Se
  uint8_t A = read_byte(d);
  jpegInfo.Ah = (A >> 4) & 0xF; // Ah
  jpegInfo.Al = A & 0xF;        // Al

  if (jpegInfo.ss != 0 || jpegInfo.se != 63) {
    printf("Error: Invalid SOS - invalid spectral selection\n");
    return JPEG_INVALID_ERROR_CODE;
  }
  if (jpegInfo.Ah != 0 || jpegInfo.Al != 0) {
    printf("Error: Invalid SOS - invalid successive approximation\n");
    return JPEG_INVALID_ERROR_CODE;
  }

  return JPEG_VALID;
}

// Page 37: Section B.2.3
static int process_SOS(JpegDecompressor *d) {
  int length = read_short(d); // Ls

  uint8_t num_components = read_byte(d); // Ns
  if (num_components == 0 || num_components != jpegInfo.num_color_components) {
    printf("Error: Invalid SOS - number of color components does not match SOF: %d vs %d\n", num_components,
           jpegInfo.num_color_components);
    return JPEG_INVALID_ERROR_CODE;
  }

  for (int i = 0; i < num_components; i++) {
    int error = read_SOS_color_component_info(d);
    if (error) {
      return error;
    }
  }

  int error = read_SOS_metadata(d);
  if (error) {
    return error;
  }

  if (length - 6 - (2 * num_components) != 0) {
    printf("Error: Invalid SOS - length incorrect\n");
    return JPEG_INVALID_ERROR_CODE;
  }

  build_huffman_tables();

  return JPEG_VALID;
}

/**
 * Read JPEG markers
 * Return 0 when the SOS marker is found
 * Otherwise return 1
 *
 * @param d JpegDecompressor struct that holds all information about the JPEG currently being decoded
 */
int read_next_marker(JpegDecompressor *d) {
  int marker = skip_to_next_marker(d);
  int process_result;

  switch (marker) {
    case -1:
      jpegInfo.valid = 0;
      printf("Error: Read past EOF\n");
      break;

    case M_APP_FIRST ... M_APP_LAST:
      process_result = skip_marker(d);
      break;

    case M_DQT:
      process_result = process_DQT(d);
      break;

    case M_DRI:
      process_result = process_DRI(d);
      break;

    case M_SOF0:
      // case M_SOF5 ... M_SOF7:
      // case M_SOF9 ... M_SOF11:
      // case M_SOF13 ... M_SOF15:
      process_result = process_SOFn(d);
      break;

    case M_DHT:
      process_result = process_DHT(d);
      break;

    case M_SOS:
      process_result = process_SOS(d);
      return 0;

    case M_COM:
    case M_EXT_FIRST ... M_EXT_LAST:
    case M_DNL:
    case M_DHP:
    case M_EXP:
      process_result = skip_marker(d);
      break;

    default:
      jpegInfo.valid = 0;
      printf("Error: Unhandled marker: FF %X\n", marker);
      break;
  }

  if (process_result != JPEG_VALID) {
    jpegInfo.valid = 0;
  }

  return 1;
}