/*
 * remove_lines.c
 *
 * A standalone program that reduce the number of lines in the jpeg file.
 * This essentially has the effect of cropping the bottom of the image.
 * By default we crop half of the image. We might be able to further
 * crop a parametrized numbe of lines.
 *
 * The key is to generate a valid jpeg image without decompressing the entire
 * image.
 *
 * Usage:
 * 	remove_lines [-n num_lines] <inputfile> <outputfile>
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "jpeglib.h"
/*#include "cdjpeg.h" 	// Not a 100% convinced we need this*/

#define START_OF_FRAME_MARKER 0xffc0
#define DISTANCE_TO_LINES_FIELD 4 // This is the distance in bytes from the START_OF_FRAME marker to the location where we set the NUMBER_OF_LINES field
#define BUFFER_SIZE 1024

void print_usage(void)
{
	fprintf(stderr, "usage: remove_lines [-n num_lines] <inputfile>");
	fprintf(stderr, " <outputfile>\n");
}

int main(int argc, char **argv)
{ 	
    FILE *input_file;
	FILE *output_file;

    /* Check argc */
    if (argc == 3) {
        input_file = fopen(argv[1], "rb");
        output_file = fopen(argv[2], "wb");
    } else if (argc == 5) {
        input_file = fopen(argv[3], "rb");
        output_file = fopen(argv[4], "wb");
    } else {
        fprintf(stderr,"Invalid number of args\n");
        print_usage();
        exit(EXIT_FAILURE);
    }

    if (input_file == NULL) {
        fprintf(stderr, "Cannot open input file\n");
        exit(EXIT_FAILURE);
    }
	if (output_file == NULL) {
        fprintf(stderr, "Cannot open output file\n");
        fclose(input_file);
        exit(EXIT_FAILURE);
    }
    
    struct jpeg_decompress_struct cinfo;
	struct jpeg_error_mgr jerr;
	unsigned char *inbuffer = NULL;
	JDIMENSION num_scanlines;

	/* Initialize the JPEG decompression object with default error handling */       
	cinfo.err = jpeg_std_error(&jerr);
	jpeg_create_decompress(&cinfo);

    /* Specify data source for decompression */
    jpeg_stdio_src(&cinfo, TRUE);
     
    /* Read the file header, set default decompression parameters */
    (void)jpeg_read_header(&cinfo, TRUE);

    /* Now we know the image's height, we can seek to the binary postion where
     * this is written, modify that value, delete any extra lines of data if
     * possible, and write the modified data to the output file. Alternatively
     * we don't even need to do all the setup for using the library if we're
     * just going to seek to the START_OF_FRAME marker anyway
     */

    /* Find the START_OF_FRAME marker in the input file */
    unsigned char *in_buf = malloc(sizeof(unsigned char) * BUFFER_SIZE);
    if (in_buf == NULL) {
        goto cleanup;
    }

    size_t bytes_read;
    size_t char_size = sizeof(unsigned char);
    unsigned char last_byte_read = 0;
    int found_marker = FALSE;
    int local_distance_to_lines_field = DISTANCE_TO_LINES_FIELD;
    size_t total_bytes_read = 0;
    size_t buf_index;

    while (bytes_read = fread(in_buf, char_size, BUFFER_SIZE, input_file) != 0) {
        for (buf_index = 0; buf_index < bytes_read; buf_index++) {
            total_bytes_read += 1;

            unsigned short potential_marker = last_byte_read << 8;
            potential_marker += in_buf[buf_index];

            if (potential_marker == START_OF_FRAME_MARKER) {
                found_marker = TRUE;
                break;
            } else {
                last_byte_read = in_buf[buf_index];
            }

        } 

        if (!found_marker) {
            /* We can safely copy the current buffer to the new file */
            size_t bytes_written = fwrite(in_buf, char_size, bytes_read, output_file);
            if (bytes_written != bytes_read) {
                fprintf(stderr, "Error while writing to output file\n");
                goto cleanup;
            }
        } else {
            /* Write everything up to and excluding the marker */
            size_t bytes_written = fwrite(in_buf, char_size, buf_index, output_file);
            if (bytes_written != buf_index) {
                fprintf(stderr, "Error while writing to output file\n");
                goto cleanup;
            }
            break;
        }
    }

   if (!found_marker) {
       fprintf(stderr, "Went through entire file without finding START_OF_FRAME marker\n");
       goto cleanup;
   } 

   /* 
    * Invariant: At this point, we have found the marker, we have written everything before the marker to the output file
    */
   if (buf_index + DISTANCE_TO_LINES_FIELD + 1 > BUFFER_SIZE) {
       /* Edge case. We need to read in another chunk in order to modify the Lines field.. Low priority so we just fail for now */
       fprintf(stderr, "We got unlucky and hit an edge case which wasn't worth handling at the moment. Please try another jpeg input file\n");
    goto cleanup;
   }

   unsigned short old_lines = in_buf[buf_index + DISTANCE_TO_LINES_FIELD] << 8;
   old_lines += in_buf[buf_index + DISTANCE_TO_LINES_FIELD + 1];
   unsigned short new_lines = old_lines >> 1;

   fprintf(stdout, "old lines: %d == 0x%04x\n", old_lines, old_lines);
   fprintf(stdout, "new lines: %d == 0x%04x\n", new_lines, new_lines);

   in_buf[buf_index + DISTANCE_TO_LINES_FIELD] = new_lines >> 8;
   in_buf[buf_index + DISTANCE_TO_LINES_FIELD + 1] = new_lines & 0x00FF;

   /* Write the rest of the current chunk to the output file */
   int num_data_to_flush = BUFFER_SIZE - buf_index;
   unsigned char *tmp_buf = malloc(sizeof(unsigned char) * num_data_to_flush);
   if (tmp_buf == NULL) {
       goto cleanup;
   }

   for (int count = 0; count < num_data_to_flush; count++) {
       tmp_buf[count] = in_buf[buf_index + count];
   }

   size_t bytes_written = fwrite(tmp_buf, char_size, num_data_to_flush, output_file);
   if (bytes_written != num_data_to_flush) {
       fprintf(stderr, "Failed to write to output file\n");
       goto cleanup;
   } 

   /* Write the rest of the input file to the output file */
   /* TODO: Skip writing data that we're not going to use */

    while (bytes_read = fread(in_buf, char_size, BUFFER_SIZE, input_file) != 0) {
        bytes_written = fwrite(in_buf, char_size, bytes_read, output_file);
    }

    fprintf(stdout, "Done!\n");

    
cleanup:
    /* Close the files */
    if (in_buf != NULL) {
        free(in_buf);
    }

    if (tmp_buf != NULL) {
        free(tmp_buf);
    }
    fclose(input_file);
    fclose(output_file);
    return 0;
}
