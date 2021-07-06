#include <stdio.h>

#include "dpu-jpeg.h"

static int skip_to_next_marker(JpegDecompressor *d);
static int skip_marker(JpegDecompressor *d);
static int count_and_skip_non_marker_bytes(JpegDecompressor *d);
static int get_marker_and_ignore_ff_bytes(JpegDecompressor *d);

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

static int skip_to_next_marker(JpegDecompressor *d) {
  int num_skipped_bytes = count_and_skip_non_marker_bytes(d);
  int marker = get_marker_and_ignore_ff_bytes(d);

  if (num_skipped_bytes) {
    printf("WARNING: Discarded %u bytes\n", num_skipped_bytes);
  }

  return marker;
}

static int skip_marker(JpegDecompressor *d) {
  int length = read_short(d);
  length -= 2;

  if (length < 0) {
    printf("ERROR: Invalid length encountered in skip_marker");
    return JPEG_INVALID_ERROR_CODE;
  }

  skip_bytes(d, length);
  return JPEG_VALID;
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
