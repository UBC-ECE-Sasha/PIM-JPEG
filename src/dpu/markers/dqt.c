#include <stdio.h>

#include "dpu-jpeg.h"

static int read_and_form_DQT(JpegDecompressor *d, int *length);
static void form_low_precision_DQT(JpegDecompressor *d, int *length, uint8_t table_id);
static void form_high_precision_DQT(JpegDecompressor *d, int *length, uint8_t table_id);

// Page 39: Section B.2.4.1
int process_DQT(JpegDecompressor *d) {
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
