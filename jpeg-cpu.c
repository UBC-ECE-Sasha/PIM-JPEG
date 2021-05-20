#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <byteswap.h>
#include "bmp.h"
#include "jpeg-cpu.h"

#define MAX_COMP_INFO 3
#define DCTSIZE 8
#define MAX_COMPS_IN_SCAN 4
#define MAX_SAMP_FACTOR 4
#define MAX(_a, _b) (_a > _b ? _a : _b)

#define MIN_GET_BITS 15

#define HUFFMAN_TABLE_CLASS_DC 0
#define HUFFMAN_TABLE_CLASS_AC 1

#define rounded_division(_a, _b) (((_a) + (_b)-1) / (_b))

/* We want to emulate the behaviour of 'tjbench <jpg> -scale 1/8'
	That calls 'process_data_simple_main' and 'decompress_onepass' in turbojpeg
	On my laptop, I see:

./tjbench ../DSCF5148.JPG -scale 1/8

>>>>>  JPEG 4:2:2 --> BGR (Top-down)  <<<<<

Image size: 1920 x 1080 --> 240 x 135
Decompress    --> Frame rate:         92.177865 fps
                  Throughput:         191.140021 Megapixels/sec
*/

/* Markers: CCITT Rec T.81 page 32 */
enum markers
{
	/* Start Of Frame non-differential, Huffman coding */
	M_SOF0 = 0xC0,
	M_SOF1,
	M_SOF2,
	M_SOF3,

	M_DHT = 0xC4, /* Define Huffman Tables */

	/* Start Of Frame differential, Huffman coding */
	M_SOF5 = 0xC5,
	M_SOF6,
	M_SOF7,

	/* Start Of Frame non-differential, arithmetic coding */
	M_JPG = 0xC8,
	M_SOF9,
	M_SOF10,
	M_SOF11,

	M_DAC = 0xCC, /* Define Arithmetic Coding */

	/* Start Of Frame differential, arithmetic coding */
	M_SOF13, /* Differential sequential DCT */
	M_SOF14, /* Differential progressive DCT */
	M_SOF15, /* Differential lossless */

	M_RST_FIRST = 0xD0, /* Restart interval termination */
	M_RST_LAST = 0xD7,	/* Restart interval termination */

	M_SOI = 0xD8, /* Start Of Image (beginning of datastream) */
	M_EOI,		  /* End Of Image (end of datastream) */
	M_SOS,		  /* Start Of Scan (begins compressed data) */
	M_DQT,		  /* Define quantization table */
	M_DNL,
	M_DRI,
	M_DHP,
	M_EXP,
	M_APP_FIRST = 0xE0, /* Application-specific marker, type N */
	M_APP_LAST = 0xEF,
	M_EXT_FIRST = 0xF0,
	M_EXT_LAST = 0xFD,
	M_COM /* COMment */
};

/* Error exit handler */
#define ERREXIT(msg) return -1;

typedef struct jpeg_component_info
{
	uint8_t component_id;
	uint8_t component_index;
	uint8_t h_samp_factor; // also MCU_width
	uint8_t v_samp_factor; // also MCU_height
	uint8_t quant_tbl_no;
	uint8_t dc_tbl_no;
	uint8_t ac_tbl_no;
	uint16_t width_in_blocks;
	uint16_t height_in_blocks;
} jpeg_component_info;

typedef struct huffman_table
{
	uint8_t table_class; // see HUFFMAN_TABLE_CLASS_
	uint8_t dest;
	uint8_t bits[16];	  // BITS: gives the number of codes of each length (1-16)
	uint8_t huffval[256]; // HUFFVAL: actually sum(length[0] .. length[15])

	uint32_t valoffset[18]; // offset into huffval for codes of length k
	uint32_t maxcode[18];	// largest code of length k
} huffman_table;

typedef struct jpeg_decompressor
{
	char *ptr;
	char *data;
	uint64_t length;
	uint8_t data_precision;
	uint16_t image_height;
	uint16_t image_width;
	uint8_t num_components;
	uint16_t restart_interval;
	uint16_t restarts_left;
	uint32_t num_restart_intervals;
	uint32_t max_h_samp_factor;
	uint32_t max_v_samp_factor;
	jpeg_component_info comp_info[MAX_COMPS_IN_SCAN];
	huffman_table huffman[4];
	uint32_t num_huffman_tables;

	/* updated each scan */
	uint8_t first_scan;
	uint8_t comps_in_scan;
	uint8_t component_sel;
	uint8_t coding_table;
	uint8_t ss;
	uint8_t se;
	uint8_t Ah;
	uint8_t Al;
	jpeg_component_info cur_comp_info[MAX_COMP_INFO];
	uint8_t MCUs_per_row;
	uint32_t rows_per_scan;
	uint8_t blocks_per_MCU;

	/* bit buffer */
	uint32_t get_buffer;
	uint8_t bits_left;
} jpeg_decompressor;

/**
 * Check whether EOF is reached by comparing current ptr location to start location + entire length of file
 */
static int eof(jpeg_decompressor *d)
{
	return (d->ptr >= d->data + d->length);
}

/**
 * Read 1 byte from the file
 */
static uint8_t read_byte(jpeg_decompressor *d)
{
	uint8_t temp;

	temp = (*d->ptr);
	d->ptr++;
	//printf("%s: 0x%02x\n", __func__, temp);
	return temp;
}

/**
 * Read 2 bytes from the file, MSB order
 */
static uint16_t read_short(jpeg_decompressor *d)
{
	uint16_t temp3;
	uint8_t temp1, temp2;

	temp1 = *d->ptr;
	d->ptr++;
	temp2 = *d->ptr;
	d->ptr++;

	temp3 = (temp1 << 8) | temp2;
	//printf("%s: 0x%04x\n", __func__, temp3);
	return temp3;
}

/**
 * Skip count bytes
 */
static void skip_bytes(jpeg_decompressor *d, int count)
{
	//printf("Skipping %i bytes\n", count);

	/* If after skipping count bytes we go beyond EOF, then only skip till EOF */
	if (d->ptr + count > d->data + d->length)
		d->ptr = d->data + d->length;
	else
		d->ptr += count;
}

/**
 * Skip over an unknown or uninteresting variable-length marker
 */
static int skip_variable(jpeg_decompressor *d)
{
	unsigned short length;

	/* Get the marker parameter length count */
	length = read_short(d);
	/* Length includes itself, so must be at least 2 */
	if (length < 2)
		ERREXIT("Erroneous JPEG marker length");

	length -= 2;
	/* Skip over the remaining bytes */
	skip_bytes(d, length);

	return 0;
}

/**
 * Find the next marker (byte with value FF). Swallow consecutive duplicate FF bytes
 */
static int next_marker(jpeg_decompressor *d)
{
	uint8_t byte;
	int marker;
	int discarded_bytes = 0;

	/* Find 0xFF byte; count and skip any non-FFs. */
	byte = read_byte(d);
	while (byte != 0xFF)
	{
		if (eof(d))
			return -1;

		discarded_bytes++;
		byte = read_byte(d);
	}

	/**
	 * Get marker code byte, swallowing any duplicate FF bytes.  Extra FFs
	 * are legal as pad bytes, so don't count them in discarded_bytes.
	 */
	do
	{
		if (eof(d))
			return -1;

		marker = read_byte(d);
	} while (marker == 0xFF);

	if (discarded_bytes)
		printf("WARNING: discarded %u bytes\n", discarded_bytes);

	return marker;
}

/**
 * Read whether we are at valid START OF IMAGE
 */
static int read_jpeg_header(jpeg_decompressor *d)
{
	uint8_t c1 = 0, c2 = 0;

	if (!eof(d))
	{
		c1 = read_byte(d);
		c2 = read_byte(d);
	}
	if (c1 != 0xFF || c2 != M_SOI)
		printf("Not JPEG: %i %i\n", c1, c2);

	printf("Got JPEG marker!\n");

	return 0;
}

#define MIN_HUFFMAN_TABLE_LENGTH 17 // for an empty table

/**
 * Extract the Huffman tables
 */
static void process_DHT(jpeg_decompressor *d)
{
	unsigned int length;
	unsigned int i;
	unsigned int total;
	unsigned int num_tables = 0;

	length = read_short(d); // Lf
	//printf("%s header\n", __func__);
	length -= 2;

	/* Page 41: Table B.5 */
	// keep reading Huffman tables until we run out of data
	while (length >= MIN_HUFFMAN_TABLE_LENGTH)
	{
		//printf("%s header length = %u\n", __func__, length);
		total = 0;
		uint8_t temp = read_byte(d);
		length -= 1;
		d->huffman[num_tables].table_class = temp >> 4; // Tc
		d->huffman[num_tables].dest = temp & 0xF;		// Th

		for (i = 0; i < 16; i++)
		{
			d->huffman[num_tables].bits[i] = read_byte(d); // Li
			total += d->huffman[num_tables].bits[i];
		}
		length -= 16;

		for (i = 0; i < total; i++)
			d->huffman[num_tables].huffval[i] = read_byte(d); // Vij
		length -= total;

		/*
		printf("Table class: %u\n", d->huffman[num_tables].table_class);
		printf("destination: %u\n", d->huffman[num_tables].dest);
		printf("lengths: %i %i %i %i %i %i %i %i %i %i %i %i %i %i %i %i\n",
			d->huffman[num_tables].bits[0],
			d->huffman[num_tables].bits[1],
			d->huffman[num_tables].bits[2],
			d->huffman[num_tables].bits[3],
			d->huffman[num_tables].bits[4],
			d->huffman[num_tables].bits[5],
			d->huffman[num_tables].bits[6],
			d->huffman[num_tables].bits[7],
			d->huffman[num_tables].bits[8],
			d->huffman[num_tables].bits[9],
			d->huffman[num_tables].bits[10],
			d->huffman[num_tables].bits[11],
			d->huffman[num_tables].bits[12],
			d->huffman[num_tables].bits[13],
			d->huffman[num_tables].bits[14],
			d->huffman[num_tables].bits[15]);
		printf("total table bits: %u\n", total);
*/

		num_tables++;
	}
	if (length)
		printf("ERROR: %u bytes left over in DHT\n", length);
	d->num_huffman_tables = num_tables;
}

/**
 * Decode Start Of Frame
 */
static void process_SOFn(int marker, jpeg_decompressor *d)
{
	unsigned int length;
	unsigned int i;

	length = read_short(d); // Lf
	//printf("%s length=%u\n", __func__, length);

	/* Page 36: Table B.2 */
	d->data_precision = read_byte(d); // P
	d->image_height = read_short(d);  // Y
	d->image_width = read_short(d);	  // X
	d->num_components = read_byte(d); // Nf
	for (i = 0; i < d->num_components; i++)
	{
		d->comp_info[i].component_id = read_byte(d); // Ci
		uint8_t factor = read_byte(d);
		d->comp_info[i].h_samp_factor = (factor >> 4) & 0xF; // Hi
		d->comp_info[i].v_samp_factor = factor & 0xF;		 // Vi
		d->comp_info[i].quant_tbl_no = read_byte(d);		 // Tqi

		printf("Index: %u, H-samp factor: %u, V-samp factor: %u, Quant table: %u\n", d->comp_info[i].component_id,
			   d->comp_info[i].h_samp_factor,
			   d->comp_info[i].v_samp_factor,
			   d->comp_info[i].quant_tbl_no);
	}
}

/**
 * Decode Restart Interval
 */
static int process_DRI(jpeg_decompressor *d)
{
	unsigned int length;

	length = read_short(d); /* usual parameter length count */
	if (length != 4)
		printf("Unexpected length in restart interval (saw %u)\n", length);

	d->restart_interval = read_short(d);
	printf("%u MCUs per restart interval\n", d->restart_interval);

	return 0;
}

static void reset_decoder(jpeg_decompressor *d)
{
	d->restarts_left = d->restart_interval;
	d->get_buffer = 0;
	d->bits_left = 0;
}

int process_restart(jpeg_decompressor *d)
{
	int marker = next_marker(d);
	switch (marker)
	{
	case -1:
		printf("Error: EOF while looking for RST\n");
		return -1;

	case M_EOI:
		printf("EOI after %u restarts\n", d->num_restart_intervals);
		return -1;

	case M_RST_FIRST ... M_RST_LAST:
		d->num_restart_intervals++;
		reset_decoder(d);
		printf("Found restart %u (%u)\n", marker - M_RST_FIRST, d->num_restart_intervals);
		return 0;

	default:
		printf("Found unexpected marker 0x%x\n", marker);
	}

	return -1;
}

/* F.2.2.3 Decode */

/**
 * Get number of specified bits from the buffer
 */
static uint8_t get_bits(jpeg_decompressor *d, uint8_t num_bits)
{
	uint8_t b;
	uint32_t c;
	uint8_t temp;

	while (d->bits_left < num_bits)
	{
		// read a byte and decode it, if it is 0xFF
		b = read_byte(d);
		c = b;
		printf("Read byte 0x%x\n", b);
		while (b == 0xFF)
		{
			printf("Warning: got 0xFF byte\n");
			b = read_byte(d);
			if (b == 0)
				c = 0xFF;
			else
				c = b;
		}

		// add the new bits to the buffer (MSB aligned)
		//printf("before: buffer: 0x%08X (left %u)\n", d->get_buffer, d->bits_left);
		d->get_buffer |= c << (32 - 8 - d->bits_left);
		d->bits_left += 8;
		//printf("after: buffer: 0x%08X (left %u)\n", d->get_buffer, d->bits_left);
	}

	temp = d->get_buffer >> (32 - num_bits);
	printf("Getting %u bits: 0x%u\n", num_bits, temp);
	d->get_buffer <<= num_bits;
	d->bits_left -= num_bits;
	//printf("remain: buffer: 0x%08X\n", d->get_buffer);
	return temp;
}

static uint16_t huff_decode(jpeg_decompressor *d, huffman_table *table)
{
	uint32_t l = 1;
	uint16_t code;
	code = get_bits(d, 1);

	while (code > table->maxcode[l])
	{
		code <<= 1;
		l++;
	}

	return 0;
}

/* F.2.1.2 Decode 8x8 block data unit */
static int decode_mcu(jpeg_decompressor *d, uint8_t *buffer)
{
	uint32_t block;

	printf("%s: left %u\n", __func__, d->restarts_left);
	if (d->restart_interval)
	{
		if (d->restarts_left == 0)
		{
			if (process_restart(d) != 0)
			{
				printf("failed processing restart\n");
				return -1;
			}
		}
	}

	for (block = 0; block < d->blocks_per_MCU; block++)
	{
		uint8_t c;
		struct huffman_table *dctbl = &d->huffman[0];
		struct huffman_table *actbl = &d->huffman[1];

		// decode DC coefficient (F.2.2.1)
		uint16_t t = huff_decode(d, dctbl);
		//diff = RECEIVE(t);
		//diff = HUFFEXTEND(diff, t);

		// decode AC coefficients

		// dequantize
	}

	d->restarts_left--;

	return 0;
}

static void build_huffman_tables(jpeg_decompressor *d)
{
	uint8_t length;
	uint32_t p = 0;
	uint32_t code = 0;
	uint32_t index;
	uint8_t huffsize[257];
	uint32_t huffcode[257];
	//uint32_t numsymbols;
	uint32_t si;

	// huffsize is an array of lengths, indexed by symbol number
	for (length = 1; length <= 16; length++)
	{
		index = d->huffman[0].bits[length - 1];
		if (p + index > 256) /* protect against table overrun */
		{
			printf("Huffman table overrun\n");
			return;
		}
		while (index--)
			huffsize[p++] = length;
	}
	huffsize[p] = 0;
	//numsymbols = p;

	si = huffsize[0];

	// reconstruct the codes
	p = 0;
	while (huffsize[p])
	{
		while (huffsize[p] == si)
		{
			huffcode[p++] = code;
			code++;
		}

		/* code is now 1 more than the last code used for codelength si; but
		* it must still fit in si bits, since no code is allowed to be all ones.
		*/
		if (code >= (1UL << si))
		{
			printf("Bad huffman code length\n");
			return;
		}
		code <<= 1;
		si++;
	}

	/* Figure F.15: generate decoding tables for bit-sequential decoding */

	p = 0;
	for (length = 1; length <= 16; length++)
	{
		if (d->huffman[0].bits[length - 1])
		{
			/* valoffset[l] = huffval[] index of 1st symbol of code length l,
			* minus the minimum code of length l
			*/
			d->huffman[0].valoffset[length] = p - huffcode[p];
			p += d->huffman[0].bits[length - 1];
			d->huffman[0].maxcode[length] = huffcode[p - 1]; /* maximum code of length l */
		}
		else
		{
			d->huffman[0].maxcode[length] = -1; /* -1 if no codes of this length */
		}
	}
	d->huffman[0].valoffset[17] = 0;
	d->huffman[0].maxcode[17] = 0xFFFFFL; /* ensures jpeg_huff_decode terminates */
}

static int process_scan(jpeg_decompressor *d)
{
	unsigned int length;
	unsigned int num_ecs_segments;

	// read the scan header (SOS)
	length = read_short(d); // Ls
	//printf("%s header length=%u\n", __func__, length);

	d->comps_in_scan = read_byte(d); // Ns

	printf("Num components: %u\n", d->comps_in_scan);
	if (d->comps_in_scan == 1)
	{
		printf("We can't handle non-interleaved JPEGs\n");
		return -1;
	}

	if (d->comps_in_scan > MAX_COMP_INFO)
	{
		printf("Too many components (%u) max=%u\n", d->comps_in_scan, MAX_COMP_INFO);
		return -1;
	}

	if (d->comps_in_scan <= 1)
	{
		//d->blocks_per_MCU = 1;
		printf("ERROR: can't handle greyscale\n");
		return -1;
	}
	else
	{
		d->blocks_per_MCU = 0;
	}

	for (uint8_t i = 0; i < d->comps_in_scan; i++)
	{
		d->cur_comp_info[i].component_id = read_byte(d); // Csj
		uint8_t tdta = read_byte(d);
		d->cur_comp_info[i].dc_tbl_no = tdta >> 4;	// Tdj
		d->cur_comp_info[i].ac_tbl_no = tdta & 0xF; // Taj

		printf("[%u] DC entropy table = %u, AC entropy table = %u\n",
			   d->cur_comp_info[i].component_id,
			   d->cur_comp_info[i].dc_tbl_no,
			   d->cur_comp_info[i].ac_tbl_no);

		d->blocks_per_MCU += d->comp_info[i].h_samp_factor * d->comp_info[i].v_samp_factor;
	}
	printf("Blocks per MCU: %u\n", d->blocks_per_MCU);

	d->ss = read_byte(d); // Ss
	d->se = read_byte(d); // Se
	uint8_t A = read_byte(d);
	d->Ah = A >> 4;	 // Ah
	d->Al = A & 0xF; // Al

	//printf("ss=%u se=%u Ah=%u Al=%u\n", d->ss, d->se, d->Ah, d->Al);

	if (d->first_scan)
	{
		d->first_scan = 0;

		// set the maximum sampling factors
		d->max_h_samp_factor = 1;
		d->max_v_samp_factor = 1;
		for (uint8_t i = 0; i < d->comps_in_scan; i++)
		{
			d->max_h_samp_factor = MAX(d->max_h_samp_factor, d->comp_info[i].h_samp_factor);
			d->max_v_samp_factor = MAX(d->max_v_samp_factor, d->comp_info[i].v_samp_factor);
		}

		printf("Max H-samp factor: %u\n", d->max_h_samp_factor);
		printf("Max V-samp factor: %u\n", d->max_v_samp_factor);
	}

	if (d->comps_in_scan > 1)
	{
		d->MCUs_per_row = rounded_division(d->image_width, d->max_h_samp_factor * DCTSIZE);
		d->rows_per_scan = rounded_division(d->image_height, d->max_v_samp_factor * DCTSIZE);

		printf("MCUs per row: %u\n", d->MCUs_per_row);
		printf("MCUs per component per row: %u\n", d->MCUs_per_row / d->comps_in_scan);
		printf("MCU rows per scan: %u\n", d->rows_per_scan);
	}

	//for (comp_index = 0; comp_index < d->comps_in_scan; comp_index++)
	{
		//jpeg_decompress_component *component = d->cur_comp_info[comp_index];

		//MCU_blocks = component->h_samp_factor * component->v_samp_factor;

		// calculate number of blocks in last MCU column & row
		// last_row_width = X;
		// last_row_height = X;
	}

	// build Huffman tables
	build_huffman_tables(d);

	// ready to decode data
	return 0;
}

int decompress_scanline(jpeg_decompressor *d)
{
	uint8_t mcu_buffer[64];

	//num_ecs_segments = d->MCUs_per_row / restart_interval;
	d->restarts_left = d->restart_interval;
	d->num_restart_intervals = 0;

	// precalculate decoding info

	//uint32_t row;
	//for (row = 0; row < d->rows_per_scan; row++)
	{
		// each entropy-coded segment except the last one shall contain 'restart_interval' MCUs
		uint32_t mcu;
		for (mcu = 0; mcu < d->MCUs_per_row; mcu++)
		{
			printf("Decoding MCU %u\n", mcu);
			if (decode_mcu(d, mcu_buffer) != 0)
			{
				printf("Error decoding MCU\n");
				return -1;
			}

			for (uint8_t i = 0; i < d->comps_in_scan; i++)
			{
				uint32_t yindex, xindex;
				for (yindex = 0; yindex < d->comp_info[i].v_samp_factor; yindex++)
				{
					for (xindex = 0; xindex < d->comp_info[i].h_samp_factor; xindex++)
					{
						//inverse_DCT(d, mcu_buffer);
					}
				}
			}
		}
	}

	return 0;
}

static int find_frame(jpeg_decompressor *d)
{
	int found = 0;

	//printf("Looking for SOF\n");

	while (!found)
	{
		int marker = next_marker(d);
		switch (marker)
		{
		case -1:
			printf("Error: EOF while looking for SOF\n");
			return -1;

		case M_SOF0 ... M_SOF3: /* Baseline */
			//printf("SOF non-differential huffman\n");
			process_SOFn(marker, d);
			found = 1;
			break;

		case M_SOF5 ... M_SOF7:
			//printf("SOF differential huffman\n");
			process_SOFn(marker, d);
			found = 1;
			break;

		case M_SOF9 ... M_SOF11:
			//printf("SOF non-differential arithmetic\n");
			process_SOFn(marker, d);
			found = 1;
			break;

		case M_SOF13 ... M_SOF15:
			//printf("SOF differential arithmetic\n");
			process_SOFn(marker, d);
			found = 1;
			break;

		case M_EOI: /* in case it's a tables-only JPEG stream */
			printf("Error: Hit EOI while looking for frame\n");
			return -1;

		case M_DRI:
			process_DRI(d);
			break;

		default:			  /* Anything else just gets skipped */
			skip_variable(d); /* we assume it has a parameter count... */
		}
	}

	return 0;
}

static int find_scan(jpeg_decompressor *d)
{
	int marker;

	//printf("Looking for SOS\n");

	/* Scan miscellaneous markers until we reach SOS (start of scan). */
	for (;;)
	{
		marker = next_marker(d);
		switch (marker)
		{
		case -1:
			printf("reading past EOF\n");
			return -1;

		case M_RST_FIRST ... M_RST_LAST:
			printf("Found restart %u @ 0x%lx\n", marker - M_RST_FIRST, d->ptr - d->data);
			return -1;

		case M_SOS:
			printf("<SOS>\n");
			return process_scan(d);

		case M_DHT:
			printf("<DHT>\n");
			process_DHT(d);
			break;

		case M_EOI: /* in case it's a tables-only JPEG stream */
			printf("Error: Hit EOI while looking for scan\n");
			return -1;

		case M_APP_FIRST ... M_APP_LAST:
			skip_variable(d);
			break;

		default: /* Anything else just gets skipped */
			printf("<%X>\n", marker);
			skip_variable(d); /* we assume it has a parameter count... */
			break;
		}
	}

	return -2;
}

void jpeg_cpu_scale(uint64_t file_length, char *buffer)
{
	jpeg_decompressor decompressor;
	int ret;

	decompressor.data = buffer;
	decompressor.ptr = decompressor.data;
	decompressor.length = file_length;
	decompressor.get_buffer = 0;
	decompressor.bits_left = 0;

	read_jpeg_header(&decompressor);

	// there should be a single frame in the buffer
	if (find_frame(&decompressor) == 0)
	{
		printf("Size: %u x %u\n", decompressor.image_width, decompressor.image_height);
		//printf("Precision: %u bits\nNum components: %u\n",
		//	decompressor.data_precision, decompressor.num_components);
	}

	// the frame contains multiple scans
	decompressor.first_scan = 1;
	ret = find_scan(&decompressor);
	while (ret == 0)
	{
		// decompress the scan
		for (uint32_t scanline = 0; scanline < decompressor.rows_per_scan; scanline++)
		{
			printf("Processing scanline %u\n", scanline);
			decompress_scanline(&decompressor);
		}

		// look for the next one
		decompressor.first_scan = 0;
		ret = find_scan(&decompressor);
	}

	/*
	// now write the data out as BMP
	bmp_object image;
	uint8_t *ptr;

	image.win_header.width = 320;
	image.win_header.height = 200;
	ptr = malloc(320 * 200 * 3);
	image.data = ptr;
	for (int h = 0; h < 200; h++)
	for (int w = 0; w < 320; w++)
	{
		ptr[0] = h;
		ptr[1] = w;
		ptr[2] = 100;
		ptr += 3;
	}
	write_bmp("output.bmp", &image);
*/
}
