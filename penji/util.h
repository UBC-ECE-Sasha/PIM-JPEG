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


// Reading file input
unsigned int read_1_byte(FILE *file)
{
    unsigned int c;

    c = getc(file);
    if (c == EOF)
       ERREXIT("Premature EOF in JPEG file"); 
    
    return c;
}

unsigned int read_2_bytes(FILE *file)
{
    unsigned int c1, c2, ret; 

    c1 = getc(file);
    if (c1 == EOF)
        ERREXIT("Premature EOF in JPEG file");
    
    c2 = getc(file);
    if (c2 == EOF)
        ERREXIT("Premature EOF in JPEG file");

    ret = c1 << 8;
    ret = ret + c2;
    return ret; 
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
    c1 = read_1_byte(file);
    *bytes_read += 1;
    while(c1 != 0xFF) {
        c1 = read_1_byte(file);
        *bytes_read += 1;
    }

    // Get the marker, swallowing any duplicate FF bytes which are pad bytes
    do {
        c2 = read_1_byte(file); 
        *bytes_read += 1;
    } while (c2 == 0xFF);

    *marker = c1 << 8;
    *marker += c2;
}

#endif /* UTIL_H */
