#define _DEFAULT_SOURCE // needed for S_ISREG() and strdup
#include <dpu.h>
#include <dpu_memory.h>
#include <dpu_log.h>
#include <dpu_management.h>
#include <dpu_runner.h>
#include <unistd.h>

#include <string.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <getopt.h>
#include <time.h>

#include <host.h>
#include "jpeg-host.h"
#include "jpeg-cpu.h"

#define DPU_PROGRAM "dpu-jpeg/jpeg.dpu"
#define TOTAL_MRAM MEGABYTE(32)
#define MAX_OUTPUT_LENGTH MEGABYTE(1)
#define MAX_INPUT_LENGTH (TOTAL_MRAM - MAX_OUTPUT_LENGTH)
#define MIN_CHUNK_SIZE 256 // not worthwhile making another tasklet work for data less than this
#define TEMP_LENGTH 256
#define ALL_RANKS (rank_count == 64 ? 0xFFFFFFFFFFFFFFFF : (1UL<<rank_count) - 1)

// to extract components from dpu_id_t
#define DPU_ID_RANK(_x) ((_x >> 16) & 0xFF)
#define DPU_ID_SLICE(_x) ((_x >> 8) & 0xFF)
#define DPU_ID_DPU(_x) ((_x) & 0xFF)

const char options[]="cdm:r:s:M";
static uint32_t rank_count, dpu_count;
static uint32_t dpus_per_rank;
static char **input_files = NULL;
static char dummy_buffer[MAX_INPUT_LENGTH];

#ifdef STATISTICS
static uint64_t total_data_processed;
static uint64_t total_dpus_launched;
#endif // STATISTICS

#ifdef DEBUG
static char* to_bin(uint64_t i, uint8_t length)
{
	uint8_t outchar;
	static char outstr[65];

	for (outchar=0; outchar < length; outchar++)
	{
		outstr[outchar] = (i & (1UL<<outchar)) ? 'X':'-';
	}
	outstr[outchar] = 0;
	return outstr;
}
#endif // DEBUG

int scale_rank(struct dpu_set_t dpu_rank, uint8_t rank_id, struct host_dpu_descriptor input[], 
	uint32_t file_count, uint32_t used_dpus, struct jpeg_options* opts)
{
	struct dpu_set_t dpu;
	dpu_error_t err;
	uint32_t dpu_id=0; // the id of the DPU inside the rank (0-63)
	uint32_t chunk_size;
	UNUSED(rank_id);

	err = dpu_copy_to(dpu_rank, "options", 0, opts, ALIGN(sizeof(struct jpeg_options), 8));
	if (err != DPU_OK)
	{
		dbg_printf("Error %u copying options\n", err);
		return -1;
	}

	// copy the items specific to each DPU
	DPU_FOREACH(dpu_rank, dpu, dpu_id)
	{
		// copy the data to the DPU
#ifdef DEBUG_DPU
		err = dpu_copy_to(dpu, "dpu_id", 0, &dpu_id, sizeof(uint32_t));
		if (err != DPU_OK)
		{
			dbg_printf("Error %u copying dpu_id\n", err);
			return -1;
		}
#endif // DEBUG_DPU
		err = dpu_copy_to(dpu, "total_length", 0, &input[dpu_id].total_length, sizeof(uint32_t));
		if (err != DPU_OK)
		{
			dbg_printf("Error %u copying total_length\n", err);
			return -1;
		}

		// calculate how much work each tasklet has to do
		chunk_size = ALIGN(input[dpu_id].total_length, NR_TASKLETS) / NR_TASKLETS;

		err = dpu_copy_to(dpu, "chunk_size", 0, &chunk_size, sizeof(uint32_t));
		if (err != DPU_OK)
		{
			dbg_printf("Error %u copying chunk size\n", err);
			return -1;
		}
		err = dpu_copy_to(dpu, "file_count", 0, &file_count, sizeof(uint32_t));
		if (err != DPU_OK)
		{
			dbg_printf("Error %u copying file count\n", err);
			return -1;
		}

#ifndef BULK_TRANSFER
		// copy file contents
		if (dpu_id < used_dpus)
		{
			err = dpu_copy_to(dpu, "input_buffer", 0, input[dpu_id].buffer, ALIGN(input[dpu_id].total_length, 8));
			if (err != DPU_OK)
			{
				dbg_printf("Error %u copying input buffer\n", err);
				return -1;
			}
		}

		// copy file info
		err = dpu_copy_to(dpu, "file", 0, input[dpu_id].files, ALIGN(sizeof(file_descriptor) * MAX_FILES_PER_DPU, 8));
		if (err != DPU_OK)
		{
			dbg_printf("Error %u copying input files\n", err);
			return -1;
		}
#endif //BULK_TRANSFER
	}

#ifdef BULK_TRANSFER
	uint32_t largest_length;
	char *buffer;

	// transfer the buffers (file contents)
	largest_length=0;
	DPU_FOREACH(dpu_rank, dpu, dpu_id)
	{
		uint32_t buffer_length = MIN(MAX_INPUT_LENGTH, input[dpu_id].total_length);
		if (dpu_id < used_dpus)
		{
			buffer_length = MIN(MAX_INPUT_LENGTH, input[dpu_id].total_length);
			buffer = input[dpu_id].buffer;
			// calculate an equal amount of work that each tasklet should do
			chunk_size = MAX(MIN_CHUNK_SIZE, ALIGN(buffer_length / NR_TASKLETS, 8));
		}
		else
		{
			// in the case of an empty DPU, we must provide a dummy buffer for prepare_xfer
			buffer_length = 0;
			buffer = dummy_buffer;
		}

		err = dpu_prepare_xfer(dpu, (void*)buffer);
		if (err != DPU_OK)
		{
			dbg_printf("Error %u preparing xfer\n", err);
			return -1;
		}

		// keep track of the largest length seen so far, since the xfer must be
		// the size of the largest buffer
		if (buffer_length > largest_length)
			largest_length = buffer_length;
	}
	err = dpu_push_xfer(dpu_rank, DPU_XFER_TO_DPU, "input_buffer", 0, ALIGN(largest_length, 8), DPU_XFER_DEFAULT);
	if (err != DPU_OK)
	{
		dbg_printf("Error %u pushing file contents\n", err);
		return -1;
	}

	// transfer the file start & length in the buffer
	largest_length=0;
	DPU_FOREACH(dpu_rank, dpu, dpu_id)
	{
		err = dpu_prepare_xfer(dpu, (void*)input[dpu_id].files);
		if (err != DPU_OK)
		{
			dbg_printf("Error %u copying input files\n", err);
			return -1;
		}
		if (input[dpu_id].file_count > largest_length)
			largest_length = input[dpu_id].file_count;
	}
	err = dpu_push_xfer(dpu_rank, DPU_XFER_TO_DPU, "file", 0,
		ALIGN(sizeof(file_descriptor) * largest_length, 8), DPU_XFER_DEFAULT);
	if (err != DPU_OK)
	{
		dbg_printf("Error %u pushing file info\n", err);
		return -1;
	}
#endif //BULK_TRANSFER

	// launch all of the DPUs after they have been loaded
	err = dpu_launch(dpu_rank, DPU_ASYNCHRONOUS);
	if (err != DPU_OK)
	{
		dbg_printf("Error %u pushing xfer\n", err);
		return -1;
	}

	return 0;
}

int read_results_dpu_rank(struct dpu_set_t dpu_rank, struct host_rank_context *rank_ctx)
{
	struct dpu_set_t dpu;
	dpu_error_t err;
	uint8_t dpu_id;

#ifdef STATISTICS
	struct timespec stop;
	clock_gettime(CLOCK_MONOTONIC, &stop);
	double rank_time = TIME_DIFFERENCE(rank_ctx->start_rank, stop);
	printf("Rank processed in %2.2f s\n", rank_time);
#endif // STATISTICS

#ifdef BULK_TRANSFER
	// get performance metrics (instructions per DPU)
	uint32_t size = sizeof(uint32_t);
	DPU_FOREACH(dpu_rank, dpu, dpu_id)
	{
		err = dpu_prepare_xfer(dpu, (void*)&rank_ctx->dpus[dpu_id].perf);
		if (err != DPU_OK)
		{
			dbg_printf("Error %u preparing xfer for results\n", err);
			return -1;
		}
	}
	err = dpu_push_xfer(dpu_rank, DPU_XFER_FROM_DPU, "perf", 0, size, DPU_XFER_DEFAULT);
	if (err != DPU_OK)
	{
		dbg_printf("Error %u pushing xfer\n", err);
		return -1;
	}

	// get file statistics
	size = ALIGN(sizeof(rank_ctx->dpus[dpu_id].stats), 8);
	DPU_FOREACH(dpu_rank, dpu, dpu_id)
	{
		err = dpu_prepare_xfer(dpu, (void*)rank_ctx->dpus[dpu_id].stats);
		if (err != DPU_OK)
		{
			dbg_printf("Error %u preparing xfer for results\n", err);
			return -1;
		}
	}
	err = dpu_push_xfer(dpu_rank, DPU_XFER_FROM_DPU, "stats", 0, size, DPU_XFER_DEFAULT);
	if (err != DPU_OK)
	{
		dbg_printf("Error %u pushing xfer\n", err);
		return -1;
	}
#endif //BULK_TRANSFER

	DPU_FOREACH(dpu_rank, dpu, dpu_id)
	{
		if (dpu_id >= rank_ctx->dpu_count)
			break;

#ifdef DEBUG_DPU
		// get any DPU debug messages
		err = dpu_log_read(dpu, stdout);
		if (err != DPU_OK)
		{
			dbg_printf("Error %u retrieving log\n", err);
			return -1;
		}
#endif // DEBUG_DPU

#ifndef BULK_TRANSFER
		// Get the results back from each individual DPU in the rank
		err = dpu_copy_from(dpu, "stats", 0, &rank_ctx->dpus[dpu_id].stats, sizeof(file_stats) * MAX_FILES_PER_DPU);
		if (err != DPU_OK)
		{
			dbg_printf("Error %u getting line count\n", err);
			return -1;
		}

		// the list containing precise offset in the file for each match
		//DPU_ASSERT(dpu_copy_from(dpu, "matches", 0, matches, sizeof(uint32_t) * match_count);
#endif //BULK_TRANSFER
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
					results->total_files += rank_ctx->dpus[dpu_id].file_count;
					results->total_instructions += rank_ctx->dpus[dpu_id].perf;
					printf("DPU %u: files: %u bytes: %u instructions %u inst/byte: %02.2f\n",
						dpu_id, 
						rank_ctx->dpus[dpu_id].file_count,
						rank_ctx->dpus[dpu_id].total_length,
						rank_ctx->dpus[dpu_id].perf,
						(double) rank_ctx->dpus[dpu_id].perf / (double)rank_ctx->dpus[dpu_id].total_length);

					for (uint32_t file=0; file < rank_ctx->dpus[dpu_id].file_count; file++)
					{
						results->total_line_count += rank_ctx->dpus[dpu_id].stats[file].line_count;
						results->total_match_count += rank_ctx->dpus[dpu_id].stats[file].match_count;
						printf("%s:%u\n", rank_ctx->dpus[dpu_id].filename[file], rank_ctx->dpus[dpu_id].stats[file].match_count);

						// free the memory of the descriptor
						free(rank_ctx->dpus[dpu_id].filename[file]);
					}
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
	if (fin == NULL) {
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

static int dpu_main(struct jpeg_options *opts, host_results *results)
{
	char dpu_program_name[32];
	struct host_rank_context *ctx;
	struct dpu_set_t dpus, dpu_rank;
	int status;
	uint8_t rank_id;
	uint64_t rank_status=0; // bitmap indicating if the rank is busy or free
	uint32_t submitted;

#ifdef STATISTICS
	struct timespec start_load, stop_load;
#endif // STATISTICS

#ifdef BULK_TRANSFER
	printf("Using bulk transfer\n");
#endif // BULK_TRANSFER

	// allocate all of the DPUS up-front, then check to see how many we got
	status = dpu_alloc(DPU_ALLOCATE_ALL, NULL, &dpus);
	if (status != DPU_OK)
	{
		fprintf(stderr, "Error %i allocating DPUs\n", status);
		return -3;
	}

	dpu_get_nr_ranks(dpus, &rank_count);
	dpu_get_nr_dpus(dpus, &dpu_count);
	dpus_per_rank = dpu_count/rank_count;
	dbg_printf("Got %u dpus across %u ranks (%u dpus per rank)\n", dpu_count, rank_count, dpus_per_rank);

	// artificially limit the number of ranks based on user request
	if (rank_count > opts->max_ranks)
		rank_count= opts->max_ranks;

	snprintf(dpu_program_name, 31, "%s-%u", DPU_PROGRAM, NR_TASKLETS);
	DPU_ASSERT(dpu_load(dpus, dpu_program_name, NULL));

	if (rank_count > 64)
	{
		printf("Error: too many ranks for a 64-bit bitmask!\n");
		return -4;
	}
	if (opts->input_file_count < dpu_count)
		printf("Warning: fewer input files than DPUs (%u < %u)\n", opts->input_file_count, dpu_count);

	// allocate space for DPU descriptors for all ranks
	ctx = calloc(rank_count, sizeof(host_rank_context));

	// prepare the dummy buffer
	sprintf(dummy_buffer, "DUMMY DUMMY DUMMY");

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

		rank_input = calloc(dpus_per_rank, sizeof(struct host_dpu_descriptor));

		// set the dpu_count to 0 for each input
		for (dpu_id=0; dpu_id < dpus_per_rank; dpu_id++)
		{
			rank_input[dpu_id].file_count = 0;
			rank_input[dpu_id].total_length = 0;
			rank_input[dpu_id].buffer = malloc(MAX_INPUT_LENGTH);
		}

		// prepare enough files to fill the rank, trying to fit in as many
		// files as possible in a round-robin fashion to spread out the load
		// across (1) all DPUs of the rank, (2) all tasklets in a DPU
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
					(rank_input[dpu_id].total_length + file_length < MAX_INPUT_LENGTH))
				{
					dbg_printf("Allocating %s to DPU %u file count=%u, length=%lu, total_length=%lu\n",
						filename, dpu_id, rank_input[dpu_id].file_count,
						file_length, rank_input[dpu_id].total_length + file_length);
					file_descriptor *input = &rank_input[dpu_id].files[rank_input[dpu_id].file_count];

					// prepare the input buffer descriptor
					memset(input, 0, sizeof(file_descriptor));
					input->start = rank_input[dpu_id].total_length;
					input->length = file_length;

					// read the file into the descriptor
					next = rank_input[dpu_id].buffer + rank_input[dpu_id].total_length;
					rank_input[dpu_id].filename[rank_input[dpu_id].file_count] = strdup(filename);
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
					rank_input[dpu_id].total_length += file_length;// if we need alignment, do it here
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
		printf("[%u] %u of %u MB (%2.2f%%)loaded in %2.2f s\n", dpu_id,
			rank_input[dpu_id].total_length, TOTAL_MRAM>>20,
			(double)rank_input[dpu_id].total_length * 100 / TOTAL_MRAM, load_time);
#endif // STATISTICS
		dbg_printf("Prepared %u files in %u DPUs\n", prepared_file_count, prepared_dpu_count);
		submitted = 0;
		while (!submitted)
		{
			while (rank_status == ALL_RANKS)
			{
				int ret = check_for_completed_rank(dpus, &rank_status, ctx, results);
				if (ret == -2)
				{
					printf("A rank has faulted\n");
					status = PROG_FAULT;
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
					clock_gettime(CLOCK_MONOTONIC, &ctx[rank_id].start_rank);
					status = scale_rank(dpu_rank, rank_id, rank_input, prepared_file_count, prepared_dpu_count, opts);
					ctx[rank_id].dpus = rank_input;
					ctx[rank_id].dpu_count = prepared_dpu_count;
					submitted = 1;
					break;
				}
			}

			if (!submitted)
				printf("ERROR: failed to submit\n");
		}
	}

	// all files have been submitted; wait for all jobs to finish
	dbg_printf("Waiting for all DPUs to finish\n");
	while (rank_status)
	{
		int ret = check_for_completed_rank(dpus, &rank_status, ctx, results);
		if (ret == -2)
		{
			status = PROG_FAULT;
			goto done;
		}
		usleep(1);
	}

done:
	dpu_free(dpus);

	return status;
}

static int cpu_main(struct jpeg_options *opts, host_results *results)
{
	char *buffer = malloc(MAX_INPUT_LENGTH);
	uint32_t file_index=0;
	uint32_t remaining_file_count = opts->input_file_count;

	dbg_printf("Input file count=%u\n", opts->input_file_count);

	// as long as there are still files to process
	while (remaining_file_count--)
	{
			struct stat st;
		char *filename = input_files[file_index];

		// read the length of the next input file
		stat(filename, &st);
		uint64_t file_length = st.st_size;
		if (file_length > MAX_INPUT_LENGTH)
		{
			dbg_printf("Skipping file %s (%lu > %u)\n", filename, file_length, MAX_INPUT_LENGTH);
			continue;
		}

		// read the file into the descriptor
		if (read_input_host(filename, file_length, buffer) < 0)
		{
			dbg_printf("Skipping invalid file %s\n", input_files[file_index]);
			break;
		}

#ifdef STATISTICS
		total_data_processed += file_length;
#endif // STATISTICS

		jpeg_cpu_scale(file_length, buffer);
	}
	
	return 0;
}

static void usage(const char* exe_name)
{
#ifdef DEBUG
	fprintf(stderr, "**DEBUG BUILD**\n");
#endif //DEBUG
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
int main(int argc, char **argv)
{
	int opt;
	int use_dpu = 0;
	int status;
	uint32_t allocated_count = 0;
	struct jpeg_options opts;
	host_results results;

#ifdef STATISTICS
	double total_time;
	struct timespec start, stop;

	if (clock_gettime(CLOCK_MONOTONIC, &start) < 0)
	{
		printf("Error getting time\n");
		return -10;
	}
#endif // STATISTICS

	memset(&results, 0, sizeof(host_results));
	memset(&opts, 0, sizeof(struct jpeg_options));
	opts.max_files = -1; // no effective maximum by default
	opts.max_ranks = -1; // no effective maximum by default

	while ((opt = getopt(argc, argv, options)) != -1)
	{
		switch(opt)
		{
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
			opts.flags |= (1<<OPTION_FLAG_MULTIPLE_FILES);
			dbg_printf("Allocating multiple files per DPU\n");
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
	if (remain_arg_count && strcmp(argv[optind], "-") == 0)
	{
		char buff[TEMP_LENGTH];
		int bytes_remaining;
		allocated_count = 1;
		input_files = malloc(sizeof(char*) * allocated_count);
		bytes_remaining = fread(buff, 1, TEMP_LENGTH, stdin);
		while(bytes_remaining > 0)
		{
			int consumed;
			struct stat st;
			// see if we need more space for file names
			if (opts.input_file_count == allocated_count)
			{
				allocated_count <<= 1;
				input_files = realloc(input_files, sizeof(char*) * allocated_count);
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
	}
	else
	{
		input_files = malloc(sizeof(char*) * remain_arg_count);
		for (int i=0; i < remain_arg_count; i++)
		{
			struct stat s;
			char *next = argv[i + optind];
			if (stat(next, &s) == 0 && S_ISREG(s.st_mode))
				input_files[opts.input_file_count++] = argv[i + optind];
		}
	}

	// if there are no input files, we have no work to do!
	if (opts.input_file_count == 0)
	{
		printf("No input files!\n");
		usage(argv[0]);
		return -1;
	}

	if (opts.input_file_count > opts.max_files)
	{
		opts.input_file_count = opts.max_files;
		dbg_printf("Limiting input files to %u\n", opts.input_file_count);
	}

	if (use_dpu)
		status = dpu_main(&opts, &results);
	else
		status = cpu_main(&opts, &results);

	if (status != PROG_OK)
	{
		fprintf(stderr, "encountered error %u\n", status);
		exit(EXIT_FAILURE);
	}

#ifdef STATISTICS
	clock_gettime(CLOCK_MONOTONIC, &stop);
	total_time = TIME_DIFFERENCE(start, stop);
	printf("Sequential reader size: %u\n", SEQREAD_CACHE_SIZE);
	printf("Number of DPUs: %u\n", dpu_count);
	printf("Number of ranks: %u\n", rank_count);
	printf("Total line count: %u\n", results.total_line_count);
	printf("Total matches: %u\n", results.total_match_count);
	printf("Total files: %u\n", results.total_files);
	printf("Total data processed: %lu\n", total_data_processed);
	printf("Total time: %0.2fs\n", total_time);
	printf("Total DPUs launched: %lu\n", total_dpus_launched);
	printf("Total instructions: %lu\n", results.total_instructions);
	printf("Average instructions per byte: %lu\n", results.total_instructions / total_data_processed);
	printf("Average utilization per DPU: %2.3f%%\n", (double)total_data_processed * 100 / (double)total_dpus_launched / (double)TOTAL_MRAM);
#endif // STATISTICS

	dbg_printf("Freeing input files\n");
	free(input_files);
	input_files = NULL;

	return 0;
}
