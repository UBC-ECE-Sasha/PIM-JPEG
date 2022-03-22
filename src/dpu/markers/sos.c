#include <stdio.h>

#include "dpu-jpeg.h"

static int read_SOS_color_component_info(JpegDecompressor *d);
static int read_SOS_metadata(JpegDecompressor *d);
static void build_huffman_tables();
static void generate_codes(HuffmanTable *h_table);

// Page 37: Section B.2.3
int process_SOS(JpegDecompressor *d) {
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

static int read_SOS_color_component_info(JpegDecompressor *d) {
  uint8_t component_id = read_byte(d); // Csj
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
    printf("Error: Invalid SOS - component ID: %d\n", component_id);
    return JPEG_INVALID_ERROR_CODE;
  }

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
