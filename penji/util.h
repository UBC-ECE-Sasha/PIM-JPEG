#ifndef UTIL_H
#define UTIL_H

#include <stdio.h>
#include <stdlib.h>

// Error exit handler
#define ERREXIT(msg)    (fprintf(stderr, "%s\n", msg), exit(EXIT_FAILURE))

// JPEG markers
#define SOI_MARKER 0xFFD8   /* Start of Image */
#define SOF_MARKER 0xFFC0   /* Start of Frame */
#define SOS_MARKER 0xFFDA   /* Start of Scan */
#define EOI_MARKER 0xFFD9   /* End of Image */
#define DHT_MARKER 0xFFC4   /* Define Huffman Table */

// Huffman table classes
#define HUFF_TABLE_DC 0
#define HUFF_TABLE_AC 1

// Huffman End Of Block Code
#define HUFF_CODE_EOB 0x00

// Reading file input
unsigned int read_1_byte(FILE *file, int *bytes_read)
{
    unsigned int c;

    c = getc(file);
    if (c == EOF)
       ERREXIT("Premature EOF in JPEG file"); 
    
    *bytes_read++;
    return c;
}

unsigned int read_2_bytes(FILE *file, int *bytes_read)
{
    unsigned int c1, c2, ret; 

    c1 = getc(file);
    if (c1 == EOF)
        ERREXIT("Premature EOF in JPEG file");
    
    c2 = getc(file);
    if (c2 == EOF)
        ERREXIT("Premature EOF in JPEG file");

    *bytes_read += 2;

    ret = c1 << 8;
    ret = ret + c2;
    return ret; 
}

void print_file_offset(FILE *file)
{
    long offset = ftell(file);
    printf("File position: 0x%05x\n", offset);
}

/*
 * Find the next JPEG marker and store it in marker parameter.
 * Add the number of bytes read to bytes_read.
 *
 * NOTE: Do not use this function to after seeing an SOS marker, it will not
 * correctly deal with FF/00 sequences in the compressed image data
 */
void next_marker(FILE *file, int *bytes_read, unsigned int *marker)
{
    unsigned int c1, c2;

    // Find 0xFF byte
    c1 = read_1_byte(file, bytes_read);
    while(c1 != 0xFF) {
        c1 = read_1_byte(file, bytes_read);
    }

    // Get the marker, swallowing any duplicate FF bytes which are pad bytes
    do {
        c2 = read_1_byte(file, bytes_read); 
    } while (c2 == 0xFF);

    *marker = c1 << 8;
    *marker += c2;
}



struct huffman_symbol {
    unsigned char value;
    unsigned char length;
    struct huffman_symbol *next;
};

typedef struct huffman_symbol huffman_symbol;

struct huffman_table {
    unsigned char frequencies[16];  /* frequencies table */
    huffman_symbol symbols[16];     /* List of linked list of symbols */
    unsigned char table_class;
    unsigned char destination_identifier;
};

typedef struct huffman_table huffman_table;


/**
 * Create a huffman table from the given file.
 * NOTE: The file must be currently at exactly one byte past a DHT marker.
 * i.e. we've just read the DHT marker, and then we call this function to get
 * the corresponding huffman table.
 *
 * Return null if unable to parse the table.
 */
huffman_table* parse_huffman_table(FILE *file, int *bytes_read)
{
    huffman_table *table = (huffman_table *) malloc(sizeof(huffman_table));

    unsigned short table_length = read_2_bytes(file, bytes_read); 
    unsigned char tc_th = read_1_byte(file, bytes_read);
    unsigned char table_class = tc_th >> 4;
    unsigned char destination_id = tc_th & 0x0F;

    table->table_class = table_class;
    table->destination_identifier = destination_id;

    // Read through the frequency values
    for (int index = 0; index < 16; index++) {
        unsigned char frequency = read_1_byte(file, bytes_read);
        table->frequencies[index] = frequency;
    }

    // Read the symbols associated with each length
    for (int index = 0; index < 16; index++) {
        int frequency = table->frequencies[index];
        huffman_symbol *current_symbol = NULL;
        for (int count = 0; count < frequency; count++) {
            unsigned char value = read_1_byte(file, bytes_read);
            if (count == 0) {
                table->symbols[index].value = value;
                table->symbols[index].length = index + 1;
                table->symbols[index].next = NULL;
            } else {
                huffman_symbol *new_symbol = (huffman_symbol *) malloc(sizeof(huffman_symbol));
                new_symbol->value = value;
                new_symbol->length = index + 1;
                new_symbol->next = NULL;

                if (count == 1) {
                    table->symbols[index].next = new_symbol;
                } else {
                    current_symbol->next = new_symbol;
                }
                current_symbol = new_symbol;
            }
        }
    }

    return table;
}

/**
 * Print out a huffman table
 */
void print_huffman_table(huffman_table *table)
{
    fprintf(stdout, "Frequencies:\n");
    for (int index = 0; index < 16; index++) {
        printf("%d: %d\n", index + 1, table->frequencies[index]);
    } 

    fprintf(stdout, "Values by length:\n");
    for (int index = 0; index < 16; index++) {
        printf("%d: ", index + 1);
        int frequency = table->frequencies[index];
        huffman_symbol *current_symbol;
        for (int count = 0; count < frequency; count++) {
            if (count == 0) {
                current_symbol = &table->symbols[index];
            } else {
                current_symbol = current_symbol->next;
            }

            printf("0x%02x ", current_symbol->value);
        }
        printf("\n");
    }
}

/**
 * Cleanup the resources associated with a given huffman table
 */
void destroy_huffman_table(huffman_table *table)
{
    // TODO: Implement this
}


#endif /* UTIL_H */
