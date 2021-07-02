#include "bmp.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static char *form_bmp_filename(const char *filename) {
  char *filename_copy = (char *) malloc(sizeof(char) * (strlen(filename) + 9));
  strcpy(filename_copy, filename);
  char *period_ptr = strrchr(filename_copy, '.');
  if (period_ptr == NULL) {
    strcpy(filename_copy + strlen(filename_copy), "-dpu.bmp");
  } else {
    strcpy(period_ptr, "-dpu.bmp");
  }

  return filename_copy;
}

static void initialize_window_info_header(BmpObject *image, uint32_t image_width, uint32_t image_height) {
  image->win_header.width = image_width;
  image->win_header.height = image_height;

  image->win_header.size = sizeof(WindowsInfoheader);
  image->win_header.planes = 1;
  image->win_header.bits_per_pixel = 24;
  image->win_header.compression = BI_RGB;
  image->win_header.length =
      image->win_header.width * image->win_header.height * (image->win_header.bits_per_pixel >> 3);
  image->win_header.hres = 1;
  image->win_header.vres = 1;
  image->win_header.palette = 0;
  image->win_header.important = 0;
}

static void initialize_bmp_header(BmpObject *image) {
  image->header.magic[0] = 'B';
  image->header.magic[1] = 'M';
  image->header.data = sizeof(BmpHeader) + sizeof(WindowsInfoheader);
  image->header.size = image->header.data + image->win_header.length;
}

static void initialize_bmp_body(BmpObject *image, uint32_t image_padding, uint32_t mcu_width, short *MCU_buffer) {
  uint8_t *ptr = (uint8_t *) malloc(image->win_header.height * (image->win_header.width * 3 + image_padding));
  image->data = ptr;

  for (int y = image->win_header.height - 1; y >= 0; y--) {
    uint32_t mcu_row = y / 8;
    uint32_t pixel_row = y % 8;

    for (int x = 0; x < image->win_header.width; x++) {
      uint32_t mcu_column = x / 8;
      uint32_t pixel_column = x % 8;
      uint32_t mcu_index = mcu_row * mcu_width + mcu_column;
      uint32_t pixel_index = pixel_row * 8 + pixel_column;
      ptr[0] = MCU_buffer[(mcu_index * 3 + 2) * 64 + pixel_index];
      ptr[1] = MCU_buffer[(mcu_index * 3 + 1) * 64 + pixel_index];
      ptr[2] = MCU_buffer[(mcu_index * 3 + 0) * 64 + pixel_index];
      ptr += 3;
    }

    for (uint32_t i = 0; i < image_padding; i++) {
      ptr[0] = 0;
      ptr++;
    }
  }
}

int write_bmp_to_file(const char *filename, BmpObject *picture) {
  FILE *output;

  output = fopen(filename, "wb");
  if (!output) {
    return -1;
  }

  fwrite(&picture->header, sizeof(BmpHeader), 1, output);
  fwrite(&picture->win_header, sizeof(WindowsInfoheader), 1, output);
  fwrite(picture->data, 1, picture->win_header.length, output);

  fclose(output);

  return 0;
}

int write_bmp(const char *filename, uint32_t image_width, uint32_t image_height, uint32_t image_padding,
              uint32_t mcu_width, short *MCU_buffer) {
  BmpObject image;

  initialize_window_info_header(&image, image_width, image_height);
  initialize_bmp_header(&image);
  initialize_bmp_body(&image, image_padding, mcu_width, MCU_buffer);

  char *filename_dpu = form_bmp_filename(filename);

  int result = write_bmp_to_file(filename_dpu, &image);
  free(image.data);
  free(filename_dpu);

  return result;
}

/*
int read_bmp(const char *filename, BmpObject *picture) {
  FILE *infile;

  infile = fopen(filename, "r");
  if (!infile) {
    return -1;
  }

  fread(&picture->header, sizeof(BmpHeader), 1, infile);
  if (memcmp(picture->header.magic, "BM", 2)) {
    printf("This is not a BMP!\n");
    return -1;
  }

  printf("Magic: %c%c\n", picture->header.magic[0], picture->header.magic[1]);
  printf("File size: %u\n", picture->header.size);
  printf("data offset: %u\n", picture->header.data);

  fread(&picture->win_header, sizeof(WindowsInfoheader), 1, infile);
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

int main(int argc, char **argv)
{
        uint32_t row, col;
        BmpObject obj;
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
