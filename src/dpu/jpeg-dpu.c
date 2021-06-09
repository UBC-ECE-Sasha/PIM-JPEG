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
 * Skip count bytes
 *
 * @param d JpegDecompressor struct that holds all information about the JPEG currently being decoded
 * @param count The number of bytes to skip
 */
static void skip_bytes(JpegDecompressor *d, int count) {
  // If after skipping count bytes we go beyond EOF, then only skip till EOF
  if (d->index + count > d->length)
    d->index = d->length;
  else
    d->index += count;
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
    d->valid = 0;
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
    d->valid = 0;
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
      d->valid = 0;
      printf("Error: Invalid DQT - got quantization table ID: %d, ID should be between 0 and 3\n", table_id);
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
    d->valid = 0;
    printf("Error: Invalid DRI - length is not 4\n");
    return;
  }

  d->restart_interval = read_short(d);
}

/**
 * Read Start Of Frame
 * Page 36: Table B.2
 *
 * @param d JpegDecompressor struct that holds all information about the JPEG currently being decoded
 */
static void process_SOFn(JpegDecompressor *d) {
  if (d->num_color_components != 0) {
    d->valid = 0;
    printf("Error: Invalid SOF - multiple SOFs encountered\n");
    return;
  }

  int length = read_short(d); // Lf

  uint8_t precision = read_byte(d); // P
  if (precision != 8) {
    d->valid = 0;
    printf("Error: Invalid SOF - precision is %d, should be 8\n", precision);
    return;
  }

  d->image_height = read_short(d); // Y
  d->image_width = read_short(d);  // X
  if (d->image_height == 0 || d->image_width == 0) {
    d->valid = 0;
    printf("Error: Invalid SOF - dimensions: %d x %d\n", d->image_width, d->image_height);
    return;
  }

  d->mcu_height = (d->image_height + 7) / 8;
  d->mcu_width = (d->image_width + 7) / 8;
  d->padding = d->image_width % 4;
  d->mcu_height_real = d->mcu_height;
  d->mcu_width_real = d->mcu_width;

  // Should be 3
  d->num_color_components = read_byte(d); // Nf
  if (d->num_color_components == 0 || d->num_color_components > 3) {
    d->valid = 0;
    printf("Error: Invalid SOF - number of color components: %d\n", d->num_color_components);
    return;
  }

  for (int i = 0; i < d->num_color_components; i++) {
    // The component ID is expected to be 1, 2, or 3
    uint8_t component_id = read_byte(d); // Ci
    if (component_id == 0 || component_id > 3) {
      d->valid = 0;
      printf("Error: Invalid SOF - component ID: %d\n", component_id);
      return;
    }

    ColorComponentInfo *component = &d->color_components[component_id - 1];
    component->exists = 1;
    component->component_id = component_id;

    uint8_t factor = read_byte(d);
    component->h_samp_factor = (factor >> 4) & 0x0F; // Hi
    component->v_samp_factor = factor & 0x0F;        // Vi
    if (component_id == 1) {
      // Only luminance channel can have horizontal or vertical sampling factor greater than 1
      if ((component->h_samp_factor != 1 && component->h_samp_factor != 2) ||
          (component->v_samp_factor != 1 && component->v_samp_factor != 2)) {
        d->valid = 0;
        printf("Error: Invalid SOF - horizontal or vertical sampling factor for luminance out of range %d %d\n",
               component->h_samp_factor, component->v_samp_factor);
        return;
      }

      if (component->h_samp_factor == 2 && d->mcu_width % 2 == 1) {
        // Add padding to real MCU width if horizontal sampling factor is 2
        d->mcu_width_real++;
      }
      if (component->v_samp_factor == 2 && d->mcu_height % 2 == 1) {
        // Add padding to real MCU height if vertical sampling factor is 2
        d->mcu_height_real++;
      }

      d->max_h_samp_factor = component->h_samp_factor;
      d->max_v_samp_factor = component->v_samp_factor;
    } else if (component->h_samp_factor != 1 || component->v_samp_factor != 1) {
      d->valid = 0;
      printf("Error: Invalid SOF - horizontal and vertical sampling factor for Cr and Cb not 1");
      return;
    }

    component->quant_table_id = read_byte(d); // Tqi
  }

  if (length - 8 - (3 * d->num_color_components) != 0) {
    d->valid = 0;
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
      d->valid = 0;
      printf("Error: Invalid DHT - Huffman Table ID: %d\n", table_id);
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
 *
 * @param d JpegDecompressor struct that holds all information about the JPEG currently being decoded
 */
static void process_SOS(JpegDecompressor *d) {
  int length = read_short(d); // Ls
  length -= 2;

  uint8_t num_components = read_byte(d); // Ns
  length -= 1;
  if (num_components == 0 || num_components != d->num_color_components) {
    d->valid = 0;
    printf("Error: Invalid SOS - number of color components does not match SOF: %d vs %d\n", num_components,
           d->num_color_components);
    return;
  }

  for (int i = 0; i < num_components; i++) {
    uint8_t component_id = read_byte(d); // Csj
    if (component_id == 0 || component_id > 3) {
      d->valid = 0;
      printf("Error: Invalid SOS - component ID: %d\n", component_id);
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
    printf("Error: Invalid SOS - invalid spectral selection\n");
    return;
  }
  if (d->Ah != 0 || d->Al != 0) {
    d->valid = 0;
    printf("Error: Invalid SOS - invalid successive approximation\n");
    return;
  }

  if (length - (2 * num_components) != 0) {
    d->valid = 0;
    printf("Error: Invalid SOS - length incorrect\n");
  }

  build_huffman_tables(d);
}

/**
 * Decode the Huffman coded bitstream, compute inverse DCT, and convert from YCbCr to RGB
 *
 * @param d JpegDecompressor struct that holds all information about the JPEG currently being decoded
 */
static MCU *decompress_scanline(JpegDecompressor *d) {
  MCU *mcus = (MCU *)malloc((d->mcu_height_real * d->mcu_width_real) * sizeof(MCU));
  int previous_dcs[3] = {0};

  for (uint32_t row = 0; row < d->mcu_height; row += d->max_v_samp_factor) {
    for (uint32_t col = 0; col < d->mcu_width; col += d->max_h_samp_factor) {
      // TODO: Restart Intervals

      for (uint32_t index = 0; index < d->num_color_components; index++) {
        for (uint32_t y = 0; y < d->color_components[index].v_samp_factor; y++) {
          for (uint32_t x = 0; x < d->color_components[index].h_samp_factor; x++) {
            // MCU to index is (current row + vertical sampling) * total number of MCUs in a row of the JPEG
            // + (current col + horizontal sampling)
            int *buffer = mcus[(row + y) * d->mcu_width_real + (col + x)].buffer[index];

            // Decode Huffman coded bitstream
            if (decode_mcu(d, index, buffer, &previous_dcs[index]) != 0) {
              d->valid = 0;
              printf("Error: Invalid MCU\n");
              free(mcus);
              return NULL;
            }

            // Compute inverse DCT with ANN algorithm
            // inverse_dct_component(buffer);
          }
        }
      }

      // int(*cbcr)[64] = mcus[row * d->mcu_width_real + col].buffer;
      // // Convert from YCbCr to RGB
      // for (int y = d->max_v_samp_factor - 1; y >= 0; y--) {
      //   for (int x = d->max_h_samp_factor - 1; x >= 0; x--) {
      //     ycbcr_to_rgb_pixel(mcus[(row + y) * d->mcu_width_real + (col + x)].buffer, cbcr, d->max_v_samp_factor,
      //                        d->max_h_samp_factor, y, x);
      //   }
      // }
    }
  }

  return mcus;
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
      d->valid = 0;
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
      d->valid = 0;
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
      d->valid = 0;
      printf("Error: Unhandled marker: FF %X\n", marker);
      break;
  }

  return 1;
}

/**
 * Helper function to print out filled in values of the JPEG decompressor
 *
 * @param d JpegDecompressor struct that holds all information about the JPEG currently being decoded
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
  for (int i = 0; i < 2; i++) {
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
  for (int i = 0; i < 2; i++) {
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

  // Continuously read all markers until we reach Huffman coded bitstream
  while (decompressor.valid && res) {
    res = read_marker(&decompressor);
  }

  if (!decompressor.valid) {
    printf("Error: Invalid JPEG\n");
    return 1;
  }

  print_jpeg_decompressor(&decompressor);

  // Process Huffman coded bitstream, perform inverse DCT, and convert YCbCr to RGB
  // MCU *mcus = decompress_scanline(&decompressor);
  // if (mcus == NULL || !decompressor.valid) {
  //   printf("Error: Invalid JPEG\n");
  //   return;
  // }

  printf("\n");
  return 0;
}