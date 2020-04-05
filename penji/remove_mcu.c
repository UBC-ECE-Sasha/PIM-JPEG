/*
 * remove_mcu.c
 *
 * A standalone program that removes every other MCU in a row, effectively
 * modifying the 'samples_per_line' header field, but without explicitly
 * updating the header field.
 *
 * Usage:
 *  remove_mcu <inputfile> <outputfile>
 *
 */

/*
 * NOTE: A lot of functions have been copied from mcu_block_counter.c
 * I'm sure we can do better to prevent this duplication
 *
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "util.h"
#include <assert.h>

#define BUFFER_SIZE 1 << 14

FILE *infile;
FILE *outfile;
unsigned char out_buf[BUFFER_SIZE];
int out_buf_index = 0;
int bytes_read = 0;
unsigned int marker;

int original_num_lines = 0;
int original_samples_per_line = 0;
long expected_mcu_count = 0;
int num_image_components = 0;

void print_usage(void)
{
    fprintf(stderr, "usage: remove_mcu <inputfile> <outputfile>\n");
}


/* NOTE: Copied pretty much all of this from mcu_block_counter.c */
void add_bit_to_scan_buf(unsigned short *scan_buf, unsigned char *valid_buf_bits, unsigned char *input_data, unsigned char *input_data_bit_index)
{
    assert (*valid_buf_bits < 16);
    assert (*input_data_bit_index < 8);
    *scan_buf = (*scan_buf << 1) + ((*input_data >> (7 - *input_data_bit_index)) & 0x01); 
    *input_data_bit_index += 1;
    *valid_buf_bits += 1;
    if (*input_data_bit_index > 7) {
        *input_data = read_1_byte(infile, &bytes_read);
        *input_data_bit_index = 0;
    }    
}

/* NOTE: Copied pretty much all of this from mcu_block_counter.c */
void input_data_skip_bits(unsigned char *input_data, unsigned char *input_data_bit_index, short nbits)
{
    assert (*input_data_bit_index < 8);
    if (nbits > (8 - *input_data_bit_index)) {
        *input_data = 0;
        nbits -= 8 - *input_data_bit_index;
        *input_data_bit_index = 0;
    } else if (nbits == (8 - *input_data_bit_index)) {
        *input_data = 0;
        nbits = 0;
        *input_data = read_1_byte(infile, &bytes_read);
        *input_data_bit_index = 0;
    } else {
        *input_data_bit_index += nbits;
        nbits = 0;
    }

    while (nbits > 0) {
        if (nbits >= 16) {
            read_2_bytes(infile, &bytes_read);
            nbits -= 16;
            continue;
        }

        if (nbits >= 8) {
            read_1_byte(infile, &bytes_read);
            nbits -= 8;
            continue;
        }

        *input_data = read_1_byte(infile, &bytes_read);
        *input_data_bit_index = nbits;
        nbits = 0;
    }
    assert (nbits == 0);
}

/* NOTE: Copied pretty much all of this from mcu_block_counter.c */
void process_mcu(huffman_table *dc_table, huffman_table *ac_table,
        unsigned short *scan_buf, unsigned char *valid_buf_bits, unsigned char *input_data, unsigned char *input_data_bit_index)
{

    // First, we obtain a value from the DC table
    while (*valid_buf_bits <= 16) {
        huffman_symbol *huffsymbol = find_hufftable_entry(dc_table, scan_buf, valid_buf_bits);
        if (huffsymbol == NULL) {
            add_bit_to_scan_buf(scan_buf, valid_buf_bits, input_data, input_data_bit_index);
            continue;
        } 

        *scan_buf = 0;
        *valid_buf_bits = 0;

        if (huffsymbol->value == 0) {
            // Reached EOB
            break;
        }

        assert ((huffsymbol->value & 0xF0) == 0);
        input_data_skip_bits(input_data, input_data_bit_index, huffsymbol->value);
        break;
    } 

    assert (*valid_buf_bits <= 16);

    int mcu_indices[64];
    int mcu_indices_len = 0;
    // Now we obtain the 1-63 values for the AC table
    for (int i = 1; i < 64; i++) {
        unsigned char reached_eob = FALSE;
        while (*valid_buf_bits <= 16) {
            huffman_symbol *huffsymbol = find_hufftable_entry(ac_table, scan_buf, valid_buf_bits);
            if (huffsymbol == NULL) {
                add_bit_to_scan_buf(scan_buf, valid_buf_bits, input_data, input_data_bit_index);
                continue;
            } 

            *scan_buf = 0;
            *valid_buf_bits = 0;

            if (huffsymbol->value == 0) {
                // Reached EOB
                reached_eob = TRUE;
                mcu_indices[mcu_indices_len++] = i;
                break;
            }

            if (huffsymbol->value == 0xF0) {
                // Found ZRL
                // TODO: Potential off-by-one-error
                // TODO: Might even still need to skip data bits
                i += 16;
                assert (i < 64);
                break;
            }

            unsigned char runlen = huffsymbol->value >> 4;
            unsigned char skipbits = huffsymbol->value & 0x0F; 

            assert (runlen < 16);
            i += runlen;
            if (i >= 64) {
                fprintf(stderr, "WARNING: value of i is %d\n", i);
            }
            /*assert (i < 64);*/
            input_data_skip_bits(input_data, input_data_bit_index, skipbits);
            mcu_indices[mcu_indices_len++] = i;
            break;
        } 

        if (reached_eob == TRUE) {
            break;
        }
    }

    /*printf("MCU indices: [");*/
    /*for (int j = 0; j < mcu_indices_len; j++) {*/
        /*printf("%d, ", mcu_indices[j]);*/
    /*}*/
    /*printf("]\n");*/
}

void process_scan(huffman_table *dc_table, huffman_table *ac_table)
{
    printf("Processing Scan ");
    print_file_offset(infile);
    int header_len = read_2_bytes(infile, &bytes_read) - 2;
    unsigned char scan_header[header_len];

    while (header_len != 0) {
        if (header_len > 1) {
            read_2_bytes(infile, &bytes_read);
            header_len -= 2;
        } else {
            read_1_byte(infile, &bytes_read);
            header_len--;
        }
    }

    // We should now be at the start of the ECS block
    printf("Successfully got to ECS block\n");

    /* We must now do some matching between the ECS block data and the
     * corresponding huffman code. I'd love for this to be done more
     * efficiently, but the current approach is to try to match every huffman
     * code (starting from the shortest length) to the current data buffer
     * and see what sticks.
     */
    unsigned short scan_buf = 0;
    unsigned char valid_buf_bits = 0;
    unsigned char input_data = read_1_byte(infile, &bytes_read);
    unsigned char input_data_bit_index = 0;

    add_bit_to_scan_buf(&scan_buf, &valid_buf_bits, &input_data, &input_data_bit_index);

    long mcu_counter = 0;
    while(!feof(infile) && (last_marker_seen != EOI_MARKER) && mcu_counter < expected_mcu_count) {
        //TODO: Figure out a different way to stop...
        process_mcu(dc_table, ac_table,&scan_buf, &valid_buf_bits, &input_data, &input_data_bit_index);
        mcu_counter++;
        if (mcu_counter % 880 == 0) {
            fprintf(stdout, "Progress: Found %d MCUs so far\n", mcu_counter);
        }
    }

    fprintf(stdout, "Finished processing MCUs. Found %d MCU blocks\n", mcu_counter);
}


/* NOTE: Copied pretty much all of this from mcu_block_counter.c */
void process_frame()
{
    printf("Found frame? ");
    print_file_offset(infile);
    // Print the number of image components in frame
    read_2_bytes(infile, &bytes_read);     // length
    read_1_byte(infile, &bytes_read);      // Sample Precision
    original_num_lines = read_2_bytes(infile, &bytes_read);     // Lines
    original_samples_per_line = read_2_bytes(infile, &bytes_read);     // Samples per line
    num_image_components = read_1_byte(infile, &bytes_read);

    fprintf(stdout, "Number of image components in frame: %d\n", num_image_components);

    if (num_image_components != 1) {
        fprintf(stderr, "Can't deal with frames with more than one component at the moment\n");
        return;
    }

    expected_mcu_count = original_num_lines * original_samples_per_line / 64;
    fprintf(stdout, "Original Num Lines: %d\nOriginal Samples Per Line: %d\nExpected MCU count: %d\n", original_num_lines, original_samples_per_line,
            expected_mcu_count);

    // Find Define Huffman Table marker for DC table
    while (marker != DHT_MARKER) {
        next_marker(infile, &bytes_read, &marker);
    }
    
    printf("DC Table? ");
    print_file_offset(infile);
    huffman_table *dc_htable = parse_huffman_table(infile, &bytes_read);
    if (dc_htable == NULL) {
        fprintf(stderr, "Could not parse Huffman Table");
        fclose(infile);
        exit(EXIT_FAILURE);
    }

    /*printf("DC table:\n");*/
    /*print_huffman_table(dc_htable);*/
    /*print_huffman_bitstrings(dc_htable);*/

    // Find Define Huffman Table marker for AC table
    next_marker(infile, &bytes_read, &marker);
    while (marker != DHT_MARKER) {
        next_marker(infile, &bytes_read, &marker);
    }
    
    printf("AC Table? ");
    print_file_offset(infile);
    huffman_table *ac_htable = parse_huffman_table(infile, &bytes_read);
    if (ac_htable == NULL) {
        fprintf(stderr, "Could not parse Huffman Table");
        fclose(infile);
        exit(EXIT_FAILURE);
    }

    /*printf("AC table:\n");*/
    /*print_huffman_table(ac_htable);*/
    /*print_huffman_bitstrings(ac_htable);*/
    
    // Find Start of Scan marker
    next_marker(infile, &bytes_read, &marker);
    while (marker != SOS_MARKER) {
        next_marker(infile, &bytes_read, &marker);
    }

    process_scan(dc_htable, ac_htable);
}

int main(int argc, char **argv)
{
    /* Check argc */
    if (argc == 3) {
        infile = fopen(argv[1], "rb");
        if (infile == NULL) {
            fprintf(stderr, "Cannot open input file\n");
            exit(EXIT_FAILURE);
        }
        outfile = fopen(argv[2], "wb");
        if (outfile == NULL) {
            fprintf(stderr, "Cannot open output file\n");
            exit(EXIT_FAILURE);
        }
    } else {
        fprintf(stderr, "Invalid number of args\n");
        print_usage();
        exit(EXIT_FAILURE);
    }

    setup_output_buffer(out_buf, &out_buf_index, BUFFER_SIZE, outfile);
    set_place_bytes_into_buffer(TRUE);

    // Find Start of Frame marker
    next_marker(infile, &bytes_read, &marker);
    while (marker != SOF_MARKER) {
        next_marker(infile, &bytes_read, &marker);
    }
    
    process_frame();
    
    // repeat frame processing in case there's another frame
    next_marker(infile, &bytes_read, &marker);
    while (marker != SOF_MARKER) {
        next_marker(infile, &bytes_read, &marker);
    }

    process_frame();

    // Write everything that's left to the output file
    while(last_marker_seen != EOI_MARKER) {
        next_marker(infile, &bytes_read, &marker);
    }
    write_buffer_to_file();
}
