#include <stdio.h>

#include "dpu-jpeg.h"

static int read_DHT(JpegDecompressor *d, int *length);

// Page 40: Section B.2.4.2
int process_DHT(JpegDecompressor *d) {
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
