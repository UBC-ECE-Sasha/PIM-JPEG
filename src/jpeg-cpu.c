#include <byteswap.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "bmp.h"
#include "jpeg-cpu.h"

#define rounded_division(_a, _b) (((_a) + (_b)-1) / (_b))

/* We want to emulate the behaviour of 'tjbench <jpg> -scale 1/8'
        That calls 'process_data_simple_main' and 'decompress_onepass' in
turbojpeg On my laptop, I see:

./tjbench ../DSCF5148.JPG -scale 1/8

>>>>>  JPEG 4:2:2 --> BGR (Top-down)  <<<<<

Image size: 1920 x 1080 --> 240 x 135
Decompress    --> Frame rate:         92.177865 fps
                  Throughput:         191.140021 Megapixels/sec
*/

/* Error exit handler */
#define ERREXIT(msg) return -1;

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
  if (length < 2)
    ERREXIT("Erroneous JPEG marker length");

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
    printf("Error: Not JPEG: %X %X\n", c1, c2);
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
      printf("Error: Invalid quantization table ID: %d, ID should be between 0 and 3\n", table_id);
      return;
    }
    d->quant_table[table_id].exists = 1;

    uint8_t precision = (qt_info >> 4) & 0x0F; // Pq
    if (precision == 0) {
      // 8 bit precision
      for (int i = 0; i < 64; i++) {
        d->quant_table[table_id].table[ZIGZAG_ORDER[i]] = read_byte(d); // Qk
      }
      length -= 64;
    } else {
      // 16 bit precision
      for (int i = 0; i < 64; i++) {
        d->quant_table[table_id].table[ZIGZAG_ORDER[i]] = read_short(d); // Qk
      }
      length -= 128;
    }
  }

  if (length != 0) {
    d->valid = 0;
    printf("Error: Invalid DQT - Actual quantization table length does not match length field\n");
  }
}

/**
 * Read Restart Interval
 */
static void process_DRI(JpegDecompressor *d) {
  int length = read_short(d);
  if (length != 4) {
    d->valid = 0;
    printf("Error: Invalid DRI - length is not 4\n");
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
    printf("Error: Multiple SOFs encountered\n");
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
    if (component->h_samp_factor != 1 || component->v_samp_factor != 1) {
      d->valid = 0;
      printf("Error: Invalid SOF - horizontal and vertical samplying factor other than 1 not support yet\n");
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
    printf("Error: Invalid SOS - number of color components does not match SOF: %d vs %d", num_components,
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

/* F.2.2.3 Decode */

/**
 * Get number of specified bits from the buffer
 */
static uint8_t get_bits(JpegDecompressor *d, uint8_t num_bits) {
  uint8_t b;
  uint32_t c;
  uint8_t temp;

  while (d->bits_left < num_bits) {
    // read a byte and decode it, if it is 0xFF
    b = read_byte(d);
    c = b;
    printf("Read byte 0x%x\n", b);
    while (b == 0xFF) {
      printf("Warning: got 0xFF byte\n");
      b = read_byte(d);
      if (b == 0)
        c = 0xFF;
      else
        c = b;
    }

    // add the new bits to the buffer (MSB aligned)
    // printf("before: buffer: 0x%08X (left %u)\n", d->get_buffer,
    // d->bits_left);
    d->get_buffer |= c << (32 - 8 - d->bits_left);
    d->bits_left += 8;
    // printf("after: buffer: 0x%08X (left %u)\n", d->get_buffer, d->bits_left);
  }

  temp = d->get_buffer >> (32 - num_bits);
  printf("Getting %u bits: 0x%u\n", num_bits, temp);
  d->get_buffer <<= num_bits;
  d->bits_left -= num_bits;
  // printf("remain: buffer: 0x%08X\n", d->get_buffer);
  return temp;
}

static uint16_t huff_decode(JpegDecompressor *d, HuffmanTable *table) {
  uint32_t l = 1;
  uint16_t code;
  code = get_bits(d, 1);

  // while (code > table->maxcode[l])
  // {
  // 	code <<= 1;
  // 	l++;
  // }

  return 0;
}

/* F.2.1.2 Decode 8x8 block data unit */
// static int decode_mcu(JpegDecompressor *d, uint8_t *buffer) {
//   uint32_t block;

//   printf("%s: left %u\n", __func__, d->restarts_left);
//   if (d->restart_interval) {
//     if (d->restarts_left == 0) {
//       if (process_restart(d) != 0) {
//         printf("failed processing restart\n");
//         return -1;
//       }
//     }
//   }

//   for (block = 0; block < d->blocks_per_MCU; block++) {
//     uint8_t c;
//     struct HuffmanTable *dctbl = &d->huffman[0];
//     struct HuffmanTable *actbl = &d->huffman[1];

//     // decode DC coefficient (F.2.2.1)
//     uint16_t t = huff_decode(d, dctbl);
//     // diff = RECEIVE(t);
//     // diff = HUFFEXTEND(diff, t);

//     // decode AC coefficients

//     // dequantize
//   }

//   d->restarts_left--;

//   return 0;
// }

// int decompress_scanline(JpegDecompressor *d) {
//   uint8_t mcu_buffer[64];

//   uint32_t mcu_height = (d->image_height + 7) / 8;
//   uint32_t mcu_width = (d->image_width + 7) / 8;

//   // uint32_t row;
//   // for (row = 0; row < d->rows_per_scan; row++)
//   {
//     // each entropy-coded segment except the last one shall contain
//     // 'restart_interval' MCUs
//     uint32_t mcu;
//     for (mcu = 0; mcu < d->MCUs_per_row; mcu++) {
//       printf("Decoding MCU %u\n", mcu);
//       if (decode_mcu(d, mcu_buffer) != 0) {
//         printf("Error decoding MCU\n");
//         return -1;
//       }

//       for (uint8_t i = 0; i < d->comps_in_scan; i++) {
//         uint32_t yindex, xindex;
//         for (yindex = 0; yindex < d->comp_info[i].v_samp_factor; yindex++) {
//           for (xindex = 0; xindex < d->comp_info[i].h_samp_factor; xindex++) {
//             // inverse_DCT(d, mcu_buffer);
//           }
//         }
//       }
//     }
//   }

//   return 0;
// }

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

    case M_SOF0 ... M_SOF3:
    case M_SOF5 ... M_SOF7:
    case M_SOF9 ... M_SOF11:
    case M_SOF13 ... M_SOF15:
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
      printf("Error: Unhandled marker: FF %X\n", marker);
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
    if (d->quant_table[i].exists) {
      printf("Table ID: %d", i);
      for (int j = 0; j < 64; j++) {
        if (j % 8 == 0) {
          printf("\n");
        }
        printf("%d ", d->quant_table[i].table[j]);
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
}

/**
 * Initialize the JPEG decompressor with default values
 */
static void init_jpeg_decompressor(JpegDecompressor *d) {
  d->valid = 1;

  for (int i = 0; i < 4; i++) {
    d->quant_table[i].exists = 0;
    d->dc_huffman_tables[i].exists = 0;
    d->ac_huffman_tables[i].exists = 0;
  }
  for (int i = 0; i < 3; i++) {
    d->color_components[i].exists = 0;
  }

  d->restart_interval = 0;
  d->image_height = 0;
  d->image_width = 0;
  d->num_color_components = 0;
  d->ss = 0;
  d->se = 0;
  d->Ah = 0;
  d->Al = 0;
}

void jpeg_cpu_scale(uint64_t file_length, char *buffer) {
  JpegDecompressor decompressor;
  int res = 1;

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
    printf("Error: Invalid JPEG\n");
    return;
  }

  // Process Huffman coded bitstream
  print_jpeg_decompressor(&decompressor);

  // decompress_scanline(&decompressor);

  return;
}
