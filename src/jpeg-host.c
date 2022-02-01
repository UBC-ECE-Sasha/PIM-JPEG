#define _DEFAULT_SOURCE // needed for S_ISREG() and strdup
#include <dpu.h>
#include <dpu_log.h>
#include <dpu_management.h>
#include <dpu_memory.h>
#include <dpu_runner.h>
#include <unistd.h>

#include <getopt.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <time.h>

#include "bmp.h"
#include "host.h"
#include "jpeg-common.h"
#include "jpeg-host.h"

#define DPU_PROGRAM "src/dpu/jpeg-dpu"
#define TEMP_LENGTH 256
#define ALL_RANKS (rank_count == 64 ? 0xFFFFFFFFFFFFFFFF : (1UL << rank_count) - 1)

// to extract components from dpu_id_t
#define DPU_ID_RANK(_x) ((_x >> 16) & 0xFF)
#define DPU_ID_SLICE(_x) ((_x >> 8) & 0xFF)
#define DPU_ID_DPU(_x) ((_x) &0xFF)

#define TIME_NOW(_t) (clock_gettime(CLOCK_MONOTONIC, (_t)))

const char options[] = "cdm:r:s:Mw:f";
static uint32_t rank_count, dpu_count;
static uint32_t dpus_per_rank;
static char **input_files = NULL;

#ifdef STATISTICS
static uint64_t total_data_processed;
static uint64_t total_dpus_launched;
#endif // STATISTICS

#ifdef DEBUG
static char *to_bin(uint64_t i, uint8_t length) {
  uint8_t outchar;
  static char outstr[65];

  for (outchar = 0; outchar < length; outchar++) {
    outstr[outchar] = (i & (1UL << outchar)) ? 'X' : '-';
  }
  outstr[outchar] = 0;
  return outstr;
}
#endif // DEBUG

void scale_rank(struct dpu_set_t dpu_rank, host_rank_context *desc, struct jpeg_options *opts)
{
	struct dpu_set_t dpu;
	uint32_t dpu_id = 0; // the id of the DPU inside the rank (0-63)
	dpu_inputs_t dpu_inputs[64];
	struct host_dpu_descriptor *input = desc->dpus;

	dbg_printf("Using %u DPUs\n", desc->dpu_count);

	// copy the input metadata to the DPUs
	DPU_FOREACH(dpu_rank, dpu, dpu_id)
	{
		dpu_inputs[dpu_id].file_length = input[dpu_id].in_length;
		dpu_inputs[dpu_id].scale_width = opts->scale_width;
		dpu_inputs[dpu_id].horizontal_flip = opts->horizontal_flip;

		DPU_ASSERT(dpu_prepare_xfer(dpu, (void *) &dpu_inputs[dpu_id]));
	}
	DPU_ASSERT(dpu_push_xfer(dpu_rank, DPU_XFER_TO_DPU, "input", 0, ALIGN(sizeof(dpu_inputs_t), 8), DPU_XFER_DEFAULT));

	// copy the compressed files to the DPUs
	uint32_t longest_length = 0;
	DPU_FOREACH(dpu_rank, dpu, dpu_id)
	{
		DPU_ASSERT(dpu_prepare_xfer(dpu, (void *) input[dpu_id].in_buffer));

		if (input[dpu_id].in_length > longest_length)
			longest_length = input[dpu_id].in_length;
	}
	DPU_ASSERT(dpu_push_xfer(dpu_rank, DPU_XFER_TO_DPU, "file_buffer", 0, ALIGN(longest_length, 8), DPU_XFER_DEFAULT));

	// launch the rank as soon as the data is copied
	DPU_ASSERT(dpu_launch(dpu_rank, DPU_ASYNCHRONOUS));
}

int read_results_dpu_rank(struct dpu_set_t dpu_rank, struct host_rank_context *rank_ctx)
{
	struct dpu_set_t dpu;
	uint8_t dpu_id;

#ifdef STATISTICS
	struct timespec stop;
	clock_gettime(CLOCK_MONOTONIC, &stop);
	double rank_time = TIME_DIFFERENCE(rank_ctx->start_rank, stop);
	printf("Rank processed in %2.2f s\n", rank_time);
#endif // STATISTICS

	// get image metadata
	DPU_FOREACH(dpu_rank, dpu, dpu_id)
	{
		DPU_ASSERT(dpu_prepare_xfer(dpu, (void *) &rank_ctx->dpus[dpu_id].img));
	}
	DPU_ASSERT(dpu_push_xfer(dpu_rank, DPU_XFER_FROM_DPU, "output", 0, sizeof(dpu_output_t), DPU_XFER_DEFAULT));

	// get image data
	uint64_t largest_size = 0;
	DPU_FOREACH(dpu_rank, dpu, dpu_id)
	{
		uint32_t buf_size = MEGABYTE(16);
		dbg_printf("Out buffer size: %u\n", buf_size);
		rank_ctx->dpus[dpu_id].out_buffer = malloc(buf_size);
		DPU_ASSERT(dpu_prepare_xfer(dpu, (void *) rank_ctx->dpus[dpu_id].out_buffer));
		if (buf_size > largest_size)
			largest_size = buf_size;
	}
	DPU_ASSERT(dpu_push_xfer(dpu_rank, DPU_XFER_FROM_DPU, "MCU_buffer", 0, largest_size, DPU_XFER_DEFAULT));

	DPU_FOREACH(dpu_rank, dpu, dpu_id)
	{
		if (dpu_id >= rank_ctx->dpu_count)
			break;

#ifdef DEBUG_DPU
		// get any DPU debug messages
		dpu_error_t err = dpu_log_read(dpu, stdout);
		if (err != DPU_OK)
		{
			dbg_printf("Error %u retrieving log\n", err);
			return -1;
		}
#endif // DEBUG_DPU
	}

	return 0;
}

int check_for_completed_rank(struct dpu_set_t dpus, uint64_t* rank_status, struct host_rank_context ctx[], host_results *results)
{
	struct dpu_set_t dpu_rank, dpu;
	uint8_t rank_id=0;

	DPU_RANK_FOREACH(dpus, dpu_rank)
	{
		bool done, fault;

		if (*rank_status & ((uint64_t)1<<rank_id))
		{
			uint32_t dpu_id;
			struct host_rank_context* rank_ctx = &ctx[rank_id];

			// check to see if anything has completed
			dpu_status(dpu_rank, &done, &fault);
			if (fault)
			{
				bool dpu_done, dpu_fault;
				printf("rank %u fault - abort!\n", rank_id);

				// try to find which DPU caused the fault
				DPU_FOREACH(dpu_rank, dpu)
				{
					dpu_status(dpu, &dpu_done, &dpu_fault);
					if (dpu_fault)
					{
						dpu_id_t id = dpu_get_id(dpu.dpu);
						fprintf(stderr, "[%u:%u:%u] at fault\n", DPU_ID_RANK(id), DPU_ID_SLICE(id), DPU_ID_DPU(id));
#ifdef DEBUG_DPU
						fprintf(stderr, "Halting for debug");
						while (1)
							usleep(100000);
#endif // DEBUG_DPU
					}
				}

				// free the associated memory
				for (dpu_id=0; dpu_id < rank_ctx->dpu_count; dpu_id++)
				{
					// free the memory of the descriptor
					for (uint32_t file=0; file < rank_ctx->dpus[dpu_id].file_count; file++)
						free(rank_ctx->dpus[dpu_id].filename[file]);

					// free the output buffer
					free(rank_ctx->dpus[dpu_id].out_buffer);
				}
				free(rank_ctx->dpus);

				return -2;
			}

			if (done)
			{
				*rank_status &= ~((uint64_t)1<<rank_id);
				dbg_printf("Reading results from rank %u status %s\n", rank_id, to_bin(*rank_status, rank_count));
				read_results_dpu_rank(dpu_rank, rank_ctx);

				// aggregate statistics
				for (dpu_id=0; dpu_id < rank_ctx->dpu_count; dpu_id++)
				{
					host_dpu_descriptor *desc = &rank_ctx->dpus[dpu_id];
					results->total_files += desc->file_count;
					results->total_instructions += desc->perf;

					for (uint32_t file=0; file < rank_ctx->dpus[dpu_id].file_count; file++)
					{
						// do something with the output
						dpu_output_t *img = &desc->img[file];
						write_bmp_dpu(desc->filename[file], img->width, img->height, img->padding, img->mcu_width_real, desc->out_buffer);

						// free the memory of the descriptor
						free(rank_ctx->dpus[dpu_id].filename[file]);
					}

					// free the output buffer
					free(rank_ctx->dpus[dpu_id].out_buffer);
				}
				free(rank_ctx->dpus);
			}
		}
		rank_id++;
	}
	return 0;
}

/**
 * Read the contents of a file into an in-memory buffer. Upon success,
 * return 0 and write the amount read to input->length.
 *
 * @param in_file The input filename.
 * @param input The struct to which contents of file are written to.
 */
static int read_input_host(char *in_file, uint64_t length, char *buffer)
{
	FILE *fin = fopen(in_file, "r");
	if (fin == NULL)
	{
		fprintf(stderr, "Invalid input file: %s\n", in_file);
		return -2;
	}

	if (length == 0)
	{
		fprintf(stderr, "Skipping %s: size is too small (%ld)\n",
				in_file, length);
		return -2;
	}

	size_t n = fread(buffer, 1, length, fin);
	fclose(fin);

	return n;
}

static int dpu_main(struct jpeg_options *opts) {
	char dpu_program_name[32];
	struct dpu_set_t dpus, dpu_rank;
	int status;
	uint8_t rank_id;
	uint64_t rank_status = 0; // bitmap indicating if the rank is busy or free
	uint32_t submitted;
	struct host_rank_context *ctx;
	host_results results;

#ifdef STATISTICS
	struct timespec start_load, stop_load;
#endif // STATISTICS

	memset(&results, 0, sizeof(host_results));

	// allocate all of the DPUS up-front, then check to see how many we got
	status = dpu_alloc(DPU_ALLOCATE_ALL, NULL, &dpus);
	if (status != DPU_OK) {
		fprintf(stderr, "Error %i allocating DPUs\n", status);
		return -3;
	}

	dpu_get_nr_ranks(dpus, &rank_count);
	dpu_get_nr_dpus(dpus, &dpu_count);
	dpus_per_rank = dpu_count / rank_count;
	printf("Got %u dpus across %u ranks (%u dpus per rank)\n", dpu_count, rank_count, dpus_per_rank);

	// artificially limit the number of ranks based on user request
	if (rank_count > opts->max_ranks) {
		rank_count = opts->max_ranks;
	}

	if (rank_count > 64) {
		printf("Error: too many ranks for a 64-bit bitmask!\n");
		return -4;
	}
	if (opts->input_file_count < dpu_count) {
		printf("Warning: fewer input files than DPUs (%u < %u)\n", opts->input_file_count, dpu_count);
	}

	snprintf(dpu_program_name, 31, "%s-%u", DPU_PROGRAM, NR_TASKLETS);
	DPU_ASSERT(dpu_load(dpus, dpu_program_name, NULL));

	// allocate space for DPU descriptors for all ranks
	ctx = calloc(rank_count, sizeof(host_rank_context));

	// Read each input file into main memory
	uint32_t file_index=0;
	uint32_t remaining_file_count = opts->input_file_count;
	dbg_printf("Input file count=%u\n", opts->input_file_count);

	// as long as there are still files to process
	while (remaining_file_count)
	{
		struct host_dpu_descriptor *rank_input;
		uint8_t dpu_id=0;
		uint32_t prepared_file_count;
		uint8_t prepared_dpu_count=0;

#ifdef STATISTICS
		clock_gettime(CLOCK_MONOTONIC, &start_load);
#endif // STATISTICS

		// allocate a new set of descriptors to save the context of work to be
		// done by a rank. We don't know exactly which rank hardware will be
		// used to do the work yet, but we will find one later, once the work
		// is ready.
		rank_input = calloc(dpus_per_rank, sizeof(struct host_dpu_descriptor));

		// prepare empty work descriptor for each DPU
		for (dpu_id=0; dpu_id < dpus_per_rank; dpu_id++)
		{
			rank_input[dpu_id].file_count = 0;
			rank_input[dpu_id].in_length = 0;
			rank_input[dpu_id].in_buffer = malloc(MAX_INPUT_LENGTH);
		}

		// fill descriptors by preparing files until the rank is full, or we run out
		for (prepared_file_count = 0;
			remaining_file_count;
			remaining_file_count--, file_index++)
		{
			uint8_t dpus_searched;
			struct stat st;
			char *filename = input_files[file_index];

			// read the length of the next input file
			stat(filename, &st);
			uint64_t file_length = st.st_size;
			if (file_length > MAX_INPUT_LENGTH)
			{
				dbg_printf("Skipping file %s (%lu > %u)\n", input_files[file_index], file_length, MAX_INPUT_LENGTH);
				continue;
			}

			// find a free slot among the DPUs
			// 'free' means number of tasklets and free memory
			char *next;
			for (dpus_searched=0; dpus_searched < dpus_per_rank; dpus_searched++)
			{
				dpu_id++;
				dpu_id%=dpus_per_rank;
				if (rank_input[dpu_id].file_count < MAX_FILES_PER_DPU &&
					(rank_input[dpu_id].in_length + file_length < MAX_INPUT_LENGTH))
				{
					dbg_printf("Allocating %s to DPU %u file count=%u, length=%lu, total length=%lu\n",
						filename, dpu_id, rank_input[dpu_id].file_count,
						file_length, rank_input[dpu_id].in_length + file_length);
					file_descriptor *input = &rank_input[dpu_id].files[rank_input[dpu_id].file_count];

					// prepare the input buffer descriptor
					memset(input, 0, sizeof(file_descriptor));
					input->start = rank_input[dpu_id].in_length;

					// read the file into the descriptor
					next = rank_input[dpu_id].in_buffer + rank_input[dpu_id].in_length;
					rank_input[dpu_id].filename[rank_input[dpu_id].file_count] = strdup(filename);
					input->length = file_length;
					if (read_input_host(filename, file_length, next) < 0)
					{
						dbg_printf("Skipping invalid file %s\n", input_files[file_index]);
						break;
					}

					// if this is the first file for this DPU, mark the DPU as used
					if (rank_input[dpu_id].file_count == 0)
					{
						prepared_dpu_count++;
#ifdef STATISTICS
						total_dpus_launched++;
#endif // STATISTICS
					}

					rank_input[dpu_id].file_count++;
					rank_input[dpu_id].in_length += file_length;// if we need alignment, do it here
 					prepared_file_count++;
#ifdef STATISTICS
					total_data_processed += file_length;
#endif // STATISTICS
					break;
				}
			}

			// did we look at all possible DPUs and not find an empty place?
			if (dpus_searched == dpus_per_rank)
				break;
		}

#ifdef STATISTICS
		clock_gettime(CLOCK_MONOTONIC, &stop_load);
		double load_time = TIME_DIFFERENCE(start_load, stop_load);
		printf("[%u] %u loaded in %2.2f s\n", dpu_id, rank_input[dpu_id].in_length, load_time);
#endif // STATISTICS

		dbg_printf("Prepared %u files in %u DPUs\n", prepared_file_count, prepared_dpu_count);
		submitted = 0;
		while (!submitted)
		{
			while (rank_status == ALL_RANKS)
			{
				int ret = check_for_completed_rank(dpus, &rank_status, ctx, &results);
				if (ret == -2)
				{
					printf("A rank has faulted\n");
					status = -100;
					goto done;
				}
			}

			// submit those files to a free rank, and save the files in the host context
			DPU_RANK_FOREACH(dpus, dpu_rank, rank_id)
			{
				if (!(rank_status & (1UL<<rank_id)))
				{
					rank_status |= (1UL<<rank_id);
					dbg_printf("Submitted to rank %u status=%s\n", rank_id, to_bin(rank_status, rank_count));
#ifdef STATISTICS
					clock_gettime(CLOCK_MONOTONIC, &ctx[rank_id].start_rank);
#endif // STATISTICS
					ctx[rank_id].dpus = rank_input;
					ctx[rank_id].dpu_count = prepared_dpu_count;
					scale_rank(dpu_rank, &ctx[rank_id], opts);
					submitted = 1;
					break;
				}
			}

			if (!submitted)
				printf("ERROR: failed to submit\n");
		}
	}

	dbg_printf("Freeing input files\n");
	free(input_files);
	input_files = NULL;

	// all files have been submitted; wait for all jobs to finish
	dbg_printf("Waiting for all DPUs to finish\n");
	while (rank_status)
	{
		int ret = check_for_completed_rank(dpus, &rank_status, ctx, &results);
		if (ret == -2)
		{
			status = -100;
			goto done;
		}
		usleep(1);
	}

done:
  dpu_free(dpus);

  return status;
}

static int cpu_main(struct jpeg_options *opts) {
  struct timespec start, end;
  char *buffer = malloc(MAX_INPUT_LENGTH);
  uint32_t file_index = 0;

  dbg_printf("Input file count=%u\n", opts->input_file_count);

  // as long as there are still files to process
  for (; file_index < opts->input_file_count; file_index++) {
    struct stat st;
    char *filename = input_files[file_index];

    TIME_NOW(&start);
    // read the length of the next input file
    stat(filename, &st);
    uint64_t file_length = st.st_size;
    if (file_length > MAX_INPUT_LENGTH) {
      dbg_printf("Skipping file %s (%lu > %u)\n", filename, file_length, MAX_INPUT_LENGTH);
      continue;
    }

    // read the file into the descriptor
	if (read_input_host(filename, file_length, buffer) < 0) {
      dbg_printf("Skipping invalid file %s\n", input_files[file_index]);
      break;
    }

#ifdef STATISTICS
    total_data_processed += file_length;
#endif // STATISTICS

    jpeg_cpu_scale(file_length, filename, buffer);
    TIME_NOW(&end);
    float run_time = TIME_DIFFERENCE(start, end);

    printf("Total runtime: %fs\n\n", run_time);
  }

  free(buffer);
  return 0;
}

static void usage(const char *exe_name) {
#ifdef DEBUG
  fprintf(stderr, "**DEBUG BUILD**\n");
#endif // DEBUG
  fprintf(stderr, "Scale a JPEG without decompression\nCan use either the host CPU or UPMEM DPU\n");
  fprintf(stderr, "usage: %s [-d] -s <scale percent> <filenames>\n", exe_name);
  fprintf(stderr, "d: use DPU\n");
  fprintf(stderr, "m: maximum number of files to process\n");
  fprintf(stderr, "r: maximum number of ranks to use\n");
  fprintf(stderr, "t: term to search for\n");
}

/**
 * Main function
 */
int main(int argc, char **argv) {
  int opt;
  int use_dpu = 0;
  int status;
  uint32_t allocated_count = 0;
  struct jpeg_options opts;

#ifdef STATISTICS
  double total_time;
  struct timespec start, stop;

  if (clock_gettime(CLOCK_MONOTONIC, &start) < 0) {
    printf("Error getting time\n");
    return -10;
  }
#endif // STATISTICS

  memset(&opts, 0, sizeof(struct jpeg_options));
  opts.max_files = -1; // no effective maximum by default
  opts.max_ranks = -1; // no effective maximum by default
  opts.scale_width = 256;
  opts.scale_height = 256;
  opts.horizontal_flip = 0; // no horizontal flip by default

  while ((opt = getopt(argc, argv, options)) != -1) {
    switch (opt) {
      case 'd':
        use_dpu = 1;
        break;

      case 'm':
        opts.max_files = strtoul(optarg, NULL, 0);
        break;

      case 'r':
        opts.max_ranks = strtoul(optarg, NULL, 0);
        break;

      case 's':
        opts.scale = strtoul(optarg, NULL, 0);
        break;

      case 'M':
        opts.flags |= (1 << OPTION_FLAG_MULTIPLE_FILES);
        dbg_printf("Allocating multiple files per DPU\n");
        break;

      case 'w':
        opts.scale_width = strtoul(optarg, NULL, 0);
        break;

      case 'f':
        opts.horizontal_flip = 1;
        break;

      case 'C':
      case 'D':
      case 'E':
      case 'F':
      case 'G':
      case 'H':
      case 'I':
      case 'T':
      case 'U':
        printf("%s is not supported\n", optarg);
        return -99;
        break;

      default:
        printf("Unrecognized option\n");
        usage(argv[0]);
        return -2;
    }
  }

  // at this point, all the rest of the arguments are files to search through
  int remain_arg_count = argc - optind;
  if (remain_arg_count && strcmp(argv[optind], "-") == 0) {
    char buff[TEMP_LENGTH];
    int bytes_remaining;
    allocated_count = 1;
    input_files = malloc(sizeof(char *) * allocated_count);
    bytes_remaining = fread(buff, 1, TEMP_LENGTH, stdin);
    while (bytes_remaining > 0) {
      int consumed;
      struct stat st;
      // see if we need more space for file names
      if (opts.input_file_count == allocated_count) {
        allocated_count <<= 1;
        input_files = realloc(input_files, sizeof(char *) * allocated_count);
      }
      strtok(buff, "\r\n\t");
      if (stat(buff, &st) == 0 && S_ISREG(st.st_mode))
        input_files[opts.input_file_count++] = strdup(buff);

      consumed = strlen(buff) + 1;
      bytes_remaining -= consumed;
      if (consumed == 1)
        break;

      // scootch the remaining bytes forward and read some more
      memmove(buff, buff + consumed, TEMP_LENGTH - consumed);
      int bytes_read = fread(buff + TEMP_LENGTH - consumed, 1, consumed, stdin);
      if (bytes_read > 0)
        bytes_remaining += bytes_read;
    }
  } else {
    input_files = malloc(sizeof(char *) * remain_arg_count);
    for (int i = 0; i < remain_arg_count; i++) {
      struct stat s;
      char *next = argv[i + optind];
      if (stat(next, &s) == 0 && S_ISREG(s.st_mode))
        input_files[opts.input_file_count++] = argv[i + optind];
    }
  }

  // if there are no input files, we have no work to do!
  if (opts.input_file_count == 0) {
    printf("No input files!\n");
    usage(argv[0]);
    return -1;
  }

  if (opts.input_file_count > opts.max_files) {
    opts.input_file_count = opts.max_files;
    dbg_printf("Limiting input files to %u\n", opts.input_file_count);
  }

  if (use_dpu)
    status = dpu_main(&opts);
  else
    status = cpu_main(&opts);

  if (status != PROG_OK) {
    fprintf(stderr, "encountered error %u\n", status);
    exit(EXIT_FAILURE);
  }

#ifdef STATISTICS
  clock_gettime(CLOCK_MONOTONIC, &stop);
  total_time = TIME_DIFFERENCE(start, stop);
  printf("Sequential reader size: %u\n", SEQREAD_CACHE_SIZE);
  printf("Number of DPUs: %u\n", dpu_count);
  printf("Number of ranks: %u\n", rank_count);
  printf("Total data processed: %lu\n", total_data_processed);
  printf("Total time: %0.2fs\n", total_time);
  printf("Total DPUs launched: %lu\n", total_dpus_launched);
#endif // STATISTICS

  dbg_printf("Freeing input files\n");
  free(input_files);
  input_files = NULL;

  return 0;
}
