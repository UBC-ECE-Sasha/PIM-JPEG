#include <barrier.h>
#include <defs.h>
#include <mram.h>
#include <stdio.h>

#include "dpu-jpeg.h"
#include "jpeg-host.h"

__host uint64_t file_length;
__host dpu_output_t output;

JpegInfo jpegInfo;
JpegInfoDpu jpegInfoDpu;

BARRIER_INIT(init_barrier, NR_TASKLETS);
BARRIER_INIT(idct_barrier, NR_TASKLETS);

#define DEBUG 0

#if DEBUG
static void print_jpeg_decompressor() {
  printf("\n********** DQT **********\n");
  for (int i = 0; i < 4; i++) {
    if (jpegInfo.quant_tables[i].exists) {
      printf("Table ID: %d", i);
      for (int j = 0; j < 64; j++) {
        if (j % 8 == 0) {
          printf("\n");
        }
        printf("%d ", jpegInfo.quant_tables[i].table[j]);
      }
      printf("\n\n");
    }
  }

  printf("********** DRI **********\n");
  printf("Restart Interval: %d\n", jpegInfo.restart_interval);

  printf("\n********** SOF **********\n");
  printf("Width: %d\n", jpegInfo.image_width);
  printf("Height: %d\n", jpegInfo.image_height);
  printf("Number of color components: %d\n\n", jpegInfo.num_color_components);
  for (int i = 0; i < jpegInfo.num_color_components; i++) {
    printf("Component ID: %d\n", jpegInfo.color_components[i].component_id);
    printf("H-samp factor: %d\n", jpegInfo.color_components[i].h_samp_factor);
    printf("V-samp factor: %d\n", jpegInfo.color_components[i].v_samp_factor);
    printf("Quantization table ID: %d\n\n", jpegInfo.color_components[i].quant_table_id);
  }

  printf("\n********** DHT **********\n");
  for (int i = 0; i < MAX_HUFFMAN_TABLES; i++) {
    if (jpegInfo.dc_huffman_tables[i].exists) {
      printf("DC Table ID: %d\n", i);
      for (int j = 0; j < 16; j++) {
        printf("%d: ", j + 1);
        for (int k = jpegInfo.dc_huffman_tables[i].valoffset[j]; k < jpegInfo.dc_huffman_tables[i].valoffset[j + 1];
             k++) {
          printf("%d ", jpegInfo.dc_huffman_tables[i].huffval[k]);
        }
        printf("\n");
      }
      printf("\n");
    }
  }
  for (int i = 0; i < MAX_HUFFMAN_TABLES; i++) {
    if (jpegInfo.ac_huffman_tables[i].exists) {
      printf("AC Table ID: %d\n", i);
      for (int j = 0; j < 16; j++) {
        printf("%d: ", j + 1);
        for (int k = jpegInfo.ac_huffman_tables[i].valoffset[j]; k < jpegInfo.ac_huffman_tables[i].valoffset[j + 1];
             k++) {
          printf("%d ", jpegInfo.ac_huffman_tables[i].huffval[k]);
        }
        printf("\n");
      }
      printf("\n");
    }
  }

  printf("\n********** SOS **********\n");
  for (int i = 0; i < jpegInfo.num_color_components; i++) {
    printf("Component ID: %d\n", jpegInfo.color_components[i].component_id);
    printf("DC table ID: %d\n", jpegInfo.color_components[i].dc_huffman_table_id);
    printf("AC table ID: %d\n\n", jpegInfo.color_components[i].ac_huffman_table_id);
  }
  printf("Start of selection: %d\n", jpegInfo.ss);
  printf("End of selection: %d\n", jpegInfo.se);
  printf("Successive approximation high: %d\n", jpegInfo.Ah);
  printf("Successive approximation low: %d\n\n", jpegInfo.Al);

  printf("\n********** BMP **********\n");
  printf("MCU width: %d\n", jpegInfo.mcu_width);
  printf("MCU height: %d\n", jpegInfo.mcu_height);
  printf("BMP padding: %d\n", jpegInfo.padding);
}
#endif

static void init_jpeg_info() {
  jpegInfo.valid = 1;

  for (int i = 0; i < 4; i++) {
    jpegInfo.quant_tables[i].exists = 0;

    if (i < 2) {
      jpegInfo.color_components[i].exists = 0;
      jpegInfo.dc_huffman_tables[i].exists = 0;
      jpegInfo.ac_huffman_tables[i].exists = 0;

    } else if (i < 3) {
      jpegInfo.color_components[i].exists = 0;
    }
  }

  jpegInfo.restart_interval = 0;
  jpegInfo.image_height = 0;
  jpegInfo.image_width = 0;
  jpegInfo.num_color_components = 0;
  jpegInfo.ss = 0;
  jpegInfo.se = 0;
  jpegInfo.Ah = 0;
  jpegInfo.Al = 0;

  jpegInfo.mcu_width = 0;
  jpegInfo.mcu_height = 0;
  jpegInfo.padding = 0;

  for (int i = 0; i < NR_TASKLETS; i++) {
    jpegInfoDpu.mcu_end_index[i] = 0;
    jpegInfoDpu.mcu_start_index[i] = 0;
    if (i < NR_TASKLETS - 1) {
      jpegInfoDpu.dc_offset[i][0] = 0;
      jpegInfoDpu.dc_offset[i][1] = 0;
      jpegInfoDpu.dc_offset[i][2] = 0;
    }
  }
}

int main() {
  JpegDecompressor decompressor;
  decompressor.length = file_length;
  decompressor.tasklet_id = me();
  jpegInfo.length = decompressor.length;

  if (decompressor.tasklet_id == 0) {
    int result = 1;

    init_file_reader_index(&decompressor);
    init_jpeg_info();

    int not_jpeg = check_start_of_image(&decompressor);
    if (not_jpeg) {
      return 1;
    }

    // Continuously read all markers until we reach Huffman coded bitstream
    while (jpegInfo.valid && result) {
      result = read_next_marker(&decompressor);
    }

    if (!jpegInfo.valid) {
      return 1;
    }

    jpegInfo.image_data_start = decompressor.file_index + decompressor.cache_index;
    jpegInfo.size_per_tasklet = (jpegInfo.length - jpegInfo.image_data_start + (NR_TASKLETS - 1)) / NR_TASKLETS;

    output.image_width = jpegInfo.image_width;
    output.image_height = jpegInfo.image_height;
    output.padding = jpegInfo.padding;
    output.mcu_width_real = jpegInfo.mcu_width_real;

#if DEBUG
    print_jpeg_decompressor();
#endif
  }

  // All tasklets should wait until tasklet 0 has finished reading all JPEG markers
  barrier_wait(&init_barrier);

  init_jpeg_decompressor(&decompressor);

  // Process Huffman coded bitstream, perform inverse DCT, and convert YCbCr to RGB
  decode_bitstream(&decompressor);
  if (!jpegInfo.valid) {
    return 1;
  }

  // All tasklets should wait until tasklet 0 has finished adjusting the DC coefficients
  barrier_wait(&idct_barrier);

  inverse_dct_convert(&decompressor);

  // TODO: make scaling work with multiple tasklets
  if (decompressor.tasklet_id == 0) {
    // jpeg_scale();

    // crop(&decompressor);

    output.image_width = jpegInfo.image_width;
    output.image_height = jpegInfo.image_height;
    output.mcu_width_real = jpegInfo.mcu_width_real;
  }
  horizontal_flip(&decompressor);

  return 0;
}
