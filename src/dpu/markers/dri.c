#include <stdio.h>

#include "dpu-jpeg.h"

int process_DRI(JpegDecompressor *d) {
  int length = read_short(d); // Lr

  if (length != 4) {
    printf("Error: Invalid DRI - length is not 4\n");
    return JPEG_INVALID_ERROR_CODE;
  }

  jpegInfo.restart_interval = read_short(d); // Ri
  return JPEG_VALID;
}
