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

// #include "PIM-common/host/include/host.h"
#include "bmp.h"
#include "host.h"
#include "jpeg-common.h"
#include "jpeg-host.h"

#define DPU_PROGRAM "src/dpu/jpeg-dpu"
#define MIN_CHUNK_SIZE 256 // not worthwhile making another tasklet work for data less than this
#define TEMP_LENGTH 256
#define ALL_RANKS (rank_count == 64 ? 0xFFFFFFFFFFFFFFFF : (1UL << rank_count) - 1)

// to extract components from dpu_id_t
#define DPU_ID_RANK(_x) ((_x >> 16) & 0xFF)
#define DPU_ID_SLICE(_x) ((_x >> 8) & 0xFF)
#define DPU_ID_DPU(_x) ((_x) &0xFF)

#define TIME_NOW(_t) (clock_gettime(CLOCK_MONOTONIC, (_t)))

const char options[] = "cdmn:k:r:s:Mw:f";
static uint32_t rank_count, dpu_count;
static uint32_t dpus_per_rank;
static char **input_files = NULL;
static char dummy_buffer[MAX_INPUT_LENGTH];

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

void scale_rank(struct dpu_set_t dpu_rank, dpu_settings_t *dpu_settings, uint32_t dpus_to_use) {
  struct dpu_set_t dpu;
  uint32_t dpu_id = 0; // the id of the DPU inside the rank (0-63)
  dpu_inputs_t dpu_inputs;
  dpu_inputs.file_length = dpu_settings->file_length;
  dpu_inputs.scale_width = dpu_settings->scale_width;
  dpu_inputs.horizontal_flip = dpu_settings->horizontal_flip;

  DPU_FOREACH(dpu_rank, dpu, dpu_id) {
    // printf("\tDPU ID %d\n", dpu_id);
    if (dpu_id >= dpus_to_use) {
      break;
    }

    DPU_ASSERT(dpu_copy_to(dpu, "input", 0, &dpu_inputs, sizeof(dpu_inputs_t)));

#ifndef BULK_TRANSFER
    DPU_ASSERT(
        dpu_copy_to(dpu, "file_buffer", 0, dpu_settings[dpu_id].buffer, ALIGN(dpu_settings[dpu_id].file_length, 8)));
#endif
  }

#ifdef BULK_TRANSFER
  int longest_length = 0;

  DPU_FOREACH(dpu_rank, dpu, dpu_id) {
    if (dpu_id >= dpus_to_use) {
      break;
    }

    DPU_ASSERT(dpu_prepare_xfer(dpu, (void *) dpu_settings[dpu_id].buffer));
    int file_length = dpu_settings[dpu_id].file_length;
    if (file_length > longest_length) {
      longest_length = file_length;
    }
  }
  DPU_ASSERT(dpu_push_xfer(dpu_rank, DPU_XFER_TO_DPU, "file_buffer", 0, ALIGN(longest_length, 8), DPU_XFER_DEFAULT));
#endif

  DPU_ASSERT(dpu_launch(dpu_rank, DPU_ASYNCHRONOUS));
}

int read_results_dpu_rank(struct dpu_set_t dpu_rank, dpu_output_t *dpu_outputs, short **MCU_buffer) {
  struct dpu_set_t dpu;
  uint8_t dpu_id;

#ifdef BULK_TRANSFER
  DPU_FOREACH(dpu_rank, dpu, dpu_id) {
    DPU_ASSERT(dpu_prepare_xfer(dpu, (void *) &dpu_outputs[dpu_id]));
  }
  DPU_ASSERT(dpu_push_xfer(dpu_rank, DPU_XFER_FROM_DPU, "output", 0, sizeof(dpu_output_t), DPU_XFER_DEFAULT));

  int largest_pixel_count = 0;
  DPU_FOREACH(dpu_rank, dpu, dpu_id) {
    DPU_ASSERT(dpu_prepare_xfer(dpu, (void *) MCU_buffer[dpu_id]));
    int pixel_count = ALIGN(dpu_outputs[dpu_id].image_height, 8) * ALIGN(dpu_outputs[dpu_id].image_width, 8);
    if (pixel_count > largest_pixel_count) {
      largest_pixel_count = pixel_count;
    }
  }
  DPU_ASSERT(dpu_push_xfer(dpu_rank, DPU_XFER_FROM_DPU, "MCU_buffer", 0, sizeof(short) * largest_pixel_count * 3,
                           DPU_XFER_DEFAULT));
#endif // BULK_TRANSFER

#ifndef BULK_TRANSFER
  DPU_FOREACH(dpu_rank, dpu, dpu_id) {
    DPU_ASSERT(dpu_copy_from(dpu, "output", 0, &dpu_outputs[dpu_id], sizeof(dpu_output_t)));
    DPU_ASSERT(dpu_copy_from(dpu, "MCU_buffer", 0, MCU_buffer[dpu_id],
                             sizeof(short) * ALIGN(dpu_outputs[dpu_id].image_height, 8) *
                                 ALIGN(dpu_outputs[dpu_id].image_width, 8) * 3));
  }
#endif // BULK_TRANSFER

  return 0;
}

int check_for_completed_rank(struct dpu_set_t dpus, uint64_t *rank_status, dpu_output_t *dpu_outputs,
                             short **MCU_buffer) {
  struct dpu_set_t dpu_rank, dpu;
  uint8_t rank_id = 0;

  DPU_RANK_FOREACH(dpus, dpu_rank) {
    bool done, fault;

    if (*rank_status & ((uint64_t) 1 << rank_id)) {
      // check to see if anything has completed
      dpu_status(dpu_rank, &done, &fault);
      /*if (fault) {
        bool dpu_done, dpu_fault;
        printf("rank %u fault - abort!\n", rank_id);

        // try to find which DPU caused the fault
        DPU_FOREACH(dpu_rank, dpu) {
          dpu_status(dpu, &dpu_done, &dpu_fault);
          if (dpu_fault) {
            dpu_id_t id = dpu_get_id(dpu.dpu);
            fprintf(stderr, "[%u:%u:%u] at fault\n", DPU_ID_RANK(id), DPU_ID_SLICE(id), DPU_ID_DPU(id));
          }
        }

        return -2;
      }*/

      if (done) {
        *rank_status &= ~((uint64_t) 1 << rank_id);
        // printf("Reading results from rank %u status %lu\n", rank_id, *rank_status);
        read_results_dpu_rank(dpu_rank, dpu_outputs, MCU_buffer);
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
static int read_input_host(char *in_file, uint64_t length, char *buffer) {
  FILE *fin = fopen(in_file, "r");
  if (fin == NULL) {
    fprintf(stderr, "Invalid input file: %s\n", in_file);
    return -2;
  }

  if (length == 0) {
    fprintf(stderr, "Skipping %s: size is too small (%ld)\n", in_file, length);
    return -2;
  }

  size_t n = fread(buffer, 1, length, fin);
  fclose(fin);

  return n;
}

static int dpu_main(struct jpeg_options *opts, host_results *results) {
  char dpu_program_name[32];
  struct dpu_set_t dpus, dpu_rank, dpu;
  int status;
  uint8_t rank_id;
  uint64_t rank_status = 0; // bitmap indicating if the rank is busy or free

  int rank_iterations = 0;
#ifdef STATISTICS
  // struct timespec start_load, stop_load;
#endif // STATISTICS

#ifdef BULK_TRANSFER
  printf("Using bulk transfer\n");
#endif // BULK_TRANSFER

  // allocate all of the DPUS up-front, then check to see how many we got
  // status = dpu_alloc(DPU_ALLOCATE_ALL, NULL, &dpus);
  status = dpu_alloc(opts->num_dpus, NULL, &dpus);
  if (status != DPU_OK) {
    fprintf(stderr, "Error %i allocating DPUs\n", status);
    return -3;
  }
  status = dpu_alloc_ranks(opts->num_ranks, NULL, &dpu_rank);
  if (status != DPU_OK) {
    fprintf(stderr, "Error %i allocating Ranks\n", status);
    return -3;
  }

  dpu_get_nr_ranks(dpu_rank, &rank_count);
  // rank_count=opts->num_ranks; //testing to manually set number of ranks to use since the above call seems to choose
  // #ranks differently
  // dpu_get_nr_dpus(dpus, &dpu_count);
  dpu_get_nr_dpus(dpus, &dpu_count);
  dpus_per_rank = dpu_count / rank_count;
  printf("Got %u dpus across %u ranks (%u dpus per rank)\n", dpu_count, rank_count, dpus_per_rank);

  // artificially limit the number of ranks based on user request
  if (rank_count > opts->max_ranks) {
    rank_count = opts->max_ranks;
  }

  snprintf(dpu_program_name, 31, "%s-%u", DPU_PROGRAM, NR_TASKLETS);
  DPU_ASSERT(dpu_load(dpus, dpu_program_name, NULL));

  if (rank_count > 64) {
    printf("Error: too many ranks for a 64-bit bitmask!\n");
    return -4;
  }
  if (opts->input_file_count < dpu_count) {
    printf("Warning: fewer input files than DPUs (%u < %u)\n", opts->input_file_count, dpu_count);
  }

  // prepare the dummy buffer
  sprintf(dummy_buffer, "DUMMY DUMMY DUMMY");

  uint32_t remaining_file_count = opts->input_file_count;
  dpu_settings_t *dpu_settings = calloc(dpus_per_rank, sizeof(dpu_settings_t));
  uint32_t dpu_id;
  for (dpu_id = 0; dpu_id < dpus_per_rank; dpu_id++) {
    dpu_settings[dpu_id].buffer = malloc(MAX_INPUT_LENGTH);
  }

  dpu_output_t *dpu_outputs = calloc(dpus_per_rank, sizeof(dpu_output_t));
  short **MCU_buffer = malloc(sizeof(short *) * dpus_per_rank);
  for (dpu_id = 0; dpu_id < dpus_per_rank; dpu_id++) {
    MCU_buffer[dpu_id] = malloc(sizeof(short) * 87380 * 3 * 64);
  }

  for (uint32_t i = 0; i < remaining_file_count; i += dpus_per_rank) {
    for (dpu_id = 0; dpu_id < dpus_per_rank; dpu_id++) {
      int file_index = i + dpu_id;
      if (file_index >= remaining_file_count) {
        break;
      }

      struct stat st;
      char *filename = input_files[file_index];

      // read the length of the next input file
      stat(filename, &st);
      uint64_t file_length = st.st_size;
      if (file_length > MAX_INPUT_LENGTH) {
        printf("Skipping file %s (%lu > %u)\n", filename, file_length, MAX_INPUT_LENGTH);
        continue;
      }
      dpu_settings[dpu_id].file_length = file_length;
      dpu_settings[dpu_id].filename = filename;
      dpu_settings[dpu_id].scale_width = opts->scale_width;
      dpu_settings[dpu_id].horizontal_flip = opts->horizontal_flip;

      // read the file into the descriptor
      if (read_input_host(filename, file_length, dpu_settings[dpu_id].buffer) < 0) {
        printf("Skipping invalid file %s\n", input_files[file_index]);
        break;
      }
    }
  }
  uint32_t dpus_to_use = dpu_id;
  printf("dpus to use = %d\n", dpus_to_use);
  DPU_RANK_FOREACH(dpus, dpu_rank, rank_id) {
    printf("Rank ID: %d\n", rank_id);
    if (!(rank_status & (1UL << rank_id))) {
      rank_status |= (1UL << rank_id);
      rank_iterations++;
      scale_rank(dpu_rank, dpu_settings, dpus_to_use);
    }
  }

  while (rank_status) {
    int ret = check_for_completed_rank(dpus, &rank_status, dpu_outputs, MCU_buffer);
    if (ret == -2) {
      status = PROG_FAULT;
    }
  }

  for (dpu_id = 0; dpu_id < dpus_to_use; dpu_id++) {
    write_bmp_dpu(dpu_settings[dpu_id].filename, dpu_outputs[dpu_id].image_width, dpu_outputs[dpu_id].image_height,
                  dpu_outputs[dpu_id].padding, dpu_outputs[dpu_id].mcu_width_real, MCU_buffer[dpu_id]);

    dpu_output_t this_dpu_output = dpu_outputs[dpu_id];
  }

  /*DPU_RANK_FOREACH(dpus, dpu_rank, rank_id) {
    printf("Rank ID: %d\n", rank_id);
    DPU_FOREACH(dpu_rank, dpu, dpu_id) {
      printf("DPU ID: %d\n", dpu_id);
      DPU_ASSERT(dpu_log_read(dpu, stdout));
    }
  }*/

  free(dpu_outputs);
  free(MCU_buffer);

  for (dpu_id = 0; dpu_id < dpus_per_rank; dpu_id++) {
    free(dpu_settings[dpu_id].buffer);
  }
  free(dpu_settings);
  dpu_free(dpus);
  printf("%d\n", rank_iterations);
  return status;
}

static int cpu_main(struct jpeg_options *opts, host_results *results) {
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
  fprintf(stderr, "n: use n DPUs\n");
  fprintf(stderr, "k: use k Ranks\n");
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
  host_results results;

#ifdef STATISTICS
  double total_time;
  struct timespec start, stop;

  if (clock_gettime(CLOCK_MONOTONIC, &start) < 0) {
    printf("Error getting time\n");
    return -10;
  }
#endif // STATISTICS

  memset(&results, 0, sizeof(host_results));
  memset(&opts, 0, sizeof(struct jpeg_options));
  opts.max_files = -1; // no effective maximum by default
  opts.max_ranks = -1; // no effective maximum by default
  opts.scale_width = 256;
  opts.scale_height = 256;
  opts.horizontal_flip = 0; // no horizontal flip by default
  opts.num_dpus = 1;
  opts.num_ranks = 1;

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

      case 'n':
        opts.num_dpus = strtoul(optarg, NULL, 0);
        break;

      case 'k':
        opts.num_ranks = strtoul(optarg, NULL, 0);
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
    status = dpu_main(&opts, &results);
  else
    status = cpu_main(&opts, &results);

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
  printf("Total line count: %u\n", results.total_line_count);
  printf("Total matches: %u\n", results.total_match_count);
  printf("Total files: %u\n", results.total_files);
  printf("Total data processed: %lu\n", total_data_processed);
  printf("Total time: %0.2fs\n", total_time);
  printf("Total DPUs launched: %lu\n", total_dpus_launched);
  printf("Total instructions: %lu\n", results.total_instructions);
  printf("Average instructions per byte: %lu\n", results.total_instructions / total_data_processed);
  // printf("Average utilization per DPU: %2.3f%%\n",
  //        (double) total_data_processed * 100 / (double) total_dpus_launched / (double) TOTAL_MRAM);
#endif // STATISTICS

  dbg_printf("Freeing input files\n");
  free(input_files);
  input_files = NULL;

  return 0;
}
