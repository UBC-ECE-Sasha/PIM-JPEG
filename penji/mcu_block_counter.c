/*
 * mcu_block_counter.c
 *
 * A standalone program that finds and counts the MCU blocks in a jpeg file.
 *
 * Usage:
 *  mcu_block_counter <inputfile>
 *
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "util.h"

void print_usage(void)
{
    fprintf(stderr, "usage: mcu_block_counter <inputfile>\n");
}

int main(int argc, char **argv)
{
    FILE *file;

    /* Check argc */
    if (argc == 2) {
        file = fopen(argv[1], "rb");
        if (file == NULL) {
            fprintf(stderr, "Cannot open input file\n");
        }
    } else {
        fprintf(stderr, "Invalid number of args\n");
        print_usage();
        exit(EXIT_FAILURE);
    }

    // Find Start of Frame marker
    int bytes_read = 0;
    unsigned int marker;
    next_marker(file, &bytes_read, &marker);
    while (marker != SOF_MARKER) {
        next_marker(file, &bytes_read, &marker);
    }

    // Print the number of image components in frame
    read_2_bytes(file);     // length
    read_1_byte(file);      // Sample Precision
    read_2_bytes(file);     // Lines
    read_2_bytes(file);     // Samples per line
    unsigned int num_image_components = read_1_byte(file);

    bytes_read += 8;
    fprintf(stdout, "Number of image components in frame: %d\n", num_image_components);

    // Find Start of Segment marker
    next_marker(file, &bytes_read, &marker);
    while (marker != SOS_MARKER) {
        next_marker(file, &bytes_read, &marker);
    }

    int header_len = read_2_bytes(file) - 2;
    unsigned char scan_header[header_len];
    if ((fread(scan_header, sizeof(unsigned char), header_len, file)) != header_len) {
        ERREXIT("Reached EOF?\n");
    }

    bytes_read += 2 + header_len;

    // We should now be at the start of the ECS block
}
