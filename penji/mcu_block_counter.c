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

huffman_symbol* find_hufftable_entry(huffman_table *table, unsigned short *scan_buf, unsigned char *valid_buf_bits)
{
    unsigned char len_index = *valid_buf_bits - 1;
    if (len_index < 0) {
        return NULL;
    }

    unsigned char available_codes = table->frequencies[len_index];
    if (available_codes == 0) {
        return NULL;
    }

    huffman_symbol *result = &table->symbols[len_index];
    for (unsigned char i = 0; i < available_codes; i++) {
        if (result->bitstring == *scan_buf) {
            break;
        } 
        result = result->next;
    }

    return result;
}

void add_bit_to_scan_buf(unsigned short *scan_buf, unsigned char *valid_buf_bits, unsigned char *input_data, unsigned char *input_data_bit_index)
{
    *scan_buf = (*scan_buf << 1) + ((*input_data >> (7 - *input_data_bit_index)) & 0x01); 
    *input_data_bit_index += 1;
    *valid_buf_bits += 1;
    if (*input_data_bit_index > 7) {
        *input_data = read_1_byte(file, &bytes_read);
        *input_data_bit_index = 0;
    }    
}

void input_data_skip_bits(unsigned char *input_data, unsigned char *input_data_bit_index, unsigned short nbits)
{
    if (nbits > (7 - *input_data_bit_index)) {
        *input_data = 0;
        nbits -= 7 - *input_data_bit_index;
        *input_data_bit_index = 0;
    } else {
        *input_data_bit_index += nbits;
    }

    while (nbits > 0) {
        if (nbits >= 16) {
            read_2_bytes(file, &bytes_read);
            nbits -= 16;
            continue;
        }

        if (nbits >= 8) {
            read_1_byte(file, &bytes_read);
            nbits -= 8;
            continue;
        }

        *input_data = read_1_byte(file, &bytes_read);
        *input_data_bit_index = nbits;
        nbits = 0;
    }
}

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

        input_data_skip_bits(input_data, input_data_bit_index, huffsymbol->value);
        break;
    } 

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
                break;
            }

            input_data_skip_bits(input_data, input_data_bit_index, huffsymbol->value);
            break;
        } 

        if (reached_eob == TRUE) {
            break;
        }
    }
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
    unsigned short scan_buf = 0;
    unsigned char valid_buf_bits = 0;
    unsigned char input_data = read_1_byte(file, &bytes_read);
    unsigned char input_data_bit_index = 0;

    add_bit_to_scan_buf(&scan_buf, &valid_buf_bits, &input_data, &input_data_bit_index);

    long mcu_counter = 0;
    while(!feof(file)) {
        //TODO: Figure out a different way to stop...
        process_mcu(dc_table, ac_table,&scan_buf, &valid_buf_bits, &input_data, &input_data_bit_index);
        mcu_counter++;
        if (mcu_counter % 80 == 0) {
            fprintf(stdout, "Progress: Found %d MCUs so far\n", mcu_counter);
        }
    }

    fprintf(stdout, "Finished processing MCUs. Found %d MCU blocks\n", mcu_counter);
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

    /*printf("DC table:\n");*/
    /*print_huffman_table(dc_htable);*/

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

    /*printf("AC table:\n");*/
    /*print_huffman_table(ac_htable);*/
    
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
