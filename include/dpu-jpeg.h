#ifndef _DPU_JPEG_H
#define _DPU_JPEG_H

#include <jpeg-common.h>
#include <stdint.h>

#include "common.h"

#ifndef NR_TASKLETS
#define NR_TASKLETS 16
#endif

typedef struct JpegInfoDpu {
  uint32_t mcu_end_index[NR_TASKLETS];   // end index of each tasklet in the 2D MRAM MCU buffer
  uint32_t mcu_start_index[NR_TASKLETS]; // start index of each tasklet in the 2D MRAM MCU buffer
  int dc_offset[NR_TASKLETS - 1][3];     // offset to the 3 DC coefficients from tasklet i to tasklet i + 1
  uint32_t rows_per_tasklet;
} JpegInfoDpu;

void init_file_reader_index(JpegDecompressor *d);
void init_jpeg_decompressor(JpegDecompressor *d);
uint8_t read_byte(JpegDecompressor *d);
uint16_t read_short(JpegDecompressor *d);
int is_eof(JpegDecompressor *d);
void skip_bytes(JpegDecompressor *d, int num_bytes);

int check_start_of_image(JpegDecompressor *d);
int read_next_marker(JpegDecompressor *d);
int process_DQT(JpegDecompressor *d);
int process_DRI(JpegDecompressor *d);
int process_SOFn(JpegDecompressor *d);
int process_DHT(JpegDecompressor *d);
int process_SOS(JpegDecompressor *d);

void decode_bitstream(JpegDecompressor *d);
void inverse_dct_convert(JpegDecompressor *d);

void horizontal_flip(JpegDecompressor *d);
void crop(JpegDecompressor *d, int start_x, int start_y, int new_width, int new_height);
void jpeg_scale(JpegDecompressor *d, int x_scale_factor, int y_scale_factor);

extern JpegInfo jpegInfo;
extern JpegInfoDpu jpegInfoDpu;

#endif // _DPU_JPEG_H