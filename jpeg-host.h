#ifndef _JPEG_HOST__H
#define _JPEG_HOST__H

#include "PIM-common/common/include/common.h"

#ifndef MAX_FILES_PER_DPU
#define MAX_FILES_PER_DPU 64
#endif

#ifndef NR_TASKLETS
#define NR_TASKLETS 16
#endif

enum {
	PROG_OK = 0,
	PROG_INVALID_INPUT,
	PROG_BUFFER_TOO_SMALL,
	PROG_OUTPUT_ERROR,
	PROG_FAULT
};

enum option_flags
{
	OPTION_FLAG_COUNT_MATCHES,
	OPTION_FLAG_OUT_BYTE,
	OPTION_FLAG_MULTIPLE_FILES, // multiple files per DPU
};

struct jpeg_options
{
	uint32_t scale;		/* percent scaling of the image */
	uint32_t flags;		/* most other single-bit options (see OPTION_FLAG_) */
	uint32_t max_files;	/* stop processing after this many files */
	uint32_t max_ranks;	/* use this number of ranks, even if we have more */
	uint32_t input_file_count;
}__attribute__((aligned(8)));

typedef struct file_stats
{
	uint32_t line_count; // total lines in the file
	uint32_t match_count; // total matches of the search term
} file_stats;

typedef struct file_descriptor
{
	uint32_t start; 	// offset into host_dpu_descriptor.buffer
	uint32_t length;
} file_descriptor;

typedef struct host_dpu_descriptor
{
	uint32_t file_count; // how many files are processed by this DPU
	uint32_t total_length; // size of the concatenated buffer
	uint32_t perf; // value from the DPU's performance counter
	char *buffer; // concatenated buffer for this DPU
	char *filename[MAX_FILES_PER_DPU];
	file_descriptor files[MAX_FILES_PER_DPU];
	file_stats stats[MAX_FILES_PER_DPU];
} host_dpu_descriptor;

typedef struct host_rank_context
{
	uint32_t dpu_count; // how many dpus are filled in the descriptor array
	host_dpu_descriptor *dpus; // the descriptors for the dpus in this rank
#ifdef STATISTICS
	struct timespec start_rank;
#endif // STATISTICS
} host_rank_context;

typedef struct host_results
{
	uint32_t total_line_count;
	uint32_t total_match_count;
	uint32_t total_files;
	uint64_t total_instructions;
} host_results;

#endif	/* _JPEG_HOST__H */

