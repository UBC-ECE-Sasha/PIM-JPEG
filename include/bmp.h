#ifndef _BMP__H
#define _BMP__H

#include <stdint.h>

enum {
	BI_RGB,
	BI_RLE8,
	BI_RLE4,
	BI_BITFIELDS,
	BI_JPEG,
	BI_PNG,
	BI_ALPHABITFIELDS,
	BI_CMYK,
	BI_CMYKRLE8,
	BI_CMYKRLE4
};

typedef struct  __attribute__((packed)) bmp_header
{
	uint8_t magic[2]; // BM
	uint32_t size;	// size of file in bytes
	uint16_t reserved1;
	uint16_t reserved2;
	uint32_t data; // offset of pixel data
} bmp_header ;

typedef struct  __attribute__((packed)) windows_infoheader
{
	uint32_t size;
	int32_t width;
	int32_t height;
	uint16_t planes;
	uint16_t bits_per_pixel;
	uint32_t compression;
	uint32_t length;
	int32_t hres;
	int32_t vres;
	uint32_t palette;
	uint32_t important;
} windows_infoheader;

typedef struct bmp_object
{
	bmp_header header;
	windows_infoheader win_header;
	uint8_t *data;
} bmp_object;

int write_bmp(const char *filename, bmp_object *picture);

#endif // _BMP__H
