#include <mram.h>
#include <mutex.h>
#include <stdio.h>

#include "jpeg-host.h"
#include "dpu-jpeg.h"

#define MCU_SIZE 64 // 8x8 bytes per plane
#define MCU_DC_OFFSET 0		// first byte of the MCU is the DC value

// bytes offsets into the MCU_cache
#define MCU_RED_OFFSET 0
#define MCU_GREEN_OFFSET 64
#define MCU_BLUE_OFFSET 128

__mram_noinit short MCU_buffer[MEGABYTE(16)];
__dma_aligned short MCU_cache[NR_TASKLETS][MCU_SIZE * 3];
short file_counter[NR_TASKLETS][128];
short dc_coeffs[NR_TASKLETS][128];

static int get_num_bits(JpegDecompressor *d, int num_bits);
static uint8_t huff_decode(JpegDecompressor *d, HuffmanTable *h_table);
static int decode_mcu(JpegDecompressor *d, uint8_t colour_idx, short *previous_dc);

static void synchronise_tasklets(JpegDecompressor *d, int row, int col, short *previous_dcs);

static void inverse_dct_component(JpegDecompressor *d, int cache_index);
static void ycbcr_to_rgb_pixel(JpegDecompressor *d, int cache_index, int v, int h);

void decode_bitstream(JpegDecompressor *d)
{
	short previous_dcs[3] = {0};
	int restart_interval = jpegInfo.restart_interval * jpegInfo.max_h_samp_factor * jpegInfo.max_v_samp_factor;
	int synch_mcu_index = 0;

	dbg_printf("[:%u] max_h_samp: %u max_v_samp: %u\n", d->tasklet_id, jpegInfo.max_h_samp_factor, jpegInfo.max_v_samp_factor);
	dbg_printf("mcu_count_v: %u mcu_count_h: %u max_h_samp: %u max_v_samp: %u\n", jpegInfo.mcu_count_v, jpegInfo.mcu_count_h, jpegInfo.max_h_samp_factor, jpegInfo.max_v_samp_factor);

	// iterate over the 2D matrix of MCUs, skipping by sampling factor
	// the MCUs must be decoded in this order, because that's how they are stored in the file
	for (int row = 0; row < jpegInfo.mcu_count_v; row += jpegInfo.max_v_samp_factor)
	{
		for (int col = 0; col < jpegInfo.mcu_count_h; col += jpegInfo.max_h_samp_factor)
		{
			// synchronize at the end of the input
			if (is_eof(d))
			{
				synchronise_tasklets(d, row, col, previous_dcs);
				return;
			}

			for (uint8_t color_index = 0; color_index < jpegInfo.num_color_components; color_index++)
			{
				for (uint8_t y = 0; y < jpegInfo.color_components[color_index].v_samp_factor; y++)
				{
					// iterate over the planes of each MCU
					for (uint8_t x = 0; x < jpegInfo.color_components[color_index].h_samp_factor; x++)
					{
						uint16_t mcu_index =
							((row + y) * jpegInfo.mcu_count_h_real // current row x row size
							+ (col + x))									// current column
							* 3												// color planes
							+ color_index;									// current plane
						uint32_t mcu_offset = mcu_index << 6;
						//dbg_printf("[%u] decoding MCU index %u (row %u)\n", d->tasklet_id, mcu_index, row + y);

						// find and decode an MCU
						while (decode_mcu(d, color_index, &previous_dcs[color_index]) != 0);

						if (synch_mcu_index < 128) {
							file_counter[d->tasklet_id][synch_mcu_index] = d->file_index + d->cache_index;
							dc_coeffs[d->tasklet_id][synch_mcu_index] = MCU_cache[d->tasklet_id][MCU_DC_OFFSET];
							synch_mcu_index++;
						}

						//dbg_printf("[%u] writing MCU index %u offset %u\n", d->tasklet_id, mcu_index, mcu_offset);
						//mram_write(MCU_cache[d->tasklet_id], &MCU_buffer[mcu_offset], MCU_SIZE * sizeof(short));
					}
				}
			}
		}
	}
}

// This relies on tasklets having decoded their first few MCUs
//
static void synchronise_tasklets(JpegDecompressor *d, int row, int col, short *previous_dcs)
{
	dbg_printf("[:%u]\n", d->tasklet_id);
	// Tasklet i has to overflow to MCUs decoded by Tasklet i + 1 for synchronisation
	// The last tasklet cannot overflow, so it returns first
	int current_mcu_index = (row * jpegInfo.mcu_count_h_real + col) * 192;
	if (current_mcu_index > 16776960 / NR_TASKLETS) {
		printf("Warning: Tasklet %d exceeded buffer size limit, output image is most likely malformed\n", d->tasklet_id);
	}

	// if this is the last tasklet, set its end index
	if (d->tasklet_id == NR_TASKLETS - 1) {
		jpegInfoDpu.mcu_end_index[d->tasklet_id] = current_mcu_index;
		return;
	}

	// The synchronisation strategy is to have Tasklet i continually decode MCU blocks until several MCU blocks
	// are decoded that match the blocks decoded by Tasklet i + 1. Matching blocks are detected through comparing
	// the file offset between the 2 tasklets
	int next_tasklet_mcu_blocks_elapsed = 0;
	int num_synched_mcu_blocks = 0;
	int minimum_synched_mcu_blocks = jpegInfo.max_h_samp_factor * jpegInfo.max_v_samp_factor + 2;

	for (; row < jpegInfo.mcu_count_v; row += jpegInfo.max_v_samp_factor) {
		for (; col < jpegInfo.mcu_count_h; col += jpegInfo.max_h_samp_factor) {
			if (num_synched_mcu_blocks >= minimum_synched_mcu_blocks + 1) {
			dbg_printf("Setting end index of %u to %u\n", d->tasklet_id, (row * jpegInfo.mcu_count_h_real + col) * 192);

				jpegInfoDpu.mcu_end_index[d->tasklet_id] = (row * jpegInfo.mcu_count_h_real + col) * 192;
				int blocks_elapsed =
						(next_tasklet_mcu_blocks_elapsed / minimum_synched_mcu_blocks) * jpegInfo.max_h_samp_factor;
				jpegInfoDpu.mcu_start_index[d->tasklet_id + 1] = blocks_elapsed * 192;

				// remove this from sync
				concat_adjust_mcus(d, row, col);
				return;
			}

			for (int color_index = 0; color_index < jpegInfo.num_color_components; color_index++) {
				for (int y = 0; y < jpegInfo.color_components[color_index].v_samp_factor; y++) {
					for (int x = 0; x < jpegInfo.color_components[color_index].h_samp_factor; x++) {
						if (decode_mcu(d, color_index, &previous_dcs[color_index]) != 0) {
							jpegInfo.valid = 0;
							printf("Error: Invalid MCU\n");
							return;
						}

						short current_tasklet_file_index = d->file_index + d->cache_index;
						short next_tasklet_file_index =
								file_counter[d->tasklet_id + 1][next_tasklet_mcu_blocks_elapsed];

						uint32_t mcu_index = (((row + y) * jpegInfo.mcu_count_h_real + (col + x)) * 3 + color_index);
						uint32_t mcu_offset = mcu_index << 6;
				//dbg_printf("[%u] writing MCU index %u offset %u\n", d->tasklet_id, mcu_index, mcu_offset);
						//mram_write(MCU_cache[d->tasklet_id], &MCU_buffer[mcu_offset], MCU_SIZE * sizeof(short));

						if (current_tasklet_file_index < next_tasklet_file_index) {
							// Tasklet i needs to decode more blocks
							num_synched_mcu_blocks = 0;
						} else if (current_tasklet_file_index > next_tasklet_file_index) {
							// More blocks needs to be elapsed for tasklet i + 1
							num_synched_mcu_blocks = 0;
							next_tasklet_mcu_blocks_elapsed++;

							while (current_tasklet_file_index > next_tasklet_file_index) {
								next_tasklet_file_index =
										file_counter[d->tasklet_id + 1][next_tasklet_mcu_blocks_elapsed];
								next_tasklet_mcu_blocks_elapsed++;
							}

							if (current_tasklet_file_index == next_tasklet_file_index) {
								jpegInfoDpu.dc_offset[d->tasklet_id][color_index] =
										MCU_cache[d->tasklet_id][0] -
										dc_coeffs[d->tasklet_id + 1][next_tasklet_mcu_blocks_elapsed];
								num_synched_mcu_blocks++;
							}
						} else {
							jpegInfoDpu.dc_offset[d->tasklet_id][color_index] =
									MCU_cache[d->tasklet_id][0] -
									dc_coeffs[d->tasklet_id + 1][next_tasklet_mcu_blocks_elapsed];

							num_synched_mcu_blocks++;
							next_tasklet_mcu_blocks_elapsed++;
						}
					}
				}
			}
		}
		col = 0;
	}
}

// The tasklets don't know their starting MCU index, so this is needed
// This is a serial task
void concat_adjust_mcus(JpegDecompressor *d, int row, int col)
{
	// Tasklet 0 does a one pass through all MCUs to adjust DC coefficients
	if (d->tasklet_id != 0) {
		return;
	}

	dbg_printf("[%u] row=%i col=%i\n", d->tasklet_id, row, col);

	int tasklet_index = 1;
	int start_index = jpegInfoDpu.mcu_start_index[tasklet_index] / 192;
	int tasklet_row = start_index / jpegInfo.mcu_count_h_real;
	int tasklet_col = start_index % jpegInfo.mcu_count_h_real;
	int dc_offset[3] = {jpegInfoDpu.dc_offset[0][0], jpegInfoDpu.dc_offset[0][1], jpegInfoDpu.dc_offset[0][2]};

	for (; row < jpegInfo.mcu_count_v; row += jpegInfo.max_v_samp_factor) {
		for (; col < jpegInfo.mcu_count_h; col += jpegInfo.max_h_samp_factor, tasklet_col += jpegInfo.max_h_samp_factor) {
		// if we hit the end of the row, go to the next row
			if (tasklet_col >= jpegInfo.mcu_count_h) {
				tasklet_col = 0;
				tasklet_row += jpegInfo.max_v_samp_factor;
			}

		// if we passed the boundary for this tasklet, go to the next one
			if ((tasklet_row * jpegInfo.mcu_count_h_real + tasklet_col) * 192 >= jpegInfoDpu.mcu_end_index[tasklet_index]) {
			dbg_printf("hit end of %u @ %u\n", tasklet_index, jpegInfoDpu.mcu_end_index[tasklet_index]);
				tasklet_index++;
				start_index = jpegInfoDpu.mcu_start_index[tasklet_index] / 192;
				tasklet_row = start_index / jpegInfo.mcu_count_h_real;
				tasklet_col = start_index % jpegInfo.mcu_count_h_real;
				dc_offset[0] += jpegInfoDpu.dc_offset[tasklet_index - 1][0];
				dc_offset[1] += jpegInfoDpu.dc_offset[tasklet_index - 1][1];
				dc_offset[2] += jpegInfoDpu.dc_offset[tasklet_index - 1][2];
			}

			for (int color_index = 0; color_index < jpegInfo.num_color_components; color_index++) {
				for (int y = 0; y < jpegInfo.color_components[color_index].v_samp_factor; y++) {
					for (int x = 0; x < jpegInfo.color_components[color_index].h_samp_factor; x++) {
						uint32_t mcu_index = (((tasklet_row + y) * jpegInfo.mcu_count_h_real + (tasklet_col + x)) * 3 + color_index);
						uint32_t mcu_offset = mcu_index << 6;
				dbg_printf("[%u] reading MCU index %u offset %u tindex %u\n", d->tasklet_id, mcu_index, mcu_offset, tasklet_index);
				if (((uint32_t)&MCU_buffer[mcu_offset] + MCU_SIZE * sizeof(short)) > ((uint32_t)MCU_buffer + sizeof(MCU_buffer)))
				{
					dbg_printf("Error: offset 0x%x MCU_buffer: 0x%x-0x%x\n",
						((uint32_t)&MCU_buffer[mcu_offset] + MCU_SIZE),
						(uint32_t)MCU_buffer, ((uint32_t)MCU_buffer + sizeof(MCU_buffer)));
					return;
				}
				dbg_printf("Reading 0x%x to 0x%x size %u\n", (uint32_t)&MCU_buffer[mcu_offset], (uint32_t)MCU_cache[0], MCU_SIZE);
						//mram_read(&MCU_buffer[mcu_offset], MCU_cache[0], MCU_SIZE * sizeof(short));

						MCU_cache[0][0] += dc_offset[color_index];

						mcu_offset = (((row + y) * jpegInfo.mcu_count_h_real + (col + x)) * 3 + color_index) << 6;
				//dbg_printf("[%u] writing MCU index %u offset %u\n", d->tasklet_id, mcu_index, mcu_offset);
						//mram_write(MCU_cache[0], &MCU_buffer[mcu_offset], MCU_SIZE * sizeof(short));
					}
				}
			}
		}
		col = 0;
	}
}

/*
	Decode a single plane of an MCU
*/
static int decode_mcu(JpegDecompressor *d, uint8_t colour_idx, short *previous_dc)
{
	QuantizationTable *q_table;
	HuffmanTable *dc_table;
	HuffmanTable *ac_table;
	uint8_t index; // quantization table or huffman table index

	index = jpegInfo.color_components[colour_idx].quant_table_id;
	if (index >= MAX_QUANT_TABLES)
	{
		printf("Invalid quantization table index: %u\n", index);
		return -2;
	}

	q_table = &jpegInfo.quant_tables[index];

	index = jpegInfo.color_components[colour_idx].dc_huffman_table_id;
	if (index >= MAX_HUFFMAN_TABLES)
	{
		printf("Invalid dc huffman table index: %u\n", index);
		return -3;
	}

	dc_table = &jpegInfo.dc_huffman_tables[index];

	index = jpegInfo.color_components[colour_idx].ac_huffman_table_id;
	if (index >= MAX_HUFFMAN_TABLES)
	{
		printf("Invalid ac huffman table index: %u\n", index);
		return -3;
	}

	ac_table = &jpegInfo.ac_huffman_tables[index];

	// Get DC value for this MCU block
	uint8_t dc_length = huff_decode(d, dc_table);
	if (dc_length == (uint8_t) -1) {
		printf("Error: Invalid DC code\n");
		return -1;
	}
	if (dc_length > 11) {
		printf("Error: DC coefficient length greater than 11\n");
		return -1;
	}

	int coeff = get_num_bits(d, dc_length);
	if (dc_length != 0 && coeff < (1 << (dc_length - 1))) {
		// Convert to negative coefficient
		coeff -= (1 << dc_length) - 1;
	}

	MCU_cache[d->tasklet_id][MCU_DC_OFFSET] = coeff + *previous_dc;
	*previous_dc = MCU_cache[d->tasklet_id][MCU_DC_OFFSET];
	// Dequantization
	MCU_cache[d->tasklet_id][MCU_DC_OFFSET] *= q_table->table[MCU_DC_OFFSET];

	// Get the AC values for this MCU block
	int i = 1;
	while (i < MCU_SIZE) {
		uint8_t ac_length = huff_decode(d, ac_table);
		if (ac_length == (uint8_t) -1) {
			printf("Error: Invalid AC code\n");
			return -1;
		}

		// Got 0x00, fill remaining MCU block with 0s
		if (ac_length == 0x00) {
			while (i < MCU_SIZE) {
				MCU_cache[d->tasklet_id][ZIGZAG_ORDER[i++]] = 0;
			}
			break;
		}

		// MCU block not entirely filled yet, continue reading
		uint8_t num_zeroes = (ac_length >> 4) & 0x0F;
		uint8_t coeff_length = ac_length & 0x0F;
		coeff = 0;

		// Got 0xF0, skip 16 0s
		if (ac_length == 0xF0) {
			num_zeroes = 16;
		}

		if (i + num_zeroes >= 64) {
			printf("Error: Invalid AC code - zeros exceeded MCU length %d >= 64\n", i + num_zeroes);
			return -1;
		}
		for (int j = 0; j < num_zeroes; j++) {
			MCU_cache[d->tasklet_id][ZIGZAG_ORDER[i++]] = 0;
		}

		if (coeff_length > 10) {
			printf("Error: AC coefficient length greater than 10\n");
			return -1;
		}
		if (coeff_length != 0) {
			coeff = get_num_bits(d, coeff_length);
			if (coeff < (1 << (coeff_length - 1))) {
				// Convert to negative coefficient
				coeff -= (1 << coeff_length) - 1;
			}
			// Write coefficient to buffer as well as perform dequantization
			MCU_cache[d->tasklet_id][ZIGZAG_ORDER[i]] = coeff * q_table->table[ZIGZAG_ORDER[i]];
			i++;
		}
	}

	return 0;
}

static uint8_t huff_decode(JpegDecompressor *d, HuffmanTable *h_table) {
	uint32_t code = 0;

	for (int i = 0; i < 16; i++) {
		int bit = get_num_bits(d, 1);
		code = (code << 1) | bit;
		for (int j = h_table->valoffset[i]; j < h_table->valoffset[i + 1]; j++) {
			if (code == h_table->codes[j]) {
				return h_table->huffval[j];
			}
		}
	}

	return -1;
}

static int get_num_bits(JpegDecompressor *d, int num_bits) {
	int bits = 0;
	if (num_bits == 0) {
		return bits;
	}

	uint8_t temp_byte;
	uint32_t actual_byte;
	while (d->bits_left < num_bits) {
		// Read a byte and decode it, if it is 0xFF
		temp_byte = read_byte(d);
		actual_byte = temp_byte;

		while (temp_byte == 0xFF) {
			// FF may be padded with FFs, read as many FFs as necessary
			temp_byte = read_byte(d);
			if (temp_byte == 0) {
				// Got FF which is not a marker, save it to buffer
				actual_byte = 0xFF;
			} else if (temp_byte >= M_RST_FIRST && temp_byte <= M_RST_LAST) {
				// Got restart markers, ignore and read new byte
				temp_byte = read_byte(d);
				actual_byte = temp_byte;
			} else {
				actual_byte = temp_byte;
			}
		}

		// Add the new bits to the buffer (MSB aligned)
		d->bit_buffer |= actual_byte << (32 - 8 - d->bits_left);
		d->bits_left += 8;
	}

	bits = d->bit_buffer >> (32 - num_bits);
	d->bit_buffer <<= num_bits;
	d->bits_left -= num_bits;
	return bits;
}

void inverse_dct_convert(JpegDecompressor *d)
{
	uint16_t row = jpegInfoDpu.rows_per_tasklet * d->tasklet_id;
	uint16_t end_row = jpegInfoDpu.rows_per_tasklet * (d->tasklet_id + 1);

	if (row & 1)
		row++;

	if (end_row & 1)
		end_row++;

	// if this is the last tasklet, process until the end
	if (d->tasklet_id == NR_TASKLETS - 1)
		end_row = jpegInfo.mcu_count_v;

	//dbg_printf("[%u] rows %i - %i\n", d->tasklet_id, row, end_row);

	for (; row < end_row; row += jpegInfo.max_v_samp_factor)
	{
		for (uint16_t col = 0; col < jpegInfo.mcu_count_h; col += jpegInfo.max_h_samp_factor)
		{
			for (uint8_t color_index = 0; color_index < jpegInfo.num_color_components; color_index++)
			{
				for (uint8_t y = 0; y < jpegInfo.color_components[color_index].v_samp_factor; y++)
				{
					for (uint8_t x = 0; x < jpegInfo.color_components[color_index].h_samp_factor; x++)
					{
						uint32_t mcu_index = (((row + y) * jpegInfo.mcu_count_h_real + (col + x)) * 3 + color_index);
						uint32_t mcu_offset = mcu_index << 6;
						//dbg_printf("[%u] reading MCU index %u offset %u\n", d->tasklet_id, mcu_index, mcu_offset);

						int cache_index = ((y << 8) + (y << 7)) // 384y
							+ ((x << 7) + (x << 6))					// 192x
							+ (color_index << 6);
						//mram_read(&MCU_buffer[mcu_offset], &MCU_cache[d->tasklet_id][cache_index], MCU_SIZE * sizeof(short));

						// Compute inverse DCT with ANN algorithm
						inverse_dct_component(d, cache_index);
					}
				}
			}

			// Convert from YCbCr to RGB
			for (int y = jpegInfo.max_v_samp_factor - 1; y >= 0; y--)
			{
				for (int x = jpegInfo.max_h_samp_factor - 1; x >= 0; x--)
				{
					// MCU to index is 
					uint32_t mcu3_index = (((row + y)	// (current row + vertical sampling) 
						* jpegInfo.mcu_count_h_real  	// x total number of MCUs in a row of the JPEG
						+ (col + x)) * 3);				//+ (current col + horizontal sampling)

					uint32_t mcu3_offset = mcu3_index << 6;
					uint32_t cache_index = ((y << 8) + (y << 7)) + ((x << 7) + (x << 6)); // 384y + 192x
					//uint32_t cache_index = ((y << 7) + (y << 6)) + ((x << 6) + (x << 5)); // 192y + 96x

					ycbcr_to_rgb_pixel(d, cache_index, y, x);

			//	dbg_printf("[%u] writing MCU index %u offset %u\n", d->tasklet_id, mcu_index, mcu_offset);
					//mram_write(&MCU_cache[d->tasklet_id][cache_index], &MCU_buffer[mcu3_offset], MCU_SIZE * sizeof(short) * 3);
				}
			}
		}
	}
}

static void inverse_dct_component(JpegDecompressor *d, int cache_index) {
	// ANN algorithm, intermediate values are bit shifted to the left to preserve precision
	// and then bit shifted to the right at the end
	for (int i = 0; i < 8; i++) {
		// Higher accuracy
		int g0 = (MCU_cache[d->tasklet_id][cache_index + (0 << 3) + i] * 181) >> 5;
		int g1 = (MCU_cache[d->tasklet_id][cache_index + (4 << 3) + i] * 181) >> 5;
		int g2 = (MCU_cache[d->tasklet_id][cache_index + (2 << 3) + i] * 59) >> 3;
		int g3 = (MCU_cache[d->tasklet_id][cache_index + (6 << 3) + i] * 49) >> 4;
		int g4 = (MCU_cache[d->tasklet_id][cache_index + (5 << 3) + i] * 71) >> 4;
		int g5 = (MCU_cache[d->tasklet_id][cache_index + (1 << 3) + i] * 251) >> 5;
		int g6 = (MCU_cache[d->tasklet_id][cache_index + (7 << 3) + i] * 25) >> 4;
		int g7 = (MCU_cache[d->tasklet_id][cache_index + (3 << 3) + i] * 213) >> 5;

		// Lower accuracy
		// int g0 = (MCU_cache[d->tasklet_id][cache_index + (0 << 3) + i] * 22) >> 2;
		// int g1 = (MCU_cache[d->tasklet_id][cache_index + (4 << 3) + i] * 22) >> 2;
		// int g2 = (MCU_cache[d->tasklet_id][cache_index + (2 << 3) + i] * 30) >> 2;
		// int g3 = (MCU_cache[d->tasklet_id][cache_index + (6 << 3) + i] * 12) >> 2;
		// int g4 = (MCU_cache[d->tasklet_id][cache_index + (5 << 3) + i] * 18) >> 2;
		// int g5 = (MCU_cache[d->tasklet_id][cache_index + (1 << 3) + i] * 31) >> 2;
		// int g6 = (MCU_cache[d->tasklet_id][cache_index + (7 << 3) + i] * 6) >> 2;
		// int g7 = (MCU_cache[d->tasklet_id][cache_index + (3 << 3) + i] * 27) >> 2;

		int f4 = g4 - g7;
		int f5 = g5 + g6;
		int f6 = g5 - g6;
		int f7 = g4 + g7;

		int e2 = g2 - g3;
		int e3 = g2 + g3;
		int e5 = f5 - f7;
		int e7 = f5 + f7;
		int e8 = f4 + f6;

		// Higher accuracy
		int d2 = (e2 * 181) >> 7;
		int d4 = (f4 * 277) >> 8;
		int d5 = (e5 * 181) >> 7;
		int d6 = (f6 * 669) >> 8;
		int d8 = (e8 * 49) >> 6;

		// Lower accuracy
		// int d2 = (e2 * 90) >> 6;
		// int d4 = (f4 * 69) >> 6;
		// int d5 = (e5 * 90) >> 6;
		// int d6 = (f6 * 167) >> 6;
		// int d8 = (e8 * 49) >> 6;

		int c0 = g0 + g1;
		int c1 = g0 - g1;
		int c2 = d2 - e3;
		int c4 = d4 + d8;
		int c5 = d5 + e7;
		int c6 = d6 - d8;
		int c8 = c5 - c6;

		int b0 = c0 + e3;
		int b1 = c1 + c2;
		int b2 = c1 - c2;
		int b3 = c0 - e3;
		int b4 = c4 - c8;
		int b6 = c6 - e7;

		MCU_cache[d->tasklet_id][cache_index + (0 << 3) + i] = (b0 + e7) >> 4;
		MCU_cache[d->tasklet_id][cache_index + (1 << 3) + i] = (b1 + b6) >> 4;
		MCU_cache[d->tasklet_id][cache_index + (2 << 3) + i] = (b2 + c8) >> 4;
		MCU_cache[d->tasklet_id][cache_index + (3 << 3) + i] = (b3 + b4) >> 4;
		MCU_cache[d->tasklet_id][cache_index + (4 << 3) + i] = (b3 - b4) >> 4;
		MCU_cache[d->tasklet_id][cache_index + (5 << 3) + i] = (b2 - c8) >> 4;
		MCU_cache[d->tasklet_id][cache_index + (6 << 3) + i] = (b1 - b6) >> 4;
		MCU_cache[d->tasklet_id][cache_index + (7 << 3) + i] = (b0 - e7) >> 4;
	}

	for (int i = 0; i < 8; i++) {
		// Higher accuracy
		int g0 = (MCU_cache[d->tasklet_id][cache_index + (i << 3) + 0] * 181) >> 5;
		int g1 = (MCU_cache[d->tasklet_id][cache_index + (i << 3) + 4] * 181) >> 5;
		int g2 = (MCU_cache[d->tasklet_id][cache_index + (i << 3) + 2] * 59) >> 3;
		int g3 = (MCU_cache[d->tasklet_id][cache_index + (i << 3) + 6] * 49) >> 4;
		int g4 = (MCU_cache[d->tasklet_id][cache_index + (i << 3) + 5] * 71) >> 4;
		int g5 = (MCU_cache[d->tasklet_id][cache_index + (i << 3) + 1] * 251) >> 5;
		int g6 = (MCU_cache[d->tasklet_id][cache_index + (i << 3) + 7] * 25) >> 4;
		int g7 = (MCU_cache[d->tasklet_id][cache_index + (i << 3) + 3] * 213) >> 5;

		// Lower accuracy
		// int g0 = (MCU_cache[d->tasklet_id][cache_index + (i << 3) + 0] * 22) >> 2;
		// int g1 = (MCU_cache[d->tasklet_id][cache_index + (i << 3) + 4] * 22) >> 2;
		// int g2 = (MCU_cache[d->tasklet_id][cache_index + (i << 3) + 2] * 30) >> 2;
		// int g3 = (MCU_cache[d->tasklet_id][cache_index + (i << 3) + 6] * 12) >> 2;
		// int g4 = (MCU_cache[d->tasklet_id][cache_index + (i << 3) + 5] * 18) >> 2;
		// int g5 = (MCU_cache[d->tasklet_id][cache_index + (i << 3) + 1] * 31) >> 2;
		// int g6 = (MCU_cache[d->tasklet_id][cache_index + (i << 3) + 7] * 6) >> 2;
		// int g7 = (MCU_cache[d->tasklet_id][cache_index + (i << 3) + 3] * 27) >> 2;

		int f4 = g4 - g7;
		int f5 = g5 + g6;
		int f6 = g5 - g6;
		int f7 = g4 + g7;

		int e2 = g2 - g3;
		int e3 = g2 + g3;
		int e5 = f5 - f7;
		int e7 = f5 + f7;
		int e8 = f4 + f6;

		// Higher accuracy
		int d2 = (e2 * 181) >> 7;
		int d4 = (f4 * 277) >> 8;
		int d5 = (e5 * 181) >> 7;
		int d6 = (f6 * 669) >> 8;
		int d8 = (e8 * 49) >> 6;

		// Lower accuracy
		// int d2 = (e2 * 90) >> 6;
		// int d4 = (f4 * 69) >> 6;
		// int d5 = (e5 * 90) >> 6;
		// int d6 = (f6 * 167) >> 6;
		// int d8 = (e8 * 49) >> 6;

		int c0 = g0 + g1;
		int c1 = g0 - g1;
		int c2 = d2 - e3;
		int c4 = d4 + d8;
		int c5 = d5 + e7;
		int c6 = d6 - d8;
		int c8 = c5 - c6;

		int b0 = c0 + e3;
		int b1 = c1 + c2;
		int b2 = c1 - c2;
		int b3 = c0 - e3;
		int b4 = c4 - c8;
		int b6 = c6 - e7;

		MCU_cache[d->tasklet_id][cache_index + (i << 3) + 0] = (b0 + e7) >> 4;
		MCU_cache[d->tasklet_id][cache_index + (i << 3) + 1] = (b1 + b6) >> 4;
		MCU_cache[d->tasklet_id][cache_index + (i << 3) + 2] = (b2 + c8) >> 4;
		MCU_cache[d->tasklet_id][cache_index + (i << 3) + 3] = (b3 + b4) >> 4;
		MCU_cache[d->tasklet_id][cache_index + (i << 3) + 4] = (b3 - b4) >> 4;
		MCU_cache[d->tasklet_id][cache_index + (i << 3) + 5] = (b2 - c8) >> 4;
		MCU_cache[d->tasklet_id][cache_index + (i << 3) + 6] = (b1 - b6) >> 4;
		MCU_cache[d->tasklet_id][cache_index + (i << 3) + 7] = (b0 - e7) >> 4;
	}
}

// https://en.wikipedia.org/wiki/YUV Y'UV444 to RGB888 conversion
static void ycbcr_to_rgb_pixel(JpegDecompressor *d, int cache_index, int v, int h)
{
	int max_v = jpegInfo.max_v_samp_factor;
	int max_h = jpegInfo.max_h_samp_factor;

	// Iterating from bottom right to top left because otherwise the pixel data will get overwritten
	for (int y = 7; y >= 0; y--)
	{
		for (int x = 7; x >= 0; x--)
		{
			uint8_t pixel = cache_index + (y << 3) + x; // pixel offset 0-63 in this color plane of the MCU
			int cbcr_pixel_row = y / max_v + 4 * v;
			int cbcr_pixel_col = x / max_h + 4 * h;
			int cbcr_pixel = (cbcr_pixel_row << 3) + cbcr_pixel_col + 64;

			short r =
					MCU_cache[d->tasklet_id][pixel] + ((45 * MCU_cache[d->tasklet_id][64 + cbcr_pixel]) >> 5) + 128;
			short g =
					MCU_cache[d->tasklet_id][pixel] -
					((11 * MCU_cache[d->tasklet_id][cbcr_pixel] + 23 * MCU_cache[d->tasklet_id][64 + cbcr_pixel]) >>
					 5) +
					128;
			short b =
					MCU_cache[d->tasklet_id][pixel] + ((113 * MCU_cache[d->tasklet_id][cbcr_pixel]) >> 6) + 128;

			if (r < 0)
				r = 0;
			if (r > 255)
				r = 255;
			if (g < 0)
				g = 0;
			if (g > 255)
				g = 255;
			if (b < 0)
				b = 0;
			if (b > 255)
				b = 255;

			MCU_cache[d->tasklet_id][MCU_RED_OFFSET + pixel] = r;
			MCU_cache[d->tasklet_id][MCU_GREEN_OFFSET + pixel] = g;
			MCU_cache[d->tasklet_id][MCU_BLUE_OFFSET + pixel] = b;
		}
	}
}


MUTEX_INIT(sum_rgb_lock);

void find_sum_rgb(JpegDecompressor *d) {
	int row = jpegInfoDpu.rows_per_tasklet * d->tasklet_id;
	int end_row = jpegInfoDpu.rows_per_tasklet * (d->tasklet_id + 1);
	if (d->tasklet_id == NR_TASKLETS - 1) {
		end_row = jpegInfo.mcu_count_v;
	}

	uint32_t sum_rgb[3] = {0, 0, 0};

	for (; row < end_row; row++) {
		for (int col = 0; col < jpegInfo.mcu_count_h_real; col++) {
			uint32_t mcu_index = ((row * jpegInfo.mcu_count_h_real + col) * 3);
		uint32_t mcu_offset = mcu_index << 6;
		//dbg_printf("[%u] reading MCU index %u offset %u\n", d->tasklet_id, mcu_index, mcu_offset);
			//mram_read(&MCU_buffer[mcu_offset], &MCU_cache[d->tasklet_id][0], MCU_SIZE * sizeof(short) * 3);
			for (int color_index = 0; color_index < jpegInfo.num_color_components; color_index++) {
				for (int i = 0; i < 64; i++) {
					sum_rgb[color_index] += MCU_cache[d->tasklet_id][(color_index << 6) + i];
				}
			}
		}
	}

	mutex_lock(sum_rgb_lock);
	for (int color_index = 0; color_index < jpegInfo.num_color_components; color_index++) {
		jpegInfoDpu.sum_rgb[color_index] += sum_rgb[color_index];
	}
	mutex_unlock(sum_rgb_lock);
}
