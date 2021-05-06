#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "bmp.h"

int write_bmp(const char *filename, bmp_object *picture)
{
	FILE *output;

	output = fopen(filename, "wb");
	if (!output)
	{
		return -1;
	}

	picture->win_header.size = sizeof(windows_infoheader);
	picture->win_header.planes = 1;
	picture->win_header.bits_per_pixel = 24;
	picture->win_header.compression = BI_RGB;
	picture->win_header.length = picture->win_header.width * picture->win_header.height * (picture->win_header.bits_per_pixel >> 3);
	picture->win_header.hres = 1;
	picture->win_header.vres = 1;
	picture->win_header.palette = 0;
	picture->win_header.important = 0;

	picture->header.magic[0] = 'B';
	picture->header.magic[1] = 'M';
	picture->header.data = sizeof(bmp_header) + sizeof(windows_infoheader);
	picture->header.size = picture->header.data + picture->win_header.length;

	fwrite(&picture->header, sizeof(bmp_header), 1, output);
	fwrite(&picture->win_header, sizeof(windows_infoheader), 1, output);
	fwrite(picture->data, 1, picture->win_header.length, output);

	fclose(output);

	return 0;
}

int read_bmp(const char *filename, bmp_object *picture)
{
	FILE *infile;

	infile = fopen(filename, "r");
	if (!infile)
	{
		return -1;
	}

	fread(&picture->header, sizeof(bmp_header), 1, infile);
	if (memcmp(picture->header.magic, "BM", 2))
	{
		printf("This is not a BMP!\n");
		return -1;
	}

	printf("Magic: %c%c\n", picture->header.magic[0], picture->header.magic[1]);
	printf("File size: %u\n", picture->header.size);
	printf("data offset: %u\n", picture->header.data);

	fread(&picture->win_header, sizeof(windows_infoheader), 1, infile);
	printf("header size: %u\n", picture->win_header.size);
	printf("width: %u\n", picture->win_header.width);
	printf("height: %u\n", picture->win_header.height);
	printf("planes: %u\n", picture->win_header.planes);
	printf("bits per pixel: %u\n", picture->win_header.bits_per_pixel);
	printf("compression: %u\n", picture->win_header.compression);

	// figure out how much data should be in each line
	uint32_t line_data = picture->win_header.width * (picture->win_header.bits_per_pixel >> 3);
	printf("bytes per line: %u\n", line_data);

	fseek(infile, picture->header.data, SEEK_SET);
	uint32_t data_size = picture->header.size - picture->header.data;
	printf("data size: %u\n", data_size);
	picture->data = malloc(data_size);
	fread(picture->data, 1, data_size, infile);

	fclose(infile);

	return 0;
}

/*
int main(int argc, char **argv)
{
	uint32_t row, col;
	bmp_object obj;
	uint8_t *new_data;
	uint32_t row_size, new_row_size, new_width;
	uint32_t new_height;

	read_bmp("test.bmp", &obj);
	row_size = obj.win_header.width * (obj.win_header.bits_per_pixel >> 3);

	new_width = obj.win_header.width / 2;
	new_height = obj.win_header.height / 2;
	new_row_size = new_width * (obj.win_header.bits_per_pixel >> 3);
	new_data = malloc(obj.win_header.height * new_row_size);

	// half in x dimensions
	for (row = 0; row < new_height; row++)
	{
		for (col = 0; col < new_width; col++)
		{
			new_data[row * new_row_size + (col * 3) + 0] = obj.data[row * 2 * row_size + (col * 2 * 3) + 0];
			new_data[row * new_row_size + (col * 3) + 1] = obj.data[row * 2 * row_size + (col * 2 * 3) + 1];
			new_data[row * new_row_size + (col * 3) + 2] = obj.data[row * 2 * row_size + (col * 2 * 3) + 2];
		}
	}
	obj.win_header.width = new_width;
	obj.win_header.height = new_height;
	obj.data = new_data;

	write_bmp("half.bmp", &obj);
	free(obj.data);
	return 0;
}
*/
