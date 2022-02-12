#include <stdio.h>

#include "dpu-jpeg.h"

static int read_SOF_metadata(JpegDecompressor *d);
static int read_SOF_color_component_info(JpegDecompressor *d);
static void initialize_MCU_height_width();

// Page 35: Section B.2.2
int process_SOFn(JpegDecompressor *d) {
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
  jpegInfo.mcu_count_v = (jpegInfo.image_height + 7) / 8;
  jpegInfo.mcu_count_h = (jpegInfo.image_width + 7) / 8;
  jpegInfo.padding = jpegInfo.image_width % 4;
  jpegInfo.mcu_count_v_real = jpegInfo.mcu_count_v;
  jpegInfo.mcu_count_h_real = jpegInfo.mcu_count_h;
  if (jpegInfo.max_v_samp_factor == 2 && jpegInfo.mcu_count_v_real % 2 == 1) {
    jpegInfo.mcu_count_v_real++;
  }
  if (jpegInfo.max_h_samp_factor == 2 && jpegInfo.mcu_count_h_real % 2 == 1) {
    jpegInfo.mcu_count_h_real++;
  }
  jpegInfoDpu.rows_per_tasklet = jpegInfo.mcu_count_v_real / NR_TASKLETS;
}
