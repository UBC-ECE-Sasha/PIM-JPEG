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


FILE *file;
int bytes_read = 0;
unsigned int marker;

void print_usage(void)
{
    fprintf(stderr, "usage: mcu_block_counter <inputfile>\n");
}

void process_scan(huffman_table *dc_table, huffman_table *ac_table)
{
    printf("Processing Scan ");
    print_file_offset(file);
    int header_len = read_2_bytes(file, &bytes_read) - 2;
    unsigned char scan_header[header_len];
    if ((fread(scan_header, sizeof(unsigned char), header_len, file)) != header_len) {
        ERREXIT("Reached EOF?\n");
    }

    bytes_read += header_len;

    // We should now be at the start of the ECS block
    printf("Successfully got to ECS block\n");

    /* We must now do some matching between the ECS block data and the
     * corresponding huffman code. I'd love for this to be done more
     * efficiently, but the current approach is to try to match every huffman
     * code (starting from the shortest length) to the current data buffer
     * and see what sticks.
     */
}

void process_frame()
{
    printf("Found frame? ");
    print_file_offset(file);
    // Print the number of image components in frame
    read_2_bytes(file, &bytes_read);     // length
    read_1_byte(file, &bytes_read);      // Sample Precision
    read_2_bytes(file, &bytes_read);     // Lines
    read_2_bytes(file, &bytes_read);     // Samples per line
    unsigned int num_image_components = read_1_byte(file, &bytes_read);

    fprintf(stdout, "Number of image components in frame: %d\n", num_image_components);

    if (num_image_components != 1) {
        fprintf(stderr, "Can't deal with frames with more than one component at the moment\n");
        return;
    }

    // Find Define Huffman Table marker for DC table
    while (marker != DHT_MARKER) {
        next_marker(file, &bytes_read, &marker);
    }
    
    printf("DC Table? ");
    print_file_offset(file);
    huffman_table *dc_htable = parse_huffman_table(file, &bytes_read);
    if (dc_htable == NULL) {
        fprintf(stderr, "Could not parse Huffman Table");
        fclose(file);
        exit(EXIT_FAILURE);
    }

    printf("DC table:\n");
    print_huffman_table(dc_htable);

    // Find Define Huffman Table marker for AC table
    next_marker(file, &bytes_read, &marker);
    while (marker != DHT_MARKER) {
        next_marker(file, &bytes_read, &marker);
    }
    
    printf("AC Table? ");
    print_file_offset(file);
    huffman_table *ac_htable = parse_huffman_table(file, &bytes_read);
    if (ac_htable == NULL) {
        fprintf(stderr, "Could not parse Huffman Table");
        fclose(file);
        exit(EXIT_FAILURE);
    }

    printf("AC table:\n");
    print_huffman_table(ac_htable);
    
    // Find Start of Scan marker
    next_marker(file, &bytes_read, &marker);
    while (marker != SOS_MARKER) {
        next_marker(file, &bytes_read, &marker);
    }

    process_scan(dc_htable, ac_htable);
}

int main(int argc, char **argv)
{

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
    next_marker(file, &bytes_read, &marker);
    while (marker != SOF_MARKER) {
        next_marker(file, &bytes_read, &marker);
    }
    
    process_frame();
    
    // repeat frame processing in case there's another frame
    next_marker(file, &bytes_read, &marker);
    while (marker != SOF_MARKER) {
        next_marker(file, &bytes_read, &marker);
    }

    process_frame();
}
