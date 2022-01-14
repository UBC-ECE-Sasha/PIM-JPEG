#ifndef _JPEG_HOST__H
#define _JPEG_HOST__H

#include "common.h"

#ifndef MAX_FILES_PER_DPU
#define MAX_FILES_PER_DPU 64
#endif

#ifndef NR_TASKLETS
#define NR_TASKLETS 16
#endif

#define MAX_INPUT_LENGTH MEGABYTE(16)

enum { PROG_OK = 0, PROG_INVALID_INPUT, PROG_BUFFER_TOO_SMALL, PROG_OUTPUT_ERROR, PROG_FAULT };

enum option_flags {
  OPTION_FLAG_COUNT_MATCHES,
  OPTION_FLAG_OUT_BYTE,
  OPTION_FLAG_MULTIPLE_FILES, // multiple files per DPU
};

struct jpeg_options {
  uint32_t scale;     /* percent scaling of the image */
  uint32_t flags;     /* most other single-bit options (see OPTION_FLAG_) */
  uint32_t max_files; /* stop processing after this many files */
  uint32_t max_ranks; /* use this number of ranks, even if we have more */
  uint32_t input_file_count;

  uint32_t scale_width;
  uint32_t scale_height;
  uint32_t horizontal_flip;
} __attribute__((aligned(8)));

typedef struct file_stats {
  uint32_t line_count;  // total lines in the file
  uint32_t match_count; // total matches of the search term
} file_stats;

typedef struct file_descriptor {
  uint32_t start; // offset into host_dpu_descriptor.buffer
  uint32_t length;
} file_descriptor;

typedef struct host_dpu_descriptor {
  uint32_t perf; // value from the DPU's performance counter
  char *buffer;  // concatenated buffer for this DPU
  char *filename[MAX_FILES_PER_DPU];
  file_descriptor files[MAX_FILES_PER_DPU];
  file_stats stats[MAX_FILES_PER_DPU];
} host_dpu_descriptor;

typedef struct host_rank_context {
  uint32_t dpu_count;        // how many dpus are filled in the descriptor array
  host_dpu_descriptor *dpus; // the descriptors for the dpus in this rank
#ifdef STATISTICS
  struct timespec start_rank;
#endif // STATISTICS
} host_rank_context;

typedef struct dpu_settings_t {
  char *buffer;
  uint64_t file_length;
  char *filename;
  uint32_t scale_width;
  uint32_t horizontal_flip;
} dpu_settings_t;

typedef struct dpu_inputs_t {
  uint64_t file_length;
  uint32_t scale_width;
  uint32_t horizontal_flip;
} dpu_inputs_t;

typedef struct dpu_output_t {
  uint16_t image_width;
  uint16_t image_height;
  uint32_t padding;
  uint32_t mcu_width_real;
  uint32_t sum_rgb[3];
} dpu_output_t;

#endif /* _JPEG_HOST__H */
