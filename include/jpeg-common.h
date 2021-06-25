#ifndef _JPEG_CPU_H
#define _JPEG_CPU_H

#include <stdint.h>

#define DCTSIZE 8
#define MAX_SAMP_FACTOR 4
#define MAX(_a, _b) (_a > _b ? _a : _b)

#define MIN_GET_BITS 15

#ifndef NR_TASKLETS
#define NR_TASKLETS 16
#endif

/**
 * JPEG Markers: CCITT Rec T.81 page 32
 */
enum markers {
  /* Start Of Frame non-differential, Huffman coding */
  M_SOF0 = 0xC0,
  M_SOF1,
  M_SOF2,
  M_SOF3,

  M_DHT = 0xC4, /* Define Huffman Tables */

  /* Start Of Frame differential, Huffman coding */
  M_SOF5 = 0xC5,
  M_SOF6,
  M_SOF7,

  M_JPG = 0xC8, /* JPEG extensions (unused) */

  /* Start Of Frame non-differential, arithmetic coding */
  M_SOF9,
  M_SOF10,
  M_SOF11,

  M_DAC = 0xCC, /* Define Arithmetic Coding */

  /* Start Of Frame differential, arithmetic coding */
  M_SOF13, /* Differential sequential DCT */
  M_SOF14, /* Differential progressive DCT */
  M_SOF15, /* Differential lossless */

  M_RST_FIRST = 0xD0, /* Restart interval termination */
  M_RST_LAST = 0xD7,  /* Restart interval termination */

  M_SOI = 0xD8, /* Start Of Image (beginning of datastream) */
  M_EOI,        /* End Of Image (end of datastream) */
  M_SOS,        /* Start Of Scan (begins compressed data) */
  M_DQT,        /* Define Quantization Table */
  M_DNL,        /* Define Number of Lines */
  M_DRI,        /* Define Restart Interval */
  M_DHP,        /* Define Hierarchical Progression */
  M_EXP,        /* Expand Reference Component(s) */

  M_APP_FIRST = 0xE0, /* Application-specific marker, type N */
  M_APP_LAST = 0xEF,

  M_EXT_FIRST = 0xF0, /* JPEG extension markers, 0-13 */
  M_EXT_LAST = 0xFD,

  M_COM /* COMment */
};

/**
 * Struct to store Quantization Table information
 */
typedef struct QuantizationTable {
  uint8_t exists;

  uint32_t table[64];
} QuantizationTable;

/**
 * Struct to store Huffman Table information
 */
typedef struct HuffmanTable {
  uint8_t exists;

  uint8_t huffval[256];  // HUFFVAL: actually sum(length[0] .. length[15])
  uint8_t valoffset[18]; // offset into huffval for codes of length k
  uint32_t codes[256];
} HuffmanTable;

/**
 * Struct to store Color Component information
 */
typedef struct ColorComponentInfo {
  uint8_t exists;

  // from SOF
  uint8_t component_id;
  uint8_t h_samp_factor;
  uint8_t v_samp_factor;
  uint8_t quant_table_id;

  // from SOS
  uint8_t dc_huffman_table_id;
  uint8_t ac_huffman_table_id;
} ColorComponentInfo;

typedef struct JpegDecompressor {
  char *ptr;       // current position within JPEG
  char *data;      // start position of JPEG
  uint32_t length; // total length of JPEG

  uint32_t tasklet_id;
  int file_index;
  uint32_t cache_index;

  uint8_t valid; // indicates whether file is actually a JPEG file

  // from DQT
  QuantizationTable quant_tables[4];

  // from DRI
  uint16_t restart_interval;

  // from SOF
  uint16_t image_height;
  uint16_t image_width;
  uint8_t num_color_components;
  ColorComponentInfo color_components[3];

  // from DHT
  HuffmanTable dc_huffman_tables[2];
  HuffmanTable ac_huffman_tables[2];

  // from SOS
  uint8_t ss; // Start of spectral selection
  uint8_t se; // End of spectral selection
  uint8_t Ah; // Successive approximation high
  uint8_t Al; // Successive approximation low

  // for decoding and writing to BMP
  uint32_t mcu_height;
  uint32_t mcu_width;
  uint32_t padding;

  // used when horizontal or vertical sampling factors are not 1
  uint32_t mcu_height_real;   // mcu_height + padding, padding must be 0 or 1
  uint32_t mcu_width_real;    // mcu_width + padding, padding must be 0 or 1
  uint32_t max_h_samp_factor; // maximum value of horizontal sampling factors amongst all color components
  uint32_t max_v_samp_factor; // maximum value of vertical sampling factors amongst all color components

  // uint16_t restarts_left;
  // uint32_t num_restart_intervals;

  /* updated each scan */
  // uint8_t first_scan;
  // uint8_t component_sel;
  // uint8_t coding_table;
  // uint8_t MCUs_per_row;
  // uint32_t rows_per_scan;
  // uint8_t blocks_per_MCU;

  // bit buffer
  uint32_t get_buffer;
  uint32_t bits_left;
} JpegDecompressor;

typedef struct JpegInfo {
  uint8_t valid;             // indicates whether file is actually a JPEG file
  uint32_t image_data_start; // index where image data begins in SOS
  uint32_t length;           // total length of JPEG
  uint32_t size_per_tasklet; // number of bytes to decode per tasklet

  // from DQT
  QuantizationTable quant_tables[4];

  // from DRI
  uint16_t restart_interval;

  // from SOF
  uint16_t image_height;
  uint16_t image_width;
  uint8_t num_color_components;
  ColorComponentInfo color_components[3];

  // from DHT
  HuffmanTable dc_huffman_tables[2];
  HuffmanTable ac_huffman_tables[2];

  // from SOS
  uint8_t ss; // Start of spectral selection
  uint8_t se; // End of spectral selection
  uint8_t Ah; // Successive approximation high
  uint8_t Al; // Successive approximation low

  // for decoding and writing to BMP
  uint32_t mcu_height;
  uint32_t mcu_width;
  uint32_t padding;

  // used when horizontal or vertical sampling factors are not 1
  uint32_t mcu_height_real;   // mcu_height + padding, padding must be 0 or 1
  uint32_t mcu_width_real;    // mcu_width + padding, padding must be 0 or 1
  uint32_t max_h_samp_factor; // maximum value of horizontal sampling factors amongst all color components
  uint32_t max_v_samp_factor; // maximum value of vertical sampling factors amongst all color components

  uint32_t mcu_end_index[NR_TASKLETS];   // end index of each tasklet in the 2D MRAM MCU buffer
  uint32_t mcu_start_index[NR_TASKLETS]; // start index of each tasklet in the 2D MRAM MCU buffer
  int dc_offset[NR_TASKLETS - 1][3];     // offset to the 3 DC coefficients from tasklet i to tasklet i + 1
  uint32_t rows_per_mcu;
  uint32_t overall_end_index; // end index after combining all decoded MCUs
  uint32_t mcus_per_tasklet;  // number of MCUs to do idct and ycbcr to rgb for each tasklet
} JpegInfo;

/**
 * MCU has a 2D array. 3 indices for color channels (RGB) and 64 indices the 64 values in an 8x8 MCU
 */
typedef struct MCU {
  short buffer[3][64];
} MCU;

void jpeg_cpu_scale(uint64_t file_length, char *filename, char *buffer);

#endif // _JPEG_CPU_H
